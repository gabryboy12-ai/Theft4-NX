#!/usr/bin/env python3
"""Enforce gameplay/UI multiplayer width classifications outside core networking."""

from __future__ import annotations

import pathlib
import re


ROOT = pathlib.Path(__file__).resolve().parents[1]
GENERATED = ROOT / "generated"
TRUSTED = ROOT.parents[2] / "gta_iv" / "xex_excavation_retail"
HOOKS = (ROOT / "src" / "gta4_multiplayer_64_hooks.cpp").read_text(encoding="utf-8")
FUNCTION_RE = re.compile(
    r"DEFINE_REX_FUNC\((sub_([0-9A-Fa-f]{8}))\) \{(.*?)(?=\nDEFINE_REX_FUNC\(|\Z)",
    re.DOTALL,
)
LITERAL_SIXTEEN_RE = re.compile(
    r"// (?:cmp(?:l)?wi cr\d+,r\d+,16|li r\d+,16)$", re.MULTILINE
)

SESSION_UI_CLASSIFICATIONS = {
    "sub_826C6BE0": "session-state value 16, not a player capacity",
    "sub_826C6C88": "session-state value 16, not a player capacity",
    "sub_826C6DA0": "fixed UI text copy length",
    "sub_826C7188": "identity lookup routes through widened sub_826FE908",
    "sub_826C7E98": "STARTING_GAME high-peer notification/action expansion hook",
    "sub_826C8A30": "session-state value 16 and diagnostic labels",
    "sub_826C9AF0": "kick flow uses widened key and identity lookups",
    "sub_826C9FE8": "session request field value, not a player capacity",
    "sub_826CA1A8": "kick-vote flow uses widened identity lookup",
    "sub_826CA280": "kick flow uses widened identity lookup",
    "sub_826CC2F8": "notification text copy length",
    "sub_826C1080": "shader constant width outside multiplayer",
    "sub_826C2CC0": "two fixed sixteen-byte inline strings",
    "sub_826C3010": "leaderboard parser key width",
    "sub_826C34A0": "leaderboard column/string width",
    "sub_826C38F0": "leaderboard schema column construction",
    "sub_826C3D10": "leaderboard schema/string construction",
    "sub_826C4FA0": "connection-key lookup routes through widened sub_826FE808",
    "sub_826C5038": "allocator/alignment argument, not a player capacity",
}

KICK_FUNCTIONS = {"sub_826C75F0", "sub_826C9AF0", "sub_826CA1A8", "sub_82A673D8"}


