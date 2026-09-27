package store

import (
	"context"
	"encoding/json"
	"sync"
	"time"

	"libertyrecomp/community-server/internal/domain"
)

type memoryIdempotency struct {
	result  IdempotencyResult
	expires time.Time
}

type Memory struct {
	mu          sync.RWMutex
	documents   map[string]domain.Document
	devices     map[string]domain.Device
	challenges  map[string]domain.Challenge
	refreshes   map[string]domain.RefreshSession
	idempotency map[string]memoryIdempotency
	events      []domain.Event
	delivered   map[int64]bool
	leasedUntil map[int64]time.Time
	audits      [][]byte
}

func NewMemory() *Memory {
	return &Memory{
		documents: map[string]domain.Document{}, devices: map[string]domain.Device{},
		challenges: map[string]domain.Challenge{}, refreshes: map[string]domain.RefreshSession{},
		idempotency: map[string]memoryIdempotency{}, delivered: map[int64]bool{}, leasedUntil: map[int64]time.Time{},
	}
}

func key(kind, id string) string              { return kind + "\x00" + id }
func (m *Memory) Ready(context.Context) error { return nil }
func (m *Memory) Close()                      {}

func (m *Memory) Maintain(_ context.Context, now time.Time) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	for id, challenge := range m.challenges {
		if challenge.Used || !now.Before(challenge.ExpiresAt) {
			delete(m.challenges, id)
		}
	}
	for hash, refresh := range m.refreshes {
		if refresh.Revoked || !now.Before(refresh.ExpiresAt) {
			delete(m.refreshes, hash)
		}
	}
	for keyValue, value := range m.idempotency {
		if !now.Before(value.expires) {
			delete(m.idempotency, keyValue)
		}
	}
	for documentKey, doc := range m.documents {
		expiresAt := ""
		switch doc.Kind {
		case "session":
			var session domain.SessionRecord
			if json.Unmarshal(doc.Body, &session) == nil {
				expiresAt = session.HostLeaseExpiresAt
			}
		case "presence", "voice-route", "invite-record":
			var body struct {
				ExpiresAt string `json:"expires_at"`
			}
			if json.Unmarshal(doc.Body, &body) == nil {
				expiresAt = body.ExpiresAt
			}
		case "ticket":
			if doc.UpdatedAt.Before(now.Add(-10 * time.Minute)) {
				delete(m.documents, documentKey)
			}
		}
		if expiresAt != "" {
			expires, err := time.Parse(time.RFC3339Nano, expiresAt)
			if err != nil || !now.Before(expires) {
				delete(m.documents, documentKey)
			}
		}
	}
	cutoff := now.Add(-30 * 24 * time.Hour)
	kept := m.events[:0]
	for _, event := range m.events {
		if !m.delivered[event.ID] || event.CreatedAt.After(cutoff) {
			kept = append(kept, event)
		} else {
			delete(m.delivered, event.ID)
			delete(m.leasedUntil, event.ID)
		}
	}
	m.events = kept
	return nil
}

func (m *Memory) CreateDocument(_ context.Context, doc domain.Document) (domain.Document, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	k := key(doc.Kind, doc.ID)
	if _, found := m.documents[k]; found {
		return domain.Document{}, domain.ErrConflict
	}
	now := time.Now().UTC()
	doc.CreatedAt = now
	doc.UpdatedAt = now
	doc.Version = 1
	m.documents[k] = cloneDocument(doc)
	m.appendEventsLocked(doc.Events, now)
	return cloneDocument(doc), nil
}

func (m *Memory) GetDocument(_ context.Context, kind, id string) (domain.Document, error) {
	m.mu.RLock()
	defer m.mu.RUnlock()
	doc, found := m.documents[key(kind, id)]
	if !found {
		return domain.Document{}, domain.ErrNotFound
	}
	return cloneDocument(doc), nil
}

func (m *Memory) UpdateDocument(_ context.Context, doc domain.Document, expected int64) (domain.Document, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	k := key(doc.Kind, doc.ID)
	current, found := m.documents[k]
	if !found {
		return domain.Document{}, domain.ErrNotFound
	}
	if current.Version != expected {
		return domain.Document{}, domain.ErrConflict
	}
	doc.CreatedAt = current.CreatedAt
	doc.UpdatedAt = time.Now().UTC()
	doc.Version = current.Version + 1
	m.documents[k] = cloneDocument(doc)
	m.appendEventsLocked(doc.Events, doc.UpdatedAt)
	return cloneDocument(doc), nil
}
func (m *Memory) PutDocumentsAtomic(_ context.Context, documents []domain.Document) ([]domain.Document, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, doc := range documents {
		current, found := m.documents[key(doc.Kind, doc.ID)]
		if doc.Version == 0 && found {
			return nil, domain.ErrConflict
		}
		if doc.Version > 0 && (!found || current.Version != doc.Version) {
			return nil, domain.ErrConflict
		}
	}
	now := time.Now().UTC()
	out := make([]domain.Document, 0, len(documents))
	for _, doc := range documents {
		current, found := m.documents[key(doc.Kind, doc.ID)]
		if found {
			doc.CreatedAt = current.CreatedAt
			doc.Version = current.Version + 1
		} else {
			doc.CreatedAt = now
			doc.Version = 1
		}
		doc.UpdatedAt = now
		m.documents[key(doc.Kind, doc.ID)] = cloneDocument(doc)
		m.appendEventsLocked(doc.Events, now)
		out = append(out, cloneDocument(doc))
	}
	return out, nil
}

