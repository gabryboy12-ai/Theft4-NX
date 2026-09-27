package httpapi

import (
	"encoding/json"
	"net/http"
	"regexp"
	"strconv"
	"strings"
	"time"

	"libertyrecomp/community-server/internal/domain"
	"libertyrecomp/community-server/internal/store"
)

type sessionSearch struct {
	TitleID         string            `json:"title_id"`
	MediaID         string            `json:"media_id"`
	TitleVersion    string            `json:"title_version"`
	ProtocolVersion int               `json:"protocol_version"`
	ProcedureIndex  int               `json:"procedure_index"`
	Contexts        map[string]uint32 `json:"contexts"`
	Properties      map[string]string `json:"properties"`
	MaximumResults  int               `json:"maximum_results"`
}

var sessionIDPattern = regexp.MustCompile(`^0x[0-9a-fA-F]{16}$`)
var exchangeKeyPattern = regexp.MustCompile(`^0x[0-9a-fA-F]{32}$`)

func (a *API) createSession(w http.ResponseWriter, r *http.Request) {
	raw, ok := readRaw(w, r)
	if !ok {
		return
	}
	hash, replayed := a.idempotencyLookup(w, r, raw)
	if replayed {
		return
	}
	var session domain.SessionRecord
	if !strictRaw(raw, &session) {
		writeError(w, r, 400, "invalid_json", "body is invalid or contains unknown fields")
		return
	}
	device, err := a.store.GetDevice(r.Context(), claims(r).DeviceID)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	now := time.Now().UTC()
	session.ID = session.SessionID
	if !sessionIDPattern.MatchString(session.SessionID) || !exchangeKeyPattern.MatchString(session.ExchangeKey) {
		writeError(w, r, 400, "invalid_request", "session_id and exchange_key must be client-supplied hex strings")
		return
	}
	session.Revision = 1
	session.HostEpoch = 1
	session.HostXUID = device.XUID
	session.HostLeaseExpiresAt = now.Add(a.config.SessionLeaseTTL).Format(time.RFC3339Nano)
	session.State = "open"
	session.CreatedAt = now.Format(time.RFC3339Nano)
	session.UpdatedAt = session.CreatedAt
	var route domain.Route
	var hostMember domain.SessionMember
	if len(session.Members) > 0 {
		hostMember = session.Members[0]
		route = hostMember.Route
	}
	route.PeerID = 0
	hostMember.XUID = device.XUID
	hostMember.AccountID = device.AccountID
	if session.PublicSlots < 1 {
		hostMember.Private = true
	} else {
		hostMember.Private = false
	}
	if hostMember.MachineID == "" {
		hostMember.MachineID = session.HostMachineID
	}
	if hostMember.OnlinePort == 0 {
		hostMember.OnlinePort = session.HostPort
	}
	if hostMember.PeerID == "" {
		hostMember.PeerID = session.HostPeerID
	}
	hostMember.Role = "host"
	hostMember.Route = route
	hostMember.JoinedAt = now.Format(time.RFC3339Nano)
	virtualIP, err := a.relay.EnsureVirtualIPv4(r.Context(), device.AccountID)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	hostMember.VirtualIPv4 = virtualIP
	session.HostIPv4 = virtualIP
	if hostMember.OnlinePort > 0 {
		session.HostPort = hostMember.OnlinePort
	}
	session.Members = []domain.SessionMember{hostMember}
	if session.Visibility != "public" && session.Visibility != "private" && session.Visibility != "friends" {
		writeError(w, r, 400, "invalid_request", "visibility must be public, friends, or private")
		return
	}
	if err := validateSession(session, a.config.MaxSessionMembers); err != nil {
		a.fail(w, r, err)
		return
	}
	doc, err := a.store.CreateDocument(r.Context(), domain.Document{Kind: "session", ID: session.ID, OwnerID: device.AccountID, Body: marshal(session), Events: a.sessionEvents(r, session, "session.created")})
	if err != nil {
		a.fail(w, r, err)
		return
	}
	session.Revision = doc.Version
	body, err := a.idempotencySave(r, hash, 201, session)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	writeRawJSON(w, 201, body)
}

