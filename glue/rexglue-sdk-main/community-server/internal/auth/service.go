package auth

import (
	"context"
	"crypto/ed25519"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"time"

	"libertyrecomp/community-server/internal/domain"
	"libertyrecomp/community-server/internal/store"
)

type Claims struct {
	Subject   string   `json:"sub"`
	DeviceID  string   `json:"device_id"`
	Roles     []string `json:"roles"`
	IssuedAt  int64    `json:"iat"`
	ExpiresAt int64    `json:"exp"`
	TokenID   string   `json:"jti"`
}

type Service struct {
	store        store.Store
	key          []byte
	accessTTL    time.Duration
	refreshTTL   time.Duration
	challengeTTL time.Duration
	now          func() time.Time
}

func New(s store.Store, key []byte, accessTTL, refreshTTL, challengeTTL time.Duration) *Service {
	return &Service{store: s, key: append([]byte(nil), key...), accessTTL: accessTTL, refreshTTL: refreshTTL, challengeTTL: challengeTTL, now: time.Now}
}

func (s *Service) Challenge(ctx context.Context, deviceID, xuid, publicKey string) (domain.Challenge, error) {
	decoded, decodeErr := base64.RawURLEncoding.DecodeString(publicKey)
	if !domain.ValidateID(deviceID) || !domain.ValidateID(xuid) || decodeErr != nil || len(decoded) != ed25519.PublicKeySize {
		return domain.Challenge{}, domain.ErrInvalid
	}
	raw := make([]byte, 32)
	if _, err := rand.Read(raw); err != nil {
		return domain.Challenge{}, err
	}
	c := domain.Challenge{ID: domain.NewID("ch"), DeviceID: deviceID, Nonce: base64.RawURLEncoding.EncodeToString(raw), XUID: xuid, PublicKey: publicKey, ExpiresAt: s.now().UTC().Add(s.challengeTTL)}
	return c, s.store.PutChallenge(ctx, c)
}

type EnrollRequest struct {
	DeviceID    string `json:"device_id"`
	XUID        string `json:"xuid"`
	MachineID   string `json:"machine_id"`
	PlayerName  string `json:"player_name"`
	PublicKey   string `json:"public_key"`
	ChallengeID string `json:"challenge_id"`
	Signature   string `json:"signature"`
}

type TokenPair struct {
	AccessToken  string `json:"access_token"`
	ExpiresIn    int64  `json:"expires_in"`
	RefreshToken string `json:"refresh_token"`
	AccountID    string `json:"-"`
	DeviceID     string `json:"-"`
	XUID         string `json:"-"`
}

func (s *Service) Enroll(ctx context.Context, request EnrollRequest) (TokenPair, error) {
	if !domain.ValidateID(request.DeviceID) || !domain.ValidateID(request.MachineID) || !domain.ValidateID(request.XUID) || len(strings.TrimSpace(request.PlayerName)) < 1 || len(request.PlayerName) > 32 {
		return TokenPair{}, domain.ErrInvalid
	}
	publicKey, err := base64.RawURLEncoding.DecodeString(request.PublicKey)
	if err != nil || len(publicKey) != ed25519.PublicKeySize {
		return TokenPair{}, domain.ErrInvalid
	}
	challenge, err := s.store.ConsumeChallenge(ctx, request.ChallengeID, request.DeviceID, s.now().UTC())
	if err != nil {
		return TokenPair{}, err
	}
	if challenge.PublicKey != request.PublicKey || challenge.XUID != request.XUID {
		return TokenPair{}, domain.ErrForbidden
	}
	signature, err := base64.RawURLEncoding.DecodeString(request.Signature)
	if err != nil || !ed25519.Verify(ed25519.PublicKey(publicKey), []byte(challenge.Nonce), signature) {
		return TokenPair{}, domain.ErrForbidden
	}
	accountID := domain.NewID("acct")
	if existing, getErr := s.store.GetDevice(ctx, request.DeviceID); getErr == nil {
		accountID = existing.AccountID
	}
	xuid := request.XUID
	device := domain.Device{ID: request.DeviceID, AccountID: accountID, DisplayName: strings.TrimSpace(request.PlayerName), PublicKey: request.PublicKey, XUID: xuid, LastSeenAt: s.now().UTC()}
	if err := s.store.PutDevice(ctx, device); err != nil {
		return TokenPair{}, err
	}
	return s.issue(ctx, accountID, request.DeviceID, xuid, []string{"player"})
}

