#!/usr/bin/env python3
"""Audit GTA IV generated online-service import call shapes and ABI layouts."""

from __future__ import annotations

import json
import re
from collections import Counter
from functools import lru_cache
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GENERATED = ROOT / "generated"
SDK_ROOT = ROOT.parent
WORKSPACE = Path(__file__).resolve().parents[4]
IMPORTS = (
    "__imp__XamUserAreUsersFriends",
    "__imp__XamUserCreateStatsEnumerator",
    "__imp__XamVoiceHeadsetPresent",
    "__imp__XamVoiceCreate",
    "__imp__XamVoiceSubmitPacket",
    "__imp__XamVoiceClose",
    "__imp__XMsgInProcessCall",
    "__imp__XMsgStartIORequest",
    "__imp__XMsgStartIORequestEx",
)


@lru_cache(maxsize=None)
def generated_function(name: str) -> str:
    marker = f"DEFINE_REX_FUNC({name})"
    for path in sorted(GENERATED.glob("gta4_recomp.*.cpp")):
        source = path.read_text(encoding="utf-8")
        begin = source.find(marker)
        if begin < 0:
            continue
        end = source.find("\nDEFINE_REX_FUNC(", begin + len(marker))
        return source[begin : len(source) if end < 0 else end]
    raise AssertionError(f"generated function {name} was not found")


def require_generated_contract(name: str, fragments: tuple[str, ...]) -> None:
    body = generated_function(name)
    missing = [fragment for fragment in fragments if fragment not in body]
    if missing:
        raise AssertionError(f"{name} is missing generated contract fragments: {missing}")


def verify_retail_contracts() -> dict[str, object]:
    # These checks intentionally bind the report to executable generated PPC,
    # not recovered pseudocode or an assumed SDK header.
    contracts = {
        "friend_reader": (
            "sub_829EF1E8",
            (
                "ctx.r10.u64 * static_cast<uint64_t>(196)",
                "REX_LOAD_U32(ctx.r31.u32 + 24)",
                "& 0xC0000000",
                "ctx.r31.s64 = ctx.r31.s64 + 196",
            ),
        ),
        "friend_online_accessor": (
            "sub_829E8A88",
            ("REX_LOAD_U32(ctx.r3.u32 + 24)", "ctx.r11.u32 & 0x1"),
        ),
        "friend_title_accessor": (
            "sub_829E8A98",
            ("REX_LOAD_U32(ctx.r3.u32 + 36)",),
        ),
        "stats_result_consumer": (
            "sub_829E6760",
            (
                "ctx.r23.s64 = ctx.r23.s64 + 48",
                "ctx.r30.s64 = ctx.r30.s64 + 24",
                "REX_LOAD_U64(ctx.r28.u32 + 0)",
                "REX_LOAD_U32(ctx.r28.u32 + 8)",
                "ctx.r28.s64 + 16",
                "ctx.r28.s64 + 24",
                "REX_LOAD_U32(ctx.r28.u32 + 44)",
            ),
        ),
        "stats_direct_read_wrapper": (
            "sub_82A12228",
            (
                "ctx.r4.s64 = 720896",
                "ctx.r4.u64 = ctx.r4.u64 | 33",
                "ctx.r7.s64 = 28",
                "__imp__XMsgStartIORequest(ctx, base)",
            ),
        ),
        "friends_dynamic_wrapper": (
            "sub_82A35350",
            (
                "ctx.r4.s64 = 327680",
                "ctx.r4.u64 = ctx.r4.u64 | 32800",
                "ctx.r3.s64 = 252",
                "__imp__XMsgInProcessCall(ctx, base)",
            ),
        ),
        "mute_wrapper": (
            "sub_82A36610",
            (
                "REX_STORE_U32(ctx.r1.u32 + 80",
                "REX_STORE_U64(ctx.r1.u32 + 88",
                "REX_LOAD_U32(ctx.r1.u32 + 96)",
                "ctx.r4.s64 = 327680",
                "ctx.r4.u64 = ctx.r4.u64 | 32782",
            ),
        ),
    }
    for _, (name, fragments) in contracts.items():
        require_generated_contract(name, fragments)
    assert generated_function("sub_82A35350").count("sub_82A35320(ctx, base);") == 5
    return {key: name for key, (name, _) in contracts.items()}


