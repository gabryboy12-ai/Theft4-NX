#!/usr/bin/env python3
"""Verify every audited retail-width multiplayer consumer has a host hook.

This complements the layout derivation script. It intentionally checks the
generated tree without modifying it, then verifies the regeneration-safe hook
translation unit owns each direct 32-participant or migration-snapshot seam.
"""

from pathlib import Path
import re


ROOT = Path(__file__).parents[1]
HOOKS = ROOT / "src" / "gta4_multiplayer_64_hooks.cpp"
SESSION_GENERATED = ROOT / "generated" / "gta4_recomp.78.cpp"
FUNCTION_RE = re.compile(r"DEFINE_REX_FUNC\((sub_[0-9A-Fa-f]{8})\)")

REQUIRED_COMMAND_AND_SNAPSHOT_HOOKS = {
    "sub_827CD7B0",
    "sub_827CFD20",
    "sub_827CFE18",
    "sub_827D04D8",
    "sub_827D0D00",
    "sub_82801730",
    "sub_828019F8",
    "sub_82807308",
    "sub_828076C8",
    "sub_828086B8",
    "sub_82808720",
    "sub_829F52E8",
    "sub_829F58B8",
    "sub_829F6080",
    "sub_829F6AA8",
    "sub_829F7208",
    "sub_829F7428",
    "sub_829F8710",
    "sub_829F8F40",
    "sub_829F9118",
    "sub_829F9E58",
}

REQUIRED_DISPATCH_AND_OBJECT_HOOKS = {
    "sub_826DDA10",
    "sub_826DDB48",
    "sub_826DDD90",
    "sub_826DEE10",
    "sub_826DF530",
    "sub_826DF640",
    "sub_826DFA28",
    "sub_826E2738",
    "sub_826E2918",
    "sub_826E2CC8",
    "sub_826E2F50",
    "sub_826E3020",
    "sub_826E30A8",
    "sub_826E3150",
    "sub_826E3740",
    "sub_826E7B50",
    "sub_826E7CC8",
    "sub_826E7F48",
    "sub_826E81E0",
    "sub_826E8278",
    "sub_826E82A8",
    "sub_826E8308",
    "sub_826E8380",
    "sub_826E85B0",
    "sub_826E8EA8",
    "sub_826E8F10",
    "sub_826E8FF0",
    "sub_826E90C0",
    "sub_826E95B8",
    "sub_826E9688",
    "sub_826E9738",
    "sub_826E99A8",
    "sub_826E9BA8",
    "sub_826E9C78",
    "sub_826E9D18",
    "sub_826E9E60",
    "sub_826EAA68",
    "sub_826EB048",
    "sub_826EB5D8",
    "sub_826EB9F8",
    "sub_826EBAA0",
    "sub_826EC048",
    "sub_826ED450",
    "sub_826ED600",
    "sub_826EDF48",
    "sub_826EDFE0",
    "sub_826EE9A8",
    "sub_826EEC10",
    "sub_826EEFD0",
    "sub_826EF9F8",
    "sub_826EFE08",
    "sub_826EFF58",
    "sub_826FE908",
    "sub_826FE9C8",
    "sub_826F0B38",
    "sub_826F2040",
    "sub_826F0390",
    "sub_826F0520",
    "sub_826F0840",
    "sub_826F1158",
    "sub_826F13C0",
    "sub_826F1468",
    "sub_826F14E8",
    "sub_826F1570",
    "sub_826F15F0",
    "sub_826F2358",
    "sub_826F2870",
    "sub_826F3668",
    "sub_826F3DD0",
    "sub_826F4448",
    "sub_826F7050",
    "sub_826FDDC0",
    "sub_826FFCB0",
    "sub_826D9E10",
    "sub_826DA050",
    "sub_826DA0E0",
    "sub_826DAC48",
    "sub_826DAD00",
    "sub_82702C58",
    "sub_82702190",
    "sub_82702CA0",
    "sub_82702CD8",
    "sub_82702CF0",
    "sub_82702D08",
    "sub_82702D48",
    "sub_82702D60",
    "sub_82702D78",
    "sub_82702DB8",
    "sub_82702DD0",
    "sub_82702E98",
    "sub_82702F90",
    "sub_82703050",
    "sub_82703200",
    "sub_82703350",
    "sub_827038B0",
    "sub_82703938",
    "sub_82703BC0",
    "sub_82703D08",
    "sub_82703DC0",
    "sub_82703F80",
    "sub_82704070",
    "sub_82704180",
    "sub_827041A8",
    "sub_82704288",
    "sub_82704390",
    "sub_827045B8",
    "sub_827047B8",
    "sub_82704AF8",
    "sub_82704D40",
    "sub_82704FE8",
    "sub_82705190",
    "sub_82705390",
    "sub_82705488",
    "sub_82705BD0",
    "sub_82706CA8",
    "sub_82707968",
    "sub_82707D78",
    "sub_82718420",
    "sub_8278CCE8",
}


def session_table_consumers() -> set[str]:
    text = SESSION_GENERATED.read_text()
    functions = list(FUNCTION_RE.finditer(text))
    result: set[str] = set()
    for index, match in enumerate(functions):
        end = functions[index + 1].start() if index + 1 < len(functions) else len(text)
        body = text[match.start() : end]
        if "1544(r" in body or re.search(r"addi r\d+,r\d+,520", body):
            result.add(match.group(1))
    return result


def main() -> None:
    hooks = HOOKS.read_text()
    direct_session_consumers = session_table_consumers()
    required = (
        direct_session_consumers
        | REQUIRED_COMMAND_AND_SNAPSHOT_HOOKS
        | REQUIRED_DISPATCH_AND_OBJECT_HOOKS
    )
    missing = sorted(
        function
        for function in required
        if f'extern "C" void {function}(' not in hooks
    )
    if missing:
        raise RuntimeError(
            "retail-width multiplayer consumers lack hooks: " + ", ".join(missing)
        )

    print("direct_session_table_consumers=" + ",".join(sorted(direct_session_consumers)))
    print(f"hook_coverage={len(required)}/{len(required)}")


if __name__ == "__main__":
    main()
