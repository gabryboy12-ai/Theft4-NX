#!/usr/bin/env python3
"""Isolated source-recovery and fail-closed transformation tests."""

import bz2
import io
import struct
import tarfile
import unittest

import recover_shader_sources as recovery


class RecoveryTests(unittest.TestCase):
    def test_exact_body_preservation_and_descriptor_slots(self):
        body = ("#define A_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 416)\n"
                "#define B_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 516)\n"
                "r0 = tfetch2D(x, float2(1, 2), A_SamplerDescriptorIndex);\n"
                "r1 = tfetch3DLod(y, B_SamplerDescriptorIndex, min(r0.x, 4));\n")
        adapted, insertions = recovery.adapt_fetches(body)
        self.assertEqual([item["slot"] for item in insertions], [0, 25])
        self.assertIn("min(r0.x, 4), GetTextureLodBias(25)", adapted)
        for item in insertions:
            adapted = adapted[:item["offset"]] + adapted[item["offset"] + len(item["text"]):]
        self.assertEqual(adapted, body)

    def test_unknown_misaligned_and_already_adapted_fetch_rejected(self):
        for body in (
            "tfetch2D(x, Missing_SamplerDescriptorIndex)",
            "#define A_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 417)\n",
            "#define A_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 520)\n",
            "tfetch2D(x)",
            "tfetch2D(x, Missing_SamplerDescriptorIndex",
            "#define A_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 416)\n"
            "tfetch2D(A_SamplerDescriptorIndex, GetTextureLodBias(0))",
        ):
            with self.subTest(body=body), self.assertRaises(ValueError):
                recovery.adapt_fetches(body)

    def test_common_nested_guard_and_late_contract(self):
        prefix = "#ifndef SHADER_COMMON_H_INCLUDED\n#ifdef A\nfloat x;\n#endif\n#endif\n"
        body = "\n[[early_fragment_tests]]\n[earlydepthstencil]\nvoid shaderMain() {}\n"
        self.assertEqual(recovery.split_common(prefix + body), (prefix, body))
        pixel = recovery.PIXEL_PREAMBLE + prefix + body
        self.assertEqual(recovery.split_common(pixel), (recovery.PIXEL_PREAMBLE + prefix, body))
        self.assertLess(pixel.index("#define XENOS_RECOMP_PIXEL_SHADER"), pixel.index("float x;"))
        self.assertEqual(recovery.late_source(body), "\n\n\nvoid shaderMain() {}\n")
        with self.assertRaises(ValueError):
            recovery.late_source("void shaderMain() {}")
        with self.assertRaises(ValueError):
            recovery.split_common(prefix[:-7])

    @staticmethod
    def metal_library(source):
        archive = io.BytesIO()
        with tarfile.open(fileobj=archive, mode="w") as output:
            member = tarfile.TarInfo("/untrusted/archive/path/original.metal")
            member.size = len(source)
            output.addfile(member, io.BytesIO(source))
        payload = b"id\0" + bz2.compress(archive.getvalue())
        tag = b"SARC" + struct.pack("<I", len(payload)) + payload
        section = struct.pack("<I", 1) + b"options\0directory\0" + struct.pack("<I", len(tag) + 4) + tag
        header = bytearray(88)
        extension = b"HSRD" + struct.pack("<HQQ", 16, len(header) + 4 + 26, len(section)) + b"ENDT"
        data = header + b"ENDT" + extension + section
        data[:4] = b"MTLB"
        struct.pack_into("<Q", data, 16, len(data))
        struct.pack_into("<QQQ", data, 24, len(header), 0, len(header) + 4 + len(extension))
        return data

    def test_archive_extraction_does_not_extract_member_paths(self):
        source = b"exact original source\n"
        data = self.metal_library(source)
        recovered, member = recovery.embedded_source(data)
        self.assertEqual(recovered, source)
        self.assertEqual(member, "/untrusted/archive/path/original.metal")

    def test_archive_truncated_and_invalid_bounds_rejected(self):
        data = self.metal_library(b"source")
        with self.assertRaises(ValueError):
            recovery.embedded_source(data[:-1])
        struct.pack_into("<Q", data, 98, len(data) + 1)
        with self.assertRaises(ValueError):
            recovery.embedded_source(data)


if __name__ == "__main__":
    unittest.main()
