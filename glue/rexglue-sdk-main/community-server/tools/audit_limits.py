#!/usr/bin/env python3
"""Fail when arithmetic-derived constants drift from their reviewed Python derivation."""
from pathlib import Path
import base64

root = Path(__file__).resolve().parents[1]
sources = {
    "api": (root / "internal/httpapi/api.go").read_text(),
    "config": (root / "internal/config/config.go").read_text(),
    "relay": (root / "internal/relay/relay.go").read_text(),
    "resources": (root / "internal/httpapi/resources.go").read_text(),
    "typed": (root / "internal/httpapi/typed_services.go").read_text(),
    "store": (root / "internal/store/postgres.go").read_text(),
}

derived = {
    "body": 1 << 20,
    "members": 1 << 6,
    "property": 1 << 9,
    "property_base64": len(base64.b64encode(bytes(1 << 9))),
    "udp": ((1 << 16) - 1) - 20 - 8,
    "queue": 1 << 8,
    "allocator_attempts": 1 << 10,
    "rate_keys": 10_000,
    "outbox_lease": 30,
    "idempotency_reservation": 2 * 60,
    "idempotency_result_hours": 24,
    "invite_recipients": 1 << 5,
    "voice_packet": 1 << 12,
    "voice_response": 1 << 20,
    "datagram_send_batch": 1 << 6,
    "datagram_encoded_payload_sum": (1 << 20) - (1 << 12) - ((1 << 6) * (1 << 9)),
    "maintenance_interval_seconds": 60,
    "ticket_retention_seconds": 10 * 60,
    "delivered_event_retention_seconds": 30 * 24 * 60 * 60,
    "presence_ttl_seconds": 90,
    "voice_batch_packets": 1 << 5,
    "voice_batch_body_bytes": 195_328,
    "voice_route_ttl_seconds": 2 * 60 * 60,
    "access_ttl_min_seconds": 60,
    "access_ttl_max_seconds": 24 * 60 * 60,
    "refresh_ttl_min_seconds": 60 * 60,
    "refresh_ttl_max_seconds": 365 * 24 * 60 * 60,
    "challenge_ttl_min_seconds": 30,
    "challenge_ttl_max_seconds": 15 * 60,
    "session_lease_min_seconds": 10,
    "session_lease_max_seconds": 5 * 60,
    "rate_limit_max_per_minute": 100_000,
}

assert 'integer("COMMUNITY_MAX_SESSION_MEMBERS", 64)' in sources["config"]
assert '1<<20' in sources["api"]
assert 'len(payload) > 65507' in sources["resources"]
assert 'len(s.queues[key]) > 256' in sources["relay"]
assert 'attempt < 1024' in sources["relay"]
assert 'const maxInMemoryRateEntries = 10_000' in sources["api"]
assert "interval '30 seconds'" in sources["store"]
assert 'time.Now().Add(2*time.Minute)' in sources["api"]
assert 'time.Now().Add(24*time.Hour)' in sources["api"]
assert 'len(request.RecipientXUIDs) > 32' in sources["typed"]
assert 'decodeBase64(request.Payload, 4096)' in sources["typed"]
assert 'responseBytes+len(event.Payload) > 1<<20' in sources["typed"]
assert 'len(datagrams) > 64' in sources["relay"]
assert 'encodedBytes > 1011712' in sources["resources"]
assert 'time.NewTicker(time.Minute)' in (root / "internal/roles/roles.go").read_text()
assert 'now.Add(-10 * time.Minute)' in sources["store"]
assert 'now.Add(-30 * 24 * time.Hour)' in sources["store"]
assert 'Add(90 * time.Second)' in (root / "internal/httpapi/parity.go").read_text()
assert 'len(raw) > 195328' in sources["typed"]
assert 'len(packets) > 32' in sources["typed"]
assert 'Add(2 * time.Hour)' in sources["typed"]
assert 'c.AccessTTL < time.Minute || c.AccessTTL > 24*time.Hour' in sources["config"]
assert 'c.RefreshTTL < time.Hour || c.RefreshTTL > 365*24*time.Hour' in sources["config"]
assert 'c.ChallengeTTL < 30*time.Second || c.ChallengeTTL > 15*time.Minute' in sources["config"]
assert 'c.SessionLeaseTTL < 10*time.Second || c.SessionLeaseTTL > 5*time.Minute' in sources["config"]
assert 'c.RateLimitPerMin < 1 || c.RateLimitPerMin > 100000' in sources["config"]
assert derived == {
    "body": 1048576,
    "members": 64,
    "property": 512,
    "property_base64": 684,
    "udp": 65507,
    "queue": 256,
    "allocator_attempts": 1024,
    "rate_keys": 10000,
    "outbox_lease": 30,
    "idempotency_reservation": 120,
    "idempotency_result_hours": 24,
    "invite_recipients": 32,
    "voice_packet": 4096,
    "voice_response": 1048576,
    "datagram_send_batch": 64,
    "datagram_encoded_payload_sum": 1011712,
    "maintenance_interval_seconds": 60,
    "ticket_retention_seconds": 600,
    "delivered_event_retention_seconds": 2592000,
    "presence_ttl_seconds": 90,
    "voice_batch_packets": 32,
    "voice_batch_body_bytes": 195328,
    "voice_route_ttl_seconds": 7200,
    "access_ttl_min_seconds": 60,
    "access_ttl_max_seconds": 86400,
    "refresh_ttl_min_seconds": 3600,
    "refresh_ttl_max_seconds": 31536000,
    "challenge_ttl_min_seconds": 30,
    "challenge_ttl_max_seconds": 900,
    "session_lease_min_seconds": 10,
    "session_lease_max_seconds": 300,
    "rate_limit_max_per_minute": 100000,
}
print("all arithmetic-derived service limits match")
