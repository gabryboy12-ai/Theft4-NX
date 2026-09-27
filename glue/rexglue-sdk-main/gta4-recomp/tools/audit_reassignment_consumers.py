#!/usr/bin/env python3
"""List generated reassignment functions that retain retail-width indexing.

The hook translation unit is intentionally separate from generated code.  Run
this audit after regeneration to identify every function in the ownership
reassignment address interval that still contains a 16-owner loop, 20-byte
owner-record indexing, 1220-byte recipient rows, or the owner-list bias.
"""

from pathlib import Path
import re


GENERATED = Path(__file__).parents[1] / "generated" / "gta4_recomp.54.cpp"
HOOKS = Path(__file__).parents[1] / "src" / "gta4_multiplayer_64_hooks.cpp"
START_ADDRESS = 0x82782000
END_ADDRESS = 0x82787600
FUNCTION_RE = re.compile(r"DEFINE_REX_FUNC\((sub_([0-9A-Fa-f]{8}))\)")
PATTERNS = (
    "cmpwi cr6,r30,16",
    "cmpwi cr6,r31,16",
    "cmpwi cr6,r29,16",
    "cmpwi cr6,r28,16",
    "cmpwi cr6,r27,16",
    "cmpwi cr6,r11,16",
    "mulli r29,r26,76",
    "mulli r11,r27,1220",
    "mulli r10,r29,1220",
    "addi r10,r21,42",
    "addi r11,r11,42",
)


def main() -> None:
    text = GENERATED.read_text()
    hooks = HOOKS.read_text()
    matches = list(FUNCTION_RE.finditer(text))
    consumers: list[str] = []
    for index, match in enumerate(matches):
        address = int(match.group(2), 16)
        if not START_ADDRESS <= address < END_ADDRESS:
            continue
        end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        body = text[match.start() : end]
        found = [pattern for pattern in PATTERNS if pattern in body]
        shifted_indices = len(re.findall(r"rlwinm r\d+,r\d+,2,0,29", body))
        if shifted_indices >= 2 and re.search(r"lbz r\d+,0\(r\d+\)", body):
            found.append("byte owner -> shifted owner-record index")
        if found:
            consumers.append(match.group(1))
            print(f"{match.group(1)}: {', '.join(found)}")

    missing = [
        function
        for function in consumers
        if f'extern "C" void {function}(' not in hooks
    ]
    if missing:
        raise RuntimeError(
            "generated fixed-width reassignment consumers lack hooks: "
            + ", ".join(missing)
        )
    print(f"hook_coverage={len(consumers)}/{len(consumers)}")


if __name__ == "__main__":
    main()
