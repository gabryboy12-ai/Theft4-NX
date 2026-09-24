# Switch port — 02: compilazione pulita e primo link

Aggiornato: 2026-09-24 · Ramo `switch-port` · Segue [01-first-build.md](01-first-build.md).

## Sintesi

| | Run 3 (01) | **Run 4 (questo)** |
|---|---|---|
| Errori di compilazione SDK | 97 | **0** |
| Oggetti compilati | 331 / 339 | **339 / 339** |
| Warning | 579 | 587 (nessuno bloccante) |
| Link di `switch-smoke.elf` | — | **fallito: 1 simbolo mancante** (`pthread_setname_np`, 2 riferimenti) |
| Link diagnostico `--whole-archive` (tutto il runtime) | — | **6 simboli mancanti** (7 riferimenti), tutti di newlib/libnx |
| `switch-smoke.nro` | — | non prodotto (dipende dal link) |

Il codice dell'SDK ora **compila senza errori**. Per linkare mancano solo 6 funzioni POSIX che newlib/libnx dichiarano negli header ma non implementano. Come richiesto, non sono state implementate.

## A. Symlink di libmspack

I 15 file di `thirdparty/libmspack/cabextract/mspack/` sono **file risolti**: contengono il sorgente vero (per esempio `cab.h` è 5655 byte di codice C), non il percorso del target. Il contenuto è identico, byte per byte, al target di ciascun symlink (`../../libmspack/mspack/*`). Non sono symlink NTFS: `LinkType` è vuoto.

Per questo git, nel sottomodulo, li segna come `T` (cambio di tipo): l'indice li registra come symlink (modo `120000`), sul disco sono file normali. Il repository principale vede il sottomodulo come `modified content`, quindi **l'albero di lavoro non risulta pulito** per questo motivo, e solo per questo.

Ho provato a ricrearli come symlink veri con `git -c core.symlinks=true checkout -- cabextract/mspack`:

- con il git di MSYS2 fallisce con `unable to create symlink …: Operation not permitted`. Dal processo di questa sessione la Developer Mode non vale;
- con il git nativo da PowerShell i file tornano copie risolte.

I file sono stati ripristinati subito e verificati di nuovo come identici ai target. Per la build va bene così: gli 88 errori di mspack del run 3 sono spariti. Per avere un albero pulito servono symlink creati da una shell dove la Developer Mode è effettiva (per esempio Git Bash avviata dopo il riavvio, con `MSYS=winsymlinks:nativestrict`). In alternativa, `git update-index --assume-unchanged` sui 15 file del sottomodulo.

## B. Modifiche

Tutte si applicano solo a Switch: file `*_switch.cpp` nuovi, oppure rami `REX_PLATFORM_NX` / `__SWITCH__` / `switch` nel codice condiviso. Le altre piattaforme compilano lo stesso codice di prima.

| File | Modifica |
|---|---|
| `src/core/mapped_memory_switch.cpp` (nuovo) | `MappedMemory::Open` in sola lettura: legge l'intervallo richiesto in un buffer allineato a 4 KiB con `aligned_alloc`. `kReadWrite` restituisce `nullptr` e un `REXLOG_ERROR` esplicito. `ChunkedMappedMemoryWriter::Open` restituisce `nullptr`, come su POSIX |
| `src/core/seh_switch.cpp` (nuovo) | `seh_thread_state`, `seh_filter`, `seh_rethrow`, `seh_initialize` e `seh_active`, con le firme di `rex/platform/seh.h`. Stato thread-local e nessun handler di segnali |
| `src/core/exception_handler_switch.cpp` | Se nessun handler gestisce il fault e `seh_active()` è vero, scrive `[rex] fault inside SEH_TRY, not yet supported on Switch` più `address=` e `pc=`, poi `svcBreak` come prima |
| `src/core/CMakeLists.txt` | Ramo `switch`: `mapped_memory_posix.cpp` → `mapped_memory_switch.cpp`, `seh_posix.cpp` → `seh_switch.cpp` |
| `include/rex/filesystem/devices/stfs_xbox.h` | `nx_timegm` locale (`days_from_civil`) e ramo `REX_PLATFORM_NX` in `decode_fat_timestamp` |
| `src/system/xsocket.cpp` | Ramo `REX_PLATFORM_NX` senza `<netinet/ip.h>`: il file usa solo `sockaddr_in`, `htons`/`htonl` e `IPPROTO_*` |
| `thirdparty/crypto/sha256.cpp` | Su `__SWITCH__` usa lo stesso ramo di Apple: `__BYTE_ORDER`/`__BIG_ENDIAN` ricavati dalle macro predefinite del compilatore, invece di `<endian.h>` |
| `tools/switch-smoke/` (nuovo) | Progetto CMake solo per Switch: `CMakeLists.txt` e `main.cpp` |

