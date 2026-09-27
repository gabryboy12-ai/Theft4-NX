#!/usr/bin/env python3
"""Classify and enforce every fixed GTA IV player-info table consumer."""

from __future__ import annotations

import pathlib
import re
from collections import deque


ROOT = pathlib.Path(__file__).resolve().parents[1]
GENERATED = ROOT / "generated"
RETAIL_CALL_GRAPH = ROOT.parents[2] / "gta_iv" / "xex_excavation_retail" / "call_graph.txt"
HOOKS = (ROOT / "src" / "gta4_multiplayer_64_hooks.cpp").read_text(encoding="utf-8")
ALIAS_GATE = (ROOT / "src" / "gta4_player_info_alias_gate.h").read_text(encoding="utf-8")
FRONTEND = (ROOT / "src" / "gta4_frontend_hooks.cpp").read_text(encoding="utf-8")
FUNCTION_RE = re.compile(
    r"DEFINE_REX_FUNC\(([^)]+)\) \{(.*?)(?=\nDEFINE_REX_FUNC\(|\Z)",
    re.DOTALL,
)

PRIMARY_ALIAS_SPECIAL = {
    "sub_8225CE98": "64-entry active-player counter",
    "sub_82261CA8": "sidecar-aware destruction",
    "sub_82261EA8": "sidecar-aware player-ID move",
    "sub_82263000": "sidecar-aware full player construction",
    "sub_82700AA0": "peer-manager constructor with scoped local alias",
    "sub_827013F8": "peer-manager destructor with scoped local alias",
    "sub_82703BC0": "network-object path with scoped local alias",
}

NON_PRIMARY_SPECIAL = {
    "sub_826DB1E8": "network-array registration widened at sub_826DAD70 call seam",
    "sub_8225CD80": "64-entry player-pointer reverse lookup",
    "sub_82260D20": "retail teardown plus extended-player cleanup pass",
    "sub_822619E8": "explicit high-player entity construction through isolated alias",
    "sub_82261E48": "destroy-all extended lifecycle pass",
    "sub_82263200": "sidecar-aware payload player-info construction",
    "sub_822636F0": "generation-ordered candidate selection including high IDs",
    "sub_8225CF68": "canonical sidecar-aware player-info accessor",
}

REQUIRED_MUTATION_HOOKS = {
    "sub_82261CA8",
    "sub_82261EA8",
    "sub_82263000",
    "sub_82263200",
}


def is_hooked(function: str) -> bool:
    strong = f'extern "C" void {function}(' in HOOKS
    macro = f"GTA4_PRIMARY_PLAYER_INFO_ALIAS_HOOK({function})" in HOOKS
    if function == "sub_82258388":
        return (
            f'extern "C" void {function}(' in FRONTEND
            and "GTA4_RunWithPrimaryPlayerInfoAlias(ctx, base, __imp__sub_82258388)"
            in FRONTEND
        )
    return strong or macro


def parse_retail_call_graph() -> dict[str, set[str]]:
    graph: dict[str, set[str]] = {}
    current = ""
    for line in RETAIL_CALL_GRAPH.read_text(encoding="utf-8").splitlines():
        if line and not line.startswith((" ", ";")):
            current = line.split()[0]
            graph.setdefault(current, set())
        elif current and line.startswith("  -> "):
            graph[current].add(line.removeprefix("  -> "))
    return graph


def shortest_path(graph: dict[str, set[str]], start: str, targets: set[str]) -> list[str]:
    pending = deque([(start, [start])])
    visited = {start}
    while pending:
        function, path = pending.popleft()
        if function in targets and function != start:
            return path
        for callee in graph.get(function, ()):
            if callee not in visited:
                visited.add(callee)
                pending.append((callee, [*path, callee]))
    return []


