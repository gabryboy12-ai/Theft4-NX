#!/usr/bin/env python3
"""Recover exact dual-target shader source embedded in baseline Metal libraries.

The Metal compiler's -frecord-sources archives are HSRD/SARC bzip2 tar payloads.
This tool never extracts archive paths directly. Every AIR/SPIR-V artifact must
match its hash in a passed baseline preservation inventory before recovery.
With --common, replace only the original guarded common-header prefix and add
per-slot bias arguments to tfetch calls; all other shader-body bytes must match.
Original sources are retained separately. No shader cache is changed.
"""

from __future__ import annotations

import argparse
import bz2
import hashlib
import io
import json
import re
import struct
import tarfile
from pathlib import Path

PIXEL_PREAMBLE = ("#if defined(__air__) && !defined(XENOS_RECOMP_PIXEL_SHADER)\n"
                  "#define XENOS_RECOMP_PIXEL_SHADER\n#endif\n")


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def checked_slice(data, offset, size, label):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError(f"{label}: range exceeds Metal library")
    return data[offset:offset + size]


def embedded_source(data: bytes) -> tuple[bytes, str]:
    if len(data) < 88 or data[:4] != b"MTLB":
        raise ValueError("invalid Metal library header")
    if struct.unpack_from("<Q", data, 16)[0] != len(data):
        raise ValueError("Metal library size mismatch")
    function_offset, function_size, public_offset = struct.unpack_from("<QQQ", data, 24)
    cursor = function_offset + function_size + 4
    section = None
    while cursor < public_offset:
        tag = checked_slice(data, cursor, 4, "header tag")
        cursor += 4
        if tag == b"ENDT":
            break
        size = struct.unpack("<H", checked_slice(data, cursor, 2, "header tag size"))[0]
        cursor += 2
        payload = checked_slice(data, cursor, size, "header tag payload")
        cursor += size
        if tag in (b"HSRD", b"HSRC"):
            if section is not None or size != 16:
                raise ValueError("ambiguous/invalid embedded source section")
            offset, length = struct.unpack("<QQ", payload)
            section = (tag, checked_slice(data, offset, length, "source section"))
    if section is None:
        raise ValueError("Metal library has no embedded source archive")
    tag, section = section
    # The captured Metal 32023 archives use a 32-bit item count.
    if len(section) < 4 or struct.unpack_from("<I", section)[0] != 1:
        raise ValueError("expected exactly one source archive")
    cursor = 4
    for _ in range(2 if tag == b"HSRD" else 1):
        end = section.find(b"\0", cursor)
        if end < 0:
            raise ValueError("unterminated embedded source metadata")
        cursor = end + 1
    group_size = struct.unpack("<I", checked_slice(section, cursor, 4, "source tag group size"))[0]
    checked_slice(section, cursor, group_size, "source tag group")
    cursor += 4
    if checked_slice(section, cursor, 4, "source archive tag") != b"SARC":
        raise ValueError("missing SARC source archive tag")
    size = struct.unpack("<I", checked_slice(section, cursor + 4, 4, "SARC size"))[0]
    payload = checked_slice(section, cursor + 8, size, "SARC payload")
    end = payload.find(b"\0")
    if end < 0 or not payload[end + 1:].startswith(b"BZh"):
        raise ValueError("invalid source archive identifier/compression")
    archive = bz2.decompress(payload[end + 1:])
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:") as members:
        sources = [member for member in members.getmembers()
                   if member.isfile() and member.name.endswith(".metal")]
        if len(sources) != 1:
            raise ValueError("expected one original Metal source file")
        return members.extractfile(sources[0]).read(), sources[0].name


def split_common(source: str) -> tuple[str, str]:
    preamble = ""
    if source.startswith(PIXEL_PREAMBLE):
        preamble = PIXEL_PREAMBLE
        source = source[len(preamble):]
    if not source.startswith("#ifndef SHADER_COMMON_H_INCLUDED\n"):
        raise ValueError("recovered source does not start with the known common-header guard")
    depth = 0
    cursor = 0
    for line in source.splitlines(keepends=True):
        directive = re.match(r"\s*#\s*(if|ifdef|ifndef|endif)\b", line)
        if directive:
            depth += -1 if directive.group(1) == "endif" else 1
        cursor += len(line)
        if depth == 0:
            return preamble + source[:cursor], source[cursor:]
    raise ValueError("unterminated common-header guard")


