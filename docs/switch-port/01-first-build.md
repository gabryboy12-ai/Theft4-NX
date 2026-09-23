# Switch port — 01: prima build dell'SDK con GCC (devkitA64)

Data: 2026-09-23 · Passo di **misura**. Sono stati corretti solo problemi di configurazione (CMake, toolchain e flag); nessun errore di codice è stato toccato.

## Sintesi

| | Run 1 | Run 2 (questo rapporto) |
|---|---|---|
| Errori totali | 86 | **3157** (1388 build + 1769 target sbloccati) |
| Errori distinti (file, riga, messaggio) | 86 | **341** |
| Cause distinte | 1 (`-ffp-model=strict`) | **8** |
| Oggetti tentati | 86 | **340** |
| Oggetti compilati senza errori | 0 | **97** |
| `*_switch.cpp` compilati puliti | 0 / 8 | **4 / 8** (≈ 81% delle righe) |

In breve:

- **Il backend Switch è in buona parte sano.** `threading_switch.cpp`, `memory_switch.cpp`, `net_switch.cpp` e `logging_switch.cpp` (2195 righe su 2702) compilano senza errori. I 4 file che falliscono hanno 28 errori in tutto, dovuti a disallineamenti con l'API attuale dell'SDK e di libnx.
- **Tutto il codice comune di `rexcore` compila** (29 file su 29). I fallback POSIX compilano in 7 casi su 11.
- **Il 91% degli errori (2888) viene da FFmpeg,** e la causa è una sola: `thirdparty/FFmpeg/config.h` non ha una configurazione per Horizon.
- **In `rexruntime`, 73 dei 77 file che falliscono non hanno errori nel proprio sorgente.** Falliscono per l'`#error` di `include/rex/platform/dynlib.h:82`, che non ha un ramo per NX e viene incluso da `kernel_state.h`, `ppc/function.h` e `hook.h`. In un caso si aggiunge anche `timegm` in `stfs_xbox.h`.

"Compila" qui significa soltanto che il file supera il compilatore: il link non è stato provato.

## Ambiente

| Strumento | Versione / percorso |
|---|---|
| Compilatore | `aarch64-none-elf-g++` (devkitA64) **GCC 16.1.0** |
| CMake | 4.0.2 (MSYS2 di devkitPro), generatore **Unix Makefiles** (Ninja non installato) |
| devkitPro | `DEVKITPRO=/opt/devkitpro` (= `C:\devkitPro`) |
| Toolchain | `toolchains/switch-libnx.cmake` → `include($DEVKITPRO/cmake/Switch.cmake)` |
| Build | `out/build/switch-sdk`, `CMAKE_BUILD_TYPE=Release`, `make -k -j4` |

File in `out/build/switch-sdk/`:

| File | Contenuto |
|---|---|
| `configure.log` | configurazione |
| `build.log` | build ufficiale `make -k -j4` |
| `build-blocked.log` | secondo passaggio sui target bloccati (vedi sotto) |
| `build-run1.log` | build del run 1 (86 errori) |
| `errors-build.tsv`, `errors-build-blocked.tsv` | errori estratti: file, riga, fatal, messaggio |

Comandi:

```sh
export DEVKITPRO=/opt/devkitpro
cmake -S glue/rexglue-sdk-main -B out/build/switch-sdk -G "Unix Makefiles" \
      -DCMAKE_TOOLCHAIN_FILE=/c/dev/Theft4/toolchains/switch-libnx.cmake \
      -DCMAKE_BUILD_TYPE=Release
make -C out/build/switch-sdk -k -j4 > out/build/switch-sdk/build.log 2>&1

# Secondo passaggio: make -k non avvia i target che dipendono da un target
# fallito (rexcore), quindi i loro oggetti vengono compilati direttamente.
for d in src/filesystem/CMakeFiles/rexfilesystem.dir src/system/CMakeFiles/rexruntime.dir \
         src/audio/CMakeFiles/rexaudio.dir thirdparty/CMakeFiles/libavcodec.dir; do
  make -k -j4 -f $d/build.make $d/build
done > build-blocked.log 2>&1
```

