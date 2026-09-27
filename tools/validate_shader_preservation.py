#!/usr/bin/env python3
"""Fail closed if regeneration loses or changes an existing shader contract.

This supplements the skill's candidate range/addition validator. It compares
complete stock-cache inventories and the entire override manifest, including
pipeline-pair declarations. Shader bytecode may change; hash, filename, texture
and specialization metadata, decoded stage, entry point, early-test mode and
variant availability must remain intact. Both SPIR-V and Metal AIR are decoded
from the actual Zstandard arrays, not trusted from entry sizes alone.

Build the small decoder bridge once (macOS example):
  c++ -std=c++17 -shared -fPIC tools/shader_preservation_smolv.cpp \\
    tools/XenosRecomp/thirdparty/smol-v/source/smolv.cpp \\
    -I tools/XenosRecomp/thirdparty/smol-v/source -o /tmp/liberty_smolv.dylib

Then pass --before, --candidate, --before-overrides, --candidate-overrides,
--smolv-library /tmp/liberty_smolv.dylib and optionally --report inventory.json.
The cache and manifest inputs are never modified. Stage inspection is mandatory.
Use --spirv-val /absolute/path/to/spirv-val for semantic validation of every
candidate variant. Vulkan 1.2 is the native renderer's minimum device API;
no layout or legalization checks are disabled. This does not replace draw-time
Vulkan validation or visual comparison.
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.util
import hashlib
import importlib.util
import json
import re
import struct
import subprocess
import sys
from pathlib import Path


def load_range_validator():
    path = Path(__file__).resolve().with_name("validate_shader_cache_candidate.py")
    spec = importlib.util.spec_from_file_location("liberty_shader_range_validator", path)
    if not spec or not spec.loader:
        raise ValueError(f"cannot load existing cache validator: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


RANGES = load_range_validator()
MAX_DECOMPRESSED_CACHE = 1024 * 1024 * 1024
MAX_DECODED_MODULE = 64 * 1024 * 1024
SPIRV_MAGIC = 0x07230203
OP_ENTRY_POINT = 15
OP_EXECUTION_MODE = 16
EARLY_FRAGMENT_TESTS = 9
STAGES = {0: "vertex", 4: "pixel"}


class SpirvValidator:
    def __init__(self, executable: Path):
        self.executable = str(executable.resolve())
        self.validated_modules = 0

    def validate(self, data: bytes, label: str) -> None:
        # SPIRV-Tools accepts '-' as stdin, avoiding thousands of temporary files.
        # See tools/val/val.cpp in KhronosGroup/SPIRV-Tools. Device admission in
        # vulkan_device.cpp requires Vulkan 1.2 for this native shader cache.
        try:
            result = subprocess.run(
                [self.executable, "--target-env", "vulkan1.2", "-"], input=data,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30,
                check=False)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise ValueError(f"{label}: spirv-val could not complete: {error}") from error
        if result.returncode != 0:
            diagnostic = (result.stderr + result.stdout).decode("utf-8", errors="replace")
            raise ValueError(f"{label}: spirv-val exited {result.returncode}: {diagnostic.strip()}")
        self.validated_modules += 1


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def array_body(text: str, declaration: str, source: Path) -> str:
    pattern = re.compile(re.escape(declaration) + r"\s*=\s*\{(.*?)\};", re.DOTALL)
    matches = pattern.findall(text)
    if len(matches) != 1:
        raise ValueError(f"{source}: expected exactly one {declaration}")
    return matches[0]


def compressed_array(text: str, name: str, source: Path) -> bytes:
    body = array_body(text, f"const uint8_t {name}[]", source)
    if (not re.fullmatch(r"[0-9,\s]*", body) or
            re.search(r",\s*,|^\s*,|\d\s+\d", body)):
        raise ValueError(f"{source}: malformed byte array {name}")
    try:
        return bytes(int(match.group()) for match in re.finditer(r"\d+", body))
    except ValueError as error:
        raise ValueError(f"{source}: {name} contains a value outside uint8_t") from error


class NativeDecoders:
    def __init__(self, smolv_library: Path, zstd_library: str | None = None):
        library = zstd_library or ctypes.util.find_library("zstd")
        if not library:
            raise ValueError("libzstd not found; provide --zstd-library")
        self.zstd = ctypes.CDLL(library)
        self.zstd.ZSTD_decompress.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                             ctypes.c_void_p, ctypes.c_size_t]
        self.zstd.ZSTD_decompress.restype = ctypes.c_size_t
        self.zstd.ZSTD_isError.argtypes = [ctypes.c_size_t]
        self.zstd.ZSTD_isError.restype = ctypes.c_uint
        self.zstd.ZSTD_getErrorName.argtypes = [ctypes.c_size_t]
        self.zstd.ZSTD_getErrorName.restype = ctypes.c_char_p
        self.smolv = ctypes.CDLL(str(smolv_library.resolve()))
        self.smolv.liberty_smolv_decoded_size.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        self.smolv.liberty_smolv_decoded_size.restype = ctypes.c_size_t
        self.smolv.liberty_smolv_decode.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                                  ctypes.c_void_p, ctypes.c_size_t]
        self.smolv.liberty_smolv_decode.restype = ctypes.c_int

    def decompress(self, compressed: bytes, expected: int, label: str) -> bytes:
        if not 0 < expected <= MAX_DECOMPRESSED_CACHE:
            raise ValueError(f"{label}: invalid decompressed cache size {expected}")
        output = ctypes.create_string_buffer(expected)
        size = self.zstd.ZSTD_decompress(output, expected, compressed, len(compressed))
        if self.zstd.ZSTD_isError(size):
            reason = self.zstd.ZSTD_getErrorName(size).decode("utf-8", errors="replace")
            raise ValueError(f"{label}: Zstandard decompression failed: {reason}")
        if size != expected:
            raise ValueError(f"{label}: decompressed {size} bytes, expected {expected}")
        return output.raw

    def decode_spirv(self, encoded: bytes, label: str) -> bytes:
        size = self.smolv.liberty_smolv_decoded_size(encoded, len(encoded))
        if not 0 < size <= MAX_DECODED_MODULE:
            raise ValueError(f"{label}: invalid SMOL-V decoded size {size}")
        output = ctypes.create_string_buffer(size)
        if not self.smolv.liberty_smolv_decode(encoded, len(encoded), output, size):
            raise ValueError(f"{label}: SMOL-V decode failed")
        return output.raw


def spirv_contract(data: bytes, label: str) -> dict:
    if len(data) < 20 or len(data) % 4:
        raise ValueError(f"{label}: malformed SPIR-V word buffer")
    words = struct.unpack(f"<{len(data) // 4}I", data)
    if words[0] != SPIRV_MAGIC or not words[3] or words[4] != 0:
        raise ValueError(f"{label}: invalid SPIR-V header")
    entry_points = []
    early_test_entries = set()
    cursor = 5
    while cursor < len(words):
        count = words[cursor] >> 16
        opcode = words[cursor] & 0xFFFF
        if not count or cursor + count > len(words):
            raise ValueError(f"{label}: invalid SPIR-V instruction at word {cursor}")
        operands = words[cursor + 1:cursor + count]
        if opcode == OP_ENTRY_POINT:
            if len(operands) < 3 or operands[0] not in STAGES:
                raise ValueError(f"{label}: unsupported/malformed shader entry point")
            raw_name = struct.pack(f"<{len(operands) - 2}I", *operands[2:])
            if b"\0" not in raw_name:
                raise ValueError(f"{label}: unterminated entry-point name")
            name = raw_name.split(b"\0", 1)[0].decode("utf-8")
            if not name or not 0 < operands[1] < words[3]:
                raise ValueError(f"{label}: invalid entry-point name/id")
            entry_points.append((operands[0], operands[1], name))
        elif opcode == OP_EXECUTION_MODE:
            if len(operands) < 2:
                raise ValueError(f"{label}: malformed execution mode")
            if operands[1] == EARLY_FRAGMENT_TESTS:
                early_test_entries.add(operands[0])
        cursor += count
    if len(entry_points) != 1:
        raise ValueError(f"{label}: expected one entry point, found {len(entry_points)}")
    model, identifier, name = entry_points[0]
    if early_test_entries - {identifier} or (early_test_entries and model != 4):
        raise ValueError(f"{label}: invalid EarlyFragmentTests entry point")
    return {"stage": STAGES[model], "entry_point": name,
            "early_fragment_tests": identifier in early_test_entries}


def cache_inventory(path: Path, decoders: NativeDecoders,
                    validator: SpirvValidator | None = None) -> dict:
    source_bytes = path.read_bytes()
    text = source_bytes.decode("utf-8")
    body = array_body(text, "ShaderCacheEntry g_shaderCacheEntries[]", path)
    entries = RANGES.parse_entries(body, path)
    if RANGES.ENTRY_PATTERN.sub("", body).strip():
        raise ValueError(f"{path}: unparsed shader table syntax")
    count = RANGES.declared_size(text, "g_shaderCacheEntryCount", path)
    hashes = [entry.shader_hash for entry in entries]
    if count != len(entries) or len(hashes) != len(set(hashes)):
        raise ValueError(f"{path}: entry-count mismatch or duplicate shader hash")
    if hashes != sorted(hashes):
        raise ValueError(f"{path}: shader hashes are not sorted")
    totals = {}
    blobs = {}
    for kind in ("Spirv", "Air"):
        compressed = compressed_array(text, f"g_compressed{kind}Cache", path)
        declared_compressed = RANGES.declared_size(text, f"g_{kind.lower()}CacheCompressedSize", path)
        if len(compressed) != declared_compressed:
            raise ValueError(f"{path}: {kind} compressed byte count mismatch")
        total = RANGES.declared_size(text, f"g_{kind.lower()}CacheDecompressedSize", path)
        blobs[kind] = decoders.decompress(compressed, total, f"{path}: {kind}")
        totals[kind] = total
    spirv_ranges = []
    air_ranges = []
    inventory = {}
    for entry in entries:
        shader_hash = f"{entry.shader_hash:016X}"
        label = f"{path}: {shader_hash} ({entry.filename})"
        if not 0 <= entry.shader_hash < 1 << 64:
            raise ValueError(f"{label}: hash outside uint64_t")
        if entry.used_texture_mask is None or not 0 <= entry.used_texture_mask < 1 << 26:
            raise ValueError(f"{label}: missing/invalid usedTextureMask")
        if not 0 <= entry.spec_constants_mask < 1 << 32:
            raise ValueError(f"{label}: invalid specConstantsMask")
        if not entry.spirv_size or not entry.air_size:
            raise ValueError(f"{label}: empty SPIR-V or AIR variant")
        if not entry.late_spirv_size and entry.late_spirv_offset:
            raise ValueError(f"{label}: late SPIR-V offset without data")
        variants = {}
        for variant, offset, size in (("early", entry.spirv_offset, entry.spirv_size),
                                      ("late", entry.late_spirv_offset, entry.late_spirv_size)):
            if not size:
                continue
            if offset + size > totals["Spirv"]:
                raise ValueError(f"{label}: {variant} SPIR-V range exceeds cache")
            spirv_ranges.append((offset, size, f"{shader_hash} {variant}"))
            decoded = decoders.decode_spirv(blobs["Spirv"][offset:offset + size], label)
            variants[variant] = {**spirv_contract(decoded, f"{label} {variant}"),
                                 "decoded_bytes": len(decoded), "sha256": digest(decoded)}
            if validator is not None:
                validator.validate(decoded, f"{label} {variant}")
        if "late" in variants and (variants["late"]["stage"] != "pixel" or
                                   variants["early"]["stage"] != "pixel" or
                                   variants["late"]["early_fragment_tests"]):
            raise ValueError(f"{label}: invalid late pixel-shader variant")
        if entry.air_offset + entry.air_size > totals["Air"]:
            raise ValueError(f"{label}: AIR range exceeds cache")
        air = blobs["Air"][entry.air_offset:entry.air_offset + entry.air_size]
        if not any(air):
            raise ValueError(f"{label}: AIR contains no payload")
        air_ranges.append((entry.air_offset, entry.air_size, shader_hash))
        inventory[shader_hash] = {
            "filename": entry.filename, "used_texture_mask": entry.used_texture_mask,
            "spec_constants_mask": entry.spec_constants_mask,
            "dxil_present": bool(entry.dxil_size), "variants": variants,
            "air_bytes": len(air), "air_sha256": digest(air),
        }
    RANGES.validate_ranges(spirv_ranges, totals["Spirv"], f"{path}: SPIR-V")
    RANGES.validate_ranges(air_ranges, totals["Air"], f"{path}: AIR")
    return {"path": str(path), "source_sha256": digest(source_bytes),
            "entry_count": count, "entries": inventory}


def compare_inventories(before: dict, candidate: dict) -> list[str]:
    old, new = before["entries"], candidate["entries"]
    errors = []
    if before["entry_count"] != candidate["entry_count"]:
        errors.append(f"stock count changed: {before['entry_count']} -> {candidate['entry_count']}")
    for shader_hash in sorted(old.keys() - new.keys()):
        errors.append(f"dropped stock shader {shader_hash}: {old[shader_hash]['filename']}")
    for shader_hash in sorted(new.keys() - old.keys()):
        errors.append(f"unexpected stock shader addition {shader_hash}")
    for shader_hash in sorted(old.keys() & new.keys()):
        previous, current = old[shader_hash], new[shader_hash]
        for field in ("filename", "used_texture_mask", "spec_constants_mask", "dxil_present"):
            if previous[field] != current[field]:
                errors.append(f"{shader_hash}: {field} changed: {previous[field]!r} -> {current[field]!r}")
        if previous["variants"].keys() != current["variants"].keys():
            errors.append(f"{shader_hash}: SPIR-V variant availability changed")
        for variant in previous["variants"].keys() & current["variants"].keys():
            for field in ("stage", "entry_point", "early_fragment_tests"):
                if previous["variants"][variant][field] != current["variants"][variant][field]:
                    errors.append(f"{shader_hash}: {variant} {field} changed")
    return errors


def unique_json_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def override_inventory(path: Path) -> dict:
    raw = path.read_bytes()
    manifest = json.loads(raw, object_pairs_hook=unique_json_object)
    if not isinstance(manifest, dict) or not isinstance(manifest.get("overrides"), list):
        raise ValueError(f"{path}: invalid override manifest")
    identities = set()
    for entry in manifest["overrides"]:
        if not isinstance(entry, dict) or entry.get("stage") not in ("vertex", "pixel"):
            raise ValueError(f"{path}: malformed override entry")
        shader_hash = int(str(entry.get("hash", "")), 16)
        if not 0 <= shader_hash < 1 << 64:
            raise ValueError(f"{path}: invalid override hash")
        identity = (entry["stage"], shader_hash)
        if identity in identities:
            raise ValueError(f"{path}: duplicate override {identity}")
        identities.add(identity)
    return {"path": str(path), "source_sha256": digest(raw),
            "entry_count": len(identities), "manifest": manifest}


def compare_overrides(before: dict, candidate: dict) -> list[str]:
    # JSON types are part of the manifest contract: Python's True == 1 and
    # 1.0 == 1 must not conceal metadata edits.
    if (json.dumps(before["manifest"], sort_keys=True, separators=(",", ":")) ==
            json.dumps(candidate["manifest"], sort_keys=True, separators=(",", ":"))):
        return []
    return ["override manifest changed (all entries, metadata, ordering and counterpart pairs must be preserved)"]


def preferred_sources_tsv(inventory: dict) -> str:
    lines = [f"liberty-shader-sources-v1\t{inventory['entry_count']}"]
    for shader_hash, entry in sorted(inventory["entries"].items()):
        filename = entry["filename"]
        if any(character in filename for character in ("\t", "\r", "\n", "\0")):
            raise ValueError(f"{shader_hash}: filename cannot be represented in a source map")
        lines.append(f"{shader_hash}\t{filename}")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for argument in ("before", "candidate", "before-overrides", "candidate-overrides", "smolv-library"):
        parser.add_argument(f"--{argument}", type=Path, required=True)
    parser.add_argument("--zstd-library")
    parser.add_argument("--spirv-val", type=Path,
                        help="validate all candidate variants with this SPIRV-Tools executable")
    parser.add_argument("--report", type=Path)
    parser.add_argument("--preferred-sources", type=Path,
                        help="on success, export the baseline hash/filename map for XenosRecomp --preserve-sources")
    args = parser.parse_args()
    report = {"schema_version": 1, "status": "failed", "errors": []}
    outputs_valid = False
    try:
        inputs = (args.before, args.candidate, args.before_overrides, args.candidate_overrides,
                  args.smolv_library) + ((args.spirv_val,) if args.spirv_val else ())
        outputs = [path.resolve() for path in (args.report, args.preferred_sources) if path]
        if len(outputs) != len(set(outputs)) or any(path in {item.resolve() for item in inputs}
                                                   for path in outputs):
            raise ValueError("output paths must be distinct and must not overwrite an input")
        outputs_valid = True
        decoders = NativeDecoders(args.smolv_library, args.zstd_library)
        validator = SpirvValidator(args.spirv_val) if args.spirv_val else None
        report["before"] = cache_inventory(args.before, decoders)
        report["candidate"] = cache_inventory(args.candidate, decoders, validator)
        if validator is not None:
            report["spirv_validation"] = {
                "executable": validator.executable, "target_env": "vulkan1.2",
                "validated_modules": validator.validated_modules}
        report["before_overrides"] = override_inventory(args.before_overrides)
        report["candidate_overrides"] = override_inventory(args.candidate_overrides)
        report["errors"] = compare_inventories(report["before"], report["candidate"])
        report["errors"] += compare_overrides(report["before_overrides"], report["candidate_overrides"])
        if not report["errors"]:
            if args.preferred_sources:
                args.preferred_sources.write_text(preferred_sources_tsv(report["before"]), encoding="utf-8")
            report["status"] = "passed"
    except (OSError, ValueError, AttributeError) as error:
        report["errors"].append(str(error))
    if args.report and outputs_valid:
        args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"shader_preservation={report['status']}")
    if report["errors"]:
        for error in report["errors"][:20]:
            print(error, file=sys.stderr)
        print(f"preservation_errors={len(report['errors'])}", file=sys.stderr)
        return 1
    print(f"stock_entries={report['candidate']['entry_count']}")
    print(f"override_entries={report['candidate_overrides']['entry_count']}")
    print("decoded_stages_and_variant_contracts=preserved")
    if "spirv_validation" in report:
        print(f"spirv_validated_modules={report['spirv_validation']['validated_modules']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
