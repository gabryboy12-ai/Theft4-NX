#!/usr/bin/env python3
"""Direct3D census of the European base executable (docs/switch-port/06-game-boot-plan.md section 11, phase (b)).

The game links the Xbox 360 XDK's Direct3D statically. The code generated for the European base
(gta4-recomp/generated/eu-base) holds every function with its PowerPC listing in comments. This script:

  1. takes the functions of the Direct3D library's range (EU_D3D_LO..EU_D3D_HI, an estimate: see below);
  2. maps each to the v8 (USA, title update 8) executable that the native renderer (gta4_native,
     gta4-recomp/src) was written for: the library is the same, moved by V8_MINUS_EU (77 of the 78 v8
     addresses the native renderer and gta4_config.toml name fall on a European function start with
     this one offset; the 78th, 0x82A355E0, is outside the library);
  3. marks the functions that build a PM4 type-3 packet header in code (lis 0xC0nn + ori/addi with a
     known opcode in bits 8-15): they write PM4 themselves. A function can also write PM4 from a
     table or through a callee: the static mark is a lower bound;
  4. writes, for the PC build (tools/pc-eu-base, THEFT4_D3D_CENSUS=ON), a C++ file of wrappers and a
     linker response file (lld-link -wrap:) that count every call that reaches a library function from
     another translation unit (the game's calls; calls inside one generated file are not counted), and
     a dump every 10 s to d3d-census.log: calls per function in the window and the frames (calls of
     the swap function) in it.

Usage:
  python tools/d3d_census.py [--gen <generated/eu-base>] [--out out/d3d-census]
  python tools/d3d_census.py --report out/d3d-census/d3d_functions.tsv d3d-census.log
"""
import argparse
import collections
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN_DEFAULT = os.path.join(ROOT, 'glue', 'rexglue-sdk-main', 'gta4-recomp', 'generated', 'eu-base')
OUT_DEFAULT = os.path.join(ROOT, 'out', 'd3d-census')

# The library's range in the European base. Low end: the render-state dispatcher v8 0x82A38F28
# (gta4_config.toml) is EU 0x829C7A88, and the functions just below it are XAPI/XAM code; high end:
# past the last Vd* caller (EU 0x829E3B70, VdGetCurrentDisplayInformation) and the vertex/index
# header builder (v8 0x82A554A0 = EU 0x829E4000). An estimate: a few functions at both ends may not
# be Direct3D.
EU_D3D_LO = 0x829C7000
EU_D3D_HI = 0x829E4600
V8_MINUS_EU = 0x714A0
# The function that calls VdSwap (v8 0x82A467D8): one call per presented frame.
EU_SWAP = 0x829D5338

