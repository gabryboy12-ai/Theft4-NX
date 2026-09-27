package roles

import (
	"context"
	"time"

	"libertyrecomp/community-server/internal/domain"
	"libertyrecomp/community-server/internal/realtime"
	"libertyrecomp/community-server/internal/store"
)

func RunWorker(ctx context.Context, s store.Store, hub *realtime.Hub) error {
	ticker := time.NewTicker(250 * time.Millisecond)
	defer ticker.Stop()
	maintenance := time.NewTicker(time.Minute)
	defer maintenance.Stop()
	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case now := <-maintenance.C:
			_ = s.Maintain(ctx, now.UTC())
		case <-ticker.C:
			events, err := s.LeaseOutbox(ctx, 100)
			if err != nil {
				continue
			}
			ids := make([]int64, 0, len(events))
			for _, event := range events {
				if hub.Broadcast(ctx, event) == nil {
					ids = append(ids, event.ID)
				}
			}
			if len(ids) > 0 {
				_ = s.MarkOutboxDelivered(ctx, ids)
			}
		}
	}
}

func RunMatchmaker(ctx context.Context, s store.Store, hub *realtime.Hub) error {
	_ = hub
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-ticker.C:
			match(ctx, s, hub)
		}
	}
}
func match(ctx context.Context, s store.Store, hub *realtime.Hub) {
	tickets, err := s.ListDocuments(ctx, "ticket", "", 100)
	if err != nil {
		return
	}
	sessions, err := s.ListDocuments(ctx, "session", "", 1000)
	if err != nil {
		return
	}
	for _, ticketDoc := range tickets {
		var ticket domain.MatchmakingTicket
		if err := jsonBody(ticketDoc.Body, &ticket); err != nil || ticket.State != "searching" {
			continue
		}
		for _, sessionDoc := range sessions {
			var session domain.SessionRecord
			if err := jsonBody(sessionDoc.Body, &session); err != nil {
				continue
			}
			lease, leaseErr := time.Parse(time.RFC3339Nano, session.HostLeaseExpiresAt)
			if leaseErr != nil || !time.Now().UTC().Before(lease) || session.State != "open" || session.Mode != ticket.Mode || session.Episode != ticket.Episode || session.Ranked != ticket.Ranked || len(session.Members)+ticket.PartySize > session.PublicSlots+session.PrivateSlots {
				continue
			}
			ticket.State = "matched"
			ticket.MatchedSession = session.ID
			ticket.UpdatedAt = time.Now().UTC().Format(time.RFC3339Nano)
			ticketDoc.Body = marshal(ticket)
			ticketDoc.Events = []domain.Event{{EventKey: domain.NewID("evt"), UserID: ticket.OwnerID, Type: "matchmaking.matched", AggregateType: "ticket", AggregateID: ticket.ID, Revision: ticketDoc.Version + 1, Payload: marshal(ticket)}}
			_, err := s.UpdateDocument(ctx, ticketDoc, ticketDoc.Version)
			if err != nil {
				break
			}
			break
		}
	}
}
