package config

import (
	"errors"
	"fmt"
	"net/netip"
	"net/url"
	"os"
	"strconv"
	"strings"
	"time"
)

type Config struct {
	Role              string
	ListenAddress     string
	PublicURL         string
	DatabaseURL       string
	RedisURL          string
	TokenSigningKey   []byte
	AccessTTL         time.Duration
	RefreshTTL        time.Duration
	ChallengeTTL      time.Duration
	SessionLeaseTTL   time.Duration
	MaxSessionMembers int
	RateLimitPerMin   int
	AllowInMemory     bool
	TrustedProxies    []netip.Prefix
}

func Load() (Config, error) {
	publicURL, err := parsePublicURL(env("COMMUNITY_PUBLIC_URL", "http://localhost:8080"))
	if err != nil {
		return Config{}, err
	}
	trustedProxies, err := parseTrustedProxies(os.Getenv("COMMUNITY_TRUSTED_PROXIES"))
	if err != nil {
		return Config{}, err
	}
	accessTTL, err := duration("COMMUNITY_ACCESS_TTL", 15*time.Minute)
	if err != nil {
		return Config{}, err
	}
	refreshTTL, err := duration("COMMUNITY_REFRESH_TTL", 30*24*time.Hour)
	if err != nil {
		return Config{}, err
	}
	challengeTTL, err := duration("COMMUNITY_CHALLENGE_TTL", 5*time.Minute)
	if err != nil {
		return Config{}, err
	}
	sessionLeaseTTL, err := duration("COMMUNITY_SESSION_LEASE_TTL", 45*time.Second)
	if err != nil {
		return Config{}, err
	}
	maxSessionMembers, err := integer("COMMUNITY_MAX_SESSION_MEMBERS", 64)
	if err != nil {
		return Config{}, err
	}
	rateLimit, err := integer("COMMUNITY_RATE_LIMIT_PER_MINUTE", 120)
	if err != nil {
		return Config{}, err
	}
	allowInMemory, err := boolean("COMMUNITY_ALLOW_IN_MEMORY", false)
	if err != nil {
		return Config{}, err
	}
	c := Config{
		Role:              env("COMMUNITY_ROLE", "api"),
		ListenAddress:     env("COMMUNITY_LISTEN", ":8080"),
		PublicURL:         publicURL,
		DatabaseURL:       os.Getenv("COMMUNITY_DATABASE_URL"),
		RedisURL:          os.Getenv("COMMUNITY_REDIS_URL"),
		TokenSigningKey:   []byte(os.Getenv("COMMUNITY_TOKEN_SIGNING_KEY")),
		AccessTTL:         accessTTL,
		RefreshTTL:        refreshTTL,
		ChallengeTTL:      challengeTTL,
		SessionLeaseTTL:   sessionLeaseTTL,
		MaxSessionMembers: maxSessionMembers,
		RateLimitPerMin:   rateLimit,
		AllowInMemory:     allowInMemory,
		TrustedProxies:    trustedProxies,
	}
	if c.Role != "api" && c.Role != "matchmaker" && c.Role != "worker" {
		return Config{}, fmt.Errorf("unsupported COMMUNITY_ROLE %q", c.Role)
	}
	if len(c.TokenSigningKey) < 32 || string(c.TokenSigningKey) == "replace-with-at-least-32-random-bytes" {
		return Config{}, errors.New("COMMUNITY_TOKEN_SIGNING_KEY must contain at least 32 bytes")
	}
	if c.MaxSessionMembers < 2 || c.MaxSessionMembers > 64 {
		return Config{}, errors.New("COMMUNITY_MAX_SESSION_MEMBERS must be in [2,64]")
	}
	if c.AccessTTL < time.Minute || c.AccessTTL > 24*time.Hour {
		return Config{}, errors.New("COMMUNITY_ACCESS_TTL must be in [1m,24h]")
	}
	if c.RefreshTTL < time.Hour || c.RefreshTTL > 365*24*time.Hour {
		return Config{}, errors.New("COMMUNITY_REFRESH_TTL must be in [1h,8760h]")
	}
	if c.ChallengeTTL < 30*time.Second || c.ChallengeTTL > 15*time.Minute {
		return Config{}, errors.New("COMMUNITY_CHALLENGE_TTL must be in [30s,15m]")
	}
	if c.SessionLeaseTTL < 10*time.Second || c.SessionLeaseTTL > 5*time.Minute {
		return Config{}, errors.New("COMMUNITY_SESSION_LEASE_TTL must be in [10s,5m]")
	}
	if c.RateLimitPerMin < 1 || c.RateLimitPerMin > 100000 {
		return Config{}, errors.New("COMMUNITY_RATE_LIMIT_PER_MINUTE must be in [1,100000]")
	}
	if c.DatabaseURL == "" && !c.AllowInMemory {
		return Config{}, errors.New("COMMUNITY_DATABASE_URL is required unless COMMUNITY_ALLOW_IN_MEMORY=true")
	}
	return c, nil
}

