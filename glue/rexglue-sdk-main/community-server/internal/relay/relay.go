package relay

import (
	"context"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"sync"
	"time"

	redis "github.com/redis/go-redis/v9"
	"libertyrecomp/community-server/internal/domain"
)

type Route struct {
	AccountID   string    `json:"account_id"`
	SessionID   string    `json:"session_id"`
	LocalPort   int       `json:"local_port"`
	VirtualIPv4 string    `json:"virtual_ipv4"`
	ExpiresAt   time.Time `json:"expires_at"`
}
type Datagram struct {
	DestinationIPv4 string    `json:"destination_ipv4"`
	DestinationPort int       `json:"destination_port"`
	SourceIPv4      string    `json:"source_ipv4"`
	SourcePort      int       `json:"source_port"`
	Payload         string    `json:"payload"`
	ReceivedAt      time.Time `json:"received_at"`
}

type Service struct {
	redis      *redis.Client
	mu         sync.Mutex
	routes     map[string]Route
	sources    map[string]Route
	accountIPs map[string]string
	ipOwners   map[string]string
	queues     map[string][]Datagram
}

func New(redisURL string) (*Service, error) {
	s := &Service{routes: map[string]Route{}, sources: map[string]Route{}, accountIPs: map[string]string{}, ipOwners: map[string]string{}, queues: map[string][]Datagram{}}
	if redisURL != "" {
		options, err := redis.ParseURL(redisURL)
		if err != nil {
			return nil, err
		}
		s.redis = redis.NewClient(options)
	}
	return s, nil
}
func (s *Service) Close() {
	if s.redis != nil {
		_ = s.redis.Close()
	}
}
func (s *Service) Ready(ctx context.Context) error {
	if s.redis != nil {
		return s.redis.Ping(ctx).Err()
	}
	return nil
}
func routeKey(ip string, port int) string      { return fmt.Sprintf("%s:%d", ip, port) }
func queueKey(account string, port int) string { return fmt.Sprintf("relay:q:%s:%d", account, port) }
func sourceKey(account string, port int) string {
	return fmt.Sprintf("relay:source:%s:%d", account, port)
}
func candidateIPv4(account string, attempt int) string {
	digest := sha256.Sum256([]byte(fmt.Sprintf("%s:%d", account, attempt)))
	return fmt.Sprintf("10.%d.%d.%d", digest[0], digest[1], digest[2])
}
func (s *Service) EnsureVirtualIPv4(ctx context.Context, account string) (string, error) {
	if s.redis != nil {
		accountKey := "relay:account-ip:" + account
		if value, err := s.redis.Get(ctx, accountKey).Result(); err == nil {
			return value, nil
		}
		for attempt := 0; attempt < 1024; attempt++ {
			ip := candidateIPv4(account, attempt)
			ownerKey := "relay:ip-owner:" + ip
			claimed, err := s.redis.SetNX(ctx, ownerKey, account, 0).Result()
			if err != nil {
				return "", err
			}
			if claimed {
				if err := s.redis.Set(ctx, accountKey, ip, 0).Err(); err != nil {
					return "", err
				}
				return ip, nil
			}
			owner, _ := s.redis.Get(ctx, ownerKey).Result()
			if owner == account {
				_ = s.redis.Set(ctx, accountKey, ip, 0).Err()
				return ip, nil
			}
		}
		return "", domain.ErrConflict
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if ip := s.accountIPs[account]; ip != "" {
		return ip, nil
	}
	for attempt := 0; attempt < 1024; attempt++ {
		ip := candidateIPv4(account, attempt)
		if owner := s.ipOwners[ip]; owner == "" || owner == account {
			s.ipOwners[ip] = account
			s.accountIPs[account] = ip
			return ip, nil
		}
	}
	return "", domain.ErrConflict
}

func (s *Service) Register(ctx context.Context, account, session string, port int, ttl time.Duration) (Route, error) {
	if port < 1 || port > 65535 {
		return Route{}, domain.ErrInvalid
	}
	ip, err := s.EnsureVirtualIPv4(ctx, account)
	if err != nil {
		return Route{}, err
	}
	route := Route{AccountID: account, SessionID: session, LocalPort: port, VirtualIPv4: ip, ExpiresAt: time.Now().UTC().Add(ttl)}
	raw, _ := json.Marshal(route)
	key := "relay:route:" + routeKey(route.VirtualIPv4, port)
	if existing, err := s.getRoute(ctx, route.VirtualIPv4, port); err == nil && existing.AccountID != account {
		return Route{}, domain.ErrConflict
	}
	if s.redis != nil {
		pipe := s.redis.TxPipeline()
		pipe.Set(ctx, key, raw, ttl)
		pipe.Set(ctx, sourceKey(account, port), raw, ttl)
		if _, err := pipe.Exec(ctx); err != nil {
			return Route{}, err
		}
	} else {
		s.mu.Lock()
		s.routes[key] = route
		s.sources[sourceKey(account, port)] = route
		s.mu.Unlock()
	}
	return route, nil
}
func (s *Service) Unregister(ctx context.Context, route Route) error {
	key := "relay:route:" + routeKey(route.VirtualIPv4, route.LocalPort)
	if s.redis != nil {
		return s.redis.Del(ctx, key, sourceKey(route.AccountID, route.LocalPort)).Err()
	}
	s.mu.Lock()
	delete(s.routes, key)
	delete(s.sources, sourceKey(route.AccountID, route.LocalPort))
	s.mu.Unlock()
	return nil
}
func (s *Service) getRoute(ctx context.Context, ip string, port int) (Route, error) {
	key := "relay:route:" + routeKey(ip, port)
	var raw []byte
	if s.redis != nil {
		value, err := s.redis.Get(ctx, key).Bytes()
		if err != nil {
			return Route{}, domain.ErrNotFound
		}
		raw = value
	} else {
		s.mu.Lock()
		route, ok := s.routes[key]
		s.mu.Unlock()
		if !ok || time.Now().After(route.ExpiresAt) {
			return Route{}, domain.ErrNotFound
		}
		return route, nil
	}
	var route Route
	if json.Unmarshal(raw, &route) != nil {
		return Route{}, domain.ErrInvalid
	}
	return route, nil
}
func (s *Service) Source(ctx context.Context, account string, port int) (Route, error) {
	if s.redis != nil {
		raw, err := s.redis.Get(ctx, sourceKey(account, port)).Bytes()
		if err != nil {
			return Route{}, domain.ErrNotFound
		}
		var route Route
		if json.Unmarshal(raw, &route) != nil {
			return Route{}, domain.ErrInvalid
		}
		return route, nil
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if route, ok := s.sources[sourceKey(account, port)]; ok && time.Now().Before(route.ExpiresAt) {
		return route, nil
	}
	return Route{}, domain.ErrNotFound
}
func (s *Service) Send(ctx context.Context, source Route, d Datagram) error {
	return s.SendBatch(ctx, map[int]Route{source.LocalPort: source}, []Datagram{d})
}

func (s *Service) SendBatch(ctx context.Context, sources map[int]Route, datagrams []Datagram) error {
	if len(datagrams) == 0 || len(datagrams) > 64 {
		return domain.ErrInvalid
	}
	if s.redis != nil {
		lookup := s.redis.Pipeline()
		commands := make([]*redis.StringCmd, 0, len(datagrams))
		for _, datagram := range datagrams {
			commands = append(commands, lookup.Get(ctx, "relay:route:"+routeKey(datagram.DestinationIPv4, datagram.DestinationPort)))
		}
		_, _ = lookup.Exec(ctx)
		destinations := make([]Route, len(datagrams))
		for index, command := range commands {
			raw, err := command.Bytes()
			if err != nil || json.Unmarshal(raw, &destinations[index]) != nil {
				return domain.ErrNotFound
			}
			source, found := sources[datagrams[index].SourcePort]
			if !found || destinations[index].SessionID != source.SessionID {
				return domain.ErrForbidden
			}
		}
		now := time.Now().UTC()
		queueKeys := map[string]bool{}
		transaction := s.redis.TxPipeline()
		for index, datagram := range datagrams {
			source := sources[datagram.SourcePort]
			datagram.SourceIPv4 = source.VirtualIPv4
			datagram.ReceivedAt = now
			raw, _ := json.Marshal(datagram)
			key := queueKey(destinations[index].AccountID, destinations[index].LocalPort)
			transaction.RPush(ctx, key, raw)
			queueKeys[key] = true
		}
		for key := range queueKeys {
			transaction.LTrim(ctx, key, -256, -1)
			transaction.Expire(ctx, key, 2*time.Minute)
		}
		_, err := transaction.Exec(ctx)
		return err
	}

	s.mu.Lock()
	defer s.mu.Unlock()
	destinations := make([]Route, len(datagrams))
	for index, datagram := range datagrams {
		destination, found := s.routes["relay:route:"+routeKey(datagram.DestinationIPv4, datagram.DestinationPort)]
		source, sourceFound := sources[datagram.SourcePort]
		if !found || time.Now().After(destination.ExpiresAt) {
			return domain.ErrNotFound
		}
		if !sourceFound || destination.SessionID != source.SessionID {
			return domain.ErrForbidden
		}
		destinations[index] = destination
	}
	now := time.Now().UTC()
	for index, datagram := range datagrams {
		source := sources[datagram.SourcePort]
		datagram.SourceIPv4 = source.VirtualIPv4
		datagram.ReceivedAt = now
		key := queueKey(destinations[index].AccountID, destinations[index].LocalPort)
		s.queues[key] = append(s.queues[key], datagram)
		if len(s.queues[key]) > 256 {
			s.queues[key] = s.queues[key][len(s.queues[key])-256:]
		}
	}
	return nil
}

func (s *Service) Receive(ctx context.Context, account string, port, maxBytes, waitMS int) ([]Datagram, error) {
	key := queueKey(account, port)
	deadline := time.Now().Add(time.Duration(waitMS) * time.Millisecond)
	out := []Datagram{}
	used := 0
	for {
		if len(out) >= 64 || used >= maxBytes {
			break
		}
		var raw []byte
		popped := false
		if s.redis != nil {
			if len(out) == 0 && waitMS > 0 {
				timeout := time.Until(deadline)
				if timeout <= 0 {
					break
				}
				values, err := s.redis.BLPop(ctx, timeout, key).Result()
				if err != nil || len(values) != 2 {
					break
				}
				raw = []byte(values[1])
				popped = true
			} else {
				value, err := s.redis.LIndex(ctx, key, 0).Bytes()
				if err != nil {
					break
				}
				raw = value
			}
		} else {
			s.mu.Lock()
			queue := s.queues[key]
			if len(queue) == 0 {
				s.mu.Unlock()
				if len(out) > 0 || waitMS == 0 || time.Now().After(deadline) {
					break
				}
				select {
				case <-ctx.Done():
					return out, ctx.Err()
				case <-time.After(10 * time.Millisecond):
				}
				continue
			}
			raw, _ = json.Marshal(queue[0])
			s.mu.Unlock()
		}
		var datagram Datagram
		if json.Unmarshal(raw, &datagram) != nil {
			if s.redis != nil && !popped {
				_, _ = s.redis.LPop(ctx, key).Result()
			} else if s.redis == nil {
				s.mu.Lock()
				s.queues[key] = s.queues[key][1:]
				s.mu.Unlock()
			}
			continue
		}
		size := len(datagram.Payload)
		if used+size > maxBytes && len(out) > 0 {
			break
		}
		if s.redis != nil && !popped {
			value, err := s.redis.LPop(ctx, key).Bytes()
			if err != nil || json.Unmarshal(value, &datagram) != nil {
				continue
			}
		} else if s.redis == nil {
			s.mu.Lock()
			if len(s.queues[key]) == 0 {
				s.mu.Unlock()
				continue
			}
			datagram = s.queues[key][0]
			s.queues[key] = s.queues[key][1:]
			s.mu.Unlock()
		}
		used += len(datagram.Payload)
		out = append(out, datagram)
	}
	return out, nil
}
