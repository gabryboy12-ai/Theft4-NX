#!/usr/bin/env python3
"""Derive GTA IV multiplayer guest-layout constants used by host hooks.

Keep arithmetic here so changes to the audited retail layout can be checked
without doing pointer or capacity calculations by hand.
"""

from dataclasses import dataclass


@dataclass(frozen=True)
class PeerManagerLayout:
    local_peer_pointer_offset: int = 64
    inline_peer_offset: int = 408
    inline_peer_size: int = 64
    legacy_peer_count: int = 16
    free_list_offset: int = 1440
    pointer_table_offset: int = 1452
    pointer_size: int = 4
    capacity_field_offset: int = 1516
    peer_next_offset: int = 48
    peer_previous_offset: int = 52
    flags_offset: int = 8000


def main() -> None:
    layout = PeerManagerLayout()
    inline_end = (
        layout.inline_peer_offset
        + layout.inline_peer_size * layout.legacy_peer_count
    )
    last_pointer_offset = (
        layout.pointer_table_offset
        + layout.pointer_size * (layout.legacy_peer_count - 1)
    )
    first_invalid_pointer_offset = (
        layout.pointer_table_offset
        + layout.pointer_size * layout.legacy_peer_count
    )
    extended_peer_count = layout.legacy_peer_count * 4
    last_extended_pointer_offset = (
        layout.pointer_table_offset
        + layout.pointer_size * (extended_peer_count - 1)
    )
    mask_bytes = extended_peer_count // 8
    xsession_header_size = 0x80
    xsession_member_size = 0x10
    xsession_legacy_size = (
        xsession_header_size
        + xsession_member_size * layout.legacy_peer_count
    )
    xsession_extended_size = (
        xsession_header_size + xsession_member_size * extended_peer_count
    )
    player_info_table_base = ((-32064 & 0xFFFF) << 16) + 7280
    player_info_generation_base = ((-32064 & 0xFFFF) << 16) + 7216
    primary_player_id_address = ((-32086 & 0xFFFF) << 16) - 30856
    secondary_player_id_address = ((-32086 & 0xFFFF) << 16) - 30852
    global_peer_manager_address = ((-31983 & 0xFFFF) << 16) - 25464
    player_info_last_legacy = (
        player_info_table_base
        + layout.pointer_size * (layout.legacy_peer_count - 1)
    )
    player_info_generation_last_legacy = (
        player_info_generation_base
        + layout.pointer_size * (layout.legacy_peer_count - 1)
    )
    player_info_last_extended = (
        player_info_table_base
        + layout.pointer_size * (extended_peer_count - 1)
    )
    player_info_player_pointer_offset = 1400
    player_network_object_offset = 26 * layout.pointer_size
    player_network_object_id_offset = 1114
    set_max_slots_command_words = 9
    set_max_slots_command_size = set_max_slots_command_words * layout.pointer_size
    set_max_slots_public_offset = 5 * layout.pointer_size
    set_max_slots_private_offset = 6 * layout.pointer_size
    snapshot_48_legacy_size = 48 * 32
    snapshot_48_extended_size = 48 * extended_peer_count
    snapshot_8_legacy_size = 8 * 32
    snapshot_8_extended_size = 8 * extended_peer_count
    voice_population = extended_peer_count - 1
    voice_window = layout.legacy_peer_count - 1
    voice_starts = []
    voice_cursor = 0
    for _ in range(6):
        voice_starts.append(voice_cursor)
        voice_cursor = (voice_cursor + voice_window) % voice_population
    network_endpoint_bias = 23
    network_endpoint_table_offset = network_endpoint_bias * layout.pointer_size
    network_endpoint_last_legacy = (
        network_endpoint_table_offset
        + (layout.legacy_peer_count - 1) * layout.pointer_size
    )
    network_endpoint_first_extended = (
        network_endpoint_table_offset
        + layout.legacy_peer_count * layout.pointer_size
    )
    network_object_endpoint_state_offset = layout.pointer_size
    network_object_authoritative_offset = 14
    network_object_owner_peer_offset = network_object_authoritative_offset + 1
    network_object_pending_owner_offset = network_object_owner_peer_offset + 1
    network_object_flags_offset = network_object_pending_owner_offset + 1
    network_object_sync_flags_offset = network_object_flags_offset + 1
    network_object_ownership_token_offset = 40 * layout.pointer_size
    network_object_peer_flags_offset = 10 * layout.pointer_size
    network_object_peer_flags_stride = 3
    network_endpoint_sync_current_offset = 8
    network_endpoint_sync_queued_offset = network_endpoint_sync_current_offset + 1
    network_endpoint_sync_mode_offset = network_endpoint_sync_queued_offset + 1
    network_endpoint_sync_timer_offset = 3 * layout.pointer_size
    network_object_create_endpoint_vtable_offset = 17 * layout.pointer_size
    network_object_recipient_filter_vtable_offset = 43 * layout.pointer_size
    network_object_recipient_mode_vtable_offset = 58 * layout.pointer_size
    network_object_peer_joined_vtable_offset = 32 * layout.pointer_size
    network_object_peer_departed_vtable_offset = 33 * layout.pointer_size
    network_object_recipient_allowed_vtable_offset = 23 * layout.pointer_size
    network_object_expired_vtable_offset = 19 * layout.pointer_size
    network_peer_mask_legacy_bits = layout.legacy_peer_count
    network_peer_mask_marker_bits = 1
    network_peer_mask_header_bits = (
        network_peer_mask_legacy_bits + network_peer_mask_marker_bits
    )
    network_peer_mask_extension_chunk_bits = layout.legacy_peer_count
    network_peer_mask_extension_chunk_count = (
        extended_peer_count - network_peer_mask_legacy_bits
    ) // network_peer_mask_extension_chunk_bits
    network_peer_mask_marker = 1 << network_peer_mask_legacy_bits
    network_peer_mask_low_mask = network_peer_mask_marker - 1
    network_peer_mask_extension_bits = (
        network_peer_mask_extension_chunk_bits
        * network_peer_mask_extension_chunk_count
    )
    network_peer_mask_apply_return_address = 0x827046C8 + 0x30
    peer_manager_identity_lookup_enabled_flag = 1 << 6
    event_peer_outbound_buffer_size = 65 * layout.legacy_peer_count
    event_peer_inbound_buffer_size = 2 * event_peer_outbound_buffer_size
    event_peer_outbound_table_offset = 4592
    event_peer_inbound_table_offset = 21236
    event_scope_mask_offset = 2 * layout.pointer_size
    event_scope_high_peer_sentinel = 1 << (layout.legacy_peer_count - 1)
    event_peer_manager_pointer_offset = 2 * layout.pointer_size
    event_list_head_offset = 10 * layout.pointer_size
    event_node_value_offset = layout.pointer_size
    event_node_next_offset = 2 * layout.pointer_size
    event_metadata_pointer_offset = 3 * layout.pointer_size
    event_peer_scope_vtable_offset = 4 * layout.pointer_size
    event_queue_capacity = 128
    connection_broadcast_payload_size = 2 * layout.pointer_size
    connection_message_payload_pointer_offset = 3 * layout.pointer_size
    connection_message_address_offset = 8 * layout.pointer_size
    object_manager_peer_manager_offset = 76 * layout.pointer_size
    player_tick_disabled_address = ((-32064 & 0xFFFF) << 16) - 24252
    player_tick_global_base = ((-32087 & 0xFFFF) << 16) + 18256
    player_tick_global_activity_address = player_tick_global_base + layout.pointer_size
    player_tick_delta_address = ((-32057 & 0xFFFF) << 16) - 15708 + 8
    player_tick_scale_address = ((-32254 & 0xFFFF) << 16) - 30116
    player_tick_first_byte_array_offset = 8
    player_tick_second_byte_array_offset = 24
    player_tick_eight_byte_table_offset = 5 * 8
    player_tick_four_byte_table_offset = 42 * layout.pointer_size
    player_tick_pointer_table_offset = 58 * layout.pointer_size
    player_tick_large_record_offset = (1 << 16) + 62912
    player_tick_large_record_size = 40
    proximity_peer_result_capacity = 2 * layout.legacy_peer_count
    proximity_peer_scratch_size = extended_peer_count * layout.pointer_size
    proximity_weight_local_transform_offset = 8 * layout.pointer_size
    proximity_weight_position_offset = 12 * layout.pointer_size
    proximity_weight_peer_pointer_offset = 1376
    proximity_weight_stack_offset = 128
    proximity_weight_constant_base = ((-32251 & 0xFFFF) << 16) + 24464
    proximity_weight_first_threshold_address = proximity_weight_constant_base - 4
    proximity_weight_second_threshold_address = proximity_weight_constant_base
    proximity_weight_fourth_threshold_address = proximity_weight_constant_base - 20
    proximity_weight_first_slope_address = ((-32251 & 0xFFFF) << 16) + 24908
    proximity_weight_second_slope_address = ((-32255 & 0xFFFF) << 16) + 20888
    proximity_weight_fourth_slope_address = ((-32244 & 0xFFFF) << 16) - 4304
    proximity_weight_examples = [
        max(1.0 - (distance - 10.0) * 0.1, 0.0)
        if distance > 10.0
        else 1.0
        for distance in (5.0, 15.0, 20.0)
    ]
    player_info_unique_value_offset = 346 * layout.pointer_size
    peer_uniqueness_function_address = 0x826FFCB0
    peer_uniqueness_identity_scan_return_address = (
        peer_uniqueness_function_address + (0x826FFD88 - peer_uniqueness_function_address)
    )
    peer_uniqueness_identity_info_return_address = (
        peer_uniqueness_function_address + (0x826FFDB8 - peer_uniqueness_function_address)
    )
    peer_uniqueness_candidate_info_return_address = (
        peer_uniqueness_function_address + (0x826FFE18 - peer_uniqueness_function_address)
    )
    clone_peer_copy_function_address = 0x826EFF58
    clone_endpoint_build_return_address = (
        clone_peer_copy_function_address + (0x826F022C - clone_peer_copy_function_address)
    )
    clone_type_query_return_address = (
        clone_peer_copy_function_address + (0x826F0234 - clone_peer_copy_function_address)
    )
    clone_endpoint_type_one_flag = 1 << 14
    network_clock_address = ((-32057 & 0xFFFF) << 16) - 15724
    object_update_current_time_address = ((-32057 & 0xFFFF) << 16) - 15688
    object_update_flags_address = ((-31975 & 0xFFFF) << 16) - 9388
    object_update_previous_time_address = ((-31975 & 0xFFFF) << 16) - 9392
    object_manager_short_peer_timeout_offset = 333988
    object_manager_long_peer_timeout_offset = 333992
    object_manager_channel_timeout = 3000
    object_manager_channel_count = layout.legacy_peer_count // 4
    object_peer_sync_ack_size = 1048
    object_peer_reliable_size = 4160
    object_peer_queue_size = 3 * layout.pointer_size
    object_peer_message_size = 1032
    object_peer_message_table_offset = 1468
    object_peer_message_queue_offset = 2 * layout.pointer_size
    object_peer_message_payload_offset = 9 * layout.pointer_size
    object_peer_message_payload_capacity = object_peer_message_size - 37
    object_peer_sync_ack_table_offset = 17984
    object_peer_reliable_table_offset = 34756
    object_peer_queue_table_offset = 101320
    object_peer_queue_count_offset = 2 * layout.pointer_size
    object_peer_sequence_stride = 2
    object_peer_sequence_table_offset = (
        object_peer_queue_table_offset
        + object_peer_queue_size * layout.legacy_peer_count
        + 2 * layout.pointer_size
    )
    object_owner_list_bias = 42
    object_owner_list_header_size = 2 * layout.pointer_size
    object_owner_list_table_offset = (
        object_owner_list_bias * object_owner_list_header_size
    )
    object_owner_list_node_object_offset = layout.pointer_size
    object_owner_list_node_next_offset = 2 * layout.pointer_size
    object_owner_list_node_previous_offset = 3 * layout.pointer_size
    object_peer_matrix_bias = 32212
    object_peer_matrix_legacy_stride = layout.legacy_peer_count
    object_peer_matrix_table_offset = (
        object_peer_matrix_bias * layout.pointer_size
    )
    object_peer_matrix_legacy_row_size = (
        object_peer_matrix_legacy_stride * layout.pointer_size
    )
    object_peer_matrix_object_capacity = 51200 // object_peer_matrix_legacy_stride
    object_peer_matrix_legacy_bytes = (
        object_peer_matrix_object_capacity
        * object_peer_matrix_legacy_stride
        * layout.pointer_size
    )
    object_peer_matrix_threshold_offset = (5 << 16) + 6288
    object_manager_initialized_flag_offset = (5 << 16) + 6335
    object_manager_reassignment_offset = (2 << 16) - 22824
    object_removal_batch_capacity = 800 // layout.pointer_size
    dispatch_state_offset = 2 * layout.pointer_size
    dispatch_element_count_offset = 5 * layout.pointer_size
    dispatch_element_state_table_offset = 8 * layout.pointer_size
    dispatch_peer_state_pointer_table_offset = 10 * layout.pointer_size
    dispatch_peer_mask_pointer_table_offset = 12 * layout.pointer_size
    dispatch_authority_table_offset = 14 * layout.pointer_size
    dispatch_element_state_record_size = layout.pointer_size
    dispatch_authority_record_size = 2
    dispatch_element_pointer_record_size = 2 * layout.pointer_size
    dispatch_message_stride = 1 << 10
    dispatch_message_queue_offset = layout.pointer_size
    dispatch_message_payload_offset = 8 * layout.pointer_size
    dispatch_message_payload_capacity = dispatch_message_stride - 35
    dispatch_message_wire_header_bytes = 16 - 3
    dispatch_sequence_offset = 28 * layout.pointer_size
    dispatch_send_all_offset = dispatch_sequence_offset + 2
    dispatch_initialized_offset = dispatch_sequence_offset + 3
    dispatch_reset_enabled_offset = dispatch_sequence_offset + 4
    network_array_manager_pointer_offset = layout.pointer_size
    network_array_peer_manager_offset = 3 * layout.pointer_size
    network_array_handler_list_offset = 11 * layout.pointer_size
    network_array_handler_node_value_offset = layout.pointer_size
    network_array_handler_node_next_offset = 2 * layout.pointer_size
    session_participant_record_size = 32
    session_participant_table_offset = 520
    session_participant_count_offset = 1544
    session_participant_legacy_bytes = (
        session_participant_record_size * 32
    )
    session_participant_extended_bytes = (
        session_participant_record_size * extended_peer_count
    )
    session_participant_first_overlapping_offset = (
        session_participant_table_offset + session_participant_legacy_bytes
    )
    reassign_command_record_size = 20
    reassign_command_live_bytes = 24
    reassign_object_list_bias = 42
    reassign_object_list_header_size = 8
    reassign_transport_state_size = 76
    reassign_legacy_recipient_stride = (
        reassign_transport_state_size * layout.legacy_peer_count
        + layout.pointer_size
    )
    reassign_extended_recipient_stride = (
        reassign_transport_state_size * extended_peer_count
        + layout.pointer_size
    )
    reassign_extended_transport_bytes = (
        reassign_extended_recipient_stride * extended_peer_count
    )
    reassign_last_command_start = (
        reassign_command_record_size * (extended_peer_count - 1)
    )
    reassign_last_command_end = (
        reassign_last_command_start + reassign_command_live_bytes
    )
    reassign_last_object_list_offset = (
        (reassign_object_list_bias + extended_peer_count - 1)
        * reassign_object_list_header_size
    )
    reassign_transport_constructor_address = (
        ((-32097 & 0xFFFF) << 16) + 3600
    )
    reassign_transport_destructor_address = (
        ((-32097 & 0xFFFF) << 16) + 2336
    )
    reassign_message_parser_state_address = (
        ((-31975 & 0xFFFF) << 16) - 4304
    )
    reassign_status_parser_state_address = (
        ((-31975 & 0xFFFF) << 16) - 4240
    )

    assert inline_end == 1432
    assert last_pointer_offset == 1512
    assert first_invalid_pointer_offset == layout.capacity_field_offset
    assert layout.flags_offset == 8000
    assert extended_peer_count == 64
    assert last_extended_pointer_offset == 1704
    assert mask_bytes == 8
    assert xsession_legacy_size == 0x180
    assert xsession_extended_size == 0x480
    assert player_info_table_base == 0x82C01C70
    assert player_info_generation_base == 0x82C01C30
    assert primary_player_id_address == 0x82A98778
    assert secondary_player_id_address == 0x82A9877C
    assert global_peer_manager_address == 0x83109C88
    assert player_info_last_legacy == 0x82C01CAC
    assert player_info_generation_last_legacy == 0x82C01C6C
    assert player_info_last_extended == 0x82C01D6C
    assert player_network_object_offset == 104
    assert set_max_slots_command_size == 36
    assert set_max_slots_public_offset == 20
    assert set_max_slots_private_offset == 24
    assert snapshot_48_legacy_size == 1536
    assert snapshot_48_extended_size == 3072
    assert snapshot_8_legacy_size == 256
    assert snapshot_8_extended_size == 512
    assert voice_starts == [0, 15, 30, 45, 60, 12]
    assert network_endpoint_table_offset == 92
    assert network_endpoint_last_legacy == 152
    assert network_endpoint_first_extended == 156
    assert network_object_endpoint_state_offset == 4
    assert network_object_authoritative_offset == 14
    assert network_object_owner_peer_offset == 15
    assert network_object_pending_owner_offset == 16
    assert network_object_flags_offset == 17
    assert network_object_sync_flags_offset == 18
    assert network_object_ownership_token_offset == 160
    assert network_object_peer_flags_offset == 40
    assert network_object_peer_flags_stride == 3
    assert network_endpoint_sync_current_offset == 8
    assert network_endpoint_sync_queued_offset == 9
    assert network_endpoint_sync_mode_offset == 10
    assert network_endpoint_sync_timer_offset == 12
    assert network_object_create_endpoint_vtable_offset == 68
    assert network_object_recipient_filter_vtable_offset == 172
    assert network_object_recipient_mode_vtable_offset == 232
    assert network_object_peer_joined_vtable_offset == 128
    assert network_object_peer_departed_vtable_offset == 132
    assert network_object_recipient_allowed_vtable_offset == 92
    assert network_object_expired_vtable_offset == 76
    assert network_peer_mask_legacy_bits == 16
    assert network_peer_mask_header_bits == 17
    assert network_peer_mask_extension_chunk_bits == 16
    assert network_peer_mask_extension_chunk_count == 3
    assert network_peer_mask_marker == 0x10000
    assert network_peer_mask_low_mask == 0xFFFF
    assert network_peer_mask_extension_bits == 48
    assert network_peer_mask_apply_return_address == 0x827046F8
    assert peer_manager_identity_lookup_enabled_flag == 0x40
    assert event_peer_outbound_buffer_size == 1040
    assert event_peer_inbound_buffer_size == 2080
    assert event_scope_mask_offset == 8
    assert event_scope_high_peer_sentinel == 0x8000
    assert event_peer_manager_pointer_offset == 8
    assert event_list_head_offset == 40
    assert event_node_value_offset == 4
    assert event_node_next_offset == 8
    assert event_metadata_pointer_offset == 12
    assert event_peer_scope_vtable_offset == 16
    assert connection_broadcast_payload_size == 8
    assert connection_message_payload_pointer_offset == 12
    assert connection_message_address_offset == 32
    assert object_manager_peer_manager_offset == 304
    assert player_tick_disabled_address == 0x82BFA144
    assert player_tick_global_base == 0x82A94750
    assert player_tick_global_activity_address == 0x82A94754
    assert player_tick_delta_address == 0x82C6C2AC
    assert player_tick_scale_address == 0x82018A5C
    assert player_tick_first_byte_array_offset == 8
    assert player_tick_second_byte_array_offset == 24
    assert player_tick_eight_byte_table_offset == 40
    assert player_tick_four_byte_table_offset == 168
    assert player_tick_pointer_table_offset == 232
    assert player_tick_large_record_offset == 128448
    assert player_tick_large_record_size == 40
    assert proximity_peer_result_capacity == 32
    assert proximity_peer_scratch_size == 256
    assert proximity_weight_local_transform_offset == 32
    assert proximity_weight_position_offset == 48
    assert proximity_weight_peer_pointer_offset == 1376
    assert proximity_weight_stack_offset == 128
    assert proximity_weight_constant_base == 0x82055F90
    assert proximity_weight_first_threshold_address == 0x82055F8C
    assert proximity_weight_second_threshold_address == 0x82055F90
    assert proximity_weight_fourth_threshold_address == 0x82055F7C
    assert proximity_weight_first_slope_address == 0x8205614C
    assert proximity_weight_second_slope_address == 0x82015198
    assert proximity_weight_fourth_slope_address == 0x820BEF30
    assert proximity_weight_examples == [1.0, 0.5, 0.0]
    assert player_info_unique_value_offset == 1384
    assert peer_uniqueness_identity_scan_return_address == 0x826FFD88
    assert peer_uniqueness_identity_info_return_address == 0x826FFDB8
    assert peer_uniqueness_candidate_info_return_address == 0x826FFE18
    assert clone_endpoint_build_return_address == 0x826F022C
    assert clone_type_query_return_address == 0x826F0234
    assert clone_endpoint_type_one_flag == 16384
    assert network_clock_address == 0x82C6C294
    assert object_update_current_time_address == 0x82C6C2B8
    assert object_update_flags_address == 0x8318DB54
    assert object_update_previous_time_address == 0x8318DB50
    assert object_manager_channel_count == 4
    assert object_peer_queue_size == 12
    assert object_peer_queue_count_offset == 8
    assert object_peer_sequence_stride == 2
    assert object_peer_sequence_table_offset == 101520
    assert object_peer_message_payload_capacity == 995
    assert object_owner_list_table_offset == 336
    assert object_owner_list_header_size == 8
    assert object_owner_list_node_object_offset == 4
    assert object_owner_list_node_next_offset == 8
    assert object_owner_list_node_previous_offset == 12
    assert object_peer_matrix_table_offset == 128848
    assert object_peer_matrix_legacy_row_size == 64
    assert object_peer_matrix_object_capacity == 3200
    assert object_peer_matrix_legacy_bytes == 204800
    assert object_peer_matrix_threshold_offset == 333968
    assert object_manager_initialized_flag_offset == 334015
    assert object_manager_reassignment_offset == 108248
    assert object_removal_batch_capacity == 200
    assert dispatch_state_offset == 8
    assert dispatch_element_count_offset == 20
    assert dispatch_element_state_table_offset == 32
    assert dispatch_peer_state_pointer_table_offset == 40
    assert dispatch_peer_mask_pointer_table_offset == 48
    assert dispatch_authority_table_offset == 56
    assert dispatch_element_state_record_size == 4
    assert dispatch_authority_record_size == 2
    assert dispatch_element_pointer_record_size == 8
    assert dispatch_message_stride == 1024
    assert dispatch_message_queue_offset == 4
    assert dispatch_message_payload_offset == 32
    assert dispatch_message_payload_capacity == 989
    assert dispatch_message_wire_header_bytes == 13
    assert dispatch_sequence_offset == 112
    assert dispatch_send_all_offset == 114
    assert dispatch_initialized_offset == 115
    assert dispatch_reset_enabled_offset == 116
    assert network_array_manager_pointer_offset == 4
    assert network_array_peer_manager_offset == 12
    assert network_array_handler_list_offset == 44
    assert network_array_handler_node_value_offset == 4
    assert network_array_handler_node_next_offset == 8
    assert session_participant_legacy_bytes == 1024
    assert session_participant_extended_bytes == 2048
    assert session_participant_first_overlapping_offset == 1544
    assert session_participant_first_overlapping_offset == session_participant_count_offset
    assert reassign_legacy_recipient_stride == 1220
    assert reassign_extended_recipient_stride == 4868
    assert reassign_extended_transport_bytes == 311552
    assert reassign_command_live_bytes == 24
    assert reassign_last_command_start == 1260
    assert reassign_last_command_end == 1284
    assert reassign_last_object_list_offset == 840
    assert reassign_transport_constructor_address == 0x829F0E10
    assert reassign_transport_destructor_address == 0x829F0920
    assert reassign_message_parser_state_address == 0x8318EF30
    assert reassign_status_parser_state_address == 0x8318EF70

    print(f"inline_end={inline_end}")
    print(f"last_pointer_offset={last_pointer_offset}")
    print(f"first_invalid_pointer_offset={first_invalid_pointer_offset}")
    print(f"peer_manager_flags_offset={layout.flags_offset}")
    print(f"extended_peer_count={extended_peer_count}")
    print(f"last_extended_pointer_offset={last_extended_pointer_offset}")
    print(f"mask_bytes={mask_bytes}")
    print(f"xsession_legacy_size=0x{xsession_legacy_size:X}")
    print(f"xsession_extended_size=0x{xsession_extended_size:X}")
    print(f"player_info_table_base=0x{player_info_table_base:X}")
    print(f"player_info_generation_base=0x{player_info_generation_base:X}")
    print(f"primary_player_id_address=0x{primary_player_id_address:X}")
    print(f"secondary_player_id_address=0x{secondary_player_id_address:X}")
    print(f"global_peer_manager_address=0x{global_peer_manager_address:X}")
    print(f"player_info_last_legacy=0x{player_info_last_legacy:X}")
    print(
        "player_info_generation_last_legacy="
        f"0x{player_info_generation_last_legacy:X}"
    )
    print(f"player_info_last_extended=0x{player_info_last_extended:X}")
    print(f"player_info_player_pointer_offset={player_info_player_pointer_offset}")
    print(f"player_network_object_offset={player_network_object_offset}")
    print(f"player_network_object_id_offset={player_network_object_id_offset}")
    print(f"set_max_slots_command_size={set_max_slots_command_size}")
    print(f"set_max_slots_public_offset={set_max_slots_public_offset}")
    print(f"set_max_slots_private_offset={set_max_slots_private_offset}")
    print(f"snapshot_48_legacy_size={snapshot_48_legacy_size}")
    print(f"snapshot_48_extended_size={snapshot_48_extended_size}")
    print(f"snapshot_8_legacy_size={snapshot_8_legacy_size}")
    print(f"snapshot_8_extended_size={snapshot_8_extended_size}")
    print(f"voice_starts={voice_starts}")
    print(f"network_endpoint_table_offset={network_endpoint_table_offset}")
    print(f"network_endpoint_last_legacy={network_endpoint_last_legacy}")
    print(f"network_endpoint_first_extended={network_endpoint_first_extended}")
    print(f"network_object_endpoint_state_offset={network_object_endpoint_state_offset}")
    print(f"network_object_authoritative_offset={network_object_authoritative_offset}")
    print(f"network_object_owner_peer_offset={network_object_owner_peer_offset}")
    print(f"network_object_pending_owner_offset={network_object_pending_owner_offset}")
    print(f"network_object_flags_offset={network_object_flags_offset}")
    print(f"network_object_sync_flags_offset={network_object_sync_flags_offset}")
    print(f"network_object_ownership_token_offset={network_object_ownership_token_offset}")
    print(f"network_object_peer_flags_offset={network_object_peer_flags_offset}")
    print(f"network_object_peer_flags_stride={network_object_peer_flags_stride}")
    print(f"network_endpoint_sync_current_offset={network_endpoint_sync_current_offset}")
    print(f"network_endpoint_sync_queued_offset={network_endpoint_sync_queued_offset}")
    print(f"network_endpoint_sync_mode_offset={network_endpoint_sync_mode_offset}")
    print(f"network_endpoint_sync_timer_offset={network_endpoint_sync_timer_offset}")
    print(
        "network_object_create_endpoint_vtable_offset="
        f"{network_object_create_endpoint_vtable_offset}"
    )
    print(
        "network_object_recipient_filter_vtable_offset="
        f"{network_object_recipient_filter_vtable_offset}"
    )
    print(
        "network_object_recipient_mode_vtable_offset="
        f"{network_object_recipient_mode_vtable_offset}"
    )
    print(f"network_object_peer_joined_vtable_offset={network_object_peer_joined_vtable_offset}")
    print(f"network_object_peer_departed_vtable_offset={network_object_peer_departed_vtable_offset}")
    print(
        "network_object_recipient_allowed_vtable_offset="
        f"{network_object_recipient_allowed_vtable_offset}"
    )
    print(f"network_object_expired_vtable_offset={network_object_expired_vtable_offset}")
    print(f"network_peer_mask_legacy_bits={network_peer_mask_legacy_bits}")
    print(f"network_peer_mask_header_bits={network_peer_mask_header_bits}")
    print(
        "network_peer_mask_extension_chunk_bits="
        f"{network_peer_mask_extension_chunk_bits}"
    )
    print(
        "network_peer_mask_extension_chunk_count="
        f"{network_peer_mask_extension_chunk_count}"
    )
    print(f"network_peer_mask_marker=0x{network_peer_mask_marker:X}")
    print(f"network_peer_mask_low_mask=0x{network_peer_mask_low_mask:X}")
    print(f"network_peer_mask_extension_bits={network_peer_mask_extension_bits}")
    print(
        "network_peer_mask_apply_return_address="
        f"0x{network_peer_mask_apply_return_address:X}"
    )
    print(
        "peer_manager_identity_lookup_enabled_flag="
        f"0x{peer_manager_identity_lookup_enabled_flag:X}"
    )
    print(f"event_peer_outbound_buffer_size={event_peer_outbound_buffer_size}")
    print(f"event_peer_inbound_buffer_size={event_peer_inbound_buffer_size}")
    print(f"event_peer_outbound_table_offset={event_peer_outbound_table_offset}")
    print(f"event_peer_inbound_table_offset={event_peer_inbound_table_offset}")
    print(f"event_scope_mask_offset={event_scope_mask_offset}")
    print(f"event_scope_high_peer_sentinel=0x{event_scope_high_peer_sentinel:X}")
    print(f"event_peer_manager_pointer_offset={event_peer_manager_pointer_offset}")
    print(f"event_list_head_offset={event_list_head_offset}")
    print(f"event_node_value_offset={event_node_value_offset}")
    print(f"event_node_next_offset={event_node_next_offset}")
    print(f"event_metadata_pointer_offset={event_metadata_pointer_offset}")
    print(f"event_peer_scope_vtable_offset={event_peer_scope_vtable_offset}")
    print(f"event_queue_capacity={event_queue_capacity}")
    print(f"connection_broadcast_payload_size={connection_broadcast_payload_size}")
    print(
        "connection_message_payload_pointer_offset="
        f"{connection_message_payload_pointer_offset}"
    )
    print(f"connection_message_address_offset={connection_message_address_offset}")
    print(f"object_manager_peer_manager_offset={object_manager_peer_manager_offset}")
    print(f"player_tick_disabled_address=0x{player_tick_disabled_address:X}")
    print(f"player_tick_global_base=0x{player_tick_global_base:X}")
    print(
        "player_tick_global_activity_address="
        f"0x{player_tick_global_activity_address:X}"
    )
    print(f"player_tick_delta_address=0x{player_tick_delta_address:X}")
    print(f"player_tick_scale_address=0x{player_tick_scale_address:X}")
    print(f"player_tick_first_byte_array_offset={player_tick_first_byte_array_offset}")
    print(f"player_tick_second_byte_array_offset={player_tick_second_byte_array_offset}")
    print(f"player_tick_eight_byte_table_offset={player_tick_eight_byte_table_offset}")
    print(f"player_tick_four_byte_table_offset={player_tick_four_byte_table_offset}")
    print(f"player_tick_pointer_table_offset={player_tick_pointer_table_offset}")
    print(f"player_tick_large_record_offset={player_tick_large_record_offset}")
    print(f"player_tick_large_record_size={player_tick_large_record_size}")
    print(f"proximity_peer_result_capacity={proximity_peer_result_capacity}")
    print(f"proximity_peer_scratch_size={proximity_peer_scratch_size}")
    print(f"proximity_weight_local_transform_offset={proximity_weight_local_transform_offset}")
    print(f"proximity_weight_position_offset={proximity_weight_position_offset}")
    print(f"proximity_weight_peer_pointer_offset={proximity_weight_peer_pointer_offset}")
    print(f"proximity_weight_stack_offset={proximity_weight_stack_offset}")
    print(f"proximity_weight_constant_base=0x{proximity_weight_constant_base:X}")
    print(
        "proximity_weight_first_threshold_address="
        f"0x{proximity_weight_first_threshold_address:X}"
    )
    print(
        "proximity_weight_second_threshold_address="
        f"0x{proximity_weight_second_threshold_address:X}"
    )
    print(
        "proximity_weight_fourth_threshold_address="
        f"0x{proximity_weight_fourth_threshold_address:X}"
    )
    print(
        "proximity_weight_first_slope_address="
        f"0x{proximity_weight_first_slope_address:X}"
    )
    print(
        "proximity_weight_second_slope_address="
        f"0x{proximity_weight_second_slope_address:X}"
    )
    print(
        "proximity_weight_fourth_slope_address="
        f"0x{proximity_weight_fourth_slope_address:X}"
    )
    print(f"proximity_weight_examples={proximity_weight_examples}")
    print(f"player_info_unique_value_offset={player_info_unique_value_offset}")
    print(
        "peer_uniqueness_identity_scan_return_address="
        f"0x{peer_uniqueness_identity_scan_return_address:X}"
    )
    print(
        "peer_uniqueness_identity_info_return_address="
        f"0x{peer_uniqueness_identity_info_return_address:X}"
    )
    print(
        "peer_uniqueness_candidate_info_return_address="
        f"0x{peer_uniqueness_candidate_info_return_address:X}"
    )
    print(f"clone_endpoint_build_return_address=0x{clone_endpoint_build_return_address:X}")
    print(f"clone_type_query_return_address=0x{clone_type_query_return_address:X}")
    print(f"clone_endpoint_type_one_flag=0x{clone_endpoint_type_one_flag:X}")
    print(f"network_clock_address=0x{network_clock_address:X}")
    print(f"object_update_current_time_address=0x{object_update_current_time_address:X}")
    print(f"object_update_flags_address=0x{object_update_flags_address:X}")
    print(f"object_update_previous_time_address=0x{object_update_previous_time_address:X}")
    print(
        "object_manager_short_peer_timeout_offset="
        f"{object_manager_short_peer_timeout_offset}"
    )
    print(
        "object_manager_long_peer_timeout_offset="
        f"{object_manager_long_peer_timeout_offset}"
    )
    print(f"object_manager_channel_timeout={object_manager_channel_timeout}")
    print(f"object_manager_channel_count={object_manager_channel_count}")
    print(f"object_peer_sync_ack_size={object_peer_sync_ack_size}")
    print(f"object_peer_reliable_size={object_peer_reliable_size}")
    print(f"object_peer_queue_size={object_peer_queue_size}")
    print(f"object_peer_message_size={object_peer_message_size}")
    print(f"object_peer_message_table_offset={object_peer_message_table_offset}")
    print(f"object_peer_message_queue_offset={object_peer_message_queue_offset}")
    print(f"object_peer_message_payload_offset={object_peer_message_payload_offset}")
    print(f"object_peer_message_payload_capacity={object_peer_message_payload_capacity}")
    print(f"object_peer_sync_ack_table_offset={object_peer_sync_ack_table_offset}")
    print(f"object_peer_reliable_table_offset={object_peer_reliable_table_offset}")
    print(f"object_peer_queue_table_offset={object_peer_queue_table_offset}")
    print(f"object_peer_queue_count_offset={object_peer_queue_count_offset}")
    print(f"object_peer_sequence_stride={object_peer_sequence_stride}")
    print(f"object_peer_sequence_table_offset={object_peer_sequence_table_offset}")
    print(f"object_owner_list_bias={object_owner_list_bias}")
    print(f"object_owner_list_header_size={object_owner_list_header_size}")
    print(f"object_owner_list_table_offset={object_owner_list_table_offset}")
    print(f"object_owner_list_node_object_offset={object_owner_list_node_object_offset}")
    print(f"object_owner_list_node_next_offset={object_owner_list_node_next_offset}")
    print(f"object_owner_list_node_previous_offset={object_owner_list_node_previous_offset}")
    print(f"object_peer_matrix_bias={object_peer_matrix_bias}")
    print(f"object_peer_matrix_legacy_stride={object_peer_matrix_legacy_stride}")
    print(f"object_peer_matrix_table_offset={object_peer_matrix_table_offset}")
    print(f"object_peer_matrix_legacy_row_size={object_peer_matrix_legacy_row_size}")
    print(f"object_peer_matrix_object_capacity={object_peer_matrix_object_capacity}")
    print(f"object_peer_matrix_legacy_bytes={object_peer_matrix_legacy_bytes}")
    print(f"object_peer_matrix_threshold_offset={object_peer_matrix_threshold_offset}")
    print(f"object_manager_initialized_flag_offset={object_manager_initialized_flag_offset}")
    print(f"object_manager_reassignment_offset={object_manager_reassignment_offset}")
    print(f"object_removal_batch_capacity={object_removal_batch_capacity}")
    print(f"dispatch_state_offset={dispatch_state_offset}")
    print(f"dispatch_element_count_offset={dispatch_element_count_offset}")
    print(f"dispatch_element_state_table_offset={dispatch_element_state_table_offset}")
    print(
        "dispatch_peer_state_pointer_table_offset="
        f"{dispatch_peer_state_pointer_table_offset}"
    )
    print(
        "dispatch_peer_mask_pointer_table_offset="
        f"{dispatch_peer_mask_pointer_table_offset}"
    )
    print(f"dispatch_authority_table_offset={dispatch_authority_table_offset}")
    print(f"dispatch_element_state_record_size={dispatch_element_state_record_size}")
    print(f"dispatch_authority_record_size={dispatch_authority_record_size}")
    print(f"dispatch_element_pointer_record_size={dispatch_element_pointer_record_size}")
    print(f"dispatch_message_stride={dispatch_message_stride}")
    print(f"dispatch_message_queue_offset={dispatch_message_queue_offset}")
    print(f"dispatch_message_payload_offset={dispatch_message_payload_offset}")
    print(f"dispatch_message_payload_capacity={dispatch_message_payload_capacity}")
    print(f"dispatch_message_wire_header_bytes={dispatch_message_wire_header_bytes}")
    print(f"dispatch_sequence_offset={dispatch_sequence_offset}")
    print(f"dispatch_send_all_offset={dispatch_send_all_offset}")
    print(f"dispatch_initialized_offset={dispatch_initialized_offset}")
    print(f"dispatch_reset_enabled_offset={dispatch_reset_enabled_offset}")
    print(f"network_array_manager_pointer_offset={network_array_manager_pointer_offset}")
    print(f"network_array_peer_manager_offset={network_array_peer_manager_offset}")
    print(f"network_array_handler_list_offset={network_array_handler_list_offset}")
    print(
        "network_array_handler_node_value_offset="
        f"{network_array_handler_node_value_offset}"
    )
    print(
        "network_array_handler_node_next_offset="
        f"{network_array_handler_node_next_offset}"
    )
    print(f"session_participant_legacy_bytes={session_participant_legacy_bytes}")
    print(f"session_participant_extended_bytes={session_participant_extended_bytes}")
    print(
        "session_participant_first_overlapping_offset="
        f"{session_participant_first_overlapping_offset}"
    )
    print(f"reassign_legacy_recipient_stride={reassign_legacy_recipient_stride}")
    print(f"reassign_extended_recipient_stride={reassign_extended_recipient_stride}")
    print(f"reassign_extended_transport_bytes={reassign_extended_transport_bytes}")
    print(f"reassign_command_live_bytes={reassign_command_live_bytes}")
    print(f"reassign_last_command_start={reassign_last_command_start}")
    print(f"reassign_last_command_end={reassign_last_command_end}")
    print(f"reassign_last_object_list_offset={reassign_last_object_list_offset}")
    print(
        "reassign_transport_constructor_address="
        f"0x{reassign_transport_constructor_address:X}"
    )
    print(
        "reassign_transport_destructor_address="
        f"0x{reassign_transport_destructor_address:X}"
    )
    print(
        "reassign_message_parser_state_address="
        f"0x{reassign_message_parser_state_address:X}"
    )
    print(
        "reassign_status_parser_state_address="
        f"0x{reassign_status_parser_state_address:X}"
    )


if __name__ == "__main__":
    main()
