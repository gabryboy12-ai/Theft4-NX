package httpapi

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"encoding/base64"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"testing"
	"time"

	"libertyrecomp/community-server/internal/auth"
	"libertyrecomp/community-server/internal/config"
	"libertyrecomp/community-server/internal/domain"
	"libertyrecomp/community-server/internal/realtime"
	"libertyrecomp/community-server/internal/relay"
	"libertyrecomp/community-server/internal/store"
)

func testAPI(t *testing.T) (http.Handler, *auth.Service, store.Store) {
	t.Helper()
	repository := store.NewMemory()
	cfg := config.Config{Role: "api", PublicURL: "https://community.example.test", TokenSigningKey: []byte("0123456789abcdef0123456789abcdef"), AccessTTL: time.Hour, RefreshTTL: time.Hour, ChallengeTTL: time.Minute, SessionLeaseTTL: time.Minute, MaxSessionMembers: 64, RateLimitPerMin: 1000}
	authService := auth.New(repository, cfg.TokenSigningKey, cfg.AccessTTL, cfg.RefreshTTL, cfg.ChallengeTTL)
	hub, err := realtime.New(repository, "")
	if err != nil {
		t.Fatal(err)
	}
	relayService, err := relay.New("")
	if err != nil {
		t.Fatal(err)
	}
	api, err := New(cfg, repository, authService, hub, relayService)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { api.Close(); hub.Close(); relayService.Close() })
	return api.Handler(), authService, repository
}

func TestHealthExposesPublicURL(t *testing.T) {
	handler, _, _ := testAPI(t)
	response := request(t, handler, http.MethodGet, "/health/live", "", "", nil)
	if response.Code != http.StatusOK || !bytes.Contains(response.Body.Bytes(), []byte(`"public_url":"https://community.example.test"`)) {
		t.Fatalf("health status=%d body=%s", response.Code, response.Body.String())
	}
}

func TestClientIPTrustsOnlyConfiguredProxyChains(t *testing.T) {
	api := &API{config: config.Config{TrustedProxies: []netip.Prefix{netip.MustParsePrefix("172.30.0.0/24")}}}
	tests := []struct {
		name       string
		remoteAddr string
		forwarded  string
		want       string
	}{
		{name: "trusted proxy", remoteAddr: "172.30.0.4:8080", forwarded: "198.51.100.8, 172.30.0.3", want: "198.51.100.8"},
		{name: "untrusted peer", remoteAddr: "203.0.113.9:8080", forwarded: "198.51.100.8", want: "203.0.113.9"},
		{name: "invalid chain", remoteAddr: "172.30.0.4:8080", forwarded: "not-an-ip", want: "172.30.0.4"},
		{name: "no header", remoteAddr: "192.0.2.7:8080", want: "192.0.2.7"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			req := httptest.NewRequest(http.MethodGet, "/health/live", nil)
			req.RemoteAddr = test.remoteAddr
			req.Header.Set("X-Forwarded-For", test.forwarded)
			if got := api.resolveClientIP(req); got != test.want {
				t.Fatalf("resolveClientIP() = %q, want %q", got, test.want)
			}
		})
	}
}

func TestRateLimitUsesClientIPAndBoundsMemory(t *testing.T) {
	api := &API{
		config:  config.Config{RateLimitPerMin: 1, TrustedProxies: []netip.Prefix{netip.MustParsePrefix("172.30.0.0/24")}},
		rate:    map[string]rateEntry{},
		rateMax: 3,
	}
	handler := api.withClientIP(api.rateLimit(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusNoContent)
	})))
	call := func(forwarded string) int {
		req := httptest.NewRequest(http.MethodGet, "/", nil)
		req.RemoteAddr = "172.30.0.4:8080"
		req.Header.Set("X-Forwarded-For", forwarded)
		response := httptest.NewRecorder()
		handler.ServeHTTP(response, req)
		return response.Code
	}
	if got := call("198.51.100.1"); got != http.StatusNoContent {
		t.Fatalf("first client status = %d", got)
	}
	if got := call("198.51.100.2"); got != http.StatusNoContent {
		t.Fatalf("second client status = %d", got)
	}
	if got := call("198.51.100.1"); got != http.StatusTooManyRequests {
		t.Fatalf("repeated client status = %d", got)
	}
	call("198.51.100.3")
	call("198.51.100.4")
	if len(api.rate) > api.rateMax {
		t.Fatalf("rate map size = %d, maximum = %d", len(api.rate), api.rateMax)
	}
}