Il secondo passaggio chiude con 4 messaggi "No rule to make target … needed by `librexruntime.a`". Sono un effetto dell'invocazione diretta (manca la fase di archivio), non errori di compilazione, e sono esclusi dai conteggi.

## Modifiche di configurazione

Le altre piattaforme non passano da nessuno di questi rami.

| File | Modifica | Run |
|---|---|---|
| `glue/rexglue-sdk-main/CMakeLists.txt` | Controllo "Clang ≥ 18" saltato solo se `LIBERTY_RECOMP_TARGET_PLATFORM` = `switch` | 1 |
| `glue/rexglue-sdk-main/CMakeLists.txt` | Su Switch forza la modalità headless: `CORE_ONLY`, `RUNTIME_ONLY`, `HEADLESS_KERNEL`, `HEADLESS_RENDER_AUDIO`, `HEADLESS_XMA_DECODER`, `HEADLESS_INPUT` | 1 |
| `glue/rexglue-sdk-main/CMakeLists.txt` | Ramo piattaforma `switch`: `REX_PLATFORM=switch-aarch64`, `REX_PLATFORM_NX=1`, `REX_PLATFORM_SWITCH=1` | 1 |
| `glue/rexglue-sdk-main/src/core/CMakeLists.txt` | Ramo `switch`: `*_switch.cpp` dove esistono, `*_posix.cpp` per il resto | 1 |
| `glue/rexglue-sdk-main/include/rex/platform.h` | `__SWITCH__` → `REX_PLATFORM_NX 1`; `REX_PLATFORM_POSIX` comprende NX; `REX_PLATFORM_LINUX` resta 0 | 1 |
| `toolchains/switch-libnx.cmake` + `switch-libnx-rules.cmake` (nuovo) | Mode A usa il `Switch.cmake` ufficiale; i flag di Theft4 vengono aggiunti tramite rules override | 1 |
| `glue/rexglue-sdk-main/CMakeLists.txt` | **GCC:** `-ffp-contract=off -frounding-math -ftrapping-math` al posto di `-ffp-model=strict`. Condizionale sul compilatore; Clang non cambia | **2** |
| `glue/rexglue-sdk-main/CMakeLists.txt` | **Switch:** `CMAKE_CXX_EXTENSIONS ON` → `-std=gnu++23` (altrove resta OFF) | **2** |
| `glue/rexglue-sdk-main/thirdparty/CMakeLists.txt` | **Switch:** `CXX_EXTENSIONS ON` sul target `spdlog`, perché il suo `CMakeLists` lo forza a OFF | **2** |
| `glue/rexglue-sdk-main/thirdparty/CMakeLists.txt` | **Switch:** `unset(DISPATCH)` prima di xxHash (vedi sotto) | **2** |

**Nota su xxHash.** `thirdparty/xxHash/cmake_unofficial/CMakeLists.txt:75` usa `CMAKE_HOST_SYSTEM_INFORMATION(... OS_PLATFORM)`, cioè l'architettura dell'**host**. Per questo stampa `Architecture: x86_64` anche in cross-compile. Il valore serve solo ad attivare il dispatch x86 (`xxh_x86dispatch.c`) quando è definito `DISPATCH`. Su Switch `DISPATCH` ora viene tolto sempre, e xxHash compila il semplice `xxhash.c`, che sceglie il percorso SIMD dalle macro del target. Verifica con gli stessi flag: `XXH_VECTOR = 4 = XXH_NEON`. Il messaggio "Architecture: x86_64" resta nel log perché viene dal sottomodulo, che non è stato modificato.

Flag effettivi verificati in `flags.make`: rexcore e spdlog `-std=gnu++23`; `-ffp-contract=off -frounding-math -ftrapping-math`; `-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -D__SWITCH__ -D_GNU_SOURCE -DSPDLOG_NO_TZ_OFFSET`.

