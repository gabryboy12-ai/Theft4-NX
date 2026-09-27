package httpapi

import (
	"encoding/base64"
	"encoding/json"
	"net"
	"net/http"
	"strconv"
	"strings"
	"time"

	"libertyrecomp/community-server/internal/domain"
	"libertyrecomp/community-server/internal/relay"
)

var resourceKinds = map[string]bool{"friends": true, "blocks": true, "presence": true, "recent-players": true, "parties": true, "invites": true, "match-results": true, "profiles": true, "leaderboard-entries": true, "achievements": true, "viral-events": true, "reports": true, "bans": true}

type resourcePayload struct {
	TargetAccountID string            `json:"target_account_id,omitempty"`
	SessionID       string            `json:"session_id,omitempty"`
	Category        string            `json:"category,omitempty"`
	Value           string            `json:"value,omitempty"`
	Values          map[string]int64  `json:"values,omitempty"`
	Metadata        map[string]string `json:"metadata,omitempty"`
	ExpiresAt       string            `json:"expires_at,omitempty"`
}
type resourceResponse struct {
	ID        string          `json:"id"`
	Kind      string          `json:"kind"`
	OwnerID   string          `json:"owner_id"`
	Revision  int64           `json:"revision"`
	Data      resourcePayload `json:"data"`
	CreatedAt time.Time       `json:"created_at"`
	UpdatedAt time.Time       `json:"updated_at"`
}

