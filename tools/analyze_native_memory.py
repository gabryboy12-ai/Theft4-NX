#!/usr/bin/env python3
"""Analyze LibertyRecomp native-renderer memory captures.

The renderer CSV deliberately keeps process memory, Vulkan heap telemetry and
renderer-owned allocations separate. This tool reports correlations and leak
suspects; it never treats their difference as proof of an untracked leak.
"""

from __future__ import annotations

import argparse
import csv
import html
from dataclasses import dataclass
from pathlib import Path


DEFAULT_INPUT = (
    Path.home()
    / "Library"
    / "Application Support"
    / "LibertyRecomp"
    / "Diagnostics"
    / "native-memory-latest.csv"
)
MIB = 1024.0 * 1024.0


@dataclass(frozen=True)
class Finding:
    category: str
    domain: str
    first_mib: float
    last_mib: float
    peak_mib: float
    delta_mib: float
    overall_mib_per_min: float
    late_mib_per_min: float
    churn_mib: float
    classification: str


@dataclass(frozen=True)
class LifecycleFinding:
    kind: str
    identity: int
    generation: int
    retained_mib: float
    allocation_mib: float
    age_frames: int
    variant_count: int
    pending_release: bool
    last_action: str
    last_reason: str
    classification: str


def read_dict_rows(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        return []
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def integer(row: dict[str, str], name: str) -> int:
    try:
        return int(row.get(name, "0") or 0)
    except ValueError:
        return 0


def lifecycle_key(row: dict[str, str]) -> tuple[str, int, int]:
    return (row.get("kind", "unknown"), integer(row, "identity"), integer(row, "generation"))


def build_lifecycle_findings(
    event_rows: list[dict[str, str]], retained_rows: list[dict[str, str]]
) -> tuple[list[LifecycleFinding], dict[str, dict[str, int]]]:
    event_state: dict[tuple[str, int, int], tuple[str, str]] = {}
    event_summary: dict[str, dict[str, int]] = {}
    for row in event_rows:
        kind = row.get("kind", "unknown")
        action = row.get("action", "unknown")
        reason = row.get("reason", "unknown")
        event_state[lifecycle_key(row)] = (action, reason)
        actions = event_summary.setdefault(kind, {})
        actions[action] = actions.get(action, 0) + 1

    if not retained_rows:
        return [], event_summary
    final_capture = max(integer(row, "capture_index") for row in retained_rows)
    findings: list[LifecycleFinding] = []
    for row in retained_rows:
        if integer(row, "capture_index") != final_capture:
            continue
        kind, identity, generation = lifecycle_key(row)
        action, reason = event_state.get((kind, identity, generation), ("baseline", "none"))
        retained_bytes = integer(row, "retained_bytes")
        allocation_bytes = integer(row, "allocation_bytes")
        age_frames = integer(row, "age_frames")
        variants = integer(row, "variant_count")
        pending = integer(row, "pending_release") != 0
        release_seen = action in {"release-requested", "retire-pending"}
        if pending or release_seen:
            classification = "release-retirement-suspect"
        elif kind in {"buffer", "vertex-conversion", "index-conversion"} and (
            age_frames >= 600 and retained_bytes >= 1024 * 1024
        ):
            classification = "aged-cache-suspect"
        elif kind == "buffer" and variants >= 8:
            classification = "variant-explosion-suspect"
        else:
            classification = "live-working-set"
        findings.append(
            LifecycleFinding(
                kind=kind,
                identity=identity,
                generation=generation,
                retained_mib=retained_bytes / MIB,
                allocation_mib=allocation_bytes / MIB,
                age_frames=age_frames,
                variant_count=variants,
                pending_release=pending,
                last_action=action,
                last_reason=reason,
                classification=classification,
            )
        )
    findings.sort(
        key=lambda item: (
            item.classification == "live-working-set",
            -(item.retained_mib + item.allocation_mib),
            -item.age_frames,
        )
    )
    return findings, event_summary


def write_lifecycle_findings(path: Path, findings: list[LifecycleFinding]) -> None:
    fields = list(LifecycleFinding.__dataclass_fields__)
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for finding in findings:
            writer.writerow(finding.__dict__)


def read_rows(path: Path) -> list[dict[str, float]]:
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        if not reader.fieldnames:
            raise ValueError(f"{path} has no CSV header")
        rows: list[dict[str, float]] = []
        for raw in reader:
            row: dict[str, float] = {}
            for name, value in raw.items():
                if name is None:
                    continue
                try:
                    row[name] = float(value or 0)
                except ValueError as error:
                    raise ValueError(f"non-numeric value in {name}: {value!r}") from error
            rows.append(row)
    if not rows:
        raise ValueError(f"{path} has no sample rows")
    return rows


def category_names(rows: list[dict[str, float]]) -> list[str]:
    suffix = "_live_bytes"
    return sorted(
        name[: -len(suffix)]
        for name in rows[0]
        if name.endswith(suffix) and name.startswith(("host_", "gpu_"))
    )


def slope_per_minute(times: list[float], values: list[float]) -> float:
    if len(times) < 2 or len(values) != len(times):
        return 0.0
    mean_time = sum(times) / len(times)
    mean_value = sum(values) / len(values)
    denominator = sum((value - mean_time) ** 2 for value in times)
    if denominator == 0.0:
        return 0.0
    numerator = sum(
        (time - mean_time) * (value - mean_value)
        for time, value in zip(times, values)
    )
    return numerator / denominator * 60.0 / MIB


def classify(
    delta_mib: float,
    overall_slope: float,
    late_slope: float,
    churn_mib: float,
    peak_mib: float,
) -> str:
    meaningful_growth = max(1.0, peak_mib * 0.01)
    slope_floor = 0.5
    if delta_mib >= meaningful_growth and overall_slope > slope_floor and late_slope > slope_floor:
        return "sustained-growth-suspect"
    if delta_mib >= meaningful_growth and overall_slope > slope_floor and late_slope <= slope_floor:
        return "warmup-or-bounded-cache"
    if churn_mib >= max(4.0, abs(delta_mib) * 4.0):
        return "high-bounded-churn"
    if delta_mib <= -meaningful_growth:
        return "released"
    return "stable"


def build_findings(rows: list[dict[str, float]]) -> list[Finding]:
    times = [row.get("elapsed_seconds", 0.0) for row in rows]
    late_begin = max(0, len(rows) * 3 // 4)
    late_times = times[late_begin:]
    findings: list[Finding] = []
    for category in category_names(rows):
        live_column = f"{category}_live_bytes"
        peak_column = f"{category}_peak_bytes"
        growth_column = f"{category}_cumulative_growth_bytes"
        shrink_column = f"{category}_cumulative_shrink_bytes"
        values = [row.get(live_column, 0.0) for row in rows]
        first = values[0]
        last = values[-1]
        peak = max(row.get(peak_column, 0.0) for row in rows)
        growth = max(row.get(growth_column, 0.0) for row in rows)
        shrink = max(row.get(shrink_column, 0.0) for row in rows)
        delta_mib = (last - first) / MIB
        overall_slope = slope_per_minute(times, values)
        late_slope = slope_per_minute(late_times, values[late_begin:])
        churn_mib = min(growth, shrink) / MIB
        findings.append(
            Finding(
                category=category.replace("_", "-"),
                domain="gpu" if category.startswith("gpu_") else "host",
                first_mib=first / MIB,
                last_mib=last / MIB,
                peak_mib=peak / MIB,
                delta_mib=delta_mib,
                overall_mib_per_min=overall_slope,
                late_mib_per_min=late_slope,
                churn_mib=churn_mib,
                classification=classify(
                    delta_mib, overall_slope, late_slope, churn_mib, peak / MIB
                ),
            )
        )
    return findings


def build_process_series(rows: list[dict[str, float]]) -> dict[str, list[float]]:
    direct_columns = {
        "physical-footprint": "process_physical_footprint_bytes",
        "resident": "process_resident_bytes",
        "task-internal": "process_internal_bytes",
        "task-external": "process_external_bytes",
        "task-device": "process_device_bytes",
        "task-compressed": "process_compressed_bytes",
        "graphics-footprint-ledger": "process_graphics_footprint_bytes",
        "graphics-footprint-compressed": "process_graphics_footprint_compressed_bytes",
        "graphics-nofootprint-ledger": "process_graphics_nofootprint_bytes",
        "malloc-in-use": "process_malloc_in_use_bytes",
        "malloc-reserved": "process_malloc_reserved_bytes",
    }
    series = {
        label: [row.get(column, 0.0) for row in rows]
        for label, column in direct_columns.items()
        if column in rows[0]
    }
    if {
        "process_malloc_reserved_bytes",
        "process_malloc_in_use_bytes",
    }.issubset(rows[0]):
        series["malloc-reserved-minus-in-use"] = [
            max(
                0.0,
                row.get("process_malloc_reserved_bytes", 0.0)
                - row.get("process_malloc_in_use_bytes", 0.0),
            )
            for row in rows
        ]
    resident_suffix = "_resident_bytes"
    region_prefix = "process_vm_"
    for column in sorted(rows[0]):
        if not column.startswith(region_prefix) or not column.endswith(resident_suffix):
            continue
        name = column[len(region_prefix) : -len(resident_suffix)]
        swapped_column = f"{region_prefix}{name}_swapped_bytes"
        series[f"vm-{name.replace('_', '-')}-resident-plus-swap"] = [
            row.get(column, 0.0) + row.get(swapped_column, 0.0) for row in rows
        ]
    return series


def build_process_findings(rows: list[dict[str, float]]) -> list[Finding]:
    times = [row.get("elapsed_seconds", 0.0) for row in rows]
    late_begin = max(0, len(rows) * 3 // 4)
    late_times = times[late_begin:]
    findings: list[Finding] = []
    for category, values in build_process_series(rows).items():
        first = values[0]
        last = values[-1]
        peak = max(values)
        delta_mib = (last - first) / MIB
        overall_slope = slope_per_minute(times, values)
        late_slope = slope_per_minute(late_times, values[late_begin:])
        findings.append(
            Finding(
                category=category,
                domain="process",
                first_mib=first / MIB,
                last_mib=last / MIB,
                peak_mib=peak / MIB,
                delta_mib=delta_mib,
                overall_mib_per_min=overall_slope,
                late_mib_per_min=late_slope,
                churn_mib=0.0,
                classification=classify(
                    delta_mib, overall_slope, late_slope, 0.0, peak / MIB
                ),
            )
        )
    return findings


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, round((len(ordered) - 1) * fraction)))
    return ordered[index]


def write_suspects(path: Path, findings: list[Finding]) -> None:
    fields = list(Finding.__dataclass_fields__)
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for finding in sorted(
            findings,
            key=lambda item: (
                item.classification != "sustained-growth-suspect",
                -item.late_mib_per_min,
                -item.delta_mib,
            ),
        ):
            writer.writerow(finding.__dict__)


def polyline(values: list[float], width: float, height: float, maximum: float) -> str:
    denominator = max(1, len(values) - 1)
    return " ".join(
        f"{index * width / denominator:.2f},{height - min(max(value, 0.0), maximum) * height / maximum:.2f}"
        for index, value in enumerate(values)
    )


def write_timeline(path: Path, rows: list[dict[str, float]]) -> None:
    series = {
        "physical footprint": (
            "#ff6b57",
            [row.get("process_physical_footprint_bytes", 0.0) / MIB for row in rows],
        ),
        "resident size": (
            "#f7b731",
            [row.get("process_resident_bytes", 0.0) / MIB for row in rows],
        ),
        "tracked host": (
            "#26de81",
            [row.get("tracked_host_bytes", 0.0) / MIB for row in rows],
        ),
        "tracked GPU allocations": (
            "#45aaf2",
            [row.get("tracked_gpu_bytes", 0.0) / MIB for row in rows],
        ),
    }
    width, height = 1200.0, 560.0
    left, top, plot_width, plot_height = 85.0, 65.0, 1080.0, 400.0
    maximum = max(1.0, *(max(values) for _, values in series.values()))
    grid: list[str] = []
    for step in range(6):
        y = top + plot_height * step / 5.0
        value = maximum * (5.0 - step) / 5.0
        grid.append(
            f'<line x1="{left}" y1="{y:.2f}" x2="{left + plot_width}" y2="{y:.2f}" stroke="#303846"/>'
            f'<text x="{left - 10}" y="{y + 4:.2f}" text-anchor="end" fill="#aeb7c4" font-size="12">{value:.1f}</text>'
        )
    lines: list[str] = []
    legend: list[str] = []
    for index, (name, (color, values)) in enumerate(series.items()):
        points = polyline(values, plot_width, plot_height, maximum)
        lines.append(f'<polyline points="{points}" stroke="{color}" stroke-width="2" fill="none"/>')
        x = left + index * 245.0
        legend.append(
            f'<rect x="{x}" y="505" width="12" height="12" fill="{color}"/>'
            f'<text x="{x + 18}" y="516" fill="#d8dee9" font-size="12">{html.escape(name)}</text>'
        )
    document = f"""<svg xmlns="http://www.w3.org/2000/svg" width="{width:.0f}" height="{height:.0f}">
<rect width="100%" height="100%" fill="#10151d"/>
<text x="{left}" y="30" fill="#f2f4f8" font-size="20">Liberty native renderer memory timeline</text>
<text x="{left}" y="50" fill="#aeb7c4" font-size="12">MiB; process and renderer allocation domains are intentionally not summed</text>
<g font-family="sans-serif">{''.join(grid)}<g transform="translate({left},{top})">{''.join(lines)}</g>{''.join(legend)}</g>
</svg>"""
    path.write_text(document, encoding="utf-8")


def write_subsystems(
    path: Path, findings: list[Finding], title: str = "Renderer subsystem memory"
) -> None:
    selected = sorted(findings, key=lambda item: item.peak_mib, reverse=True)[:16]
    width = 1200.0
    row_height = 27.0
    height = 90.0 + row_height * len(selected)
    left, bar_left, bar_width = 35.0, 330.0, 820.0
    maximum = max(1.0, *(item.peak_mib for item in selected))
    rows: list[str] = []
    for index, item in enumerate(selected):
        y = 70.0 + index * row_height
        live_width = item.last_mib / maximum * bar_width
        peak_width = item.peak_mib / maximum * bar_width
        color = "#ff6b57" if item.classification == "sustained-growth-suspect" else (
            "#f7b731" if item.classification == "high-bounded-churn" else "#45aaf2"
        )
        rows.append(
            f'<text x="{left}" y="{y + 14:.2f}" fill="#d8dee9" font-size="12">{html.escape(item.category)}</text>'
            f'<rect x="{bar_left}" y="{y:.2f}" width="{peak_width:.2f}" height="18" fill="#303846"/>'
            f'<rect x="{bar_left}" y="{y:.2f}" width="{live_width:.2f}" height="18" fill="{color}"/>'
            f'<text x="{bar_left + bar_width + 10}" y="{y + 14:.2f}" fill="#d8dee9" font-size="12">{item.last_mib:.1f} / {item.peak_mib:.1f} MiB</text>'
        )
    document = f"""<svg xmlns="http://www.w3.org/2000/svg" width="{width:.0f}" height="{height:.0f}">
<rect width="100%" height="100%" fill="#10151d"/>
<text x="{left}" y="30" fill="#f2f4f8" font-size="20">{html.escape(title)}</text>
<text x="{left}" y="50" fill="#aeb7c4" font-size="12">solid = final live; dark = capture peak</text>
<g font-family="sans-serif">{''.join(rows)}</g></svg>"""
    path.write_text(document, encoding="utf-8")


def write_report(
    path: Path,
    rows: list[dict[str, float]],
    findings: list[Finding],
    process_findings: list[Finding],
    lifecycle_findings: list[LifecycleFinding] | None = None,
    event_summary: dict[str, dict[str, int]] | None = None,
) -> None:
    lifecycle_findings = lifecycle_findings or []
    event_summary = event_summary or {}
    first, last = rows[0], rows[-1]
    times = [row.get("elapsed_seconds", 0.0) for row in rows]
    late_begin = max(0, len(rows) * 3 // 4)
    duration = last.get("elapsed_seconds", 0.0) - first.get("elapsed_seconds", 0.0)
    footprint_delta = (
        last.get("process_physical_footprint_bytes", 0.0)
        - first.get("process_physical_footprint_bytes", 0.0)
    ) / MIB
    suspects = [item for item in findings if item.classification == "sustained-growth-suspect"]
    process_suspects = [
        item
        for item in process_findings
        if item.classification == "sustained-growth-suspect"
    ]
    envelope_columns = {
        "Physical footprint": "process_physical_footprint_bytes",
        "Resident size": "process_resident_bytes",
        "Tracked host": "tracked_host_bytes",
        "Tracked GPU": "tracked_gpu_bytes",
        "Vulkan heap usage": "vulkan_heap_usage_bytes",
    }
    envelope_slopes = {
        name: (
            slope_per_minute(times, [row.get(column, 0.0) for row in rows]),
            slope_per_minute(
                times[late_begin:],
                [row.get(column, 0.0) for row in rows[late_begin:]],
            ),
        )
        for name, column in envelope_columns.items()
    }
    process_only_growth = envelope_slopes["Physical footprint"][1] > 0.5 and not suspects
    top = sorted(findings, key=lambda item: abs(item.delta_mib), reverse=True)[:12]
    process_top = sorted(
        process_findings,
        key=lambda item: (abs(item.delta_mib), item.peak_mib),
        reverse=True,
    )[:18]
    collection_duration_ms = [
        row.get("collection_duration_ticks", 0.0)
        / max(1.0, row.get("host_tick_frequency", 0.0))
        * 1000.0
        for row in rows
    ]
    scan_complete = sum(row.get("process_vm_region_scan_complete", 0.0) > 0 for row in rows)
    scan_errors = sum(row.get("process_vm_region_scan_errors", 0.0) for row in rows)
    final_draw_capacity = last.get("frame_descriptor_draw_capacity", 0.0)
    final_draw_demand = last.get("frame_descriptor_draws_requested", 0.0)
    descriptor_capacity_ratio = (
        final_draw_capacity / final_draw_demand if final_draw_demand else 0.0
    )
    aged_buffer_peak_mib = max(
        (row.get("buffer_cache_aged_bytes", 0.0) / MIB for row in rows), default=0.0
    )
    aged_conversion_peak_mib = max(
        (row.get("vertex_conversion_aged_bytes", 0.0) / MIB for row in rows), default=0.0
    )
    maximum_variants = int(
        max((row.get("maximum_vertex_variants_per_buffer", 0.0) for row in rows), default=0.0)
    )
    lines = [
        "# Liberty native renderer memory report",
        "",
        f"- Samples: {len(rows)} over {duration:.1f} seconds",
        f"- Process physical-footprint change: {footprint_delta:+.2f} MiB",
        f"- Final tracked host allocations: {last.get('tracked_host_bytes', 0.0) / MIB:.2f} MiB",
        f"- Final tracked GPU allocations: {last.get('tracked_gpu_bytes', 0.0) / MIB:.2f} MiB",
        f"- Sustained renderer-owned growth suspects: {len(suspects)}",
        f"- Sustained process/VM growth suspects: {len(process_suspects)}",
        f"- Pending texture releases: {int(first.get('pending_texture_releases', 0.0))} → {int(last.get('pending_texture_releases', 0.0))}",
        f"- Pending surface releases: {int(first.get('pending_surface_releases', 0.0))} → {int(last.get('pending_surface_releases', 0.0))}",
        f"- Profiler collection time: p50 {percentile(collection_duration_ms, 0.50):.3f} ms, p95 {percentile(collection_duration_ms, 0.95):.3f} ms, max {max(collection_duration_ms, default=0.0):.3f} ms",
        f"- Complete VM-region scans: {scan_complete}/{len(rows)}; scan errors: {int(scan_errors)}",
        f"- Final descriptor draw capacity/demand: {int(final_draw_capacity)}/{int(final_draw_demand)} ({descriptor_capacity_ratio:.2f}×)",
        f"- Peak aged buffer payloads: {aged_buffer_peak_mib:.2f} MiB",
        f"- Peak aged vertex conversions: {aged_conversion_peak_mib:.2f} MiB",
        f"- Maximum converted variants retained by one buffer: {maximum_variants}",
        f"- Dropped lifecycle events / retained rows: {int(last.get('dropped_events', 0.0))} / {int(last.get('dropped_retained', 0.0))}",
        "",
        "A sustained-growth label is a triage signal, not proof of a leak. Process footprint, Vulkan heap usage, and renderer-owned allocations overlap on unified-memory systems and must not be added together.",
        "",
        "## Envelope slopes",
        "",
        "| Metric | Whole capture MiB/min | Final quarter MiB/min |",
        "|---|---:|---:|",
    ]
    for name, (overall, late) in envelope_slopes.items():
        lines.append(f"| {name} | {overall:+.2f} | {late:+.2f} |")
    if process_only_growth:
        lines.extend(
            [
                "",
                "> The process footprint keeps growing in the final quarter while no renderer-owned category does. This points to untracked host or driver memory and needs an Instruments capture.",
            ]
        )
    lines.extend(
        [
            "",
            "## Largest subsystem changes",
            "",
            "| Subsystem | Domain | First MiB | Last MiB | Peak MiB | Delta MiB | Late MiB/min | Classification |",
            "|---|---:|---:|---:|---:|---:|---:|---|",
        ]
    )
    for item in top:
        lines.append(
            f"| {item.category} | {item.domain} | {item.first_mib:.2f} | {item.last_mib:.2f} | "
            f"{item.peak_mib:.2f} | {item.delta_mib:+.2f} | {item.late_mib_per_min:+.2f} | {item.classification} |"
        )
    lines.extend(
        [
            "",
            "## Process, allocator, and VM attribution",
            "",
            "These rows overlap each other and the physical footprint. They are diagnostic views, not values to sum.",
            "",
            "| Process category | First MiB | Last MiB | Peak MiB | Delta MiB | Late MiB/min | Classification |",
            "|---|---:|---:|---:|---:|---:|---|",
        ]
    )
    for item in process_top:
        lines.append(
            f"| {item.category} | {item.first_mib:.2f} | {item.last_mib:.2f} | "
            f"{item.peak_mib:.2f} | {item.delta_mib:+.2f} | "
            f"{item.late_mib_per_min:+.2f} | {item.classification} |"
        )
    lifecycle_suspects = [
        item
        for item in lifecycle_findings
        if item.classification != "live-working-set"
    ]
    lines.extend(
        [
            "",
            "## Identity-level lifecycle findings",
            "",
            f"- Final retained resources: {len(lifecycle_findings)}",
            f"- Lifecycle suspects: {len(lifecycle_suspects)}",
            "",
            "| Kind | Identity | Generation | Host MiB | GPU MiB | Age frames | Variants | Last event | Classification |",
            "|---|---:|---:|---:|---:|---:|---:|---|---|",
        ]
    )
    for item in lifecycle_findings[:24]:
        lines.append(
            f"| {item.kind} | {item.identity} | {item.generation} | {item.retained_mib:.2f} | "
            f"{item.allocation_mib:.2f} | {item.age_frames} | {item.variant_count} | "
            f"{item.last_action}/{item.last_reason} | {item.classification} |"
        )
    lines.extend(["", "## Lifecycle event balance", ""])
    if event_summary:
        lines.extend(
            [
                "| Kind | Creates | Replaces | Release requests | Pending | Evictions | Destroys |",
                "|---|---:|---:|---:|---:|---:|---:|",
            ]
        )
        for kind, actions in sorted(event_summary.items()):
            lines.append(
                f"| {kind} | {actions.get('create', 0)} | {actions.get('replace', 0)} | "
                f"{actions.get('release-requested', 0)} | {actions.get('retire-pending', 0)} | "
                f"{actions.get('evict', 0)} | {actions.get('destroy', 0)} |"
            )
    else:
        lines.append("No identity-level event file was present for this capture.")
    lines.extend(
        [
            "",
            "## Interpretation",
            "",
            "- `sustained-growth-suspect`: meaningful net growth with positive whole-capture and final-quarter slopes.",
            "- `warmup-or-bounded-cache`: early growth whose final-quarter slope flattened.",
            "- `high-bounded-churn`: substantial allocation/release traffic without equivalent retained growth.",
            "- If process footprint rises while all tracked renderer categories are stable, investigate host/driver allocations with Instruments Allocations, VM Tracker, and Metal Resource Events.",
            "- Increasing pending-release counts indicate synchronization or retirement backlog rather than ownership growth alone.",
            "",
            "See `native-memory-timeline.svg`, `native-memory-subsystems.svg`, `native-memory-process-subsystems.svg`, `native-memory-suspects.csv`, `native-memory-process-suspects.csv`, and `native-memory-leaks.csv` beside this report.",
        ]
    )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def analyze(input_path: Path, output: Path) -> dict[str, Path]:
    rows = read_rows(input_path)
    findings = build_findings(rows)
    process_findings = build_process_findings(rows)
    event_rows = read_dict_rows(input_path.parent / "native-memory-events-latest.csv")
    retained_rows = read_dict_rows(input_path.parent / "native-memory-retained-latest.csv")
    lifecycle_findings, event_summary = build_lifecycle_findings(event_rows, retained_rows)
    output.mkdir(parents=True, exist_ok=True)
    outputs = {
        "suspects": output / "native-memory-suspects.csv",
        "timeline": output / "native-memory-timeline.svg",
        "subsystems": output / "native-memory-subsystems.svg",
        "process_suspects": output / "native-memory-process-suspects.csv",
        "process_subsystems": output / "native-memory-process-subsystems.svg",
        "report": output / "native-memory-report.md",
        "leaks": output / "native-memory-leaks.csv",
    }
    write_suspects(outputs["suspects"], findings)
    write_timeline(outputs["timeline"], rows)
    write_subsystems(outputs["subsystems"], findings)
    write_suspects(outputs["process_suspects"], process_findings)
    write_subsystems(
        outputs["process_subsystems"],
        process_findings,
        "Process, allocator, and VM memory",
    )
    write_lifecycle_findings(outputs["leaks"], lifecycle_findings)
    write_report(
        outputs["report"],
        rows,
        findings,
        process_findings,
        lifecycle_findings,
        event_summary,
    )
    return outputs


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", nargs="?", type=Path, default=DEFAULT_INPUT)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()

    outputs = analyze(args.input, args.output_dir or args.input.parent)
    print(outputs["report"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
