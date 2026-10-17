# Local performance bring-up

This patch is based on Theft4-NX commit
`0b30c3ffc76158bd92eea56591c79a396f6208f3` (GPL-3.0; existing credits retained).
The game root is now `sdmc:/LibertyRecomp/game` so one extracted copy serves
both the diagnostic and the game engine.

## Measurements

Add `nx_perf_csv = true` to the engine's existing
`sdmc:/switch/theft4/game.toml`. A separate `perf-NNNN.csv` is created for each
run in that directory. Existing results are never overwritten. CSV recording
is optional and happens once per Vulkan heartbeat on the UI thread; there are
no new operations per GPU draw. Filesystem buffers are flushed without an
`fsync` each heartbeat. A filesystem error disables recording for that run.

The rows retain exact interval lengths and integer frame counts. The analysis
tool sums these instead of averaging rounded FPS values. Zero-frame intervals
are included. Select a range after the loading screens and keep the same scene,
clock settings and graphics options when comparing runs:

```sh
python3 tools/switch-game/performance.py perf-0001.csv \
  --compare perf-0002.csv --start 400 --end 600
```

The range is measured from the first performance window, not console startup.
Only whole windows inside the range are included; choose actual times from the
recorded files. Submit/present errors are explicitly reported. Fence wait time
is host time waiting for completion, not a GPU timestamp or utilization counter.
A high wait fraction identifies a synchronization/execution candidate; it does
not prove which shader or driver operation is responsible.

The upstream run-21 notes show 4,300–6,100 draws per frame and 8–10 seconds of
command-thread fence waits in a ten-second window. These are upstream results,
not measurements on this user's Switch. The first renderer investigations are
expensive fragment shaders, render-target transfers, and native depth/blending
versus shader-interlock fallback. Each candidate needs a working baseline,
matching captures and a stability check before accepting an FPS change.

## Host checks

```sh
clang++ -std=c++20 -Wall -Wextra -Werror \
  tools/switch-game/tests/perf_csv_test.cpp -o perf-csv-test
./perf-csv-test
python3 tools/switch-game/tests/test_performance.py
```

The writer and analysis tests pass locally. The matching fork generator was
built locally and completed generation for the supplied EU/Asia executable
(media 4A53F9F6, base version 0.0.0.6). Its register audit reported only the
upstream documented xor-zeroing case. The fork's direct-call helper was applied;
118,618 calls were converted and configured hooks retained weak dispatch.
This is a build optimization candidate, not a measured FPS improvement.

A local devkitA64 GCC 15.2 C++ toolchain now builds the Switch memory backends.
The Vulkan launcher (including CSV recording) compiles, and a separate C++/NVK
link check passes. The headless EU/Asia baseline NRO has built. The integrated native audio and
Vulkan NRO is compiling; no playable hardware result or FPS improvement is
claimed yet.

The downloaded NVK 25.0.7 archive bundles its dependencies. For this package,
configure with `-DTHEFT4_SWITCH_VULKAN_LIBRARIES="vulkan;nx"`. The default remains
`vulkan;expat;zstd;z;nx` for the upstream Mesa packages. This older driver still
needs device feature and renderer compatibility checks; upstream's newer driver
results cannot be assumed to apply. The archive exports only `vkGetInstanceProcAddr`
(plus the ICD entry points), while `vulkan_instance.cpp` links `vkDestroyInstance`
directly; `tools/switch-smoke/nx_elf_stubs.c` supplies a weak `vkDestroyInstance`
that forwards through `vkGetInstanceProcAddr`. A driver that exports the symbol
replaces it.

Before guest startup, default.xex is SHA256-checked against the generated image.
The supplied EU/Asia base image is the default. For another generated image, set
`THEFT4_EXPECTED_XEX_SHA256` to its hash as well as `THEFT4_GENERATED_DIR`; malformed
hashes are rejected during configuration. This guard does not provide translated
functions for other revisions automatically.

The startup registration module uses `-O1` to keep its giant virtual-call block
practical to compile locally. Gameplay modules keep `-O3`; this setting is not
an in-game frame-rate optimization.

## Native audio and rendering defaults

The launcher selects Vulkan and native Switch audren audio by default.
The core-only build now compiles AudioSystem and the Switch driver.
Eight PCM buffers provide about 43 ms of buffering; initial producer credits
match that depth. Audio submissions use fixed stack storage instead of heap
queues, preserve guest samples while waiting, and return credits after bounded
renderer failures. The native backend currently supports one active render
client; additional clients are explicitly rejected because libnx owns one
process-wide renderer session. The audio system shuts down its worker and
drivers on destruction.

Automatic screenshot GPU readbacks are disabled unless nx_vk_capture_frames
or nx_vk_dump_frames is enabled. Previously, disabling PPM output still ran
GPU readback and pixel scans at 20, 40, 60, 120 and 240 seconds. Export tracing
is now off by default; enable nx_trace_exports for crash diagnosis. These
changes avoid work without skipping guest draws. CSV frames count successful
window presents, which can include repeated images; they are not a measurement
of unique guest frames or simulation speed. No FPS improvement or audible
hardware playback is claimed before console testing.

Native audio cross-compiler checks pass. A host ARM64 test verifies big-endian
6-channel downmix channel routing and full-scale normalization over 256 samples.

Native audio fixes also reserve two libnx channel slots for the stereo voice,
and flush PCM memory before the audio renderer reads it. The previous
one-slot configuration could not initialize a two-channel voice. The flush
follows the official libnx audren sample:
https://github.com/switchbrew/switch-examples/blob/master/audio/audren-simple/source/main.c
Repeated service failures are paced and logged periodically to avoid a busy loop.