## Numeri per target

| Target | Oggetti | Puliti | Falliti | Errori | Note |
|---|---|---|---|---|---|
| `o1heap`, `aes128`, `tiny-aes`, `xxhash` | 1 ciascuno | 1 | 0 | 0 | |
| `fmt` | 2 | 2 | 0 | 0 | |
| `spdlog` | 6 | 6 | 0 | 0 | |
| `mspack` | 1 | 0 | 1 | 1 | symlink |
| `libavutil` (FFmpeg) | 79 | 2 | 77 | 1347 | config.h |
| `rexcore` | 48 | 40 | 8 | 40 | |
| `rexfilesystem` * | 15 | 14 | 1 | 1 | `timegm` (`stfs_xbox.h`) |
| `rexruntime` (system + kernel) * | 101 | 24 | 77 | 225 | 73 file falliti solo per errori negli header, quasi sempre `dynlib.h` |
| `rexaudio` (XMA) * | 4 | 2 | 2 | 2 | `dynlib.h` |
| `libavcodec` (FFmpeg) * | 80 | 3 | 77 | 1541 | config.h |
| **Totale** | **340** | **97** | **243** | **3157** | |

\* Target compilati nel secondo passaggio (`build-blocked.log`).

## Errori raggruppati per causa

"Grezzi" conta ogni occorrenza. Un errore in un header incluso da 70 file conta 70 volte.

| # | Causa | Grezzi | Distinti | Difficoltà |
|---|---|---|---|---|
| A | Estensioni / flag solo-Clang | **0** (run 1: 86) | 0 | risolto |
| B | API POSIX mancanti in newlib/libnx | 12 | 11 | media |
| C | Header di sistema mancanti | 3 | 3 | media |
| D1 | Terze parti: FFmpeg senza config per Horizon | 2888 | 158 | media |
| D2 | Terze parti: symlink di libmspack non materializzati | 88 | 84 | bassa |
| E | Errori nei `*_switch.cpp` | 28 | 25 | bassa–media |
| F | Altro: codice comune senza ramo NX | 138 | 60 | bassa |
| | Terze parti SDL3 / glslang / spirv-tools | — | — | escluse (headless) |
| | **Totale** | **3157** | **341** | |

### A. Flag solo-Clang: risolto

I 86 errori del run 1 erano tutti `-ffp-model=strict`. Con i flag equivalenti di GCC sono scomparsi, e non sono emersi altri flag o estensioni solo-Clang.

### B. API POSIX mancanti in newlib/libnx: difficoltà media

| File | Mancante |
|---|---|
| `src/core/filesystem_posix.cpp:90,94,105` | API LFS64: `off64_t`, `fseeko64`, `ftello64`, `ftruncate64`. newlib ha `off_t` a 64 bit e `fseeko`/`ftello`/`ftruncate` |
| `src/core/seh_posix.cpp:64,109,111` | `siginfo_t::si_addr`, `sigaction::sa_sigaction`, `SA_SIGINFO`, `SA_NODEFER`. newlib/Horizon non hanno segnali POSIX reali |
| `src/core/socket_posix.cpp:93` | `ESHUTDOWN` |
| `include/rex/filesystem/devices/stfs_xbox.h:46` | `timegm` |

**Approccio consigliato.** Per LFS64 e `ESHUTDOWN` bastano alias sotto `REX_PLATFORM_NX`. `timegm` va implementata a mano: sono poche righe, oppure `mktime` con TZ=UTC, visto che newlib non ha `tm_gmtoff`. Per `seh_posix.cpp` non ha senso adattare i segnali: su Horizon le eccezioni arrivano tramite `__libnx_exception_handler`, che `exception_handler_switch.cpp` già implementa. La soluzione giusta è un `seh_switch.cpp`, oppure escludere il file su Switch.

### C. Header di sistema mancanti: difficoltà media

