#!/usr/bin/env python3
"""Audit GTA IV's opaque XSession advertisement and search authority."""

from __future__ import annotations

import json
import re
from pathlib import Path


GTA_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = GTA_ROOT.parents[2]
GENERATED = GTA_ROOT / "generated"
XSESSION_HEADER = GTA_ROOT.parent / "include/rex/system/xam/xsession.h"
XGI_APP = GTA_ROOT.parent / "src/kernel/xam/apps/xgi_app.cpp"
LIVE_HEADER = GTA_ROOT.parent / "include/rex/system/xam/live_compatibility.h"
LIVE_SOURCE = GTA_ROOT.parent / "src/system/xam/live_compatibility.cpp"
CLIENT = GTA_ROOT / "src/network/community_multiplayer.cpp"
SERVER = REPO_ROOT / "libserver/src/main.cpp"
RETAIL_STRINGS = REPO_ROOT / "gta_iv/xex_excavation_retail/all_strings_with_addrs.txt"


def ppc_lis_ori(lis_immediate: int, low_immediate: int) -> int:
    return ((lis_immediate & 0xFFFF) << 16) | (low_immediate & 0xFFFF)


def function_body(source: str, name: str) -> str:
    marker = f"DEFINE_REX_FUNC({name})"
    start = source.find(marker)
    if start < 0:
        raise AssertionError(f"missing generated function {name}")
    end = source.find("DEFINE_REX_FUNC(", start + len(marker))
    return source[start:] if end < 0 else source[start:end]