### Nomi SEH

La richiesta citava `initialize_seh`, `initialize_seh_thread` e `seh_active`. `initialize_seh()` e `initialize_seh_thread()` sono funzioni `inline` in `include/rex/platform/exceptions.h` e chiamano `platform::seh_initialize()` e `platform::seh_active()`. Il file di piattaforma deve quindi fornire le 5 funzioni di `rex/platform/seh.h`, le stesse di `seh_posix.cpp`, ed è ciò che fa `seh_switch.cpp`.

### Comportamento di un fault in `SEH_TRY`

1. `SehGuard` imposta `seh_active() = true`, come su POSIX.
2. Il fault arriva a `__libnx_exception_handler`, l'unico punto d'ingresso su NX. `seh_switch.cpp` non installa nulla.
3. Gli handler registrati (MMIO, write-watch) vengono provati per primi: un fault gestito dall'MMIO dentro `SEH_TRY` riprende normalmente.
4. Se nessuno lo gestisce e `seh_active()` è vero: `fault inside SEH_TRY, not yet supported on Switch`, `address=0x…`, `pc=0x…`, poi `svcBreak(BreakReason_Panic)`.

Il messaggio va su `svcOutputDebugString` e su `stderr` tramite `write(2)`. **Non va nel file di log spdlog**: nel gestore delle eccezioni spdlog non è sicuro, perché il thread che ha causato il fault potrebbe tenere il suo mutex. È la stessa regola che il file applicava già ai messaggi esistenti.

## C. Compilazione (run 4)

Build da zero in `out/build/switch-sdk` (la build incrementale falliva con `multiple target patterns` nei `compiler_depend.make`, per i percorsi `C:/`):

```sh
export DEVKITPRO=/opt/devkitpro
cmake -S glue/rexglue-sdk-main -B out/build/switch-sdk -G "Unix Makefiles" \
      -DCMAKE_TOOLCHAIN_FILE=/c/dev/Theft4/toolchains/switch-libnx.cmake \
      -DCMAKE_BUILD_TYPE=Release
make -C out/build/switch-sdk -k -j$(nproc) > out/build/switch-sdk/build.log 2>&1
```

Risultato: **exit 0, 0 errori, 339 oggetti**. Un secondo passaggio sui target bloccati non serve più. Le librerie vengono scritte in `glue/rexglue-sdk-main/out/switch-aarch64/`, ignorato da `.gitignore`.

| Libreria | Dimensione |
|---|---|
| `librexruntime.a` (include rexfilesystem, rexaudio e kernel HLE) | 311 MB |
| `librexcore.a` | 46,7 MB |
| `libspdlog.a` | 17,1 MB |
| `liblibavcodec.a` | 4,6 MB |
| `liblibavutil.a` | 4,0 MB |
| `libfmt.a` | 3,0 MB |
| `libxxhash.a` | 1,4 MB |
| `libmspack.a`, `libo1heap.a`, `libtiny-aes.a`, `libaes128.a` | < 110 KB ciascuna |

Warning: 587 righe, 458 `-Wunused-parameter` (stub HLE), 35 `-Wmissing-field-initializers`, il resto sotto 12 ciascuno.

## D. Primo link: `tools/switch-smoke`

### Il target

