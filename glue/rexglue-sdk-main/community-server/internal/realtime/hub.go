package realtime

import (
	"context"
	"encoding/json"
	"sync"
	"time"

	"github.com/gorilla/websocket"
	redis "github.com/redis/go-redis/v9"
	"libertyrecomp/community-server/internal/domain"
	"libertyrecomp/community-server/internal/store"
)

type Hub struct {
	store   store.Store
	redis   *redis.Client
	mu      sync.RWMutex
	clients map[string]map[*websocket.Conn]struct{}
}

func New(s store.Store, redisURL string) (*Hub, error) {
	h := &Hub{store: s, clients: map[string]map[*websocket.Conn]struct{}{}}
	if redisURL != "" {
		options, err := redis.ParseURL(redisURL)
		if err != nil {
			return nil, err
		}
		h.redis = redis.NewClient(options)
	}
	return h, nil
}
func (h *Hub) Close() {
	if h.redis != nil {
		_ = h.redis.Close()
	}
}
func (h *Hub) Ready(ctx context.Context) error {
	if h.redis != nil {
		return h.redis.Ping(ctx).Err()
	}
	return nil
}

func (h *Hub) Publish(ctx context.Context, event domain.Event) (domain.Event, error) {
	stored, err := h.store.AppendEvent(ctx, event)
	if err != nil {
		return stored, err
	}
	payload, _ := json.Marshal(stored)
	_ = payload
	h.deliver(stored)
	return stored, nil
}
func (h *Hub) Broadcast(ctx context.Context, event domain.Event) error {
	payload, err := json.Marshal(event)
	if err != nil {
		return err
	}
	if h.redis != nil {
		return h.redis.Publish(ctx, "community.events", payload).Err()
	}
	h.deliver(event)
	return nil
}

func (h *Hub) RunSubscriber(ctx context.Context) error {
	if h.redis == nil {
		<-ctx.Done()
		return ctx.Err()
	}
	subscription := h.redis.Subscribe(ctx, "community.events")
	defer subscription.Close()
	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case message, ok := <-subscription.Channel():
			if !ok {
				return nil
			}
			var event domain.Event
			if json.Unmarshal([]byte(message.Payload), &event) == nil {
				h.deliver(event)
			}
		}
	}
}

func (h *Hub) Serve(conn *websocket.Conn, user string, replay []domain.Event) {
	h.mu.Lock()
	if h.clients[user] == nil {
		h.clients[user] = map[*websocket.Conn]struct{}{}
	}
	h.clients[user][conn] = struct{}{}
	h.mu.Unlock()
	defer func() {
		h.mu.Lock()
		delete(h.clients[user], conn)
		if len(h.clients[user]) == 0 {
			delete(h.clients, user)
		}
		h.mu.Unlock()
		_ = conn.Close()
	}()
	_ = conn.SetReadDeadline(time.Now().Add(90 * time.Second))
	conn.SetPongHandler(func(string) error { return conn.SetReadDeadline(time.Now().Add(90 * time.Second)) })
	for _, event := range replay {
		if err := conn.WriteJSON(event); err != nil {
			return
		}
	}
	for {
		if _, _, err := conn.ReadMessage(); err != nil {
			return
		}
	}
}

func (h *Hub) deliver(event domain.Event) {
	payload, err := json.Marshal(event)
	if err != nil {
		return
	}
	h.mu.RLock()
	defer h.mu.RUnlock()
	send := func(user string) {
		for conn := range h.clients[user] {
			_ = conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
			_ = conn.WriteMessage(websocket.TextMessage, payload)
		}
	}
	send(event.UserID)
	if event.UserID != "*" {
		send("*")
	}
}
