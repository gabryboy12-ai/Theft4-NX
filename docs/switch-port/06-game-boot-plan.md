# 06 – Piano: rigenerare il codice e primo NRO del gioco

Solo analisi: niente di questo documento è implementato. Le stime di tempo e
memoria sono stime, non misure, ed è indicato dove.

## 0. Punto di partenza: quale eseguibile

Tutto ciò che in `gta4-recomp` dipende dall'eseguibile è fissato su **GTA IV
USA retail + TU8**:

| Cosa | Valore atteso | Dove |
|---|---|---|
| Title ID | `545407F2` | `src/install/gta4_source_inspector.cpp`, `gta4_installer.cpp` |
| Media ID del disco | `6AC07221` (USA) | idem |
| Regione | `0x000000FF` (NTSC-U) | idem |
| Versione del `default.xex` di base | `0.0.0.5` (version e base version) | idem |
| Firma RSA del base (SHA-1) | `192B3F56…251A89` | idem; è anche il `delta_source_digest` della XEXP |
| `default.xexp` (TU8) | delta patch `0.0.0.5 → 0.0.8.5`, SHA-256 `480aee5e…648fd3` | `gta4_installer.cpp` |
| XEX patchato letto dal generatore | `gta4-recomp/assets/default_v8.xex`, versione `0.0.8.5`, SHA-256 `8268fdc9…d1ff21` | `gta4_manifest.toml`, `gta4_installer.cpp` |
| Indirizzi guest scritti a mano | 142 voci in `gta4_config.toml` (`[rexcrt]`, `[functions]`), 401 indirizzi distinti in 30 file di `gta4-recomp/src` (hook) | |

Il disco europeo del proprietario (`default.xex` letto il 2026-09-27) ha:

| Campo | Valore |
|---|---|
| Title ID | `545407F2` |
| Media ID | `4A53F9F6` |
| Versione / versione base | `0.0.0.6` / `0.0.0.6` |
| Regione | `0x00FFFD00` (PAL + NTSC-J tranne Cina) |
| Cifratura / compressione | normale (AES) / basic |
| Immagine | `0x11F0000` byte a `0x82000000` |

**La TU8 prevista non si applica a questo disco**: la XEXP richiede la
versione sorgente `0.0.0.5` e la firma RSA del base USA come digest
sorgente; il disco europeo è `0.0.0.6` con un'altra firma. `ApplyPatch` e
l'installer la rifiutano, e non va aggirato: il risultato non sarebbe
l'immagine su cui sono scritti config e hook.

Strade possibili, da scegliere prima di rigenerare:

- **A. Disco USA `6AC07221` + TU8** (consigliata per il primo NRO): tutto il
  lavoro esistente (config, hook, installer, verifiche) vale così com'è.
- **B. Immagine europea patchata** dalla TU della sua linea (se esiste una
  TU per `4A53F9F6`: non è verificabile da questo repository). Rigenerazione
  sull'immagine europea, e i 142 + 401 indirizzi vanno ritrovati uno per uno
  (confronto delle funzioni tra le due immagini, firme FLIRT come fatto per
  `[rexcrt]`). Lavoro lungo, ma porta al gioco completo sul disco europeo.
- **C. Solo `default.xex` europeo, senza TU**: il generatore accetta
  qualunque XEX (vedi §2), ma serve un config nuovo senza indirizzi della v8
  e nessun hook di `gta4-recomp/src`. Basta per l'obiettivo di §4 (arrivare
  al punto d'ingresso), non per giocare.

## 1. Strumento host: `rexglue`

- **Cos'è.** `glue/rexglue-sdk-main/src/rexglue` (`add_executable(rexglue)`),
  che linka `rexcodegen` → `rexruntime`, fmt, spdlog, toml++, disasm, xxhash,
  simde. Si usa come `rexglue codegen gta4_manifest.toml` dalla cartella
  `gta4-recomp`.
- **Build Windows.** Preset `win-amd64-release` di
  `glue/rexglue-sdk-main/CMakePresets.json`: Ninja Multi-Config,
  `clang`/`clang++`, `-march=x86-64-v3`, C++23. Il `CMakeLists.txt` rifiuta
  compilatori diversi da Clang e Clang < 18 (tranne che per Switch).
  Per costruire solo lo strumento: `cmake --preset win-amd64` poi
  `cmake --build --preset win-amd64-release --target rexglue`. Il configure
  però attraversa tutti i sottoprogetti (grafica D3D12, SDL3, UI), quindi
  servono i submodule e Windows SDK anche se non vengono compilati.
