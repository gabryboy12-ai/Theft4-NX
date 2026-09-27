package httpapi

import (
	"encoding/json"
	"math"
	"net/http"
	"sort"
	"strings"
	"time"

	"libertyrecomp/community-server/internal/domain"
)

type nextGameState struct {
	Mode       string            `json:"mode"`
	Episode    string            `json:"episode"`
	Ranked     bool              `json:"ranked"`
	Contexts   map[string]uint32 `json:"contexts"`
	Properties map[string]string `json:"properties"`
}

type lobbyState struct {
	SessionID  string              `json:"session_id"`
	Revision   int64               `json:"revision"`
	Ready      map[string]bool     `json:"ready"`
	Spectators map[string]bool     `json:"spectators"`
	KickVotes  map[string][]string `json:"kick_votes"`
	NextGame   *nextGameState      `json:"next_game,omitempty"`
}

func (a *API) loadLobby(r *http.Request, session domain.SessionRecord) (lobbyState, domain.Document, error) {
	doc, err := a.store.GetDocument(r.Context(), "session-lobby", session.ID)
	if err == domain.ErrNotFound {
		host, _ := session.MemberByXUID(session.HostXUID)
		return lobbyState{SessionID: session.ID, Ready: map[string]bool{}, Spectators: map[string]bool{}, KickVotes: map[string][]string{}}, domain.Document{Kind: "session-lobby", ID: session.ID, OwnerID: host.AccountID}, nil
	}
	if err != nil {
		return lobbyState{}, doc, err
	}
	state, err := documentBody[lobbyState](doc)
	if state.Ready == nil {
		state.Ready = map[string]bool{}
	}
	if state.Spectators == nil {
		state.Spectators = map[string]bool{}
	}
	if state.KickVotes == nil {
		state.KickVotes = map[string][]string{}
	}
	state.Revision = doc.Version
	return state, doc, err
}

func (a *API) saveLobby(r *http.Request, session domain.SessionRecord, state lobbyState, doc domain.Document, eventType string) (lobbyState, error) {
	state.Revision = doc.Version + 1
	doc.Body = marshal(state)
	doc.Events = lobbyEvents(r, session, state, eventType)
	var stored domain.Document
	var err error
	if doc.Version == 0 {
		stored, err = a.store.CreateDocument(r.Context(), doc)
	} else {
		stored, err = a.store.UpdateDocument(r.Context(), doc, doc.Version)
	}
	if err != nil {
		return state, err
	}
	state.Revision = stored.Version
	return state, nil
}

func lobbyEvents(r *http.Request, session domain.SessionRecord, state lobbyState, eventType string) []domain.Event {
	payload := marshal(state)
	events := make([]domain.Event, 0, len(session.Members))
	seen := map[string]bool{}
	for _, member := range session.Members {
		if seen[member.AccountID] {
			continue
		}
		seen[member.AccountID] = true
		events = append(events, domain.Event{EventKey: domain.NewID("evt"), UserID: member.AccountID, Type: eventType, AggregateType: "session-lobby", AggregateID: state.SessionID, Revision: state.Revision, CorrelationID: requestID(r), Payload: payload})
	}
	return events
}

func (a *API) lobbyStateGet(w http.ResponseWriter, r *http.Request) {
	session, _, err := a.loadSession(r)
	if err != nil || !session.ContainsAccount(claims(r).Subject) {
		a.fail(w, r, domain.ErrForbidden)
		return
	}
	state, _, err := a.loadLobby(r, session)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	writeJSON(w, 200, state)
}

func (a *API) lobbyReady(w http.ResponseWriter, r *http.Request) {
	var request struct {
		ExpectedSessionRevision int64 `json:"expected_session_revision"`
		Ready                   bool  `json:"ready"`
	}
	if !decode(w, r, &request) {
		return
	}
	session, _, err := a.loadSession(r)
	member, found := session.MemberByXUID(a.xuid(r))
	if err != nil || !found || member.AccountID != claims(r).Subject || session.Revision != request.ExpectedSessionRevision {
		a.fail(w, r, domain.ErrConflict)
		return
	}
	state, doc, err := a.loadLobby(r, session)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	state.Ready[member.XUID] = request.Ready
	state, err = a.saveLobby(r, session, state, doc, "session.ready_changed")
	if err != nil {
		a.fail(w, r, err)
		return
	}
	writeJSON(w, 200, state)
}

