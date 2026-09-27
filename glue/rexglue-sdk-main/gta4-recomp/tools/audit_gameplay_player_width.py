#!/usr/bin/env python3
"""Classify canonical player-info loops that retain a literal retail width.

The direct-global audit protects the fixed tables themselves.  This audit is
the complementary gameplay pass: it finds every generated function that both
calls the canonical player-info accessor and contains a literal 16 loop/value,
then requires an explicit classification and the expected high-player hook.
"""

from __future__ import annotations

import pathlib
import re


ROOT = pathlib.Path(__file__).resolve().parents[1]
GENERATED = ROOT / "generated"
TRUSTED = ROOT.parents[2] / "gta_iv" / "xex_excavation_retail"
HOOKS = (ROOT / "src" / "gta4_multiplayer_64_hooks.cpp").read_text(encoding="utf-8")
FUNCTION_RE = re.compile(
    r"DEFINE_REX_FUNC\(([^)]+)\) \{(.*?)(?=\nDEFINE_REX_FUNC\(|\Z)",
    re.DOTALL,
)
LITERAL_SIXTEEN_RE = re.compile(
    r"// (?:cmp(?:l)?wi cr\d+,r\d+,16|li r\d+,16)$", re.MULTILINE
)


WIDENED = {
    "sub_822D7188": "world-transition all-player loop bodies replayed for high IDs",
    "sub_822705A0": "64-player float-minimum reduction",
    "sub_825B2D70": "64-player matching-state counter",
    "sub_826E5D50": "64-player float-minimum reduction",
    "sub_826E5E18": "64-player float-minimum reduction",
    "sub_82718AE8": "high-player fallback lookup",
    "sub_821C1460": "all-player side-effect replay",
    "sub_826EAA68": "extended network-owner sidecar path",
    "sub_826EDF48": "extended network-owner sidecar path",
    "sub_826F7050": "extended proximity-weight injection",
    "sub_821F89C0": "64-player universal predicate",
    "sub_821F8A78": "64-player existential predicate",
    "sub_823A3DD0": "64-player existential predicate",
    "sub_825755A0": "high-player fallback lookup",
    "sub_82502B88": "64-player universal predicate with local alias",
    "sub_826DE548": "all-player lobby scans plus bounded position-cache projection",
    "sub_826E6E48": "64-player nearest network decision with one final dispatch",
    "sub_8278D0C8": "four-batch proximity-status sidecar with one global 64-player weighting pass",
    "sub_8216D490": "64-player threshold reduction over the bounded result ABI",
    "sub_8222A030": "64-player existential gameplay decision",
    "sub_823BC0B8": "four-batch coordinate sampling merged into the bounded output ABI",
    "sub_823F70E8": "deduplicated four-batch population dispatch",
}

NON_PLAYER_CAPACITY = {
    "sub_826D2970": "event payload bit width",
    "sub_82186998": "vector-load byte offset",
    "sub_82159298": "vector-load byte offset",
    "sub_8222BB58": "task/status enum value",
}

# These loops are memory-safe because every iteration remains inside 0..15 and
# reaches the sidecar-aware canonical accessor.  Their *algorithmic* output is
# nevertheless a fixed retail aggregate/candidate ABI, so they are reported
# separately instead of being mislabeled as fully widened.
BOUNDED_RETAIL_ALGORITHM: dict[str, str] = {}
TRUSTED_TAIL_ACCESSOR_THUNKS = {"sub_827087D8"}


def has_strong_hook(function: str) -> bool:
    return f'extern "C" void {function}(' in HOOKS


def has_primary_alias_hook(function: str) -> bool:
    return f"GTA4_PRIMARY_PLAYER_INFO_ALIAS_HOOK({function})" in HOOKS