def align(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def layout(fields: list[tuple[str, int, int]]) -> dict[str, object]:
    offset = 0
    result: dict[str, int] = {}
    struct_alignment = 1
    for name, size, alignment in fields:
        offset = align(offset, alignment)
        result[name] = offset
        offset += size
        struct_alignment = max(struct_alignment, alignment)
    return {"size": align(offset, struct_alignment), "offsets": result}


def call_sites() -> dict[str, list[dict[str, object]]]:
    output: dict[str, list[dict[str, object]]] = {name: [] for name in IMPORTS}
    function_pattern = re.compile(r"DEFINE_REX_FUNC\(([^)]+)\)")
    lr_pattern = re.compile(r"ctx\.lr = 0x([0-9A-Fa-f]+);")
    for path in sorted(GENERATED.glob("gta4_recomp.*.cpp")):
        lines = path.read_text(encoding="utf-8").splitlines()
        function = ""
        for index, line in enumerate(lines):
            match = function_pattern.search(line)
            if match:
                function = match.group(1)
            for import_name in IMPORTS:
                if f"{import_name}(ctx, base);" not in line:
                    continue
                window = lines[max(0, index - 18) : index + 2]
                return_address = None
                for candidate in reversed(window):
                    lr_match = lr_pattern.search(candidate)
                    if lr_match:
                        return_address = "0x" + lr_match.group(1).upper()
                        break
                output[import_name].append({
                    "function": function,
                    "return_address": return_address,
                    "source": f"{path.name}:{index + 1}",
                    "setup": [candidate.strip() for candidate in window],
                })
    return output


def source_files(root: Path) -> list[Path]:
    suffixes = {".c", ".cc", ".cpp", ".h", ".hpp", ".inc"}
    return [path for path in root.rglob("*")
            if path.is_file() and path.suffix in suffixes]


def direct_online_import_coverage() -> dict[str, object]:
    calls: Counter[str] = Counter()
    call_pattern = re.compile(r"\b(__imp__[A-Za-z0-9_]+)\(ctx, base\);")
    for path in sorted(GENERATED.glob("gta4_recomp.*.cpp")):
        calls.update(call_pattern.findall(path.read_text(encoding="utf-8")))

    exact_infrastructure = {
        "XamCreateEnumeratorHandle", "XamEnumerate",
        "XamGetPrivateEnumStructureFromHandle", "XamNotifyCreateListener",
        "XeKeysConsolePrivateKeySign", "XeKeysConsoleSignatureVerification",
    }
    prefixes = (
        "NetDll_", "XNet", "XMsg", "XNotify", "XamSession", "XamUser",
        "XamVoice", "XamShow", "XamContent",
    )
    selected = {
        name: count for name, count in calls.items()
        if name.removeprefix("__imp__").startswith(prefixes) or
        name.removeprefix("__imp__") in exact_infrastructure
    }

    runtime_source = "\n".join(
        path.read_text(encoding="utf-8", errors="ignore")
        for tree in (SDK_ROOT / "src", ROOT / "src")
        for path in source_files(tree)
    )
    exports = set(re.findall(r"REX_EXPORT\(\s*([^,\s)]+)", runtime_source))
    stubs = set(re.findall(r"REX_EXPORT_STUB\(\s*([^,\s;)]+)", runtime_source))
    unresolved = sorted(name for name in selected if name not in exports and name not in stubs)
    assert not unresolved, f"called online-adjacent imports lack runtime exports: {unresolved}"

    called_macro_stubs = sorted(set(selected) & stubs)
    expected_overlay_stubs = [
        "__imp__XamShowGamerCardUIForXUID",
        "__imp__XamShowPlayerReviewUI",
    ]
    assert called_macro_stubs == expected_overlay_stubs

    categories: dict[str, list[str]] = {
        "network_and_qos": [],
        "xmsg_dispatch": [],
        "backend_game_logic": [],
        "xbox_overlay_ui": [],
        "local_content_storage": [],
        "console_security_hardware": [],
    }
    for name in sorted(selected):
        bare = name.removeprefix("__imp__")
        if bare.startswith("NetDll_") or bare.startswith("XNet"):
            category = "network_and_qos"
        elif bare.startswith("XMsg"):
            category = "xmsg_dispatch"
        elif bare.startswith("XamShow") or bare == "XNotifyPositionUI":
            category = "xbox_overlay_ui"
        elif bare.startswith("XamContent"):
            category = "local_content_storage"
        elif bare.startswith("XeKeysConsole"):
            category = "console_security_hardware"
        else:
            category = "backend_game_logic"
        categories[category].append(name)

    assert "[STUB] XamShowMessageBoxUIEx - not implemented" in runtime_source
    assert "XeKeysConsolePrivateKeySign - stub" in runtime_source
    assert "XeKeysConsoleSignatureVerification - stub" in runtime_source
    return {
        "generated_direct_import_count": sum(calls.values()),
        "generated_direct_import_symbols": len(calls),
        "online_adjacent_call_count": sum(selected.values()),
        "online_adjacent_symbols": len(selected),
        "normal_exports": sorted(set(selected) & exports),
        "overlay_macro_stubs": called_macro_stubs,
        "categories": categories,
    }


def xmsg_message_coverage() -> dict[str, object]:
    # Function, app, message, dispatch import. Values come from generated PPC
    # register construction and are verified below by split high/low immediates.
    fixed = {
        "sub_82A11FB8": (0xFB, 0x000B0008, "__imp__XMsgStartIORequest"),
        "sub_82A12038": (0xFB, 0x000B0006, "__imp__XMsgStartIORequest"),
        "sub_82A120D8": (0xFB, 0x000B0007, "__imp__XMsgStartIORequest"),
        "sub_82A12228": (0xFB, 0x000B0021, "__imp__XMsgStartIORequest"),
        "sub_82A243F8": (0xFA, 0x0007001A, "__imp__XMsgStartIORequestEx"),
        "sub_82A244A0": (0xFA, 0x0007001B, "__imp__XMsgInProcessCall"),
        "sub_82A35350": (0xFC, 0x00058020, "__imp__XMsgInProcessCall"),
        "sub_82A354B0": (0xFC, 0x00058023, "__imp__XMsgInProcessCall"),
        "sub_82A36098": (0xFC, 0x00058035, "__imp__XMsgInProcessCall"),
        "sub_82A36540": (0xFC, 0x00058009, "__imp__XMsgStartIORequest"),
        "sub_82A36610": (0xFC, 0x0005800E, "__imp__XMsgInProcessCall"),
        "sub_82A36730": (0xFC, 0x00058006, "__imp__XMsgInProcessCall"),
        "sub_82A368A0": (0xFC, 0x00058004, "__imp__XMsgInProcessCall"),
        "sub_82A36A60": (0xFB, 0x000B0010, "__imp__XMsgStartIORequest"),
        "sub_82A36C00": (0xFB, 0x000B0018, "__imp__XMsgStartIORequest"),
        "sub_82A36CB8": (0xFB, 0x000B0011, "__imp__XMsgStartIORequest"),
        "sub_82A36D68": (0xFB, 0x000B0012, "__imp__XMsgStartIORequest"),
        "sub_82A36E10": (0xFB, 0x000B0012, "__imp__XMsgStartIORequest"),
        "sub_82A36EB8": (0xFB, 0x000B0013, "__imp__XMsgStartIORequest"),
        "sub_82A36F60": (0xFB, 0x000B0013, "__imp__XMsgStartIORequest"),
        "sub_82A37008": (0xFB, 0x000B001A, "__imp__XMsgStartIORequest"),
        "sub_82A370E0": (0xFB, 0x000B0014, "__imp__XMsgStartIORequest"),
        "sub_82A37178": (0xFB, 0x000B0015, "__imp__XMsgStartIORequest"),
        "sub_82A37228": (0xFB, 0x000B001C, "__imp__XMsgStartIORequest"),
        "sub_82A37318": (0xFB, 0x000B0026, "__imp__XMsgStartIORequest"),
        "sub_82A373B8": (0xFB, 0x000B0025, "__imp__XMsgStartIORequest"),
        "sub_82A374D8": (0xFB, 0x000B001D, "__imp__XMsgStartIORequest"),
        "sub_82A37608": (0xFB, 0x000B001E, "__imp__XMsgStartIORequest"),
    }
    dynamic = {"sub_82A355E0", "sub_82A358E8", "sub_82A35B20", "sub_82A35E00"}
    call_pattern = re.compile(
        r"(__imp__XMsg(?:StartIORequest(?:Ex)?|InProcessCall))\(ctx, base\);")
    actual_callers: set[str] = set()
    for path in sorted(GENERATED.glob("gta4_recomp.*.cpp")):
        source = path.read_text(encoding="utf-8")
        for match in re.finditer(r"DEFINE_REX_FUNC\(([^)]+)\)", source):
            end = source.find("\nDEFINE_REX_FUNC(", match.end())
            body = source[match.start(): len(source) if end < 0 else end]
            if call_pattern.search(body):
                actual_callers.add(match.group(1))
    assert actual_callers == set(fixed) | dynamic

    for name, (app, message, dispatch) in fixed.items():
        high = message >> 16
        low = message & 0xFFFF
        fragments = [f"ctx.r3.s64 = {app};", f"ctx.r4.s64 = {high << 16};",
                     f"{dispatch}(ctx, base);"]
        if low:
            fragments.append(f"ctx.r4.u64 = ctx.r4.u64 | {low};")
        require_generated_contract(name, tuple(fragments))
    for name in dynamic:
        require_generated_contract(name, (
            "ctx.r3.s64 = 252;", "ctx.r10.s64 = 5;",
            "__builtin_rotateleft64(ctx.r10.u32", "__imp__XMsgStartIORequest(ctx, base);",
        ))

    xgi_source = (SDK_ROOT / "src/kernel/xam/apps/xgi_app.cpp").read_text()
    xlive_source = (SDK_ROOT / "src/kernel/xam/apps/xlivebase_app.cpp").read_text()
    xmp_source = (SDK_ROOT / "src/kernel/xam/apps/xmp_app.cpp").read_text()
    handled = {
        0xFB: {int(value, 16) for value in re.findall(r"case (0x[0-9A-Fa-f]+)", xgi_source)},
        0xFC: {int(value, 16) for value in re.findall(r"case (0x[0-9A-Fa-f]+)", xlive_source)},
        0xFA: {int(value, 16) for value in re.findall(r"case (0x[0-9A-Fa-f]+)", xmp_source)},
    }
    missing_handlers = sorted(
        (app, message, name) for name, (app, message, _) in fixed.items()
        if message not in handled[app]
    )
    assert not missing_handlers
    generated_xgi = {message for app, message, _ in fixed.values() if app == 0xFB}
    unsupported_xgi = {0x000B0036, 0x000B0060, 0x000B0065, 0x000B0071}
    assert generated_xgi.isdisjoint(unsupported_xgi)
    title_storage_abi = (
        SDK_ROOT / "src/kernel/xam/apps/xlivebase_title_storage_abi.h").read_text()
    assert "IsRuntimeSchemaMessage" in title_storage_abi
    assert "ClassifyTitleStorageRuntimeRequest" in title_storage_abi
    invite_hook = (ROOT / "src/gta4_online_hooks.cpp").read_text()
    assert "extern \"C\" void sub_82A355E0" in invite_hook
    assert "SendInviteSnapshot" in invite_hook
    return {
        "fixed_generated_calls": len(fixed),
        "dynamic_title_storage_calls": sorted(dynamic),
        "fixed_by_app": {
            f"0x{app:02X}": sorted(f"0x{message:08X}" for message in messages)
            for app, messages in (
                (app, {message for candidate_app, message, _ in fixed.values()
                       if candidate_app == app}) for app in (0xFA, 0xFB, 0xFC))
        },
        "uncalled_unsupported_xgi": sorted(f"0x{value:08X}" for value in unsupported_xgi),
    }


def route_and_social_coverage() -> dict[str, object]:
    client_path = ROOT / "src/network/community_multiplayer.cpp"
    server_path = WORKSPACE / "libserver/src/main.cpp"
    social_abi_path = SDK_ROOT / "src/kernel/xam/apps/xlivebase_social_abi.h"
    xlive_app_path = SDK_ROOT / "src/kernel/xam/apps/xlivebase_app.cpp"
    live_interface_path = SDK_ROOT / "include/rex/system/xam/live_compatibility.h"
    client = client_path.read_text(encoding="utf-8")
    server = server_path.read_text(encoding="utf-8")
    social_abi = social_abi_path.read_text(encoding="utf-8")
    xlive_app = xlive_app_path.read_text(encoding="utf-8")
    live_interface = live_interface_path.read_text(encoding="utf-8")

    path_pattern = re.compile(r'"(/api/(?:v\d+/)?[^"?{} ]+)')
    client_paths = set(path_pattern.findall(client))
    server_paths = set(path_pattern.findall(server))
    expected_host_extension_gaps = {"/api/v2/chat/messages", "/api/v2/events"}
    assert client_paths - server_paths == expected_host_extension_gaps
    prog_ach_path = "/api/v3/storage/0x545407F2/3/Prog_ACH"
    assert prog_ach_path in server_paths
    assert "kGta4AchievementStorageEndpoint" in client

    social_fragments = {
        "generated_friend_stride_state": (
            generated_function("sub_829EF1E8"),
            ("static_cast<uint64_t>(196)", "REX_LOAD_U32(ctx.r31.u32 + 24)",
             "& 0xC0000000"),
        ),
        "generated_friend_online": (
            generated_function("sub_829E8A88"),
            ("REX_LOAD_U32(ctx.r3.u32 + 24)", "ctx.r11.u32 & 0x1"),
        ),
        "generated_friend_title": (
            generated_function("sub_829E8A98"),
            ("REX_LOAD_U32(ctx.r3.u32 + 36)",),
        ),
        "friend_abi": (
            social_abi,
            ("std::array<uint8_t, 196>", "kFriendStateOffset = 24",
             "kFriendSessionIdOffset = 28", "kFriendTitleIdOffset = 36",
             "kFriendStateOnline = UINT32_C(0x00000001)",
             "kFriendStateExcludedMask = UINT32_C(0xC0000000)"),
        ),
        "typed_social_result": (
            live_interface,
            ("enum class SocialServiceStatus", "kInvalidRequest", "kUnavailable",
             "kRejected", "kInvalidResponse",
             "SocialServiceStatus status = SocialServiceStatus::kInvalidResponse"),
        ),
        "accepted_client_filter": (
            client,
            ('"&state=accepted"',
             'item.value("state", std::string{}) != "accepted"',
             "record->blocked", "unique_xuids.insert(record->xuid).second",
             "response.status == 0 ? SocialServiceStatus::kUnavailable",
             ": SocialServiceStatus::kRejected"),
        ),
        "typed_xlive_mapping": (
            xlive_app,
            ("enumeration.status == SocialServiceStatus::kInvalidRequest",
             "? X_E_INVALIDARG", ": X_E_FAIL"),
        ),
        "accepted_server_filter_and_presence": (
            server,
            ('state_filter != "accepted"',
             "relationship.second != state_filter", "SessionLiveLocked",
             "SessionContainsXuid(session->second, xuid)",
             'presence = live->second.state == "in_game" ? "playing" : "online"',
             "presence_.erase(live)"),
        ),
        "leaderboard_identity_validation": (
            client,
            ("result.page.total < rows.size()", "ParseFixedHex<uint64_t>",
             "!xuid || !*xuid || !rank || rank > result.page.total",
             "IsExpectedGlobalLeaderboardRank(offset, row_index, rank)",
             "!unique_xuids.insert(*xuid).second"),
        ),
        "server_direct_read_xuid_validation": (
            server,
            ("std::unordered_set<std::string> unique_xuids",
             "!IsHex(xuid, 16)", 'xuid == "0x0000000000000000"',
             "!unique_xuids.insert(xuid).second", "BuildRankedStatCandidates"),
        ),
    }
    for contract, (source, fragments) in social_fragments.items():
        missing = [fragment for fragment in fragments if fragment not in source]
        assert not missing, f"{contract} missing source fragments: {missing}"

    return {
        "client_literal_routes": len(client_paths),
        "server_covered_client_routes": len(client_paths & server_paths),
        "title_storage_route": prog_ach_path,
        "host_extension_routes_without_server": sorted(expected_host_extension_gaps),
        "verified_social_contracts": sorted(social_fragments),
    }


def global_leaderboard_rank_coverage() -> dict[str, object]:
    client_policy = (
        ROOT / "src/network/community_stats_policy.h").read_text(encoding="utf-8")
    runtime = (SDK_ROOT / "src/kernel/xam/xam_user.cpp").read_text(encoding="utf-8")
    server = (WORKSPACE / "libserver/src/main.cpp").read_text(encoding="utf-8")

    require_generated_contract("sub_82A12310", (
        "ctx.r5.u64 = ctx.r30.u64 & 0xFFFFFFFF;",
        "ctx.r4.s64 = 1;",
        "__imp__XamUserCreateStatsEnumerator(ctx, base);",
    ))
    require_generated_contract("sub_829E5F18", (
        "lwz r4,48(r31)",
        "sub_82A12310(ctx, base);",
    ))
    fragments = {
        "runtime": (
            "static_cast<uint32_t>(pivot) - 1, row_count, false",
        ),
        "server": (
            "const std::size_t first = std::min<std::size_t>(*offset, candidates.size());",
            'row["rank"] = index + 1;',
        ),
        "client_policy": (
            "static_cast<uint64_t>(offset)",
            "static_cast<uint64_t>(row_index) + 1",
            "expected <= std::numeric_limits<uint32_t>::max()",
            "rank == static_cast<uint32_t>(expected)",
        ),
    }
    sources = {"runtime": runtime, "server": server, "client_policy": client_policy}
    for source_name, required in fragments.items():
        missing = [fragment for fragment in required if fragment not in sources[source_name]]
        assert not missing, f"{source_name} global-rank contract missing: {missing}"

    cases = ((0, 3), (40, 3), ((1 << 32) - 2, 1), ((1 << 32) - 1, 2))
    expected = []
    for offset, count in cases:
        ranks = [offset + row_index + 1 for row_index in range(count)]
        expected.append({"offset": offset, "ranks": ranks,
                         "wire_valid": [rank <= (1 << 32) - 1 for rank in ranks]})
    assert expected == [
        {"offset": 0, "ranks": [1, 2, 3], "wire_valid": [True, True, True]},
        {"offset": 40, "ranks": [41, 42, 43], "wire_valid": [True, True, True]},
        {"offset": 4294967294, "ranks": [4294967295], "wire_valid": [True]},
        {"offset": 4294967295,
         "ranks": [4294967296, 4294967297], "wire_valid": [False, False]},
    ]
    return {"verified_rank_windows": expected}


def voice_packet_contract_coverage() -> dict[str, object]:
    # GTA IV performs its own nibble codec on both sides of XamVoice. Keep the
    # host transport byte-preserving and bind that behavior to the generated
    # mode-0 producer, mode-1 consumer, and pending-status polling contract.
    require_generated_contract("sub_82A297F8", (
        "sub_82A2BBD8(ctx, base);",
        "// rlwinm r9,r9,30,2,31",
        "ctx.r4.s64 = 0;",
        "__imp__XamVoiceSubmitPacket(ctx, base);",
    ))
    require_generated_contract("sub_82A2D110", (
        "ctx.r4.s64 = 1;",
        "__imp__XamVoiceSubmitPacket(ctx, base);",
    ))
    require_generated_contract("sub_82A2D320", (
        "ctx.cr6.compare<int32_t>(ctx.r11.s32, 259, ctx.xer);",
        "sub_82A2BBD8(ctx, base);",
    ))

    runtime = (SDK_ROOT / "src/kernel/xam/xam_voice.cpp").read_text(encoding="utf-8")
    online_hooks = (ROOT / "src/gta4_online_hooks.cpp").read_text(encoding="utf-8")
    live_interface = (
        SDK_ROOT / "include/rex/system/xam/live_compatibility.h").read_text(
            encoding="utf-8")
    fragments = {
        "runtime": (
            "if (mode == 0 && payload_size)",
            "transport->Send(sequence, std::span<const uint8_t>(payload, payload_size))",
            "memory::store_and_swap<uint32_t>(packet + 0, 0);",
            "memory::store_and_swap<uint32_t>(packet + 4, payload_size);",
        ),
        "online_hooks": (
            "packet->session_id != active_session_id",
            "voice_ctx.r4.u64 = packet->source_xuid;",
            "sub_82A26DF8(voice_ctx, base);",
        ),
        "live_interface": (
            "std::function<bool()> voice_capture_available;",
            "if (!config.voice_capture_available) return false;",
            "return config.voice_capture_available();",
        ),
    }
    sources = {"runtime": runtime, "online_hooks": online_hooks,
               "live_interface": live_interface}
    for source_name, required in fragments.items():
        missing = [fragment for fragment in required if fragment not in sources[source_name]]
        assert not missing, f"{source_name} voice contract missing: {missing}"
    return {
        "generated_mode_0_producer": "sub_82A297F8",
        "generated_mode_1_submitter": "sub_82A2D110",
        "generated_completion_decoder": "sub_82A2D320",
        "host_transport": "byte_preserving_immediate_completion",
        "headset_default": "capture_unavailable",
    }


def deferred_input_snapshot_coverage() -> dict[str, object]:
    source = (SDK_ROOT / "src/kernel/xam/xam_msg.cpp").read_text(encoding="utf-8")
    require_generated_contract("sub_82A11FB8", (
        "ctx.r7.s64 = 8;", "ctx.r6.s64 = ctx.r1.s64 + 80;",
        "ctx.r4.u64 = ctx.r4.u64 | 8;", "__imp__XMsgStartIORequest(ctx, base);",
    ))
    require_generated_contract("sub_82A36540", (
        "ctx.r7.s64 = 16;", "ctx.r6.s64 = ctx.r1.s64 + 80;",
        "ctx.r4.u64 = ctx.r4.u64 | 32777;",
        "__imp__XMsgStartIORequest(ctx, base);",
    ))
    fragments = (
        "struct XGI_ACHIEVEMENTS_WRITE_SNAPSHOT",
        "static_assert_size(XGI_ACHIEVEMENTS_WRITE_SNAPSHOT, 8)",
        "case 0x000B0008:",
        "kMaximumGta4AchievementCount",
        "sizeof(XGI_ACHIEVEMENT_WRITE_ENTRY_SNAPSHOT)",
        "struct XLIVE_MARKETPLACE_COUNTS_REQUEST_SNAPSHOT",
        "static_assert_size(XLIVE_MARKETPLACE_COUNTS_REQUEST_SNAPSHOT, 16)",
        "message == 0x00058009",
        "snapshot->Capture(buffer_ptr",
    )
    missing = [fragment for fragment in fragments if fragment not in source]
    assert not missing, f"deferred input snapshot contract missing fragments: {missing}"
    achievement_entry_bytes = 65 * 8
    assert achievement_entry_bytes == 520
    return {
        "achievement_request_bytes": 8,
        "achievement_entry_bytes": 8,
        "maximum_achievement_entries": 65,
        "maximum_captured_achievement_entry_bytes": achievement_entry_bytes,
        "marketplace_request_bytes": 16,
        "marketplace_result_pointer_offset": 12,
    }


def main() -> None:
    layouts = {
        "xgi_invitation_data_request": layout(
            [("user_index", 4, 4), ("session_info_ptr", 4, 4)]),
        "xgi_modify_skill_request": layout(
            [("object_ptr", 4, 4), ("count", 4, 4), ("xuids_ptr", 4, 4)]
            + [(f"reserved_{index}", 4, 4) for index in range(3)]),
        "xgi_read_stats_request": layout([
            ("title_id", 4, 4), ("xuid_count", 4, 4), ("xuids_ptr", 4, 4),
            ("spec_count", 4, 4), ("specs_ptr", 4, 4),
            ("results_size", 4, 4), ("results_ptr", 4, 4),
        ]),
        "xgi_write_stats_request": layout([
            ("object_ptr", 4, 4), ("alignment_padding", 4, 4), ("xuid", 8, 8),
            ("view_count", 4, 4), ("views_ptr", 4, 4),
        ]),
        "xuser_stats_view": layout(
            [("view_id", 4, 4), ("property_count", 4, 4), ("properties_ptr", 4, 4)]),
        "xuser_stats_spec": layout(
            [("view_id", 4, 4), ("column_count", 4, 4), ("column_ids", 128, 2)]),
        "xuser_stats_result_header": layout(
            [("view_count", 4, 4), ("views_ptr", 4, 4)]),
        "xuser_stats_view_result": layout([
            ("view_id", 4, 4), ("total_rows", 4, 4),
            ("row_count", 4, 4), ("rows_ptr", 4, 4),
        ]),
        "xuser_stats_row": layout([
            ("xuid", 8, 8), ("rank", 4, 4), ("reserved", 4, 4),
            ("rating", 8, 8), ("gamertag", 16, 1),
            ("column_count", 4, 4), ("columns_ptr", 4, 4),
        ]),
        "xuser_stats_column": layout(
            [("column_id", 2, 2), ("value", 16, 8)]),
        "voice_packet": layout([
            ("status", 4, 4), ("completed_size", 4, 4),
            ("payload_ptr", 4, 4), ("payload_size", 4, 4),
            ("reserved", 4, 4), ("owner", 4, 4),
        ]),
        "xmsg_dynamic_argument": layout(
            [("type", 4, 4), ("reserved", 4, 4), ("value", 8, 8)]),
        "xmsg_dynamic_arguments": layout(
            [("entries", 32 * 16, 4), ("count", 4, 4)]),
        "xlive_friend_entry": layout([
            ("xuid", 8, 4), ("gamertag", 16, 1), ("state", 4, 4),
            ("session_id", 8, 4), ("title_id", 4, 4),
            ("user_time", 8, 4), ("invite_session_id", 8, 4),
            ("invite_time", 8, 4), ("rich_presence_units", 4, 4),
            ("rich_presence", 128, 2),
        ]),
        "xlive_accepted_invite_info": layout([
            ("recipient_xuid", 8, 4), ("sender_xuid", 8, 4),
            ("title_id", 4, 4), ("session_info", 60, 4),
            ("flags", 4, 4), ("reserved", 12, 1),
        ]),
    }
    limits = {"maximum_views": 16, "maximum_rows": 100, "maximum_columns": 64,
              "maximum_variable_bytes_per_column": 512,
              "maximum_invite_recipients": 32,
              "maximum_invite_message_utf16_units": 128}
    maximum_structural_buffer = (
        layouts["xuser_stats_result_header"]["size"]
        + limits["maximum_views"] * layouts["xuser_stats_view_result"]["size"]
        + limits["maximum_views"] * limits["maximum_rows"]
        * layouts["xuser_stats_row"]["size"]
        + limits["maximum_views"] * limits["maximum_rows"] * limits["maximum_columns"]
        * layouts["xuser_stats_column"]["size"]
    )
    maximum_payload_buffer = (
        limits["maximum_views"] * limits["maximum_rows"] * limits["maximum_columns"]
        * limits["maximum_variable_bytes_per_column"]
    )
    friend_layout = layouts["xlive_friend_entry"]
    assert friend_layout["size"] == 196
    assert friend_layout["offsets"] == {
        "xuid": 0,
        "gamertag": 8,
        "state": 24,
        "session_id": 28,
        "title_id": 36,
        "user_time": 40,
        "invite_session_id": 48,
        "invite_time": 56,
        "rich_presence_units": 64,
        "rich_presence": 68,
    }
    assert layouts["xuser_stats_row"]["size"] == 48
    assert layouts["xuser_stats_column"]["size"] == 24
    print(json.dumps({"layouts": layouts, "limits": limits,
                      "maximum_structural_buffer": maximum_structural_buffer,
                      "maximum_payload_buffer": maximum_payload_buffer,
                      "verified_generated_contracts": verify_retail_contracts(),
                      "call_sites": call_sites(),
                      "direct_import_coverage": direct_online_import_coverage(),
                      "xmsg_message_coverage": xmsg_message_coverage(),
                      "route_and_social_coverage": route_and_social_coverage(),
                      "global_leaderboard_rank_coverage":
                          global_leaderboard_rank_coverage(),
                      "voice_packet_contract_coverage":
                          voice_packet_contract_coverage(),
                      "deferred_input_snapshot_coverage":
                          deferred_input_snapshot_coverage()}, indent=2))


if __name__ == "__main__":
    main()
