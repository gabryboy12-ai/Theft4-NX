#!/usr/bin/env python3
"""Report retail-width peer consumers in the GTA IV network address range.

This is a discovery audit rather than a hook-coverage assertion: a literal
sixteen can also describe a wire field or an object-local vector.  The report
keeps the generated-code review reproducible and records which generated
functions still need semantic classification after regeneration.
"""

from pathlib import Path
import re


ROOT = Path(__file__).parents[1]
GENERATED = ROOT / "generated" / "gta4_recomp.50.cpp"
HOOKS = ROOT / "src" / "gta4_multiplayer_64_hooks.cpp"
FUNCTION_RE = re.compile(r"DEFINE_REX_FUNC\((sub_([0-9A-Fa-f]{8}))\)")
NETWORK_START = 0x826D0000
NETWORK_END = 0x82720000
PATTERNS = {
    "limit-16": re.compile(r"// cmp(?:l)?wi cr\d+,r\d+,16$", re.MULTILINE),
    "peer-manager": re.compile(r"(?:304|1452|1516)\(r\d+\)"),
    "owner-list": re.compile(r"addi r\d+,r\d+,42$", re.MULTILINE),
    "peer-message": re.compile(r"(?:1032|1048|4160)"),
    "peer-sequence": re.compile(r"addi r\d+,r\d+,-14776$", re.MULTILINE),
}

# These functions are deliberately left as retail-width embedded constructors
# or route every peer-bearing value through a canonical widened accessor. Keep
# the classification explicit so a newly discovered unhooked consumer fails
# this audit instead of being waved through as a likely false positive.
SAFE_CLASSIFICATIONS = {
    "sub_826E89D0": "constructs the 16 inline reliable buffers; high buffers use the same element constructor lazily",
    "sub_826EC288": "constructs the 16 inline message buffers; high buffers reproduce the element constructor in guest heap storage",
    "sub_826ED6B0": "only loads object-manager+304 and resolves the connection key through hooked sub_826FE808",
    "sub_826F0FF0": "only loads object-manager+304 and resolves the connection key through hooked sub_826FE808",
    "sub_826F1670": "peer ID is consumed through hooked sub_826FE880 and sidecar-aware object accessors",
    "sub_826F1DD8": "peer ID is consumed through hooked sub_826FE880 and sidecar-aware object accessors",
    "sub_826F2118": "connection keys are consumed through hooked sub_826FE808",
    "sub_826F2640": "peer lookups route through sub_826FF150/sub_826FE808, both ending at widened peer lookup",
    "sub_826F2FD8": "peer ID is consumed through hooked sub_826FE880 and sidecar-aware endpoint accessors",
    "sub_826F33D8": "peer ID is consumed through hooked sub_826FE880 and sidecar-aware endpoint accessors",
    "sub_826F4080": "connection key is consumed through hooked sub_826FE808",
    "sub_826F4288": "connection key is consumed through hooked sub_826FE808",
    "sub_826FC6F8": "the scanner matched 304 inside unrelated offset 29304",
    "sub_826FDB78": "literal 16 selects a pointer-range sorting algorithm and is not a peer capacity",
    "sub_82701CE8": "constructs the 16 inline peer records; sub_82700AA0 registers the 64-peer sidecar",
}

WIDENED_LOOKUP_CLASSIFICATIONS = {
    **SAFE_CLASSIFICATIONS,
    "sub_826F6510": "resolved peer pointer is used only by peer predicates and logging payload construction",
    "sub_826FF150": "leaf connection-number accessor over hooked sub_826FE880",
    "sub_826FF190": "tail-call wrapper over hooked sub_826FE880",
    "sub_826FF258": "connection-key lookup flows through sidecar-safe peer/player-info accessors",
    "sub_826FF378": "connection-key lookup flows through peer predicates and packet serialization",
    "sub_826FF400": "connection-key lookup flows through peer predicates and player-info accessor",
    "sub_827010A0": "peer removal delegates to hooked sub_82700E30 and sub_82261CA8",
    "sub_82701F68": "owner-byte accessor tail-calls hooked sub_826FE880",
    "sub_82702378": "owner comparison consumes only the pointer returned by hooked sub_826FE880",
    "sub_82703EC8": "object constructor consumes the resolved pointer only through a peer predicate before hooked teardown",
    "sub_82706378": "message handler resolves IDs through hooked peer and player-info accessors",
    "sub_827069B8": "message handler resolves IDs through hooked peer and player-info accessors",
    "sub_82709EB8": "player proximity query resolves high player-info through hooked accessors",
}


def main() -> None:
    text = GENERATED.read_text()
    hooks = HOOKS.read_text()
    functions = list(FUNCTION_RE.finditer(text))
    discovered = 0
    covered = 0
    unclassified: list[str] = []
    for index, match in enumerate(functions):
        address = int(match.group(2), 16)
        if not NETWORK_START <= address < NETWORK_END:
            continue
        end = functions[index + 1].start() if index + 1 < len(functions) else len(text)
        body = text[match.start() : end]
        found = [name for name, pattern in PATTERNS.items() if pattern.search(body)]
        if found:
            discovered += 1
            function = match.group(1)
            hooked = f'extern "C" void {function}(' in hooks
            classification = SAFE_CLASSIFICATIONS.get(function, "")
            is_covered = hooked or bool(classification)
            if is_covered:
                covered += 1
            else:
                unclassified.append(function)
            suffix = f"; classification={classification}" if classification else ""
            print(
                f"{function}: {','.join(found)}; hooked={str(hooked).lower()}; "
                f"covered={str(is_covered).lower()}{suffix}"
            )
    if unclassified:
        raise RuntimeError(
            "unclassified retail-width peer consumers: " + ", ".join(unclassified)
        )
    print(f"semantic_coverage={covered}/{discovered}")

    lookup_discovered = 0
    lookup_covered = 0
    lookup_unclassified: list[str] = []
    for index, match in enumerate(functions):
        address = int(match.group(2), 16)
        if not NETWORK_START <= address < NETWORK_END:
            continue
        end = functions[index + 1].start() if index + 1 < len(functions) else len(text)
        body = text[match.start() : end]
        if "sub_826FE880(ctx, base);" not in body and "sub_826FE808(ctx, base);" not in body:
            continue
        function = match.group(1)
        hooked = f'extern "C" void {function}(' in hooks
        classification = WIDENED_LOOKUP_CLASSIFICATIONS.get(function, "")
        is_covered = hooked or bool(classification)
        lookup_discovered += 1
        if is_covered:
            lookup_covered += 1
        else:
            lookup_unclassified.append(function)
        suffix = f"; classification={classification}" if classification else ""
        print(
            f"lookup:{function}; hooked={str(hooked).lower()}; "
            f"covered={str(is_covered).lower()}{suffix}"
        )
    if lookup_unclassified:
        raise RuntimeError(
            "unclassified widened-lookup consumers: " + ", ".join(lookup_unclassified)
        )
    print(f"lookup_consumer_coverage={lookup_covered}/{lookup_discovered}")


if __name__ == "__main__":
    main()