def main() -> None:
    discovered: dict[str, str] = {}
    all_generated_callers: set[str] = set()
    generated_call_sites = 0
    for path in sorted(GENERATED.glob("gta4_recomp.*.cpp")):
        text = path.read_text(encoding="utf-8")
        for match in FUNCTION_RE.finditer(text):
            function, body = match.groups()
            if "sub_8225CF68(ctx, base);" in body:
                all_generated_callers.add(function)
                generated_call_sites += body.count("sub_8225CF68(ctx, base);")
            if "sub_8225CF68(ctx, base);" in body and LITERAL_SIXTEEN_RE.search(body):
                discovered[function] = body

    callers_text = (TRUSTED / "callers.txt").read_text(encoding="utf-8")
    trusted_match = re.search(
        r"^sub_8225CF68\s+0x8225CF68\s+\[called by (\d+)\]\n"
        r"(?P<callers>(?:  <- sub_[0-9A-Fa-f]{8}\n)+)",
        callers_text,
        re.MULTILINE,
    )
    if trusted_match is None:
        raise RuntimeError("trusted sub_8225CF68 caller block is missing")
    trusted_callers = set(re.findall(r"sub_[0-9A-Fa-f]{8}", trusted_match.group("callers")))
    trusted_count = int(trusted_match.group(1))

    classified = WIDENED.keys() | NON_PLAYER_CAPACITY.keys() | BOUNDED_RETAIL_ALGORITHM.keys()
    failures: list[str] = []
    unexpected = sorted(discovered.keys() - classified)
    missing = sorted(classified - discovered.keys())
    if unexpected:
        failures.append("unclassified canonical-accessor width consumers: " + ", ".join(unexpected))
    if missing:
        failures.append("classified canonical-accessor consumers disappeared: " + ", ".join(missing))

    for function in sorted(discovered):
        if function in WIDENED:
            classification = WIDENED[function]
            if not has_strong_hook(function):
                failures.append(f"{function}: widened gameplay loop lost its strong hook")
            status = "widened"
        elif function in NON_PLAYER_CAPACITY:
            classification = NON_PLAYER_CAPACITY[function]
            status = "not-player-capacity"
        else:
            classification = BOUNDED_RETAIL_ALGORITHM[function]
            body = discovered[function]
            if re.search(r"addi r\d+,r\d+,7280", body) and not (
                has_strong_hook(function) or has_primary_alias_hook(function)
            ):
                failures.append(
                    f"{function}: bounded algorithm gained an unaliased direct fixed-table load"
                )
            status = "bounded-retail-algorithm"
        print(f"{function}: status={status}; classification={classification}")

    if len(discovered) != 26:
        failures.append(f"canonical-accessor width set changed: expected 26, found {len(discovered)}")
    nearest_hook = HOOKS[HOOKS.index('extern "C" void sub_826E6E48(') :]
    nearest_hook = nearest_hook[: nearest_hook.index('\nextern "C" void ', 1)]
    if "kNearestNetworkTimestampOffset" not in nearest_hook or nearest_hook.count(
        "REX_STORE_U32(*timestamp_address"
    ) < 2:
        failures.append("nearest-player high probes can be hidden by the retail timestamp gate")
    proximity_hook = HOOKS[HOOKS.index('extern "C" void sub_8278D0C8(') :]
    proximity_hook = proximity_hook[: proximity_hook.index('// Retail peer-manager constructor.')]
    for seam in (
        'extern "C" void sub_8278D608(',
        "ApplyGlobalProximityStatusWeights(base)",
        "WithProjectedPlayerInfoBatch",
    ):
        if seam not in proximity_hook:
            failures.append(f"64-player proximity status semantics lost seam: {seam}")
    if "g_proximity_status_capture.eligible[actual_id] = true;" not in HOOKS:
        failures.append("64-player proximity status eligibility capture disappeared")
    if trusted_count != 201 or len(trusted_callers) != trusted_count:
        failures.append(
            f"trusted canonical-accessor caller set changed: header={trusted_count}, "
            f"unique={len(trusted_callers)}"
        )
    expected_generated_callers = trusted_callers | TRUSTED_TAIL_ACCESSOR_THUNKS
    if all_generated_callers != expected_generated_callers:
        failures.append(
            "generated/trusted canonical-accessor caller mismatch: generated-only="
            + ", ".join(sorted(all_generated_callers - expected_generated_callers))
            + "; trusted-only="
            + ", ".join(sorted(expected_generated_callers - all_generated_callers))
        )
    if generated_call_sites != 236:
        failures.append(
            f"canonical-accessor call-site set changed: expected 235 trusted calls plus "
            f"one tail thunk, found {generated_call_sites}"
        )
    if "// b 0x8225cf68\n\tsub_8225CF68(ctx, base);" not in next(
        body
        for function, body in (
            match.groups()
            for path in sorted(GENERATED.glob("gta4_recomp.*.cpp"))
            for match in FUNCTION_RE.finditer(path.read_text(encoding="utf-8"))
        )
        if function == "sub_827087D8"
    ):
        failures.append("sub_827087D8 stopped being the trusted tail accessor thunk")
    if failures:
        raise RuntimeError("\n".join(failures))

    print(f"canonical_accessor_width_coverage={len(discovered)}/{len(discovered)}")
    print(f"fully_widened_gameplay_loops={len(WIDENED)}")
    print(f"non_capacity_literals={len(NON_PLAYER_CAPACITY)}")
    print(f"bounded_retail_algorithm_abis={len(BOUNDED_RETAIL_ALGORITHM)}")
    print(f"trusted_canonical_accessor_callers={len(trusted_callers)}/{trusted_count}")
    print(f"trusted_tail_accessor_thunks={len(TRUSTED_TAIL_ACCESSOR_THUNKS)}")
    print(f"generated_canonical_accessor_call_sites={generated_call_sites}")
    print(f"sidecar_aware_single_id_or_dynamic_width_callers={len(trusted_callers - discovered.keys())}")


if __name__ == "__main__":
    main()