- **Compilatore disponibile su questa macchina.**
  - `D:/Microsoft Visual Studio/VC/Tools/Llvm/x64/bin/clang++.exe`: **clang
    22.1.3**, target `x86_64-pc-windows-msvc`. Soddisfa "Clang 18+" ed è la
    combinazione che il preset Windows si aspetta (headers e librerie MSVC).
    Va usato da un "x64 Native Tools Command Prompt" / `vcvars64.bat`, con
    quella cartella `Llvm/x64/bin` in testa al `PATH`, e CMake/Ninja di
    Visual Studio (non quelli di MSYS, che scrivono percorsi POSIX).
  - `D:/MSYS2/clang64` **esiste ma è vuoto**: pacman c'è, clang no. Si può
    installare (`pacman -S mingw-w64-clang-x86_64-{clang,lld,cmake,ninja}`),
    ma è un target MinGW che il preset non prevede: sconsigliato, a meno che
    la strada MSVC non si blocchi.
  - `-march=x86-64-v3` richiede AVX2: l'i5-7300HQ lo ha.
- **Anche su Linux/WSL** funziona il preset `linux-amd64-release`; non
  serve per questo piano.

### Tempo e memoria (stime)

La macchina ha 4 thread (i5-7300HQ), 16 GB di RAM, 78 GB liberi su C: e 53 su
D:.

| Fase | Stima | Base della stima |
|---|---|---|
| Build di `rexglue` + dipendenze | 30–60 min a `-j4` | numero di sorgenti di `rexcodegen`/`rexruntime`/`rexcore`; non misurato su Windows |
| `rexglue codegen` su un'immagine da 18 MB | minuti, 2–6 GB RAM | non misurato: va misurato alla prima esecuzione (Task Manager, `Measure-Command`) |
| Output | ~171 MB di C++: 87 × ~2 MB `gta4_recomp.N.cpp` + `gta4_init.{h,cpp}` + `sources.cmake` | misurato sull'output di upstream in `/c/dev/Theft4` |
| Compilazione del codice generato per Switch (GCC 16, `-O3`) | 1–3 min e 1–2 GB per file da 2 MB; 1,5–3 h in totale a `-j2`/`-j3` | stima; con 16 GB non andare oltre `-j3` |

### Dove scrive

`out_directory_path = "generated"` nel manifest: l'output va in
`glue/rexglue-sdk-main/gta4-recomp/generated/`, già ignorata da git
(`.gitignore`: `glue/rexglue-sdk-main/gta4-recomp/generated/`). Anche
`gta4-recomp/assets/` (dove sta `default_v8.xex`) è ignorata. Prima di ogni
commit: `git status --ignored gta4-recomp` non deve mostrare nulla di queste
due cartelle come tracciato. `gta4-recomp/CMakeLists.txt` legge
`generated/sources.cmake`: spostare l'output fuori dall'albero vorrebbe dire
cambiare anche quel file, e non serve.

## 2. Il generatore e la TU

- **Il generatore non applica XEXP.** `project_recompiler.cpp` e
  `codegen_context.cpp` caricano un solo file (`[entrypoint] file_path`). La
  TU va applicata prima, producendo l'immagine patchata (`default_v8.xex`).
  Il runtime invece applica da solo la sibling `default.xexp` al caricamento
  (`user_module.cpp`: `path_ + "p"`).
- **Codice generato e immagine caricata devono coincidere.** Le funzioni
  generate sostituiscono il codice PPC dell'immagine che il runtime carica:
  se il runtime carica base + TU8, il codice va generato da base + TU8.