func resourceKind(r *http.Request) string {
	kind := r.PathValue("kind")
	if kind != "" {
		return kind
	}
	if strings.HasSuffix(r.URL.Path, "/reports") {
		return "reports"
	}
	if strings.HasSuffix(r.URL.Path, "/bans") {
		return "bans"
	}
	return ""
}
func validResource(p resourcePayload) bool {
	if p.TargetAccountID != "" && !domain.ValidateID(p.TargetAccountID) {
		return false
	}
	if p.SessionID != "" && !domain.ValidateID(p.SessionID) {
		return false
	}
	if len(p.Category) > 64 || len(p.Value) > 512 || len(p.Values) > 64 || len(p.Metadata) > 64 {
		return false
	}
	for k, v := range p.Metadata {
		if !domain.ValidateID(k) || len(v) > 512 {
			return false
		}
	}
	return true
}
func resourceFromDocument(doc domain.Document) (resourceResponse, error) {
	data, err := documentBody[resourcePayload](doc)
	return resourceResponse{ID: doc.ID, Kind: doc.Kind, OwnerID: doc.OwnerID, Revision: doc.Version, Data: data, CreatedAt: doc.CreatedAt, UpdatedAt: doc.UpdatedAt}, err
}
func (a *API) createResource(w http.ResponseWriter, r *http.Request) {
	kind := resourceKind(r)
	if !resourceKinds[kind] {
		writeError(w, r, 404, "not_found", "unknown resource kind")
		return
	}
	raw, ok := readRaw(w, r)
	if !ok {
		return
	}
	hash, replayed := a.idempotencyLookup(w, r, raw)
	if replayed {
		return
	}
	var payload resourcePayload
	if !strictRaw(raw, &payload) || !validResource(payload) {
		writeError(w, r, 400, "invalid_request", "resource validation failed")
		return
	}
	if kind == "bans" && !authRole(r, "moderator") {
		a.fail(w, r, domain.ErrForbidden)
		return
	}
	resourceID := domain.NewID(strings.TrimSuffix(kind, "s"))
	event := domain.Event{EventKey: domain.NewID("evt"), UserID: claims(r).Subject, Type: kind + ".created", AggregateType: kind, AggregateID: resourceID, Revision: 1, CorrelationID: requestID(r), Payload: marshal(payload)}
	doc, err := a.store.CreateDocument(r.Context(), domain.Document{Kind: kind, ID: resourceID, OwnerID: claims(r).Subject, Body: marshal(payload), Events: []domain.Event{event}})
	if err != nil {
		a.fail(w, r, err)
		return
	}
	response, _ := resourceFromDocument(doc)
	_ = a.store.AppendAudit(r.Context(), claims(r).Subject, kind+".create", kind, doc.ID, raw)
	body, err := a.idempotencySave(r, hash, 201, response)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	writeRawJSON(w, 201, body)
}
func (a *API) listResource(w http.ResponseWriter, r *http.Request) {
	kind := resourceKind(r)
	if !resourceKinds[kind] {
		writeError(w, r, 404, "not_found", "unknown resource kind")
		return
	}
	docs, err := a.store.ListDocuments(r.Context(), kind, claims(r).Subject, 100)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	out := []resourceResponse{}
	for _, doc := range docs {
		value, err := resourceFromDocument(doc)
		if err == nil {
			out = append(out, value)
		}
	}
	writeJSON(w, 200, map[string]any{"items": out})
}
func (a *API) getResource(w http.ResponseWriter, r *http.Request) {
	kind := resourceKind(r)
	doc, err := a.store.GetDocument(r.Context(), kind, r.PathValue("id"))
	if err != nil {
		a.fail(w, r, err)
		return
	}
	if doc.OwnerID != claims(r).Subject && !authRole(r, "moderator") {
		a.fail(w, r, domain.ErrForbidden)
		return
	}
	response, err := resourceFromDocument(doc)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	w.Header().Set("ETag", strconv.FormatInt(doc.Version, 10))
	writeJSON(w, 200, response)
}
func (a *API) patchResource(w http.ResponseWriter, r *http.Request) {
	kind := resourceKind(r)
	if kind == "match-results" || kind == "reports" {
		writeError(w, r, 409, "immutable", "this ledger resource is immutable")
		return
	}
	raw, ok := readRaw(w, r)
	if !ok {
		return
	}
	hash, replayed := a.idempotencyLookup(w, r, raw)
	if replayed {
		return
	}
	var payload resourcePayload
	if !strictRaw(raw, &payload) || !validResource(payload) {
		writeError(w, r, 400, "invalid_request", "resource validation failed")
		return
	}
	doc, err := a.store.GetDocument(r.Context(), kind, r.PathValue("id"))
	if err != nil {
		a.fail(w, r, err)
		return
	}
	if doc.OwnerID != claims(r).Subject && !authRole(r, "moderator") {
		a.fail(w, r, domain.ErrForbidden)
		return
	}
	expected, err := parseRevision(r)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	doc.Body = marshal(payload)
	doc.Events = []domain.Event{{EventKey: domain.NewID("evt"), UserID: claims(r).Subject, Type: kind + ".updated", AggregateType: kind, AggregateID: doc.ID, Revision: expected + 1, CorrelationID: requestID(r), Payload: marshal(payload)}}
	updated, err := a.store.UpdateDocument(r.Context(), doc, expected)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	response, _ := resourceFromDocument(updated)
	_ = a.store.AppendAudit(r.Context(), claims(r).Subject, kind+".update", kind, doc.ID, raw)
	body, err := a.idempotencySave(r, hash, 200, response)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	writeRawJSON(w, 200, body)
}
func (a *API) deleteResource(w http.ResponseWriter, r *http.Request) {
	kind := resourceKind(r)
	if kind == "match-results" || kind == "reports" {
		writeError(w, r, 409, "immutable", "this ledger resource is immutable")
		return
	}
	hash, replayed := a.idempotencyLookup(w, r, []byte(`{}`))
	if replayed {
		return
	}
	doc, err := a.store.GetDocument(r.Context(), kind, r.PathValue("id"))
	if err != nil {
		a.fail(w, r, err)
		return
	}
	if doc.OwnerID != claims(r).Subject && !authRole(r, "moderator") {
		a.fail(w, r, domain.ErrForbidden)
		return
	}
	expected, err := parseRevision(r)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	if err := a.store.DeleteDocument(r.Context(), kind, doc.ID, expected); err != nil {
		a.fail(w, r, err)
		return
	}
	response := map[string]any{"id": doc.ID, "deleted": true}
	body, err := a.idempotencySave(r, hash, 200, response)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	writeRawJSON(w, 200, body)
}
func authRole(r *http.Request, role string) bool {
	for _, candidate := range claims(r).Roles {
		if candidate == role {
			return true
		}
	}
	return false
}

