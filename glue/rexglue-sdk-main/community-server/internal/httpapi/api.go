package httpapi

import (
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http"
	"net/netip"
	"net/url"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/gorilla/websocket"
	redis "github.com/redis/go-redis/v9"
	"libertyrecomp/community-server/internal/auth"
	"libertyrecomp/community-server/internal/config"
	"libertyrecomp/community-server/internal/domain"
	"libertyrecomp/community-server/internal/realtime"
	"libertyrecomp/community-server/internal/relay"
	"libertyrecomp/community-server/internal/store"
)

type contextKey string

const (
	claimsKey   contextKey = "claims"
	clientIPKey contextKey = "client_ip"
)

const maxInMemoryRateEntries = 10_000

type API struct {
	config   config.Config
	store    store.Store
	auth     *auth.Service
	hub      *realtime.Hub
	relay    *relay.Service
	redis    *redis.Client
	mux      *http.ServeMux
	requests atomic.Int64
	errors   atomic.Int64
	mu       sync.Mutex
	rate     map[string]rateEntry
	rateMax  int
}
type rateEntry struct {
	minute int64
	count  int
}

func New(c config.Config, s store.Store, a *auth.Service, h *realtime.Hub, relayService *relay.Service) (*API, error) {
	api := &API{config: c, store: s, auth: a, hub: h, relay: relayService, mux: http.NewServeMux(), rate: map[string]rateEntry{}, rateMax: maxInMemoryRateEntries}
	if c.RedisURL != "" {
		options, err := redis.ParseURL(c.RedisURL)
		if err != nil {
			return nil, err
		}
		api.redis = redis.NewClient(options)
	}
	api.routes()
	return api, nil
}
func (a *API) Close() {
	if a.redis != nil {
		_ = a.redis.Close()
	}
}
func (a *API) Handler() http.Handler {
	return a.security(a.requestID(a.withClientIP(a.logging(a.rateLimit(a.mux)))))
}