func (a *API) searchSessions(w http.ResponseWriter, r *http.Request) {
	var request sessionSearch
	if !decode(w, r, &request) {
		return
	}
	if request.MaximumResults < 1 || request.MaximumResults > 100 || request.ProtocolVersion < 1 || request.ProcedureIndex < 0 {
		writeError(w, r, 400, "invalid_request", "invalid search parameters")
		return
	}
	if !validEncodedProperties(request.Properties) {
		writeError(w, r, 400, "invalid_request", "properties must be base64 values of at most 512 bytes")
		return
	}
	documents, err := a.store.SearchSessions(r.Context(), store.SessionQuery{TitleID: request.TitleID, MediaID: request.MediaID, TitleVersion: request.TitleVersion, Requestor: claims(r).Subject, ProtocolVersion: request.ProtocolVersion, Contexts: request.Contexts, Properties: request.Properties, Limit: request.MaximumResults})
	if err != nil {
		a.fail(w, r, err)
		return
	}
	results := []domain.SessionRecord{}
	for _, doc := range documents {
		session, err := documentBody[domain.SessionRecord](doc)
		if err != nil {
			continue
		}
		session.Revision = doc.Version
		if session.State != "open" || session.TitleID != request.TitleID || session.MediaID != request.MediaID || session.TitleVersion != request.TitleVersion || session.ProtocolVersion != request.ProtocolVersion {
			continue
		}
		if !containsContexts(session.Contexts, request.Contexts) || !containsProperties(session.Properties, request.Properties) {
			continue
		}
		if session.Visibility == "private" && !session.ContainsAccount(claims(r).Subject) {
			continue
		}
		if len(session.Members) >= session.PublicSlots+session.PrivateSlots {
			continue
		}
		results = append(results, session)
		if len(results) == request.MaximumResults {
			break
		}
	}
	writeJSON(w, 200, map[string]any{"procedure_index": request.ProcedureIndex, "sessions": results})
}

func (a *API) getSession(w http.ResponseWriter, r *http.Request) {
	session, doc, err := a.loadSession(r)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	if session.Visibility == "private" && !session.ContainsAccount(claims(r).Subject) {
		a.fail(w, r, domain.ErrForbidden)
		return
	}
	session.Revision = doc.Version
	w.Header().Set("ETag", strconv.FormatInt(doc.Version, 10))
	writeJSON(w, 200, session)
}

func (a *API) getSessionByXboxID(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("xbox_id")
	if !sessionIDPattern.MatchString(id) {
		a.fail(w, r, domain.ErrInvalid)
		return
	}
	r.SetPathValue("id", id)
	a.getSession(w, r)
}

type sessionPatch struct {
	ExpectedRevision    int64             `json:"expected_revision"`
	State               *string           `json:"state,omitempty"`
	Visibility          *string           `json:"visibility,omitempty"`
	PublicSlots         *int              `json:"public_slots,omitempty"`
	PrivateSlots        *int              `json:"private_slots,omitempty"`
	Contexts            map[string]uint32 `json:"contexts,omitempty"`
	Properties          map[string]string `json:"properties,omitempty"`
	PreviousSessionID   *string           `json:"previous_session_id,omitempty"`
	Nonce               *string           `json:"nonce,omitempty"`
	Flags               *uint32           `json:"flags,omitempty"`
	LifecycleState      *uint32           `json:"lifecycle_state,omitempty"`
	HostMachineID       *string           `json:"host_machine_id,omitempty"`
	HostIPv4            *string           `json:"host_ipv4,omitempty"`
	HostPort            *int              `json:"host_port,omitempty"`
	HostEthernetAddress *string           `json:"host_ethernet_address,omitempty"`
	HostPeerID          *string           `json:"host_peer_id,omitempty"`
	Members             *[]domain.SessionMember `json:"members,omitempty"`
}