# v8 names: FLIRT matches of gta4_config.toml, and roles read from the native renderer's hooks
# (gta4-recomp/src/gta4_native_hooks.cpp, src/graphics/gta4_native).
V8_NAMES = {
    0x82A38F28: 'D3DDevice_SetRenderState (dispatcher, FLIRT)',
    0x82A39928: 'D3DDevice_SetSamplerState (dispatcher, FLIRT)',
    0x82A3AB50: 'render/sampler state dispatcher (FLIRT)',
    0x82A3A890: 'DrawPrimitiveUP-style begin (native hook: vertex declaration + UP draw)',
    0x82A3AA08: 'device helper (native renderer calls it with device, 2)',
    0x82A3B540: 'native hook (device)',
    0x82A3B690: 'SetStreamSource (native hook: SetVertexStreamCommand)',
    0x82A3B7B0: 'SetIndices (native hook: SetIndexBufferCommand)',
    0x82A3B8C8: 'gamma/display LUT store (bulb_appearance_trace)',
    0x82A3B990: 'gamma/display LUT store (bulb_appearance_trace)',
    0x82A3BAA8: 'flush of the dirty state (native hook: StoreAllDirty)',
    0x82A3BE18: 'device helper (native renderer calls it)',
    0x82A3BE98: 'D3DDevice_SetViewportF (FLIRT)',
    0x82A3BF50: 'SetRenderTarget (native hook: SetRenderTargetCommand)',
    0x82A3C2B8: 'SetDepthStencilSurface (native hook: SetDepthStencilCommand)',
    0x82A3C598: 'D3D_IsFloatGpuFormat (FLIRT)',
    0x82A3C690: 'D3D_ConvertD32FS8ToD24FS8 (FLIRT)',
    0x82A3C6F8: 'clear packer (graphics_system: forwarded by 0x82A3CC68)',
    0x82A3CB90: 'clear packer (graphics_system: forwarded by 0x82A3CC68)',
    0x82A3CC68: 'Resolve (native hook: ResolveRectangle/ResolvePoint)',
    0x82A3DAB0: 'DrawIndexedVertices (native hook: indexed draw)',
    0x82A3DF50: 'DrawVerticesUP (native hook: DrawPrimitiveUpCommand)',
    0x82A3DF60: 'DrawVertices (native hook: DrawPrimitiveCommand)',
    0x82A3E348: 'DrawIndexedVertices (native hook: DrawIndexedPrimitiveCommand)',
    0x82A3E910: 'Clear with rectangles (native hook, deferred phase)',
    0x82A3EDA8: 'Resolve batch (native hook: ResolveBatch)',
    0x82A3F268: 'native hook (dirty defaults, device+10916)',
    0x82A412D0: 'D3D_MemAllocAligned (FLIRT)',
    0x82A413F8: 'D3DDevice_Release (FLIRT)',
    0x82A41520: 'native hook (device)',
    0x82A416B8: 'native hook (mode)',
    0x82A42168: 'shader constant upload (native hook, trace only)',
    0x82A42250: 'shader constant upload (graphics_system)',
    0x82A423F8: 'D3DDevice_SetVertexShaderConstantI (FLIRT)',
    0x82A42450: 'D3DDevice_SetPixelShaderConstantI (FLIRT)',
    0x82A424A8: 'SetPixelShader (native hook: SetShaderCommand)',
    0x82A42760: 'SetVertexShader (native hook: SetShaderCommand + vertex declaration)',
    0x82A42930: 'SetVertexDeclaration (native hook)',
    0x82A42AC0: 'native hook (dirty defaults)',
    0x82A42BA8: 'CreatePixelShader (native hook: RegisterNativeShader, pixel)',
    0x82A42CB8: 'CreateVertexShader (native hook: RegisterNativeShader, vertex)',
    0x82A42F10: 'D3D_BlockSizeOfGpuFormat (FLIRT)',
    0x82A43030: 'D3D_AlignTextureDimensions (FLIRT)',
    0x82A44168: 'LockRect of an array/cube texture (native hook: TextureLockCommand)',
    0x82A441F8: 'LockRect (native hook: TextureLockCommand)',
    0x82A44808: 'D3DBaseTexture_GetLevelCount (FLIRT)',
    0x82A44850: 'CreateTexture (native hook: display backing)',
    0x82A44970: 'CreateRenderTarget/Surface (native hook: display backing)',
    0x82A44B78: 'native hook (device)',
    0x82A457B0: 'native hook (device)',
    0x82A467D8: 'Swap: display descriptor copy, calls VdSwap',
    0x82A46DA0: 'native hook (device)',
    0x82A46EA8: 'fence helper (native renderer calls it)',
    0x82A46FB0: 'SetShader (native hook: SetShaderCommand)',
    0x82A47248: 'CCommandBuffer_Destroy (FLIRT)',
    0x82A47E28: 'native hook (device)',
    0x82A49AB8: 'D3DDevice_InsertFence (FLIRT)',
    0x82A4A150: 'D3DResource_GetType (FLIRT)',
    0x82A4A3C8: 'resource lock (native hook: RecordNativeResourceLock)',
    0x82A4A600: 'resource unlock (native hook)',
    0x82A4A710: 'D3DResource_AddRef (FLIRT)',
    0x82A4A788: 'D3DResource_Release (FLIRT)',
    0x82A4A808: 'vertex buffer constructor (native_buffer_metadata)',
    0x82A4A8D0: 'vertex buffer lock wrapper (native_buffer_metadata)',
    0x82A4A920: 'D3DVertexBuffer_Unlock (FLIRT)',
    0x82A4A930: 'index buffer constructor (native_buffer_metadata)',
    0x82A4A9E0: 'index buffer lock (native_buffer_metadata)',
    0x82A4BD20: 'D3D_PatchVertexShaderToMatchPixelShader (FLIRT)',
    0x82A4CE38: 'D3D_SetRawGammaRamp (FLIRT)',
    0x82A4DAB0: 'device init helper (native renderer calls it)',
    0x82A4F0E0: 'present worker (persisted display slot)',
    0x82A4F7E0: 'CPU prefix of the device (native hook: ApplyDeviceCpuPrefix)',
    0x82A50160: 'device init helper (native renderer calls it)',
    0x82A503C8: 'video mode query (calls VdQueryVideoMode)',
    0x82A50700: 'D3D_VideoModeInitialize (FLIRT)',
    0x82A50820: 'device helper (native renderer calls it)',
    0x82A53058: 'device helper (native renderer calls it)',
    0x82A53340: 'D3D_XBMAllocateFrameMemory (FLIRT)',
    0x82A554A0: 'vertex/index header builder (native_buffer_metadata)',
}

