# Switch port — 01: prima build dell'SDK con GCC (devkitA64)

Aggiornato: 2026-09-24 · Ramo `switch-port`.

- **Run 1 e 2** (2026-09-23): solo misura, con correzioni limitate alla configurazione.
- **Run 3** (questo aggiornamento): prime correzioni di codice, tutte dentro rami `REX_PLATFORM_NX` o `switch`.

## Sintesi

| | Run 1 | Run 2 | **Run 3** |
|---|---|---|---|
| Errori totali | 86 | 3157 | **97** |
| Errori distinti (file, riga, messaggio) | 86 | 341 | **92** |
| Cause distinte | 1 | 8 | **5** (2 rinviate per scelta, 1 dell'ambiente) |
| Oggetti tentati | 86 | 340 | **339** |
| Oggetti compilati senza errori | 0 | 97 | **331** (97,6%) |
| Oggetti falliti | 86 | 243 | **8** |
| `*_switch.cpp` puliti | 0 / 8 | 4 / 8 | **7 / 7** (keyboard_dialog escluso) |
| `*_posix.cpp` di fallback puliti | 0 / 11 | 7 / 11 | **9 / 11** (seh e mapped_memory rinviati) |
| FFmpeg (libavutil + libavcodec) | 79 errori | 2888 errori | **0** (159/159 oggetti, NEON compreso) |

Dei 97 errori rimasti:

| Quota | Errori | Causa | Stato |
|---|---|---|---|
| 91% | **88** | symlink di libmspack non materializzati | problema dell'ambiente Windows, non della Switch; bloccato perché mancano i privilegi per creare symlink (vedi "mspack") |
| | **5** | `seh_posix.cpp` (4) e `mapped_memory_posix.cpp` (1) | rinviati per scelta, analisi sotto |
| | **4** | `timegm` ×2, `endian.h` ×1, `netinet/ip.h` ×1 | fuori dal perimetro di questo passo |

Tolti i symlink di mspack, **il codice compila tranne 9 errori in 6 file.** "Compila" significa che il file supera il compilatore: il link non è stato provato.

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
| `build-blocked.log` | secondo passaggio sui target bloccati |
| `errors-build.tsv`, `errors-build-blocked.tsv` | errori estratti: file, riga, fatal, messaggio |

I log dei run 1 e 2 non sono nella cartella di build, perché viene ricreata da zero a ogni run. I numeri sono riportati in questo documento.

Comandi:

```sh
export DEVKITPRO=/opt/devkitpro
cmake -S glue/rexglue-sdk-main -B out/build/switch-sdk -G "Unix Makefiles" \
      -DCMAKE_TOOLCHAIN_FILE=/c/dev/Theft4/toolchains/switch-libnx.cmake \
      -DCMAKE_BUILD_TYPE=Release
make -C out/build/switch-sdk -k -j4 > out/build/switch-sdk/build.log 2>&1

# make -k non avvia i target che dipendono da un target fallito (rexcore):
# i loro oggetti vengono compilati direttamente.
for d in src/filesystem/CMakeFiles/rexfilesystem.dir src/system/CMakeFiles/rexruntime.dir \
         src/audio/CMakeFiles/rexaudio.dir thirdparty/CMakeFiles/libavcodec.dir; do
  make -k -j4 -f $d/build.make $d/build
done > build-blocked.log 2>&1
```

Nel run 3 `libavcodec` viene costruito già dalla build principale, perché ora `libavutil` compila. Il secondo passaggio lo trova aggiornato. I messaggi "No rule to make target … `librexruntime.a`" vengono dall'invocazione diretta (manca la fase di archivio) e sono esclusi dai conteggi.

## Modifiche

### Configurazione (run 1–2, commit `d650112e`)

| File | Modifica |
|---|---|
| `glue/rexglue-sdk-main/CMakeLists.txt` | Controllo "Clang ≥ 18" saltato su `switch`. Modalità headless forzata su `switch`. Ramo piattaforma `switch` (`REX_PLATFORM_NX=1`). Flag FP GCC al posto di `-ffp-model=strict`. `CMAKE_CXX_EXTENSIONS ON` su `switch` |
| `glue/rexglue-sdk-main/src/core/CMakeLists.txt` | Ramo `switch`: `*_switch.cpp` più fallback `*_posix.cpp` |
| `glue/rexglue-sdk-main/include/rex/platform.h` | `__SWITCH__` → `REX_PLATFORM_NX`; NX fa parte di POSIX, non di Linux |
| `glue/rexglue-sdk-main/thirdparty/CMakeLists.txt` | spdlog `CXX_EXTENSIONS ON`; niente dispatch x86 per xxHash (su `switch`) |
| `toolchains/switch-libnx.cmake`, `toolchains/switch-libnx-rules.cmake` | Toolchain ufficiale devkitPro più i flag di Theft4 |
| `.gitignore` | Ignora tutta la cartella `tools/local_game_payload/` |

### Codice (run 3)

| File | Modifica |
|---|---|
| `include/rex/platform/dynlib.h` | Ramo `REX_PLATFORM_NX` in `lib_names`, tutto a `nullptr` come per iOS: su Switch non c'è caricamento dinamico |
| `include/rex/thread/fiber.h` | Ramo `REX_PLATFORM_NX`, con `<csetjmp>` e gli stessi membri del backend iOS (`jmp_buf context_`, `stack_`, `stack_size_`, `entry_`, `arg_`, `is_thread_fiber_`, `started_`, `Trampoline()`) |
| `src/core/fiber_switch.cpp` | Usa `context_` al posto del vecchio buffer `jmpbuf_`. `std::aligned_alloc` al posto di `posix_memalign`, che newlib dichiara ma libnx non fornisce (link fallito nella prova) |
| `src/core/dynlib_switch.cpp` | `handle_` a `nullptr` (`kInvalidDynamicLibraryHandle` non esiste più). `Load(path, SymbolResolution)` come nell'header attuale |
| `src/core/exception_handler_switch.cpp` | `ThreadExceptionDump` non ha `fpsr`/`fpcr`: i valori vengono letti con `mrs` sul thread che ha generato l'eccezione (dettagli sotto) |
| `src/core/CMakeLists.txt` | `keyboard_dialog_switch.cpp` escluso dalla build Switch (dettagli sotto) |
| `src/kernel/xam/xam_net.cpp` | Ramo `REX_PLATFORM_NX` per gli header socket BSD di libnx (`arpa/inet.h`, `netdb.h`, `netinet/in.h`, `sys/socket.h`), senza `netinet/ip.h` |
| `src/core/socket_posix.cpp` | `case ESHUTDOWN` escluso su NX: newlib lo definisce solo con `__LINUX_ERRNO_EXTENSIONS__` |
| `src/core/filesystem_posix.cpp` | Su NX `fseeko`/`ftello`/`ftruncate` con `off_t` al posto delle API LFS64, più `static_assert(sizeof(off_t) == 8)` |
| `thirdparty/ffmpeg-overlay/horizon/config.h` (nuovo) | Configurazione FFmpeg per Horizon aarch64 (vedi "FFmpeg") |
| `thirdparty/CMakeLists.txt` | Su `switch`, `ffmpeg-overlay/horizon` viene messo davanti nell'include path di `libavutil`/`libavcodec` |

**`exception_handler_switch.cpp`.** `ThreadContext` contiene sì `fpcr`/`fpsr`, ma si ottiene solo con `svcGetThreadContext3` su un thread sospeso: dal thread che ha generato l'eccezione non si può. `__libnx_exception_handler` gira proprio su quel thread, e prima di `FillThreadContext` esegue solo operazioni intere, quindi i registri FP attivi hanno ancora i valori del momento dell'eccezione.

**`keyboard_dialog_switch.cpp`.** L'header `rex/kernel/xam/keyboard_dialog.h` è stato rimosso nel commit `a1d3a84d` ("Replace vendored RexGlue with Graine SDK"). Oggi `XamShowKeyboardUI_entry` (`src/kernel/xam/xam_ui.cpp:402`) non passa da nessuna astrazione di piattaforma. Il file è escluso invece di ricreare l'API; anche i gemelli android, ios e ps4 sono orfani.

## FFmpeg

### Come lo costruisce l'SDK

- `thirdparty/FFmpeg` è un **sottomodulo git**, un fork `wmarti/FFmpeg` al commit `0604b46` ("Add multi-platform config dispatch and premake wiring"). Non viene lanciato nessuno script `configure`: i `config_<os>_<arch>.h` sono **pregenerati** e committati nel fork, e `FFmpeg/config.h` sceglie quello giusto in base alle macro del compilatore (Windows, macOS, Linux, Android; per il resto `#error "no config"`).
- `thirdparty/CMakeLists.txt` compila con elenchi di sorgenti fissi solo `libavutil` e `libavcodec`, ed esclude avformat, swresample e swscale.
- Su aarch64 aggiunge gli `.S` NEON e `HAVE_NEON=1 HAVE_ARMV8=1 HAVE_INLINE_ASM=1`.
- Gli elenchi di codec, parser e bsf vengono da `thirdparty/ffmpeg-overlay/{codec,parser,bsf}_list.c`. Questa cartella appartiene al **repo principale**, non al sottomodulo.
- Per questo `config.h` per Horizon è stato messo in `ffmpeg-overlay/horizon/`, e non dentro il sottomodulo. Un commit nel sottomodulo esisterebbe solo in locale e renderebbe il repo principale non clonabile.

### Cosa usa il runtime

Solo il decoder **`AV_CODEC_ID_XMAFRAMES`** (`src/audio/xma_context.cpp`), che si appoggia a wmapro. Le API usate sono:

- `avcodec_find_decoder`, `avcodec_alloc_context3`, `avcodec_open2`, `avcodec_send_packet`, `avcodec_receive_frame`, `avcodec_free_context`, `avcodec_is_open`;
- `av_packet_alloc/unref/free`, `av_frame_alloc/unref/free`;
- `av_strerror`, `av_log_set_callback`.

Nessun parser, nessun bsf oltre `null`, nessun encoder. Il config Linux aarch64 abilita già solo `CONFIG_WMAPRO_DECODER`, `CONFIG_XMAFRAMES_DECODER` e `CONFIG_NULL_BSF` (`--disable-everything --enable-decoder=xmaframes`), quindi l'elenco dei componenti non è cambiato.

### Configurazione Horizon

`ffmpeg-overlay/horizon/config.h` è `config_linux_aarch64.h` con queste differenze, ciascuna verificata compilando e linkando una prova con devkitA64 e `switch.specs -lnx -lm`:

| Voce | Linux | Horizon | Motivo |
|---|---|---|---|
| `HAVE_ASM_TYPES_H`, `HAVE_LINUX_PERF_EVENT_H`, `HAVE_SYS_UN_H`, `HAVE_TERMIOS_H` | 1 | 0 | header assenti |
| `HAVE_MMAP`, `HAVE_MPROTECT` | 1 | 0 | `sys/mman.h` assente |
| `HAVE_SCHED_GETAFFINITY`, `HAVE_SETRLIMIT` | 1 | 0 | non dichiarate |
| `HAVE_ARC4RANDOM`, `HAVE_GETRUSAGE`, `HAVE_POSIX_MEMALIGN`, `HAVE_SYSCONF` | 1 | 0 | dichiarate ma non linkabili |
| `HAVE_STRUCT_RUSAGE_RU_MAXRSS` | 1 | 0 | segue `getrusage` |
| `HAVE_MALLOC_H` | 0 | 1 | serve a `mem.c` per `memalign`, che è linkabile |
| `OS_NAME`, `CC_IDENT`, `SLIBSUF`, `FFMPEG_CONFIGURATION` | linux | horizon | solo informativi |

Le funzioni matematiche (`cbrt`, `round`, `trunc`, `hypot`, `erf` e le altre) esistono tutte in newlib e restano a 1. Proprio queste avevano generato i 1704 conflitti di `libm.h` nel run 2. Effetti pratici: senza `sysconf`/`sched_getaffinity`, `av_cpu_count()` restituisce 1, cosa irrilevante per il solo decoder XMA; l'allocazione allineata usa `memalign`.

**NEON:** i 7 `.S` (libavutil `float_dsp_neon`; libavcodec `fft_neon`, `mdct_neon`, `simple_idct_neon`, `videodsp`, `neon`, `mpegaudiodsp_neon`) si assemblano senza errori con GCC, quindi non è stato necessario disattivarli. Resta da verificare al link che i riferimenti `movrel` funzionino in un eseguibile PIE statico.

**Risultato:** `libavutil` 79/79 e `libavcodec` 80/80 compilano. Nel run 2 gli errori erano 2888.

## mspack: bloccato

I 16 file di `thirdparty/libmspack/cabextract/mspack/` sono **symlink git** (mode `120000`) verso `../../libmspack/mspack/`, per esempio `lzxd.c`, `lzx.h`, `mspack.h`, `system.h`. Su questo checkout `core.symlinks=false`, quindi sul disco sono file di testo con il solo percorso.

Prima di cambiare `core.symlinks` ho verificato che Windows permetta di creare symlink:

- `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock` non esiste, quindi **la Developer Mode non è attiva**;
- `New-Item -ItemType SymbolicLink` fallisce con "Per questa operazione sono necessari privilegi di amministratore".

Come da istruzioni non ho copiato i file a mano e non ho cambiato `core.symlinks`. Le strade possibili sono:

1. attivare la Developer Mode (Impostazioni → Sistema → Per sviluppatori);
2. oppure eseguire la shell come amministratore.

Poi:

```sh
git -C glue/rexglue-sdk-main/thirdparty/libmspack config core.symlinks true
git -C glue/rexglue-sdk-main/thirdparty/libmspack checkout -- cabextract/mspack
```

Il problema non è specifico della Switch: colpisce qualunque build fatta da questo checkout Windows.

## Numeri per target (run 3)

| Target | Oggetti | Puliti | Falliti | Errori | Causa |
|---|---|---|---|---|---|
| `o1heap`, `aes128`, `tiny-aes`, `xxhash` | 1 ciascuno | 1 | 0 | 0 | |
| `fmt` | 2 | 2 | 0 | 0 | |
| `spdlog` | 6 | 6 | 0 | 0 | |
| `libavutil` | 79 | 79 | 0 | 0 | |
| `libavcodec` | 80 | 80 | 0 | 0 | |
| `mspack` | 1 | 0 | 1 | 1 | symlink |
| `rexcore` | 47 | 45 | 2 | 5 | `seh_posix` (4), `mapped_memory_posix` (1), rinviati |
| `rexfilesystem` * | 15 | 14 | 1 | 1 | `timegm` in `stfs_xbox.h` |
| `rexruntime` * | 101 | 97 | 4 | 90 | `lzx.cpp` 86 + `lzx.h` 1 (symlink), `content_manager.cpp` 1 (`timegm`), `xsocket.cpp` 1 (`netinet/ip.h`), `xboxkrnl_crypt.cpp` 1 (`endian.h`) |
| `rexaudio` (XMA) * | 4 | 4 | 0 | 0 | |
| **Totale** | **339** | **331** | **8** | **97** | |

\* Target compilati nel secondo passaggio (`build-blocked.log`). Gli oggetti di `rexcore` scendono da 48 a 47 perché `keyboard_dialog_switch.cpp` è stato escluso.

## Errori per causa: confronto fra i run

| # | Causa | Run 1 | Run 2 | **Run 3** | Note run 3 |
|---|---|---|---|---|---|
| A | Estensioni / flag solo-Clang | 86 | 0 | **0** | |
| B | API POSIX mancanti in newlib/libnx | — | 12 | **6** | LFS64 ed `ESHUTDOWN` corretti. Restano `timegm` ×2 e i segnali di `seh_posix` ×4 (rinviato) |
| C | Header di sistema mancanti | — | 3 | **3** | `sys/mman.h` (rinviato), `netinet/ip.h` (`xsocket.cpp`), `endian.h` (`crypto/sha256.cpp`) |
| D1 | FFmpeg senza config Horizon | — | 2888 | **0** | config Horizon |
| D2 | Symlink di libmspack | — | 88 | **88** | bloccato: servono privilegi per i symlink |
| E | `*_switch.cpp` | — | 28 | **0** | allineati all'API; keyboard_dialog escluso |
| F | Codice comune senza ramo NX | — | 138 | **0** | `dynlib.h` e `xam_net.cpp` corretti |
| | **Totale** | **86** | **3157** | **97** | |

## Errori per origine del codice (run 3)

| Origine | File tentati | File puliti | Errori |
|---|---|---|---|
| `*_switch.cpp` | 7 | **7** | 0 |
| `*_posix.cpp` di fallback | 11 | **9** | 5 (seh 4, mapped_memory 1: entrambi rinviati) |
| Codice comune SDK (29 in rexcore + 120 negli altri target) | 149 | **144** | 89 (`lzx.cpp` 86 per i symlink, `timegm` 2, `netinet/ip.h` 1) |
| Terze parti | 172 | **171** | 3 (mspack 2, `crypto/sha256.cpp` 1 tramite `xboxkrnl_crypt.cpp`) |

Il backend Switch esistente, 7 file e circa 2600 righe, **compila per intero**.

## I 10 file con più errori (run 3)

Tutti i file con errori sono 8.

| # | File | Errori | Causa |
|---|---|---|---|
| 1 | `src/system/lzx.cpp` | 86 | D2 symlink mspack |
| 2 | `src/core/seh_posix.cpp` | 4 | B segnali POSIX (rinviato) |
| 3 | `include/rex/filesystem/devices/stfs_xbox.h` | 2 | B `timegm` (incluso da `stfs_container_device.cpp` e `content_manager.cpp`) |
| 4 | `thirdparty/libmspack/cabextract/mspack/lzxd.c` | 1 | D2 |
| 5 | `thirdparty/libmspack/cabextract/mspack/lzx.h` | 1 | D2 |
| 6 | `thirdparty/crypto/sha256.cpp` | 1 | C `endian.h` |
| 7 | `src/system/xsocket.cpp` | 1 | C `netinet/ip.h` |
| 8 | `src/core/mapped_memory_posix.cpp` | 1 | C `sys/mman.h` (rinviato) |

## Errori rimasti fuori perimetro: difficoltà e approccio

| Errore | Difficoltà | Approccio consigliato |
|---|---|---|
| `timegm` (`stfs_xbox.h:46`) | bassa | Ramo NX con un'implementazione `days_from_civil` di poche righe. Non conviene `mktime` più TZ, perché newlib non ha `tm_gmtoff` |
| `netinet/ip.h` (`xsocket.cpp:30`) | bassa | Stesso ramo NX usato per `xam_net.cpp`, senza `netinet/ip.h` |
| `endian.h` (`thirdparty/crypto/sha256.cpp:18`) | bassa | Ramo `__SWITCH__` con `<machine/endian.h>`, oppure le macro `__BYTE_ORDER__` già usate nel ramo Apple dello stesso file |
| symlink mspack | bassa | Developer Mode o shell da amministratore, poi `core.symlinks=true` e checkout del sottomodulo |

## mapped_memory: analisi (non implementato)

`rex::memory::MappedMemory` (`include/rex/memory/mapped_memory.h`) offre `Open(path, Mode{kRead,kReadWrite}, offset, length)`, `Slice`, `Remap`, `Flush` e `Close(truncate)`. `mapped_memory_posix.cpp` lo implementa con `open` più `mmap(MAP_SHARED)` sul file e `ftruncate64` alla chiusura. Esiste anche `ChunkedMappedMemoryWriter`.

**Chi lo usa** (esclusa la grafica, che non fa parte della build Switch):

| Chiamante | Modalità | Tipo | File mappato |
|---|---|---|---|
| `DiscImageDevice::Initialize` (`src/filesystem/devices/disc_image_device.cpp:56`) | `kRead` | file, **tutto il file** | immagine disco GDFX/ISO (fino a ~7-8 GB) |
| `DiscImageEntry::OpenMapped` (`disc_image_entry.cpp:40`) | solo `kRead`: rifiuta le altre modalità | slice non proprietario della mappatura del disco | voce dentro l'ISO |
| `HostPathEntry::OpenMapped` (`host_path_entry.cpp:69`) | passa la modalità del chiamante | file, con offset e lunghezza | file sul filesystem host |
| `UserModule` (`src/system/user_module.cpp:68`), tramite `OpenMapped` | `kRead` | file | **XEX del gioco** (default.xex) |
| `vfs_dump.cpp:84`, tramite `OpenMapped` | `kRead` | file | strumento di dump del VFS |
| `StfsContainerDevice` (`stfs_container_device.cpp:864`) | `kRead` | file, 4 byte | lettura del magic STFS/SVOD |
| `trace_reader.cpp` (grafica, esclusa) | `kRead` | file | trace GPU |

Riepilogo:

- **Sempre in sola lettura.** Nessun chiamante usa `kReadWrite`, e `ChunkedMappedMemoryWriter` non ha chiamanti.
- **Sempre su file,** mai memoria anonima. La memoria guest (le viste da 4,5 GB) è un'altra cosa: la gestisce `memory_switch.cpp`, che compila.
- **Implicazione per la Switch.** Horizon non ha `mmap` su file, quindi un `mapped_memory_switch.cpp` dovrà leggere il file in un buffer (`fread` in un'allocazione allineata). Per XEX, magic STFS e file host è semplice. Il problema vero è `DiscImageDevice`, che mappa l'**intera ISO**: su Switch va evitato (usare i file estratti con `HostPathDevice`/RomFS) oppure riscritto per leggere su richiesta.

## seh_posix: analisi (non implementato)

**Cosa fa.** Offre il supporto runtime delle macro `SEH_TRY`/`SEH_CATCH` (`include/rex/platform/exceptions.h`), che il codegen emette nel codice ricompilato per i blocchi `__try/__except` dello Xbox 360 (`src/codegen/function_graph.cpp:677`):

- `seh_initialize()` installa un handler `sigaction(SA_SIGINFO)` per SIGSEGV, SIGBUS, SIGFPE e SIGILL;
- l'handler, se il thread è dentro una regione SEH (`seh_active()`), **lancia un'eccezione C++** `SehException(code, si_addr)` dal contesto del segnale;
- `seh_rethrow()` ri-solleva il segnale.

`SehGuard` usa `seh_active()` per ogni `SEH_TRY`. `runtime.cpp:115` chiama `initialize_seh()` e `xthread.cpp:441` chiama `initialize_seh_thread()`.

**`exception_handler_switch.cpp` copre la stessa funzione? No.**

| | `seh_posix.cpp` | `exception_handler_switch.cpp` |
|---|---|---|
| Scopo | trasformare un fault dentro `SEH_TRY` in una `SehException` C++, catturata da `SEH_CATCH` | gestore di access violation dell'host: gira gli handler installati (MMIO, write-watch), riscrive i registri e riprende l'esecuzione, altrimenti `svcBreak` |
| Meccanismo | segnali POSIX più unwind C++ dal frame del segnale | `__libnx_exception_handler` |
| Simboli esportati | `seh_initialize`, `seh_active`, `seh_thread_state`, `seh_filter`, `seh_rethrow` | `ExceptionHandler::Install/Uninstall` |

**Si può semplicemente escludere? No.** Senza `seh_posix.cpp` mancano al link `seh_initialize()` e `seh_active()`, chiamate da `runtime.cpp`, `xthread.cpp` e ogni `SehGuard`.

Nei sorgenti presenti nel repo non compare nessun `SEH_TRY` (0 occorrenze in `LibertyRecompLib` e `gta4-recomp`). Il codice ricompilato però viene generato dal XEX e potrebbe contenerne; va verificato sul codice generato.

**Approccio consigliato:** un `seh_switch.cpp` minimo al posto di `seh_posix.cpp`, con lo stato thread-local, `seh_initialize()` vuota e `seh_rethrow()` che chiama `svcBreak`. Un fault dentro `SEH_TRY` diventerebbe fatale: è lo stesso comportamento di oggi con `exception_handler_switch.cpp`, e non esiste un modo sicuro per lanciare un'eccezione C++ da `__libnx_exception_handler`. Se in seguito il codice generato mostrasse blocchi `__except` necessari al gioco, bisognerà convertire il fault in un `longjmp` verso il blocco catch dentro l'handler libnx.

## Warning (run 3)

In totale 579 righe, 230 distinte, nessuna bloccante. Sono aumentate perché ora compila tutto `rexruntime`. Le principali:

| Warning | Occorrenze | Note |
|---|---|---|
| `-Wunused-parameter` | 451 | quasi tutti negli stub del kernel HLE |
| `-Wmissing-field-initializers` | 35 | |
| `-Wimplicit-fallthrough` | 25 | |
| `-Wunused-variable`, `-Wunknown-pragmas` | 11 ciascuno | |
| `-Wtype-limits` | 8 | |
| `-Wsign-compare`, `-Wreorder`, `-Wclass-memaccess` | 6 ciascuno | |
| `-fno-char8_t` sui file C | | il flag globale arriva anche ai file C |

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

1. **mspack/symlink:** servono la Developer Mode o una shell da amministratore, poi `core.symlinks=true` e il checkout di `cabextract/mspack` (vedi "mspack"). Colpisce qualunque build fatta da questo checkout Windows.
2. **`mapped_memory_switch.cpp` da scrivere:** lettura in un buffer, in sola lettura. `DiscImageDevice` (mappatura dell'intera ISO) va evitato su Switch.
3. **`seh_switch.cpp` minimo da scrivere al posto di `seh_posix.cpp`,** che non si può escludere senza sostituirlo. Va verificato se il codice ricompilato di GTA IV contiene `SEH_TRY`.
4. **Errori fuori perimetro da correggere:** `timegm` (`stfs_xbox.h`), `netinet/ip.h` (`xsocket.cpp`), `endian.h` (`thirdparty/crypto/sha256.cpp`).
5. **`LibertyRecomp/CMakeLists.txt:4` controlla solo `CMAKE_SYSTEM_NAME STREQUAL "Switch"`** per decidere se scaricare zlib con FetchContent. Con il toolchain ufficiale il nome è `NintendoSwitch`, quindi quel ramo non scatta più su Switch. Gli altri controlli dello stesso file usano anche `LIBERTY_RECOMP_TARGET_PLATFORM STREQUAL "switch"`. Non modificato.
6. **Backend Switch di input e audio** (`src/input/switch/`, `src/audio/switch/`) orfani: da collegare quando si decide su SDL3.
7. **API keyboard dialog rimossa dall'SDK** (commit `a1d3a84d`): `keyboard_dialog_switch.cpp` è escluso, e i gemelli android, ios e ps4 sono orfani.
8. **Link non ancora provato.** In particolare vanno verificati i riferimenti `movrel` degli `.S` NEON di FFmpeg in un eseguibile PIE statico, e i simboli newlib dichiarati ma non forniti. `posix_memalign`, `sysconf`, `getrusage` e `arc4random` sono già stati esclusi dal config FFmpeg, e `fiber_switch.cpp` non usa più `posix_memalign`.
9. **Artefatti nel sorgente:** l'SDK scrive gli archivi `.a` in `glue/rexglue-sdk-main/out/switch-aarch64/` (`CMAKE_ARCHIVE_OUTPUT_DIRECTORY = ${REXGLUE_ROOT}/out/${REX_PLATFORM}`), non nella cartella di build. La cartella è ignorata da git (`glue/rexglue-sdk-main/.gitignore:33`).
10. **Il preset `switch-base` in `CMakePresets.json` usa il generatore Ninja,** che su questa macchina non è installato.
11. **Il messaggio "Architecture: x86_64" di xxHash** resta nel log di configurazione: viene dal sottomodulo, ma non ha più effetto (dispatch disattivato su `switch`, percorso NEON verificato).
