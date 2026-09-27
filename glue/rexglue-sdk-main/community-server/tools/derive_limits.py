#!/usr/bin/env python3
"""Derive every arithmetic-dependent service limit; do not hand-calculate these values."""
import base64
import json

ipv4_header_bytes = 20
udp_header_bytes = 8
ipv4_total_length_max = (1 << 16) - 1
udp_payload_max = ipv4_total_length_max - ipv4_header_bytes - udp_header_bytes
request_body_bytes = 1 << 20
property_decoded_bytes = 1 << 9
property_base64_bytes = len(base64.b64encode(bytes(property_decoded_bytes)))
session_members = 1 << 6
relay_queue_datagrams = 1 << 8
relay_allocator_attempts = 1 << 10
virtual_ipv4_host_bits = 24
virtual_ipv4_pool = 1 << virtual_ipv4_host_bits
event_replay_batch = 1 << 8
datagram_receive_batch = 1 << 6
datagram_send_batch = 1 << 6
datagram_json_reserve_bytes = 1 << 9
datagram_envelope_reserve_bytes = 1 << 12
datagram_encoded_payload_sum = request_body_bytes - datagram_envelope_reserve_bytes - (datagram_send_batch * datagram_json_reserve_bytes)
relay_queue_ttl_seconds = 2 * 60
outbox_lease_seconds = 30
worker_tick_milliseconds = 1_000 // 4
long_poll_max_milliseconds = 30 * 1_000
rate_window_seconds = 60
rate_key_cap = 10_000
idempotency_reservation_seconds = 2 * 60
idempotency_result_hours = 24
maintenance_interval_seconds = 60
ticket_retention_seconds = 10 * 60
delivered_event_retention_seconds = 30 * 24 * 60 * 60
presence_ttl_seconds = 90
access_ttl_min_seconds = 60
access_ttl_max_seconds = 24 * 60 * 60
refresh_ttl_min_seconds = 60 * 60
refresh_ttl_max_seconds = 365 * 24 * 60 * 60
challenge_ttl_min_seconds = 30
challenge_ttl_max_seconds = 15 * 60
session_lease_min_seconds = 10
session_lease_max_seconds = 5 * 60
rate_limit_max_per_minute = 100_000
invite_recipients = 1 << 5
voice_packet_decoded_bytes = 1 << 12
voice_response_bytes = 1 << 20
voice_batch_packets = 1 << 5
voice_batch_body_bytes = 195_328
voice_route_ttl_seconds = 2 * 60 * 60

print(json.dumps({
    "datagram_receive_batch": datagram_receive_batch,
    "datagram_send_batch": datagram_send_batch,
    "datagram_json_reserve_bytes": datagram_json_reserve_bytes,
    "datagram_envelope_reserve_bytes": datagram_envelope_reserve_bytes,
    "datagram_encoded_payload_sum": datagram_encoded_payload_sum,
    "event_replay_batch": event_replay_batch,
    "idempotency_reservation_seconds": idempotency_reservation_seconds,
    "idempotency_result_hours": idempotency_result_hours,
    "invite_recipients": invite_recipients,
    "maintenance_interval_seconds": maintenance_interval_seconds,
    "ticket_retention_seconds": ticket_retention_seconds,
    "delivered_event_retention_seconds": delivered_event_retention_seconds,
    "presence_ttl_seconds": presence_ttl_seconds,
    "access_ttl_min_seconds": access_ttl_min_seconds,
    "access_ttl_max_seconds": access_ttl_max_seconds,
    "refresh_ttl_min_seconds": refresh_ttl_min_seconds,
    "refresh_ttl_max_seconds": refresh_ttl_max_seconds,
    "challenge_ttl_min_seconds": challenge_ttl_min_seconds,
    "challenge_ttl_max_seconds": challenge_ttl_max_seconds,
    "session_lease_min_seconds": session_lease_min_seconds,
    "session_lease_max_seconds": session_lease_max_seconds,
    "rate_limit_max_per_minute": rate_limit_max_per_minute,
    "long_poll_max_milliseconds": long_poll_max_milliseconds,
    "outbox_lease_seconds": outbox_lease_seconds,
    "property_base64_bytes": property_base64_bytes,
    "property_decoded_bytes": property_decoded_bytes,
    "rate_key_cap": rate_key_cap,
    "rate_window_seconds": rate_window_seconds,
    "relay_allocator_attempts": relay_allocator_attempts,
    "relay_queue_datagrams": relay_queue_datagrams,
    "relay_queue_ttl_seconds": relay_queue_ttl_seconds,
    "request_body_bytes": request_body_bytes,
    "session_members": session_members,
    "udp_payload_max": udp_payload_max,
    "virtual_ipv4_pool": virtual_ipv4_pool,
    "voice_packet_decoded_bytes": voice_packet_decoded_bytes,
    "voice_response_bytes": voice_response_bytes,
    "voice_batch_packets": voice_batch_packets,
    "voice_batch_body_bytes": voice_batch_body_bytes,
    "voice_route_ttl_seconds": voice_route_ttl_seconds,
    "worker_tick_milliseconds": worker_tick_milliseconds,
}, indent=2, sort_keys=True))