func (m *Memory) DeleteDocument(_ context.Context, kind, id string, expected int64) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	k := key(kind, id)
	current, found := m.documents[k]
	if !found {
		return domain.ErrNotFound
	}
	if current.Version != expected {
		return domain.ErrConflict
	}
	delete(m.documents, k)
	return nil
}
func (m *Memory) DeleteSession(_ context.Context, id string, expected int64, events []domain.Event) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	k := key("session", id)
	current, found := m.documents[k]
	if !found {
		return domain.ErrNotFound
	}
	if current.Version != expected {
		return domain.ErrConflict
	}
	delete(m.documents, k)
	m.appendEventsLocked(events, time.Now().UTC())
	return nil
}
func (m *Memory) MigrateSession(_ context.Context, oldDoc, newDoc domain.Document, expected int64) (domain.Document, domain.Document, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	oldKey := key("session", oldDoc.ID)
	current, found := m.documents[oldKey]
	if !found {
		return oldDoc, newDoc, domain.ErrNotFound
	}
	if current.Version != expected {
		return oldDoc, newDoc, domain.ErrConflict
	}
	newKey := key("session", newDoc.ID)
	if _, found := m.documents[newKey]; found {
		return oldDoc, newDoc, domain.ErrConflict
	}
	now := time.Now().UTC()
	oldDoc.CreatedAt = current.CreatedAt
	oldDoc.UpdatedAt = now
	oldDoc.Version = current.Version + 1
	newDoc.CreatedAt = now
	newDoc.UpdatedAt = now
	newDoc.Version = 1
	m.documents[oldKey] = cloneDocument(oldDoc)
	m.documents[newKey] = cloneDocument(newDoc)
	m.appendEventsLocked(oldDoc.Events, now)
	return cloneDocument(oldDoc), cloneDocument(newDoc), nil
}

func (m *Memory) ListDocuments(_ context.Context, kind, owner string, limit int) ([]domain.Document, error) {
	m.mu.RLock()
	defer m.mu.RUnlock()
	result := make([]domain.Document, 0)
	for _, doc := range m.documents {
		if doc.Kind == kind && (owner == "" || doc.OwnerID == owner) {
			result = append(result, cloneDocument(doc))
			if len(result) == limit {
				break
			}
		}
	}
	return result, nil
}
func (m *Memory) SearchSessions(_ context.Context, q SessionQuery) ([]domain.Document, error) {
	m.mu.RLock()
	defer m.mu.RUnlock()
	out := []domain.Document{}
	for _, doc := range m.documents {
		if doc.Kind != "session" {
			continue
		}
		var s domain.SessionRecord
		if json.Unmarshal(doc.Body, &s) != nil {
			continue
		}
		lease, leaseErr := time.Parse(time.RFC3339Nano, s.HostLeaseExpiresAt)
		if leaseErr != nil || !time.Now().UTC().Before(lease) || s.State != "open" || s.TitleID != q.TitleID || s.MediaID != q.MediaID || s.TitleVersion != q.TitleVersion || s.ProtocolVersion != q.ProtocolVersion || len(s.Members) >= s.PublicSlots+s.PrivateSlots || (s.Visibility == "private" && !s.ContainsAccount(q.Requestor)) {
			continue
		}
		matched := true
		for k, v := range q.Contexts {
			if s.Contexts[k] != v {
				matched = false
				break
			}
		}
		for k, v := range q.Properties {
			if s.Properties[k] != v {
				matched = false
				break
			}
		}
		if matched {
			out = append(out, cloneDocument(doc))
			if len(out) == q.Limit {
				break
			}
		}
	}
	return out, nil
}

func (m *Memory) PutDevice(_ context.Context, value domain.Device) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	for id, existing := range m.devices {
		if id != value.ID && existing.XUID == value.XUID {
			return domain.ErrConflict
		}
	}
	if existing, found := m.devices[value.ID]; found && existing.PublicKey != value.PublicKey {
		return domain.ErrConflict
	}
	m.devices[value.ID] = value
	return nil
}

func (m *Memory) GetDevice(_ context.Context, id string) (domain.Device, error) {
	m.mu.RLock()
	defer m.mu.RUnlock()
	value, found := m.devices[id]
	if !found {
		return domain.Device{}, domain.ErrNotFound
	}
	return value, nil
}
func (m *Memory) GetDeviceByXUID(_ context.Context, xuid string) (domain.Device, error) {
	m.mu.RLock()
	defer m.mu.RUnlock()
	for _, device := range m.devices {
		if device.XUID == xuid {
			return device, nil
		}
	}
	return domain.Device{}, domain.ErrNotFound
}