def main() -> None:
    discovered: dict[str, tuple[str, int, str]] = {}
    for path in sorted(GENERATED.glob("gta4_recomp.*.cpp")):
        text = path.read_text(encoding="utf-8")
        for match in FUNCTION_RE.finditer(text):
            function, body = match.groups()
            if "lis r" not in body or ",-32064" not in body:
                continue
            references = []
            if re.search(r"addi r\d+,r\d+,7280", body):
                references.append("pointer")
            if re.search(r"addi r\d+,r\d+,7216", body):
                references.append("generation")
            if re.search(r"(?:lwz|stw) r\d+,7208\(r\d+\)", body):
                references.append("counter")
            if not references:
                continue
            line = text.count("\n", 0, match.start()) + 1
            discovered[function] = (path.name, line, body)

    failures: list[str] = []
    primary_count = 0
    special_count = 0
    for function, (path, line, body) in sorted(discovered.items()):
        primary_indexed = ",-30856(" in body
        if primary_indexed:
            primary_count += 1
            classification = PRIMARY_ALIAS_SPECIAL.get(function, "scoped-local-player-alias")
        else:
            special_count += 1
            classification = NON_PRIMARY_SPECIAL.get(function, "")
            if not classification:
                failures.append(f"{function}: non-primary direct-table consumer is unclassified")
        covered = is_hooked(function) or function == "sub_826DB1E8"
        if not covered:
            failures.append(f"{function}: classification={classification} has no active hook")
        print(
            f"{path}:{line}:{function}: primary={str(primary_indexed).lower()}; "
            f"classification={classification}; covered={str(covered).lower()}"
        )

    if not all(is_hooked(function) for function in REQUIRED_MUTATION_HOOKS):
        failures.append("one or more fixed-table mutation hooks are absent")
    if "extern \"C\" void sub_826DAD70(" not in HOOKS or (
        "kPlayerInfoNetworkArrayRegisterReturnAddress" not in HOOKS
    ):
        failures.append("CNetworkArrayMgr player-info registration seam is not widened")
    if "JoinableProjectionGate<PrimaryPlayerInfoAliasSession>" not in HOOKS:
        failures.append("primary player-info aliases lost their joinable projection session")
    if "std::recursive_mutex g_player_info_alias_mutex;" not in HOOKS:
        failures.append("nested player-info projections no longer use a recursive alias mutex")
    primary_helper = HOOKS[HOOKS.index("void WithPrimaryPlayerInfoAlias(") :]
    primary_helper = primary_helper[: primary_helper.index("void RunPrimaryPlayerInfoAlias(")]
    for required in (
        "g_player_info_alias_gate.EnterProjection(",
        "admission.session()",
        "admission.is_owner()",
        "admission.Complete(",
    ):
        if required not in primary_helper:
            failures.append(f"primary alias session is missing {required}")
    if "projection_participants_ == 1" not in ALIAS_GATE:
        failures.append("primary alias owner can restore before joined callbacks drain")
    if "mode_ == Mode::kProjection" not in ALIAS_GATE or (
        "AdmissionKind::kProjectionParticipant" not in ALIAS_GATE
    ):
        failures.append("cross-thread direct consumers can no longer join a projected session")
    for required in (
        "AdmissionKind::kOrdinaryOperation",
        "ordinary_operations_",
        "exclusive_waiters_",
        "closing_projection_",
        "state.operation_depth == 0",
    ):
        if required not in ALIAS_GATE:
            failures.append(f"alias gate is missing synchronization state {required}")
    if "struct PlayerInfoLease : mp64::PlayerInfoEntry" not in HOOKS:
        failures.append("PlayerInfoForId no longer leases returned guest player-info entries")
    if "std::exception_ptr callback_failure" not in primary_helper:
        failures.append("primary alias callback restoration is not exception-safe")
    accessor = HOOKS[HOOKS.index('extern "C" void sub_8225CF68(') :]
    accessor = accessor[: accessor.index('// Player-info destruction.')]
    if "g_player_info_alias_gate.EnterRead()" not in accessor:
        failures.append("canonical accessor can observe an unstable guest projection")
    shadow = HOOKS[HOOKS.index("void SyncPlayerInfoShadow(") :]
    shadow = shadow[: shadow.index("template <typename Callback>")]
    if shadow.index("alias_gate") > shadow.index("shadow_lock"):
        failures.append("player-info shadow lock order can invert against alias sessions")
    batch_projection = HOOKS[HOOKS.index("void WithProjectedPlayerInfoBatch(") :]
    batch_projection = batch_projection[: batch_projection.index("void WarnInvalidPeerOnce(")]
    if "saved_projection" not in batch_projection or "alias_gate" not in batch_projection:
        failures.append("nested batch projection lost session-gated save/restore semantics")
    for function in ("sub_82261CA8", "sub_82261EA8", "sub_822619E8", "sub_82263000"):
        hook = HOOKS[HOOKS.index(f'extern "C" void {function}(') :]
        hook = hook[: hook.index('\nextern "C" void ', 1)]
        if "EnterOpaque()" not in hook:
            failures.append(f"{function}: lifecycle mutation is not exclusive with readers")
    for function in ("sub_82261CA8", "sub_82261EA8", "sub_822619E8", "sub_82263000"):
        hook = HOOKS[HOOKS.index(f'extern "C" void {function}(') :]
        hook = hook[: hook.index('\nextern "C" void ', 1)]
        if "RunPrimaryPlayerInfoAlias" not in hook:
            failures.append(f"{function}: legacy path bypasses projection serialization")
    call_graph = parse_retail_call_graph()
    lifecycle_mutations = {
        "sub_82261CA8",
        "sub_82261EA8",
        "sub_822619E8",
        "sub_82263000",
        "sub_82263200",
    }
    for world_job in ("sub_82149390", "sub_824F2E68"):
        path = shortest_path(call_graph, world_job, lifecycle_mutations)
        if path:
            failures.append(f"{world_job}: world-job reader reaches lifecycle mutation: {' -> '.join(path)}")
    if len(discovered) != 166:
        failures.append(f"generated direct-consumer set changed: expected 166, found {len(discovered)}")
    if failures:
        raise RuntimeError("\n".join(failures))

    print(f"direct_consumer_coverage={len(discovered)}/{len(discovered)}")
    print(f"primary_index_consumers={primary_count}")
    print(f"enumerator_lifecycle_network_consumers={special_count}")


if __name__ == "__main__":
    main()
