package store

import (
	"context"
	"encoding/json"
	"errors"
	"sync"
	"testing"
	"time"

	"libertyrecomp/community-server/internal/domain"
)

func TestDocumentOptimisticConcurrency(t *testing.T) {
	s := NewMemory()
	ctx := context.Background()
	created, err := s.CreateDocument(ctx, domain.Document{Kind: "session", ID: "one", OwnerID: "owner", Body: json.RawMessage(`{"state":"open"}`)})
	if err != nil {
		t.Fatal(err)
	}
	created.Body = json.RawMessage(`{"state":"closed"}`)
	updated, err := s.UpdateDocument(ctx, created, 1)
	if err != nil {
		t.Fatal(err)
	}
	if updated.Version != 2 {
		t.Fatalf("version=%d", updated.Version)
	}
	if _, err := s.UpdateDocument(ctx, created, 1); !errors.Is(err, domain.ErrConflict) {
		t.Fatalf("expected conflict, got %v", err)
	}
}

func TestPutDocumentsAtomicRollsBackEntireBatch(t *testing.T) {
	s := NewMemory()
	ctx := context.Background()
	if _, err := s.CreateDocument(ctx, domain.Document{Kind: "stat-row", ID: "existing", OwnerID: "owner", Body: json.RawMessage(`{"value":1}`)}); err != nil {
		t.Fatal(err)
	}
	_, err := s.PutDocumentsAtomic(ctx, []domain.Document{
		{Kind: "stat-row", ID: "new", OwnerID: "owner", Body: json.RawMessage(`{"value":2}`)},
		{Kind: "stat-row", ID: "existing", OwnerID: "owner", Body: json.RawMessage(`{"value":3}`)},
	})
	if !errors.Is(err, domain.ErrConflict) {
		t.Fatalf("expected conflict, got %v", err)
	}
	if _, err := s.GetDocument(ctx, "stat-row", "new"); !errors.Is(err, domain.ErrNotFound) {
		t.Fatalf("partial batch commit: %v", err)
	}
}

func TestIdempotencyReservationIsAtomicAcrossConcurrentCallers(t *testing.T) {
	s := NewMemory()
	ctx := context.Background()
	states := make(chan IdempotencyState, 2)
	start := make(chan struct{})
	var group sync.WaitGroup
	for range 2 {
		group.Add(1)
		go func() {
			defer group.Done()
			<-start
			_, state, err := s.ReserveIdempotency(ctx, "owner", "POST", "/mutation", "key", "hash", time.Now().Add(time.Minute))
			if err != nil {
				t.Errorf("reserve: %v", err)
			}
			states <- state
		}()
	}
	close(start)
	group.Wait()
	close(states)
	counts := map[IdempotencyState]int{}
	for state := range states {
		counts[state]++
	}
	if counts[IdempotencyReserved] != 1 || counts[IdempotencyInProgress] != 1 {
		t.Fatalf("states=%v", counts)
	}
	if err := s.FinalizeIdempotency(ctx, "owner", "POST", "/mutation", "key", IdempotencyResult{Status: 201, Body: []byte(`{"ok":true}`), RequestHash: "hash"}, time.Now().Add(time.Hour)); err != nil {
		t.Fatal(err)
	}
	result, state, err := s.ReserveIdempotency(ctx, "owner", "POST", "/mutation", "key", "hash", time.Now().Add(time.Minute))
	if err != nil || state != IdempotencyReplay || result.Status != 201 {
		t.Fatalf("replay state=%v status=%d err=%v", state, result.Status, err)
	}
}

func TestMemoryRejectsDuplicateXUIDAcrossDevices(t *testing.T) {
	s := NewMemory()
	ctx := context.Background()
	first := domain.Device{ID: "device_one", AccountID: "account_one", PublicKey: "key_one", XUID: "0xe000000000000001"}
	second := domain.Device{ID: "device_two", AccountID: "account_two", PublicKey: "key_two", XUID: first.XUID}
	if err := s.PutDevice(ctx, first); err != nil {
		t.Fatal(err)
	}
	if err := s.PutDevice(ctx, second); !errors.Is(err, domain.ErrConflict) {
		t.Fatalf("duplicate XUID error=%v", err)
	}
}

func TestSearchSessionsFiltersExpiredHostLease(t *testing.T) {
	s := NewMemory()
	ctx := context.Background()
	now := time.Now().UTC()
	base := domain.SessionRecord{State: "open", TitleID: "title", MediaID: "media", TitleVersion: "version", ProtocolVersion: 2, PublicSlots: 64, Contexts: map[string]uint32{}, Properties: map[string]string{}}
	expired := base
	expired.ID = "expired"
	expired.HostLeaseExpiresAt = now.Add(-time.Minute).Format(time.RFC3339Nano)
	future := base
	future.ID = "future"
	future.HostLeaseExpiresAt = now.Add(time.Minute).Format(time.RFC3339Nano)
	if _, err := s.CreateDocument(ctx, domain.Document{Kind: "session", ID: expired.ID, Body: marshalTest(expired)}); err != nil {
		t.Fatal(err)
	}
	if _, err := s.CreateDocument(ctx, domain.Document{Kind: "session", ID: future.ID, Body: marshalTest(future)}); err != nil {
		t.Fatal(err)
	}
	results, err := s.SearchSessions(ctx, SessionQuery{TitleID: "title", MediaID: "media", TitleVersion: "version", ProtocolVersion: 2, Contexts: map[string]uint32{}, Properties: map[string]string{}, Limit: 10})
	if err != nil || len(results) != 1 || results[0].ID != future.ID {
		t.Fatalf("results=%+v err=%v", results, err)
	}
}