def adapt_fetches(body: str) -> tuple[str, list[dict]]:
    sampler_offsets = {}
    pattern = (r"^#define\s+(\w+_SamplerDescriptorIndex)\s+"
               r"vk::RawBufferLoad<uint>\(g_PushConstants.SharedConstants\s*\+\s*(\d+)\)")
    for name, raw_offset in re.findall(pattern, body, re.MULTILINE):
        offset = int(raw_offset)
        if offset < 416 or (offset - 416) % 4 or (offset - 416) // 4 >= 26:
            raise ValueError(f"invalid sampler descriptor offset for {name}: {offset}")
        slot = (offset - 416) // 4
        if name in sampler_offsets and sampler_offsets[name] != slot:
            raise ValueError(f"ambiguous sampler declaration {name}")
        sampler_offsets[name] = slot
    matches = list(re.finditer(r"\btfetch\w*\s*\(", body))
    insertions = []
    for match in matches:
        cursor = match.end()
        depth = 1
        while cursor < len(body) and depth:
            if body[cursor] == "(":
                depth += 1
            elif body[cursor] == ")":
                depth -= 1
            cursor += 1
        if depth:
            raise ValueError("unterminated texture-fetch call")
        expression = body[match.end():cursor - 1]
        samplers = set(re.findall(r"\b\w+_SamplerDescriptorIndex\b", expression))
        if len(samplers) != 1:
            raise ValueError("texture fetch must use one identifiable sampler descriptor")
        sampler = samplers.pop()
        if sampler not in sampler_offsets or "GetTextureLodBias" in expression:
            raise ValueError(f"unmapped/already adapted sampler: {sampler}")
        insertion = f", GetTextureLodBias({sampler_offsets[sampler]})"
        insertions.append({"offset": cursor - 1, "text": insertion, "sampler": sampler,
                           "slot": sampler_offsets[sampler], "function": match.group().split("(")[0].strip()})
    adapted = body
    for insertion in reversed(insertions):
        offset = insertion["offset"]
        adapted = adapted[:offset] + insertion["text"] + adapted[offset:]
    restored = adapted
    for insertion in insertions:
        # Removing the earliest inserted text restores subsequent original offsets.
        offset = insertion["offset"]
        if restored[offset:offset + len(insertion["text"])] != insertion["text"]:
            raise ValueError("texture-fetch adaptation did not preserve source offsets")
        restored = restored[:offset] + restored[offset + len(insertion["text"]):]
    if restored != body:
        raise ValueError("shader arithmetic changed during texture-fetch adaptation")
    return adapted, insertions


def late_source(early: str) -> str:
    result = early
    for annotation in ("[[early_fragment_tests]]", "[earlydepthstencil]"):
        if result.count(annotation) != 1:
            raise ValueError(f"expected exactly one {annotation} annotation")
        result = result.replace(annotation, "")
    return result


