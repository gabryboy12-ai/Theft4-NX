#!/usr/bin/env python3
"""Validate a regenerated LibertyRecomp shader cache before replacing the live one."""

from __future__ import annotations

import argparse
import json
import re
from dataclasses import dataclass
from pathlib import Path


ENTRY_PATTERN = re.compile(
    r"""^\s*\{\s*
        0x(?P<hash>[0-9A-Fa-f]+)\s*,\s*
        (?P<dxil_offset>\d+)\s*,\s*(?P<dxil_size>\d+)\s*,\s*
        (?P<spirv_offset>\d+)\s*,\s*(?P<spirv_size>\d+)\s*,\s*
        (?P<late_spirv_offset>\d+)\s*,\s*(?P<late_spirv_size>\d+)\s*,\s*
        (?P<air_offset>\d+)\s*,\s*(?P<air_size>\d+)\s*,\s*
        (?P<spec_constants_mask>\d+)\s*,\s*
        \"(?P<filename>[^\"]+)\"
        (?:\s*,\s*nullptr\s*,\s*(?P<used_texture_mask>\d+))?
        \s*\}\s*,\s*$""",
    re.VERBOSE | re.MULTILINE,
)


@dataclass(frozen=True)
class CacheEntry:
    shader_hash: int
    dxil_offset: int
    dxil_size: int
    spirv_offset: int
    spirv_size: int
    late_spirv_offset: int
    late_spirv_size: int
    air_offset: int
    air_size: int
    spec_constants_mask: int
    filename: str
    used_texture_mask: int | None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", required=True, type=Path)
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    return parser.parse_args()


def parse_entries(text: str, source: Path) -> list[CacheEntry]:
    entries = []
    for match in ENTRY_PATTERN.finditer(text):
        values = match.groupdict()
        entries.append(
            CacheEntry(
                shader_hash=int(values["hash"], 16),
                dxil_offset=int(values["dxil_offset"]),
                dxil_size=int(values["dxil_size"]),
                spirv_offset=int(values["spirv_offset"]),
                spirv_size=int(values["spirv_size"]),
                late_spirv_offset=int(values["late_spirv_offset"]),
                late_spirv_size=int(values["late_spirv_size"]),
                air_offset=int(values["air_offset"]),
                air_size=int(values["air_size"]),
                spec_constants_mask=int(values["spec_constants_mask"]),
                filename=values["filename"],
                used_texture_mask=(
                    int(values["used_texture_mask"])
                    if values["used_texture_mask"] is not None
                    else None
                ),
            )
        )
    if not entries:
        raise ValueError(f"{source}: no ShaderCacheEntry records found")
    return entries


def declared_size(text: str, name: str, source: Path) -> int:
    pattern = re.compile(rf"const size_t {re.escape(name)} = (\d+);")
    matches = pattern.findall(text)
    if len(matches) != 1:
        raise ValueError(f"{source}: expected one declaration of {name}, found {len(matches)}")
    return int(matches[0])


def compressed_byte_count(text: str, name: str, source: Path) -> int:
    marker = f"const uint8_t {name}[] = {{"
    start = text.find(marker)
    if start < 0:
        raise ValueError(f"{source}: missing {name}")
    body_start = start + len(marker)
    body_end = text.find("};", body_start)
    if body_end < 0:
        raise ValueError(f"{source}: unterminated {name}")
    body = text[body_start:body_end]
    if re.fullmatch(r"[0-9,\s]*", body) is None:
        raise ValueError(f"{source}: {name} contains non-byte syntax")
    stripped = body.strip()
    if not stripped:
        return 0
    comma_count = stripped.count(",")
    return comma_count if stripped.endswith(",") else comma_count + 1


def validate_ranges(
    ranges: list[tuple[int, int, str]], declared_total: int, label: str
) -> None:
    cursor = 0
    for offset, size, owner in sorted(ranges):
        if offset != cursor:
            relation = "overlap" if offset < cursor else "gap"
            raise ValueError(
                f"{label}: {relation} before {owner}: offset={offset}, expected={cursor}"
            )
        cursor = offset + size
    if cursor != declared_total:
        raise ValueError(f"{label}: ranges end at {cursor}, declared size is {declared_total}")


def manifest_hashes(path: Path) -> set[int]:
    data = json.loads(path.read_text())
    records = data.get("unseen") if isinstance(data, dict) else data
    if not isinstance(records, list):
        raise ValueError(f"{path}: expected an unseen list or a legacy top-level list")
    hashes = []
    for record in records:
        if not isinstance(record, dict) or "hash" not in record:
            raise ValueError(f"{path}: every unseen record must contain hash")
        hashes.append(int(str(record["hash"]), 16))
    if len(hashes) != len(set(hashes)):
        raise ValueError(f"{path}: duplicate hashes in unseen manifest")
    return set(hashes)