func TestMaintainPrunesExpiredAuthAndDocuments(t *testing.T) {
	s := NewMemory()
	ctx := context.Background()
	now := time.Now().UTC()
	_ = s.PutChallenge(ctx, domain.Challenge{ID: "expired", ExpiresAt: now.Add(-time.Minute)})
	_ = s.PutRefresh(ctx, domain.RefreshSession{TokenHash: "revoked", Revoked: true, ExpiresAt: now.Add(time.Hour)})
	_, _, _ = s.ReserveIdempotency(ctx, "owner", "POST", "/path", "key", "hash", now.Add(-time.Minute))
	_, _ = s.CreateDocument(ctx, domain.Document{Kind: "presence", ID: "owner", Body: json.RawMessage(`{"expires_at":"2000-01-01T00:00:00Z"}`)})
	if err := s.Maintain(ctx, now); err != nil {
		t.Fatal(err)
	}
	if _, err := s.GetDocument(ctx, "presence", "owner"); !errors.Is(err, domain.ErrNotFound) {
		t.Fatalf("expired presence survived: %v", err)
	}
	if _, err := s.ConsumeChallenge(ctx, "expired", "", now); !errors.Is(err, domain.ErrNotFound) {
		t.Fatalf("expired challenge survived: %v", err)
	}
	if _, err := s.ConsumeRefresh(ctx, "revoked", now); !errors.Is(err, domain.ErrForbidden) {
		t.Fatalf("revoked refresh survived: %v", err)
	}
}

func marshalTest(value any) json.RawMessage {
	raw, _ := json.Marshal(value)
	return raw
}

func TestEventReplayIsUserScoped(t *testing.T) {
	s := NewMemory()
	ctx := context.Background()
	_, _ = s.AppendEvent(ctx, domain.Event{UserID: "alice", Type: "private"})
	_, _ = s.AppendEvent(ctx, domain.Event{UserID: "*", Type: "broadcast"})
	_, _ = s.AppendEvent(ctx, domain.Event{UserID: "bob", Type: "private"})
	events, err := s.ReplayEvents(ctx, "alice", 0, 10)
	if err != nil {
		t.Fatal(err)
	}
	if len(events) != 2 {
		t.Fatalf("got %d events", len(events))
	}
}

func TestOutboxLeaseDoesNotDoubleClaim(t *testing.T) {
	s := NewMemory()
	ctx := context.Background()
	_, _ = s.AppendEvent(ctx, domain.Event{UserID: "alice", Type: "one"})
	first, err := s.LeaseOutbox(ctx, 10)
	if err != nil || len(first) != 1 {
		t.Fatalf("first lease: %v %d", err, len(first))
	}
	second, err := s.LeaseOutbox(ctx, 10)
	if err != nil || len(second) != 0 {
		t.Fatalf("duplicate lease: %v %d", err, len(second))
	}
	if err := s.MarkOutboxDelivered(ctx, []int64{first[0].ID}); err != nil {
		t.Fatal(err)
	}
}

func TestSessionStateAndOutboxAreAtomic(t *testing.T) {
	s := NewMemory()
	ctx := context.Background()
	created, err := s.CreateDocument(ctx, domain.Document{Kind: "session", ID: "atomic", OwnerID: "owner", Body: json.RawMessage(`{"revision":1}`), Events: []domain.Event{{UserID: "owner", Type: "created"}}})
	if err != nil {
		t.Fatal(err)
	}
	events, err := s.ReplayEvents(ctx, "owner", 0, 10)
	if err != nil || len(events) != 1 {
		t.Fatalf("create events: %v %d", err, len(events))
	}
	created.Body = json.RawMessage(`{"revision":2}`)
	created.Events = []domain.Event{{UserID: "owner", Type: "updated"}}
	if _, err := s.UpdateDocument(ctx, created, 99); !errors.Is(err, domain.ErrConflict) {
		t.Fatalf("expected conflict, got %v", err)
	}
	events, err = s.ReplayEvents(ctx, "owner", 0, 10)
	if err != nil || len(events) != 1 {
		t.Fatalf("conflict leaked event: %v %d", err, len(events))
	}
	if _, err := s.UpdateDocument(ctx, created, 1); err != nil {
		t.Fatal(err)
	}
	events, err = s.ReplayEvents(ctx, "owner", 0, 10)
	if err != nil || len(events) != 2 {
		t.Fatalf("commit event missing: %v %d", err, len(events))
	}
}