func (a *API) lobbySpectator(w http.ResponseWriter, r *http.Request) {
	var request struct {
		ExpectedSessionRevision int64 `json:"expected_session_revision"`
		Spectating              bool  `json:"spectating"`
	}
	if !decode(w, r, &request) {
		return
	}
	session, _, err := a.loadSession(r)
	member, found := session.MemberByXUID(a.xuid(r))
	if err != nil || !found || member.AccountID != claims(r).Subject || session.Revision != request.ExpectedSessionRevision {
		a.fail(w, r, domain.ErrConflict)
		return
	}
	state, doc, err := a.loadLobby(r, session)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	state.Spectators[member.XUID] = request.Spectating
	state.Ready[member.XUID] = false
	state, err = a.saveLobby(r, session, state, doc, "session.spectator_changed")
	if err != nil {
		a.fail(w, r, err)
		return
	}
	writeJSON(w, 200, state)
}

func (a *API) lobbyNextGame(w http.ResponseWriter, r *http.Request) {
	session, _, err := a.loadSession(r)
	if err != nil || session.HostXUID != a.xuid(r) || !session.ContainsAccount(claims(r).Subject) {
		a.fail(w, r, domain.ErrForbidden)
		return
	}
	var request struct {
		ExpectedSessionRevision int64         `json:"expected_session_revision"`
		Next                    nextGameState `json:"next_game"`
	}
	if !decode(w, r, &request) {
		return
	}
	if session.Revision != request.ExpectedSessionRevision || !domain.ValidateID(request.Next.Mode) || !validEpisode(request.Next.Episode) || len(request.Next.Contexts) > 64 || len(request.Next.Properties) > 64 {
		a.fail(w, r, domain.ErrInvalid)
		return
	}
	for _, value := range request.Next.Properties {
		if _, err := decodeBase64(value, 512); err != nil {
			a.fail(w, r, err)
			return
		}
	}
	state, doc, err := a.loadLobby(r, session)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	state.NextGame = &request.Next
	state, err = a.saveLobby(r, session, state, doc, "session.next_game_changed")
	if err != nil {
		a.fail(w, r, err)
		return
	}
	writeJSON(w, 200, state)
}

func (a *API) lobbyKickVote(w http.ResponseWriter, r *http.Request) {
	var request struct {
		ExpectedSessionRevision int64  `json:"expected_session_revision"`
		TargetXUID              string `json:"target_xuid"`
		Vote                    bool   `json:"vote"`
	}
	if !decode(w, r, &request) {
		return
	}
	session, sessionDoc, err := a.loadSession(r)
	voter, voterFound := session.MemberByXUID(a.xuid(r))
	target, targetFound := session.MemberByXUID(request.TargetXUID)
	if err != nil || !voterFound || !targetFound || voter.AccountID != claims(r).Subject || target.XUID == session.HostXUID || voter.XUID == target.XUID {
		a.fail(w, r, domain.ErrForbidden)
		return
	}
	if session.Revision != request.ExpectedSessionRevision {
		a.fail(w, r, domain.ErrConflict)
		return
	}
	state, lobbyDoc, err := a.loadLobby(r, session)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	voters := map[string]bool{}
	for _, xuid := range state.KickVotes[target.XUID] {
		voters[xuid] = true
	}
	if request.Vote {
		voters[voter.XUID] = true
	} else {
		delete(voters, voter.XUID)
	}
	state.KickVotes[target.XUID] = state.KickVotes[target.XUID][:0]
	for xuid := range voters {
		state.KickVotes[target.XUID] = append(state.KickVotes[target.XUID], xuid)
	}
	sort.Strings(state.KickVotes[target.XUID])
	threshold := kickVoteThreshold(len(session.Members))
	kicked := len(state.KickVotes[target.XUID]) >= threshold
	if !kicked {
		state, err = a.saveLobby(r, session, state, lobbyDoc, "session.kick_vote_changed")
		if err != nil {
			a.fail(w, r, err)
			return
		}
		writeJSON(w, 200, map[string]any{"lobby": state, "threshold": threshold, "kicked": false})
		return
	}
	members := make([]domain.SessionMember, 0, len(session.Members)-1)
	for _, member := range session.Members {
		if member.XUID != target.XUID {
			members = append(members, member)
		}
	}
	session.Members = members
	session.Revision = sessionDoc.Version + 1
	session.UpdatedAt = time.Now().UTC().Format(time.RFC3339Nano)
	sessionDoc.Body = marshal(session)
	sessionDoc.Events = a.sessionEvents(r, session, "session.member_kicked")
	state.Revision = lobbyDoc.Version + 1
	delete(state.Ready, target.XUID)
	delete(state.Spectators, target.XUID)
	delete(state.KickVotes, target.XUID)
	lobbyDoc.Body = marshal(state)
	lobbyDoc.Events = lobbyEvents(r, session, state, "session.kick_vote_passed")
	if _, err = a.store.PutDocumentsAtomic(r.Context(), []domain.Document{sessionDoc, lobbyDoc}); err != nil {
		a.fail(w, r, err)
		return
	}
	writeJSON(w, 200, map[string]any{"lobby": state, "session": session, "threshold": threshold, "kicked": true})
}

