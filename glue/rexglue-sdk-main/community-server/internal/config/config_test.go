package config

import "testing"

func setValidEnvironment(t *testing.T) {
	t.Helper()
	t.Setenv("COMMUNITY_ROLE", "api")
	t.Setenv("COMMUNITY_LISTEN", ":8080")
	t.Setenv("COMMUNITY_PUBLIC_URL", "https://community.example.test")
	t.Setenv("COMMUNITY_DATABASE_URL", "")
	t.Setenv("COMMUNITY_REDIS_URL", "")
	t.Setenv("COMMUNITY_TOKEN_SIGNING_KEY", "0123456789abcdef0123456789abcdef")
	t.Setenv("COMMUNITY_ACCESS_TTL", "")
	t.Setenv("COMMUNITY_REFRESH_TTL", "")
	t.Setenv("COMMUNITY_CHALLENGE_TTL", "")
	t.Setenv("COMMUNITY_SESSION_LEASE_TTL", "")
	t.Setenv("COMMUNITY_MAX_SESSION_MEMBERS", "")
	t.Setenv("COMMUNITY_RATE_LIMIT_PER_MINUTE", "")
	t.Setenv("COMMUNITY_ALLOW_IN_MEMORY", "true")
	t.Setenv("COMMUNITY_TRUSTED_PROXIES", "")
}

func TestLoadValidatesAndNormalizesPublicURL(t *testing.T) {
	setValidEnvironment(t)
	t.Setenv("COMMUNITY_PUBLIC_URL", "https://community.example.test/")

	loaded, err := Load()
	if err != nil {
		t.Fatal(err)
	}
	if loaded.PublicURL != "https://community.example.test" {
		t.Fatalf("PublicURL = %q", loaded.PublicURL)
	}

	invalid := []string{
		"ftp://community.example.test",
		"https://user@community.example.test",
		"https://community.example.test/api",
		"https://community.example.test?debug=true",
		"https://community.example.test?",
		"https://community.example.test/#fragment",
		"not-a-url",
	}
	for _, value := range invalid {
		t.Run(value, func(t *testing.T) {
			setValidEnvironment(t)
			t.Setenv("COMMUNITY_PUBLIC_URL", value)
			if _, err := Load(); err == nil {
				t.Fatalf("Load accepted COMMUNITY_PUBLIC_URL %q", value)
			}
		})
	}
}

func TestLoadRequiresCanonicalTrustedProxyCIDRs(t *testing.T) {
	setValidEnvironment(t)
	t.Setenv("COMMUNITY_TRUSTED_PROXIES", "172.30.0.0/24, 2001:db8::/32")

	loaded, err := Load()
	if err != nil {
		t.Fatal(err)
	}
	if len(loaded.TrustedProxies) != 2 {
		t.Fatalf("TrustedProxies count = %d", len(loaded.TrustedProxies))
	}

	invalid := []string{
		"172.30.0.1",
		"172.30.0.1/24",
		"172.30.0.0/24,",
		"::ffff:172.30.0.0/120",
		"172.30.0.0/24,172.30.0.0/24",
	}
	for _, value := range invalid {
		t.Run(value, func(t *testing.T) {
			setValidEnvironment(t)
			t.Setenv("COMMUNITY_TRUSTED_PROXIES", value)
			if _, err := Load(); err == nil {
				t.Fatalf("Load accepted COMMUNITY_TRUSTED_PROXIES %q", value)
			}
		})
	}
}

func TestLoadRejectsRemovedRealtimeRole(t *testing.T) {
	setValidEnvironment(t)
	t.Setenv("COMMUNITY_ROLE", "realtime")
	if _, err := Load(); err == nil {
		t.Fatal("Load accepted removed realtime role")
	}
}

func TestLoadRejectsMalformedAndUnsafeOperationalValues(t *testing.T) {
	cases := map[string]string{
		"COMMUNITY_ACCESS_TTL":               "not-a-duration",
		"COMMUNITY_REFRESH_TTL":              "0s",
		"COMMUNITY_CHALLENGE_TTL":            "29s",
		"COMMUNITY_SESSION_LEASE_TTL":        "6m",
		"COMMUNITY_MAX_SESSION_MEMBERS":      "sixty-four",
		"COMMUNITY_RATE_LIMIT_PER_MINUTE":    "0",
		"COMMUNITY_ALLOW_IN_MEMORY":          "sometimes",
	}
	for name, value := range cases {
		t.Run(name, func(t *testing.T) {
			setValidEnvironment(t)
			t.Setenv(name, value)
			if _, err := Load(); err == nil {
				t.Fatalf("Load accepted %s=%q", name, value)
			}
		})
	}
}

func TestLoadRejectsPublishedSigningKeyPlaceholder(t *testing.T) {
	setValidEnvironment(t)
	t.Setenv("COMMUNITY_TOKEN_SIGNING_KEY", "replace-with-at-least-32-random-bytes")
	if _, err := Load(); err == nil {
		t.Fatal("Load accepted the published signing-key placeholder")
	}
}
