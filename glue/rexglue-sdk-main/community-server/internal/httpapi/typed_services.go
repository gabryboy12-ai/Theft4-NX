package httpapi

import (
	"encoding/json"
	"math/big"
	"net/http"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"

	"libertyrecomp/community-server/internal/domain"
)

type friendLink struct {
	XUID       string `json:"xuid"`
	AccountID  string `json:"account_id"`
	PlayerName string `json:"player_name"`
	State      string `json:"state"`
	Presence   string `json:"presence"`
	UpdatedAt  string `json:"updated_at"`
}

func friendDocID(owner, target string) string { return "friend_" + owner + "_" + target }
func (a *API) friendLinks(r *http.Request) ([]friendLink, error) {
	docs, err := a.store.ListDocuments(r.Context(), "friend-link", claims(r).Subject, 1000)
	if err != nil {
		return nil, err
	}
	items := []friendLink{}
	for _, doc := range docs {
		value, err := documentBody[friendLink](doc)
		if err == nil {
			items = append(items, value)
		}
	}
	sort.Slice(items, func(i, j int) bool { return items[i].XUID < items[j].XUID })
	return items, nil
}
func (a *API) friendsPage(w http.ResponseWriter, r *http.Request) {
	items, err := a.friendLinks(r)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	after := r.URL.Query().Get("after")
	limit := pageLimit(r, 100)
	page := []friendLink{}
	for _, item := range items {
		if item.XUID <= after {
			continue
		}
		page = append(page, item)
		if len(page) == limit {
			break
		}
	}
	next := ""
	if len(page) == limit {
		next = page[len(page)-1].XUID
	}
	writeJSON(w, 200, map[string]any{"items": page, "next_cursor": next})
}
func (a *API) friendsCheck(w http.ResponseWriter, r *http.Request) {
	var request struct {
		XUIDs []string `json:"xuids"`
	}
	if !decode(w, r, &request) {
		return
	}
	if len(request.XUIDs) > 100 {
		a.fail(w, r, domain.ErrInvalid)
		return
	}
	links, err := a.friendLinks(r)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	states := map[string]string{}
	for _, link := range links {
		states[link.XUID] = link.State
	}
	results := []map[string]any{}
	for _, xuid := range request.XUIDs {
		state := states[xuid]
		results = append(results, map[string]any{"xuid": xuid, "is_friend": state == "accepted", "is_blocked": state == "blocked"})
	}
	writeJSON(w, 200, map[string]any{"results": results})
}
func (a *API) friendRelationship(w http.ResponseWriter, r *http.Request) {
	raw, ok := readRaw(w, r)
	if !ok {
		return
	}
	hash, replayed := a.idempotencyLookup(w, r, raw)
	if replayed {
		return
	}
	var request struct {
		TargetXUID string `json:"target_xuid"`
		Action     string `json:"action"`
	}
	if !strictRaw(raw, &request) {
		a.fail(w, r, domain.ErrInvalid)
		return
	}
	target, err := a.store.GetDeviceByXUID(r.Context(), request.TargetXUID)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	actor, err := a.store.GetDevice(r.Context(), claims(r).DeviceID)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	states := map[string][2]string{"request": {"outgoing", "incoming"}, "accept": {"accepted", "accepted"}, "remove": {"removed", "removed"}, "block": {"blocked", "blocked_by"}, "unblock": {"removed", "removed"}}
	pair, found := states[request.Action]
	if !found || target.AccountID == actor.AccountID {
		a.fail(w, r, domain.ErrInvalid)
		return
	}
	if request.Action == "accept" {
		current, err := a.store.GetDocument(r.Context(), "friend-link", friendDocID(actor.AccountID, target.AccountID))
		if err != nil {
			a.fail(w, r, domain.ErrForbidden)
			return
		}
		link, _ := documentBody[friendLink](current)
		if link.State != "incoming" {
			a.fail(w, r, domain.ErrForbidden)
			return
		}
	}
	now := time.Now().UTC().Format(time.RFC3339Nano)
	actorLink := friendLink{XUID: target.XUID, AccountID: target.AccountID, PlayerName: target.DisplayName, State: pair[0], UpdatedAt: now}
	targetLink := friendLink{XUID: actor.XUID, AccountID: actor.AccountID, PlayerName: actor.DisplayName, State: pair[1], UpdatedAt: now}
	documents := []domain.Document{
		{Kind: "friend-link", ID: friendDocID(actor.AccountID, target.AccountID), OwnerID: actor.AccountID, Body: marshal(actorLink)},
		{Kind: "friend-link", ID: friendDocID(target.AccountID, actor.AccountID), OwnerID: target.AccountID, Body: marshal(targetLink)},
	}
	for index := range documents {
		if current, getErr := a.store.GetDocument(r.Context(), documents[index].Kind, documents[index].ID); getErr == nil {
			documents[index].Version = current.Version
		} else if getErr != domain.ErrNotFound {
			a.fail(w, r, getErr)
			return
		}
		link := actorLink
		if index == 1 {
			link = targetLink
		}
		documents[index].Events = []domain.Event{{EventKey: domain.NewID("evt"), UserID: documents[index].OwnerID, Type: "friends.relationship_changed", AggregateType: "friend-link", AggregateID: documents[index].ID, Revision: documents[index].Version + 1, CorrelationID: requestID(r), Payload: marshal(link)}}
	}
	if _, err := a.store.PutDocumentsAtomic(r.Context(), documents); err != nil {
		a.fail(w, r, err)
		return
	}
	body, err := a.idempotencySave(r, hash, 200, actorLink)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	writeRawJSON(w, 200, body)
}
func (a *API) upsertOwned(r *http.Request, kind, id, owner string, value any) error {
	doc, err := a.store.GetDocument(r.Context(), kind, id)
	if err == nil {
		doc.Body = marshal(value)
		_, err = a.store.UpdateDocument(r.Context(), doc, doc.Version)
		return err
	}
	if err != domain.ErrNotFound {
		return err
	}
	_, err = a.store.CreateDocument(r.Context(), domain.Document{Kind: kind, ID: id, OwnerID: owner, Body: marshal(value)})
	return err
}