def main() -> None:
    bodies: dict[str, str] = {}
    ui_width_consumers: set[str] = set()
    for path in GENERATED.glob("gta4_recomp.*.cpp"):
        text = path.read_text(encoding="utf-8")
        for match in FUNCTION_RE.finditer(text):
            function, address_text, body = match.groups()
            bodies[function] = body
            address = int(address_text, 16)
            if not 0x826C0000 <= address < 0x826D0000:
                continue
            has_sixteen = bool(
                re.search(r"// (?:cmp(?:l)?wi cr\d+,r\d+,16|li r\d+,16)$", body, re.MULTILINE)
            )
            widened_lookup = any(
                seam in body
                for seam in (
                    "sub_826FE808(ctx, base);",
                    "sub_826FE908(ctx, base);",
                    "sub_826FE9C8(ctx, base);",
                )
            )
            if has_sixteen or widened_lookup:
                ui_width_consumers.add(function)

    canonical_callers = {
        function for function, body in bodies.items() if "sub_8225CF68(ctx, base);" in body
    }

    failures: list[str] = []
    unexpected = sorted(ui_width_consumers - SESSION_UI_CLASSIFICATIONS.keys())
    missing = sorted(SESSION_UI_CLASSIFICATIONS.keys() - ui_width_consumers)
    if unexpected:
        failures.append("unclassified session/UI width consumers: " + ", ".join(unexpected))
    if missing:
        failures.append("classified session/UI consumers disappeared: " + ", ".join(missing))

    for function in sorted(ui_width_consumers):
        print(f"{function}: {SESSION_UI_CLASSIFICATIONS[function]}")

    if 'extern "C" void sub_826C7E98(' not in HOOKS or (
        "kStartGameMessageSinkAddress" not in HOOKS
    ):
        failures.append("STARTING_GAME still has only its sixteen-pointer retail batch")
    if 'extern "C" void sub_82786088(' not in HOOKS or "WithExtendedReassignmentOwner" not in HOOKS:
        failures.append("ready/reassignment status path is not projected for high peers")
    if "sub_82778B98" not in bodies or re.search(
        r"// cmp(?:l)?wi cr\d+,r\d+,16$", bodies["sub_82778B98"], re.MULTILINE
    ):
        failures.append("spectator debug/player-object path gained an unclassified peer ceiling")

    for function in KICK_FUNCTIONS:
        if function not in bodies:
            failures.append(f"kick-path function missing: {function}")
    if "sub_826FE808(ctx, base);" not in bodies["sub_826C9AF0"] or (
        "sub_826FE908(ctx, base);" not in bodies["sub_826C9AF0"]
    ):
        failures.append("kick selection stopped using widened peer identity/key lookups")
    if "sub_826FE908(ctx, base);" not in bodies["sub_826CA1A8"]:
        failures.append("kick-vote selection stopped using widened identity lookup")
    if re.search(r"// cmp(?:l)?wi cr\d+,r\d+,16$", bodies["sub_82A673D8"], re.MULTILINE):
        failures.append("MsgKickPlayer constructor gained an unclassified sixteen-player limit")

    # Trusted metadata identifies the named presentation/native surfaces.  The
    # generated bodies then prove whether any one of them still carries a fixed
    # player ceiling; capacity-sensitive identity access must reach a widened
    # lookup or the sidecar-aware canonical player-info accessor.
    function_class_map = (TRUSTED / "function_class_map.txt").read_text(encoding="utf-8")
    radar_methods = set(
        re.findall(
            r"0x[0-9A-Fa-f]{8}\s+(sub_[0-9A-Fa-f]{8})\s+→\s+"
            r"(?:CViewportRadar|CCamRadar)::vfunc\[\d+\]",
            function_class_map,
        )
    )
    expected_radar_methods = {
        "sub_82155680",
        "sub_82155770",
        "sub_826057A8",
        "sub_82605850",
        "sub_826058E0",
    }
    if radar_methods != expected_radar_methods:
        failures.append(
            "trusted radar method set changed: " + ", ".join(sorted(radar_methods))
        )
    radar_non_player_sixteen = {"sub_826058E0": "sixteen-byte camera transform vector"}
    for function in radar_methods:
        if function not in bodies:
            failures.append(f"radar method missing from generated code: {function}")
        elif LITERAL_SIXTEEN_RE.search(bodies[function]) and function not in radar_non_player_sixteen:
            failures.append(f"radar method gained a sixteen-player loop: {function}")
    if not re.search(r"// li r\d+,16$", bodies["sub_826058E0"], re.MULTILINE) or re.search(
        r"// cmp(?:l)?wi cr\d+,r\d+,16$", bodies["sub_826058E0"], re.MULTILINE
    ):
        failures.append("CCamRadar vector-size sixteen changed classification")
    if "sub_82777590" not in canonical_callers:
        failures.append("CNetObjPlayer radar/HUD state bridge lost canonical player-info lookup")

    func_strings = (TRUSTED / "func_string_refs.txt").read_text(encoding="utf-8")
    trusted_string_evidence = {
        "scoreboard_rank_rows": ("sub_821680E8", '"PLRankIcons"'),
        "scoreboard_rank_rows_alt": ("sub_82168370", '"PLRankIcons"'),
        "script_player_native_table": ("sub_825B7008", '"GET_PLAYER_NAME"'),
        "game_mode_team_schema": ("sub_826C3D10", '"gameModeId"'),
        "peer_team_player_list": ("sub_82700108", '"ADDING_PEER"'),
        "spectator_player_state": ("sub_82778B98", '"Spectating\\t\\t- %s"'),
        "ready_player_list": ("sub_82786088", '"NOT_READY"'),
        "kick_player_list": ("sub_826C75F0", '"MP_KICK_PEER"'),
    }
    for category, (function, literal) in trusted_string_evidence.items():
        block = re.search(
            rf"^{function}\s+\[\d+ strings\]\n(?P<body>(?:  \".*\"\n)+)",
            func_strings,
            re.MULTILINE,
        )
        if block is None or literal not in block.group("body"):
            failures.append(f"trusted string evidence disappeared for {category}: {function}")

    script_player_callers = {
        function
        for function in canonical_callers
        if 0x825B2BE8 <= int(function.removeprefix("sub_"), 16) <= 0x825B6F30
    }
    if not script_player_callers:
        failures.append("script-player native handlers no longer reach canonical player-info")
    if len(script_player_callers) != 113:
        failures.append(
            f"script-player canonical-accessor set changed: expected 113, "
            f"found {len(script_player_callers)}"
        )
    for function in script_player_callers:
        if LITERAL_SIXTEEN_RE.search(bodies[function]) and function != "sub_825B2D70":
            failures.append(f"script-player native gained an unclassified 16-player loop: {function}")

    required_hooks_by_surface = {
        "scoreboard_player_list_nametags": ("sub_826FE808", "sub_826FE908", "sub_826FE9C8"),
        "team_and_peer_player_arrays": ("sub_82700108", "sub_82263000"),
        "ped_player_creation_destruction": ("sub_822619E8", "sub_82261E48", "sub_82261EA8"),
        "game_mode_free_roam_start": ("sub_826C7E98",),
        "ready_reassignment": ("sub_82786088",),
    }
    for category, functions in required_hooks_by_surface.items():
        for function in functions:
            if f'extern "C" void {function}(' not in HOOKS:
                failures.append(f"{category} lost required high-player hook {function}")

    if failures:
        raise RuntimeError("\n".join(failures))
    print(f"session_ui_width_coverage={len(ui_width_consumers)}/{len(ui_width_consumers)}")
    print("kick_ready_spectator_player_list_coverage=4/4")
    print(f"radar_class_width_coverage={len(radar_methods)}/{len(expected_radar_methods)}")
    print(f"script_player_native_accessor_coverage={len(script_player_callers)}")
    print("scoreboard_player_list_nametag_team_script_hud_lifecycle_mode_coverage=10/10")


if __name__ == "__main__":
    main()