func kickVoteThreshold(memberCount int) int {
	return memberCount/2 + 1
}

func validEpisode(value string) bool {
	return value == "base" || value == "tlad" || value == "tbogt"
}

var rankCashThresholds = [...]int64{0, 1000, 10000, 50000, 100000, 250000, 500000, 750000, 1000000, 2500000, 5000000}

type progressionRecord struct {
	XUID            string   `json:"xuid"`
	Cash            int64    `json:"cash"`
	Rank            int      `json:"rank"`
	ClothingUnlocks []string `json:"clothing_unlocks"`
	UpdatedAt       string   `json:"updated_at"`
}

func progressionForCash(xuid string, cash int64, now string) progressionRecord {
	rank := 0
	for candidate, threshold := range rankCashThresholds {
		if cash >= threshold {
			rank = candidate
		}
	}
	unlocks := make([]string, 0, rank+1)
	for candidate := 0; candidate <= rank; candidate++ {
		unlocks = append(unlocks, "rank_"+strconvItoa(candidate))
	}
	return progressionRecord{XUID: xuid, Cash: cash, Rank: rank, ClothingUnlocks: unlocks, UpdatedAt: now}
}

func strconvItoa(value int) string {
	const digits = "0123456789"
	if value >= 0 && value < len(digits) {
		return string(digits[value])
	}
	return "0"
}

type rankedResultRequest struct {
	ResultID          string `json:"result_id"`
	SessionID         string `json:"session_id"`
	ExpectedRevision  int64  `json:"expected_revision"`
	ExpectedHostEpoch int64  `json:"expected_host_epoch"`
	Mode              string `json:"mode"`
	Rows              []struct {
		XUID      string `json:"xuid"`
		CashDelta int64  `json:"cash_delta"`
		Score     int64  `json:"score"`
		Kills     uint32 `json:"kills"`
		Deaths    uint32 `json:"deaths"`
		Won       bool   `json:"won"`
	} `json:"rows"`
}

