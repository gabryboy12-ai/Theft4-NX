#!/usr/bin/env python3
"""Regression tests for shader-loss and metadata/variant/override preservation."""

import copy
import importlib.util
import json
import struct
import subprocess
import tempfile
import unittest
from unittest import mock
from pathlib import Path


SPEC = importlib.util.spec_from_file_location(
    "shader_preservation", Path(__file__).with_name("validate_shader_preservation.py"))
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def spirv(stage=0, early=False):
    name = b"main\0\0\0\0"
    operands = [stage, 1, *struct.unpack("<2I", name)]
    words = [MODULE.SPIRV_MAGIC, 0x00010000, 0, 2, 0,
             ((len(operands) + 1) << 16) | MODULE.OP_ENTRY_POINT, *operands]
    if early:
        words.extend([(3 << 16) | MODULE.OP_EXECUTION_MODE, 1, MODULE.EARLY_FRAGMENT_TESTS])
    return struct.pack(f"<{len(words)}I", *words)


class FixtureDecoders:
    """Isolate table/range/contract validation from the separately exercised native codecs."""
    def decompress(self, data, expected, label):
        if len(data) != expected:
            raise ValueError(f"{label}: bad fixture decompressed size")
        return data

    def decode_spirv(self, data, label):
        return data


def source(shaders):
    entries = []
    spirv_bytes = b""
    air_bytes = b""
    for shader in shaders:
        early = spirv(shader.get("stage", 0), shader.get("early", False))
        late = spirv(4) if shader.get("late", False) else b""
        early_offset = len(spirv_bytes)
        spirv_bytes += early
        late_offset = len(spirv_bytes) if late else 0
        spirv_bytes += late
        air = shader.get("air", b"MTLBfixture")
        entries.append(
            f'\t{{ 0x{shader["hash"]:X}, 0, 0, {early_offset}, {len(early)}, '
            f'{late_offset}, {len(late)}, {len(air_bytes)}, {len(air)}, '
            f'{shader.get("spec", 0)}, "{shader.get("filename", "shader/test.bin")}", '
            f'nullptr, {shader.get("textures", 0)} }},')
        air_bytes += air
    result = "ShaderCacheEntry g_shaderCacheEntries[] = {\n" + "\n".join(entries) + "\n};\n"
    for kind, data in (("Spirv", spirv_bytes), ("Air", air_bytes)):
        result += f"const uint8_t g_compressed{kind}Cache[] = {{" + ",".join(map(str, data)) + "};\n"
        result += f"const size_t g_{kind.lower()}CacheCompressedSize = {len(data)};\n"
        result += f"const size_t g_{kind.lower()}CacheDecompressedSize = {len(data)};\n"
    return result + f"const size_t g_shaderCacheEntryCount = {len(shaders)};\n"


class ShaderPreservationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.shaders = [{"hash": 1}, {"hash": 2, "stage": 4, "early": True, "late": True,
                                      "spec": 1794, "textures": 3}]

    def inventory(self, shaders, name="cache.cpp"):
        path = self.root / name
        path.write_text(source(shaders))
        return MODULE.cache_inventory(path, FixtureDecoders())

    def test_full_fixture_inventory_and_source_map(self):
        inventory = self.inventory(self.shaders)
        self.assertEqual(inventory["entry_count"], 2)
        self.assertEqual(inventory["entries"]["0000000000000001"]["variants"]["early"]["stage"], "vertex")
        self.assertEqual(MODULE.compare_inventories(inventory, inventory), [])
        self.assertEqual(MODULE.preferred_sources_tsv(inventory),
                         "liberty-shader-sources-v1\t2\n"
                         "0000000000000001\tshader/test.bin\n"
                         "0000000000000002\tshader/test.bin\n")

    def test_shader_loss_and_same_count_hash_replacement_fail(self):
        before = self.inventory(self.shaders)
        reduced = self.inventory(self.shaders[:1])
        self.assertTrue(any("dropped stock shader" in e for e in MODULE.compare_inventories(before, reduced)))
        changed = copy.deepcopy(self.shaders)
        changed[1]["hash"] = 3
        errors = MODULE.compare_inventories(before, self.inventory(changed))
        self.assertTrue(any("dropped stock shader" in e for e in errors))
        self.assertTrue(any("unexpected stock shader addition" in e for e in errors))

    def test_metadata_filename_and_decoded_stage_changes_fail(self):
        before = self.inventory(self.shaders)
        for field, value, diagnostic in (("filename", "shader/other.bin", "filename"),
                                          ("textures", 7, "used_texture_mask"),
                                          ("spec", 8, "spec_constants_mask"),
                                          ("stage", 4, "stage")):
            with self.subTest(field=field):
                changed = copy.deepcopy(self.shaders)
                changed[0][field] = value
                errors = MODULE.compare_inventories(before, self.inventory(changed))
                self.assertTrue(any(diagnostic in e for e in errors), errors)

    def test_late_variant_and_early_test_contract_are_preserved(self):
        before = self.inventory(self.shaders)
        changed = copy.deepcopy(self.shaders)
        changed[1]["late"] = False
        self.assertTrue(any("variant availability" in e for e in
                            MODULE.compare_inventories(before, self.inventory(changed))))
        changed = copy.deepcopy(self.shaders)
        changed[1]["early"] = False
        self.assertTrue(any("early_fragment_tests" in e for e in
                            MODULE.compare_inventories(before, self.inventory(changed))))

    def test_semantic_validator_checks_every_early_and_late_variant(self):
        path = self.root / "candidate.cpp"
        path.write_text(source(self.shaders))
        validator = MODULE.SpirvValidator(self.root / "spirv-val")
        result = subprocess.CompletedProcess([], 0, b"", b"")
        with mock.patch.object(MODULE.subprocess, "run", return_value=result) as run:
            MODULE.cache_inventory(path, FixtureDecoders(), validator)
        self.assertEqual(validator.validated_modules, 3)
        self.assertEqual(run.call_count, 3)
        for call in run.call_args_list:
            self.assertEqual(call.args[0], [str((self.root / "spirv-val").resolve()),
                                           "--target-env", "vulkan1.2", "-"])
            self.assertEqual(call.kwargs["input"][:4], struct.pack("<I", MODULE.SPIRV_MAGIC))
            self.assertEqual(call.kwargs["timeout"], 30)

    def test_semantic_validator_fails_closed(self):
        validator = MODULE.SpirvValidator(self.root / "spirv-val")
        rejected = subprocess.CompletedProcess([], 1, b"", b"invalid instruction")
        with mock.patch.object(MODULE.subprocess, "run", return_value=rejected):
            with self.assertRaisesRegex(ValueError, "candidate late: spirv-val exited 1"):
                validator.validate(spirv(), "candidate late")
        for error in (FileNotFoundError("missing tool"),
                      subprocess.TimeoutExpired("spirv-val", 30)):
            with self.subTest(error=error), \
                    mock.patch.object(MODULE.subprocess, "run", side_effect=error):
                with self.assertRaisesRegex(ValueError, "spirv-val could not complete"):
                    validator.validate(spirv(), "candidate early")
        self.assertEqual(validator.validated_modules, 0)

    def test_empty_air_invalid_metadata_duplicate_or_unparsed_entries_fail(self):
        for mutation in ({"air": b""}, {"air": b"\0"}, {"textures": 1 << 26}):
            with self.subTest(mutation=mutation):
                changed = copy.deepcopy(self.shaders)
                changed[0].update(mutation)
                with self.assertRaises(ValueError):
                    self.inventory(changed)
        with self.assertRaisesRegex(ValueError, "duplicate"):
            self.inventory([self.shaders[0], self.shaders[0]])
        path = self.root / "bad.cpp"
        path.write_text(source(self.shaders).replace("0x1,", "not_a_hash,"))
        with self.assertRaisesRegex(ValueError, "unparsed"):
            MODULE.cache_inventory(path, FixtureDecoders())

    def test_malformed_spirv_and_byte_arrays_fail(self):
        for data in (b"", b"\0" * 20, spirv() + b"\0\0\0\0", spirv()[:-4]):
            with self.subTest(data=data):
                with self.assertRaises(ValueError):
                    MODULE.spirv_contract(data, "fixture")
        for body in ("256", "-1", "1,,2", "1 2", ",1"):
            with self.subTest(body=body):
                with self.assertRaises(ValueError):
                    MODULE.compressed_array(f"const uint8_t blob[] = {{{body}}};", "blob", Path("fixture"))

    def test_all_override_fields_and_pipeline_pairs_must_match(self):
        manifest = {"version": 1, "overrides": [
            {"stage": "vertex", "hash": "0x1", "source": "v.hlsl", "pipeline_pair_id": 1,
             "counterpart_hashes": ["0x2"], "supported_sample_counts": [1, 2, 4]},
            {"stage": "pixel", "hash": "0x2", "source": "p.hlsl", "pipeline_pair_id": 1,
             "counterpart_hashes": ["0x1"]}]}
        path = self.root / "manifest.json"
        path.write_text(json.dumps(manifest))
        before = MODULE.override_inventory(path)
        for field, value in (("counterpart_hashes", []), ("pipeline_pair_id", 2),
                              ("pipeline_pair_id", True),
                              ("supported_sample_counts", [1]), ("source", "different.hlsl")):
            with self.subTest(field=field):
                changed = copy.deepcopy(manifest)
                changed["overrides"][0][field] = value
                path.write_text(json.dumps(changed))
                self.assertTrue(MODULE.compare_overrides(before, MODULE.override_inventory(path)))
        path.write_text('{"version":1,"version":2,"overrides":[]}')
        with self.assertRaisesRegex(ValueError, "duplicate JSON"):
            MODULE.override_inventory(path)

    def test_report_cannot_overwrite_baseline_or_source_map(self):
        baseline = self.root / "baseline.cpp"
        baseline.write_text("protected baseline")
        required = ["gate", "--before", str(baseline), "--candidate", str(baseline),
                    "--before-overrides", str(baseline), "--candidate-overrides", str(baseline),
                    "--smolv-library", str(self.root / "unused-library")]
        for outputs in (["--report", str(baseline)],
                        ["--report", str(baseline), "--preferred-sources", str(baseline)]):
            with self.subTest(outputs=outputs), mock.patch("sys.argv", required + outputs), \
                    mock.patch("sys.stdout"), mock.patch("sys.stderr"):
                self.assertEqual(MODULE.main(), 1)
                self.assertEqual(baseline.read_text(), "protected baseline")


if __name__ == "__main__":
    unittest.main()