def write_exact(path: Path, data: bytes):
    if path.exists() and path.read_bytes() != data:
        raise ValueError(f"refusing to overwrite a different recovery artifact: {path}")
    path.write_bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", required=True, type=Path)
    parser.add_argument("--restored-manifest", type=Path)
    parser.add_argument("--artifacts", type=Path)
    parser.add_argument("--recovered-original", type=Path,
                        help="reuse a preserved extraction recovery.json, checking every source SHA")
    parser.add_argument("--common", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    inventory = json.loads(args.inventory.read_text())
    if inventory.get("status") != "passed":
        raise ValueError("baseline inventory did not pass preservation validation")
    baseline = inventory["before"]["entries"]
    prior = None
    if args.recovered_original:
        if args.restored_manifest or args.artifacts:
            parser.error("--recovered-original cannot be combined with raw restoration inputs")
        prior = json.loads(args.recovered_original.read_text())
        if (prior.get("version") != 1 or
                prior.get("baseline_cache_sha256") != inventory["before"]["source_sha256"] or
                prior.get("entry_count") != len(prior["sources"])):
            raise ValueError("preserved extraction is not from this baseline")
        restored = prior["sources"]
    else:
        if not args.restored_manifest or not args.artifacts:
            parser.error("provide --recovered-original or both --restored-manifest and --artifacts")
        restored = json.loads(args.restored_manifest.read_text())["unseen"]
    if len(restored) != len({entry["hash"] for entry in restored}):
        raise ValueError("duplicate restored shader hash")
    common = args.common.read_text() if args.common else None
    if common is not None and "GetTextureLodBias" not in common:
        raise ValueError("updated common header does not implement GetTextureLodBias")
    args.output.mkdir(parents=True, exist_ok=True)
    original_dir = args.output / "original"
    original_dir.mkdir(exist_ok=True)
    records = []
    rows = [f"liberty-recovered-sources-v1\t{len(restored)}"]
    for entry in sorted(restored, key=lambda record: record["hash"]):
        shader_hash = entry["hash"]
        original = baseline[shader_hash]
        if prior is not None:
            source_bytes = (args.recovered_original.parent / "original" / f"{shader_hash}.hlsl").read_bytes()
            if (sha256(source_bytes) != entry["source_sha256"] or
                    entry["air_sha256"] != original["air_sha256"]):
                raise ValueError(f"{shader_hash}: preserved extraction hash mismatch")
            member = entry["archive_member"]
        else:
            air = (args.artifacts / f"{shader_hash}.air").read_bytes()
            if sha256(air) != original["air_sha256"]:
                raise ValueError(f"{shader_hash}: AIR differs from the preserved baseline")
            source_bytes, member = embedded_source(air)
        original_path = original_dir / f"{shader_hash}.hlsl"
        write_exact(original_path, source_bytes)
        source = source_bytes.decode("utf-8")
        prefix, body = split_common(source)
        record = {"hash": shader_hash, "filename": original["filename"],
                  "stage": original["variants"]["early"]["stage"],
                  "used_texture_mask": original["used_texture_mask"],
                  "spec_constants_mask": original["spec_constants_mask"],
                  "archive_member": member, "air_sha256": original["air_sha256"],
                  "source_sha256": sha256(source_bytes), "body_sha256": sha256(body.encode()),
                  "original_path": str(original_path), "old_common_sha256": sha256(prefix.encode()),
                  "late_variant": "late" in original["variants"]}
        if record["stage"] != entry["stage"]:
            raise ValueError(f"{shader_hash}: restoration stage does not match baseline")
        if common is not None:
            adapted_body, insertions = adapt_fetches(body)
            early = (PIXEL_PREAMBLE if record["stage"] == "pixel" else "") + common + adapted_body
            early_path = args.output / f"{shader_hash}.early.hlsl"
            write_exact(early_path, early.encode())
            late_path = None
            if record["late_variant"]:
                late_path = args.output / f"{shader_hash}.late.hlsl"
                write_exact(late_path, late_source(early).encode())
            record.update({"new_common_sha256": sha256(common.encode()), "fetches": insertions,
                           "adapted_early_sha256": sha256(early.encode())})
            rows.append("\t".join((shader_hash, record["stage"], str(record["spec_constants_mask"]),
                                   str(record["used_texture_mask"]), record["filename"],
                                   early_path.name, late_path.name if late_path else "-")))
        records.append(record)
    report = {"version": 1, "entry_count": len(records), "sources": records,
              "baseline_cache_sha256": inventory["before"]["source_sha256"],
              "body_preservation": "only tfetch bias arguments added" if common else "unmodified extraction"}
    write_exact(args.output / "recovery.json", (json.dumps(report, indent=2) + "\n").encode())
    if common is not None:
        write_exact(args.output / "recovered-sources.tsv", ("\n".join(rows) + "\n").encode())
    print(f"recovered_sources={len(records)} adapted={common is not None}")


if __name__ == "__main__":
    main()