func (a *API) rankedResult(w http.ResponseWriter, r *http.Request) {
	raw, ok := readRaw(w, r)
	if !ok {
		return
	}
	hash, replayed := a.idempotencyLookup(w, r, raw)
	if replayed {
		return
	}
	var request rankedResultRequest
	if !strictRaw(raw, &request) || !domain.ValidateID(request.ResultID) || !domain.ValidateID(request.Mode) || len(request.Rows) == 0 || len(request.Rows) > 64 {
		a.fail(w, r, domain.ErrInvalid)
		return
	}
	session, _, err := a.loadSessionByID(r, request.SessionID)
	if err != nil || !session.Ranked || session.HostXUID != a.xuid(r) || !session.ContainsAccount(claims(r).Subject) {
		a.fail(w, r, domain.ErrForbidden)
		return
	}
	if session.Revision != request.ExpectedRevision || session.HostEpoch != request.ExpectedHostEpoch {
		a.fail(w, r, domain.ErrConflict)
		return
	}
	seen := map[string]bool{}
	now := time.Now().UTC().Format(time.RFC3339Nano)
	documents := []domain.Document{{Kind: "ranked-result", ID: request.ResultID, OwnerID: claims(r).Subject, Body: marshal(request)}}
	progression := make([]progressionRecord, 0, len(request.Rows))
	for _, row := range request.Rows {
		member, found := session.MemberByXUID(row.XUID)
		if !found || seen[row.XUID] || row.CashDelta < 0 {
			a.fail(w, r, domain.ErrInvalid)
			return
		}
		seen[row.XUID] = true
		id := row.XUID
		currentCash := int64(0)
		version := int64(0)
		if current, getErr := a.store.GetDocument(r.Context(), "progression", id); getErr == nil {
			existing, decodeErr := documentBody[progressionRecord](current)
			if decodeErr != nil {
				a.fail(w, r, domain.ErrConflict)
				return
			}
			currentCash = existing.Cash
			version = current.Version
		} else if getErr != domain.ErrNotFound {
			a.fail(w, r, getErr)
			return
		}
		if row.CashDelta > math.MaxInt64-currentCash {
			a.fail(w, r, domain.ErrInvalid)
			return
		}
		record := progressionForCash(row.XUID, currentCash+row.CashDelta, now)
		progression = append(progression, record)
		documents = append(documents, domain.Document{Kind: "progression", ID: id, OwnerID: member.AccountID, Body: marshal(record), Version: version, Events: []domain.Event{{EventKey: domain.NewID("evt"), UserID: member.AccountID, Type: "progression.updated", AggregateType: "progression", AggregateID: id, Revision: version + 1, CorrelationID: requestID(r), Payload: marshal(record)}}})
	}
	if _, err := a.store.PutDocumentsAtomic(r.Context(), documents); err != nil {
		a.fail(w, r, err)
		return
	}
	response := map[string]any{"result_id": request.ResultID, "progression": progression}
	body, err := a.idempotencySave(r, hash, 201, response)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	writeRawJSON(w, 201, body)
}

func (a *API) progressionGet(w http.ResponseWriter, r *http.Request) {
	xuid := r.PathValue("xuid")
	if !hex64Pattern.MatchString(xuid) {
		a.fail(w, r, domain.ErrInvalid)
		return
	}
	doc, err := a.store.GetDocument(r.Context(), "progression", xuid)
	if err == domain.ErrNotFound {
		writeJSON(w, 200, progressionForCash(xuid, 0, ""))
		return
	}
	if err != nil {
		a.fail(w, r, err)
		return
	}
	record, err := documentBody[progressionRecord](doc)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	writeJSON(w, 200, record)
}

type episodeAppearance struct {
	Gender   string   `json:"gender"`
	Model    string   `json:"model"`
	Clothing []string `json:"clothing"`
}

type crossEpisodeProfile struct {
	XUID        string                       `json:"xuid"`
	PlayerName  string                       `json:"player_name"`
	Appearances map[string]episodeAppearance `json:"appearances"`
	UpdatedAt   string                       `json:"updated_at"`
}

func (a *API) profileGet(w http.ResponseWriter, r *http.Request) {
	xuid := r.PathValue("xuid")
	device, err := a.store.GetDeviceByXUID(r.Context(), xuid)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	doc, err := a.store.GetDocument(r.Context(), "cross-episode-profile", xuid)
	if err == domain.ErrNotFound {
		writeJSON(w, 200, crossEpisodeProfile{XUID: xuid, PlayerName: device.DisplayName, Appearances: map[string]episodeAppearance{}})
		return
	}
	profile, decodeErr := documentBody[crossEpisodeProfile](doc)
	if err != nil || decodeErr != nil {
		a.fail(w, r, domain.ErrNotFound)
		return
	}
	writeJSON(w, 200, profile)
}

