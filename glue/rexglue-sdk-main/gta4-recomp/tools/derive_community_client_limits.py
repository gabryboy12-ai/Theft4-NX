#!/usr/bin/env python3
"""Derive fixed wire and buffer limits for the community client.

Run this whenever the server's datagram or roster limits change.  Keeping the
arithmetic here makes the C++ constants auditable without doing size math by
hand.
"""

from pathlib import Path

MAX_DATAGRAM_BYTES = 65507
BASE64_GROUP_INPUT = 3
BASE64_GROUP_OUTPUT = 4
MAX_RELAY_BATCH = 64
PER_DATAGRAM_JSON_OVERHEAD = 512
RELAY_BATCH_ENVELOPE_OVERHEAD = 4096
HTTP_REQUEST_LIMIT = 1024 * 1024
HTTP_RESPONSE_LIMIT = 2 * 1024 * 1024
OUTBOUND_QUEUE_BATCHES = 16
RELAY_BATCH_DELAY_MILLISECONDS = 2
MAX_VOICE_PACKET_BYTES = 4096
MAX_VOICE_BATCH = 32
VOICE_QUEUE_BATCHES = 16
SERVER_ROUTE_TTL_SECONDS = 2 * 60
ROUTE_REFRESH_DIVISOR = 2
ROUTE_RETRY_DIVISOR = 6
PRESENCE_LEASE_SECONDS = 90
PRESENCE_REFRESH_NUMERATOR = 2
PRESENCE_REFRESH_DENOMINATOR = 3
MAX_STAT_VARIABLE_BYTES = 512
UTF16_CODE_UNIT_BYTES = 2
UTF8_MAX_BYTES_PER_BMP_CODEPOINT = 3


def ceil_div(value: int, divisor: int) -> int:
    return (value + divisor - 1) // divisor


max_encoded_datagram = (
    ceil_div(MAX_DATAGRAM_BYTES, BASE64_GROUP_INPUT) * BASE64_GROUP_OUTPUT
)
max_relay_batch_encoded_bytes = (
    HTTP_REQUEST_LIMIT
    - RELAY_BATCH_ENVELOPE_OVERHEAD
    - MAX_RELAY_BATCH * PER_DATAGRAM_JSON_OVERHEAD
)
max_relay_batch_json = (
    max_relay_batch_encoded_bytes
    + MAX_RELAY_BATCH * PER_DATAGRAM_JSON_OVERHEAD
    + RELAY_BATCH_ENVELOPE_OVERHEAD
)
maximum_outbound_datagrams = MAX_RELAY_BATCH * OUTBOUND_QUEUE_BATCHES
maximum_outbound_encoded_bytes = (
    max_relay_batch_encoded_bytes * OUTBOUND_QUEUE_BATCHES
)
route_refresh_seconds = SERVER_ROUTE_TTL_SECONDS // ROUTE_REFRESH_DIVISOR
route_retry_seconds = route_refresh_seconds // ROUTE_RETRY_DIVISOR
presence_refresh_seconds = (
    PRESENCE_LEASE_SECONDS
    * PRESENCE_REFRESH_NUMERATOR
    // PRESENCE_REFRESH_DENOMINATOR
)
max_stat_utf16_units = MAX_STAT_VARIABLE_BYTES // UTF16_CODE_UNIT_BYTES
max_stat_utf8_bytes = max_stat_utf16_units * UTF8_MAX_BYTES_PER_BMP_CODEPOINT
max_encoded_voice_packet = (
    ceil_div(MAX_VOICE_PACKET_BYTES, BASE64_GROUP_INPUT) * BASE64_GROUP_OUTPUT
)
maximum_voice_datagrams = MAX_VOICE_BATCH * VOICE_QUEUE_BATCHES
maximum_voice_encoded_bytes = max_encoded_voice_packet * maximum_voice_datagrams
maximum_voice_batch_json = (
    max_encoded_voice_packet * MAX_VOICE_BATCH
    + PER_DATAGRAM_JSON_OVERHEAD * MAX_VOICE_BATCH
    + RELAY_BATCH_ENVELOPE_OVERHEAD
)