func (a *API) createTicket(w http.ResponseWriter, r *http.Request) {
	raw, ok := readRaw(w, r)
	if !ok {
		return
	}
	hash, replayed := a.idempotencyLookup(w, r, raw)
	if replayed {
		return
	}
	var ticket domain.MatchmakingTicket
	if !strictRaw(raw, &ticket) || ticket.ProcedureIndex < 0 || ticket.PartySize < 1 || ticket.PartySize > a.config.MaxSessionMembers || !validEncodedProperties(ticket.Properties) {
		writeError(w, r, 400, "invalid_request", "ticket validation failed")
		return
	}
	now := time.Now().UTC().Format(time.RFC3339Nano)
	ticket.ID = domain.NewID("ticket")
	ticket.OwnerID = claims(r).Subject
	ticket.State = "searching"
	ticket.CreatedAt = now
	ticket.UpdatedAt = now
	doc, err := a.store.CreateDocument(r.Context(), domain.Document{Kind: "ticket", ID: ticket.ID, OwnerID: ticket.OwnerID, Body: marshal(ticket), Events: []domain.Event{{EventKey: domain.NewID("evt"), UserID: ticket.OwnerID, Type: "matchmaking.searching", AggregateType: "ticket", AggregateID: ticket.ID, Revision: 1, CorrelationID: requestID(r), Payload: marshal(ticket)}}})
	if err != nil {
		a.fail(w, r, err)
		return
	}
	ticket.UpdatedAt = doc.UpdatedAt.Format(time.RFC3339Nano)
	body, err := a.idempotencySave(r, hash, 201, ticket)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	writeRawJSON(w, 201, body)
}
func (a *API) getTicket(w http.ResponseWriter, r *http.Request) {
	doc, err := a.store.GetDocument(r.Context(), "ticket", r.PathValue("id"))
	if err != nil {
		a.fail(w, r, err)
		return
	}
	if doc.OwnerID != claims(r).Subject {
		a.fail(w, r, domain.ErrForbidden)
		return
	}
	ticket, err := documentBody[domain.MatchmakingTicket](doc)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	writeJSON(w, 200, ticket)
}
func (a *API) deleteTicket(w http.ResponseWriter, r *http.Request) {
	hash, replayed := a.idempotencyLookup(w, r, []byte(`{}`))
	if replayed {
		return
	}
	doc, err := a.store.GetDocument(r.Context(), "ticket", r.PathValue("id"))
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
	if err := a.store.DeleteDocument(r.Context(), "ticket", doc.ID, expected); err != nil {
		a.fail(w, r, err)
		return
	}
	response := map[string]any{"id": doc.ID, "deleted": true}
	body, err := a.idempotencySave(r, hash, 200, response)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	writeRawJSON(w, 200, body)
}

type relayRouteRequest struct {
	SessionID string `json:"session_id"`
	LocalPort int    `json:"local_port"`
}