| File | Header | Note |
|---|---|---|
| `src/core/mapped_memory_posix.cpp:15` | `sys/mman.h` | errore fatal, quindi il resto del file non è stato analizzato. Horizon non ha `mmap`/`shm_open`: serve un `mapped_memory_switch.cpp` basato su `virtmem`/`svcMapPhysicalMemory`, lo stesso modello di `memory_switch.cpp` |
| `src/system/xsocket.cpp:30` | `netinet/ip.h` | libnx ha `arpa/inet.h`, `netinet/in.h`, `netdb.h` e `sys/socket.h`, ma non `netinet/ip.h`. Probabilmente basta non includerlo su NX |
| `thirdparty/crypto/sha256.cpp:18` | `endian.h` | newlib ha solo `machine/endian.h`; serve un ramo per `__SWITCH__` accanto a quelli esistenti |

### D1. FFmpeg senza config per Horizon: difficoltà media, 1 causa per 2888 errori

`thirdparty/FFmpeg/config.h` sceglie un `config_<os>_<arch>.h` fra Windows, macOS, Linux e Android, e per tutto il resto esegue `#error "no config"`. Questo `#error` compare 1010 volte. Senza config, tutte le macro `HAVE_*`, `ARCH_*` e `CONFIG_*` sono indefinite, e da lì nascono gli altri errori:

- `libavutil/libm.h`, 1704 occorrenze: definizioni `static` di `cbrt`, `round`, `trunc`, `hypot`, `erf` e così via, in conflitto con quelle di `math.h` di newlib, perché `HAVE_CBRT` e simili valgono 0;
- `ARCH_AARCH64`/`ARCH_X86` e `HAVE_VFP` non dichiarati;
- `close`/`read` implicite, perché manca `HAVE_UNISTD_H`;
- effetti a catena in `idctdsp`, `pthread_frame`, `float_dsp`.

**Approccio consigliato.** Aggiungere un `config_horizon_aarch64.h`, partendo da `config_linux_aarch64.h`, e un ramo `defined(__SWITCH__)` in `config.h`. Poi disattivare quello che newlib/libnx non hanno (`HAVE_MMAP`, `HAVE_SYSCTL`, `HAVE_SYSCONF` da verificare) e tenere `HAVE_PTHREADS`, perché libnx fornisce pthread. Serve solo il decoder XMA/WMA, quindi l'elenco dei codec può restare quello di Linux. Mi aspetto che quasi tutti i 2888 errori scompaiano insieme.

### D2. Symlink di libmspack non materializzati: difficoltà bassa, non specifico della Switch

`thirdparty/libmspack/cabextract/mspack/lzxd.c` e `lzx.h` sono **symlink git** (mode `120000`). In questo checkout `core.symlinks=false`, quindi sul disco sono file di testo che contengono solo il percorso (`../../libmspack/mspack/lzxd.c`). Il risultato:

- `lzxd.c:1` → "expected identifier or '(' before '.' token";
- `lzx.h:1`, poi a catena 86 errori in `src/system/lzx.cpp`, che non trova più `mspack_system`, `lzxd_init` e così via.

**Approccio consigliato.** È un problema dell'ambiente di build su un host Windows, non della Switch. La soluzione è abilitare i symlink (`git config core.symlinks true` con la Developer Mode di Windows, poi `git checkout` dei file), oppure puntare le sorgenti CMake a `libmspack/libmspack/mspack/`.

### E. Errori nei `*_switch.cpp`: difficoltà bassa–media

