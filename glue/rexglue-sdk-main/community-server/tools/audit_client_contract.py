#!/usr/bin/env python3
"""Fail-closed static audit of the native community client/server HTTP contract."""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CLIENT_PATH = ROOT / "gta4-recomp/src/network/community_multiplayer.cpp"
SERVER_ROOT = ROOT / "community-server"
ROUTES_PATH = SERVER_ROOT / "internal/httpapi/api.go"
OPENAPI_PATH = SERVER_ROOT / "api/openapi.yaml"


@dataclass(frozen=True, order=True)
class Endpoint:
    method: str
    path: str


CLIENT_ENDPOINTS = {
    Endpoint("POST", "/api/v2/devices/challenge"),
    Endpoint("POST", "/api/v2/devices/enroll"),
    Endpoint("POST", "/api/v2/devices/refresh"),
    Endpoint("GET", "/api/v2/friends"),
    Endpoint("POST", "/api/v2/friends/check"),
    Endpoint("GET", "/api/v2/invites"),
    Endpoint("POST", "/api/v2/invites"),
    Endpoint("POST", "/api/v2/invites/{id}/accept"),
    Endpoint("GET", "/api/v2/leaderboards/{view_id}"),
    Endpoint("DELETE", "/api/v2/relay/routes"),
    Endpoint("POST", "/api/v2/relay/routes"),
    Endpoint("GET", "/api/v2/relay/datagrams"),
    Endpoint("POST", "/api/v2/relay/datagrams"),
    Endpoint("POST", "/api/v2/sessions"),
    Endpoint("POST", "/api/v2/sessions/search"),
    Endpoint("GET", "/api/v2/sessions/by-xbox-id/{xbox_id}"),
    Endpoint("PATCH", "/api/v2/sessions/{id}"),
    Endpoint("DELETE", "/api/v2/sessions/{id}"),
    Endpoint("POST", "/api/v2/sessions/{id}/heartbeat"),
    Endpoint("POST", "/api/v2/sessions/{id}/join"),
    Endpoint("POST", "/api/v2/sessions/{id}/leave"),
    Endpoint("POST", "/api/v2/sessions/{id}/migration"),
    Endpoint("POST", "/api/v2/stats/read"),
    Endpoint("POST", "/api/v2/stats/reset"),
    Endpoint("POST", "/api/v2/stats/skill"),
    Endpoint("POST", "/api/v2/stats/writes"),
    Endpoint("GET", "/api/v2/voice/packets"),
    Endpoint("POST", "/api/v2/voice/packets"),
    Endpoint("DELETE", "/api/v2/voice/routes"),
    Endpoint("POST", "/api/v2/voice/routes"),
}


# Every API literal in the client must belong to one classified endpoint family.
CLIENT_LITERAL_PREFIXES = (
    "/api/v2/devices/challenge",
    "/api/v2/devices/enroll",
    "/api/v2/devices/refresh",
    "/api/v2/friends",
    "/api/v2/invites",
    "/api/v2/leaderboards/",
    "/api/v2/relay/datagrams",
    "/api/v2/relay/routes",
    "/api/v2/sessions",
    "/api/v2/stats/read",
    "/api/v2/stats/reset",
    "/api/v2/stats/skill",
    "/api/v2/stats/writes",
    "/api/v2/voice/packets",
    "/api/v2/voice/routes",
)