def main() -> None:
    args = parse_args()
    before_text = args.before.read_text()
    candidate_text = args.candidate.read_text()
    before_entries = parse_entries(before_text, args.before)
    candidate_entries = parse_entries(candidate_text, args.candidate)

    missing_texture_metadata = [
        entry.shader_hash
        for entry in candidate_entries
        if entry.used_texture_mask is None
    ]
    if missing_texture_metadata:
        values = ", ".join(
            f"{shader_hash:016X}" for shader_hash in missing_texture_metadata
        )
        raise ValueError(f"candidate entries lack used-texture metadata: {values}")

    supported_texture_stage_count = 26
    supported_texture_mask = (1 << supported_texture_stage_count) - 1
    unsupported_texture_metadata = [
        entry.shader_hash
        for entry in candidate_entries
        if entry.used_texture_mask is not None
        and entry.used_texture_mask & ~supported_texture_mask
    ]
    if unsupported_texture_metadata:
        values = ", ".join(
            f"{shader_hash:016X}" for shader_hash in unsupported_texture_metadata
        )
        raise ValueError(
            "candidate entries reference texture stages outside GTA IV's native interface: "
            + values
        )

    before_hashes = [entry.shader_hash for entry in before_entries]
    candidate_hashes = [entry.shader_hash for entry in candidate_entries]
    before_set = set(before_hashes)
    candidate_set = set(candidate_hashes)

    if len(candidate_hashes) != len(candidate_set):
        raise ValueError("candidate cache contains duplicate shader hashes")
    if candidate_hashes != sorted(candidate_hashes):
        raise ValueError("candidate shader hashes are not numerically sorted")
    missing_old = before_set - candidate_set
    if missing_old:
        values = ", ".join(f"{value:016X}" for value in sorted(missing_old))
        raise ValueError(f"candidate dropped old shader hashes: {values}")

    additions = candidate_set - before_set
    expected_additions = manifest_hashes(args.manifest)
    if additions != expected_additions:
        missing = expected_additions - additions
        extra = additions - expected_additions
        raise ValueError(
            "candidate additions do not equal manifest unseen hashes; "
            f"missing={[f'{value:016X}' for value in sorted(missing)]}, "
            f"extra={[f'{value:016X}' for value in sorted(extra)]}"
        )

    before_names = {entry.shader_hash: entry.filename for entry in before_entries}
    candidate_names = {entry.shader_hash: entry.filename for entry in candidate_entries}
    renamed = {
        shader_hash: (filename, candidate_names[shader_hash])
        for shader_hash, filename in before_names.items()
        if candidate_names[shader_hash] != filename
    }
    if renamed:
        raise ValueError(f"candidate changed old shader filenames: {renamed}")

    spirv_ranges = []
    air_ranges = []
    for entry in candidate_entries:
        owner = f"{entry.shader_hash:016X} ({entry.filename})"
        if entry.spirv_size == 0:
            raise ValueError(f"candidate has empty SPIR-V for {owner}")
        if entry.air_size == 0:
            raise ValueError(f"candidate has empty AIR for {owner}")
        spirv_ranges.append((entry.spirv_offset, entry.spirv_size, f"early {owner}"))
        if entry.late_spirv_size:
            spirv_ranges.append(
                (entry.late_spirv_offset, entry.late_spirv_size, f"late {owner}")
            )
        elif entry.late_spirv_offset:
            raise ValueError(f"candidate has late SPIR-V offset without data for {owner}")
        air_ranges.append((entry.air_offset, entry.air_size, owner))

    spirv_total = declared_size(candidate_text, "g_spirvCacheDecompressedSize", args.candidate)
    air_total = declared_size(candidate_text, "g_airCacheDecompressedSize", args.candidate)
    validate_ranges(spirv_ranges, spirv_total, "SPIR-V cache")
    validate_ranges(air_ranges, air_total, "AIR cache")

    declared_entry_count = declared_size(candidate_text, "g_shaderCacheEntryCount", args.candidate)
    if declared_entry_count != len(candidate_entries):
        raise ValueError(
            f"candidate entry count is {len(candidate_entries)}, declaration is {declared_entry_count}"
        )

    for array_name, size_name in (
        ("g_compressedAirCache", "g_airCacheCompressedSize"),
        ("g_compressedSpirvCache", "g_spirvCacheCompressedSize"),
    ):
        actual_size = compressed_byte_count(candidate_text, array_name, args.candidate)
        expected_size = declared_size(candidate_text, size_name, args.candidate)
        if actual_size != expected_size:
            raise ValueError(
                f"{array_name}: contains {actual_size} bytes, declaration is {expected_size}"
            )

    print(f"old_entries={len(before_entries)}")
    print(f"candidate_entries={len(candidate_entries)}")
    print(f"validated_additions={len(additions)}")
    print(f"spirv_decompressed_bytes={spirv_total}")
    print(f"air_decompressed_bytes={air_total}")
    print("shader_cache_candidate=valid")


if __name__ == "__main__":
    main()