def require(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise AssertionError(f"{label}: missing {needle!r}")


def reject(text: str, needle: str, label: str) -> None:
    if needle in text:
        raise AssertionError(f"{label}: unexpected {needle!r}")


def main() -> None:
    generated_77 = (GENERATED / "gta4_recomp.77.cpp").read_text()
    generated_78 = (GENERATED / "gta4_recomp.78.cpp").read_text()
    generated_80 = (GENERATED / "gta4_recomp.80.cpp").read_text()
    xsession_header = XSESSION_HEADER.read_text()
    xgi_app = XGI_APP.read_text()
    live_header = LIVE_HEADER.read_text()
    live_source = LIVE_SOURCE.read_text()
    client = CLIENT.read_text()
    server = SERVER.read_text()
    retail_strings = RETAIL_STRINGS.read_text()

    message_ids = {
        "set_context": ppc_lis_ori(11, 6),
        "set_property": ppc_lis_ori(11, 7),
        "session_create": ppc_lis_ori(11, 16),
        "session_search_ex": ppc_lis_ori(11, 28),
    }
    expected_message_ids = {
        "set_context": 0x000B0006,
        "set_property": 0x000B0007,
        "session_create": 0x000B0010,
        "session_search_ex": 0x000B001C,
    }
    if message_ids != expected_message_ids:
        raise AssertionError(f"unexpected XGI message arithmetic: {message_ids}")
    for message_id in message_ids.values():
        require(xgi_app, f"case 0x{message_id:08X}", "XGI dispatch")

    create_wrapper = function_body(generated_80, "sub_82A36A60")
    require(create_wrapper, "// ori r4,r4,16", "generated XSessionCreate message")
    require(create_wrapper, "// li r7,28", "generated XSessionCreate request length")
    create_abi = re.search(
        r"struct XGI_SESSION_CREATE \{(?P<body>.*?)\n\};", xsession_header, re.DOTALL
    )
    search_abi = re.search(
        r"struct XGI_SESSION_SEARCH \{(?P<body>.*?)\n\};", xsession_header, re.DOTALL
    )
    if not create_abi or not search_abi:
        raise AssertionError("missing XSession ABI structures")
    reject(create_abi.group("body"), "procedure_index", "XSessionCreate ABI")
    require(search_abi.group("body"), "procedure_index", "XSessionSearch ABI")

    publish = function_body(generated_77, "sub_829E77A0")
    require(publish, "& 0xF0000000", "title setting type discriminator")
    require(publish, "sub_82A120D0(ctx, base);", "exact context publication")
    require(publish, "sub_82A12178(ctx, base);", "exact property publication")

    game_type_context = ppc_lis_ori(0, 32778)
    if game_type_context != 0x0000800A:
        raise AssertionError(f"unexpected context arithmetic: 0x{game_type_context:08X}")
    host = function_body(generated_78, "sub_829F53E8")
    search = function_body(generated_77, "sub_829EACE0")
    for body, label in ((host, "host"), (search, "search")):
        require(body, "// ori r30,r11,32778" if label == "host" else
                "// ori r10,r10,32778", f"{label} 0x800A context")
    require(host, "sub_829E77A0(ctx, base);", "host opaque title settings")
    require(host, "sub_82A36A60(ctx, base);", "host XSessionCreate")
    require(search, "// lwz r11,536(r31)", "search procedure provider")
    require(search, "REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);", "dynamic search procedure")
    if search.count("sub_82A37228(ctx, base);") != 2:
        raise AssertionError("XSessionSearchEx must have size-query and populated calls")
    require(search, "sub_829E8528(ctx, base);", "search property copy")
    require(search, "sub_829E86D0(ctx, base);", "search context copy")

    typed_property_ids = {
        ppc_lis_ori(4096, 32778),
        ppc_lis_ori(4096, 32779),
    }
    if typed_property_ids != {0x1000800A, 0x1000800B}:
        raise AssertionError(f"unexpected property arithmetic: {typed_property_ids}")
    property_builder = function_body(generated_77, "sub_829F1D88")
    require(property_builder, "// ori r28,r9,32778", "opaque typed property 0x1000800A")
    require(property_builder, "// ori r27,r9,32779", "opaque typed property 0x1000800B")

    mode_names = re.findall(r"MatchCreate::GAME_MODE_[A-Z0-9_]+", retail_strings)
    episodic_names = {name for name in mode_names if "_EPISODIC_" in name}
    base_names = set(mode_names) - episodic_names
    expected_episodic = {
        f"MatchCreate::GAME_MODE_EPISODIC_{index}" for index in range(1, 15)
    }
    expected_base = {
        "MatchCreate::GAME_MODE_FREEMODE",
        "MatchCreate::GAME_MODE_RACE_STANDARD",
        "MatchCreate::GAME_MODE_RACE_ATOB",
        "MatchCreate::GAME_MODE_RACE_DRIVEBY",
        "MatchCreate::GAME_MODE_TEAM_DEATHMATCH",
        "MatchCreate::GAME_MODE_TEAM_BASE",
        "MatchCreate::GAME_MODE_TEAM_CARSTEAL",
        "MatchCreate::GAME_MODE_TEAM_MAFYA",
        "MatchCreate::GAME_MODE_TEAM_FORT",
        "MatchCreate::GAME_MODE_TEAM_BANK",
        "MatchCreate::GAME_MODE_TEAM_VIP",
        "MatchCreate::GAME_MODE_COOP_SWAT",
        "MatchCreate::GAME_MODE_COOP_SNIPE",
        "MatchCreate::GAME_MODE_COOP_DRUGFACTORY",
        "MatchCreate::GAME_MODE_COMP_ANTAGONIZE",
        "MatchCreate::GAME_MODE_COMP_RANDEVENT",
        "MatchCreate::GAME_MODE_COMP_CARSTEAL",
        "MatchCreate::GAME_MODE_COMP_DEATHMATCH",
    }
    if episodic_names != expected_episodic or base_names != expected_base:
        raise AssertionError("retail MatchCreate mode-name inventory changed")

    session_record = re.search(
        r"struct SessionRecord \{(?P<body>.*?)\n\};", live_header, re.DOTALL
    )
    if not session_record:
        raise AssertionError("missing SessionRecord")
    reject(session_record.group("body"), "procedure_index", "advertised session state")
    reject(live_source, "session.procedure_index", "LAN/in-memory session state")
    require(client, '{"mode", "gta4"}', "generic compatibility mode")
    require(client, '{"episode", "all"}', "generic compatibility episode")

    search_method = re.search(
        r"std::vector<SessionRecord> Search\(uint32_t title_id,.*?\n  \}\n\n  "
        r"std::optional<SessionRecord> Get",
        client,
        re.DOTALL,
    )
    if not search_method:
        raise AssertionError("missing community XSession search method")
    active_search = search_method.group(0)
    for field in ("title_id", "media_id", "title_version", "protocol_version",
                  "procedure_index", "contexts", "properties", "maximum_results"):
        require(active_search, f'{{"{field}"', f"active search {field}")
    for semantic_field in ('{"mode"', '{"episode"', '{"ranked"'):
        reject(active_search, semantic_field, "active search semantic metadata")
    require(active_search, "decoded.at(\"procedure_index\")", "procedure response echo")
    reject(active_search, "session->procedure_index", "per-session procedure filter")

    server_search = re.search(
        r"Response SearchSessions\(.*?\n  \}\n\n  Response GetSession", server, re.DOTALL
    )
    if not server_search:
        raise AssertionError("missing libserver direct session search")
    direct_search = server_search.group(0)
    require(direct_search, '{{"procedure_index", *procedure_index}', "procedure response echo")
    reject(direct_search, 'session.value("procedure_index"', "stored procedure filter")
    require(direct_search, 'for (const auto& [key, value] : contexts.items())',
            "exact context subset filter")
    require(direct_search, 'for (const auto& [key, value] : properties.items())',
            "exact property subset filter")

    ranked_test_flags = 62
    if (ranked_test_flags & 0x10) == 0:
        raise AssertionError("nonzero-procedure Deathmatch test is not ranked")
    require(server, '{"procedure_index", 1}, {"contexts"',
            "ranked nonzero direct-search test")
    require(server, 'value("mode", "") != "deathmatch"',
            "ranked Deathmatch direct-search assertion")

    session_flag_bits = {
        "host": 0x001,
        "uses_presence": 0x002,
        "opaque_0x04": 0x004,
        "uses_matchmaking": 0x008,
        "uses_arbitration": 0x010,
        "opaque_0x20": 0x020,
        "join_via_presence_disabled": 0x200,
        "join_in_progress_disabled": 0x400,
    }
    observed_flag_values = (62, 46, 32, 1798)
    flag_builder = function_body(generated_78, "sub_829F5348")
    for value in observed_flag_values:
        require(flag_builder, f"// li r3,{value}", f"generated session flags {value}")
    flag_decomposition = {
        f"0x{value:08X}": [
            name for name, bit in session_flag_bits.items() if value & bit
        ]
        for value in observed_flag_values
    }

    print(json.dumps({
        "xgi_message_ids": {name: f"0x{value:08X}" for name, value in message_ids.items()},
        "opaque_discriminator_context": f"0x{game_type_context:08X}",
        "opaque_typed_property_ids": [
            f"0x{value:08X}" for value in sorted(typed_property_ids)
        ],
        "observed_session_flags": flag_decomposition,
        "retail_mode_name_counts": {
            "base_internal": len(base_names),
            "episodic_opaque": len(episodic_names),
            "total": len(set(mode_names)),
        },
        "authority": {
            "session_advertisement": "flags + exact XSession contexts/properties",
            "search_procedure": "query-only and response-echoed",
            "generic_mode_episode": "not sent by the active XSession search",
        },
    }, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
