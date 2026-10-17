# Switch performance work (builds C1–C6)

This directory holds the measurement tooling, configs and A/B results of the
performance rounds C1–C6 (October 2026). The source changes themselves are in
`glue/rexglue-sdk-main` and `tools/switch-game` on this branch.

The console used for every run: Switch Lite (Mariko), handheld, CPU 1785 MHz,
GPU 768 MHz, memory 1600 MHz (raised by `nx_cpu_mhz`/`nx_gpu_mhz`/`nx_emc_mhz`),
Mesa 26.2.4 NVK (`mesa-switch 606441c`).

## Where things stand

In gameplay the game runs at **~2.4 fps**. Two limits:

* **CPU (guest threads + PM4 parsing), with all GPU work skipped:** ~15.9 swaps/s.
* **GPU, per frame (~250 ms on the FBO render-target path):**

  | operation (GPU timestamps, `vulkan_operation_timestamps`) | ms/frame |
  |---|---|
  | EDRAM ownership transfers: depth | 53 |
  | EDRAM ownership transfers: stencil (shader into scratch) | 45 |
  | EDRAM ownership transfers: stencil scratch → stencil copy | 15 |
  | EDRAM ownership transfers: color | 15 |
  | guest draws | 50 |
  | other buffer/image copies (`transfer`) | 30 |
  | direct resolves | 16 |
  | EDRAM dumps | 14 |
  | texture loads | 14 |
  | EDRAM clears + resolve copies | 10 |

The game's own draws alone (~50 ms) exceed a 30 fps frame (33 ms), so 30 fps
needs more than incremental fixes to the EDRAM emulation (see "Next steps").

## Rounds

Each round is an overlay build with an in-game A/B: once gameplay is detected
(`nx_ab_trigger_draws` draws per frame for two 10 s windows), phases of
`nx_ab_phase_windows` windows cycle a mode `nx_ab_skip_cycles` times. One CSV
row per 10 s window (`ab-NNNN.csv` next to `game.log`), copies in `results/`.

| round | change | result |
|---|---|---|
| C1 | `SkipGpuWork` A/B: draws/resolves parsed but not executed | GPU on ~2.4 swaps/s, GPU off ~11 swaps/s: the CPU ceiling was ~11 |
| C2 | lock-free kernel object lookup (`object_table.cpp`, `xobject.cpp`: deferred delete, `TryRetain`), wake-one condition variables (`nx_multi_wait.h`), 10-frame profiler stacks | +2.5 % (11.07 → 11.35). Profiles: job threads ~45 % in `EnterGlobalLock`/`CheckGlobalLock` |
| C3 | guest `mfmsr`/`mtmsrd` "interrupt lock" around lwarx/stwcx (50 sites) per thread instead of the process-wide global mutex (`ppc_generated.cpp`, `nx_sync_irq_lock_free`) | CPU ceiling 10.91 → 15.72 swaps/s (+44 %) |
| C4 | `nx_ownership_transfer_skip` A/B (skip depth↔color or all ownership transfers) | 239 → 178 → 110 ms/frame, but the lower G-buffer strips break: the transfers carry real data. Not usable as is |
| C5 | semaphore release / event signal without the object mutex when nobody sleeps (`threading_switch.cpp`, `nx_sync_lock_free_release`); FSI render-target path | lock-free release +5 % (15.07 → 15.9). **FSI is slower on NVK/Maxwell:** guest draws ~430 ms/frame, ~1.4 fps. Stay on FBO |
| C6 | back on FBO; `nx_ab_stencil_modes` cycles the stencil part of depth ownership transfers: 0 scratch + copy (default), 1 eight bit passes, 2 not copied; `ownership_stencil_copy` timestamp kind | mode 0 ~2.35 fps, mode 1 ~1.25 fps, mode 2 ~3.1 fps. Visual effect of mode 2 not yet confirmed |

NVK has no `VK_EXT_shader_stencil_export` (Maxwell cannot export stencil from a
shader), so stencil ownership transfers use either eight per-bit passes or a
color scratch image copied into the stencil aspect (`VK_KHR_maintenance8`).

## GTA IV frame structure (one decoded gameplay frame: `analysis/frame9242.txt`)

~4,200–5,400 draws per frame. The G-buffer (1280x720, 2x MSAA, three RGBA8 MRTs
plus D24FS8) is drawn in three horizontal strips (EDRAM tiling, ~698 draws
each). Before each strip, 4x MSAA pitch-640 aliases of the same EDRAM ranges
clear it (one 4-MRT draw, then depth-only draws with D24S8 depth on the C1/C2
ranges), so each strip causes color, depth→color and depth ownership transfers
in both directions. `analysis/frame_structure.py` decodes the
`native_renderer_oracle_capture` output into passes.

## Next steps (suggested)

1. Check whether GTA IV reads the transferred stencil (C6 mode 2 visuals). If
   not, defer the stencil part of a depth transfer and only perform it before
   a draw that enables the stencil, a resolve of that depth, or a transfer
   that uses it as the source (~45–60 ms/frame).
2. Remove the per-strip round trips: the 4x pitch-640 clear draws are fully
   overwritten, so transfers *into* them are dead; transfers *out* of them
   carry the clear values.
3. Depth ownership transfers (53 ms) and the 30 ms of other copies.
4. 30 fps needs a renderer that draws GTA IV's frame directly (no EDRAM
   emulation) and faster guest draws; the `native_renderer_oracle_*` capture
   is the starting point for that.

## Hand edits in generated code (not in this repository)

The generated C++ (`glue/rexglue-sdk-main/gta4-recomp/generated/`) is not
tracked. The builds measured here also carried hand edits from earlier
rounds in five generated files (`gta4_recomp.2/.3/.65/.74/.77.cpp`): a fast
path for the audio packet copy `sub_8216DBB0` (translation cached when the
packet stays in one 16 MiB slice, original kept as fallback), an exact wave
helper (`include/rex/audio/nx_exact_wave_helpers.h`), and leaf functions that
keep registers in host locals. They have to be reapplied after regenerating.

## Tooling

* `build.py <overlay> <out> --build-id ID [-j N]`: incremental build of the
  NRO from an overlay directory (files with the same relative paths as the
  SDK tree, plus `main.cpp` and `switch-game/*`), reusing a baseline ninja
  build and its recorded compile commands. Set `THEFT4_WORK` to the local
  work tree (baseline build, devkitA64, Mesa); the paths inside are specific
  to that tree.
* `sd.py install <build> <game.toml> <tag>` / `sd.py capture <tag>`: install
  on the SD (Hekate USB mass storage) with backup and read-back check, or copy
  the logs. `SWITCH_SD` (default `/Volumes/SWITCH SD`), `THEFT4_PERF_DIR`.
* `sym.py <syms.txt> '+0x…'`: symbolize profiler offsets with
  `aarch64-none-elf-nm -n switch-game.elf > syms.txt`.
* `configs/game-cN.toml`: the exact config of each round (copy to
  `sdmc:/switch/theft4/game.toml`).

## New cvars (main.cpp)

`nx_sync_fast_lookup`, `nx_sync_notify_one`, `nx_sync_irq_lock_free`,
`nx_sync_lock_free_release` (all default true), `nx_ab_sync_modes`,
`nx_ab_transfer_modes`, `nx_ownership_transfer_skip`, `nx_ab_stencil_modes`,
`nx_stencil_transfer_mode`, `nx_frame_structure_capture`, `nx_profile`.