func (a *API) patchSession(w http.ResponseWriter, r *http.Request) {
	raw, ok := readRaw(w, r)
	if !ok {
		return
	}
	hash, replayed := a.idempotencyLookup(w, r, raw)
	if replayed {
		return
	}
	var patch sessionPatch
	if !strictRaw(raw, &patch) {
		writeError(w, r, 400, "invalid_json", "body is invalid or contains unknown fields")
		return
	}
	session, doc, err := a.loadSession(r)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	if session.HostXUID != a.xuid(r) || doc.OwnerID != claims(r).Subject {
		a.fail(w, r, domain.ErrForbidden)
		return
	}
	expected := patch.ExpectedRevision
	if expected == 0 {
		expected, err = parseRevision(r)
		if err != nil {
			a.fail(w, r, err)
			return
		}
	}
	if patch.State != nil {
		if *patch.State != "open" && *patch.State != "in_game" && *patch.State != "closed" {
			a.fail(w, r, domain.ErrInvalid)
			return
		}
		session.State = *patch.State
	}
	if patch.Visibility != nil {
		session.Visibility = *patch.Visibility
	}
	if patch.PublicSlots != nil {
		session.PublicSlots = *patch.PublicSlots
	}
	if patch.PrivateSlots != nil {
		session.PrivateSlots = *patch.PrivateSlots
	}
	if patch.Contexts != nil {
		session.Contexts = patch.Contexts
	}
	if patch.Properties != nil {
		session.Properties = patch.Properties
	}
	if patch.PreviousSessionID != nil {
		session.PreviousSessionID = *patch.PreviousSessionID
	}
	if patch.Nonce != nil {
		session.Nonce = *patch.Nonce
	}
	if patch.Flags != nil {
		session.Flags = *patch.Flags
	}
	if patch.LifecycleState != nil {
		session.LifecycleState = *patch.LifecycleState
	}
	if patch.HostMachineID != nil {
		session.HostMachineID = *patch.HostMachineID
	}
	if patch.HostIPv4 != nil {
		session.HostIPv4 = *patch.HostIPv4
	}
	if patch.HostPort != nil {
		session.HostPort = *patch.HostPort
	}
	if patch.HostEthernetAddress != nil {
		session.HostEthernetAddress = *patch.HostEthernetAddress
	}
	if patch.HostPeerID != nil {
		session.HostPeerID = *patch.HostPeerID
	}
	if patch.Members != nil {
		sanitized, sanitizeErr := a.sanitizeRoster(r, session, *patch.Members)
		if sanitizeErr != nil {
			a.fail(w, r, sanitizeErr)
			return
		}
		session.Members = sanitized
	}
	if !session.ContainsAccount(claims(r).Subject) {
		a.fail(w, r, domain.ErrForbidden)
		return
	}
	session.UpdatedAt = time.Now().UTC().Format(time.RFC3339Nano)
	if err := validateSession(session, a.config.MaxSessionMembers); err != nil {
		a.fail(w, r, err)
		return
	}
	session.Revision = expected + 1
	doc.Body = marshal(session)
	doc.Events = a.sessionEvents(r, session, "session.updated")
	updated, err := a.store.UpdateDocument(r.Context(), doc, expected)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	session.Revision = updated.Version
	body, err := a.idempotencySave(r, hash, 200, session)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	writeRawJSON(w, 200, body)
}

func (a *API) deleteSession(w http.ResponseWriter, r *http.Request) {
	raw := []byte(`{}`)
	hash, replayed := a.idempotencyLookup(w, r, raw)
	if replayed {
		return
	}
	session, doc, err := a.loadSession(r)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	if doc.OwnerID != claims(r).Subject {
		a.fail(w, r, domain.ErrForbidden)
		return
	}
	expected, err := parseRevision(r)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	session.Revision = expected + 1
	session.State = "deleted"
	if err := a.store.DeleteSession(r.Context(), session.ID, expected, a.sessionEvents(r, session, "session.deleted")); err != nil {
		a.fail(w, r, err)
		return
	}
	response := map[string]any{"id": session.ID, "deleted": true}
	body, err := a.idempotencySave(r, hash, 200, response)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	writeRawJSON(w, 200, body)
}

type actionRequest struct {
	ExpectedRevision  int64                 `json:"expected_revision"`
	ExpectedHostEpoch int64                 `json:"expected_host_epoch,omitempty"`
	NewHostXUID       string                `json:"new_host_xuid,omitempty"`
	Private           bool                  `json:"private,omitempty"`
	Route             domain.Route          `json:"route,omitempty"`
	Member            *domain.SessionMember `json:"member,omitempty"`
	Replacement       *domain.SessionRecord `json:"replacement,omitempty"`
}