func TestWebSocketOriginMatchesPublicURL(t *testing.T) {
	api := &API{config: config.Config{PublicURL: "https://community.example.test"}}
	tests := []struct {
		origin string
		want   bool
	}{
		{origin: "", want: true},
		{origin: "https://community.example.test", want: true},
		{origin: "https://COMMUNITY.EXAMPLE.TEST", want: true},
		{origin: "http://community.example.test", want: false},
		{origin: "https://attacker.example.test", want: false},
		{origin: "https://community.example.test/path", want: false},
	}
	for _, test := range tests {
		req := httptest.NewRequest(http.MethodGet, "/api/v2/realtime", nil)
		req.Header.Set("Origin", test.origin)
		if got := api.websocketOriginAllowed(req); got != test.want {
			t.Errorf("websocketOriginAllowed(%q) = %t, want %t", test.origin, got, test.want)
		}
	}
}
func enrollFixture(t *testing.T, service *auth.Service, device, xuid string) string {
	t.Helper()
	seed := bytes.Repeat([]byte{byte(len(device) + 1)}, ed25519.SeedSize)
	private := ed25519.NewKeyFromSeed(seed)
	public := base64.RawURLEncoding.EncodeToString(private.Public().(ed25519.PublicKey))
	challenge, err := service.Challenge(context.Background(), device, xuid, public)
	if err != nil {
		t.Fatal(err)
	}
	signature := base64.RawURLEncoding.EncodeToString(ed25519.Sign(private, []byte(challenge.Nonce)))
	pair, err := service.Enroll(context.Background(), auth.EnrollRequest{DeviceID: device, XUID: xuid, MachineID: "machine_" + device, PlayerName: "Player", PublicKey: public, ChallengeID: challenge.ID, Signature: signature})
	if err != nil {
		t.Fatal(err)
	}
	return pair.AccessToken
}
func request(t *testing.T, handler http.Handler, method, path, token, key string, body any) *httptest.ResponseRecorder {
	t.Helper()
	raw, _ := json.Marshal(body)
	req := httptest.NewRequest(method, path, bytes.NewReader(raw))
	req.RemoteAddr = "127.0.0.1:10000"
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	if key != "" {
		req.Header.Set("Idempotency-Key", key)
	}
	rec := httptest.NewRecorder()
	handler.ServeHTTP(rec, req)
	return rec
}