Progetto CMake indipendente e solo per Switch: se non è configurato con `toolchains/switch-libnx.cmake` si ferma con `FATAL_ERROR`. Include l'SDK con `add_subdirectory` e la stessa configurazione di `out/build/switch-sdk`, quindi **il CMake dell'SDK non cambia**. Linka, dentro `--start-group`/`--end-group`: `rexruntime`, `rexcore`, `mspack`, `aes128`, `tiny-aes`, `o1heap`, `xxhash`, `libavcodec`, `libavutil`, `fmt` e `spdlog`, più `-lnx -lm` dal toolchain. Genera la NACP con `nx_generate_nacp` e `switch-smoke.nro` con `nx_create_nro`.

```sh
cmake -S tools/switch-smoke -B out/build/switch-smoke -G "Unix Makefiles" \
      -DCMAKE_TOOLCHAIN_FILE=/c/dev/Theft4/toolchains/switch-libnx.cmake \
      -DCMAKE_BUILD_TYPE=Release
make -C out/build/switch-smoke -k -j$(nproc)
```

Il `main.cpp` segue quattro passi e registra ciascuno a schermo e nel log dell'SDK:

1. **Logging**: `rex::InitLogging("sdmc:/switch/theft4/smoke.log", debug)`, dopo aver creato le cartelle.
2. **Address space**: `svcGetInfo` per l'inizio e la dimensione delle regioni alias, heap e ASLR. Aggiunge memoria totale e usata e la core mask del processo, che servono al passo 4.
3. **Memoria guest, come nel runtime**: ripete `rex::system::Memory::Initialize()` (`src/system/xmemory.cpp:136`). Mapping di `round_up(0x120000000 + granularity, granularity)`, cioè `0x120200000` con la granularità Switch di 2 MiB, tramite `CreateFileMappingHandle(…, kReadWrite, false)` di `memory_switch.cpp`. Poi il ciclo `MapViews` sulle basi `1<<32 … 1<<63`, con una copia della tabella `map_info[]` (in `xmemory.cpp` è `static`). Infine scrive e rilegge un `u64` sul primo e sull'ultimo 8 byte del backing store. Le viste alias non vengono toccate: su Switch non hanno memoria fisica dietro.
4. **Thread**: 4 `rex::thread::Thread` sospesi, `set_affinity_mask(1<<i)` per i core presenti nella core mask, un lavoro breve, `Resume` e join con `rex::thread::Wait` (timeout 10 s). Ogni thread registra il core su cui ha girato (`svcGetCurrentProcessorNumber`).

Alla fine scrive `SMOKE OK` oppure `SMOKE FAIL at <passo>: <motivo>` (il primo fallimento) e aspetta il tasto +.

**Esito atteso del passo 3, dal codice e non ancora dall'hardware:** `CreateFileMappingHandle` rifiuta tutte le richieste oltre `kNxMaxMappingBytes = 0x90000000` (2304 MiB). `0x120200000` supera il limite, quindi il percorso del runtime fallisce qui con il messaggio `memory_switch: requested mapping … exceeds Switch userland cap` nel log. Per ottenere comunque un dato dalla console, lo smoke riprova **come diagnostica etichettata (3b), che non è il percorso del runtime**, con esattamente `0x90000000`. Così si vede se quel budget è davvero disponibile e se le estremità del backing sono scrivibili. Il risultato finale resta `SMOKE FAIL at guest memory`, finché `xmemory.cpp` non avrà un ramo NX.

C'è un secondo problema, visibile già nel codice: su NX `MapFileView` restituisce per la vista primaria l'indirizzo del backing (`info.base`), non `mapping_base + 0`. `Memory::Initialize` però usa `mapping_base_` come `virtual_membase_`. Lo smoke lo segnala con una riga `note:` se i due indirizzi differiscono.

### Risultato del link

```
ld: librexcore.a(threading_switch.o): in function `rex::thread::set_current_thread_name(std::string_view)':
    threading_switch.cpp:1351: undefined reference to `pthread_setname_np'
ld: librexcore.a(threading_switch.o): in function `PosixThread::set_name(const std::string&)':
    threading_switch.cpp:645: undefined reference to `pthread_setname_np'
collect2.exe: error: ld returned 1 exit status
```

**1 simbolo, 2 riferimenti.** Non sono stati prodotti né `switch-smoke.elf` né `.nro`. Il map file parziale è in `out/build/switch-smoke/switch-smoke.map`.