func (a *API) sessionAction(w http.ResponseWriter, r *http.Request) {
	raw, ok := readRaw(w, r)
	if !ok {
		return
	}
	hash, replayed := a.idempotencyLookup(w, r, raw)
	if replayed {
		return
	}
	var request actionRequest
	if !strictRaw(raw, &request) || request.ExpectedRevision < 1 {
		writeError(w, r, 400, "invalid_request", "expected_revision is required")
		return
	}
	session, doc, err := a.loadSession(r)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	device, err := a.store.GetDevice(r.Context(), claims(r).DeviceID)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	action := strings.TrimPrefix(r.URL.Path, "/api/v2/sessions/"+session.ID+"/")
	eventType := "session." + action
	if action == "migration" && request.Replacement != nil {
		replacement, migrateErr := a.migrateReplacement(r, session, doc, device, request)
		if migrateErr != nil {
			a.fail(w, r, migrateErr)
			return
		}
		body, saveErr := a.idempotencySave(r, hash, 200, replacement)
		if saveErr != nil {
			a.fail(w, r, saveErr)
			return
		}
		writeRawJSON(w, 200, body)
		return
	}
	switch action {
	case "heartbeat":
		if session.HostXUID != device.XUID {
			err = domain.ErrForbidden
		} else {
			session.HostLeaseExpiresAt = time.Now().UTC().Add(a.config.SessionLeaseTTL).Format(time.RFC3339Nano)
		}
	case "join":
		virtualIP, ipErr := a.relay.EnsureVirtualIPv4(r.Context(), device.AccountID)
		if ipErr != nil {
			err = ipErr
		} else {
			err = a.join(&session, device.ID, device.AccountID, device.XUID, request.Private, request.Route, request.Member, virtualIP)
		}
	case "leave":
		err = a.leave(&session, device.AccountID)
	case "migration":
		err = a.migrate(&session, device.XUID, request.ExpectedHostEpoch, request.NewHostXUID)
	default:
		err = domain.ErrInvalid
	}
	if err != nil {
		a.fail(w, r, err)
		return
	}
	session.UpdatedAt = time.Now().UTC().Format(time.RFC3339Nano)
	if err := validateSession(session, a.config.MaxSessionMembers); err != nil {
		a.fail(w, r, err)
		return
	}
	session.Revision = request.ExpectedRevision + 1
	doc.Body = marshal(session)
	doc.Events = a.sessionEvents(r, session, eventType)
	updated, err := a.store.UpdateDocument(r.Context(), doc, request.ExpectedRevision)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	session.Revision = updated.Version
	body, err := a.idempotencySave(r, hash, 200, session)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	writeRawJSON(w, 200, body)
}

func (a *API) join(session *domain.SessionRecord, deviceID, accountID, xuid string, private bool, route domain.Route, supplied *domain.SessionMember, virtualIP string) error {
	if session.State != "open" || session.ContainsAccount(accountID) {
		return domain.ErrConflict
	}
	public, privateCount := 0, 0
	used := map[int]bool{}
	for _, m := range session.Members {
		if m.Private {
			privateCount++
		} else {
			public++
		}
		used[m.Route.PeerID] = true
	}
	if private {
		if privateCount >= session.PrivateSlots {
			return domain.ErrConflict
		}
	} else if public >= session.PublicSlots {
		return domain.ErrConflict
	}
	peer := -1
	for candidate := 0; candidate < a.config.MaxSessionMembers; candidate++ {
		if !used[candidate] {
			peer = candidate
			break
		}
	}
	if peer < 0 {
		return domain.ErrConflict
	}
	route.PeerID = peer
	member := domain.SessionMember{}
	if supplied != nil {
		member = *supplied
	}
	member.XUID = xuid
	member.AccountID = accountID
	member.Private = private
	member.Role = "player"
	member.Route = route
	member.VirtualIPv4 = virtualIP
	member.JoinedAt = time.Now().UTC().Format(time.RFC3339Nano)
	session.Members = append(session.Members, member)
	return nil
}