func (a *API) routes() {
	a.mux.HandleFunc("GET /health/live", a.live)
	a.mux.HandleFunc("GET /health/ready", a.ready)
	a.mux.HandleFunc("GET /metrics", a.metrics)
	a.mux.HandleFunc("POST /api/v2/devices/challenge", a.deviceChallenge)
	a.mux.HandleFunc("POST /api/v2/devices/enroll", a.deviceEnroll)
	a.mux.HandleFunc("POST /api/v2/devices/token", a.deviceToken)
	a.mux.HandleFunc("POST /api/v2/devices/refresh", a.deviceRefresh)
	a.mux.Handle("POST /api/v2/sessions", a.requireAuth(http.HandlerFunc(a.createSession)))
	a.mux.Handle("POST /api/v2/sessions/search", a.requireAuth(http.HandlerFunc(a.searchSessions)))
	a.mux.Handle("GET /api/v2/sessions/{id}", a.requireAuth(http.HandlerFunc(a.getSession)))
	a.mux.Handle("GET /api/v2/sessions/by-xbox-id/{xbox_id}", a.requireAuth(http.HandlerFunc(a.getSessionByXboxID)))
	a.mux.Handle("PATCH /api/v2/sessions/{id}", a.requireAuth(http.HandlerFunc(a.patchSession)))
	a.mux.Handle("DELETE /api/v2/sessions/{id}", a.requireAuth(http.HandlerFunc(a.deleteSession)))
	for _, action := range []string{"heartbeat", "join", "leave", "migration"} {
		a.mux.Handle("POST /api/v2/sessions/{id}/"+action, a.requireAuth(http.HandlerFunc(a.sessionAction)))
	}
	a.mux.Handle("GET /api/v2/lobbies/{id}", a.requireAuth(http.HandlerFunc(a.lobbyStateGet)))
	a.mux.Handle("POST /api/v2/lobbies/{id}/ready", a.requireAuth(http.HandlerFunc(a.lobbyReady)))
	a.mux.Handle("POST /api/v2/lobbies/{id}/spectator", a.requireAuth(http.HandlerFunc(a.lobbySpectator)))
	a.mux.Handle("POST /api/v2/lobbies/{id}/kick-votes", a.requireAuth(http.HandlerFunc(a.lobbyKickVote)))
	a.mux.Handle("PUT /api/v2/lobbies/{id}/next-game", a.requireAuth(http.HandlerFunc(a.lobbyNextGame)))
	a.mux.Handle("POST /api/v2/matchmaking/tickets", a.requireAuth(http.HandlerFunc(a.createTicket)))
	a.mux.Handle("GET /api/v2/matchmaking/tickets/{id}", a.requireAuth(http.HandlerFunc(a.getTicket)))
	a.mux.Handle("DELETE /api/v2/matchmaking/tickets/{id}", a.requireAuth(http.HandlerFunc(a.deleteTicket)))
	a.mux.Handle("GET /api/v2/events", a.requireAuth(http.HandlerFunc(a.events)))
	a.mux.Handle("POST /api/v2/chat/messages", a.requireAuth(http.HandlerFunc(a.chatMessage)))
	a.mux.Handle("GET /api/v2/realtime", a.requireAuth(http.HandlerFunc(a.websocket)))
	a.mux.Handle("POST /api/v2/relay/routes", a.requireAuth(http.HandlerFunc(a.relayRoute)))
	a.mux.Handle("DELETE /api/v2/relay/routes", a.requireAuth(http.HandlerFunc(a.relayRoute)))
	a.mux.Handle("POST /api/v2/relay/datagrams", a.requireAuth(http.HandlerFunc(a.relayDatagrams)))
	a.mux.Handle("GET /api/v2/relay/datagrams", a.requireAuth(http.HandlerFunc(a.relayDatagrams)))
	a.mux.Handle("POST /api/v2/progression/results", a.requireAuth(http.HandlerFunc(a.rankedResult)))
	a.mux.Handle("GET /api/v2/progression/{xuid}", a.requireAuth(http.HandlerFunc(a.progressionGet)))
	a.mux.Handle("GET /api/v2/profiles/{xuid}", a.requireAuth(http.HandlerFunc(a.profileGet)))
	a.mux.Handle("PUT /api/v2/profiles/me", a.requireAuth(http.HandlerFunc(a.profilePut)))
	a.mux.Handle("PUT /api/v2/presence", a.requireAuth(http.HandlerFunc(a.presencePut)))
	a.mux.Handle("GET /api/v2/presence/{xuid}", a.requireAuth(http.HandlerFunc(a.presenceGet)))
	a.mux.Handle("POST /api/v2/moderation/reports", a.requireAuth(http.HandlerFunc(a.createResource)))
	a.mux.Handle("POST /api/v2/moderation/bans", a.requireRole("moderator", http.HandlerFunc(a.createResource)))
	a.mux.Handle("GET /api/v2/friends", a.requireAuth(http.HandlerFunc(a.friendsPage)))
	a.mux.Handle("POST /api/v2/friends/check", a.requireAuth(http.HandlerFunc(a.friendsCheck)))
	a.mux.Handle("POST /api/v2/friends/relationships", a.requireAuth(http.HandlerFunc(a.friendRelationship)))
	a.mux.Handle("POST /api/v2/invites", a.requireAuth(http.HandlerFunc(a.createInvites)))
	a.mux.Handle("GET /api/v2/invites", a.requireAuth(http.HandlerFunc(a.invitesPage)))
	a.mux.Handle("POST /api/v2/invites/{id}/accept", a.requireAuth(http.HandlerFunc(a.acceptInvite)))
	a.mux.Handle("POST /api/v2/stats/writes", a.requireAuth(http.HandlerFunc(a.statsWrite)))
	a.mux.Handle("POST /api/v2/stats/read", a.requireAuth(http.HandlerFunc(a.statsRead)))
	a.mux.Handle("POST /api/v2/stats/reset", a.requireAuth(http.HandlerFunc(a.statsReset)))
	a.mux.Handle("GET /api/v2/leaderboards/{view_id}", a.requireAuth(http.HandlerFunc(a.leaderboard)))
	a.mux.Handle("POST /api/v2/stats/skill", a.requireAuth(http.HandlerFunc(a.statsSkill)))
	a.mux.Handle("POST /api/v2/voice/routes", a.requireAuth(http.HandlerFunc(a.voiceRoute)))
	a.mux.Handle("DELETE /api/v2/voice/routes", a.requireAuth(http.HandlerFunc(a.voiceRouteDelete)))
	a.mux.Handle("POST /api/v2/voice/packets", a.requireAuth(http.HandlerFunc(a.voicePacket)))
	a.mux.Handle("GET /api/v2/voice/packets", a.requireAuth(http.HandlerFunc(a.voicePackets)))
}