type inviteRecord struct {
	ID            string `json:"id"`
	Revision      int64  `json:"revision"`
	SenderXUID    string `json:"sender_xuid"`
	RecipientXUID string `json:"recipient_xuid"`
	SessionID     string `json:"session_id"`
	CustomData    string `json:"custom_data"`
	State         string `json:"state"`
	ExpiresAt     string `json:"expires_at"`
	CreatedAt     string `json:"created_at"`
}

func (a *API) createInvites(w http.ResponseWriter, r *http.Request) {
	raw, ok := readRaw(w, r)
	if !ok {
		return
	}
	hash, replayed := a.idempotencyLookup(w, r, raw)
	if replayed {
		return
	}
	var request struct {
		SessionID        string   `json:"session_id"`
		RecipientXUIDs   []string `json:"recipient_xuids"`
		CustomData       string   `json:"custom_data"`
		ExpiresInSeconds int      `json:"expires_in_seconds"`
	}
	if !strictRaw(raw, &request) || len(request.RecipientXUIDs) < 1 || len(request.RecipientXUIDs) > 32 || request.ExpiresInSeconds < 60 || request.ExpiresInSeconds > 604800 {
		a.fail(w, r, domain.ErrInvalid)
		return
	}
	if _, err := decodeBase64(request.CustomData, 512); err != nil {
		a.fail(w, r, err)
		return
	}
	sessionDoc, err := a.store.GetDocument(r.Context(), "session", request.SessionID)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	session, _ := documentBody[domain.SessionRecord](sessionDoc)
	if !session.ContainsAccount(claims(r).Subject) {
		a.fail(w, r, domain.ErrForbidden)
		return
	}
	sender, _ := a.store.GetDevice(r.Context(), claims(r).DeviceID)
	now := time.Now().UTC()
	items := []inviteRecord{}
	documents := []domain.Document{}
	seen := map[string]bool{}
	for _, xuid := range request.RecipientXUIDs {
		if seen[xuid] || xuid == sender.XUID {
			a.fail(w, r, domain.ErrInvalid)
			return
		}
		seen[xuid] = true
		target, err := a.store.GetDeviceByXUID(r.Context(), xuid)
		if err != nil {
			a.fail(w, r, err)
			return
		}
		item := inviteRecord{ID: domain.NewID("invite"), Revision: 1, SenderXUID: sender.XUID, RecipientXUID: xuid, SessionID: session.ID, CustomData: request.CustomData, State: "pending", ExpiresAt: now.Add(time.Duration(request.ExpiresInSeconds) * time.Second).Format(time.RFC3339Nano), CreatedAt: now.Format(time.RFC3339Nano)}
		documents = append(documents, domain.Document{Kind: "invite-record", ID: item.ID, OwnerID: target.AccountID, Body: marshal(item), Events: []domain.Event{{EventKey: domain.NewID("evt"), UserID: target.AccountID, Type: "invite.created", AggregateType: "invite", AggregateID: item.ID, Revision: 1, CorrelationID: requestID(r), Payload: marshal(item)}}})
		items = append(items, item)
	}
	if _, err := a.store.PutDocumentsAtomic(r.Context(), documents); err != nil {
		a.fail(w, r, err)
		return
	}
	response := map[string]any{"invites": items}
	body, err := a.idempotencySave(r, hash, 201, response)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	writeRawJSON(w, 201, body)
}
func (a *API) invitesPage(w http.ResponseWriter, r *http.Request) {
	docs, err := a.store.ListDocuments(r.Context(), "invite-record", claims(r).Subject, 1000)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	after := r.URL.Query().Get("after")
	state := r.URL.Query().Get("state")
	limit := pageLimit(r, 100)
	items := []inviteRecord{}
	for _, doc := range docs {
		item, err := documentBody[inviteRecord](doc)
		if err == nil && (state == "" || item.State == state) && item.ID > after {
			item.Revision = doc.Version
			items = append(items, item)
		}
	}
	sort.Slice(items, func(i, j int) bool { return items[i].ID < items[j].ID })
	if len(items) > limit {
		items = items[:limit]
	}
	next := ""
	if len(items) == limit {
		next = items[len(items)-1].ID
	}
	writeJSON(w, 200, map[string]any{"items": items, "next_cursor": next})
}
func (a *API) acceptInvite(w http.ResponseWriter, r *http.Request) {
	raw, ok := readRaw(w, r)
	if !ok {
		return
	}
	hash, replayed := a.idempotencyLookup(w, r, raw)
	if replayed {
		return
	}
	var request struct {
		ExpectedRevision int64 `json:"expected_revision"`
	}
	if !strictRaw(raw, &request) || request.ExpectedRevision < 1 {
		a.fail(w, r, domain.ErrInvalid)
		return
	}
	doc, err := a.store.GetDocument(r.Context(), "invite-record", r.PathValue("id"))
	if err != nil {
		a.fail(w, r, err)
		return
	}
	if doc.OwnerID != claims(r).Subject {
		a.fail(w, r, domain.ErrForbidden)
		return
	}
	item, err := documentBody[inviteRecord](doc)
	if err != nil || item.State != "pending" {
		a.fail(w, r, domain.ErrConflict)
		return
	}
	expires, _ := time.Parse(time.RFC3339Nano, item.ExpiresAt)
	if !time.Now().Before(expires) {
		a.fail(w, r, domain.ErrConflict)
		return
	}
	item.State = "accepted"
	item.Revision = request.ExpectedRevision + 1
	doc.Body = marshal(item)
	doc.Events = []domain.Event{{EventKey: domain.NewID("evt"), UserID: claims(r).Subject, Type: "invite.accepted", AggregateType: "invite", AggregateID: item.ID, Revision: item.Revision, CorrelationID: requestID(r), Payload: marshal(item)}}
	updated, err := a.store.UpdateDocument(r.Context(), doc, request.ExpectedRevision)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	item.Revision = updated.Version
	session, _, err := a.loadSessionByID(r, item.SessionID)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	response := map[string]any{"invite": item, "session": session}
	body, err := a.idempotencySave(r, hash, 200, response)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	writeRawJSON(w, 200, body)
}
func (a *API) loadSessionByID(r *http.Request, id string) (domain.SessionRecord, domain.Document, error) {
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

type statValue struct {
	Type  string          `json:"type"`
	Value json.RawMessage `json:"value"`
}
type statRow struct {
	XUID      string               `json:"xuid"`
	ViewID    string               `json:"view_id"`
	Columns   map[string]statValue `json:"columns"`
	UpdatedAt string               `json:"updated_at"`
}
type statWriteRequest struct {
	SessionID string `json:"session_id"`
	Sequence  string `json:"sequence"`
	Views     []struct {
		ViewID string `json:"view_id"`
		Rows   []struct {
			XUID    string               `json:"xuid"`
			Columns map[string]statValue `json:"columns"`
		} `json:"rows"`
	} `json:"views"`
}

var (
	hex32Pattern = regexp.MustCompile(`^0x[0-9a-f]{8}$`)
	hex64Pattern = regexp.MustCompile(`^0x[0-9a-f]{16}$`)
)

func (a *API) statsWrite(w http.ResponseWriter, r *http.Request) {
	raw, ok := readRaw(w, r)
	if !ok {
		return
	}
	hash, replayed := a.idempotencyLookup(w, r, raw)
	if replayed {
		return
	}
	var request statWriteRequest
	if !strictRaw(raw, &request) || len(request.Views) == 0 || len(request.Views) > 16 {
		a.fail(w, r, domain.ErrInvalid)
		return
	}
	if _, err := strconv.ParseUint(request.Sequence, 10, 64); err != nil {
		a.fail(w, r, domain.ErrInvalid)
		return
	}
	session, _, err := a.loadSessionByID(r, request.SessionID)
	if err != nil || !session.ContainsAccount(claims(r).Subject) {
		a.fail(w, r, domain.ErrForbidden)
		return
	}
	actor, err := a.store.GetDevice(r.Context(), claims(r).DeviceID)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	type pendingRow struct {
		viewID  string
		columns map[string]statValue
	}
	pending := map[string]pendingRow{}
	now := time.Now().UTC().Format(time.RFC3339Nano)
	for _, view := range request.Views {
		if !hex32Pattern.MatchString(view.ViewID) || len(view.Rows) == 0 || len(view.Rows) > 64 {
			a.fail(w, r, domain.ErrInvalid)
			return
		}
		for _, input := range view.Rows {
			if input.XUID != actor.XUID || len(input.Columns) == 0 || len(input.Columns) > 64 || !validStatColumns(input.Columns) {
				a.fail(w, r, domain.ErrInvalid)
				return
			}
			if _, found := session.MemberByXUID(actor.XUID); !found {
				a.fail(w, r, domain.ErrForbidden)
				return
			}
			id := input.XUID + ":" + strings.ToLower(view.ViewID)
			if _, duplicate := pending[id]; duplicate {
				a.fail(w, r, domain.ErrInvalid)
				return
			}
			pending[id] = pendingRow{viewID: strings.ToLower(view.ViewID), columns: input.Columns}
		}
	}
	receiptID := domain.NewID("stats")
	documents := make([]domain.Document, 0, len(pending))
	for id, input := range pending {
		columns := map[string]statValue{}
		version := int64(0)
		if current, getErr := a.store.GetDocument(r.Context(), "stat-row", id); getErr == nil {
			row, decodeErr := documentBody[statRow](current)
			if decodeErr != nil || current.OwnerID != actor.AccountID {
				a.fail(w, r, domain.ErrConflict)
				return
			}
			for key, value := range row.Columns {
				columns[key] = value
			}
			version = current.Version
		} else if getErr != domain.ErrNotFound {
			a.fail(w, r, getErr)
			return
		}
		for key, value := range input.columns {
			columns[strings.ToLower(key)] = value
		}
		row := statRow{XUID: actor.XUID, ViewID: input.viewID, Columns: columns, UpdatedAt: now}
		event := domain.Event{EventKey: domain.NewID("evt"), UserID: actor.AccountID, Type: "stats.written", AggregateType: "stat-row", AggregateID: id, Revision: version + 1, CorrelationID: requestID(r), Payload: marshal(map[string]any{"receipt_id": receiptID, "sequence": request.Sequence, "view_id": input.viewID})}
		documents = append(documents, domain.Document{Kind: "stat-row", ID: id, OwnerID: actor.AccountID, Body: marshal(row), Version: version, Events: []domain.Event{event}})
	}
	if _, err := a.store.PutDocumentsAtomic(r.Context(), documents); err != nil {
		a.fail(w, r, err)
		return
	}
	response := map[string]any{"receipt_id": receiptID, "sequence": request.Sequence, "rows_written": len(documents)}
	body, err := a.idempotencySave(r, hash, 200, response)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	writeRawJSON(w, 200, body)
}
func validStatColumns(columns map[string]statValue) bool {
	allowed := map[string]bool{"i32": true, "i64": true, "f64": true, "unicode": true, "binary": true}
	for id, value := range columns {
		if !hex32Pattern.MatchString(id) || !allowed[value.Type] || len(value.Value) > 2048 {
			return false
		}
		switch value.Type {
		case "i32":
			if _, err := strconv.ParseInt(string(value.Value), 10, 32); err != nil {
				return false
			}
		case "i64":
			if _, err := strconv.ParseInt(string(value.Value), 10, 64); err != nil {
				return false
			}
		case "f64":
			if number, err := strconv.ParseFloat(string(value.Value), 64); err != nil || strings.ContainsAny(string(value.Value), "NnIi") || number != number {
				return false
			}
		case "unicode":
			var text string
			if json.Unmarshal(value.Value, &text) != nil || len([]byte(text)) > 1024 {
				return false
			}
		case "binary":
			var text string
			if json.Unmarshal(value.Value, &text) != nil {
				return false
			}
			if _, err := decodeBase64(text, 512); err != nil {
				return false
			}
		}
	}
	return true
}
func (a *API) statsRead(w http.ResponseWriter, r *http.Request) {
	var request struct {
		XUIDs   []string `json:"xuids"`
		ViewID  string   `json:"view_id"`
		StatIDs []string `json:"stat_ids"`
	}
	if !decode(w, r, &request) {
		return
	}
	if len(request.XUIDs) > 100 || len(request.StatIDs) > 64 || !hex32Pattern.MatchString(request.ViewID) {
		a.fail(w, r, domain.ErrInvalid)
		return
	}
	for _, statID := range request.StatIDs {
		if !hex32Pattern.MatchString(statID) {
			a.fail(w, r, domain.ErrInvalid)
			return
		}
	}
	request.ViewID = strings.ToLower(request.ViewID)
	rows := []statRow{}
	for _, xuid := range request.XUIDs {
		doc, err := a.store.GetDocument(r.Context(), "stat-row", xuid+":"+request.ViewID)
		if err != nil {
			rows = append(rows, statRow{XUID: xuid, ViewID: request.ViewID, Columns: map[string]statValue{}})
			continue
		}
		row, _ := documentBody[statRow](doc)
		filtered := map[string]statValue{}
		for _, id := range request.StatIDs {
			if value, found := row.Columns[id]; found {
				filtered[id] = value
			}
		}
		row.Columns = filtered
		rows = append(rows, row)
	}
	writeJSON(w, 200, map[string]any{"rows": rows})
}

func (a *API) statsReset(w http.ResponseWriter, r *http.Request) {
	raw, ok := readRaw(w, r)
	if !ok {
		return
	}
	hash, replayed := a.idempotencyLookup(w, r, raw)
	if replayed {
		return
	}
	var request struct {
		ViewID string `json:"view_id"`
	}
	if !strictRaw(raw, &request) || !hex32Pattern.MatchString(request.ViewID) {
		a.fail(w, r, domain.ErrInvalid)
		return
	}
	actor, err := a.store.GetDevice(r.Context(), claims(r).DeviceID)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	viewID := strings.ToLower(request.ViewID)
	id := actor.XUID + ":" + viewID
	doc, err := a.store.GetDocument(r.Context(), "stat-row", id)
	if err == nil {
		row := statRow{XUID: actor.XUID, ViewID: viewID, Columns: map[string]statValue{}, UpdatedAt: time.Now().UTC().Format(time.RFC3339Nano)}
		doc.Body = marshal(row)
		doc.Events = []domain.Event{{EventKey: domain.NewID("evt"), UserID: actor.AccountID, Type: "stats.reset", AggregateType: "stat-row", AggregateID: id, Revision: doc.Version + 1, CorrelationID: requestID(r), Payload: marshal(map[string]string{"view_id": viewID})}}
		if _, err = a.store.UpdateDocument(r.Context(), doc, doc.Version); err != nil {
			a.fail(w, r, err)
			return
		}
	} else if err != domain.ErrNotFound {
		a.fail(w, r, err)
		return
	}
	response := map[string]any{"view_id": viewID, "reset": true}
	body, err := a.idempotencySave(r, hash, 200, response)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	writeRawJSON(w, 200, body)
}

func (a *API) leaderboard(w http.ResponseWriter, r *http.Request) {
	view := r.PathValue("view_id")
	statID := r.URL.Query().Get("stat_id")
	offset, _ := strconv.Atoi(r.URL.Query().Get("offset"))
	limit := pageLimit(r, 100)
	if offset < 0 || !hex32Pattern.MatchString(view) || !hex32Pattern.MatchString(statID) {
		a.fail(w, r, domain.ErrInvalid)
		return
	}
	view = strings.ToLower(view)
	statID = strings.ToLower(statID)
	docs, err := a.store.ListDocuments(r.Context(), "stat-row", "", 10000)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	rows := []statRow{}
	friendSet := map[string]bool{}
	friendsOnly := r.URL.Query().Get("friends_only") == "true"
	if friendsOnly {
		links, _ := a.friendLinks(r)
		for _, link := range links {
			if link.State == "accepted" {
				friendSet[link.XUID] = true
			}
		}
	}
	for _, doc := range docs {
		row, err := documentBody[statRow](doc)
		if err == nil && row.ViewID == view && (!friendsOnly || friendSet[row.XUID]) {
			rows = append(rows, row)
		}
	}
	sort.SliceStable(rows, func(i, j int) bool {
		comparison := compareStatValues(rows[i].Columns[statID], rows[j].Columns[statID])
		if comparison == 0 {
			return rows[i].XUID < rows[j].XUID
		}
		return comparison > 0
	})
	total := len(rows)
	if offset > total {
		offset = total
	}
	end := offset + limit
	if end > total {
		end = total
	}
	result := []map[string]any{}
	for index, row := range rows[offset:end] {
		device, _ := a.store.GetDeviceByXUID(r.Context(), row.XUID)
		result = append(result, map[string]any{"rank": offset + index + 1, "xuid": row.XUID, "player_name": device.DisplayName, "value": row.Columns[statID]})
	}
	writeJSON(w, 200, map[string]any{"total": total, "rows": result})
}
func statNumber(value statValue) *big.Rat {
	switch value.Type {
	case "i32", "i64":
		number := new(big.Int)
		if _, ok := number.SetString(string(value.Value), 10); ok {
			return new(big.Rat).SetInt(number)
		}
	case "f64":
		if number, err := strconv.ParseFloat(string(value.Value), 64); err == nil {
			return new(big.Rat).SetFloat64(number)
		}
	}
	return new(big.Rat)
}
func compareStatValues(left, right statValue) int {
	return statNumber(left).Cmp(statNumber(right))
}
func (a *API) statsSkill(w http.ResponseWriter, r *http.Request) {
	var request struct {
		XUIDs []string `json:"xuids"`
	}
	if !decode(w, r, &request) {
		return
	}
	if len(request.XUIDs) == 0 || len(request.XUIDs) > 100 {
		a.fail(w, r, domain.ErrInvalid)
		return
	}
	rows := []map[string]any{}
	for _, xuid := range request.XUIDs {
		if !hex64Pattern.MatchString(xuid) {
			a.fail(w, r, domain.ErrInvalid)
			return
		}
		rows = append(rows, map[string]any{"xuid": xuid, "rating": 0})
	}
	writeJSON(w, 200, map[string]any{"ratings": rows})
}

type voiceRouteRecord struct {
	Token       string   `json:"route_token"`
	SessionID   string   `json:"session_id"`
	OwnerID     string   `json:"owner_id"`
	OwnerXUID   string   `json:"owner_xuid"`
	Channel     string   `json:"channel"`
	TargetXUIDs []string `json:"target_xuids"`
	MuteXUIDs   []string `json:"mute_xuids"`
	ExpiresAt   string   `json:"expires_at"`
	Revoked     bool     `json:"revoked"`
}

type activeVoiceRoute struct {
	RouteToken string `json:"route_token"`
	SessionID  string `json:"session_id"`
	OwnerID    string `json:"owner_id"`
}

func (a *API) voiceRoute(w http.ResponseWriter, r *http.Request) {
	var request struct {
		SessionID   string   `json:"session_id"`
		Channel     string   `json:"channel"`
		TargetXUIDs []string `json:"target_xuids"`
		MuteXUIDs   []string `json:"mute_xuids"`
	}
	if !decode(w, r, &request) {
		return
	}
	allowed := map[string]bool{"all": true, "team": true, "private": true}
	session, _, err := a.loadSessionByID(r, request.SessionID)
	if err != nil || !session.ContainsAccount(claims(r).Subject) || !allowed[request.Channel] || len(request.TargetXUIDs) > 64 || len(request.MuteXUIDs) > 64 {
		a.fail(w, r, domain.ErrForbidden)
		return
	}
	if (request.Channel == "private" || request.Channel == "team") && len(request.TargetXUIDs) == 0 {
		a.fail(w, r, domain.ErrInvalid)
		return
	}
	owner, err := a.store.GetDevice(r.Context(), claims(r).DeviceID)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	targets, ok := normalizedXUIDs(request.TargetXUIDs, session)
	if !ok {
		a.fail(w, r, domain.ErrInvalid)
		return
	}
	muted, ok := normalizedXUIDs(request.MuteXUIDs, session)
	if !ok {
		a.fail(w, r, domain.ErrInvalid)
		return
	}
	route := voiceRouteRecord{Token: domain.NewID("voice"), SessionID: session.ID, OwnerID: claims(r).Subject, OwnerXUID: owner.XUID, Channel: request.Channel, TargetXUIDs: targets, MuteXUIDs: muted, ExpiresAt: time.Now().UTC().Add(2 * time.Hour).Format(time.RFC3339Nano)}
	activeID := claims(r).Subject + ":" + session.ID
	activeDoc, activeErr := a.store.GetDocument(r.Context(), "voice-active", activeID)
	documents := []domain.Document{{Kind: "voice-route", ID: route.Token, OwnerID: claims(r).Subject, Body: marshal(route)}}
	if activeErr == nil {
		active, decodeErr := documentBody[activeVoiceRoute](activeDoc)
		if decodeErr != nil {
			a.fail(w, r, domain.ErrConflict)
			return
		}
		if active.RouteToken != "" {
			if priorDoc, getErr := a.store.GetDocument(r.Context(), "voice-route", active.RouteToken); getErr == nil {
				prior, decodeErr := documentBody[voiceRouteRecord](priorDoc)
				if decodeErr != nil || prior.OwnerID != claims(r).Subject {
					a.fail(w, r, domain.ErrConflict)
					return
				}
				prior.Revoked = true
				priorDoc.Body = marshal(prior)
				documents = append(documents, priorDoc)
			}
		}
	} else if activeErr != domain.ErrNotFound {
		a.fail(w, r, activeErr)
		return
	} else {
		activeDoc = domain.Document{Kind: "voice-active", ID: activeID, OwnerID: claims(r).Subject}
	}
	activeDoc.Body = marshal(activeVoiceRoute{RouteToken: route.Token, SessionID: session.ID, OwnerID: claims(r).Subject})
	documents = append(documents, activeDoc)
	if _, err = a.store.PutDocumentsAtomic(r.Context(), documents); err != nil {
		a.fail(w, r, err)
		return
	}
	writeJSON(w, 201, route)
}

func normalizedXUIDs(values []string, session domain.SessionRecord) ([]string, bool) {
	seen := map[string]bool{}
	result := make([]string, 0, len(values))
	for _, xuid := range values {
		if !hex64Pattern.MatchString(xuid) || seen[xuid] {
			return nil, false
		}
		if _, found := session.MemberByXUID(xuid); !found {
			return nil, false
		}
		seen[xuid] = true
		result = append(result, xuid)
	}
	sort.Strings(result)
	return result, true
}

func (a *API) chatMessage(w http.ResponseWriter, r *http.Request) {
	raw, ok := readRaw(w, r)
	if !ok {
		return
	}
	hash, replayed := a.idempotencyLookup(w, r, raw)
	if replayed {
		return
	}
	var request struct {
		SessionID   string   `json:"session_id"`
		Channel     string   `json:"channel"`
		TargetXUIDs []string `json:"target_xuids"`
		Sequence    uint32   `json:"sequence"`
		Text        string   `json:"text"`
	}
	if !strictRaw(raw, &request) || (request.Channel != "all" && request.Channel != "team") ||
		len(request.Text) == 0 || len(request.Text) > 256 || !utf8.ValidString(request.Text) ||
		strings.TrimSpace(request.Text) == "" || strings.ContainsAny(request.Text, "\r\n\x00") ||
		len(request.TargetXUIDs) > 64 ||
		(request.Channel == "all" && len(request.TargetXUIDs) != 0) ||
		(request.Channel == "team" && len(request.TargetXUIDs) == 0) {
		a.fail(w, r, domain.ErrInvalid)
		return
	}

	session, _, err := a.loadSessionByID(r, request.SessionID)
	if err != nil || !session.ContainsAccount(claims(r).Subject) {
		a.fail(w, r, domain.ErrForbidden)
		return
	}
	sender, err := a.store.GetDevice(r.Context(), claims(r).DeviceID)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	senderMember, found := session.MemberByXUID(sender.XUID)
	if !found || senderMember.AccountID != claims(r).Subject {
		a.fail(w, r, domain.ErrForbidden)
		return
	}

	targets := request.TargetXUIDs
	if request.Channel == "all" {
		targets = make([]string, 0, len(session.Members))
		for _, member := range session.Members {
			if member.XUID != sender.XUID {
				targets = append(targets, member.XUID)
			}
		}
	}
	targets, ok = normalizedXUIDs(targets, session)
	if !ok {
		a.fail(w, r, domain.ErrInvalid)
		return
	}

	payload := marshal(map[string]any{
		"source_xuid": sender.XUID,
		"session_id":  session.ID,
		"sequence":    request.Sequence,
		"channel":     request.Channel,
		"player_name": sender.DisplayName,
		"text":        request.Text,
	})
	events := make([]domain.Event, 0, len(targets))
	for _, xuid := range targets {
		if xuid == sender.XUID {
			continue
		}
		member, memberFound := session.MemberByXUID(xuid)
		if !memberFound || member.AccountID == "" {
			a.fail(w, r, domain.ErrInvalid)
			return
		}
		events = append(events, domain.Event{
			EventKey:      domain.NewID("evt"),
			UserID:        member.AccountID,
			Type:          "chat.message",
			AggregateType: "session-chat",
			AggregateID:   session.ID,
			Revision:      int64(request.Sequence),
			CorrelationID: requestID(r),
			Payload:       payload,
		})
	}
	if err := a.store.AppendEvents(r.Context(), events); err != nil {
		a.fail(w, r, err)
		return
	}
	response := map[string]any{"accepted": true, "recipient_count": len(events)}
	body, err := a.idempotencySave(r, hash, 201, response)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	writeRawJSON(w, 201, body)
}

func (a *API) voiceRouteDelete(w http.ResponseWriter, r *http.Request) {
	var request struct {
		RouteToken string `json:"route_token"`
	}
	if !decode(w, r, &request) {
		return
	}
	doc, err := a.store.GetDocument(r.Context(), "voice-route", request.RouteToken)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	route, err := documentBody[voiceRouteRecord](doc)
	if err != nil || route.OwnerID != claims(r).Subject {
		a.fail(w, r, domain.ErrForbidden)
		return
	}
	if route.Revoked {
		writeJSON(w, 200, map[string]bool{"revoked": true})
		return
	}
	route.Revoked = true
	doc.Body = marshal(route)
	documents := []domain.Document{doc}
	activeID := claims(r).Subject + ":" + route.SessionID
	if activeDoc, getErr := a.store.GetDocument(r.Context(), "voice-active", activeID); getErr == nil {
		active, decodeErr := documentBody[activeVoiceRoute](activeDoc)
		if decodeErr != nil {
			a.fail(w, r, domain.ErrConflict)
			return
		}
		if active.RouteToken == route.Token {
			active.RouteToken = ""
			activeDoc.Body = marshal(active)
			documents = append(documents, activeDoc)
		}
	}
	if _, err := a.store.PutDocumentsAtomic(r.Context(), documents); err != nil {
		a.fail(w, r, err)
		return
	}
	writeJSON(w, 200, map[string]bool{"revoked": true})
}

type voicePacketInput struct {
	RouteToken string `json:"route_token"`
	Sequence   uint32 `json:"sequence"`
	Payload    string `json:"payload"`
}

func (a *API) voicePacket(w http.ResponseWriter, r *http.Request) {
	raw, ok := readRaw(w, r)
	if !ok {
		return
	}
	if len(raw) > 195328 {
		a.fail(w, r, domain.ErrInvalid)
		return
	}
	var envelope struct {
		Packets []voicePacketInput `json:"packets"`
	}
	packets := []voicePacketInput{}
	if strictRaw(raw, &envelope) && len(envelope.Packets) > 0 {
		packets = envelope.Packets
	} else {
		var single voicePacketInput
		if !strictRaw(raw, &single) {
			a.fail(w, r, domain.ErrInvalid)
			return
		}
		packets = append(packets, single)
	}
	if len(packets) > 32 {
		a.fail(w, r, domain.ErrInvalid)
		return
	}
	sender, err := a.store.GetDevice(r.Context(), claims(r).DeviceID)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	routes := map[string]voiceRouteRecord{}
	recipients := map[string][]voiceRouteRecord{}
	events := []domain.Event{}
	for _, input := range packets {
		if _, err := decodeBase64(input.Payload, 4096); err != nil {
			a.fail(w, r, err)
			return
		}
		route, found := routes[input.RouteToken]
		if !found {
			route, err = a.ownedActiveVoiceRoute(r, input.RouteToken)
			if err != nil {
				a.fail(w, r, err)
				return
			}
			routes[input.RouteToken] = route
			recipients[input.RouteToken], err = a.voiceRecipients(r, route)
			if err != nil {
				a.fail(w, r, err)
				return
			}
		}
		packet := map[string]any{"source_xuid": sender.XUID, "session_id": route.SessionID, "sequence": input.Sequence, "payload": input.Payload}
		for _, recipient := range recipients[input.RouteToken] {
			events = append(events, domain.Event{EventKey: domain.NewID("evt"), UserID: recipient.OwnerID, Type: "voice.packet", AggregateType: "voice-route", AggregateID: recipient.Token, Revision: int64(input.Sequence), CorrelationID: requestID(r), Payload: marshal(packet)})
		}
	}
	if err := a.store.AppendEvents(r.Context(), events); err != nil {
		a.fail(w, r, err)
		return
	}
	writeJSON(w, 202, map[string]any{"accepted": true, "accepted_count": len(packets)})
}

func (a *API) ownedActiveVoiceRoute(r *http.Request, token string) (voiceRouteRecord, error) {
	doc, err := a.store.GetDocument(r.Context(), "voice-route", token)
	if err != nil {
		return voiceRouteRecord{}, err
	}
	route, err := documentBody[voiceRouteRecord](doc)
	if err != nil || route.OwnerID != claims(r).Subject || route.Revoked {
		return voiceRouteRecord{}, domain.ErrForbidden
	}
	expires, err := time.Parse(time.RFC3339Nano, route.ExpiresAt)
	if err != nil || !time.Now().UTC().Before(expires) {
		return voiceRouteRecord{}, domain.ErrForbidden
	}
	activeDoc, err := a.store.GetDocument(r.Context(), "voice-active", route.OwnerID+":"+route.SessionID)
	if err != nil {
		return voiceRouteRecord{}, domain.ErrForbidden
	}
	active, err := documentBody[activeVoiceRoute](activeDoc)
	if err != nil || active.RouteToken != route.Token {
		return voiceRouteRecord{}, domain.ErrForbidden
	}
	return route, nil
}

func (a *API) voiceRecipients(r *http.Request, sender voiceRouteRecord) ([]voiceRouteRecord, error) {
	session, _, err := a.loadSessionByID(r, sender.SessionID)
	if err != nil {
		return nil, err
	}
	if member, found := session.MemberByXUID(sender.OwnerXUID); !found || member.AccountID != sender.OwnerID {
		return nil, domain.ErrForbidden
	}
	targets := map[string]bool{}
	if sender.Channel == "all" {
		for _, member := range session.Members {
			targets[member.XUID] = true
		}
	} else {
		for _, xuid := range sender.TargetXUIDs {
			targets[xuid] = true
		}
	}
	muted := map[string]bool{}
	for _, xuid := range sender.MuteXUIDs {
		muted[xuid] = true
	}
	result := []voiceRouteRecord{}
	for _, member := range session.Members {
		if member.XUID == sender.OwnerXUID || !targets[member.XUID] || muted[member.XUID] {
			continue
		}
		activeDoc, getErr := a.store.GetDocument(r.Context(), "voice-active", member.AccountID+":"+session.ID)
		if getErr != nil {
			continue
		}
		active, decodeErr := documentBody[activeVoiceRoute](activeDoc)
		if decodeErr != nil || active.RouteToken == "" {
			continue
		}
		routeDoc, getErr := a.store.GetDocument(r.Context(), "voice-route", active.RouteToken)
		if getErr != nil {
			continue
		}
		receiver, decodeErr := documentBody[voiceRouteRecord](routeDoc)
		expires, expiryErr := time.Parse(time.RFC3339Nano, receiver.ExpiresAt)
		if decodeErr != nil || expiryErr != nil || receiver.Revoked || receiver.OwnerID != member.AccountID || receiver.SessionID != session.ID || !time.Now().UTC().Before(expires) {
			continue
		}
		receiverMutedSender := false
		for _, xuid := range receiver.MuteXUIDs {
			if xuid == sender.OwnerXUID {
				receiverMutedSender = true
				break
			}
		}
		if !receiverMutedSender {
			result = append(result, receiver)
		}
	}
	return result, nil
}
func (a *API) voicePackets(w http.ResponseWriter, r *http.Request) {
	token := r.URL.Query().Get("route_token")
	if _, err := a.ownedActiveVoiceRoute(r, token); err != nil {
		a.fail(w, r, err)
		return
	}
	afterText := r.URL.Query().Get("after")
	var after int64
	var parseErr error
	if afterText != "" {
		after, parseErr = strconv.ParseInt(afterText, 10, 64)
	}
	waitMS, _ := strconv.Atoi(r.URL.Query().Get("wait_ms"))
	if parseErr != nil || after < 0 || waitMS < 0 || waitMS > 30000 {
		a.fail(w, r, domain.ErrInvalid)
		return
	}
	deadline := time.Now().Add(time.Duration(waitMS) * time.Millisecond)
	for {
		events, err := a.store.ReplayEvents(r.Context(), claims(r).Subject, after, 256)
		if err != nil {
			a.fail(w, r, err)
			return
		}
		packets := []map[string]any{}
		responseBytes := 0
		for _, event := range events {
			if event.Type == "voice.packet" && event.AggregateID == token {
				if responseBytes+len(event.Payload) > 1<<20 {
					break
				}
				var packet map[string]any
				if json.Unmarshal(event.Payload, &packet) == nil {
					packets = append(packets, packet)
					responseBytes += len(event.Payload)
				}
			}
			if event.ID > after {
				after = event.ID
			}
		}
		if len(packets) > 0 || time.Now().After(deadline) {
			next := ""
			if after > 0 {
				next = strconv.FormatInt(after, 10)
			}
			writeJSON(w, 200, map[string]any{"packets": packets, "next_after": next})
			return
		}
		select {
		case <-r.Context().Done():
			return
		case <-time.After(100 * time.Millisecond):
		}
	}
}
func pageLimit(r *http.Request, max int) int {
	value, _ := strconv.Atoi(r.URL.Query().Get("limit"))
	if value < 1 {
		value = max
	}
	if value > max {
		value = max
	}
	return value
}