func (a *API) sanitizeRoster(r *http.Request, session domain.SessionRecord, supplied []domain.SessionMember) ([]domain.SessionMember, error) {
	existing := map[string]domain.SessionMember{}
	used := map[int]bool{}
	for _, member := range session.Members {
		existing[member.XUID] = member
	}
	for _, incoming := range supplied {
		if old, found := existing[incoming.XUID]; found {
			used[old.Route.PeerID] = true
		}
	}
	out := make([]domain.SessionMember, 0, len(supplied))
	for _, incoming := range supplied {
		clean := incoming
		if old, found := existing[incoming.XUID]; found {
			clean.AccountID = old.AccountID
			clean.Role = old.Role
			clean.JoinedAt = old.JoinedAt
			clean.Route = old.Route
		} else {
			device, err := a.store.GetDeviceByXUID(r.Context(), incoming.XUID)
			if err != nil {
				return nil, domain.ErrForbidden
			}
			clean.AccountID = device.AccountID
			clean.Role = "player"
			clean.JoinedAt = time.Now().UTC().Format(time.RFC3339Nano)
			assigned := -1
			for candidate := 0; candidate < a.config.MaxSessionMembers; candidate++ {
				if !used[candidate] {
					assigned = candidate
					break
				}
			}
			if assigned < 0 {
				return nil, domain.ErrConflict
			}
			clean.Route.PeerID = assigned
			used[assigned] = true
		}
		if incoming.XUID == session.HostXUID {
			clean.Role = "host"
		}
		virtualIP, err := a.relay.EnsureVirtualIPv4(r.Context(), clean.AccountID)
		if err != nil {
			return nil, err
		}
		clean.VirtualIPv4 = virtualIP
		out = append(out, clean)
	}
	return out, nil
}
func (a *API) leave(session *domain.SessionRecord, accountID string) error {
	index := -1
	for i, m := range session.Members {
		if m.AccountID == accountID {
			index = i
			break
		}
	}
	if index < 0 {
		return domain.ErrNotFound
	}
	if session.Members[index].XUID == session.HostXUID && len(session.Members) > 1 {
		return domain.ErrConflict
	}
	session.Members = append(session.Members[:index], session.Members[index+1:]...)
	if len(session.Members) == 0 {
		session.State = "closed"
	}
	return nil
}
func (a *API) migrate(session *domain.SessionRecord, actorXUID string, expectedEpoch int64, newHost string) error {
	if session.HostXUID != actorXUID || expectedEpoch != session.HostEpoch {
		return domain.ErrConflict
	}
	member, found := session.MemberByXUID(newHost)
	if !found {
		return domain.ErrInvalid
	}
	session.HostXUID = newHost
	session.HostEpoch++
	session.HostLeaseExpiresAt = time.Now().UTC().Add(a.config.SessionLeaseTTL).Format(time.RFC3339Nano)
	for i := range session.Members {
		if session.Members[i].XUID == newHost {
			session.Members[i].Role = "host"
		} else {
			session.Members[i].Role = "player"
		}
	}
	_ = member
	return nil
}