func (a *API) live(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, 200, map[string]any{"status": "live", "role": a.config.Role, "public_url": a.config.PublicURL})
}
func (a *API) ready(w http.ResponseWriter, r *http.Request) {
	if err := a.store.Ready(r.Context()); err != nil {
		writeError(w, r, 503, "not_ready", "database is unavailable")
		return
	}
	if err := a.hub.Ready(r.Context()); err != nil {
		writeError(w, r, 503, "not_ready", "event bus is unavailable")
		return
	}
	if err := a.relay.Ready(r.Context()); err != nil {
		writeError(w, r, 503, "not_ready", "relay is unavailable")
		return
	}
	writeJSON(w, 200, map[string]string{"status": "ready", "public_url": a.config.PublicURL})
}
func (a *API) metrics(w http.ResponseWriter, _ *http.Request) {
	w.Header().Set("Content-Type", "text/plain; version=0.0.4")
	_, _ = fmt.Fprintf(w, "community_http_requests_total %d\ncommunity_http_errors_total %d\n", a.requests.Load(), a.errors.Load())
}

func (a *API) deviceChallenge(w http.ResponseWriter, r *http.Request) {
	var body struct {
		DeviceID  string `json:"device_id"`
		XUID      string `json:"xuid"`
		PublicKey string `json:"public_key"`
	}
	if !decode(w, r, &body) {
		return
	}
	challenge, err := a.auth.Challenge(r.Context(), body.DeviceID, body.XUID, body.PublicKey)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	writeJSON(w, 201, map[string]string{"challenge_id": challenge.ID, "challenge": challenge.Nonce})
}
func (a *API) deviceEnroll(w http.ResponseWriter, r *http.Request) {
	var body auth.EnrollRequest
	if !decode(w, r, &body) {
		return
	}
	pair, err := a.auth.Enroll(r.Context(), body)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	writeJSON(w, 201, pair)
}
func (a *API) deviceToken(w http.ResponseWriter, r *http.Request) {
	var body struct {
		DeviceID    string `json:"device_id"`
		ChallengeID string `json:"challenge_id"`
		Signature   string `json:"signature"`
	}
	if !decode(w, r, &body) {
		return
	}
	pair, err := a.auth.AuthenticateDevice(r.Context(), body.DeviceID, body.ChallengeID, body.Signature)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	writeJSON(w, 200, pair)
}
func (a *API) deviceRefresh(w http.ResponseWriter, r *http.Request) {
	var body struct {
		RefreshToken string `json:"refresh_token"`
	}
	if !decode(w, r, &body) {
		return
	}
	pair, err := a.auth.Refresh(r.Context(), body.RefreshToken)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	writeJSON(w, 200, pair)
}