print(f"MAX_DATAGRAM_BYTES={MAX_DATAGRAM_BYTES}")
print(f"MAX_ENCODED_DATAGRAM={max_encoded_datagram}")
print(f"MAX_RELAY_BATCH_JSON={max_relay_batch_json}")
print(f"MAX_RELAY_BATCH_ENCODED_BYTES={max_relay_batch_encoded_bytes}")
print(f"MAXIMUM_OUTBOUND_DATAGRAMS={maximum_outbound_datagrams}")
print(f"MAXIMUM_OUTBOUND_ENCODED_BYTES={maximum_outbound_encoded_bytes}")
print(f"RELAY_BATCH_DELAY_MILLISECONDS={RELAY_BATCH_DELAY_MILLISECONDS}")
print(f"HTTP_REQUEST_LIMIT={HTTP_REQUEST_LIMIT}")
print(f"HTTP_RESPONSE_LIMIT={HTTP_RESPONSE_LIMIT}")
print(f"ROUTE_REFRESH_SECONDS={route_refresh_seconds}")
print(f"ROUTE_RETRY_SECONDS={route_retry_seconds}")
print(f"PRESENCE_REFRESH_SECONDS={presence_refresh_seconds}")
print(f"MAX_STAT_UTF16_UNITS={max_stat_utf16_units}")
print(f"MAX_STAT_UTF8_BYTES={max_stat_utf8_bytes}")
print(f"MAX_ENCODED_VOICE_PACKET={max_encoded_voice_packet}")
print(f"MAXIMUM_VOICE_DATAGRAMS={maximum_voice_datagrams}")
print(f"MAXIMUM_VOICE_ENCODED_BYTES={maximum_voice_encoded_bytes}")
print(f"MAXIMUM_VOICE_BATCH_JSON={maximum_voice_batch_json}")

assert max_encoded_datagram == 87344
assert max_relay_batch_encoded_bytes == 1011712
assert max_relay_batch_json == HTTP_REQUEST_LIMIT
assert maximum_outbound_datagrams == 1024
assert maximum_outbound_encoded_bytes == 16187392
assert max_relay_batch_json < HTTP_RESPONSE_LIMIT
assert route_refresh_seconds == 60
assert route_retry_seconds == 10
assert presence_refresh_seconds == 60
assert max_stat_utf16_units == 256
assert max_stat_utf8_bytes == 768
assert max_encoded_voice_packet == 5464
assert maximum_voice_datagrams == 512
assert maximum_voice_encoded_bytes == 2797568
assert maximum_voice_batch_json == 195328
assert maximum_voice_batch_json < HTTP_REQUEST_LIMIT

source = (
    Path(__file__).resolve().parents[1]
    / "src"
    / "network"
    / "community_multiplayer.cpp"
).read_text(encoding="utf-8")
expected_constants = {
    "kMaximumDatagramBytes": MAX_DATAGRAM_BYTES,
    "kMaximumRelayBatchDatagrams": MAX_RELAY_BATCH,
    "kMaximumRelayBatchEncodedBytes": max_relay_batch_encoded_bytes,
    "kMaximumOutboundDatagrams": maximum_outbound_datagrams,
    "kMaximumOutboundEncodedBytes": maximum_outbound_encoded_bytes,
    "kMaximumVoicePacketBytes": MAX_VOICE_PACKET_BYTES,
    "kMaximumVoiceBatchPackets": MAX_VOICE_BATCH,
    "kMaximumPendingVoicePackets": maximum_voice_datagrams,
    "kMaximumPendingVoiceEncodedBytes": maximum_voice_encoded_bytes,
}
for name, value in expected_constants.items():
    assert f"constexpr size_t {name} = {value};" in source, (name, value)
assert (
    "constexpr auto kRelayBatchDelay = "
    f"std::chrono::milliseconds({RELAY_BATCH_DELAY_MILLISECONDS});"
) in source
assert (
    "constexpr auto kPresenceRefreshInterval = "
    f"std::chrono::seconds({presence_refresh_seconds});"
) in source
