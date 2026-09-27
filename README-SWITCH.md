# Theft4 for Nintendo Switch (homebrew port, work in progress)

This branch (`switch-port`) ports the Theft4 runtime to the Nintendo Switch as
**native homebrew**: an NRO built with devkitPro/libnx that runs on Horizon
directly. There is no emulator in between, and the ahead-of-time recompiled
game code is meant to run on the Switch's ARM64 cores like it does on iOS.

**No game files are included in this repository. The game does not run on
Switch yet.** To use this project you need your own legally obtained copy of
Grand Theft Auto IV for Xbox 360. Nothing here downloads, contains or
reconstructs game data, keys or console firmware.

The design notes in [`docs/switch-port/`](docs/switch-port/) are written in
Italian.

## Status

| Area | State |
|---|---|
| ReXGlue SDK runtime (`rexcore`, `rexruntime`) | Compiles and links for Switch with devkitA64 (GCC), see [01](docs/switch-port/01-first-build.md) and [02](docs/switch-port/02-link.md) |
| On-console smoke test (`tools/switch-smoke`) | Runs on real hardware: logging, address-space layout, guest memory, threads, then measurement probes T1–T10 |
| Guest memory | Design A: one address-space reservation, no aliased views, guest→host translation through a 256-entry table, 2 MiB blocks committed on demand. See [03](docs/switch-port/03-memory.md). The permission bug found in console run 4 is fixed and waits for the next run |
| Threads | Core masks honoured, processor count from the process core mask, Xbox 360 CPUs mapped to cores 0–2 with core 3 kept for host workers, clean exit (timer thread stopped). See [04](docs/switch-port/04-threads-exit.md) |
| Exceptions / MMIO | libnx exception handler wired to the runtime's handlers. Unrecognised MMIO in the 0x7F window has a proposed fix that is not implemented yet ([03 §12](docs/switch-port/03-memory.md)) |
| Graphics, audio, input, game boot | Not started on Switch |

## Building

Requirements:

- [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the `switch-dev`
  group (devkitA64, libnx; developed against libnx 4.12.0);
- CMake ≥ 3.25 and Ninja.

On Windows everything runs from the devkitPro MSYS2 shell. Set `DEVKITPRO`
(`/opt/devkitpro`); the toolchain file is
[`toolchains/switch-libnx.cmake`](toolchains/switch-libnx.cmake), which
includes devkitPro's `Switch.cmake` plus
[`toolchains/switch-libnx-rules.cmake`](toolchains/switch-libnx-rules.cmake).

### Smoke test (the build this port verifies)

```sh
export DEVKITPRO=/opt/devkitpro
cmake -S tools/switch-smoke -B out/build/switch-smoke -G Ninja \
      -DCMAKE_MAKE_PROGRAM=/usr/bin/ninja \
      -DCMAKE_TOOLCHAIN_FILE="$PWD/toolchains/switch-libnx.cmake" \
      -DCMAKE_BUILD_TYPE=Release
ninja -C out/build/switch-smoke
# -> out/build/switch-smoke/switch-smoke.nro
```

Copy the NRO to the SD card and start it from the Homebrew Menu. It needs no
game files. Its log is written to `sdmc:/switch/theft4/smoke.log`; press + to
exit.

The trace of every guest-memory SVC is off by default (each line is a durable
write to the SD card). To turn it on, put `nx_memory_trace = true` in
`sdmc:/switch/theft4/smoke.toml`, or pass `--nx_memory_trace` through nxlink.

On Windows, the libmspack submodule stores 15 files as git symlinks. With
`core.symlinks=false` they are checked out as text files holding the link
path and `mspack`/`rexruntime` fail to compile (`lzxd.c:1:1: error`). Fix the
submodule checkout once:

```sh
git -C glue/rexglue-sdk-main/thirdparty/libmspack config core.symlinks true
git -C glue/rexglue-sdk-main/thirdparty/libmspack checkout -- cabextract/mspack
```

### SDK only

[01-first-build.md](docs/switch-port/01-first-build.md) has the commands to
build `glue/rexglue-sdk-main` alone into `out/build/switch-sdk`.

### Full application presets

`CMakePresets.json` has `switch-debug` / `switch-release` presets. They come
from upstream (LibertyRecomp), run on Linux/macOS hosts only, and expect your
own game payload in `tools/local_game_payload/`. That directory is ignored by
git and must never be committed. The port has not been validated through
these presets yet.

## Documents

- [01 – First build](docs/switch-port/01-first-build.md): toolchain, SDK configuration for GCC/Switch, first compile.
- [02 – Link](docs/switch-port/02-link.md): unresolved symbols, Ninja build, first link of `switch-smoke`.
- [03 – Guest memory](docs/switch-port/03-memory.md): Horizon's constraints, the designs considered, design A, console results, MMIO and write watch.
- [04 – Threads and exit](docs/switch-port/04-threads-exit.md): affinity, processor count, CPU→core mapping, the exit crash, module base for crash reports.
- [05 – Upstream import](docs/switch-port/05-upstream-import.md): which upstream paths this repository leaves out and how to import upstream updates without them.
- [06 – Game boot plan](docs/switch-port/06-game-boot-plan.md): which executable the generator expects, building the host `rexglue` tool, regenerating with the NX macros, and the first headless game NRO (plan only).

## Credits

- **[Theft4](https://github.com/KoreanSeats1/Theft4)**: the project this is a fork of (GTA IV AOT recompilation for iOS/iPadOS).
- **[LibertyRecomp](https://github.com/OZORDI/LibertyRecomp)**: the recompilation and runtime work Theft4 is built on.
- **[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)**: the Xbox 360 recompilation SDK and runtime (derived from Xenia) that this port targets.
- **devkitPro / libnx** and **Atmosphère** (whose open kernel source explains Horizon's behaviour in the memory notes).

Grand Theft Auto IV is a trademark of Take-Two Interactive / Rockstar Games.
This project is not affiliated with or endorsed by them, or by Nintendo or
Microsoft.

## License

GPL-3.0, like Theft4: see [`COPYING`](COPYING). Third-party components keep
their own licenses.