func (a *API) requireAuth(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		token, err := auth.Bearer(r.Header.Get("Authorization"))
		if err != nil {
			writeError(w, r, 401, "unauthorized", "a bearer token is required")
			return
		}
		claims, err := a.auth.Verify(token)
		if err != nil {
			writeError(w, r, 401, "unauthorized", "the bearer token is invalid or expired")
			return
		}
		next.ServeHTTP(w, r.WithContext(context.WithValue(r.Context(), claimsKey, claims)))
	})
}
func (a *API) requireRole(role string, next http.Handler) http.Handler {
	return a.requireAuth(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !auth.HasRole(claims(r), role) {
			writeError(w, r, 403, "forbidden", "insufficient role")
			return
		}
		next.ServeHTTP(w, r)
	}))
}
func claims(r *http.Request) auth.Claims { return r.Context().Value(claimsKey).(auth.Claims) }

func (a *API) requestID(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		id := r.Header.Get("X-Correlation-ID")
		if !domain.ValidateID(id) {
			id = domain.NewID("req")
		}
		w.Header().Set("X-Correlation-ID", id)
		next.ServeHTTP(w, r.WithContext(context.WithValue(r.Context(), contextKey("request_id"), id)))
	})
}
func (a *API) security(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("Cache-Control", "no-store")
		w.Header().Set("Referrer-Policy", "no-referrer")
		next.ServeHTTP(w, r)
	})
}
func (a *API) logging(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		a.requests.Add(1)
		start := time.Now()
		next.ServeHTTP(w, r)
		slog.Info("http request", "method", r.Method, "path", r.URL.Path, "client_ip", clientIP(r), "duration", time.Since(start))
	})
}
func (a *API) rateLimit(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		host := clientIP(r)
		minute := time.Now().Unix() / 60
		allowed := true
		if a.redis != nil {
			key := fmt.Sprintf("rate:%s:%d", host, minute)
			count, err := a.redis.Incr(r.Context(), key).Result()
			if err == nil {
				_ = a.redis.Expire(r.Context(), key, 2*time.Minute).Err()
				allowed = count <= int64(a.config.RateLimitPerMin)
			}
		} else {
			a.mu.Lock()
			entry, exists := a.rate[host]
			if !exists && len(a.rate) >= a.rateMax {
				for candidate, value := range a.rate {
					if value.minute < minute {
						delete(a.rate, candidate)
					}
				}
				if len(a.rate) >= a.rateMax {
					for candidate := range a.rate {
						delete(a.rate, candidate)
						break
					}
				}
			}
			if entry.minute != minute {
				entry = rateEntry{minute: minute}
			}
			entry.count++
			a.rate[host] = entry
			allowed = entry.count <= a.config.RateLimitPerMin
			a.mu.Unlock()
		}
		if !allowed {
			writeError(w, r, 429, "rate_limited", "request rate exceeded")
			return
		}
		next.ServeHTTP(w, r)
	})
}

func (a *API) withClientIP(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		next.ServeHTTP(w, r.WithContext(context.WithValue(r.Context(), clientIPKey, a.resolveClientIP(r))))
	})
}

func (a *API) resolveClientIP(r *http.Request) string {
	peer, ok := remoteIP(r.RemoteAddr)
	if !ok {
		return r.RemoteAddr
	}
	forwarded := r.Header.Get("X-Forwarded-For")
	if forwarded == "" || !a.isTrustedProxy(peer) {
		return peer.String()
	}
	parts := strings.Split(forwarded, ",")
	chain := make([]netip.Addr, 0, len(parts))
	for _, part := range parts {
		address, err := netip.ParseAddr(strings.TrimSpace(part))
		if err != nil {
			return peer.String()
		}
		chain = append(chain, address.Unmap())
	}
	for index := len(chain) - 1; index >= 0; index-- {
		if !a.isTrustedProxy(chain[index]) {
			return chain[index].String()
		}
	}
	if len(chain) != 0 {
		return chain[0].String()
	}
	return peer.String()
}