func (a *API) relayRoute(w http.ResponseWriter, r *http.Request) {
	var request relayRouteRequest
	if !decode(w, r, &request) {
		return
	}
	doc, err := a.store.GetDocument(r.Context(), "session", request.SessionID)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	session, err := documentBody[domain.SessionRecord](doc)
	if err != nil || !session.ContainsAccount(claims(r).Subject) {
		a.fail(w, r, domain.ErrForbidden)
		return
	}
	if r.Method == http.MethodPost {
		route, err := a.relay.Register(r.Context(), claims(r).Subject, session.ID, request.LocalPort, 2*time.Minute)
		if err != nil {
			a.fail(w, r, err)
			return
		}
		writeJSON(w, 200, route)
		return
	}
	route, err := a.relay.Source(r.Context(), claims(r).Subject, request.LocalPort)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	if route.SessionID != session.ID {
		a.fail(w, r, domain.ErrForbidden)
		return
	}
	if err := a.relay.Unregister(r.Context(), route); err != nil {
		a.fail(w, r, err)
		return
	}
	writeJSON(w, 200, map[string]bool{"unregistered": true})
}
func (a *API) relayDatagrams(w http.ResponseWriter, r *http.Request) {
	if r.Method == http.MethodPost {
		raw, ok := readRaw(w, r)
		if !ok {
			return
		}
		var envelope struct {
			Datagrams []relay.Datagram `json:"datagrams"`
		}
		datagrams := []relay.Datagram{}
		if strictRaw(raw, &envelope) && len(envelope.Datagrams) > 0 {
			datagrams = envelope.Datagrams
		} else {
			var single relay.Datagram
			if !strictRaw(raw, &single) {
				writeError(w, r, 400, "invalid_request", "body must be one datagram or a datagrams batch")
				return
			}
			datagrams = append(datagrams, single)
		}
		if len(datagrams) > 64 {
			writeError(w, r, 400, "invalid_request", "datagram batch exceeds 64 items")
			return
		}
		sources := map[int]relay.Route{}
		decodedBytes := 0
		encodedBytes := 0
		for _, datagram := range datagrams {
			payload, err := base64.StdEncoding.DecodeString(datagram.Payload)
			decodedBytes += len(payload)
			encodedBytes += len(datagram.Payload)
			if err != nil || len(payload) > 65507 || decodedBytes > 1<<20 || encodedBytes > 1011712 || datagram.SourcePort < 1 || datagram.SourcePort > 65535 || datagram.DestinationPort < 1 || datagram.DestinationPort > 65535 || net.ParseIP(datagram.DestinationIPv4).To4() == nil {
				writeError(w, r, 400, "invalid_request", "datagram payload, address, ports, or batch size are invalid")
				return
			}
			if _, found := sources[datagram.SourcePort]; !found {
				source, err := a.relay.Source(r.Context(), claims(r).Subject, datagram.SourcePort)
				if err != nil {
					a.fail(w, r, err)
					return
				}
				if err := a.authorizeRelayRoute(r, source); err != nil {
					a.fail(w, r, err)
					return
				}
				sources[datagram.SourcePort] = source
			}
		}
		if err := a.relay.SendBatch(r.Context(), sources, datagrams); err != nil {
			a.fail(w, r, err)
			return
		}
		writeJSON(w, 202, map[string]any{"accepted": true, "accepted_count": len(datagrams)})
		return
	}
	port, _ := strconv.Atoi(r.URL.Query().Get("local_port"))
	maxBytes, _ := strconv.Atoi(r.URL.Query().Get("max_bytes"))
	waitMS, _ := strconv.Atoi(r.URL.Query().Get("wait_ms"))
	if port < 1 || port > 65535 || maxBytes < 1 || maxBytes > 1048576 || waitMS < 0 || waitMS > 30000 {
		writeError(w, r, 400, "invalid_request", "query limits are invalid")
		return
	}
	source, err := a.relay.Source(r.Context(), claims(r).Subject, port)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	if err := a.authorizeRelayRoute(r, source); err != nil {
		a.fail(w, r, err)
		return
	}
	datagrams, err := a.relay.Receive(r.Context(), claims(r).Subject, port, maxBytes, waitMS)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	writeJSON(w, 200, map[string]any{"datagrams": datagrams})
}

func (a *API) authorizeRelayRoute(r *http.Request, route relay.Route) error {
	doc, err := a.store.GetDocument(r.Context(), "session", route.SessionID)
	if err != nil {
		return err
	}
	session, err := documentBody[domain.SessionRecord](doc)
	if err != nil {
		return err
	}
	if !session.ContainsAccount(claims(r).Subject) {
		return domain.ErrForbidden
	}
	return nil
}

func init() { _ = json.Valid }