### Link diagnostico: tutto il runtime

Il linker estrae da un archivio solo gli oggetti che servono al programma. Lo smoke usa logging, memoria e thread, quindi dal link normale restano fuori quasi tutti gli oggetti di `rexruntime` (kernel HLE, XAM, loader XEX). Per avere l'elenco completo ho rifatto il link a mano con le stesse flag, `-Wl,--whole-archive` sulle librerie proprie dell'SDK (`rexruntime`, `rexcore`, `mspack`, `aes128`, `tiny-aes`, `o1heap`) e `-Wl,--no-gc-sections`. Questo comando non fa parte del target. Log: `out/build/switch-smoke/whole-archive-link.log`.

| Simbolo | Riferimenti | Origine | Libreria | Causa |
|---|---|---|---|---|
| `pthread_setname_np` | 2 | `threading_switch.cpp:645`, `:1351` | `rexcore` | newlib/libnx: dichiarato in `pthread.h:182`, non implementato |
| `pread` | 1 | `filesystem_posix.cpp:170` | `rexcore` | newlib/libnx: mancante |
| `pwrite` | 1 | `filesystem_posix.cpp:176` | `rexcore` | newlib/libnx: mancante |
| `creat` | 1 | `filesystem_posix.cpp:152` | `rexcore` | newlib/libnx: mancante |
| `getuid` | 1 | `filesystem_posix.cpp:80` | `rexcore` | newlib/libnx: dichiarato, non implementato |
| `getpwuid_r` | 1 | `filesystem_posix.cpp:80` | `rexcore` | newlib/libnx: dichiarato in `pwd.h:65`, non implementato |

Raggruppati per causa:

| Causa | Simboli | Riferimenti |
|---|---|---|
| API di piattaforma mancante (backend `*_switch` assente) | 0 | 0 |
| Simbolo di SDL3 escluso | 0 | 0 |
| Simbolo di newlib/libnx mancante | **6** | **7** |
| Altro (definizioni duplicate, ABI) | 0 | 0 |

Nessuna definizione duplicata, nessun simbolo SDL3 e nessun export HLE mancante: la modalità headless isola bene SDL3. **Tutti i simboli mancanti si trovano in due file di `rexcore`.** `rexruntime` non aggiunge dipendenze irrisolte.

Limiti di questa misura:

- `libavcodec`/`libavutil` non sono stati forzati con `--whole-archive`: FFmpeg contiene molti codec che il decoder XMA non usa. Gli oggetti che `rexaudio` richiama davvero sono stati comunque risolti senza errori.
- Il codice ricompilato del gioco (`gta4-recomp`) non fa parte del link: aggiungerà i suoi riferimenti agli export `__imp__*`.

### Approccio consigliato (non implementato)

| Simbolo | Proposta |
|---|---|
| `pthread_setname_np` | Ramo NX in `threading_switch.cpp`: salvare solo il nome in `name_`. Horizon non ha nomi di thread visibili dall'utente |
| `pread`/`pwrite` | Ramo NX in `filesystem_posix.cpp`: `lseek` più `read`/`write` sotto un mutex per ogni file, oppure `fsFileRead`/`fsFileWrite` con offset tramite il devoptab |
| `creat` | `open(path, O_CREAT \| O_WRONLY \| O_TRUNC, mode)`, che è la sua definizione POSIX |
| `getuid`/`getpwuid_r` | Ramo NX: la cartella utente è fissa (`sdmc:/switch/theft4`), senza cercare la home |

## Prossimi passi

1. Risolvere i 6 simboli (tabella qui sopra) per ottenere il primo `switch-smoke.nro`.
2. Avviarlo sull'hardware e allegare `sdmc:/switch/theft4/smoke.log`: valori di `svcGetInfo`, esito della diagnostica 3b a 2304 MiB, core effettivi dei thread.
3. Ramo NX in `xmemory.cpp` per la dimensione del mapping e per `virtual_membase_`, visto che la vista primaria non sta a `mapping_base`.
4. Symlink di mspack (sezione A), per riavere un albero di lavoro pulito.