func remoteIP(remoteAddress string) (netip.Addr, bool) {
	host, _, err := net.SplitHostPort(remoteAddress)
	if err != nil {
		host = remoteAddress
	}
	address, err := netip.ParseAddr(host)
	if err != nil {
		return netip.Addr{}, false
	}
	return address.Unmap(), true
}

func (a *API) isTrustedProxy(address netip.Addr) bool {
	for _, prefix := range a.config.TrustedProxies {
		if prefix.Contains(address) {
			return true
		}
	}
	return false
}

func clientIP(r *http.Request) string {
	value, _ := r.Context().Value(clientIPKey).(string)
	return value
}

func (a *API) fail(w http.ResponseWriter, r *http.Request, err error) {
	a.errors.Add(1)
	switch {
	case errors.Is(err, domain.ErrInvalid):
		writeError(w, r, 400, "invalid_request", "request validation failed")
	case errors.Is(err, domain.ErrForbidden):
		writeError(w, r, 403, "forbidden", "operation is not allowed")
	case errors.Is(err, domain.ErrNotFound):
		writeError(w, r, 404, "not_found", "resource does not exist")
	case errors.Is(err, domain.ErrConflict):
		writeError(w, r, 409, "revision_conflict", "resource revision does not match")
	default:
		slog.Error("request failed", "error", err, "correlation_id", requestID(r))
		writeError(w, r, 500, "internal_error", "request could not be completed")
	}
}
func requestID(r *http.Request) string {
	value, _ := r.Context().Value(contextKey("request_id")).(string)
	return value
}
func decode(w http.ResponseWriter, r *http.Request, target any) bool {
	r.Body = http.MaxBytesReader(w, r.Body, 1<<20)
	decoder := json.NewDecoder(r.Body)
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(target); err != nil {
		writeError(w, r, 400, "invalid_json", "body is invalid or contains unknown fields")
		return false
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		writeError(w, r, 400, "invalid_json", "body must contain one JSON value")
		return false
	}
	return true
}
func writeJSON(w http.ResponseWriter, status int, value any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(value)
}
func writeError(w http.ResponseWriter, r *http.Request, status int, code, message string) {
	writeJSON(w, status, domain.ErrorEnvelope{Error: domain.APIError{Code: code, Message: message, CorrelationID: requestID(r)}})
}
func bodyHash(key string, body []byte) string {
	sum := sha256.Sum256(append([]byte(key+":"), body...))
	return hex.EncodeToString(sum[:])
}
func (a *API) idempotencyLookup(w http.ResponseWriter, r *http.Request, body []byte) (string, bool) {
	key := r.Header.Get("Idempotency-Key")
	if !domain.ValidateID(key) {
		writeError(w, r, 400, "idempotency_key_required", "a valid Idempotency-Key header is required")
		return "", true
	}
	digest := bodyHash(key, body)
	result, state, err := a.store.ReserveIdempotency(r.Context(), claims(r).Subject, r.Method, r.URL.Path, key, digest, time.Now().Add(2*time.Minute))
	if err != nil {
		a.fail(w, r, err)
		return "", true
	}
	switch state {
	case store.IdempotencyConflict:
		writeError(w, r, 409, "idempotency_conflict", "Idempotency-Key was already used with a different request body")
		return "", true
	case store.IdempotencyInProgress:
		writeError(w, r, 409, "idempotency_in_progress", "a request with this Idempotency-Key is still in progress")
		return "", true
	case store.IdempotencyReplay:
		w.Header().Set("Idempotency-Replayed", "true")
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(result.Status)
		_, _ = w.Write(result.Body)
		return "", true
	}
	return key + "\x00" + digest, false
}
func (a *API) idempotencySave(r *http.Request, hash string, status int, value any) ([]byte, error) {
	body, err := json.Marshal(value)
	if err != nil {
		return nil, err
	}
	parts := strings.SplitN(hash, "\x00", 2)
	if len(parts) != 2 {
		return nil, domain.ErrInvalid
	}
	err = a.store.FinalizeIdempotency(r.Context(), claims(r).Subject, r.Method, r.URL.Path, parts[0], store.IdempotencyResult{Status: status, Body: body, RequestHash: parts[1]}, time.Now().Add(24*time.Hour))
	return body, err
}
func writeRawJSON(w http.ResponseWriter, status int, body []byte) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_, _ = w.Write(body)
}