CLIENT_EVIDENCE = {
    Endpoint("GET", "/api/v2/friends"): '"GET","/api/v2/friends?limit="',
    Endpoint("GET", "/api/v2/invites"): '"GET","/api/v2/invites?limit=1&state=pending"',
    Endpoint("POST", "/api/v2/invites/{id}/accept"): '"POST","/api/v2/invites/"+invitation->id+"/accept"',
    Endpoint("GET", "/api/v2/leaderboards/{view_id}"): '"/api/v2/leaderboards/"+Hex32(view_id)',
    Endpoint("GET", "/api/v2/relay/datagrams"): '"GET",path,nullptr,true,{},kRelayPollTimeoutMilliseconds',
    Endpoint("GET", "/api/v2/sessions/by-xbox-id/{xbox_id}"): '"GET","/api/v2/sessions/by-xbox-id/"+Hex64(session_id)',
    Endpoint("PATCH", "/api/v2/sessions/{id}"): '"PATCH","/api/v2/sessions/"+Hex64(session.session_id)',
    Endpoint("DELETE", "/api/v2/sessions/{id}"): '"DELETE","/api/v2/sessions/"+Hex64(session_id)',
    Endpoint("POST", "/api/v2/sessions/{id}/heartbeat"): 'Action(session.session_id,"heartbeat",body)',
    Endpoint("POST", "/api/v2/sessions/{id}/join"): 'Action(session_id,"join",body)',
    Endpoint("POST", "/api/v2/sessions/{id}/leave"): 'Action(session_id,"leave",body)',
    Endpoint("POST", "/api/v2/sessions/{id}/migration"): 'Action(session_id,"migration",body)',
    Endpoint("GET", "/api/v2/voice/packets"): '"GET",path,nullptr,true',
}


IDEMPOTENT_CLIENT_ENDPOINTS = {
    Endpoint("POST", "/api/v2/invites"),
    Endpoint("POST", "/api/v2/invites/{id}/accept"),
    Endpoint("POST", "/api/v2/sessions"),
    Endpoint("PATCH", "/api/v2/sessions/{id}"),
    Endpoint("DELETE", "/api/v2/sessions/{id}"),
    Endpoint("POST", "/api/v2/sessions/{id}/heartbeat"),
    Endpoint("POST", "/api/v2/sessions/{id}/join"),
    Endpoint("POST", "/api/v2/sessions/{id}/leave"),
    Endpoint("POST", "/api/v2/sessions/{id}/migration"),
    Endpoint("POST", "/api/v2/stats/reset"),
    Endpoint("POST", "/api/v2/stats/writes"),
}


def compact(text: str) -> str:
    return re.sub(r"\s+", "", text)


def parse_server_routes(text: str) -> set[Endpoint]:
    routes = {
        Endpoint(method, path)
        for method, path in re.findall(r'"(GET|POST|PUT|PATCH|DELETE) (/api/v2/[^"+]+)', text)
    }
    action_match = re.search(r'\[\]string\{([^}]+)\}.*?/api/v2/sessions/\{id\}/"\+action', text, re.S)
    if action_match:
        for action in re.findall(r'"([a-z-]+)"', action_match.group(1)):
            routes.add(Endpoint("POST", f"/api/v2/sessions/{{id}}/{action}"))
        routes.discard(Endpoint("POST", "/api/v2/sessions/{id}/"))
    return routes


def parse_openapi_paths(text: str) -> set[Endpoint]:
    paths: set[Endpoint] = set()
    current = ""
    in_paths = False
    for line in text.splitlines():
        if line == "paths:":
            in_paths = True
            continue
        if in_paths and line == "components:":
            break
        path_match = re.match(r"^  (/api/v2/[^:]+):", line)
        if path_match:
            current = path_match.group(1)
            continue
        method_match = re.match(r"^    (get|post|put|patch|delete):", line)
        if current and method_match:
            paths.add(Endpoint(method_match.group(1).upper(), current))
        if current and re.match(r"^    \$ref: ['\"]?#/components/pathItems/SessionAction", line):
            paths.add(Endpoint("POST", current))
    return paths


def client_evidence(endpoint: Endpoint) -> str:
    if endpoint in CLIENT_EVIDENCE:
        return CLIENT_EVIDENCE[endpoint]
    if "{" in endpoint.path:
        raise KeyError(endpoint)
    return f'"{endpoint.method}","{endpoint.path}"'


def require_contains(errors: list[str], text: str, needle: str, label: str) -> None:
    if compact(needle) not in compact(text):
        errors.append(f"{label}: missing `{needle}`")