PM4_OPCODES = {
    0x48: 'ME_INIT', 0x10: 'NOP', 0x3F: 'INDIRECT_BUFFER', 0x37: 'INDIRECT_BUFFER_PFD',
    0x26: 'WAIT_FOR_IDLE', 0x3C: 'WAIT_REG_MEM', 0x52: 'WAIT_REG_EQ', 0x53: 'WAIT_REG_GTE',
    0x5C: 'WAIT_UNTIL_READ', 0x5D: 'WAIT_IB_PFD_COMPLETE', 0x21: 'REG_RMW', 0x3E: 'REG_TO_MEM',
    0x3D: 'MEM_WRITE', 0x4F: 'MEM_WRITE_CNTR', 0x44: 'COND_EXEC', 0x45: 'COND_WRITE',
    0x46: 'EVENT_WRITE', 0x58: 'EVENT_WRITE_SHD', 0x59: 'EVENT_WRITE_CFL', 0x5A: 'EVENT_WRITE_EXT',
    0x5B: 'EVENT_WRITE_ZPD', 0x22: 'DRAW_INDX', 0x36: 'DRAW_INDX_2', 0x34: 'DRAW_INDX_BIN',
    0x35: 'DRAW_INDX_2_BIN', 0x23: 'VIZ_QUERY', 0x25: 'SET_STATE', 0x2D: 'SET_CONSTANT',
    0x55: 'SET_CONSTANT2', 0x56: 'SET_SHADER_CONSTANTS', 0x2F: 'LOAD_ALU_CONSTANT', 0x27: 'IM_LOAD',
    0x2B: 'IM_LOAD_IMMEDIATE', 0x2E: 'LOAD_CONSTANT_CONTEXT', 0x3B: 'INVALIDATE_STATE',
    0x4A: 'SET_SHADER_BASES', 0x4B: 'SET_BIN_BASE_OFFSET', 0x50: 'SET_BIN_MASK', 0x51: 'SET_BIN_SELECT',
    0x5E: 'CONTEXT_UPDATE', 0x54: 'INTERRUPT', 0x64: 'XE_SWAP', 0x2C: 'IM_STORE',
    0x60: 'SET_BIN_MASK_LO', 0x61: 'SET_BIN_MASK_HI', 0x62: 'SET_BIN_SELECT_LO', 0x63: 'SET_BIN_SELECT_HI',
}

DEF = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)')
LIS = re.compile(r'^lis (r\d+),(-?(?:0x)?[0-9a-fA-F]+)$')
LOW = re.compile(r'^(ori|addi) (r\d+),(r\d+),(-?(?:0x)?[0-9a-fA-F]+)$')