func (a *API) websocket(w http.ResponseWriter, r *http.Request) {
	after, _ := strconv.ParseInt(r.URL.Query().Get("after"), 10, 64)
	replay, err := a.store.ReplayEvents(r.Context(), claims(r).Subject, after, 256)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	upgrader := websocket.Upgrader{ReadBufferSize: 4096, WriteBufferSize: 4096, CheckOrigin: a.websocketOriginAllowed}
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		return
	}
	a.hub.Serve(conn, claims(r).Subject, replay)
}

func (a *API) websocketOriginAllowed(r *http.Request) bool {
	origin := r.Header.Get("Origin")
	if origin == "" {
		return true
	}
	requestOrigin, err := url.Parse(origin)
	if err != nil || requestOrigin.Scheme == "" || requestOrigin.Host == "" || requestOrigin.Opaque != "" || requestOrigin.User != nil || requestOrigin.ForceQuery || requestOrigin.RawQuery != "" || requestOrigin.Fragment != "" || requestOrigin.RawPath != "" || requestOrigin.Path != "" {
		return false
	}
	publicOrigin, err := url.Parse(a.config.PublicURL)
	if err != nil {
		return false
	}
	return strings.EqualFold(requestOrigin.Scheme, publicOrigin.Scheme) && strings.EqualFold(requestOrigin.Host, publicOrigin.Host)
}
func (a *API) events(w http.ResponseWriter, r *http.Request) {
	after, _ := strconv.ParseInt(r.URL.Query().Get("after"), 10, 64)
	waitMS, _ := strconv.Atoi(r.URL.Query().Get("wait_ms"))
	if waitMS < 0 || waitMS > 30000 {
		writeError(w, r, 400, "invalid_request", "wait_ms must be in [0,30000]")
		return
	}
	deadline := time.Now().Add(time.Duration(waitMS) * time.Millisecond)
	for {
		events, err := a.store.ReplayEvents(r.Context(), claims(r).Subject, after, 256)
		if err != nil {
			a.fail(w, r, err)
			return
		}
		if len(events) > 0 || time.Now().After(deadline) {
			writeJSON(w, 200, map[string]any{"events": events})
			return
		}
		select {
		case <-r.Context().Done():
			return
		case <-time.After(100 * time.Millisecond):
		}
	}
}

func decodeBase64(value string, max int) ([]byte, error) {
	raw, err := base64.StdEncoding.DecodeString(value)
	if err != nil || len(raw) > max {
		return nil, domain.ErrInvalid
	}
	return raw, nil
}
func readRaw(w http.ResponseWriter, r *http.Request) ([]byte, bool) {
	r.Body = http.MaxBytesReader(w, r.Body, 1<<20)
	raw, err := io.ReadAll(r.Body)
	if err != nil || !json.Valid(raw) {
		writeError(w, r, 400, "invalid_json", "body is invalid")
		return nil, false
	}
	return raw, true
}
func documentBody[T any](doc domain.Document) (T, error) {
	var value T
	err := json.Unmarshal(doc.Body, &value)
	return value, err
}
func marshal(value any) json.RawMessage {
	raw, err := json.Marshal(value)
	if err != nil {
		panic(err)
	}
	return raw
}
func parseRevision(r *http.Request) (int64, error) {
	value := r.Header.Get("If-Match")
	value = strings.Trim(value, "\"")
	if value == "" {
		return 0, domain.ErrInvalid
	}
	revision, err := strconv.ParseInt(value, 10, 64)
	if err != nil || revision < 1 {
		return 0, domain.ErrInvalid
	}
	return revision, nil
}