func (s *Service) AuthenticateDevice(ctx context.Context, deviceID, challengeID, signatureText string) (TokenPair, error) {
	device, err := s.store.GetDevice(ctx, deviceID)
	if err != nil || device.Disabled {
		return TokenPair{}, domain.ErrForbidden
	}
	challenge, err := s.store.ConsumeChallenge(ctx, challengeID, deviceID, s.now().UTC())
	if err != nil {
		return TokenPair{}, err
	}
	key, keyErr := base64.RawURLEncoding.DecodeString(device.PublicKey)
	signature, sigErr := base64.RawURLEncoding.DecodeString(signatureText)
	if keyErr != nil || sigErr != nil || !ed25519.Verify(ed25519.PublicKey(key), []byte(challenge.Nonce), signature) {
		return TokenPair{}, domain.ErrForbidden
	}
	return s.issue(ctx, device.AccountID, device.ID, device.XUID, []string{"player"})
}

func (s *Service) Refresh(ctx context.Context, token string) (TokenPair, error) {
	hash := sha256.Sum256([]byte(token))
	session, err := s.store.ConsumeRefresh(ctx, base64.RawURLEncoding.EncodeToString(hash[:]), s.now().UTC())
	if err != nil {
		return TokenPair{}, err
	}
	device, err := s.store.GetDevice(ctx, session.DeviceID)
	if err != nil || device.Disabled {
		return TokenPair{}, domain.ErrForbidden
	}
	return s.issue(ctx, session.AccountID, session.DeviceID, device.XUID, []string{"player"})
}

func (s *Service) issue(ctx context.Context, accountID, deviceID, xuid string, roles []string) (TokenPair, error) {
	now := s.now().UTC()
	claims := Claims{Subject: accountID, DeviceID: deviceID, Roles: roles, IssuedAt: now.Unix(), ExpiresAt: now.Add(s.accessTTL).Unix(), TokenID: domain.NewID("tok")}
	access, err := s.sign(claims)
	if err != nil {
		return TokenPair{}, err
	}
	raw := make([]byte, 32)
	if _, err := rand.Read(raw); err != nil {
		return TokenPair{}, err
	}
	refresh := base64.RawURLEncoding.EncodeToString(raw)
	hash := sha256.Sum256([]byte(refresh))
	row := domain.RefreshSession{ID: domain.NewID("refresh"), AccountID: accountID, DeviceID: deviceID, TokenHash: base64.RawURLEncoding.EncodeToString(hash[:]), ExpiresAt: now.Add(s.refreshTTL)}
	if err := s.store.PutRefresh(ctx, row); err != nil {
		return TokenPair{}, err
	}
	return TokenPair{AccessToken: access, ExpiresIn: int64(s.accessTTL.Seconds()), RefreshToken: refresh, AccountID: accountID, DeviceID: deviceID, XUID: xuid}, nil
}

func (s *Service) Verify(token string) (Claims, error) {
	parts := strings.Split(token, ".")
	if len(parts) != 3 {
		return Claims{}, domain.ErrForbidden
	}
	unsigned := parts[0] + "." + parts[1]
	supplied, err := base64.RawURLEncoding.DecodeString(parts[2])
	if err != nil {
		return Claims{}, domain.ErrForbidden
	}
	mac := hmac.New(sha256.New, s.key)
	_, _ = mac.Write([]byte(unsigned))
	if !hmac.Equal(supplied, mac.Sum(nil)) {
		return Claims{}, domain.ErrForbidden
	}
	payload, err := base64.RawURLEncoding.DecodeString(parts[1])
	if err != nil {
		return Claims{}, domain.ErrForbidden
	}
	var claims Claims
	if err := json.Unmarshal(payload, &claims); err != nil || claims.Subject == "" || s.now().Unix() >= claims.ExpiresAt {
		return Claims{}, domain.ErrForbidden
	}
	return claims, nil
}

func (s *Service) sign(claims Claims) (string, error) {
	header := base64.RawURLEncoding.EncodeToString([]byte(`{"alg":"HS256","typ":"JWT"}`))
	payload, err := json.Marshal(claims)
	if err != nil {
		return "", err
	}
	unsigned := header + "." + base64.RawURLEncoding.EncodeToString(payload)
	mac := hmac.New(sha256.New, s.key)
	_, _ = mac.Write([]byte(unsigned))
	return unsigned + "." + base64.RawURLEncoding.EncodeToString(mac.Sum(nil)), nil
}

func Bearer(header string) (string, error) {
	parts := strings.Fields(header)
	if len(parts) != 2 || !strings.EqualFold(parts[0], "Bearer") {
		return "", errors.New("missing bearer token")
	}
	return parts[1], nil
}

func HasRole(claims Claims, role string) bool {
	for _, candidate := range claims.Roles {
		if candidate == role {
			return true
		}
	}
	return false
}
func (c Claims) String() string { return fmt.Sprintf("%s/%s", c.Subject, c.DeviceID) }