func (m *Memory) PutChallenge(_ context.Context, value domain.Challenge) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.challenges[value.ID] = value
	return nil
}

func (m *Memory) ConsumeChallenge(_ context.Context, id, deviceID string, now time.Time) (domain.Challenge, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	value, found := m.challenges[id]
	if !found {
		return domain.Challenge{}, domain.ErrNotFound
	}
	if value.Used || value.DeviceID != deviceID || !now.Before(value.ExpiresAt) {
		return domain.Challenge{}, domain.ErrForbidden
	}
	value.Used = true
	m.challenges[id] = value
	return value, nil
}

func (m *Memory) PutRefresh(_ context.Context, value domain.RefreshSession) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.refreshes[value.TokenHash] = value
	return nil
}

func (m *Memory) ConsumeRefresh(_ context.Context, hash string, now time.Time) (domain.RefreshSession, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	value, found := m.refreshes[hash]
	if !found || value.Revoked || !now.Before(value.ExpiresAt) {
		return domain.RefreshSession{}, domain.ErrForbidden
	}
	value.Revoked = true
	m.refreshes[hash] = value
	return value, nil
}

func (m *Memory) ReserveIdempotency(_ context.Context, subject, method, path, keyValue, requestHash string, expires time.Time) (IdempotencyResult, IdempotencyState, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	k := subject + "\x00" + method + "\x00" + path + "\x00" + keyValue
	if existing, found := m.idempotency[k]; found && time.Now().Before(existing.expires) {
		if existing.result.RequestHash != requestHash {
			return existing.result, IdempotencyConflict, nil
		}
		if existing.result.Status == 0 {
			return existing.result, IdempotencyInProgress, nil
		}
		return existing.result, IdempotencyReplay, nil
	}
	result := IdempotencyResult{RequestHash: requestHash}
	m.idempotency[k] = memoryIdempotency{result: result, expires: expires}
	return result, IdempotencyReserved, nil
}

func (m *Memory) FinalizeIdempotency(_ context.Context, subject, method, path, keyValue string, result IdempotencyResult, expires time.Time) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	k := subject + "\x00" + method + "\x00" + path + "\x00" + keyValue
	existing, found := m.idempotency[k]
	if !found || existing.result.Status != 0 || existing.result.RequestHash != result.RequestHash {
		return domain.ErrConflict
	}
	m.idempotency[k] = memoryIdempotency{result: result, expires: expires}
	return nil
}

func (m *Memory) AppendEvent(_ context.Context, value domain.Event) (domain.Event, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	value.ID = int64(len(m.events) + 1)
	if value.EventKey == "" {
		value.EventKey = domain.NewID("evt")
	}
	value.CreatedAt = time.Now().UTC()
	m.events = append(m.events, value)
	return value, nil
}
func (m *Memory) AppendEvents(_ context.Context, events []domain.Event) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.appendEventsLocked(events, time.Now().UTC())
	return nil
}
func (m *Memory) appendEventsLocked(events []domain.Event, now time.Time) {
	for _, value := range events {
		value.ID = int64(len(m.events) + 1)
		if value.EventKey == "" {
			value.EventKey = domain.NewID("evt")
		}
		value.CreatedAt = now
		m.events = append(m.events, value)
	}
}

func (m *Memory) ReplayEvents(_ context.Context, user string, after int64, limit int) ([]domain.Event, error) {
	m.mu.RLock()
	defer m.mu.RUnlock()
	result := make([]domain.Event, 0)
	for _, event := range m.events {
		if event.ID > after && (event.UserID == user || event.UserID == "*") {
			result = append(result, event)
			if len(result) == limit {
				break
			}
		}
	}
	return result, nil
}

func (m *Memory) AppendAudit(_ context.Context, actor, action, targetType, targetID string, data []byte) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	row, _ := json.Marshal(map[string]any{"actor": actor, "action": action, "target_type": targetType, "target_id": targetID, "data": json.RawMessage(data), "at": time.Now().UTC()})
	m.audits = append(m.audits, row)
	return nil
}

func (m *Memory) LeaseOutbox(_ context.Context, limit int) ([]domain.Event, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	result := make([]domain.Event, 0)
	now := time.Now()
	leaseUntil := now.Add(30 * time.Second)
	for _, event := range m.events {
		if !m.delivered[event.ID] && !now.Before(m.leasedUntil[event.ID]) {
			result = append(result, event)
			m.leasedUntil[event.ID] = leaseUntil
			if len(result) == limit {
				break
			}
		}
	}
	return result, nil
}

func (m *Memory) MarkOutboxDelivered(_ context.Context, ids []int64) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, id := range ids {
		m.delivered[id] = true
		delete(m.leasedUntil, id)
	}
	return nil
}

func cloneDocument(doc domain.Document) domain.Document {
	doc.Body = append(json.RawMessage(nil), doc.Body...)
	doc.Events = nil
	return doc
}
