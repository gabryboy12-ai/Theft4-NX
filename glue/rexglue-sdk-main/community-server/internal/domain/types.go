package domain

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"net"
	"regexp"
	"strings"
	"time"
)

var IDPattern = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9_.:-]{0,127}$`)

var (
	ErrNotFound  = errors.New("not found")
	ErrConflict  = errors.New("conflict")
	ErrForbidden = errors.New("forbidden")
	ErrInvalid   = errors.New("invalid")
)

func NewID(prefix string) string {
	raw := make([]byte, 16)
	if _, err := rand.Read(raw); err != nil {
		panic(err)
	}
	return prefix + "_" + hex.EncodeToString(raw)
}

type Document struct {
	Kind      string          `json:"kind"`
	ID        string          `json:"id"`
	OwnerID   string          `json:"owner_id"`
	Body      json.RawMessage `json:"body"`
	Version   int64           `json:"version"`
	CreatedAt time.Time       `json:"created_at"`
	UpdatedAt time.Time       `json:"updated_at"`
	Events    []Event         `json:"-"`
}

type Event struct {
	ID            int64           `json:"id"`
	EventKey      string          `json:"event_id"`
	UserID        string          `json:"user_id"`
	Type          string          `json:"type"`
	AggregateType string          `json:"aggregate_type"`
	AggregateID   string          `json:"aggregate_id"`
	Revision      int64           `json:"revision"`
	CorrelationID string          `json:"correlation_id,omitempty"`
	Payload       json.RawMessage `json:"payload"`
	CreatedAt     time.Time       `json:"created_at"`
}

type Device struct {
	ID          string    `json:"device_id"`
	AccountID   string    `json:"account_id"`
	DisplayName string    `json:"display_name"`
	PublicKey   string    `json:"public_key"`
	XUID        string    `json:"xuid"`
	Disabled    bool      `json:"disabled"`
	CreatedAt   time.Time `json:"created_at"`
	LastSeenAt  time.Time `json:"last_seen_at"`
}

type Challenge struct {
	ID        string    `json:"challenge_id"`
	DeviceID  string    `json:"device_id"`
	Nonce     string    `json:"nonce"`
	XUID      string    `json:"-"`
	PublicKey string    `json:"-"`
	ExpiresAt time.Time `json:"expires_at"`
	Used      bool      `json:"used"`
}

type RefreshSession struct {
	ID        string    `json:"id"`
	AccountID string    `json:"account_id"`
	DeviceID  string    `json:"device_id"`
	TokenHash string    `json:"token_hash"`
	ExpiresAt time.Time `json:"expires_at"`
	Revoked   bool      `json:"revoked"`
}

type Route struct {
	PeerID       int    `json:"peer_id"`
	Address      string `json:"address,omitempty"`
	Port         int    `json:"port,omitempty"`
	RelayAddress string `json:"relay_address,omitempty"`
	ConnectionID string `json:"connection_id,omitempty"`
}

type SessionMember struct {
	XUID        string `json:"xuid"`
	AccountID   string `json:"account_id"`
	MachineID   string `json:"machine_id"`
	VirtualIPv4 string `json:"virtual_ipv4"`
	OnlinePort  int    `json:"online_port"`
	PeerID      string `json:"peer_id"`
	Private     bool   `json:"private"`
	Ready       bool   `json:"ready"`
	Role        string `json:"role"`
	Route       Route  `json:"route"`
	JoinedAt    string `json:"joined_at"`
}

type SessionRecord struct {
	ID                  string            `json:"id"`
	Revision            int64             `json:"revision"`
	HostEpoch           int64             `json:"host_epoch"`
	HostXUID            string            `json:"host_xuid"`
	PreviousSessionID   string            `json:"previous_session_id"`
	Nonce               string            `json:"nonce"`
	Flags               uint32            `json:"flags"`
	LifecycleState      uint32            `json:"lifecycle_state"`
	HostMachineID       string            `json:"host_machine_id"`
	HostIPv4            string            `json:"host_ipv4"`
	HostPort            int               `json:"host_port"`
	HostEthernetAddress string            `json:"host_ethernet_address"`
	HostPeerID          string            `json:"host_peer_id"`
	HostLeaseExpiresAt  string            `json:"host_lease_expires_at"`
	State               string            `json:"state"`
	Visibility          string            `json:"visibility"`
	Mode                string            `json:"mode"`
	Episode             string            `json:"episode"`
	Region              string            `json:"region"`
	Ranked              bool              `json:"ranked"`
	PublicSlots         int               `json:"public_slots"`
	PrivateSlots        int               `json:"private_slots"`
	BuildID             string            `json:"build_id"`
	ProtocolVersion     int               `json:"protocol_version"`
	TitleID             string            `json:"title_id"`
	MediaID             string            `json:"media_id"`
	TitleVersion        string            `json:"title_version"`
	SessionID           string            `json:"session_id"`
	ExchangeKey         string            `json:"exchange_key"`
	Contexts            map[string]uint32 `json:"contexts"`
	Properties          map[string]string `json:"properties"`
	Members             []SessionMember   `json:"members"`
	CreatedAt           string            `json:"created_at"`
	UpdatedAt           string            `json:"updated_at"`
}

func (s SessionRecord) Validate(maxMembers int) error {
	if s.Mode == "" || len(s.Mode) > 64 || !IDPattern.MatchString(s.Mode) {
		return ErrInvalid
	}
	if s.Episode == "" || len(s.Episode) > 32 || !IDPattern.MatchString(s.Episode) {
		return ErrInvalid
	}
	if s.Region == "" || len(s.Region) > 32 || !IDPattern.MatchString(s.Region) {
		return ErrInvalid
	}
	if s.BuildID == "" || len(s.BuildID) > 128 || !IDPattern.MatchString(s.BuildID) {
		return ErrInvalid
	}
	if s.ProtocolVersion < 1 {
		return ErrInvalid
	}
	for _, value := range []string{s.TitleID, s.MediaID, s.TitleVersion, s.SessionID, s.ExchangeKey} {
		if !IDPattern.MatchString(value) {
			return ErrInvalid
		}
	}
	for _, value := range []string{s.PreviousSessionID, s.Nonce, s.HostMachineID} {
		if value != "" && !IDPattern.MatchString(value) {
			return ErrInvalid
		}
	}
	if s.HostPort < 0 || s.HostPort > 65535 || (s.HostPeerID != "" && !IDPattern.MatchString(s.HostPeerID)) {
		return ErrInvalid
	}
	if s.PublicSlots < 0 || s.PrivateSlots < 0 || s.PublicSlots+s.PrivateSlots > maxMembers {
		return ErrInvalid
	}
	if len(s.Members) > s.PublicSlots+s.PrivateSlots {
		return ErrInvalid
	}
	if len(s.Contexts) > 64 || len(s.Properties) > 64 {
		return ErrInvalid
	}
	for key, value := range s.Properties {
		if !IDPattern.MatchString(key) {
			return ErrInvalid
		}
		_ = value
	}
	if s.HostIPv4 != "" && net.ParseIP(s.HostIPv4) == nil {
		return ErrInvalid
	}
	if s.HostEthernetAddress != "" && !regexp.MustCompile(`^[0-9a-fA-F]{12}$`).MatchString(s.HostEthernetAddress) {
		return ErrInvalid
	}
	seenPeer := map[int]bool{}
	seenXUID := map[string]bool{}
	publicMembers, privateMembers := 0, 0
	for _, member := range s.Members {
		if member.Private {
			privateMembers++
		} else {
			publicMembers++
		}
		if !IDPattern.MatchString(member.XUID) || !IDPattern.MatchString(member.AccountID) {
			return ErrInvalid
		}
		peerID := member.Route.PeerID
		if member.MachineID != "" && !IDPattern.MatchString(member.MachineID) {
			return ErrInvalid
		}
		if member.VirtualIPv4 != "" && net.ParseIP(member.VirtualIPv4) == nil {
			return ErrInvalid
		}
		if member.PeerID != "" && !IDPattern.MatchString(member.PeerID) {
			return ErrInvalid
		}
		if member.OnlinePort < 0 || member.OnlinePort > 65535 || peerID < 0 || peerID >= maxMembers || seenPeer[peerID] || seenXUID[member.XUID] {
			return ErrInvalid
		}
		seenPeer[peerID] = true
		seenXUID[member.XUID] = true
	}
	if publicMembers > s.PublicSlots || privateMembers > s.PrivateSlots {
		return ErrInvalid
	}
	return nil
}

func (s SessionRecord) ContainsAccount(accountID string) bool {
	for _, member := range s.Members {
		if member.AccountID == accountID {
			return true
		}
	}
	return false
}

func (s SessionRecord) MemberByXUID(xuid string) (SessionMember, bool) {
	for _, member := range s.Members {
		if member.XUID == xuid {
			return member, true
		}
	}
	return SessionMember{}, false
}

func ValidateID(value string) bool { return IDPattern.MatchString(strings.TrimSpace(value)) }

type MatchmakingTicket struct {
	ID             string            `json:"id"`
	OwnerID        string            `json:"owner_id"`
	ProcedureIndex int               `json:"procedure_index"`
	Mode           string            `json:"mode"`
	Episode        string            `json:"episode"`
	Region         string            `json:"region"`
	Ranked         bool              `json:"ranked"`
	PartySize      int               `json:"party_size"`
	Contexts       map[string]uint32 `json:"contexts"`
	Properties     map[string]string `json:"properties"`
	State          string            `json:"state"`
	MatchedSession string            `json:"matched_session_id,omitempty"`
	CreatedAt      string            `json:"created_at"`
	UpdatedAt      string            `json:"updated_at"`
}

type ErrorEnvelope struct {
	Error APIError `json:"error"`
}

type APIError struct {
	Code          string            `json:"code"`
	Message       string            `json:"message"`
	CorrelationID string            `json:"correlation_id,omitempty"`
	Fields        map[string]string `json:"fields,omitempty"`
}
