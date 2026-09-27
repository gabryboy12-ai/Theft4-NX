#!/usr/bin/env python3

import csv
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("analyze_native_memory.py")
SPEC = importlib.util.spec_from_file_location("analyze_native_memory", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class AnalyzeNativeMemoryTest(unittest.TestCase):
    def test_sustained_growth_and_bounded_churn_are_separated(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "capture.csv"
            fields = [
                "elapsed_seconds",
                "process_physical_footprint_bytes",
                "process_resident_bytes",
                "tracked_host_bytes",
                "tracked_gpu_bytes",
                "host_buffer_payloads_live_bytes",
                "host_buffer_payloads_peak_bytes",
                "host_buffer_payloads_cumulative_growth_bytes",
                "host_buffer_payloads_cumulative_shrink_bytes",
                "gpu_texture_images_live_bytes",
                "gpu_texture_images_peak_bytes",
                "gpu_texture_images_cumulative_growth_bytes",
                "gpu_texture_images_cumulative_shrink_bytes",
            ]
            mib = 1024 * 1024
            with source.open("w", newline="", encoding="utf-8") as stream:
                writer = csv.DictWriter(stream, fieldnames=fields)
                writer.writeheader()
                for index in range(9):
                    retained = index * 2 * mib
                    texture = (20 if index % 2 else 16) * mib
                    writer.writerow(
                        {
                            "elapsed_seconds": index * 10,
                            "process_physical_footprint_bytes": 500 * mib + retained,
                            "process_resident_bytes": 450 * mib + retained,
                            "tracked_host_bytes": retained,
                            "tracked_gpu_bytes": texture,
                            "host_buffer_payloads_live_bytes": retained,
                            "host_buffer_payloads_peak_bytes": retained,
                            "host_buffer_payloads_cumulative_growth_bytes": retained,
                            "host_buffer_payloads_cumulative_shrink_bytes": 0,
                            "gpu_texture_images_live_bytes": texture,
                            "gpu_texture_images_peak_bytes": 20 * mib,
                            "gpu_texture_images_cumulative_growth_bytes": (index + 1) * 4 * mib,
                            "gpu_texture_images_cumulative_shrink_bytes": index * 4 * mib,
                        }
                    )

            outputs = MODULE.analyze(source, root)
            for output in outputs.values():
                self.assertTrue(output.exists())
                self.assertGreater(output.stat().st_size, 0)

            with outputs["suspects"].open(encoding="utf-8") as stream:
                findings = {row["category"]: row for row in csv.DictReader(stream)}
            self.assertEqual(
                findings["host-buffer-payloads"]["classification"],
                "sustained-growth-suspect",
            )
            self.assertEqual(
                findings["gpu-texture-images"]["classification"],
                "high-bounded-churn",
            )
            self.assertIn(
                "Sustained renderer-owned growth suspects: 1",
                outputs["report"].read_text(),
            )

    def test_flat_series_has_zero_slope(self):
        self.assertEqual(MODULE.slope_per_minute([0.0, 1.0], [5.0, 5.0]), 0.0)

    def test_process_allocator_and_vm_growth_are_attributed_separately(self):
        mib = 1024 * 1024
        rows = []
        for index in range(9):
            rows.append(
                {
                    "elapsed_seconds": index * 10,
                    "process_physical_footprint_bytes": (500 + index * 5) * mib,
                    "process_malloc_in_use_bytes": (200 + index * 2) * mib,
                    "process_malloc_reserved_bytes": (300 + index * 3) * mib,
                    "process_vm_malloc_small_resident_bytes": (100 + index) * mib,
                    "process_vm_malloc_small_swapped_bytes": (50 + index) * mib,
                }
            )

        findings = {
            finding.category: finding
            for finding in MODULE.build_process_findings(rows)
        }
        self.assertEqual(
            findings["physical-footprint"].classification,
            "sustained-growth-suspect",
        )
        self.assertEqual(
            findings["vm-malloc-small-resident-plus-swap"].last_mib,
            166.0,
        )
        self.assertEqual(
            findings["malloc-reserved-minus-in-use"].last_mib,
            108.0,
        )

    def test_percentile_reports_profiler_overhead(self):
        self.assertEqual(MODULE.percentile([1.0, 3.0, 2.0], 0.50), 2.0)

    def test_lifecycle_findings_separate_retirement_and_aged_cache_suspects(self):
        mib = 1024 * 1024
        events = [
            {
                "kind": "texture-image",
                "identity": "17",
                "generation": "4",
                "action": "release-requested",
                "reason": "guest-release",
            }
        ]
        retained = [
            {
                "capture_index": "2",
                "kind": "texture-image",
                "identity": "17",
                "generation": "4",
                "retained_bytes": "0",
                "allocation_bytes": str(8 * mib),
                "age_frames": "12",
                "variant_count": "0",
                "pending_release": "1",
            },
            {
                "capture_index": "2",
                "kind": "buffer",
                "identity": "23",
                "generation": "8",
                "retained_bytes": str(2 * mib),
                "allocation_bytes": "0",
                "age_frames": "900",
                "variant_count": "2",
                "pending_release": "0",
            },
        ]
        findings, summary = MODULE.build_lifecycle_findings(events, retained)
        classifications = {item.identity: item.classification for item in findings}
        self.assertEqual(classifications[17], "release-retirement-suspect")
        self.assertEqual(classifications[23], "aged-cache-suspect")
        self.assertEqual(summary["texture-image"]["release-requested"], 1)


if __name__ == "__main__":
    unittest.main()