def imm(text):
    return int(text, 0)


def load_functions(gen):
    funcs = {}
    cur = None
    for path in sorted(glob.glob(os.path.join(gen, '*_recomp.*.cpp'))):
        for line in open(path, encoding='utf-8', errors='replace'):
            m = DEF.match(line)
            if m:
                cur = int(m.group(1), 16)
                funcs[cur] = {'insns': [], 'file': os.path.basename(path)}
                continue
            if cur is None:
                continue
            if line.startswith('}'):
                cur = None
                continue
            s = line.strip()
            if s.startswith('// '):
                funcs[cur]['insns'].append(s[3:])
    return funcs


def pm4_headers(insns):
    """Type-3 PM4 headers the code builds: lis rA,0xC0nn then ori/addi rB,rA,opcode<<8."""
    hi = {}
    found = collections.Counter()
    for s in insns:
        m = LIS.match(s)
        if m:
            hi[m.group(1)] = imm(m.group(2)) & 0xFFFF
            continue
        m = LOW.match(s)
        if m and m.group(3) in hi:
            h = hi[m.group(3)]
            lo = imm(m.group(4)) & 0xFFFF
            if 0xC000 <= h <= 0xC3FF and (lo & 0xFF) == 0 and (lo >> 8) in PM4_OPCODES:
                found[PM4_OPCODES[lo >> 8]] += 1
    return found


def build(args):
    funcs = load_functions(args.gen)
    lib = sorted(a for a in funcs if EU_D3D_LO <= a < EU_D3D_HI)
    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, 'd3d_functions.tsv'), 'w', newline='\n') as t:
        t.write('index\teu\tv8\tinstructions\tfile\tpm4_static\tname\n')
        for i, a in enumerate(lib):
            pm4 = pm4_headers(funcs[a]['insns'])
            t.write('%d\t%08X\t%08X\t%d\t%s\t%s\t%s\n' % (
                i, a, a + V8_MINUS_EU, len(funcs[a]['insns']), funcs[a]['file'],
                ','.join('%s:%d' % kv for kv in sorted(pm4.items())), V8_NAMES.get(a + V8_MINUS_EU, '')))
    swap_index = lib.index(EU_SWAP)
    with open(os.path.join(args.out, 'd3d_census_wrap.rsp'), 'w', newline='\n') as r:
        for a in lib:
            # clang++ expands the response file itself: one -Xlinker per lld-link option.
            r.write('-Xlinker -wrap:__imp__sub_%08X\n-Xlinker -wrap:sub_%08X\n' % (a, a))
    with open(os.path.join(args.out, 'd3d_census_hooks.cpp'), 'w', newline='\n') as o:
        o.write('// Generated by tools/d3d_census.py: the Direct3D census of the PC build (diagnostic only).\n')
        o.write('#include <atomic>\n#include <chrono>\n#include <cstdint>\n#include <cstdio>\n#include <thread>\n\n')
        o.write('#include <rex/ppc/context.h>\n\nnamespace {\n')
        o.write('constexpr int kCount = %d;\nconstexpr int kSwap = %d;\n' % (len(lib), swap_index))
        o.write('constexpr uint32_t kAddress[kCount] = {\n')
        for i in range(0, len(lib), 8):
            o.write('    ' + ', '.join('0x%08X' % a for a in lib[i:i + 8]) + ',\n')
        o.write('};\nstd::atomic<uint64_t> g_calls[kCount];\n\n')
        o.write(r'''// Every 10 s: the calls of each function in the window (only the ones called), the frames (calls
// of the swap function) and the elapsed time. One file, appended, in the working directory.
struct Dumper {
  std::thread thread;
  Dumper() {
    thread = std::thread([] {
      uint64_t last[kCount] = {};
      FILE* f = std::fopen("d3d-census.log", "w");
      if (!f) return;
      const auto t0 = std::chrono::steady_clock::now();
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        uint64_t now[kCount];
        for (int i = 0; i < kCount; ++i) now[i] = g_calls[i].load(std::memory_order_relaxed);
        const uint64_t frames = now[kSwap] - last[kSwap];
        int called = 0, ever = 0;
        for (int i = 0; i < kCount; ++i) {
          called += now[i] != last[i];
          ever += now[i] != 0;
        }
        std::fprintf(f, "window %.0f s: frames %llu, functions called %d, ever %d\n", t,
                     (unsigned long long)frames, called, ever);
        for (int i = 0; i < kCount; ++i) {
          if (now[i] != last[i]) {
            std::fprintf(f, "  %08X %llu\n", kAddress[i], (unsigned long long)(now[i] - last[i]));
          }
          last[i] = now[i];
        }
        std::fflush(f);
      }
    });
    thread.detach();
  }
};
Dumper g_dumper;
}  // namespace

''')
        for i, a in enumerate(lib):
            o.write('extern "C" void __real___imp__sub_%08X(PPCContext& ctx, uint8_t* base);\n' % a)
            o.write('extern "C" void __real_sub_%08X(PPCContext& ctx, uint8_t* base);\n' % a)
            o.write('extern "C" void __wrap___imp__sub_%08X(PPCContext& ctx, uint8_t* base) {\n'
                    '  g_calls[%d].fetch_add(1, std::memory_order_relaxed);\n'
                    '  __real___imp__sub_%08X(ctx, base);\n}\n' % (a, i, a))
            o.write('extern "C" void __wrap_sub_%08X(PPCContext& ctx, uint8_t* base) {\n'
                    '  g_calls[%d].fetch_add(1, std::memory_order_relaxed);\n'
                    '  __real_sub_%08X(ctx, base);\n}\n' % (a, i, a))
    n_pm4 = 0
    for a in lib:
        n_pm4 += bool(pm4_headers(funcs[a]['insns']))
    print('%d functions in EU %08X-%08X (v8 %08X-%08X), %d build a PM4 type-3 header in code; files: %s' % (
        len(lib), EU_D3D_LO, EU_D3D_HI, EU_D3D_LO + V8_MINUS_EU, EU_D3D_HI + V8_MINUS_EU, n_pm4, args.out))