func parsePublicURL(value string) (string, error) {
	parsed, err := url.Parse(value)
	if err != nil || (parsed.Scheme != "http" && parsed.Scheme != "https") || parsed.Host == "" {
		return "", errors.New("COMMUNITY_PUBLIC_URL must be an absolute http or https URL")
	}
	if parsed.Opaque != "" || parsed.User != nil || parsed.ForceQuery || parsed.RawQuery != "" || parsed.Fragment != "" || parsed.RawPath != "" || (parsed.Path != "" && parsed.Path != "/") {
		return "", errors.New("COMMUNITY_PUBLIC_URL must contain only a scheme and authority")
	}
	if parsed.Hostname() == "" {
		return "", errors.New("COMMUNITY_PUBLIC_URL must contain a valid host")
	}
	parsed.Path = ""
	return parsed.String(), nil
}

func parseTrustedProxies(value string) ([]netip.Prefix, error) {
	if strings.TrimSpace(value) == "" {
		return nil, nil
	}
	parts := strings.Split(value, ",")
	proxies := make([]netip.Prefix, 0, len(parts))
	seen := make(map[netip.Prefix]struct{}, len(parts))
	for _, part := range parts {
		candidate := strings.TrimSpace(part)
		prefix, err := netip.ParsePrefix(candidate)
		if err != nil || prefix.Addr().Is4In6() || prefix != prefix.Masked() {
			return nil, fmt.Errorf("COMMUNITY_TRUSTED_PROXIES entry %q must be a canonical CIDR", candidate)
		}
		if _, exists := seen[prefix]; exists {
			return nil, fmt.Errorf("COMMUNITY_TRUSTED_PROXIES entry %q is duplicated", candidate)
		}
		seen[prefix] = struct{}{}
		proxies = append(proxies, prefix)
	}
	return proxies, nil
}

func env(name, fallback string) string {
	if value := os.Getenv(name); value != "" {
		return value
	}
	return fallback
}

func duration(name string, fallback time.Duration) (time.Duration, error) {
	value := os.Getenv(name)
	if value == "" {
		return fallback, nil
	}
	parsed, err := time.ParseDuration(value)
	if err != nil {
		return 0, fmt.Errorf("%s must be a valid duration: %w", name, err)
	}
	return parsed, nil
}

func integer(name string, fallback int) (int, error) {
	value := os.Getenv(name)
	if value == "" {
		return fallback, nil
	}
	parsed, err := strconv.Atoi(value)
	if err != nil {
		return 0, fmt.Errorf("%s must be an integer: %w", name, err)
	}
	return parsed, nil
}

func boolean(name string, fallback bool) (bool, error) {
	value := os.Getenv(name)
	if value == "" {
		return fallback, nil
	}
	parsed, err := strconv.ParseBool(value)
	if err != nil {
		return false, fmt.Errorf("%s must be a boolean: %w", name, err)
	}
	return parsed, nil
}