func TestSessionXboxIDPropertyBoundaryAndIdempotency(t *testing.T) {
	handler, authService, _ := testAPI(t)
	token := enrollFixture(t, authService, "device_one", "0xe000000000000001")
	property := base64.StdEncoding.EncodeToString(bytes.Repeat([]byte{0x5a}, 512))
	session := domain.SessionRecord{SessionID: "0x0000000000000001", ExchangeKey: "0x00000000000000000000000000000001", Mode: "free_mode", Episode: "base", Region: "na", Visibility: "public", PublicSlots: 64, BuildID: "build_8", ProtocolVersion: 2, TitleID: "0x545407f2", MediaID: "0x12345678", TitleVersion: "0x00000008", Contexts: map[string]uint32{}, Properties: map[string]string{"blob": property}, Members: []domain.SessionMember{{MachineID: "0x0000000000000011", PeerID: "peer_host", Route: domain.Route{PeerID: 0}}}}
	created := request(t, handler, http.MethodPost, "/api/v2/sessions", token, "create_fixture", session)
	if created.Code != 201 {
		t.Fatalf("create status=%d body=%s", created.Code, created.Body.String())
	}
	var record domain.SessionRecord
	if err := json.Unmarshal(created.Body.Bytes(), &record); err != nil {
		t.Fatal(err)
	}
	if record.ID != session.SessionID || record.SessionID != session.SessionID || record.ExchangeKey != session.ExchangeKey {
		t.Fatalf("identifiers did not round-trip: %+v", record)
	}
	lookup := request(t, handler, http.MethodGet, "/api/v2/sessions/by-xbox-id/"+session.SessionID, token, "", nil)
	if lookup.Code != 200 {
		t.Fatalf("lookup status=%d", lookup.Code)
	}
	session.Mode = "race"
	conflict := request(t, handler, http.MethodPost, "/api/v2/sessions", token, "create_fixture", session)
	if conflict.Code != 409 {
		t.Fatalf("idempotency mismatch status=%d body=%s", conflict.Code, conflict.Body.String())
	}
	replacement := record
	replacement.ID = ""
	replacement.SessionID = "0x0000000000000002"
	replacement.ExchangeKey = "0x00000000000000000000000000000002"
	replacement.PreviousSessionID = ""
	replacement.Revision = 0
	replacement.HostEpoch = 0
	replacement.State = ""
	replacement.CreatedAt = ""
	replacement.UpdatedAt = ""
	migrated := request(t, handler, http.MethodPost, "/api/v2/sessions/"+record.ID+"/migration", token, "migration_fixture", map[string]any{"expected_revision": record.Revision, "expected_host_epoch": record.HostEpoch, "replacement": replacement})
	if migrated.Code != 200 {
		t.Fatalf("migration status=%d body=%s", migrated.Code, migrated.Body.String())
	}
	var migratedRecord domain.SessionRecord
	if err := json.Unmarshal(migrated.Body.Bytes(), &migratedRecord); err != nil {
		t.Fatal(err)
	}
	if migratedRecord.ID != replacement.SessionID || migratedRecord.PreviousSessionID != record.SessionID || migratedRecord.HostEpoch != record.HostEpoch+1 {
		t.Fatalf("migration linkage invalid: %+v", migratedRecord)
	}
	oldLookup := request(t, handler, http.MethodGet, "/api/v2/sessions/"+record.ID, token, "", nil)
	if oldLookup.Code != 200 {
		t.Fatalf("old lookup status=%d", oldLookup.Code)
	}
	var old domain.SessionRecord
	_ = json.Unmarshal(oldLookup.Body.Bytes(), &old)
	if old.State != "closed" {
		t.Fatalf("old state=%s", old.State)
	}
}