func (a *API) profilePut(w http.ResponseWriter, r *http.Request) {
	var request struct {
		Episode    string            `json:"episode"`
		Appearance episodeAppearance `json:"appearance"`
	}
	if !decode(w, r, &request) {
		return
	}
	if !validEpisode(request.Episode) || (request.Appearance.Gender != "male" && request.Appearance.Gender != "female") || !domain.ValidateID(request.Appearance.Model) || len(request.Appearance.Clothing) > 32 {
		a.fail(w, r, domain.ErrInvalid)
		return
	}
	for _, item := range request.Appearance.Clothing {
		if !domain.ValidateID(item) {
			a.fail(w, r, domain.ErrInvalid)
			return
		}
	}
	device, err := a.store.GetDevice(r.Context(), claims(r).DeviceID)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	profile := crossEpisodeProfile{XUID: device.XUID, PlayerName: device.DisplayName, Appearances: map[string]episodeAppearance{}}
	doc, err := a.store.GetDocument(r.Context(), "cross-episode-profile", device.XUID)
	if err == nil {
		profile, err = documentBody[crossEpisodeProfile](doc)
	} else if err == domain.ErrNotFound {
		doc = domain.Document{Kind: "cross-episode-profile", ID: device.XUID, OwnerID: device.AccountID}
		err = nil
	}
	if err != nil {
		a.fail(w, r, err)
		return
	}
	profile.Appearances[request.Episode] = request.Appearance
	profile.UpdatedAt = time.Now().UTC().Format(time.RFC3339Nano)
	if err := a.upsertOwned(r, "cross-episode-profile", device.XUID, device.AccountID, profile); err != nil {
		a.fail(w, r, err)
		return
	}
	writeJSON(w, 200, profile)
}

type presenceRecord struct {
	XUID      string `json:"xuid"`
	State     string `json:"state"`
	SessionID string `json:"session_id,omitempty"`
	Episode   string `json:"episode"`
	ExpiresAt string `json:"expires_at"`
}

func (a *API) presencePut(w http.ResponseWriter, r *http.Request) {
	var request struct {
		State     string `json:"state"`
		SessionID string `json:"session_id"`
		Episode   string `json:"episode"`
	}
	if !decode(w, r, &request) {
		return
	}
	if (request.State != "online" && request.State != "in_game" && request.State != "away") || !validEpisode(request.Episode) || (request.SessionID != "" && !sessionIDPattern.MatchString(request.SessionID)) {
		a.fail(w, r, domain.ErrInvalid)
		return
	}
	device, err := a.store.GetDevice(r.Context(), claims(r).DeviceID)
	if err != nil {
		a.fail(w, r, err)
		return
	}
	record := presenceRecord{XUID: device.XUID, State: request.State, SessionID: request.SessionID, Episode: request.Episode, ExpiresAt: time.Now().UTC().Add(90 * time.Second).Format(time.RFC3339Nano)}
	if err := a.upsertOwned(r, "presence", device.XUID, device.AccountID, record); err != nil {
		a.fail(w, r, err)
		return
	}
	writeJSON(w, 200, record)
}

func (a *API) presenceGet(w http.ResponseWriter, r *http.Request) {
	xuid := r.PathValue("xuid")
	doc, err := a.store.GetDocument(r.Context(), "presence", xuid)
	if err != nil {
		writeJSON(w, 200, presenceRecord{XUID: xuid, State: "offline"})
		return
	}
	record, err := documentBody[presenceRecord](doc)
	expires, parseErr := time.Parse(time.RFC3339Nano, record.ExpiresAt)
	if err != nil || parseErr != nil || !time.Now().Before(expires) {
		writeJSON(w, 200, presenceRecord{XUID: xuid, State: "offline"})
		return
	}
	writeJSON(w, 200, record)
}

func normalizedStrings(values []string, max int) ([]string, bool) {
	if len(values) > max {
		return nil, false
	}
	seen := map[string]bool{}
	result := make([]string, 0, len(values))
	for _, value := range values {
		value = strings.TrimSpace(value)
		if !domain.ValidateID(value) || seen[value] {
			return nil, false
		}
		seen[value] = true
		result = append(result, value)
	}
	sort.Strings(result)
	return result, true
}

func rawObject(value any) json.RawMessage { return marshal(value) }