func (a *API) migrateReplacement(r *http.Request, old domain.SessionRecord, oldDoc domain.Document, actor domain.Device, request actionRequest) (domain.SessionRecord, error) {
	if old.HostXUID != actor.XUID || request.ExpectedHostEpoch != old.HostEpoch {
		return domain.SessionRecord{}, domain.ErrConflict
	}
	replacement := *request.Replacement
	if !sessionIDPattern.MatchString(replacement.SessionID) || !exchangeKeyPattern.MatchString(replacement.ExchangeKey) || replacement.SessionID == old.SessionID {
		return domain.SessionRecord{}, domain.ErrInvalid
	}
	replacement.ID = replacement.SessionID
	replacement.PreviousSessionID = old.SessionID
	replacement.Revision = 1
	replacement.HostEpoch = old.HostEpoch + 1
	replacement.State = "open"
	now := time.Now().UTC()
	replacement.CreatedAt = now.Format(time.RFC3339Nano)
	replacement.UpdatedAt = replacement.CreatedAt
	replacement.HostLeaseExpiresAt = now.Add(a.config.SessionLeaseTTL).Format(time.RFC3339Nano)
	sanitized, err := a.sanitizeRoster(r, old, replacement.Members)
	if err != nil {
		return domain.SessionRecord{}, err
	}
	replacement.Members = sanitized
	if replacement.HostXUID == "" {
		replacement.HostXUID = actor.XUID
	}
	hostDevice, err := a.store.GetDeviceByXUID(r.Context(), replacement.HostXUID)
	if err != nil {
		return domain.SessionRecord{}, domain.ErrForbidden
	}
	if !replacement.ContainsAccount(hostDevice.AccountID) {
		return domain.SessionRecord{}, domain.ErrInvalid
	}
	for i := range replacement.Members {
		if replacement.Members[i].XUID == replacement.HostXUID {
			replacement.Members[i].Role = "host"
			replacement.HostIPv4 = replacement.Members[i].VirtualIPv4
			if replacement.Members[i].OnlinePort > 0 {
				replacement.HostPort = replacement.Members[i].OnlinePort
			}
		} else {
			replacement.Members[i].Role = "player"
		}
	}
	if err := validateSession(replacement, a.config.MaxSessionMembers); err != nil {
		return domain.SessionRecord{}, err
	}
	old.State = "closed"
	old.UpdatedAt = now.Format(time.RFC3339Nano)
	old.Revision = request.ExpectedRevision + 1
	replacement.Revision = 1
	oldDoc.Body = marshal(old)
	oldDoc.Events = append(a.sessionEvents(r, old, "session.migrated_from"), a.sessionEvents(r, replacement, "session.migrated_to")...)
	newDoc := domain.Document{Kind: "session", ID: replacement.ID, OwnerID: hostDevice.AccountID, Body: marshal(replacement)}
	oldUpdated, newCreated, err := a.store.MigrateSession(r.Context(), oldDoc, newDoc, request.ExpectedRevision)
	if err != nil {
		return domain.SessionRecord{}, err
	}
	old.Revision = oldUpdated.Version
	replacement.Revision = newCreated.Version
	return replacement, nil
}

func (a *API) loadSession(r *http.Request) (domain.SessionRecord, domain.Document, error) {
	id := r.PathValue("id")
	if !domain.ValidateID(id) {
		return domain.SessionRecord{}, domain.Document{}, domain.ErrInvalid
	}
	doc, err := a.store.GetDocument(r.Context(), "session", id)
	if err != nil {
		return domain.SessionRecord{}, doc, err
	}
	session, err := documentBody[domain.SessionRecord](doc)
	if err == nil && !sessionLeaseActive(session, time.Now().UTC()) {
		return domain.SessionRecord{}, doc, domain.ErrNotFound
	}
	return session, doc, err
}

func sessionLeaseActive(session domain.SessionRecord, now time.Time) bool {
	lease, err := time.Parse(time.RFC3339Nano, session.HostLeaseExpiresAt)
	return err == nil && now.Before(lease)
}
func (a *API) xuid(r *http.Request) string {
	device, err := a.store.GetDevice(r.Context(), claims(r).DeviceID)
	if err != nil {
		return ""
	}
	return device.XUID
}
func (a *API) sessionEvents(r *http.Request, session domain.SessionRecord, eventType string) []domain.Event {
	payload := marshal(session)
	seen := map[string]bool{}
	events := []domain.Event{}
	for _, member := range session.Members {
		if seen[member.AccountID] {
			continue
		}
		seen[member.AccountID] = true
		events = append(events, domain.Event{EventKey: domain.NewID("evt"), UserID: member.AccountID, Type: eventType, AggregateType: "session", AggregateID: session.ID, Revision: session.Revision, CorrelationID: requestID(r), Payload: payload})
	}
	return events
}

func validateSession(session domain.SessionRecord, max int) error {
	if err := session.Validate(max); err != nil {
		return err
	}
	if !validEncodedProperties(session.Properties) {
		return domain.ErrInvalid
	}
	return nil
}
func validEncodedProperties(values map[string]string) bool {
	for _, value := range values {
		if _, err := decodeBase64(value, 512); err != nil {
			return false
		}
	}
	return true
}
func containsContexts(values, required map[string]uint32) bool {
	for key, value := range required {
		if values[key] != value {
			return false
		}
	}
	return true
}
func containsProperties(values, required map[string]string) bool {
	for key, value := range required {
		if values[key] != value {
			return false
		}
	}
	return true
}
func strictRaw(raw []byte, target any) bool {
	decoder := json.NewDecoder(strings.NewReader(string(raw)))
	decoder.DisallowUnknownFields()
	if decoder.Decode(target) != nil {
		return false
	}
	return decoder.Decode(&struct{}{}) != nil
}