def require_any(errors: list[str], text: str, needles: tuple[str, ...], label: str) -> None:
    haystack = compact(text)
    if not any(compact(needle) in haystack for needle in needles):
        errors.append(f"{label}: none of {needles!r} found")


def endpoint_text(endpoint: Endpoint) -> str:
    return f"{endpoint.method} {endpoint.path}"


def main() -> int:
    errors: list[str] = []
    client = CLIENT_PATH.read_text()
    routes_text = ROUTES_PATH.read_text()
    openapi = OPENAPI_PATH.read_text()
    go_files = sorted((SERVER_ROOT / "internal/httpapi").glob("*.go"))
    handlers = "\n".join(path.read_text() for path in go_files if not path.name.endswith("_test.go"))
    config = (SERVER_ROOT / "internal/config/config.go").read_text()
    relay = (SERVER_ROOT / "internal/relay/relay.go").read_text()
    store = (SERVER_ROOT / "internal/store/store.go").read_text()
    postgres = (SERVER_ROOT / "internal/store/postgres.go").read_text()
    compose = (SERVER_ROOT / "compose.yaml").read_text()
    main_go = (SERVER_ROOT / "cmd/community-server/main.go").read_text()

    client_compact = compact(client)
    for endpoint in sorted(CLIENT_ENDPOINTS):
        evidence = compact(client_evidence(endpoint))
        if evidence not in client_compact:
            errors.append(f"client endpoint classification is stale: {endpoint_text(endpoint)}")

    literals = sorted(set(re.findall(r'"(/api/v2/[^" ]*)"', client)))
    unknown_literals = [
        literal
        for literal in literals
        if not any(literal.startswith(prefix) for prefix in CLIENT_LITERAL_PREFIXES)
    ]
    for literal in unknown_literals:
        errors.append(f"unclassified client API literal: {literal}")

    server_routes = parse_server_routes(routes_text)
    openapi_paths = parse_openapi_paths(openapi)
    for endpoint in sorted(CLIENT_ENDPOINTS - server_routes):
        errors.append(f"server route missing for client endpoint: {endpoint_text(endpoint)}")
    for endpoint in sorted(CLIENT_ENDPOINTS - openapi_paths):
        errors.append(f"OpenAPI operation missing for client endpoint: {endpoint_text(endpoint)}")

    # The published API must not describe operations that the binary cannot serve, and all
    # runtime v2 operations must be published. This catches stale generic resource endpoints.
    for endpoint in sorted(openapi_paths - server_routes):
        errors.append(f"OpenAPI operation has no runtime route: {endpoint_text(endpoint)}")
    for endpoint in sorted(server_routes - openapi_paths):
        errors.append(f"runtime route is absent from OpenAPI: {endpoint_text(endpoint)}")

    # Authentication request/response shapes and status expectations.
    for field in ("device_id", "xuid", "public_key"):
        require_contains(errors, client, f'{{"{field}"', f"challenge client field {field}")
        require_contains(errors, handlers, f'json:"{field}"', f"challenge server field {field}")
    for field in (
        "challenge_id",
        "device_id",
        "xuid",
        "machine_id",
        "player_name",
        "public_key",
        "signature",
    ):
        require_contains(errors, client, f'{{"{field}"', f"enroll client field {field}")
        require_contains(errors, openapi, field, f"enroll OpenAPI field {field}")
    for field in ("access_token", "refresh_token", "expires_in"):
        require_contains(errors, client, f'"{field}"', f"token client response field {field}")
        require_contains(errors, openapi, field, f"token OpenAPI field {field}")
    require_contains(errors, client, "challenge_response.status != 201", "challenge status")
    require_contains(errors, client, "response.status != 201 || !AcceptTokens", "enroll status")

    # Exact identifier and stats wire rules: lowercase fixed-width Hex32/Hex64 and decimal
    # strings for 64-bit sequence values, so JSON number precision cannot be lost.
    require_contains(errors, client, '"0x%08x"', "client Hex32 formatter")
    require_contains(errors, client, '"0x%016llx"', "client Hex64 formatter")
    require_contains(errors, handlers, r"^0x[0-9a-f]{8}$", "server Hex32 validator")
    require_contains(errors, handlers, r"^0x[0-9a-f]{16}$", "server Hex64 validator")
    require_contains(errors, openapi, r"^0x[0-9a-f]{8}$", "OpenAPI Hex32 validator")
    require_contains(errors, openapi, r"^0x[0-9a-fA-F]{16}$", "OpenAPI Hex64 validator")
    require_contains(errors, client, '{"sequence",std::to_string(sequence)}', "stats string sequence")
    require_contains(errors, handlers, 'Sequence string `json:"sequence"`', "server string sequence")
    require_contains(errors, openapi, "sequence: {type: string", "OpenAPI string sequence")
    require_contains(errors, handlers, "PutDocumentsAtomic", "atomic stats write")
    require_contains(errors, handlers, "input.XUID != actor.XUID", "caller-owned stats writes")
    require_contains(errors, client, '"POST","/api/v2/stats/reset"', "client stats reset route")
    require_contains(errors, handlers, 'GetDevice(r.Context(),claims(r).DeviceID)', "caller-owned stats reset")

    # Relay batches and response shape.
    require_contains(errors, client, 'const json batch={{"datagrams",std::move(datagrams)}}', "relay batch envelope")
    require_contains(errors, handlers, 'Datagrams []relay.Datagram `json:"datagrams"`', "relay server batch envelope")
    require_contains(errors, handlers, '"accepted_count":len(datagrams)', "relay acceptance count")
    require_contains(errors, openapi, "DatagramBatch", "relay OpenAPI batch")
    require_contains(errors, client, "kMaximumRelayBatchDatagrams=64", "client relay batch limit")
    require_contains(errors, handlers, "len(datagrams)>64", "server relay batch limit")
    require_contains(errors, openapi, "maxItems: 64", "OpenAPI batch limit")

    # Voice is intentionally stricter than the legacy single-packet endpoint: the native client
    # sends batches, receives flat packet records, advances an opaque cursor, and deletes routes.
    require_contains(errors, client, 'const json batch={{"packets",std::move(packets)}}', "voice batch envelope")
    require_contains(errors, handlers, 'Packets []', "voice server batch envelope")
    require_contains(errors, handlers, 'json:"packets"', "voice server batch field")
    require_contains(errors, openapi, "VoicePacketBatch", "voice OpenAPI batch")
    require_contains(errors, client, '"DELETE","/api/v2/voice/routes"', "voice route close")
    require_contains(errors, handlers, 'RouteToken string `json:"route_token"`', "voice route token request")
    for field in ("source_xuid", "session_id", "sequence", "payload"):
        require_contains(errors, client, f'wire.at("{field}")', f"voice client packet field {field}")
        require_contains(errors, handlers, f'"{field}"', f"voice server packet field {field}")
        require_contains(errors, openapi, field, f"voice OpenAPI packet field {field}")
    require_contains(errors, client, 'decoded.value("next_after",std::string{})', "voice opaque cursor response")
    require_contains(errors, handlers, '"next_after":next', "voice opaque cursor server")
    require_contains(errors, openapi, "next_after", "voice opaque cursor OpenAPI")
    require_contains(errors, openapi, "Opaque", "voice cursor OpenAPI description")

    # Idempotency and optimistic concurrency must agree at all client mutations that use them.
    for endpoint in sorted(IDEMPOTENT_CLIENT_ENDPOINTS):
        if endpoint not in openapi_paths:
            continue
        path_start = openapi.find(f"  {endpoint.path}:")
        if path_start < 0:
            errors.append(f"idempotency OpenAPI path missing: {endpoint_text(endpoint)}")
            continue
        path_end_match = re.search(r"\n  /api/|\ncomponents:", openapi[path_start + 1 :])
        path_end = len(openapi) if not path_end_match else path_start + 1 + path_end_match.start()
        block = openapi[path_start:path_end]
        if endpoint.path.startswith("/api/v2/sessions/{id}/"):
            if "SessionAction" not in block or "IdempotencyKey" not in openapi:
                errors.append(f"OpenAPI lacks idempotency contract: {endpoint_text(endpoint)}")
        elif "IdempotencyKey" not in block:
            errors.append(f"OpenAPI lacks idempotency contract: {endpoint_text(endpoint)}")
    require_contains(errors, handlers, "ReserveIdempotency", "server atomic idempotency reservation")
    require_contains(errors, handlers, "FinalizeIdempotency", "server idempotency finalization")
    require_contains(errors, store, "IdempotencyInProgress", "idempotency in-progress state")
    require_contains(errors, postgres, "ON CONFLICT(subject,method,path,idempotency_key)", "cross-replica idempotency key")
    require_contains(errors, client, '"If-Match:"+std::to_string(metadata->revision)', "client session If-Match")
    require_contains(errors, handlers, "parseRevision(r)", "server session revision check")
    require_contains(errors, openapi, "IfMatch", "OpenAPI session revision header")

    # The host sends its complete 64-member roster on PATCH. Unknown-field rejection makes a
    # missing members field a hard client failure, while blindly accepting it would allow roster
    # ownership forgery. The handler must route it through the roster sanitizer.
    require_contains(errors, client, '{"members",full["members"]}', "client session roster patch")
    require_contains(
        errors,
        handlers,
        'Members []domain.SessionMember `json:"members,omitempty"`',
        "server session roster patch field",
    )
    require_contains(errors, handlers, "sanitizeRoster(r,session", "server roster patch sanitizer")
    session_patch_start = openapi.find("SessionPatch:")
    session_action_start = openapi.find("SessionAction:", session_patch_start)
    session_patch_openapi = openapi[session_patch_start:session_action_start]
    require_contains(errors, session_patch_openapi, "members:", "OpenAPI session roster patch")

    # Read-only deployment/security/scalability guardrails used by the public fan-server image.
    for needle, label in (
        ("COMMUNITY_DATABASE_URL", "compose PostgreSQL configuration"),
        ("COMMUNITY_REDIS_URL", "compose Redis configuration"),
        ("COMMUNITY_PUBLIC_URL", "compose public URL"),
        ("COMMUNITY_TRUSTED_PROXIES", "compose trusted proxies"),
        ("read_only: true", "read-only application container"),
        ("no-new-privileges:true", "container privilege hardening"),
    ):
        require_contains(errors, compose, needle, label)
    require_contains(errors, config, "TrustedProxies []netip.Prefix", "trusted proxy CIDRs")
    require_contains(errors, handlers, "isTrustedProxy", "trusted proxy enforcement")
    require_contains(errors, handlers, "http.MaxBytesReader", "HTTP request body limit")
    require_contains(errors, handlers, "a.redis.Incr", "cross-replica rate limiting")
    require_contains(errors, relay, "redis", "cross-replica relay state")
    require_contains(errors, main_go, "ReadHeaderTimeout", "HTTP read-header timeout")
    require_contains(errors, main_go, "ReadTimeout", "HTTP read timeout")
    require_contains(errors, main_go, "WriteTimeout", "HTTP write timeout")
    require_contains(errors, postgres, "FOR UPDATE SKIP LOCKED", "cross-replica outbox leasing")

    print(f"classified client endpoints: {len(CLIENT_ENDPOINTS)}")
    print(f"runtime API v2 routes: {len(server_routes)}")
    print(f"OpenAPI v2 operations: {len(openapi_paths)}")
    print(f"classified client API literals: {len(literals) - len(unknown_literals)}/{len(literals)}")
    if errors:
        print(f"contract audit failed with {len(errors)} finding(s):", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    print("community client/server/OpenAPI/deployment contract audit passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