func TestTypedSocialStatsInvitesAndVoice(t *testing.T) {
	handler, authService, _ := testAPI(t)
	firstXUID := "0xe000000000000011"
	secondXUID := "0xe000000000000022"
	first := enrollFixture(t, authService, "device_social_one", firstXUID)
	second := enrollFixture(t, authService, "device_social_two", secondXUID)
	session := domain.SessionRecord{SessionID: "0x0000000000000011", ExchangeKey: "0x00000000000000000000000000000011", Mode: "free_mode", Episode: "base", Region: "na", Visibility: "public", PublicSlots: 64, BuildID: "build_8", ProtocolVersion: 2, TitleID: "0x545407f2", MediaID: "0x12345678", TitleVersion: "0x00000008", Contexts: map[string]uint32{}, Properties: map[string]string{}, HostMachineID: "0x0000000000000011", HostPeerID: "peer_one", HostPort: 3074}
	created := request(t, handler, http.MethodPost, "/api/v2/sessions", first, "social_session", session)
	if created.Code != 201 {
		t.Fatalf("create: %d %s", created.Code, created.Body.String())
	}
	var record domain.SessionRecord
	_ = json.Unmarshal(created.Body.Bytes(), &record)
	joined := request(t, handler, http.MethodPost, "/api/v2/sessions/"+record.ID+"/join", second, "social_join", map[string]any{"expected_revision": record.Revision, "member": map[string]any{"xuid": secondXUID, "machine_id": "0x0000000000000022", "online_port": 3075, "peer_id": "peer_two"}})
	if joined.Code != 200 {
		t.Fatalf("join: %d %s", joined.Code, joined.Body.String())
	}
	_ = json.Unmarshal(joined.Body.Bytes(), &record)
	if len(record.Members) != 2 || record.Members[1].Route.PeerID != 1 {
		t.Fatalf("assigned roster invalid: %+v", record.Members)
	}
	chat := request(t, handler, http.MethodPost, "/api/v2/chat/messages", first,
		"chat_all_one", map[string]any{"session_id": record.ID, "channel": "all",
			"target_xuids": []string{}, "sequence": 7, "text": "hello \U0001F680"})
	if chat.Code != 201 {
		t.Fatalf("chat send: %d %s", chat.Code, chat.Body.String())
	}
	events := request(t, handler, http.MethodGet, "/api/v2/events?after=0&wait_ms=0",
		second, "", nil)
	if events.Code != 200 || !bytes.Contains(events.Body.Bytes(), []byte(`"type":"chat.message"`)) ||
		!bytes.Contains(events.Body.Bytes(), []byte(`"source_xuid":"`+firstXUID+`"`)) ||
		!bytes.Contains(events.Body.Bytes(), []byte("hello \U0001F680")) {
		t.Fatalf("chat receive: %d %s", events.Code, events.Body.String())
	}
	invalidTeam := request(t, handler, http.MethodPost, "/api/v2/chat/messages", first,
		"chat_team_invalid", map[string]any{"session_id": record.ID, "channel": "team",
			"target_xuids": []string{}, "sequence": 8, "text": "not broadcast"})
	if invalidTeam.Code != 400 {
		t.Fatalf("empty team chat was accepted: %d %s", invalidTeam.Code, invalidTeam.Body.String())
	}
	friendRequest := request(t, handler, http.MethodPost, "/api/v2/friends/relationships", first, "friend_request", map[string]any{"target_xuid": secondXUID, "action": "request"})
	if friendRequest.Code != 200 {
		t.Fatalf("friend request: %d %s", friendRequest.Code, friendRequest.Body.String())
	}
	friendAccept := request(t, handler, http.MethodPost, "/api/v2/friends/relationships", second, "friend_accept", map[string]any{"target_xuid": firstXUID, "action": "accept"})
	if friendAccept.Code != 200 {
		t.Fatalf("friend accept: %d %s", friendAccept.Code, friendAccept.Body.String())
	}
	check := request(t, handler, http.MethodPost, "/api/v2/friends/check", first, "", map[string]any{"xuids": []string{secondXUID}})
	if check.Code != 200 || !bytes.Contains(check.Body.Bytes(), []byte(`"is_friend":true`)) {
		t.Fatalf("friend check: %d %s", check.Code, check.Body.String())
	}
	invites := request(t, handler, http.MethodPost, "/api/v2/invites", first, "invite_create", map[string]any{"session_id": record.ID, "recipient_xuids": []string{secondXUID}, "custom_data": base64.StdEncoding.EncodeToString([]byte("next-game")), "expires_in_seconds": 600})
	if invites.Code != 201 {
		t.Fatalf("invite create: %d %s", invites.Code, invites.Body.String())
	}
	var inviteResponse struct {
		Invites []inviteRecord `json:"invites"`
	}
	_ = json.Unmarshal(invites.Body.Bytes(), &inviteResponse)
	accepted := request(t, handler, http.MethodPost, "/api/v2/invites/"+inviteResponse.Invites[0].ID+"/accept", second, "invite_accept", map[string]any{"expected_revision": 1})
	if accepted.Code != 200 {
		t.Fatalf("invite accept: %d %s", accepted.Code, accepted.Body.String())
	}
	invalidBatch := request(t, handler, http.MethodPost, "/api/v2/stats/writes", first, "stats_invalid_batch", map[string]any{
		"session_id": record.ID,
		"sequence":   "0",
		"views": []any{
			map[string]any{"view_id": "0x00000001", "rows": []any{map[string]any{"xuid": firstXUID, "columns": map[string]any{"0x00000002": map[string]any{"type": "i64", "value": 7}}}}},
			map[string]any{"view_id": "INVALID", "rows": []any{map[string]any{"xuid": firstXUID, "columns": map[string]any{"0x00000002": map[string]any{"type": "i64", "value": 8}}}}},
		},
	})
	if invalidBatch.Code != 400 {
		t.Fatalf("invalid stats batch: %d %s", invalidBatch.Code, invalidBatch.Body.String())
	}
	emptyRead := request(t, handler, http.MethodPost, "/api/v2/stats/read", first, "", map[string]any{"xuids": []string{firstXUID}, "view_id": "0x00000001", "stat_ids": []string{"0x00000002"}})
	if emptyRead.Code != 200 || !bytes.Contains(emptyRead.Body.Bytes(), []byte(`"columns":{}`)) {
		t.Fatalf("invalid batch partially committed: %d %s", emptyRead.Code, emptyRead.Body.String())
	}
	stats := request(t, handler, http.MethodPost, "/api/v2/stats/writes", first, "stats_write", map[string]any{"session_id": record.ID, "sequence": "1", "views": []any{map[string]any{"view_id": "0x00000001", "rows": []any{map[string]any{"xuid": firstXUID, "columns": map[string]any{"0x00000002": map[string]any{"type": "i64", "value": 9223372036854775806}}}}}}})
	if stats.Code != 200 {
		t.Fatalf("stats: %d %s", stats.Code, stats.Body.String())
	}
	secondStats := request(t, handler, http.MethodPost, "/api/v2/stats/writes", second, "stats_write_second", map[string]any{"session_id": record.ID, "sequence": "2", "views": []any{map[string]any{"view_id": "0x00000001", "rows": []any{map[string]any{"xuid": secondXUID, "columns": map[string]any{"0x00000002": map[string]any{"type": "i64", "value": 9223372036854775805}}}}}}})
	if secondStats.Code != 200 {
		t.Fatalf("second stats: %d %s", secondStats.Code, secondStats.Body.String())
	}
	board := request(t, handler, http.MethodGet, "/api/v2/leaderboards/0x00000001?stat_id=0x00000002&offset=0&limit=10", first, "", nil)
	if board.Code != 200 || !bytes.Contains(board.Body.Bytes(), []byte(`"rank":1`)) {
		t.Fatalf("board: %d %s", board.Code, board.Body.String())
	}
	var boardResponse struct {
		Rows []struct {
			XUID  string    `json:"xuid"`
			Value statValue `json:"value"`
		} `json:"rows"`
	}
	if json.Unmarshal(board.Body.Bytes(), &boardResponse) != nil || len(boardResponse.Rows) != 2 || boardResponse.Rows[0].XUID != firstXUID || boardResponse.Rows[0].Value.Type != "i64" || string(boardResponse.Rows[0].Value.Value) != "9223372036854775806" {
		t.Fatalf("leaderboard lost i64 precision or typed value: %s", board.Body.String())
	}
	voice := request(t, handler, http.MethodPost, "/api/v2/voice/routes", first, "", map[string]any{"session_id": record.ID, "channel": "all", "target_xuids": []string{}, "mute_xuids": []string{}})
	if voice.Code != 201 {
		t.Fatalf("voice route: %d %s", voice.Code, voice.Body.String())
	}
	var route voiceRouteRecord
	_ = json.Unmarshal(voice.Body.Bytes(), &route)
	secondVoice := request(t, handler, http.MethodPost, "/api/v2/voice/routes", second, "", map[string]any{"session_id": record.ID, "channel": "all", "target_xuids": []string{}, "mute_xuids": []string{}})
	if secondVoice.Code != 201 {
		t.Fatalf("second voice route: %d %s", secondVoice.Code, secondVoice.Body.String())
	}
	var secondRoute voiceRouteRecord
	_ = json.Unmarshal(secondVoice.Body.Bytes(), &secondRoute)
	packet := request(t, handler, http.MethodPost, "/api/v2/voice/packets", first, "", map[string]any{"packets": []any{
		map[string]any{"route_token": route.Token, "sequence": 1, "payload": base64.StdEncoding.EncodeToString([]byte("opus-one"))},
		map[string]any{"route_token": route.Token, "sequence": 2, "payload": base64.StdEncoding.EncodeToString([]byte("opus-two"))},
	}})
	if packet.Code != 202 {
		t.Fatalf("voice packet: %d %s", packet.Code, packet.Body.String())
	}
	received := request(t, handler, http.MethodGet, "/api/v2/voice/packets?route_token="+secondRoute.Token+"&after=0&wait_ms=0", second, "", nil)
	var voiceResponse struct {
		Packets []struct {
			SourceXUID string `json:"source_xuid"`
			SessionID  string `json:"session_id"`
			Sequence   uint32 `json:"sequence"`
			Payload    string `json:"payload"`
		} `json:"packets"`
		NextAfter string `json:"next_after"`
	}
	if received.Code != 200 || json.Unmarshal(received.Body.Bytes(), &voiceResponse) != nil || len(voiceResponse.Packets) != 2 || voiceResponse.Packets[0].SourceXUID != firstXUID || voiceResponse.Packets[0].SessionID != record.ID || voiceResponse.Packets[0].Sequence != 1 || voiceResponse.Packets[1].Sequence != 2 || voiceResponse.NextAfter == "" {
		t.Fatalf("voice receive: %d %s", received.Code, received.Body.String())
	}
	replacementVoice := request(t, handler, http.MethodPost, "/api/v2/voice/routes", second, "", map[string]any{"session_id": record.ID, "channel": "private", "target_xuids": []string{firstXUID}, "mute_xuids": []string{}})
	if replacementVoice.Code != 201 {
		t.Fatalf("voice replacement: %d %s", replacementVoice.Code, replacementVoice.Body.String())
	}
	oldPoll := request(t, handler, http.MethodGet, "/api/v2/voice/packets?route_token="+secondRoute.Token+"&wait_ms=0", second, "", nil)
	if oldPoll.Code != 403 {
		t.Fatalf("replaced token remained active: %d %s", oldPoll.Code, oldPoll.Body.String())
	}
	var replacementRoute voiceRouteRecord
	_ = json.Unmarshal(replacementVoice.Body.Bytes(), &replacementRoute)
	deleted := request(t, handler, http.MethodDelete, "/api/v2/voice/routes", second, "", map[string]any{"route_token": replacementRoute.Token})
	if deleted.Code != 200 {
		t.Fatalf("voice delete: %d %s", deleted.Code, deleted.Body.String())
	}
	deletedPoll := request(t, handler, http.MethodGet, "/api/v2/voice/packets?route_token="+replacementRoute.Token+"&wait_ms=0", second, "", nil)
	if deletedPoll.Code != 403 {
		t.Fatalf("deleted token remained active: %d %s", deletedPoll.Code, deletedPoll.Body.String())
	}
	reset := request(t, handler, http.MethodPost, "/api/v2/stats/reset", first, "stats_reset", map[string]any{"view_id": "0x00000001"})
	if reset.Code != 200 || !bytes.Contains(reset.Body.Bytes(), []byte(`"reset":true`)) {
		t.Fatalf("stats reset: %d %s", reset.Code, reset.Body.String())
	}
}