- **Generare dal solo XEX di base** (strada C) cambia:
  - config: `[rexcrt]` (memset, memcpy, str*: indirizzi della v8) e
    `[functions]` (entrate aggiunte a mano, thunk, callback) non valgono più.
    Si parte da un config con solo le opzioni globali e `[analysis]`, e si
    aggiungono le entrate che la validazione del generatore segnala;
  - hook: tutti i file di `gta4-recomp/src` che agganciano indirizzi
    (presentazione, input, lzx, popolazione, multiplayer, …) vanno esclusi;
  - patch: nessuna TU, quindi nessuna delle correzioni della v8, e
    l'installer (`IsInstallReady`) va saltato;
  - runtime: `default.xexp` non deve esserci accanto al `default.xex`.

## 3. Rigenerare con le macro NX

Le macro NX sono già nel template (`resources/templates/codegen/init_h.inja`,
ramo `#if REX_PLATFORM_NX`, vedi 03 §9). I template sono incorporati
nell'eseguibile al momento della build (`embed_templates`), quindi basta un
`rexglue` costruito da questo albero.

1. Preparare `gta4-recomp/assets/default_v8.xex` dall'eseguibile scelto in §0
   (strada A: base USA + TU8 applicata; lo fa `xex_patcher` di
   `third_party/xbox_support`, oppure l'installer).
2. Costruire `rexglue` (§1).
3. `cd glue/rexglue-sdk-main/gta4-recomp && <build>/rexglue codegen gta4_manifest.toml`.
4. Controllare che `generated/gta4_init.h` contenga il ramo
   `#if REX_PLATFORM_NX` con `rex::memory::GuestToHost`. Il generatore scrive
   un solo header per tutte le piattaforme: la scelta avviene al momento
   della compilazione con `REX_PLATFORM_NX`.
5. Compilare un solo `gta4_recomp.0.cpp` con la toolchain Switch prima di
   lanciare gli altri 86: il codice generato non è mai passato per GCC per
   intero (03 §9 ne ha compilate due funzioni in scratch).

## 4. Primo NRO del gioco

Obiettivo: NRO che carica l'eseguibile da `sdmc:`, crea il thread principale
ed entra nel punto d'ingresso del gioco, senza grafica.

### Target CMake

Un progetto nuovo `tools/switch-game` (come `tools/switch-smoke`, Switch-only):

- `add_subdirectory` dell'SDK con la configurazione headless che la
  toolchain Switch già forza (`REXGLUE_CORE_ONLY`, `REXGLUE_RUNTIME_ONLY`,
  `REXGLUE_HEADLESS_KERNEL`, render-audio e XMA);
- sorgenti: `generated/sources.cmake` + `gta4_init.cpp` + un `main.cpp`
  nuovo. **Non** `gta4_app.cpp` né gli hook: dipendono da `ReXApp`, SDL,
  finestra e grafica;
- link come switch-smoke (`--start-group` delle librerie dell'SDK), NRO con
  `nx_create_nro`. Da verificare subito: dimensione dell'NRO (il codice
  generato è grande) e tempo di link.

### `main.cpp`

Sul modello di `ios/bridge/theft4_boot.cpp`, che fa la stessa cosa su iOS
fino al caricamento:

```cpp
rex::cvar::Init(argc, argv);
// logging durevole come switch-smoke
rex::Runtime runtime("sdmc:/switch/theft4/game",          // game:, d:
                     "sdmc:/switch/theft4/user",
                     "sdmc:/switch/theft4/game/update",   // update:
                     "sdmc:/switch/theft4/cache");
rex::RuntimeConfig config;       // graphics vuoto, gpu_plugin vuoto
runtime.Setup(PPCImageConfig, std::move(config));   // da gta4_init.cpp
runtime.LoadXexImage("game:/default.xex");          // applica default.xexp
auto thread = runtime.PrepareModuleLaunch();
// log "about to resume main thread at <entry>"
thread->Resume();
```

- **File da `sdmc:`, non dalla RomFS.** `Runtime` monta `game_data_root` con
  un `HostPathDevice`, e su libnx `std::filesystem` / `fopen` funzionano su
  `sdmc:/…`. Il gioco (circa 7 GB estratti) non entrerebbe comunque in una
  RomFS ragionevole. Layout: quello di `docs/IOS_SIDELOAD_INSTALL.md` §3
  (`default.xex`, `default.xexp`, `*.rpf`, `common/`, `xbox360/`, `audio/`,
  `update/`), sotto `sdmc:/switch/theft4/game/`. Da verificare: che
  `HostPathDevice` non usi API non disponibili su newlib (per esempio
  attributi o permessi dei file).
- **Senza renderer.** `RuntimeConfig.graphics` vuoto e `gpu_plugin` vuoto:
  niente GPU. Il kernel headless ferma con un messaggio chiaro alla prima
  export non disponibile (`REX_HEADLESS_UNAVAILABLE`): è il comportamento
  voluto per questo passo. Un `IGraphicsSystem` nullo (che accetta
  `VdInitializeRingBuffer`, `VdSwap` ecc. senza disegnare) è il passo dopo,
  non questo.
- **Fino al punto d'ingresso.** Criterio di successo: nel log l'ingresso
  nella funzione generata del punto d'ingresso dell'XEX (e la prima export
  del kernel chiamata dal gioco). Da lì in poi ci si aspetta di fermarsi su
  una export headless non disponibile, oppure su MMIO non riconosciuto nella
  finestra 0x7F (03 §12, non ancora implementato).

### Prerequisiti già verificati dai run su console

Memoria guest (design A), thread e core, uscita pulita, gestione dei fault
con ripresa (run 6, T9/T10), log durevole. Resta aperto il MMIO non
riconosciuto in 0x7F (03 §12).

## 5. Ordine proposto

1. Decidere la strada A/B/C (§0).
2. Costruire `rexglue` su Windows con il clang di Visual Studio e misurare
   tempo e memoria di `rexglue codegen`.
3. Rigenerare e compilare un solo file generato per Switch.
4. `tools/switch-game` con `main.cpp` minimo; prima solo `Setup` +
   `LoadXexImage` (come iOS), poi `Resume` del thread principale.
5. Implementare 03 §12 (MMIO 0x7F) se il run si ferma lì.

## 5b. Obbligatorio per la fase grafica: Mesa

Prima di disegnare con NVK, il driver va costruito da noi con queste parti del
patch di nfsmw-nx (`mesa/mesa-switch-nfsmw.patch`, vedi 08 §7). Il Mesa
installato nei portlibs (build di NaGa, 26.2.1) non le ha:

1. **Fix `FADD32I` + `.SAT` in NAK (`sm50.rs`).** Correttezza, non
   prestazioni: su Maxwell `FADD32I` non ha il bit di saturazione e il flag
   veniva perso in silenzio (in NFS il mare nero). La legalizzazione deve
   spostare l'immediato lungo in un registro quando l'add satura.
2. **ZCULL sul percorso Vulkan**: geometria chiesta al kernel in
   `nvkmd_switch_pdev.c`, contesto ZCULL legato al canale 3D, buffer ZCULL
   azzerato, immagini depth senza `TRANSFER_DST`.
3. **Latenze di scheduling di NAK**: texture e load globali da 32 a 200 cicli;
   `peephole_select` di NIR da 0 a 8 (controllare con `NVK_SHADER_STATS=1` che
   la memoria locale resti a 0).

Ogni modifica a NAK alza la revisione nella chiave della cache degli shader,
altrimenti la console riusa i binari vecchi.

## 6. Esito: strada C, generazione del 2026-09-27

Configurazione: `gta4-recomp/config-eu-base/` (manifest e config senza hook,
senza `[rexcrt]`, senza TU). L'eseguibile europeo è letto dove sta e non viene
copiato. L'output va in `gta4-recomp/generated/eu-base/`, ignorata da git.

- **Strumento.** `rexglue` costruito con il preset `win-amd64-release` e il
  clang 22.1.3 di Visual Studio (vcvars64 + `VC/Tools/Llvm/x64/bin` in testa al
  `PATH`, CMake e Ninja di Visual Studio). Serve
  `-DCMAKE_CXX_SCAN_FOR_MODULES=OFF` al configure: con CMake 4.3 e Ninja
  Multi-Config (`CMAKE_CROSS_CONFIGS=all`) la scansione dei moduli C++ genera
  due volte lo stesso `.modmap` di SDL3 e ninja si ferma subito. Configure
  146 s la prima volta, build del solo target `rexglue` 6 min (717 passi).
- **Primo run** (solo opzioni globali e `[analysis]`): 47,8 s, picco 429 MB,
  36.646 funzioni, 4 errori `UnresolvedCall` (`b` verso una destinazione fuori
  da ogni funzione).
- **Corrispondenze con la v8 USA**, trovate confrontando finestre di
  istruzioni normalizzate del codice generato:

  | Voce del config USA | Equivalente EU | Come l'ha trattata l'analisi EU |
  |---|---|---|
  | `0x8217C108` (destinazione da `sub_8217C5A8`) | `0x82154100` (salto da `0x821545A0`: stessa distanza, 0x4A0) | non risolta |
  | `0x82A77E28` (thunk `b sub_82812248`) | `0x82A05828` (salto da `0x8274D070`) | non risolta |
  | `0x8219E410` (destinazione da `sub_821A2EDC`) | `0x82176BE0` | trovata da sola (`sub_82176BE0`) |

  Gli altri due errori EU non hanno una voce nel config USA: `0x82518A40`
  corrisponde a `0x824FF7C0`, che l'analisi USA trova da sola. Per
  `0x826EA240` il codice è ripetitivo e ci sono più candidati USA.
- **Secondo run**, con quelle 4 voci in `[functions]`: 45,6 s, picco 442 MB,
  **0 errori**, 36.643 funzioni, 85 file, 172,6 MB. `gta4_init.h` contiene il
  ramo `#if REX_PLATFORM_NX`. Per confronto, il codice generato di upstream
  dalla v8 USA ha 38.351 funzioni in 89 file (178,3 MB).
- **Effetto collaterale, corretto.** In modalità strumento il generatore
  creava il `Runtime` con la sola radice del gioco, che faceva anche da radice
  utente: il runtime ci scriveva `liberty_live_identity.bin` (identità Live,
  88 byte). Ora `project_recompiler.cpp` passa una cartella di lavoro separata
  (`%TEMP%/rexglue-codegen/<progetto>/{user,cache,marketplace,saves}`) e
  rifiuta una cartella di lavoro dentro la radice del gioco. Terzo run dopo la
  correzione: nessun file nuovo nella cartella del gioco, output identico
  (36.643 funzioni, 0 errori).
- **Registri come locali e chiamate dirette** (patch `share_registers` di
  nfsmw-nx nel generatore, `tools/switch-codegen/`). Config: `cr_as_local`,
  `xer_as_local`, `ctr_as_local`, `non_volatile_as_local` a `true`.
  - Primo run: `read_before_write.py` segnala 736 funzioni, di cui 730 falsi
    positivi dello script originale (spill `stvx128` di v64–v127 sulla pila,
    righe di sincronizzazione emesse dal patch, `cr6.setFromMask` dei `vcmp.`),
    corretti nella nostra versione. Restano 6: 5 frammenti veri, marcati
    `share_registers` nel config, e `0x829A88C0` (`r30 ^ r30`, azzeramento),
    falso positivo lasciato.
  - Secondo run: 45,7 s, picco 439 MB, **0 errori**, 36.643 funzioni, 1 sola
    segnalazione (il falso positivo), 888 righe di sincronizzazione emesse.
  - `direct_calls.py`: **123.940 chiamate dirette su 123.948 (99,99 %)**;
    restano deboli le 8 verso i 18 indirizzi del config, contati come agganciati
    per prudenza. Output: 84 file, 170,7 MB.
  - Non compilato per Switch: le macro di memoria dipendevano dall'esito di T11 (vedi §7).

## 7. Prima compilazione del gioco per Switch (2026-09-28)

**Macro definitive:** modello A, tabella (03 §13, run 8). Il template non cambia per la memoria.

### Rigenerazione

`rexglue` ricostruito da questo albero, poi `rexglue codegen config-eu-base/gta4_eu_base_manifest.toml` e `direct_calls.py --hooked-from config-eu-base`:

- 47,9 s, 0 errori, 84 file, 163 MB;
- 123.940 chiamate dirette su 123.948;
- `read_before_write.py`: solo il falso positivo noto (`0x829A88C0`).

**Unico errore del primo tentativo di compilazione.** `REX_DB16CYC()` (per `db16cyc`, cicli di spin-wait) veniva emesso da `builders/system.cpp`, ma non era definito in nessun header. Ora è nel template: `yield` su ARM64 (come il backend AArch64 di Xenia), `pause` su x86. Ho confrontato ogni nome `REX_*` dei sorgenti generati con le definizioni dell'header e dell'SDK: non ne manca nessun altro.

### `tools/switch-game`

| Parte | Cosa fa |
|---|---|
| File del gioco | letti da `sdmc:/switch/theft4/game` (`game:`/`d:`). Serve `default.xex`; se c'è `default.xexp` il programma si ferma, perché il codice è generato dall'eseguibile base |
| Runtime | `rex::Runtime` con `InitializeKernel` e senza grafica, audio o input; `Setup(PPCImageConfig)`, `LoadXexImage`, `PrepareModuleLaunch`, `Resume` del thread principale all'entry point dell'XEX |
| Log durevole | `sdmc:/switch/theft4/game.log`, come nello smoke: ogni riga (anche dell'SDK) aperta, scritta, `fsync`, chiusa; righe "about to" prima di ogni passo. Il run precedente resta in `game.prev.log` |
| Crash log | `sdmc:/switch/theft4/game-crash.log`: un gestore installato per ultimo registra i fault che nessun gestore del runtime prende. Scrive PC, LR, SP, FAR, x0–x30 e PC/LR come offset dalla base del modulo (stampata anche in `game.log`), poi lascia il fault al kernel, cioè al crash report di Atmosphère |
| Arresto | `rex::diagnostics::StopAtMissingFunction` (nuovo punto unico dell'SDK, debole) ridefinito in `main.cpp`. Il primo arrivo scrive una riga `STOP at the first function missing on Switch: <nome> (<motivo>), guest LR …` e lo stato della memoria guest, poi ferma il thread; con + si esce |
| Cosa porta all'arresto | export del kernel headless non disponibili; export che richiedono la grafica (`VdInitializeRingBuffer`, `VdSetGraphicsInterruptCallback`, `VdEnableRingBufferRPtrWriteBack`); `seh_rethrow`; import che l'SDK non definisce (`missing_imports.py` genera uno stub per ognuna, confrontando le 237 import dell'header con `nm` delle librerie linkate: oggi **0 su 237**) |
| Uscita | `svcExitProcess`: i thread guest sono ancora vivi, e tornare al loader scaricherebbe codice che stanno eseguendo (04 §5) |

**Non fermano il run** le export che il kernel implementa come `REX_EXPORT_STUB`: come sul desktop, scrivono un warning nel log e ritornano.

### Build

Senza LTO, Ninja `-j3`, GCC 16.1, `-O3`, `-g1` sui file generati (solo tabelle di riga, per `addr2line`).

| Misura | Valore |
|---|---|
| SDK + `main.cpp` + un file generato (prima build, 355 passi) | 9 min 00 s |
| 84 file generati + link + NRO | 25 min 08 s |
| Totale | circa 34 min |
| Errori / warning nel codice generato | 0 / 0 (dopo `REX_DB16CYC`) |
| `switch-game.nro` | 44.062.917 byte (42,0 MiB) |
| `switch-game.elf` | 261 MB con le informazioni di debug; `.text` 41,3 MiB, `.data` 0,7 MiB, `.bss` 8,8 MiB |

Il tempo dei file generati è per eccesso. Nei primi minuti girava ancora la build precedente (fallita su `REX_DB16CYC`), che finiva i suoi file in corso: per un po' i job erano più di tre.

**Percorso:** `out/build/switch-game/switch-game.nro`. La configurazione è nel commento in testa a `tools/switch-game/CMakeLists.txt`. Da Bash va esportato `DEVKITPRO=/opt/devkitpro` e va usato `/usr/bin/ninja`.

### Sulla console

- Copiare `default.xex` europeo (e i dati del gioco) in `sdmc:/switch/theft4/game/`, senza `default.xexp`.
- Avviare in modalità applicazione (title takeover): serve il pool di circa 3,2 GiB.
- Riportare `game.log`, `game-crash.log` e, se c'è, il crash report di Atmosphère.
- Primo arresto atteso: una export grafica, oppure MMIO non riconosciuto in 0x7F (03 §12, non implementato).

## 8. Run 9: il gioco esce in silenzio dentro `Runtime::Setup` (2026-09-28)

`out/console-runs/run9/`: smoke completo e OK. `game.log` e `game.prev.log` (due avvii) sono identici fino all'ultima riga; `game-crash.log` esiste ma è **vuoto** (0 byte); nessun crash report di Atmosphère.

### Cosa dice il log

- Ultima riga: `XThread::Execute thid 1 ... 'Kernel Host Tasks'`, cioè il worker dei compiti del kernel avviato alla fine del costruttore di `KernelState`.
- Prima: `ContentManager roots: user='sdmc:/switch/theft4/sdmc:/switch/theft4/user'` e `Unable to create multiplayer identity directory ...: Invalid argument`.
- **Non** compaiono `Runtime::Setup failed`, `Mounted ... at \Device\Harddisk0\Partition1` né la riga di `main` dopo `Setup`.

Quindi:

- **Il punto di arresto non è stato raggiunto**, e nessuna export del kernel è stata chiamata: il codice del gioco non è mai partito.
- **Ultimo sottosistema attivo:** l'inizializzazione del kernel in `Runtime::Setup`, dopo il costruttore di `KernelState` e prima di `SetupVfs`, cioè `InitializeKernel` (app XAM, moduli xboxkrnl/xam/xbdm).
- **Percorso di uscita:** non passa dal logger (un `REX_FATAL` avrebbe scritto una riga CRITICAL) né dal gestore dei fault (`game-crash.log` vuoto, nessun crash report). Resta un'uscita "normale" di newlib: `abort` → `_exit` → ritorno al loader. È il comportamento di `rex::FatalError`, degli assert a **due argomenti** (`assert_true(expr, msg)` e simili, attivi anche in Release) e di un'eccezione non catturata (`std::terminate`). Tutti questi scrivono il motivo **solo su stderr**, che il run 9 non registrava. Dal codice statico non si ricava quale sia: il run 10 lo dirà.

### Correzioni

- **Percorsi `sdmc:/`.** `std::filesystem::absolute` di newlib non conosce i dispositivi e antepone la cartella corrente. È l'origine del percorso utente raddoppiato. Lo stesso errore c'era in `SetupVfs` per `game:` e `update:`, quindi anche il montaggio del gioco sarebbe fallito. Ora c'è `rex::filesystem::MakeAbsolute`, che su Switch lascia invariati i percorsi con dispositivo.
- **Ogni uscita lascia una traccia in `game.log`:**
  - `exit`, `_exit`, `_Exit`, `abort` e `svcExitProcess` passano da wrapper di link (`-Wl,--wrap`), che registrano il codice e l'LR del chiamante come offset dalla base del modulo (indirizzo ELF, per `addr2line`);
  - `std::terminate` registra il tipo e il `what()` dell'eccezione;
  - un gestore `atexit` segna le uscite da `exit()`;
  - `main` registra `main returned`;
  - stderr è copiato in `game.log` riga per riga (prefisso `[stderr]`);
  - `kernel_init` ha una riga prima e una dopo.

  Ho verificato nell'ELF che solo i wrapper chiamano le funzioni reali.
- **Stub del kernel:** ogni `REX_EXPORT_STUB` viene registrato alla prima chiamata (nome e LR guest) e contato; il riepilogo va nel log all'arresto e all'uscita. La cvar `nx_fatal_stubs` (default `false`) li rende fatali.
- **Pool dei blocchi guest:** vedi 03 §13, "Pool dei blocchi".

**Allocazioni fallite nel run 9: nessuna.** Il log non ha errori di `memory_switch`, e l'arena si è riservata normalmente.

**File del gioco sulla SD:** il run 9 conferma solo `default.xex` (11.841.536 byte) e l'assenza di `default.xexp`. Il resto del disco non è mai stato letto, perché il run non è arrivato a `SetupVfs`.

### Build

| | |
|---|---|
| Smoke (SDK toccato + smoke) | 9 min 16 s |
| Gioco (212 passi, tutti gli 84 file generati) | 27 min 37 s, 0 errori |
| `switch-smoke.nro` | 2.332.869 byte |
| `switch-game.nro` | 43.899.077 byte |

Il gioco è stato ricompilato per intero perché il codice generato include `rex/hook.h` (e `rex/filesystem.h`, `rex/memory/utils.h`): cambiare `REX_EXPORT_STUB` in `hook.h` tocca per forza tutti i file generati.