def report(args):
    rows = {}
    for line in open(args.report[0]):
        if line.startswith('index'):
            continue
        p = line.rstrip('\n').split('\t')
        rows[int(p[1], 16)] = p
    windows = []
    for line in open(args.report[1]):
        m = re.match(r'window (\d+) s: frames (\d+)', line)
        if m:
            windows.append({'t': int(m.group(1)), 'frames': int(m.group(2)), 'calls': {}})
            continue
        m = re.match(r'\s+([0-9A-F]{8}) (\d+)', line)
        if m and windows:
            windows[-1]['calls'][int(m.group(1), 16)] = int(m.group(2))
    total = collections.Counter()
    for w in windows:
        total.update(w['calls'])
    late = [w for w in windows if w['frames'] > 0][-args.last:]
    frames = sum(w['frames'] for w in late)
    late_calls = collections.Counter()
    for w in late:
        late_calls.update(w['calls'])
    print('windows %d; functions ever called %d of %d' % (len(windows), len(total), len(rows)))
    print('last %d windows with frames: %d frames; functions called %d, of which with a PM4 header in code %d'
          % (len(late), frames, len(late_calls), sum(1 for a in late_calls if rows.get(a, [''] * 6)[5])))
    for a, n in late_calls.most_common():
        r = rows.get(a, ['', '%08X' % a, '', '', '', '', ''])
        print('  EU %08X v8 %s %10.1f per frame  pm4 %-30s %s' % (a, r[2], n / frames if frames else 0, r[5], r[6]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gen', default=GEN_DEFAULT)
    ap.add_argument('--out', default=OUT_DEFAULT)
    ap.add_argument('--report', nargs=2, metavar=('TSV', 'LOG'))
    ap.add_argument('--last', type=int, default=6, help='windows with frames to average (report)')
    args = ap.parse_args()
    if args.report:
        report(args)
    else:
        build(args)


if __name__ == '__main__':
    sys.exit(main())