| File | Righe | Esito | Errori | Causa |
|---|---|---|---|---|
| `threading_switch.cpp` | 1362 | **pulito** | 0 | |
| `memory_switch.cpp` | 658 | **pulito** | 0 | |
| `net_switch.cpp` | 93 | **pulito** | 0 | |
| `logging_switch.cpp` | 82 | **pulito** | 0 | |
| `fiber_switch.cpp` | 126 | fallito | 21 | Usa membri di `Fiber` (`jmpbuf_`, `stack_`, `stack_size_`, `entry_`, `arg_`, `started_`, `is_thread_fiber_`, `Trampoline()`) che `include/rex/thread/fiber.h` dichiara solo per iOS (con `context_` al posto di `jmpbuf_`) e per Linux/Mac. Manca il ramo NX nell'header comune |
| `dynlib_switch.cpp` | 53 | fallito | 4 (+1 dall'header) | `kInvalidDynamicLibraryHandle` non esiste più. `Load(path)` non corrisponde più alla firma attuale `Load(path, SymbolResolution mode)`. `dynlib.h:82` produce `#error` perché `lib_names` non ha un ramo NX |
| `exception_handler_switch.cpp` | 233 | fallito | 2 | Legge `fpsr`/`fpcr` da `ThreadExceptionDump`; nella libnx attuale quei campi stanno solo in `ThreadContext` |
| `keyboard_dialog_switch.cpp` | 95 | fallito | 1 fatal | `rex/kernel/xam/keyboard_dialog.h` non esiste nell'SDK: l'API keyboard dialog non c'è più. Essendo un errore fatal, il resto del file non è stato analizzato |

**Lettura.** I due file più grandi e più delicati, threading e memory, compilano senza errori. Gli errori rimasti sono tutti disallineamenti di API: l'SDK è andato avanti dopo che i file Switch erano stati scritti, e libnx ha spostato due campi. Non ci sono problemi di progetto. Il costo stimato è basso per dynlib ed exception_handler e medio per fiber, perché richiede di aggiungere il ramo NX in `fiber.h`. Per keyboard_dialog prima va deciso se l'API deve tornare.

### F. Codice comune senza ramo NX: difficoltà bassa

| Punto | Grezzi | Effetto |
|---|---|---|
| `include/rex/platform/dynlib.h:82` `#error No library names provided for the target platform.` | 79 | La catena `lib_names` (`kVulkanLoader`, `kRenderDoc`, `kSpirvToolsSdkPath`) ha rami per Win, Android, Linux, iOS e Mac, ma non per NX. L'header è incluso da `kernel_state.h`, `ppc/function.h` e `hook.h`, quindi fallisce quasi tutto `rexruntime`. Le 79 occorrenze sono 76 in rexruntime, 2 in rexaudio e 1 in `dynlib_switch.cpp`. **73 dei 77 file falliti di rexruntime non hanno errori nel proprio sorgente.** Gli altri 4 sono `xam_net.cpp`, `lzx.cpp`, `xsocket.cpp` e `xboxkrnl_crypt.cpp`; quest'ultimo include `thirdparty/crypto/sha256.cpp`, che non trova `endian.h`. La soluzione è un ramo NX con `nullptr`, come per iOS |
| `src/kernel/xam/xam_net.cpp` (+ 2 `static_assert` in `include/rex/assert.h`) | 59 | Gli header socket sono inclusi solo con `#elif REX_PLATFORM_LINUX \|\| REX_PLATFORM_DARWIN`. Su NX `in_addr`, `htonl`, `getaddrinfo` e simili non sono dichiarati, e `XNADDR`/`XNDNS` risultano di dimensione sbagliata. libnx fornisce questi header, tranne `netinet/ip.h` |

## Errori per origine del codice

Serve a capire quanto del backend Switch esistente è sano.

| Origine | File tentati | File puliti | Errori grezzi | Distinti |
|---|---|---|---|---|
| `*_switch.cpp` (backend Switch) | 8 | **4** | 28 | 25 |
| `*_posix.cpp` (fallback usati su Switch) | 11 | **7** | 11 | 11 |
| Codice comune SDK (src/, include/) | 29 in rexcore + 120 negli altri target | 29 + 40 | 227 | 144 |
| Terze parti | 172 | 17 | 2891 | 161 |

Nel dettaglio:

- **Fallback POSIX puliti (7):** atomic, clock, dbg, math_gcc, console, env, system.
- **Fallback POSIX falliti (4):** filesystem (LFS64), seh (segnali), socket (`ESHUTDOWN`), mapped_memory (`sys/mman.h`).
- **Codice comune:** in `rexcore` compila tutto, 29 file su 29, compresi xenos e i formati texture. Dei 227 errori comuni, 86 derivano dai symlink di mspack (`lzx.cpp`), 79 da `dynlib.h` e 59 da `xam_net.cpp`. Solo 3 non rientrano in queste tre cause: `timegm` ×2 e `netinet/ip.h`.

## I 10 file con più errori

| # | File | Errori grezzi | Causa |
|---|---|---|---|
| 1 | `thirdparty/FFmpeg/libavutil/libm.h` | 1704 | D1 (header, contato per ogni file che lo include) |
| 2 | `thirdparty/FFmpeg/config.h` | 1010 | D1 `#error "no config"` |
| 3 | `src/system/lzx.cpp` | 86 | D2 symlink mspack |
| 4 | `include/rex/platform/dynlib.h` | 79 | F ramo NX mancante |
| 5 | `src/kernel/xam/xam_net.cpp` | 57 | F include socket solo Linux/Darwin |
| 6 | `thirdparty/FFmpeg/libavcodec/pthread_frame.c` | 27 | D1 |
| 7 | `thirdparty/FFmpeg/libavcodec/idctdsp.c` | 25 | D1 |
| 8 | `src/core/fiber_switch.cpp` | 21 | E |
| 9 | `thirdparty/FFmpeg/libavcodec/idctdsp.h` | 20 | D1 |
| 10 | `thirdparty/FFmpeg/libavcodec/pthread_slice.c` | 12 | D1 |

In tutto ci sono 50 file distinti con almeno un errore. Per il run 1 vedi `build-run1.log`: 86 file, 1 errore ciascuno, tutti `-ffp-model=strict`.

## Warning

In totale 14, nessuno bloccante:

- 6 × `unused parameter 'one_reg'`, più altri parametri e funzioni inutilizzati: `removeCallback` e `signal_handler`, che su NX non vengono chiamati;
- 1 × `operation on 'filetime' may be undefined [-Wsequence-point]`, da controllare;
- 1 × `-fno-char8_t is valid for C++ but not for C`, perché il flag globale arriva anche ai file C.

## Censimento SDL3

Serve a decidere se sostituire SDL3 con backend nativi libnx oppure portarlo. È uno scan statico di `glue/rexglue-sdk-main/{src,include,gta4-recomp}`, esclusa `thirdparty/`.

### Target che usano SDL3

| Target | Collegamento | Condizione |
|---|---|---|
| `rexui` | `SDL3::SDL3` PUBLIC | sempre (fuori da CORE_ONLY) |
| `rexinput` | `SDL3::SDL3` PUBLIC | sempre |
| `rexaudio` | `SDL3::SDL3` PUBLIC | `NOT REXGLUE_CORE_ONLY` |
| `rexruntime` | `SDL3::SDL3` PUBLIC | `NOT REXGLUE_RUNTIME_ONLY`. Collega anche rexui/rexaudio/rexinput se `NOT HEADLESS_KERNEL` |
| `gta4-recomp` | `SDL3::SDL3` | `REXGLUE_BUILD_GTA4_RECOMP` |
| Indiretti via rexruntime | rexcodegen, rexglue, rexgpu-xenos, rexgpu-gta4-native | |

### File che usano SDL3

| File | Token `SDL_*` | Chiamate |
|---|---|---|
| `src/ui/window_sdl.cpp` | 155 | 69 |
| `src/input/sdl/sdl_input_driver.cpp` | 155 | 89 |
| `src/audio/sdl/sdl_audio_driver.cpp` | 75 | 58 |
| `src/ui/sdl_virtual_key.cpp` | 64 | 0 (mappatura tasti) |
| `src/ui/windowed_app_context_sdl.cpp` | 55 | 18 |
| `gta4-recomp/src/network/gta4_voice_audio.cpp` | 46 | 25 |
| `gta4-recomp/src/rpf_button_prompts.cpp` | 17 | 6 |
| `include/rex/ui/window_sdl.h` | 10 | — |
| `include/rex/input/sdl/sdl_input_driver.h` | 8 | — |
| `gta4-recomp/src/install/gta4_install_dialog.cpp` | 8 | 3 |
| `src/ui/sdl_mouse_motion_policy.h` | 7 | — |
| `include/rex/ui/windowed_app_context_sdl.h` | 6 | — |
| `include/rex/ui/surface_mac.h`, `src/ui/surface_mac.cpp` | 3 + 2 | 1 |
| `src/input/absolute_pointer.cpp` | 2 | 1 |
| `include/rex/ui/sdl_virtual_key.h`, `include/rex/audio/sdl/sdl_audio_driver.h` | 2 + 2 | — |
| `src/ui/windowed_app_context_android.cpp`, `include/rex/input/motion_sample_cache.h` | 1 + 1 | — |

Includono gli header wrapper SDL anche `src/audio/sdl/sdl_audio_system.cpp`, `src/audio/coreaudio/coreaudio_audio_system.cpp`, `src/input/input_system.cpp`, `src/ui/rex_app.cpp` e `src/ui/windowed_app_main_sdl.cpp`.

### Chiamate SDL_* distinte per sottosistema

In totale ci sono **101 funzioni distinte** e **266 punti di chiamata**. Di queste, **93 sono nell'SDK** e le altre solo in `gta4-recomp`.

| Sottosistema | Distinte | Chiamate | Solo SDK | In gta4-recomp | Funzioni |
|---|---|---|---|---|---|
| Input | 34 | 66 | 32 | 4 | Gamepad (Open/Close/Get*/Rumble/Sensor/PlayerIndex/Mappings), Joystick type/state, `GetKeyboards`, cursore (`Show/HideCursor`, `CaptureMouse`, `SetWindowRelativeMouseMode`), `StartTextInput` |
| Finestra / video | 21 | 32 | 21 | 0 | `CreateWindow`/`DestroyWindow`, `Get/SetWindow*` (Properties, SizeInPixels, PixelDensity, SafeArea, Fullscreen, Title…), `GetDisplays`/`GetPrimaryDisplay`/`GetDisplayContentScale`, `Metal_*` (solo macOS/iOS) |
| Audio | 15 | 39 | 11 | 8 | `OpenAudioDeviceStream`, `Put/GetAudioStreamData`, Lock/Unlock/Clear/Destroy stream, Pause/Resume device, `GetAudioDeviceFormat/Name`, `GetAudio{Playback,Recording}Devices` (voce in gta4-recomp) |
| Eventi | 7 | 11 | 7 | 2 | `Add/RemoveEventWatch`, `PumpEvents`, `WaitEvent`, `PushEvent`, `RegisterEvents`, `SetEventEnabled` |
| Thread / timer | 6 | 25 | 6 | 0 | `GetTicks`/`GetTicksNS`, `Add/RemoveTimer`, `RunOnMainThread`, `IsMainThread` |
| File / dialoghi | 2 | 2 | 0 | 2 | `ShowOpenFileDialog`, `ShowOpenFolderDialog` (installer gta4-recomp) |
| Altro | 16 | 91 | 16 | 6 | `Init/Quit/WasInitSubSystem`, `SetHint*`, `Get*Property`, `SetAppMetadataProperty`, `GetError` (46 chiamate) / `ClearError`, `free`, `stack_alloc/free`, `GetAndroidJNIEnv` |

Esclusi dal conteggio: `SDL_VERSION_ATLEAST` e `SDL_WINDOWPOS_CENTERED_DISPLAY`, che sono macro, e `SDL_Gamepad_initialized_`, che è un simbolo del progetto.

**Lettura:** l'uso di SDL3 è concentrato in 3 driver, cioè finestra, input e audio. Sono circa 216 delle 266 chiamate, con interfacce interne già separate (`InputDriver`, `AudioDriver`, `Window`/`WindowedAppContext`). Esistono già backend Switch nativi mai compilati: `src/input/switch/switch_input_driver.cpp` e `src/audio/switch/switch_audio_{driver,system}.cpp`, entrambi sotto `REX_PLATFORM_NX` e non collegati in nessun CMakeLists. Da questi dati la sostituzione con backend nativi libnx sembra più praticabile del port di SDL3, che upstream non ha un backend Horizon.

## Target esclusi su Switch

Per effetto della modalità headless (`CORE_ONLY` + `RUNTIME_ONLY` + `HEADLESS_KERNEL`):

| Escluso | Motivo |
|---|---|
| `thirdparty/sdl3` | mai aggiunto: sta dentro il blocco `if(NOT REXGLUE_CORE_ONLY)` di `thirdparty/CMakeLists.txt` |
| `rexui` | SDL3 + Vulkan/imgui |
| `rexinput` | SDL3 |
| backend SDL di `rexaudio` | con CORE_ONLY si compila solo la parte XMA (handoff_trace, xma_context, xma_decoder, xma_register_file) |
| `rexgpu-xenos`, `rexgpu-gta4-native` (grafica) | `src/graphics` non aggiunto con CORE_ONLY; `REXGLUE_USE_VULKAN` forzato OFF |
| `rexcodegen`, `rexglue` | strumenti host, non aggiunti con CORE_ONLY |
| `gta4-recomp` | `REXGLUE_BUILD_GTA4_RECOMP` OFF |
| Dipendenze Vulkan: volk, VMA, glslang, SPIRV-Tools/Headers | blocco `NOT CORE_ONLY` |
| snappy, imgui, disasm, renderdoc, Tracy, perf counters, FidelityFX | blocco `NOT CORE_ONLY` / disattivati da CORE_ONLY |

Target compilati (13): `rexcore`, `rexfilesystem`, `rexruntime`, `rexaudio` (XMA), `libavutil`, `libavcodec`, `fmt`, `spdlog`, `xxhash`, `aes128`, `tiny-aes`, `mspack`, `o1heap`.

## Punti aperti

1. **`LibertyRecomp/CMakeLists.txt:4` controlla solo `CMAKE_SYSTEM_NAME STREQUAL "Switch"`** per decidere se scaricare zlib con FetchContent. Con il toolchain ufficiale il nome è `NintendoSwitch`, quindi quel ramo non scatta più su Switch. Gli altri controlli dello stesso file usano anche `LIBERTY_RECOMP_TARGET_PLATFORM STREQUAL "switch"` e non sono toccati. Non modificato.
2. **Backend Switch di input e audio** (`src/input/switch/`, `src/audio/switch/`) orfani: da collegare quando si decide su SDL3.
3. **`mapped_memory_posix.cpp` richiede `sys/mman.h`,** che non esiste su Horizon. Serve un backend `mapped_memory_switch.cpp`. Non corretto: fa parte del conteggio.
4. **`seh_posix.cpp` presuppone i segnali POSIX:** su NX va sostituito, non adattato (vedi B).
5. **L'API keyboard dialog non esiste più nell'SDK:** `keyboard_dialog_switch.cpp` (e i gemelli android/ios/ps4) sono residui di una versione precedente.
6. **I symlink git non sono materializzati** in questo checkout Windows (`core.symlinks=false`). Colpisce libmspack e forse altri sottomoduli, per qualunque piattaforma costruita da questo checkout.
7. **Artefatti nel sorgente:** l'SDK scrive gli archivi `.a` in `glue/rexglue-sdk-main/out/switch-aarch64/` (`CMAKE_ARCHIVE_OUTPUT_DIRECTORY = ${REXGLUE_ROOT}/out/${REX_PLATFORM}`), non nella cartella di build. La cartella è ignorata da git (`glue/rexglue-sdk-main/.gitignore:33`).
8. **Il preset `switch-base` in `CMakePresets.json` usa il generatore Ninja,** che su questa macchina non è installato.
9. **`-fno-char8_t` arriva anche ai file C:** è innocuo, ma produce un warning.
