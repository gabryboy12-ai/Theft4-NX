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
  - `<Visual Studio>/VC/Tools/Llvm/x64/bin/clang++.exe`: **clang
    22.1.3**, target `x86_64-pc-windows-msvc`. Soddisfa "Clang 18+" ed è la
    combinazione che il preset Windows si aspetta (headers e librerie MSVC).
    Va usato da un "x64 Native Tools Command Prompt" / `vcvars64.bat`, con
    quella cartella `Llvm/x64/bin` in testa al `PATH`, e CMake/Ninja di
    Visual Studio (non quelli di MSYS, che scrivono percorsi POSIX).
  - un `MSYS2/clang64` separato **esiste ma è vuoto**: pacman c'è, clang no. Si può
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

## 9. Run 10: fault sulla tabella delle funzioni, nessun crash report (2026-09-29)

`out/console-runs/run10/`. Lo smoke arriva in fondo, OK. Il gioco supera `Setup` (corretto il percorso `sdmc:/`), monta `game:`, carica il XEX (entry point `829A0810`), fa `Resume` del thread principale e poi si chiude.

### Cosa è successo

- **Nessuna riga `TERMINATION`.** Le righe `[stderr]` sono quelle del runtime: `Unhandled Switch access violation`, `pc=0x34e9cba424`, `far=0x774a0e1b40`, `esr=0x92000006` (data abort, translation fault), `is_write=0`.
- **`game-crash.log`** ha il fault completo: PC `base + 0x21A3424`, LR `base + 0x21A35C4`.
- **Tipo:** un fault in lettura non gestito, sul thread principale del gioco. Non è un abort, né un'eccezione, né un exit, né un blocco.
- **addr2line** sull'ELF di quel build (`out/build/switch-game/switch-game.elf`, 23:50):
  - PC → `__imp__sub_82998DD0`, `gta4_recomp.75.cpp:25543`, cioè `REX_CALL_INDIRECT_FUNC(ctr.u32)`;
  - LR → `sub_829989F8`, `gta4_recomp.75.cpp:24896`, il chiamante.
- **Indirizzo:** `far - virtual_base = 0x842E1B40` = `x2`. È la voce della tabella delle funzioni (base `0x831F0000`, dal log) per la destinazione `0x82998DA0` (= `x0`, = `r11` nel codice): `0x831F0000 + (0x82998DA0 - 0x82120000) × 2`.
- **Causa:** `XexModule` fa `LookupHeap(0x82000000)->Reset()` (xex_module.cpp:463) su tutto l'heap `0x80000000`, dopo che `Runtime::Setup` vi ha creato e riempito la tabella (36.880 voci).
  - Sul desktop `Reset` azzera solo la tabella delle pagine dell'heap.
  - Su NX restituiva al pool ogni blocco impegnato. Si vede nel log: 23 blocchi dopo `Setup`, 21 dopo `LoadXexImage`.

### Correzioni

- **`BaseHeap::Reset` su NX come sul desktop:** le pagine impegnate restano mappate con il loro contenuto. Il loro riferimento diventa "orfano" e lo rilascia `Dispose`, quindi a fine run i conti tornano a zero.
- **`Memory::ResetHeapKeepingFunctionTables`** (usata dal loader) segna di nuovo come allocate le tabelle delle funzioni di quell'heap. Anche sul desktop, dopo `Reset`, l'heap poteva assegnare quelle pagine ad altro.
- **Crash report di Atmosphère.**
  - Il nostro `__libnx_exception_entry` restituiva i fault non gestiti con `svcReturnFromException(0xF801)`: il run 10 mostra che così il processo si chiude senza report. Il run 5, con `svcBreak`, il report l'aveva prodotto.
  - Ora il dispatcher riprende il thread in fault, con i suoi registri e il suo stack, in `rex_nx_unhandled_break`, che chiama `svcBreak(BreakReason_Panic, &info, 48)`; `info` contiene pc, lr, sp, far, esr e tipo. LR viene impostato al PC del fault.
  - Prima di arrivarci, il gestore di `switch-game` scrive `game-crash.log`.
  - **Fault dentro il gestore:** un contatore di profondità nell'entry lo manda su uno stack di emergenza, con una sola riga su stderr, poi `svcBreak`. Il frame esterno non viene sovrascritto.
  - Nessun percorso chiude il processo prima: il wrapper di `svcExitProcess` scatta solo quando qualcuno chiama `svcExitProcess`.

### Smoke: tetto del pool

| | Run 8 | Run 10 |
|---|---|---|
| Blocchi da 2 MiB ottenibili | 1542 MiB (`aligned_alloc` per blocco) | **1984 MiB** in una sola allocazione, più 256 MiB del pool del runtime = **2240 MiB** (+45%) |
| Motivo dello stop | heap esaurito | heap esaurito (non il kernel) |
| Rifiuto del kernel, solo viste | 2001-0103 a 17.624 viste | 2001-0103 a 17.965 viste |

Il gioco prende il suo pool da 1536 MiB all'avvio senza errori.

### Header del codice generato

Inventario misurato con `g++ -MM` su `gta4_recomp.0.cpp`:

| | Prima | Dopo |
|---|---|---|
| Header dell'SDK | circa 64 (`rex/ppc.h` → `hook.h`, `function.h`, `stack.h` → `kernel_state.h`, `xmemory.h`, logging, filesystem, thread, achievement, xam…; più `logging.h`, `chrono/clock.h`, `perf/counter.h`, `thread/mutex.h`, `system/mmio_handler.h`) | **9**: `ppc/generated.h`, `ppc/context.h`, `ppc/intrinsics.h`, `platform/fpscr.h`, `types.h`, `assert.h`, `platform.h`, `image_info.h`, `memory/guest_table.h` (+ simde) |

Come funziona:

- **`rex/ppc/generated.h`** contiene solo quello che il codice generato deve vedere inline: il layout di `PPCContext`, gli intrinseci, il descrittore dell'immagine e la tabella di traduzione di Switch.
- **Servizi fuori linea.** Trap, `mftb`, lock delle interruzioni e MMIO riconosciuto chiamano funzioni in `src/system/ppc_generated.cpp`: nessuna è un percorso caldo.
- **`REX_RECOMPILED_TU`.** Il generatore scrive `#define REX_RECOMPILED_TU` in testa a ogni `gta4_recomp.N.cpp` e a `gta4_init.cpp`. Hook e app che includono `gta4_init.h` senza la define ricevono l'API completa come prima.
- **Dipendenze che restano, e perché:**
  - il **layout di `PPCContext`** e le **macro di funzione** (`context.h`): sono l'ABI fra codice generato e runtime, un cambio lì deve ricompilare;
  - gli **intrinseci** (`intrinsics.h`, `fpscr.h`, simde): vanno inline per le prestazioni;
  - **`guest_table.h`**: la traduzione inline di ogni accesso su Switch;
  - `types.h`, `assert.h`, `platform.h`, `image_info.h`: piccoli e stabili;
  - fra i file generati, **`gta4_register.cpp`** include ancora `function_dispatcher.h` per l'interfaccia del registrar: un file solo.
- **Rigenerazione necessaria:** la define viene dal generatore. Rigenerato: 48,3 s, 0 errori, 123.940/123.948 chiamate dirette.
- **Primo tentativo di build:** mancava `<atomic>` (`std::atomic_thread_fence` per `sync`/`lwsync`). Rimesso fra gli header standard del template. Un controllo di tutti i nomi `std::` del codice generato trova solo `fma`/`isnan` (`<cmath>`, già incluso) e le fence.

### Stato delle build

- **Smoke:** ricompilato con tutte le modifiche all'SDK (5 min 9 s, 0 errori): `out/build/switch-smoke/switch-smoke.nro`.
- **Gioco: build interrotta.** Claude Code l'ha fermata per memoria di sistema esaurita. La causa è una mia svista: il Ninja della build precedente, fallita su `<atomic>`, era ancora attivo, e per circa dieci minuti due Ninja hanno compilato nella stessa cartella, fino a 6 file generati insieme.
  - 34 degli 84 file generati hanno compilato con il nuovo header, senza errori.
  - Ho terminato i due processi Ninja rimasti e riparato `.ninja_deps` con `ninja -t recompact`.
  - Restano 46 file generati e il link. `out/build/switch-game/switch-game.nro` è ancora quello del run 10.

## 10. Run 11: il gioco parte e non lascia traccia; il crash del + è nostro (2026-09-30)

`out/console-runs/run11/`: `game.log`, `game-crash.log`, `smoke.log`. Il gioco
supera `Setup`, `LoadXexImage` e `PrepareModuleLaunch`, fa `Resume` del thread
principale all'entry point `829A0810`, e poi il log tace fino al +. Dopo circa
un minuto il + chiude il processo con un crash report di Atmosphère.

### Dove si è fermato

- **Non è il nostro punto di arresto**: nessuna riga `STOP at the first function
  missing on Switch`. Quindi nessuna export headless mancante, nessuna export
  grafica (`VdInitializeRingBuffer` & co.), nessun import senza definizione.
- **Nessuno stub chiamato**: `export stubs at exit: 0 distinct, 0 calls`.
- **Nessun altro thread nel log**: l'unico `XThread::Execute` è `Kernel Host
  Tasks` (thid 1). Per i thread guest quella riga è `NOISY_DEBUG`, non scritta:
  il log non può dire se il gioco ne ha creati.
- **Nessun file aperto dal gioco**: le uniche righe `[fs]` sono quelle del
  loader (`default.xexp` cercato e assente).
- **Il thread principale non è finito**: la riga "main guest thread is no
  longer running" non c'è, quindi `is_running()` è rimasto vero fino al +.

**Conclusione: il thread principale gira (o aspetta) dentro il codice del
gioco o dentro export implementate, che non scrivono niente.** Il log del run 11
non può dire dove: le export implementate (`REX_EXPORT`) non lasciavano traccia.
È il motivo delle modifiche di questo giro (sotto).

### Il crash report del +

`game-crash.log` (e `[stderr]` in `game.log`): data abort in scrittura,
`far = 0`, PC `base + 0x255B500`, LR 0. Con `addr2line` sull'ELF di quella build
(`out/build/switch-game/switch-game.elf` del 2026-09-29 01:51, sorgenti del
commit `882fce2`):

| Indirizzo | Funzione |
|---|---|
| PC `+0x255B500` | `ConsoleSwRenderer_drawChar` (libnx `console_sw.c:161`), `strh w1, [x10, w4, uxtw #1]` con `x10 = 0` |
| LR `0` | `drawChar` usa x30 come registro di lavoro (`mov w4, w30`): LR non significativo |

**Non sono i thread guest.** `main` chiamava `consoleExit` e poi
`svcExitProcess`; il wrapper di `svcExitProcess` registra la terminazione con
`GAME_ERROR`, che faceva `printf` sulla console appena chiusa: framebuffer
nullo, fault nel disegno del carattere, trampolino verso `svcBreak` e crash
report. La riga `TERMINATION: svcExitProcess` infatti manca nel log: il fault
avviene nel `printf`, prima della scrittura su file. `svcExitProcess` in sé
termina tutti i thread del processo insieme e non produce report.

**Correzione** (`tools/switch-game/main.cpp`): tutta l'uscita a schermo passa da
`ConsolePrint`, sotto `g_console_mutex` (attesa limitata a 200 ms);
`CloseConsole` chiude la console una volta sola sotto lo stesso mutex, e dopo
nessuno disegna più, compresa la copia di stderr (che dal gestore dei fault
prova il mutex senza aspettare). Le righe dopo la chiusura vanno solo in
`game.log`. Non serve fermare i thread guest: `svcExitProcess` li chiude con il
processo.

Il crash report di Atmosphère non è nella cartella del run: i dati sono quelli
di `game-crash.log`, scritti dallo stesso fault.

### Le correzioni del run 10 funzionano

| Correzione | Prova nel run 11 |
|---|---|
| `BaseHeap::Reset` tiene la memoria su NX | `Heap reset: kept the function table at 831F0000+1207AB8`; blocchi guest da 23 dopo `Setup` a **31** dopo `LoadXexImage` (nel run 10 scendevano a 21) |
| Tabella delle funzioni preservata | nessun fault sulla tabella: nel run 10 il fault a `__imp__sub_82998DD0` (`REX_CALL_INDIRECT_FUNC`) arrivava subito dopo il `Resume`; qui il thread gira un minuto senza fault |
| Trampolino verso `svcBreak` | il fault della console è finito in `game-crash.log`, poi in `svcBreak` con il crash report di Atmosphère (nel run 10 il processo si chiudeva senza report) |

### Cosa aggiunge il run 12

- **Traccia delle export implementate** (solo NX, `REX_EXPORT` in
  `rex/hook.h`, `export_stubs.{h,cpp}`): la prima chiamata di ogni export va nel
  log (`export first call: <nome> (guest LR …, thread …)`), ogni export conta le
  chiamate, e ogni thread registra l'export in cui si trova, da quando, e
  l'ultima da cui è uscito. All'uscita: `exports called at exit` con i
  conteggi.
- **Battito ogni 10 s**: export distinte e chiamate, blocchi guest (attuali,
  picco, pool).
- **Istantanea di ogni XThread** a 5, 20 e 60 s, poi ogni 2 minuti e al +:
  nome, guest/host, export in corso e da quanto ("IN KeWaitForSingleObject for
  40.2 s") o ultima export, LR guest e r1, e dall'host PC, LR e fino a 12
  indirizzi di ritorno trovati sullo stack (thread messo in pausa con
  `svcSetThreadActivity` solo per leggere registri e stack; niente lock né
  allocazioni mentre è in pausa). `python tools/switch-game/where.py game.log`
  traduce gli offset con `addr2line`.
- Memoria guest all'uscita con il picco (`peak N = M MiB`) e il pool.

Con questo il run 12 dice dove sta ogni thread: in un'export che aspetta (e
quale), o in una funzione ricompilata (`sub_…` dall'`addr2line` del PC host).

Due aggiunte venute dalla fase PC (§14), già nella NRO del run 12:

- l'istantanea prende la lista degli XThread con `TryGetObjectsByType` (al
  massimo 200 ms): il lock globale del kernel può essere tenuto da un thread
  guest, e dal thread dell'interfaccia avrebbe bloccato anche il +. Se resta
  occupato, la riga lo dice ("the kernel's global lock stayed busy") e le righe
  vengono dalle tracce delle export;
- il codice generato include le 659 funzioni che l'analisi non aveva raggiunto
  (`gta4_eu_base_gaps*.toml`): il PC si fermava sulla prima dopo 12 export.

### Prossimo passo se l'arresto è su una funzione grafica (proposta; implementata nel run 14, §17)

Se il run 12 si ferma su `VdInitializeRingBuffer` (o `VdSetGraphicsInterruptCallback`,
`VdEnableRingBufferRPtrWriteBack`), il minimo per andare avanti senza grafica
vera è una **GPU nulla** che tiene il gioco in passo senza disegnare:

1. **Export `Vd*` accettate**: `VdInitializeRingBuffer` registra base e
   dimensione del ring; `VdEnableRingBufferRPtrWriteBack` registra l'indirizzo
   di writeback del puntatore di lettura; `VdSetGraphicsInterruptCallback`
   registra callback e argomento; `VdQueryVideoMode`/`VdGetCurrentDisplayInformation`
   rispondono 1280x720; `VdSwap` ritorna subito.
2. **Consumo del ring**: alla scrittura del puntatore di scrittura (MMIO
   `CP_RB_WPTR` nella finestra 0x7FC8…) un thread host "consuma" subito: legge i
   pacchetti PM4 fino al nuovo WPTR e scrive RPTR nel writeback. Dei pacchetti
   esegue **solo quelli che il gioco aspetta**: scritture in memoria e fence
   (`MEM_WRITE`, `EVENT_WRITE_SHD`/`EOP`, le scritture di `SCRATCH_REG` verso
   `SCRATCH_ADDR`: nfsmw-nx nota che senza quella il gioco si blocca entro un
   secondo), `WAIT_REG_MEM` ignorato, `INDIRECT_BUFFER` seguito; draw, stati e
   resolve scartati.
3. **Interrupt di vblank a 60 Hz** da un thread host (0x2C), che chiama la
   callback registrata come fa il `GPU VSync` del runtime; e l'interrupt di swap
   dopo ogni `VdSwap`.
4. Letture MMIO dei registri di stato della GPU (idle, `RBBM_STATUS`) con i
   valori di "GPU ferma e libera".

È quello che il `CommandProcessor` di ReXGlue fa già con la grafica completa,
ridotto alle scritture in memoria: il codice da cui partire è lì (gestione di
`PM4_MEM_WRITE`/`EVENT_WRITE_*` e del writeback). Stima: 1–2 giorni. Se invece
il run 12 mostra il thread in attesa dentro un'export non grafica, si guarda
quella.

## 11. Strategia grafica in tre fasi

Decisa dopo 08 (nfsmw-nx) e 10 (UnleashedRecomp-NX).

### a) Primi fotogrammi con la grafica imitata di ReXGlue su NVK

**Raggiunta il 2026-10-10 (run 20, avvio 3, §27):** `nx_gpu = "vulkan"`,
`nx_mount_cache = false`, tracciamento delle export acceso, GPU al clock di
sistema (non letto; probabilmente 307,2 MHz in portatile). Logo, schermate di
caricamento, "Starting a new game", il video "The Cousins Bellic" e la partita
con i comandi che rispondono; 812 s senza crash fino a un overclock esterno.
Misurato: **0,4 FPS in partita** (2,5 s per fotogramma, 5.300–6.800 draw per
fotogramma), ~98 µs di CPU per draw nel processore dei comandi, che è fermo
tre quarti del tempo; heap 2984 MiB in uso, Vulkan 1395 di 1482 MiB. Cosa
serve per (b): §27 F.

La grafica di ReXGlue (plugin `xenos`: emulazione del command processor, EDRAM
imitata, traduzione degli shader durante il gioco) su NVK, **solo per
verificare che la logica del gioco giri** fino ai menu e in partita.
nfsmw-nx misura con questo percorso circa 2 FPS in gara; GTA IV, con più draw
per frame e streaming continuo, andrà peggio. Non si ottimizza niente qui:
serve a separare gli errori della logica da quelli del renderer. Prima, la GPU
nulla di §10 se serve per arrivarci.

### b) Tracciamento del D3D del gioco

Wrap delle funzioni D3D del gioco (creazione di shader, texture, render target,
`SetRenderState`, draw, `Resolve`, `Present`) con una riga di log per chiamata,
come `app/src/nfsmw_d3d_trace.cpp` di nfsmw-nx. Scopo: **confermare ogni
indirizzo sull'eseguibile europeo** prima di sostituire qualcosa. Gli indirizzi
di partenza sono quelli della v8 USA di `gta4_native` (~135 funzioni del D3D),
tradotti con il confronto di sequenze di istruzioni normalizzate già usato per i
confini delle funzioni (06 §6), o con `tools/editions` di nfsmw-nx (08 §5). Un
indirizzo tradotto è confermato quando il tracciamento mostra la chiamata con
argomenti plausibili nel punto atteso del frame.

### c) Nucleo del renderer nativo di nfsmw-nx

Porting del nucleo PM4 → Vulkan di nfsmw-nx (`app/src/nfsmw_nativo_*`), con gli
hook minimi per GTA IV:

- **dal loro nucleo**: lettura del ring PM4 e decodifica dei pacchetti; stato
  dei registri della GPU; registrazione dei draw; render target come immagini
  Vulkan; resolve senza copia; cache delle texture con untiling al caricamento;
  cache e precompilazione delle pipeline; tre slot di lavoro (64 MB l'uno);
  sotto-allocazione della memoria GPU; il pattern "funzione nativa + guardia
  che confronta con l'originale";
- **hook di GTA IV**: solo la creazione degli shader (per riconoscerli nella
  libreria precompilata) e l'annotazione dei draw (quale passo del frame:
  ombre, riflessi, post-processing), confermati in (b).

**Parti specifiche di GTA IV** (dalla conoscenza di `gta4_native`):

- **immagini da restituire al gioco per l'HDR**: GTA IV rilegge risultati della
  GPU (luminanza media per l'esposizione adattiva e simili); il renderer deve
  scriverli nella memoria guest dove il gioco li aspetta, come il readback
  dell'esposizione di nfsmw-nx;
- **cache di lettura per lo streaming**: GTA IV carica di continuo texture e
  modelli; la cache deve riconoscere per hash i dati già caricati e non
  ricaricarli a ogni ciclo di streaming (UnleashedRecomp-NX: upload di
  streaming raggruppati nel command buffer del frame, 10 §7);
- **tetto della cache delle texture**: nfsmw-nx lo tiene a 384–512 MB; per GTA
  IV va misurato, sapendo che tutto viene dallo stesso heap del pool guest
  (768 MiB) e che il limite vero è il numero di NvMap (circa 4.000, §12).

**Vincolo del modello A:** le scritture del renderer nella memoria del gioco
(readback, fence, `SCRATCH_REG`, RPTR) **non devono passare dal fault del
write-watch**. Nel modello A una pagina sorvegliata è in sola lettura e una
scrittura host provocherebbe un fault nel thread del renderer. Il renderer
scrive con `GuestToHost` e chiama esplicitamente l'invalidazione del range
(le callback di invalidazione della memoria fisica), invece di lasciare che sia
il fault a farlo.

## 12. Trappole di NVK su Horizon (da `platform-notes.md` di nfsmw-nx)

| Trappola | Cosa fare |
|---|---|
| **`__nx_nv_transfermem_size`**: libnx dà a nvdrv 8 MB di transfer memory, circa **4.000 NvMap**. Esauriti, la creazione fallisce con `0x235C` (`LibnxNvidiaError_SharedMemoryTooSmall`), che sembra un out of memory ma non lo è (nfsmw-nx: 3.585 texture, 600 MB) | il simbolo è weak: alzarlo dall'applicazione **prima** che Mesa chiami `nvInitialize()` (loro: cvar `nfsmw_switch_nvmap_mb`) |
| **Allocazioni GPU lente**: una texture con allocazione dedicata costava ~1,9 ms di CPU (`nvMapCreate` 422 µs, `nvAddressSpaceAllocFixed` 127 µs ×2, `MapBufferEx` 626 µs ×2) | **sotto-allocazione** da blocchi grandi (32 MB: 0,75 ms per texture; 15 texture nuove in un frame da 28,5 a 11,3 ms). `requiresDedicatedAllocation` non è mai vero su questo driver |
| **Fence**: ogni `vkGetFenceStatus`/`vkWaitForFences` è un ioctl, anche a GPU finita | contare le fence per frame; non sondare |
| **Submit**: ogni `vkQueueSubmit` è una chiamata IPC; il WSI aggiunge un submit vuoto per present. Due submit per frame lasciavano la GPU ferma 3,6 ms | un submit per frame |
| **Pipeline**: ~**59 ms** l'una senza cache (112 in 6,6 s) | `VkPipelineCache` su SD **più** l'elenco delle pipeline usate, ricreate su un thread in background all'avvio (loro: 113 pronte in 0,3 s) |
| **Cache degli shader di Mesa su SD** (`sdmc:/.mesa`, thread `disk$` con stack da 8 MB) | duplicato della propria cache: **`MESA_SHADER_CACHE_DISABLE`** |
| **Presentazione**: una sola coda Vulkan; esattamente tre immagini di swapchain; un solo acquire alla volta; solo FIFO e IMMEDIATE (IMMEDIATE non strappa: compone nvnflinger a 60 Hz) | progettare il frame pacing su questi limiti; un thread di presentazione separato ha peggiorato il pacing in nfsmw-nx |
| **Timestamp GPU**: `timestampPeriod` dice 1 ns, il contatore avanza ogni **1,627 ns** | moltiplicare ogni tempo GPU per 1,627 (il profiler di UnleashedRecomp-NX leggeva il 60 % del vero) |
| **Clock in portatile**: un homebrew parte con la GPU a 307,2 MHz | chiedere la configurazione standard **`0x92220008`** (GPU 460,8 MHz, RAM 1331,2 MHz) con `apm`; `apmSetPerformanceConfiguration` ritorna prima del cambio; `pcv` rimette la frequenza della RAM se la si cambia con `clkrst`; gli ID vanno letti dalla tabella (0x92220006 non esiste) |
| **Memoria libera**: `InfoType_UsedMemorySize` vale già 3185 su 3189 MB all'avvio sotto hbloader (tutto è heap) | misurare dentro l'heap; per la GPU `VK_EXT_memory_budget` (in gara ~500 MB su 1,4–1,5 GB) |
| **Due tipi di memoria**: tipo 0 `HOST_CACHED` (serve `vkFlushMappedMemoryRanges`), tipo 1 `HOST_COHERENT` senza cache | memoria che la CPU solo scrive senza cache CPU (`NVK_SWITCH_CPU_WRITE_MEM_UNCACHED`, attivo per default; sostituisce le due variabili vecchie) |

**Correzioni Mesa obbligatorie** (già in §5b, ripetute perché sono la
condizione per entrare nella fase c): fix `FADD32I` + `.SAT` in NAK
(correttezza: il "mare nero"), ZCULL sul percorso Vulkan (geometria dal kernel,
contesto legato al canale 3D, depth senza `TRANSFER_DST`), latenze di
scheduling a 200 cicli e `peephole_select` a 8 (con `NVK_SHADER_STATS=1` per
controllare che la memoria locale resti a 0), e revisione di NAK alzata nella
chiave della cache a ogni modifica. UnleashedRecomp-NX ha portato le stesse
modifiche su mesa-switch 26.2.2 (10 §5 e il loro `SWITCH-MESA.md`): è un
secondo punto di partenza per costruirla.

## 13. LTO e PGO di switch-game: preparati, da attivare solo quando il gioco gira

Non attivati: servono un gioco che gira (PGO) e misure A/B (LTO, `-O2`). Da
10 §1–2 e 08 §4.

**Prerequisiti:**

1. `XXH_FORCE_MEMORY_ACCESS 0` prima di LTO (nfsmw-nx: xxHash con accesso a 1 e
   LTO/PGO ha rotto le chiavi delle texture); `-fno-strict-aliasing` resta.
2. Controllo che `_start` resti all'offset 0 dell'ELF (nfsmw-nx: quattro build
   non partivano).
3. RAM: il link LTO di tutto il codice generato; loro con 2 job entrano in
   16 GB. Nessun'altra build in corso (09 §7).

**LTO** (`tools/switch-game/CMakeLists.txt`, opzione da aggiungere, default OFF):

```cmake
option(SWITCH_GAME_LTO "LTO for switch-game (docs 06 section 13)" OFF)
set(SWITCH_GAME_LTO_JOBS 2 CACHE STRING "LTRANS jobs")
if(SWITCH_GAME_LTO)
  target_compile_options(switch-game PRIVATE -flto -fno-fat-lto-objects)
  target_link_options(switch-game PRIVATE
      -flto=${SWITCH_GAME_LTO_JOBS} -flto-partition=balanced)
  # anche le librerie dell'SDK con -flto, o l'LTO si ferma al confine dell'archivio
endif()
```

A/B nella stessa sessione, con le stesse cvar.

**`-O2` e `-fipa-pta`**: due opzioni separate (`SWITCH_GAME_O2`,
`SWITCH_GAME_IPA_PTA`), ciascuna misurata da sola; `-fipa-pta` solo con LTO.
`-fomit-frame-pointer` e `-fno-math-errno` sui file generati sono indipendenti
da LTO, provabili subito dopo il primo gioco che gira (10 §1).

**PGO**, stessa cartella di build per le due fasi:

1. `SWITCH_GAME_PGO=generate` (senza LTO):
   `-fprofile-generate=<cartella pgo sul PC> -fprofile-prefix-path=<cartella di build> -fprofile-update=single -fno-profile-values`;
   link `-fprofile-generate -Wl,--wrap=fopen`. Un `pgo_switch.cpp` (da
   UnleashedRecomp-NX, che l'ha preso da nfsmw-nx) ridirige i `.gcda` su
   `sdmc:/switch/theft4/pgo/` e fa `__gcov_dump()` + `__gcov_reset()` ogni 3
   minuti da un thread a 0x3B.
2. Giocare a lungo, su tutto il gioco (menu, città, missioni, cutscene, guida).
3. Copiare `sdmc:/switch/theft4/pgo/*.gcda` nella cartella pgo del PC.
4. `SWITCH_GAME_PGO=use` con LTO:
   `-fprofile-use=<cartella> -fprofile-prefix-path=... -fprofile-partial-training -fno-profile-values -fprofile-correction -Wno-missing-profile -Wno-error=coverage-mismatch`.
5. Contare nel log di build i `-Wcoverage-mismatch`: raccogliere e usare con
   lo **stesso `-O`** (UnleashedRecomp-NX: profilo da `-O3` su build `-O2`,
   solo 87 % delle funzioni col profilo). Ogni rigenerazione del codice o
   cambio degli hook invalida il profilo.

`-fno-profile-values` è obbligatorio con `-mtp=soft`; `-fprofile-update=single`
perché l'A57 non ha atomiche veloci (e da lì i contatori negativi che
`-fprofile-correction` accetta).

**Funzioni calde** (dopo il primo profilo CPU): le 300 più calde con
`__attribute__((hot))` in `.text.hot`, o un file di ordinamento del linker come
nfsmw-nx.

## 14. Prima fase su PC: `tools/pc-eu-base` (2026-09-30)

nfsmw-nx ha fatto girare il gioco su PC prima della Switch, per separare gli
errori della traduzione da quelli della piattaforma
(`porting-another-game.md`). Qui lo stesso codice generato di switch-game
(`config-eu-base`, eseguibile europeo di base, nessun hook, nessuna TU) gira
su Windows con la grafica di ReXGlue.

### Il progetto

- **Perché un progetto nuovo.** I preset desktop di Theft4 (`x64-Clang-*` nel
  `CMakePresets.json` della radice) costruiscono `LibertyRecomp`, cioè il
  percorso XenonRecomp/plume, e `gta4-recomp` costruisce un'applicazione solo su
  macOS. `tools/pc-eu-base` è un `ReXApp` minimo sopra l'SDK, con le impostazioni
  desktop di Theft4 per ReXGlue (`glue/CMakeLists.txt`: Vulkan sì, D3D12 no,
  FidelityFX no), il plugin `xenos` (GPU imitata), e i flag del preset Windows
  dell'SDK (clang di Visual Studio, `-march=x86-64-v3`, C++23).
- **Build:** `tools/pc-eu-base/build.bat` (vcvars64, CMake e Ninja di Visual
  Studio, `-j3`). Il `.bat` ripulisce il `PATH`: ereditato da MSYS, il
  `python3` di MSYS (che non legge i percorsi `C:/`) veniva trovato dal
  configure di glslang; si usa un Python di Windows indicato da `THEFT4_PYTHON` (vedi `build.bat`). 939 passi; i file
  generati compilano in circa 5 minuti con clang (con GCC per Switch ~25).
- **Due correzioni dell'SDK per costruirlo:** opzione
  `REXGLUE_BUILD_GTA4_NATIVE` (il plugin `gta4-native` richiede la cache degli
  shader di `LibertyRecompLib`, esclusa dal repository) e `dxgi` collegata a
  `rexui` anche senza D3D12 (`presenter.cpp` usa una factory DXGI su Windows).
- **Stessa traccia della Switch:** `REX_TRACE_EXPORTS` attiva su PC la traccia
  delle export di `rex/hook.h`; un battito ogni 10 s scrive, per ogni thread,
  l'export in cui si trova e da quanto. Per lanciarlo:

  ```
  theft4-eu-base.exe --diagnostics=true --diagnostics_categories=logging
      --log_flush_interval=1 --log_level=debug --log_file=<log>
      "--game_data_root=<cartella del gioco europeo, con default.xex>"
  ```

  Senza `--diagnostics=true` il main dell'SDK sopprime ogni riga di log; senza
  `--log_flush_interval` le ultime righe si perdono quando il processo viene
  chiuso.

### Fin dove arriva

| Run | Cosa cambiava | Esito |
|---|---|---|
| 1–2 | codice generato del run 11, con e senza grafica | 12 export (`RtlImageXexHeaderField` … `KeTlsGetValue`, tutte nel CRT), poi **`[FATAL] Call to invalid or unregistered function at guest address 0x829F6F10`** dopo 0,14 s. Con la grafica (Vulkan su Intel HD 630) identico |
| 3–7 | un indirizzo dichiarato per run (`gta4_eu_base_gaps.toml`) | `829F6F30`, `…6F50`, `…6F70`, `829F6FD0`, `829F7090`: una fila di funzioni da 32 byte raggiunte solo da una tabella di puntatori, 32 byte l'una dopo l'altra (il caso "più funzioni in un vuoto" di nfsmw-nx). Un indirizzo ogni 6 minuti: troppo lento |
| 8 | **653 vuoti dichiarati in blocco** (`gta4_eu_base_gaps_bulk.toml`, sotto) | passa l'avvio del CRT: 76 export, thread creati (`ExCreateThread`), file, GPU (`VdInitializeRingBuffer`, `VdSetGraphicsInterruptCallback`, EDRAM), utente (`XamUserGetSigninState`), poi il thread principale resta **dentro `XAudioRegisterRenderDriverClient`** con il lock globale del kernel preso |
| 9–10 | driver audio creato fuori dal lock globale | il lock è libero, il gioco **presenta fotogrammi 1280x720** (~59 al secondo), ma il thread principale resta nel driver audio di SDL |
| 11 | senza sistema audio (come switch-game) | **97 export, 1,55 milioni di chiamate in 90 s, circa 25 thread, 5.381 fotogrammi presentati, nessun errore fatale**; il thread principale alterna `KeDelayExecutionThread` e lavoro, gli altri aspettano eventi |

Ultimo stato (run 11, 90 s): mancano i file `game:\xbox360\audio\config\*`
(`sounds.dat15`, `game.dat16`, `curves.dat12`, `categories.dat15`,
`speech.dat`, `engineSettings.xml`, `driverSettings.xml`): la copia del disco
su D: ha solo `xbox360/audio/sfx`. Mancano anche i dispositivi `cache:` e
`cache1:` (partizioni di cache della 360). Sono limiti dei dati e del montaggio,
non della traduzione; il prossimo passo è ricopiare la cartella audio completa
dal disco e montare le due cache (come `NullDevice` o su una cartella).

### Dichiarare in blocco i pezzi non raggiunti

`tools/switch-codegen/gaps.py` (da `tools/huecos.py` di nfsmw-nx) trova i
vuoti fra la fine calcolata di una funzione (inizio + 4 × commenti di
istruzione) e l'inizio della successiva: 19.194 vuoti, 18.032 da 4 byte
(riempimento), 1.160 da almeno 8 byte. Dichiararli tutti rompe il codice: molti
sono code della funzione precedente. Filtri, ciascuno verificato rigenerando:

1. **rami non risolti** (`Unresolved conditional branch/b target`): 35 vuoti
   bersaglio di un ramo della funzione precedente e 158 che saltano all'indietro
   dentro di essa (le "continuazioni" di nfsmw-nx): tolti;
2. **`read_before_write.py`**: 313 vuoti leggevano registri mai scritti, cioè
   frammenti in cui la funzione precedente "cade" (con i registri come locali
   riceverebbero zeri): tolti;
3. **dichiarato ma non emesso**: `0x82A0238C` (4.212 byte, dati): tolto.

Risultato: 653 funzioni, 0 rami non risolti, nessun nuovo frammento
(`read_before_write.py` riporta solo il falso positivo noto `0x829A88C0`),
124.291 chiamate dirette su 124.299. La prima delle funzioni che servivano,
`0x829F6F10`, è fra queste.

### Due problemi della piattaforma PC trovati e corretti

- **Il lock globale del kernel negli strumenti di diagnosi.** La tabella degli
  oggetti è protetta dal lock globale (lo stesso che modella IRQL alzato e spin
  lock); un thread guest può tenerlo a lungo. Il battito del PC si fermava lì
  dopo la prima riga. Su Switch la stessa chiamata avviene dal thread
  dell'interfaccia e avrebbe bloccato anche il +. Ora `TryGetObjectsByType`
  (200 ms), e senza la lista degli XThread l'istantanea usa le tracce delle
  export.
- **Il driver audio creato sotto il lock globale.** `AudioSystem::RegisterClient`
  creava il driver host tenendo il lock globale; SDL prende lock suoi e chiede
  lavoro al thread principale. Ora lo slot si prenota sotto il lock, il driver
  si crea fuori, e si installa di nuovo sotto il lock. Il driver SDL non chiede
  più al thread principale se il sottosistema audio è già attivo. Sul PC di
  prova il thread resta comunque fermo nel driver SDL: da indagare con un
  debugger, non blocca il confronto (senza audio PC e Switch rispondono
  uguale).

### Confronto con la Switch

Il criterio: il PC deve arrivare almeno dove arriva la Switch; se si ferma
prima o nello stesso punto, il problema è nella traduzione.

- **Con il codice del run 11 il PC si fermava in 0,14 s su `0x829F6F10`
  (traduzione).** La Switch del run 11 **non ci è arrivata**: il `REX_FATAL`
  avrebbe scritto una riga CRITICAL e chiamato `abort` (registrato dal
  wrapper), e nel `game.log` non c'è né l'una né l'altro. Quindi la Switch si
  ferma **prima**, dentro le prime 12 export o prima ancora: lì il problema è
  della piattaforma, e il run 12 dice dove (la sequenza delle prime chiamate
  del PC è l'elenco da confrontare: `RtlImageXexHeaderField`,
  `NtAllocateVirtualMemory`, `KeGetCurrentProcessType`,
  `RtlInitializeCriticalSection`, `RtlEnterCriticalSection`,
  `RtlLeaveCriticalSection`, `XexCheckExecutablePrivilege`, `KeTlsAlloc`,
  `KeTlsSetValue`, `MmAllocatePhysicalMemoryEx` (130 ms sul PC: la grande
  allocazione fisica), `NtCreateSemaphore`, `KeTlsGetValue`).
- **Con le dichiarazioni dei vuoti il PC arriva a far girare il gioco** (thread,
  GPU, fotogrammi). La NRO del run 12 è ricompilata con lo stesso codice, quindi
  su Switch non si fermerà più su `0x829F6F10`.

## 15. Run 12: il gioco gira per sempre in `while (mftb == 0)` (2026-09-30)

`out/console-runs/run12/`, NRO del commit `38ff073`.

### Dove si ferma

- 11 export (le stesse del PC, nello stesso ordine e dagli stessi LR guest),
  poi il battito ripete per 20 minuti "11 distinct, 73 calls, 216 blocks of
  384": il thread del gioco non chiama più il kernel.
- Istantanee (5, 20, 60 s … al +), con `where.py` sull'ELF di `run12-build`:
  - `F8000018` (thread principale del gioco): "in guest code", ultima export
    `RtlLeaveCriticalSection`, catena `xstart → sub_829A7D78 → sub_829E9CC8 →
    rex::chrono::UpdateGuestClock`;
  - `F8000004` (Kernel Host Tasks): in attesa sulla sua coda, normale.
- `sub_829E9CC8` è `do { r11 = mftb; } while ((uint32_t)r11 == 0);`.
- `tools/compare_export_traces.py` (PC di riferimento contro Switch): prima
  divergenza `KeTlsGetValue` (#12 sul PC), che la Switch non chiama mai.

### Causa

`clock_posix.cpp` leggeva il tick host con `clock_gettime(CLOCK_MONOTONIC_RAW)`.
Il `__syscall_clock_gettime` di libnx accetta solo `CLOCK_REALTIME` (1) e
`CLOCK_MONOTONIC` (4) (verificato nel disassemblato): con 5 restituisce
`EINVAL` e non tocca il `timespec`. Gli `assert_zero` sono spenti in Release,
quindi il tick host non avanzava e il time base guest restava 0. Sul PC
(`QueryPerformanceCounter`) il problema non esiste: è un errore della
piattaforma, non della traduzione.

**Correzione (da verificare):** su NX il clock host è il contatore di sistema
ARM (`armGetSystemTick`, `armGetSystemTickFreq`, 19,2 MHz). Il passo 5 dello
smoke controlla che tick host e time base guest (50 MHz) avanzino su 20 ms.

### Crash alla chiusura

`game-crash.log` vuoto, report di Atmosphère non copiato. Dal `game.log`: al +
`GameMain` ritornava e il `unique_ptr<Runtime>` veniva distrutto **prima** di
`svcExitProcess` ("Host task worker stopped", poi "guest memory at exit: 0
blocks mapped", nessuna istantanea all'uscita), mentre il thread del gioco
girava ancora. **Correzione (da verificare):** il runtime non viene mai
distrutto (`runtime.release()`); `svcExitProcess` chiude tutto insieme.

### Smoke

SMOKE OK; T10: 0 discrepanze nei registri (x0–x28, q8–q15) su 3 × 10.000
fault, 81 µs per fault.

### Altro in questo giro (da verificare)

- `cache:` e `cache1:` montati su cartelle (`Runtime::MountCacheDevices`):
  Switch `sdmc:/switch/theft4/cache` e `cache1`, PC `xbox-cache*` nella
  cartella utente. La cache del runtime su Switch passa a `runtime-cache`.
- `xbox360\audio\config` non è nell'ISO europea (`extract-xiso -l`): non è un
  errore dell'estrazione. La configurazione sta in `audio.rpf` (782 KB, radice);
  le aperture fallite sono tentativi su file sciolti.
- Ogni export registra anche l'esito della prima chiamata ("export first
  result"), su Switch e PC.
- **File che il gioco apre sul PC** (log dei percorsi risolti, solo build PC,
  90 s): `default.xex`, `audio.rpf`, `common.rpf`, `xbox360.rpf`, e le radici di
  `cache:` e `cache1:`. Sulla SD, in `sdmc:/switch/theft4/game/`, servono
  almeno questi quattro file; il run 12 conferma solo `default.xex` (non è
  arrivato oltre).
- **Cache sul PC (provato):** il gioco crea `valid.txt` in `cache:` e
  `cache1:`; un secondo avvio con i file già presenti arriva allo stesso punto
  (97 export, fotogrammi presentati), senza errori. Sulla Switch da verificare.

## 16. Per dopo (quando il gioco gira)

- **Accessi guest non volatili e atomiche rilassate.** Il generatore emette
  `REX_LOAD_*`/`REX_STORE_*` con `volatile` e `stwcx.`/`stdcx.` con
  `__sync_bool_compare_and_swap` (barriera piena), come XenonRecomp prima delle
  modifiche di UnleashedRecomp-NX (10 §1). Toglierli è un'ottimizzazione che
  cambia la correttezza (ordinamento della memoria visto dal gioco): va fatta a
  gioco avviabile, ciascuna con il suo interruttore e un test fisso.
- **LZX nativo.** `gta4_lzx_hooks.cpp` aggancia il decoder LZX persistente del
  gioco (`sub_82A21BF0` e vicine, indirizzi della v8 USA). Gli indirizzi vanno
  ritrovati sull'eseguibile europeo, poi il modo `verify` sulla console prima di
  `host` (10 §3).
- **Filmati Bink** (`xbox360/movies/*.bik`): se il decoder ricompilato è troppo
  lento, dare i byte a FFmpeg (decoder `bink` e `binkaudio`) attraverso le
  callback di lettura del gioco, come nfsmw-nx fa per WMV3 (08 §9).

## 17. Run 13: il gioco si ferma alla prima export grafica; GPU nulla e uscita (2026-10-01)

`out/console-runs/run13/`, NRO del commit `e8f2d07` (ELF in
`out/console-runs/run13-build/`). Sulla SD: `default.xex`, `audio.rpf`,
`common.rpf`, `xbox360.rpf`.

### Dove si ferma

- 37 export distinte, 2702 chiamate; poi
  `STOP at the first function missing on Switch: VdSetGraphicsInterruptCallback
  (Xbox export needs a graphics backend)`. È il nostro arresto voluto (§10,
  ultima parte): senza oggetto grafico le `Vd*` che ne hanno bisogno fermano il
  run.
- Istantanee (`where.py` sull'ELF di `run13-build`):
  - `F8000018` (thread principale): `xstart → sub_827D8968 → … → sub_82851EE0 →
    sub_829D0218 → sub_829DF3F0 → sub_829DF308 → VdSetGraphicsInterruptCallback
    → StopAtMissingFunction` (parcheggiato in `svcSleepThread`);
  - `F8000C6C`, `F8000C70` (creati con `ExCreateThread`): in
    `NtWaitForSingleObjectEx` da `sub_827DAC88 → sub_829A2330`, in attesa di
    lavoro, come il T1 del PC;
  - `F8000004` (Kernel Host Tasks): in attesa sulla sua coda.
- Il clock gira: il thread principale ha superato `sub_829E9CC8` (`while (mftb
  == 0)`, run 12) ed è arrivato 25 export più avanti. Il `game.log` non aveva una
  riga del clock; il run 14 la scrive a ogni battito (`clock: host tick …, guest
  time base …`). Smoke: `step 5 clock: … host 20.00 ms (384096 ticks), guest time
  base 20.00 ms (1000250 ticks at 50 MHz) -> ok`.

### Confronto con il PC (`compare_export_traces.py`, PC `out/pc-runs/run13c`)

- Stesso ordine per le prime 29 export, stessi LR guest per tutte quelle comuni.
- **Prima divergenza**: il PC chiama `NtQueryDirectoryFile` (#30, LR
  `829A9484`) dopo `NtOpenFile`, la Switch no. L'ultima export comune,
  `NtOpenFile` di `game:\`, dà `00000000` sul PC e `C000000F` sulla Switch
  (`[NtCreateFile] FAILED: path='game:\'`). Causa: `HostPathEntry::Open` apre
  con `open()`, e quello di libnx passa da `fsFsOpenFile`, che rifiuta le
  cartelle; Linux e Windows aprono le cartelle. Il gioco va avanti lo stesso
  (legge `common.rpf` e il resto), ma senza l'elenco della radice.
  **Correzione (da verificare):** su NX una cartella dà un handle senza
  descrittore (`NxDirectoryHandle`, `filesystem_posix.cpp`), le cui letture e
  scritture falliscono come su PC; l'elenco passa dall'entry del VFS.
- **Esiti diversi**: `KeSetBasePriorityThread` dà `0x3B` sulla Switch e `0` sul
  PC: `XThread::QueryPriority` restituiva la priorità Horizon del thread host
  (fissa a 0x3B per i thread guest). **Correzione (da verificare):** su NX
  restituisce il livello che il PC riporta per l'ultimo incremento chiesto
  (scala Win32, −2…2). Gli altri esiti diversi sono indirizzi e handle.
- Dopo la divergenza il PC prosegue con le `Vd*` (`VdSetGraphicsInterruptCallback`,
  `VdInitializeRingBuffer`, `VdEnableRingBufferRPtrWriteBack`, …, `VdSwap`) e poi
  input, audio, file: la Switch si ferma sulla prima.

### GPU nulla (run 14, da verificare)

La proposta del §10, implementata: `rex/system/null_graphics_system.h`
(`src/system/null_graphics_system.cpp`, in `rexruntime`, solo header di
`rex/graphics`). switch-game la passa al runtime (`RuntimeConfig.graphics`) con
la cvar `nx_null_gpu` (vera per default; falsa = arresto come nel run 13).

- Registri GPU in MMIO a `0x7FC80000` come `GraphicsSystem` (letture fisse di
  `RB_EDRAM_TIMING`, `D1MODE_V_COUNTER`, stato interrupt = vblank,
  `D1MODE_VIEWPORT_SIZE`; `CP_RB_RPTR` = puntatore di lettura).
- `CP_RB_WPTR` sveglia il thread `GPU Commands (null)` (XHostThread): consuma i
  pacchetti PM4 fino al WPTR e scrive l'RPTR nel writeback.
- Eseguiti: registri (con writeback di `SCRATCH_REG*` verso `SCRATCH_ADDR` e
  `COHER_STATUS_HOST`), `MEM_WRITE`, `COND_WRITE`, `REG_TO_MEM`, `REG_RMW`,
  `EVENT_WRITE`/`_SHD`/`_EXT`/`_ZPD` (occlusione: 1000 campioni come la cvar del
  command processor), `WAIT_REG_MEM` (dopo 1 s una riga WARN e il battito lo
  segnala), `INDIRECT_BUFFER`, `INTERRUPT`, costanti e bin. Saltati: draw,
  `IM_LOAD`, stati, viz query. `XE_SWAP` conta i fotogrammi.
- `GPU VSync (null)`: interrupt di vblank alla frequenza del modo video (attende
  che la frequenza del time base sia nota; niente raffiche se resta indietro).
- Il battito scrive `null GPU: … WPTR writes, ring read / write, packets, draws
  skipped, swaps, vblanks, PM4 interrupts`.

### Crash alla chiusura

- **Nessun report di Atmosphère** e `game-crash.log` vuoto: il gestore dei
  fault non è scattato, quindi non c'è un fault nel nostro processo da
  decodificare. Nel `game.log` le ultime righe sono l'istantanea all'uscita, i
  conteggi, la memoria e `TERMINATION: svcExitProcess(0) called from LR …+0x66cc`,
  scritta dal wrapper subito prima di `__real_svcExitProcess`: il processo
  arriva a `svcExitProcess`. Quello che si vede dopo il + avviene quindi dentro
  o dopo `svcExitProcess` e non è un'eccezione del processo (che produrrebbe il
  report).
- Ipotesi (non verificata): la chiusura di un'applicazione con
  `svcExitProcess` senza la chiusura dei servizi di libnx (`__appExit`: applet,
  hid, fs, sm) viene trattata dal sistema come anomala. Un'uscita normale di un
  NRO chiude i servizi e torna a hbloader; noi non torniamo al loader (§10).
  Serve il codice d'errore mostrato dalla console per confermarlo.
- **Uscita del run 14 (da verificare)**, `ExitGame` in `main.cpp` e
  `tools/switch-common/nx_exit.{h,cpp}`:
  1. al + (o alla richiesta di uscita del sistema), prima di tutto: elenco degli
     XThread (tabella degli oggetti, attesa massima 200 ms) e dei thread delle
     tracce, e **pausa di ciascuno** con `svcSetThreadActivity(Paused)`; per
     ognuno si tiene il result code e si rilegge il contesto
     (`svcGetThreadContext3`) come prova che è fermo;
  2. il riepilogo (righe `exit pause: … rc …`, istantanea, export, memoria,
     clock, GPU nulla) lo scrive un thread creato all'avvio, con scadenza di
     5 s: un thread in pausa può tenere un lock che serve (malloc, log, spdlog,
     console). Da qui il log aspetta il proprio mutex al massimo 200 ms;
  3. con `nx_exit_close_services` (vero per default) lo stesso thread chiude la
     console e i servizi (`__appExit`), come un'uscita normale;
  4. `svcExitProcess` comunque; se il riepilogo non finisce in 5 s una riga va
     in `game-crash.log` (descrittore già aperto, niente allocazioni).

### Smoke (run 14)

Passo 6, la stessa sequenza (stesso `nx_exit.cpp`) su cinque thread che
lavorano a 0x3B: calcolo, mutex, malloc/free, letture dalla SD, uno che tiene un
mutex. Controlla: ogni pausa riuscita, contatori fermi per 100 ms, un lavoro
bloccato sul mutex tenuto abbandonato alla scadenza (300 ms), ripresa. Al + lo
smoke esce come il gioco, con i cinque thread al lavoro: pausa, riepilogo,
`__appExit`, `svcExitProcess`. Se anche lo smoke mostra l'errore all'uscita, la
causa è il modo di uscire, non i thread del gioco.

## 18. Run 14: il gioco gira a 60 fotogrammi con la GPU nulla; uscita in quattro modi (2026-10-01)

`out/console-runs/run14/`, NRO del commit `60212c5`.

### Il gioco con la GPU nulla

- **58 export nuove** dopo `VdSetGraphicsInterruptCallback`, nello stesso ordine
  del PC: le `Vd*` (ring a `1FCA2000`, 32 KiB, writeback a `1FCA103C`, callback
  `829D7318`), `VdSwap`, il profilo (`XamUserGet*`), audio
  (`XAudioRegisterRenderDriverClient` → `C0000002`, come sul PC), input, file
  (`NtWriteFile`, `IoDismountVolumeByFileHandle`, …), `RtlRaiseException`.
  Totale 95 export distinte, 25 XThread (il PC: 97 e 25).
- **Prima divergenza** (`compare_export_traces.py` contro `pc-runs/run13c`):
  `XamInputGetState` dà `0x48F` (`ERROR_DEVICE_NOT_CONNECTED`, nessun sistema di
  input) dove il PC dà `0`; il PC chiama poi `XamInputGetCapabilities` e
  `XamInputSetState`, la Switch no. Nessun'altra differenza.
- **Non è fermo**: da 30 s in poi il battito mostra un ciclo stabile, 95 export,
  ~7.500 chiamate al secondo, **60 `VdSwap` al secondo** (600 ogni 10 s), il
  ring consumato fino al WPTR a ogni battito (lettura = scrittura), 1 interrupt
  PM4 per fotogramma, ~510.000 draw saltati ogni 10 s, nessun `WAIT_REG_MEM`
  bloccato, nessun opcode sconosciuto. In 910 s: 54.274 fotogrammi, 55.188
  vblank. Il "battito fermo a 95" era il numero di export distinte.
- **Istantanee** (`where.py`, ELF di `run14-build`), uguali a 60 e a 900 s:
  thread principale `F8000020` in `xstart → sub_8218BEB0 → … → sub_829A20B8 →
  KeDelayExecutionThread` (il ciclo principale che dorme tra un fotogramma e
  l'altro); `F8000CA4` (rendering) in `sub_82852960 → sub_829CFCE8 →
  sub_829D5338` (presentazione); gli altri in attesa di lavoro.
- **Clock**: 91 intervalli tra battiti, rapporto time base guest / tick host
  0,99997–0,99999; il tempo guest avanza regolare (le differenze tra battiti,
  9,98–11,1 s, sono il ritardo del battito, non del clock).
- **Ipotesi (da verificare)**: il gioco è su una schermata che aspetta un
  controller o START. **Correzione per il run 15**: il controller della Switch
  diventa il pad 360 dell'utente 0 (`tools/switch-game/nx_input.{h,cpp}`, cvar
  `nx_input`, vera per default). Pulsanti per posizione (Switch B = A, A = B,
  Y = X, X = Y), + = START, − = BACK, ZL/ZR = grilletti, levette. Il battito
  scrive `input: … pad states published, … XamInputGetState reads`.

### Chiusura

Run 14, modo dell'epoca (= B): nel gioco la sequenza arriva a `exit: summary
written; next __appExit` (25 thread fermati in 246 µs; il 26°, dalle tracce, ha
un handle non più valido, `0xE401`); poi la console mostra "The software was
closed because an error occurred", senza report. Lo smoke, che prima usciva
pulito tornando da `main`, fa lo stesso con la nuova sequenza: la causa è la
sequenza d'uscita, non i thread del gioco.

**libnx 4.12 (dal disassemblato di `libnx.a`):**

- `__libnx_exit(rc)`: `__appExit()`, poi `__nx_exit(0, envGetExitFuncPtr())`
  (il codice d'uscita passato è sempre 0). `__nx_exit` (`switch_crt0`)
  ripristina lo stack iniziale e salta alla funzione d'uscita del loader: sotto
  hbloader si torna a hbloader, che ricarica hbmenu. Non chiama
  `svcExitProcess` se c'è un loader.
- `__appExit()`: `userAppExit` (se c'è), `__nx_win_exit` (se c'è),
  `fsdevUnmountAll`, `fsExit`, `timeExit`, `hidExit`, `appletExit`, `smExit`.
  Non chiude la console (`consoleExit` è a carico del programma).
- `crt0` entra in `main` con LR = `exit`: tornare da `main` = `exit(rc)` →
  handler `atexit` → `_exit` → `__libnx_exit`.
- `svcExitProcess` è chiamata da libnx in `env.o` (come funzione d'uscita
  predefinita senza loader, via GOT), in `applet.o` (`_appletExitProcess`, dopo
  `appletExit`, per la richiesta d'uscita del sistema) e in `fatal.o`. Il
  nostro `--wrap=svcExitProcess` intercetta **anche queste** (sono riferimenti
  non definiti nei loro oggetti), non quelle interne a `svc.o` (non ce ne sono).
  Il wrapper chiama `__real_svcExitProcess` una volta sola e la chiamata non
  ritorna: niente ciclo né doppia uscita. Unico rischio trovato: il wrapper
  scriveva nel log (malloc, spdlog) con i thread in pausa e poteva bloccarsi;
  ora con i thread in pausa scrive solo la riga del passo.
- Differenza con un'uscita normale di un NRO: noi non torniamo mai al loader
  (svcExitProcess chiude il processo di hbloader stesso).

**Run 15: bisezione sul campo**, stesso NRO, scelta al momento dell'uscita
(`tools/switch-common/nx_exit.{h,cpp}`):

| Tasto | Modo | Cosa fa |
|---|---|---|
| + | A | `exit(0)`, cioè il ritorno da `main` (uscita standard di libnx), con i thread ancora vivi |
| − | B | pausa di tutti i thread, riepilogo, `__appExit`, `svcExitProcess` (il run 14) |
| X | C | come B senza `__appExit` |
| Y | D | i thread guest escono da soli alla prossima export (`svcExitThread` dall'aggancio `SetExportEntryHook`), la GPU nulla si ferma, join per 2 s, poi come A |

Nel gioco il menu si apre tenendo premuto + per 1 s (+ breve è START); la cvar
`nx_exit_mode` (A–D) salta il menu; B annulla. Nello smoke il menu si apre con
+, con i cinque thread al lavoro (D: flag di stop e join). Ogni passo va in
`game-exit.log` / `smoke-exit.log` ("exit mode X: step N: …", write + fsync su
un descrittore aperto all'avvio) e, finché nessun thread è in pausa, anche nel
log principale. Lo smoke cancella `smoke-exit-scratch.bin` all'avvio e alla fine
del passo 6.

**Regola**: se nessuno dei quattro modi esce pulito, la chiusura non è più una
priorità: si esce con HOME.

## 19. Run 15: uscita decisa, il gioco aspetta il suono del primo schermo (2026-10-02)

`out/console-runs/run15/`, NRO del commit `1185ddb` (ELF in
`out/console-runs/run15-build/`).

### Chiusura

Modo A (+) e modo D (Y) escono puliti nello smoke e nel gioco; B (−) e C (X)
danno "The software was closed because an error occurred", senza report, in
tutti e due. Nel `game-exit.log` dell'ultimo run del gioco: A, passi 1–5 fino a
`_exit`; nello `smoke-exit.log`: D, 5 thread su 5 uniti, poi `atexit`. La
causa più probabile è `svcExitProcess`, non isolata dalla pausa dei thread (B
e C fanno tutti e due l'una e l'altra). Regola in 09 §9. **Run 16 (da
verificare):** l'uscita è A per default, D con `nx_exit_mode = D`; niente
menu, + tenuto premuto per 1 s esce; la pausa dei thread e `svcExitProcess`
non sono più sul percorso d'uscita; il passo 6 dello smoke (pausa) resta
dietro la cvar `nx_smoke_pause_test` (spenta). Da qui la chiusura non è più
una priorità.

### Il controller

- **Due letture in tutto il run**: il battito dice `0 XamInputGetState reads`
  a 10 s e `2` da 20 s fino all'uscita (270 s), con 14.482 stati del pad
  pubblicati (~54 al secondo dal thread dell'interfaccia).
- La prima `XamInputGetState` dà **`0`** (nel run 14 `0x48F`), e la Switch
  chiama poi `XamInputGetCapabilities` (`0`) e `XamInputSetState` come il PC.
  Unico esito diverso dal PC: `XamInputSetState` dà `0` sulla Switch e `0x48F`
  sul PC (il pad del PC non ha vibrazione); innocuo.
- Il messaggio del run non dice quali tasti sono stati premuti (il segnaposto
  "[scrivi quali tasti…]" è rimasto vuoto) e il log non registra i tasti. Il
  log dice però che **dopo i 20 s non c'è nessuna export nuova** (97 distinte
  da 30 s a 270 s), nessun file nuovo, nessun thread nuovo (25 XThread a 60 e
  a 180 s) e nessuna lettura del pad: qualunque pressione non ha avuto effetto,
  perché il gioco non legge il pad.

### Confronto lungo con il PC (`out/pc-runs/run16/pc-full.log`, 5 min 20 s)

PC ricompilato (`tools/pc-eu-base`) con il conteggio di ogni export ogni 60 s.
`compare_export_traces.py` contro il `game.log` del run 15: 97 export per
tutti e due, la Switch chiama tutte quelle del PC, stessi LR guest, stesso
ordine per le prime 37 (poi cambia solo l'intreccio dei thread), un solo esito
diverso (`XamInputSetState`, sopra).

**Il PC si ferma nello stesso ciclo**: 97 export distinte già al battito dei
10 s e nessuna nuova fino a 320 s. Conteggi per minuto, uguali dal primo al
quinto:

| Export | Per minuto (PC) | Lettura |
|---|---|---|
| `VdSwap`, `VdGetSystemCommandBuffer`, `VdRetrainEDRAM`, `KeQueryPerformanceFrequency` | 3.600 | 60 fotogrammi al secondo |
| `XGetVideoMode` | 10.800 | 3 per fotogramma |
| `XNotifyGetNext` | ~3.650 | il gioco controlla le notifiche a ogni fotogramma |
| `KeDelayExecutionThread` | ~45.400 | ~750 `Sleep` al secondo |
| `RtlEnter/LeaveCriticalSection` | ~97.000 | |
| `NtReadFile` | **0** (fermo a 615) | nessun file letto dopo l'avvio |
| `ExCreateThread` | **0** (fermo a 22) | nessun thread nuovo |
| `XamInputGetState` | **0** (fermo a 8) | il pad non viene più letto |

### Cosa aspetta il gioco

Istantanee della Switch (`where.py` sull'ELF del run 15) e, nuova, la **pila
guest** di ogni thread fermo in un'export (catena dei frame PowerPC da `r1`:
`0(r1)` è l'`r1` del chiamante, il prologo salva LR a −8 di quello),
aggiunta al battito del PC (`out/pc-runs/run16b`):

- thread principale `F8000020`: `KeDelayExecutionThread` ← `829A9624` ←
  **`821200F4`** ← `main` (`sub_8218BEB0`) ← `xstart`. Il terzo indirizzo
  letto è `8218BECC` ed è vecchio: `sub_821200D0` salva LR con `__savegprlr`
  (`bl 0x8298eac8`), che il codice generato fa da sé senza scrivere lo slot
  guest; il chiamante vero è `main` a `8218BEF8`. `sub_821200D0` (il "run" del
  gioco, dopo `sub_82120000` = init) comincia con
  **`while (sub_82124490()) Sleep(1);`**;
- `sub_82124490` = "byte `83137BB7` e byte `83137BC9` entrambi a 1";
  `sub_82124080` (chiamata dall'init con r3 = 1) li mette a 1;
- li azzera il **sequenziatore degli schermi d'avvio** `sub_821238D0`
  (chiamato in coda da `sub_821235B0`, che disegna lo schermo; gira sul thread
  di rendering `F8000CA4`, quello che presenta): tabella a `83137D18`, 400
  byte per voce, indice a `83137BC0`, tempo accumulato da `mftb` a
  `83137BB8`;
- riga aggiunta al battito del PC (`out/pc-runs/run16c`): **indice 0 per
  sempre**, voce 0 = durata `0x1770` (6000 ms) e tipo **1**, tempo a 9,6 /
  19,6 / 29,6 / 39,7 s: il tempo scade ma la voce non avanza;
- per una voce di tipo 1, scaduto il tempo, si avanza solo se la parola a
  `82B11188` non è 0, oppure se `sub_822FA498(82D09E00)` è vera, cioè se il
  **suono** dell'oggetto `82D09E00` (creato e avviato da `sub_822FAD50`
  attraverso il motore audio del gioco) ha **stato 2**. La parola a `82B11188`
  è una variabile di stato del gioco (scritta da `sub_82121C10` e da altre
  quattro funzioni), non un interruttore di debug.

**Conclusione (probabile, non provata):** il primo schermo d'avvio (il logo di
6 s) aspetta che il suo suono arrivi allo stato 2. Senza audio il suono non
parte e il gioco resta lì per sempre, sul PC come sulla Switch: il PC gira
senza sistema audio (`pc_audio` spenta) e `XAudioRegisterRenderDriverClient`
dà `C0000002` su tutti e due. Prova sul PC con `--pc_audio=true`
(`out/pc-runs/run16d`): il sistema audio SDL parte, ma il gioco **non trova
`game:\xbox360\audio\config\*`** (`engineSettings.xml`, `curves.dat12`,
`sounds.dat15`, `game.dat16`, `categories.dat15`, `speech.dat`,
`driverSettings.xml`: già mancanti nel run 11 del PC, §14) e resta all'indice
0 anche così. Nella copia del gioco usata (sul PC e sulla SD) la cartella
`xbox360/audio` ha solo `sfx`.

**Correzione: non nel codice.** Toccare `82B11188` o i byte del sequenziatore
sarebbe un trucco su variabili di stato del gioco. Servono, in ordine:

1. **i file di `xbox360/audio/config` dal disco** (cartella completa), sulla
   copia del PC e in `sdmc:/switch/theft4/game/xbox360/audio/config/`;
2. provare prima sul PC con `--pc_audio=true`: se l'indice del sequenziatore
   passa a 1, la causa è confermata;
3. sulla Switch un sistema audio: oggi switch-game non ne passa nessuno
   (`config.audio_factory` vuota). L'SDK ha `SwitchAudioDriver` e
   `SwitchAudioSystem` (`src/audio/switch/`, su audren) ma **non sono nel
   `CMakeLists.txt` di `src/audio`**: aggiungerli (sotto `REX_PLATFORM_NX`) e
   passarli da switch-game con una cvar. Da fare dopo il punto 2.

### Strumenti per il run 16 (da verificare)

- **Conteggio di ogni export ogni 60 s** nel battito (switch-game e
  pc-eu-base): i due run si confrontano nel tempo, non solo sulle prime
  chiamate.
- **Pila guest** nelle righe dei thread fermi in un'export (`guest stack …;
  host …`), con l'avvertenza di `__savegprlr` sopra.
- **Riga `boot sequencer:`** nel battito: attivo, "main aspetta", pausa,
  indice, tempo, durata e tipo della voce, parola `82B11188`, oggetto del
  suono (id, puntatore, stato). Indirizzi dell'eseguibile europeo base.

Cosa guardare nel `game.log` del run 16: la riga `boot sequencer` (indice
fermo a 0, tipo 1, `sound 00000000` = suono mai creato; un puntatore con stato
diverso da 2 = suono creato ma fermo), le righe `exports called at N s`, e la
pila guest del thread principale (attesa: `829A9624 821200F4 …`). E il passo 7
dello smoke (11-vulkan-probe.md).

## 20. Piano: profiler dei picchi per fotogramma in switch-game (da fare con la GPU vera)

Da UnleashedRecomp-NX 1.0.0 (`os/switch/cpu_profiler_switch.cpp`,
`os/switch/stall_watch_switch.cpp`, `tools/switch-cpu-profile.py`; 10 §12).
**Non ora**: con la GPU nulla non ci sono fotogrammi veri né picchi da
misurare. Da implementare quando il gioco disegna (fase a o c di §11).

### Come campionano loro

- I thread si **registrano** con un nome (involucro di `threadCreate`); il
  campionatore è un thread proprio che ogni **2 ms** (500 campioni al secondo
  per thread) prende la lista sotto un mutex.
- Per ogni thread legge prima `svcGetInfo(InfoType_ThreadTickCount)`: se il
  contatore non è cambiato dall'ultimo campione il thread **non ha girato** e
  il campione si scarta (conta come "idle"), perché la sua posizione dice solo
  dove è bloccato.
- Se ha girato: `svcSetThreadActivity(Paused)`, `svcGetThreadContext3`, e, se
  il PC è in una SVC, gli **indirizzi di ritorno sullo stack** (LR + 7, da una
  copia dello stack); poi `Runnable`. Tra pausa e ripresa niente allocazioni,
  lock o log. È lo stesso meccanismo della nostra istantanea dei thread (§18).
- Ogni 30 s un report su `stderr.log` **in una sola scrittura** (riga per riga
  su SD costava secondi e teneva il lock di stdio): per thread le righe di
  codice da 16 byte più calde (`+0x…`, offset dall'inizio dell'NRO) e le catene
  delle attese ("wait"). Lo script le nomina con `nm` dell'ELF (`sub_82XXXXXX`
  per le funzioni ricompilate).

### Come isolano i fotogrammi lenti

- `Video::Present` segna **`FrameWorkEnd`** all'inizio (fine del lavoro del
  fotogramma) e **`FrameWorkStart`** alla fine, dal thread del gioco. Un
  fotogramma il cui lavoro supera la soglia (`SwitchSlowFrameProfileMs`) è
  "lento".
- Con la soglia attiva il thread del gioco si campiona **sempre**, anche
  quando aspetta, con la catena dei chiamanti; i campioni vanno in un **anello
  degli ultimi 2 s**. Quando il fotogramma lento è finito (il campione più
  recente è oltre la sua fine), i suoi campioni passano alla sezione "main in
  slow frames" del report successivo: dove lavorava e **dove aspettava**.
- Il round 12 ha scoperto così che i picchi (48–80 ms) erano attese: dati in
  caricamento (cicli con `Sleep(5)`), il lock del mixer audio, eventi.
- Il watchdog degli stalli: nessun fotogramma da N s → per ogni thread "ha
  girato?", PC, LR, catena dei frame pointer, indirizzi di ritorno, SVC in cui
  è (`[stall]`); una volta al secondo i fotogrammi oltre 100 ms (`[hitch]`).

### Per switch-game

1. **Registro dei thread**: abbiamo già gli XThread (tabella degli oggetti,
   handle Horizon) e i thread delle tracce delle export; si aggiunge il tick di
   CPU per thread (`InfoType_ThreadTickCount`) per scartare chi non ha girato.
2. **Confini del fotogramma**: `VdSwap` (export) è il punto dove il gioco
   consegna il fotogramma; il thread che la chiama è quello di rendering
   (`F8000CA4` nei run 14 e 15), il thread principale aggiorna la logica.
   Segnare `FrameWorkEnd`/`FrameWorkStart` attorno a `VdSwap` e misurare anche
   il thread principale tra due giri del suo ciclo.
3. **Anello dei campioni** del thread principale e del thread di rendering,
   con pila host (già fatta) e **pila guest** (§19: la catena da `r1` dà
   direttamente le funzioni `sub_82…` del gioco, anche quelle che GCC ha messo
   inline nel codice host).
4. **Report in una scrittura**, per thread, e uno script (estensione di
   `where.py`) che somma per funzione.
5. **Scene critiche di GTA IV**, una per una, ognuna in un punto fisso e da
   un salvataggio fisso: **esplosioni** (auto che esplode, granate:
   particelle, fisica, audio); **incidenti** (scontri fra più auto: fisica
   Euphoria/ragdoll, danni ai modelli); **folla** (incroci di Algonquin di
   giorno: pedoni, IA, animazioni); **streaming della città** (guida veloce in
   autostrada o su un ponte: letture da SD, decompressione, caricamento di
   texture e modelli, priorità dei thread di streaming); **cambio di isola** e
   **caricamento di un salvataggio**.
6. **Watchdog**: nessun `VdSwap` da 1 s → istantanea completa (§18) più la
   riga del sequenziatore o dello stato del gioco; `[hitch]` per i
   fotogrammi oltre 100 ms.

Ogni ottimizzazione che ne esce va provata come fanno loro: un NRO, una cvar
per modifica, confronto A/B nello stesso punto, ELF conservato.

## 21. Run 16: il sequenziatore aspettava il driver audio; audio nullo (2026-10-02)

`out/console-runs/run16/`, NRO del commit `3c50605`.

- **Console:** la sonda Vulkan funziona (`VULKAN OK`, immagine visibile); dopo,
  durante l'attesa del +, un crash senza crash report nella cartella del run
  (11-vulkan-probe.md, "Il crash dopo la sonda"). Il gioco: stesso ciclo a 60
  Hz; la chiusura con + (modo A) **non dà errori** (`game-exit.log` fino a
  `_exit`): la regola di 09 §9 è confermata sul gioco.
- **Disco:** la cartella `xbox360/audio/config` **non esiste** sul disco.

### A.1 I file che il gioco apre

Il run 16 registrava solo i fallimenti di `NtCreateFile` (`[NtCreateFile]
FAILED`). Sono **gli stessi sette sul PC e sulla Switch**, tutti in
`game:\xbox360\audio\config\` (`categories.dat15`, `curves.dat12`,
`driverSettings.xml`, `engineSettings.xml`, `game.dat16`, `sounds.dat15`,
`speech.dat`), cercati **come file dell'host** con `NtCreateFile` sul
percorso `game:\`, **non dentro un RPF** (le voci di un RPF si leggono con
`NtReadFile` sull'handle dell'archivio, senza aprire un percorso). Visto che
la cartella non c'è nemmeno sul disco, sono file sciolti facoltativi che il
gioco prova e non trova anche su una 360: **l'ipotesi "mancano i file di
config" era falsa.** Con l'audio nullo il gioco prova anche
`waveslots.xml` ed `effects.dat11` nella stessa cartella (stesso esito) e va
avanti lo stesso.

**Nuovo (da verificare sulla Switch):** `NtCreateFile`/`NtOpenFile`
(`xboxkrnl_io.cpp`) scrivono `file open #N: <export> '<percorso>' disposition
… options … -> <stato> (action …)` per le prime 200 aperture e poi per ogni
percorso nuovo, su PC e Switch.

### A.2 Perché `XAudioRegisterRenderDriverClient` dava `C0000002`

`xboxkrnl_audio.cpp`: se `REX_KERNEL_STATE()->emulator()->audio_system()` è
nullo l'export ritorna `X_STATUS_NOT_IMPLEMENTED` = `C0000002`. Sul PC
`pc_audio` spenta metteva `config.audio_factory = nullptr`; switch-game non
passava nessuna `audio_factory`. **Audio spento, nient'altro.** Sulla Switch
la classe `AudioSystem` del desktop non è nemmeno compilata
(`REXGLUE_CORE_ONLY` costruisce di `src/audio` solo XMA e handoff).

### A.3 Il sistema audio nullo (da verificare sulla Switch)

`include/rex/audio/null_audio_system.h`, `src/audio/null_audio_system.cpp`
(in `rexaudio`, quindi anche nel build Switch), modellato sulla GPU nulla.
Implementa `system::IAudioSystem` direttamente:

- `RegisterClient` accetta fino a 8 client, avvolge l'argomento del callback
  in memoria guest (come `AudioSystem`) e dà **8 crediti**;
- un `XHostThread` "Audio Worker (null)" ogni **256/48000 s (5,333 ms)**
  "suona" un fotogramma in coda per client, che restituisce un credito; ogni
  credito è una chiamata del callback guest (`FunctionDispatcher::Execute`),
  che sottomette il fotogramma successivo: lo stesso contratto dei semafori di
  `AudioSystem` con un driver che rilascia un'unità per fotogramma suonato;
- `SubmitFrame` conta e scarta;
- possiede un `XmaDecoder`, come `AudioSystem`: le export XMA ci arrivano
  attraverso `audio_system()->xma_decoder()`;
- il lock dei client non è mai tenuto mentre gira codice guest (il callback
  sottomette fotogrammi, e un altro thread guest con un lock guest potrebbe
  farlo nello stesso momento); un secondo lock, tenuto solo intorno al
  callback, fa sì che `UnregisterClient` aspetti un callback in corso.

Cvar `nx_null_audio` (vera per default), definita nel file dell'SDK; la usano
switch-game (`config.audio_factory`) e pc-eu-base quando `pc_audio` è spenta.
Il battito scrive `null audio: N clients, … callbacks, … frames submitted, …
played`. Nessun trucco sulle variabili del gioco.

### A.4 Prova sul PC

| Run (PC) | Cosa cambia | Esito |
|---|---|---|
| `run17` | audio nullo | il client si registra (callback `82172BE8`), file di `audio\sfx\*.rpf`, `XMACreateContext`, poi `[FATAL] Call to invalid or unregistered function at guest address 0x82177920` |
| `run17b` | `0x82177920` dichiarata (dentro `sub_82177910`: il thunk `addi r3,r3,-4; b 0x82177920` salta a +0x10, dove comincia la funzione vera chiamata da una vtable) | **il sequenziatore avanza**: indice 1 a 10 s, indice 5 a 20 s con "main waits on **0**" (il thread principale è libero), indice 6 e "active 0" a 30 s; poi il log si ferma a 30 s |
| `run17c` | pila guest limitata allo stack del thread (dal PCR) | stesso avanzamento, poi `FATAL` a `0x827D5200` (lacuna di 32 byte dopo `827D51E0`, chiamata dal thread principale) a ~27 s: era quasi certamente anche lo stop di `run17b` (riga non scritta in tempo) |
| `run17d` | `0x827D5200` dichiarata | **275 s**: 117 export distinte, 200 milioni di chiamate; il gioco legge il pad a ogni fotogramma, carica la città, poi `FATAL` a `0x82586A60` (lacuna dopo `82586A08`) |
| `run17e` | `0x82586A60` dichiarata | **330 s senza arresti** (ucciso a mano): 117 export, 280 milioni di chiamate, `NtReadFile` da 73.596 a 84.335 negli ultimi tre minuti (continua a caricare la città, lentamente: ~6 fotogrammi al secondo con la GPU emulata del PC); thread principale nel ciclo del gioco (`… 82122A54 82120188 …`, pila guest) |

Il sistema audio nullo va al ritmo giusto: 1.768 callback nei primi ~9,4 s,
~187,5 al secondo = 48000/256; poi un callback e un fotogramma per volta
(`frames submitted` = `callbacks`, `played` 7–8 sotto: gli 8 crediti).

**Cosa ha sbloccato il sequenziatore.** Con l'audio nullo la voce 0 è passata:
nel battito dei 10 s l'indice è già 1, il suono del logo non è più visibile
(`sound 00000000`), quindi la condizione del tipo 1 (suono allo stato 2, 06
§19) è stata soddisfatta mentre il motore audio del gioco girava. Il resto
della sequenza è avanzato: voce 1 = 4000 ms tipo 2, voce 5 = 8000 ms tipo 4
(a 20 s, con il thread principale già libero), voce 6 tipo 0 con il
sequenziatore spento (a 30 s).

**Cosa fa il gioco dopo (run17d, conteggi per minuto):**

- 20 export nuove rispetto al run 16: `XAudioGetSpeakerConfig`,
  `XAudioGetVoiceCategoryVolumeChangeMask`, `XAudioSubmitRenderDriverFrame`,
  `XMACreateContext`, `XMAReleaseContext`, `XamUserReadProfileSettings`,
  `XamUserCreateAchievementEnumerator`, `XamContentCreateEnumerator`,
  `XamCreateEnumeratorHandle`, `XamEnumerate`, `XMsgStartIORequest`,
  `XMsgStartIORequestEx`, `XMsgInProcessCall`, `KeWaitForMultipleObjects`,
  `NtWaitForMultipleObjectsEx`, `NtSetEvent`, `KeResetEvent`,
  `RtlTryEnterCriticalSection`, `ExTerminateThread`, `NtSetInformationFile`;
- `XamInputGetState`: 1.516 a 60 s, poi ~23 al secondo: **ora il gioco legge
  il pad** (prima 8 volte in tutto);
- `NtReadFile` da 615 a 80.010; 325 percorsi diversi aperti; alla fine
  `game:\xbox360\data\maps\east\bronx_*.img` (la città) e, per ognuno, il
  tentativo su `cache:\` (la copia sull'HDD della 360) che fallisce prima
  della lettura dal disco, come previsto;
- `ExCreateThread` 25 (prima 22); `VdSwap` 2.292 nel primo minuto, poi
  ~6 al secondo: sul PC la GPU emulata di ReXGlue rallenta molto ora che il
  gioco disegna davvero.

**Per la Switch:** lo stesso codice (audio nullo, cvar `nx_null_audio`) e il
codice generato con le tre funzioni nuove sono nel run 17 (`run17-build`). Da
verificare sulla console.

### A.5 Piano: audio reale sulla Switch (non implementato)

- **Cosa c'è:** `src/audio/switch/switch_audio_system.cpp` e
  `switch_audio_driver.cpp` (audren di libnx: driver ad alto livello, voci,
  wave buffer doppi, `kChannelSamples` 256, 48 kHz, 6 canali convertiti in
  stereo int16), con `include/rex/audio/switch/*.h`, protetti da
  `REX_PLATFORM_NX`. `SwitchAudioSystem` **deriva da `AudioSystem`** (il
  sistema del desktop con il worker, i semafori dei client e la cvar
  `audio_maxqframes`).
- **Cosa manca nel build:** in `src/audio/CMakeLists.txt` il ramo
  `REXGLUE_CORE_ONLY` compila solo `handoff_trace`, `null_audio_system`,
  `xma_*`. Aggiungere, sotto `LIBERTY_RECOMP_TARGET_PLATFORM STREQUAL
  "switch"`: `audio_driver.cpp`, `audio_system.cpp`, `switch/switch_audio_system.cpp`,
  `switch/switch_audio_driver.cpp`, senza SDL (`rexaudio` linka SDL3 solo
  fuori da CORE_ONLY: va bene così). Controllare che `audio_system.cpp` non
  tiri dentro altro del desktop (`rex/diagnostics/gta4_transition.h`, gli
  handoff sono già nel build).
- **switch-game:** una cvar `nx_audio` (spenta all'inizio):
  `config.audio_factory = REX_AUDIO_BACKEND(rex::audio::nx::SwitchAudioSystem)`,
  altrimenti l'audio nullo.
- **Rischi noti:** priorità dei thread audio (il driver non cambia più la
  priorità del chiamante, commit `7758e6e`; attesa limitata dei wave buffer);
  il callback guest gira sul worker di `AudioSystem` (un `XHostThread`), come
  sul desktop; la memoria audren (mempool allineato) dentro l'heap.
- **Verifica:** prima l'audio nullo sulla console (il gioco deve arrivare dove
  arriva il PC); poi `nx_audio = true`: il battito con i conteggi dei
  callback deve dare ~187 al secondo, e il suono del logo deve sentirsi.

### C. Log dei tasti (da verificare)

- switch-game: ogni cambio dei tasti tenuti (maschera `HidNpadButton` di
  libnx) scrive `buttons 0x… at N.NNN s (was 0x…)`, dal thread
  dell'interfaccia (anche nell'attesa del + dopo un arresto); il battito
  aggiunge `buttons: N changes, now 0x…`.
- switch-smoke: nell'attesa del + finale, `buttons 0x… at N.NNN s of the wait
  (was 0x…)`.
- Maschera (libnx): A `0x1`, B `0x2`, X `0x4`, Y `0x8`, levetta sinistra
  premuta `0x10`, destra `0x20`, L `0x40`, R `0x80`, ZL `0x100`, ZR `0x200`,
  + `0x400`, − `0x800`, croce sinistra/su/destra/giù `0x1000`/`0x2000`/
  `0x4000`/`0x8000`; i bit alti sono le direzioni delle levette.

### Run 17: cosa guardare (`out/console-runs/run17-build/`)

- **Gioco:** `audio: the null audio system …`; poi `null audio: client 0
  registered` e nel battito `null audio: … callbacks` (~187 al secondo); la
  riga `boot sequencer` con l'indice che passa da 0 a 6 e "main waits on 0";
  le righe `file open #N` (ora tutte le aperture, non solo i fallimenti);
  `XamInputGetState` nei conteggi per minuto (`exports called at N s`), che
  sul PC sale a ~23 al secondo dopo gli schermi d'avvio; i tasti premuti
  (`buttons …`). Le tre funzioni nuove (`0x82177920`, `0x827D5200`,
  `0x82586A60`) sono nel codice generato: se ne manca un'altra, la riga è
  `STOP at the first function missing on Switch` o un fault nel crash log.
  Con la GPU nulla non si vede niente sullo schermo: quello che conta è dove
  arriva il log.
- **Smoke:** le righe `teardown step N`, il numero di thread prima e dopo,
  e `waiting for +: …` fino al crash; poi gli stessi run con le varianti
  (11-vulkan-probe.md). Se il crash si ripete, servono il crash report di
  `sdmc:/atmosphere/crash_reports/` e il testo esatto sullo schermo.

## 22. Da UnleashedRecomp-NX 1.0.0: cosa portare quando il gioco ha la GPU vera

Solo piano (10 §12). In ordine: prima ciò che dà di più con meno rischio. Per
ognuna: dove si innesta da noi, rischio per la correttezza, verifica. La
verifica è sempre la stessa base: **lo stesso codice generato gira sul PC**
(`tools/pc-eu-base`), quindi ogni modifica al generatore o al kernel si prova
prima sul PC confrontando la traccia delle export
(`compare_export_traces.py`), i conteggi per minuto e, quando ci sarà
l'immagine, i fotogrammi, con la modifica accesa e spenta.

### 1. Generatore di codice (`rexglue codegen`, `src/codegen/` dell'SDK, template `resources/templates/codegen/`)

| Modifica | Dove da noi | Rischio | Verifica |
|---|---|---|---|
| **Confronti FP inline** (`fcmpu`/`fcmpo` come codice senza salti al posto della chiamata) | `src/codegen/builders/` (emissione di `fcmp*`) e l'helper in `include/rex/ppc/`; opzione nel `gta4_eu_base_config.toml` | basso: un confronto non arrotonda; attenzione ai NaN (ordinato/non ordinato) e a `FPSCR[FPCC]`, se il codice lo legge | test unitario dell'helper su tutte le coppie speciali (±0, ±inf, NaN, denormali); traccia delle export identica sul PC; `read_before_write.py` invariato |
| **`memcpy`/`memset` guest di dimensione costante inline** (`li r5,N` subito prima della chiamata) | passo sul codice generato, come `direct_calls.py` (`tools/switch-codegen/`), o nel builder delle chiamate; serve l'indirizzo delle `memcpy`/`memset` del CRT europeo (da trovare) | medio: cambia il flusso di controllo di centinaia di funzioni (e il profilo PGO); un `li r5` che non è la dimensione darebbe una copia sbagliata | elenco dei siti con il registro letto dall'istruzione precedente, controllo a mano di un campione; PC con e senza: stessi file letti, stesse export, stessi conteggi |
| **Meno cambi di modo FPCR** (tenere il modo vettoriale per mosse, load/store float e confronti) | emissione delle istruzioni FP nel builder (`ppc_context` e helper di arrotondamento); riguarda il nostro modo di gestire flush-to-zero di VMX contro FPU | **alto**: risultati diversi sui denormali se un'istruzione dipende dal modo; loro hanno provato 105 milioni di casi | test dei loro helper "esatti" contro la conversione hardware su PC (stesso test, tutti i modi di arrotondamento); confronto dei conteggi export sul PC; poi la scena |
| **Multiply-add espliciti** (`-ffp-contract=off` + `fma` solo dove il guest ha `fmadd`/`vmaddfp`) | flag di compilazione dei file generati (`tools/switch-game/CMakeLists.txt`, `tools/pc-eu-base/CMakeLists.txt`) e builder di `fmadd*`/`fmsub*`/`vmaddfp` | basso per la correttezza (anzi la migliora: arrotondamenti come la 360), medio per le prestazioni (meno fusioni di GCC) | ricompilazione due volte: il numero di `fmla`/`fmadd` nell'ELF deve dipendere solo dal codice guest; PC e Switch con gli stessi risultati di calcolo nelle tracce |

Tutte vanno con un profilo PGO nuovo (06 §13): cambiano il flusso di
controllo di molte funzioni.

### 2. Kernel HLE (`src/kernel/xboxkrnl/`, `src/system/` dell'SDK)

| Modifica | Dove da noi | Rischio | Verifica |
|---|---|---|---|
| **Risvegli mirati**: un set di evento o un rilascio di semaforo sveglia i `KeWaitForMultipleObjects` solo se quell'oggetto è nella loro lista | `xboxkrnl_threading.cpp` (`KeSetEvent`, `KeReleaseSemaphore`, `KeWaitForMultipleObjects`, `NtWaitForMultipleObjectsEx`) e gli oggetti in `src/system/` (`xevent`, `xsemaphore`, il dispatcher) | **alto**: un risveglio perso blocca un thread per sempre; da loro il primo tentativo di cambiare il kernel guest ha fermato l'audio a una cutscene | test di stress sul PC (tanti thread, attese multiple); run lunghi sul PC con l'audio nullo (§21) e poi reale; contatori di attese per oggetto nel battito |
| **Semafori che svegliano uno solo** per un rilascio di una unità | `KeReleaseSemaphore`/`NtReleaseSemaphore` e la classe del semaforo | medio: va rispettata la semantica Windows (il conteggio, non i thread, decide); un rilascio di N unità deve poter svegliare N thread | come sopra; i worker del gioco condividono semafori: confronto dei conteggi per minuto di `NtWaitForSingleObjectEx`/`NtReleaseSemaphore` con e senza |

Prima di entrambe serve l'audio vero o nullo stabile: è il sottosistema che
loro hanno rotto.

### 3. Clock GPU 460 MHz in portatile

- **Dove**: un file nuovo `tools/switch-common/nx_apm.{h,cpp}` (dal codice di
  nfsmw-nx `sdk/src/ui/switch_apm.cpp` / `perf_switch.cpp` di Unleashed, 10
  §12), chiamato da `GameMain` dopo `StartLogging()` su un thread libnx
  proprio (non blocca l'avvio); più un `appletHook` su
  `AppletHookType_OnPerformanceMode` che, entrando in `ApmPerformanceMode_Normal`
  (portatile), lo applica se non è già stato applicato (avvio in dock, poi
  sganciata). Cvar `nx_handheld_gpu_boost`.
- **Rischio**: nessuno per la correttezza del gioco; per la console solo se si
  sbaglia configurazione: chiedere **solo** `0x92220008` (GPU 460,8, RAM
  1331,2), controllare con `clkrst` per 1,5 s che la RAM non si muova e
  tornare alla configurazione di prima altrimenti (0x92220007 alzerebbe la RAM
  a 1600).
- **Verifica**: righe `[apm]` nel log con i clock prima e dopo; in dock non
  deve chiedere niente; fps della stessa scena in portatile con e senza.

### 4. Profiler centrato sui picchi

Piano completo in §20. Dove: `tools/switch-game/` (un `nx_profiler.{h,cpp}`
che riusa l'istantanea dei thread e la pila guest), confini del fotogramma
attorno a `VdSwap`. Rischio: nessuno per la correttezza (pausa e ripresa dei
thread, già usate dalle istantanee); per le prestazioni un campionamento ogni
2 ms costa, quindi dietro una cvar spenta. Verifica: sul PC non c'è (usa
`svcSetThreadActivity`); sulla Switch un run con il profiler acceso e uno
spento, stessa scena, la differenza di fps deve essere piccola, e lo script
deve nominare le funzioni (`sub_82…` e host) dall'ELF del run.

## 23. Run 17: il gioco carica la città e si ferma su un errore di lettura (2026-10-03)

`out/console-runs/run17/`, NRO del commit `b1acd44`. PC di confronto:
`out/pc-runs/run17e` (stesso codice generato). Strumenti:
`tools/compare_export_traces.py` e i conteggi per finestra di 10 s del battito.

### A.1 Il sequenziatore e il confronto con il PC

- **La voce 0 passa anche sulla Switch.** Battito dei 20 s: `index 0`, tipo 1,
  client audio già registrato (`null audio: client 0 registered, callback
  82172BE8`); 30 s: `index 2`; 40 s: `index 5`, `main waits on 0`; 120 s:
  `active 0` (sequenziatore spento, `index 8`). Sul PC la voce 0 era passata
  prima dei 10 s e il sequenziatore era spento a 30 s: la Switch arriva allo
  stesso punto **15–20 s dopo all'inizio e circa 90 s dopo alla fine**.
- **Export:** le stesse 117 del PC, più `XamUserWriteProfileSettings` (solo
  Switch, LR `829C45B8`: c'è un pad e il gioco salva il profilo). Stesso
  ordine per le prime 37; dopo cambia solo l'ordine fra thread
  (`KfAcquireSpinLock` prima di `ExGetXConfigSetting`). Nessuna export chiamata
  da un LR diverso. Unico primo risultato diverso: `XamInputSetState`, PC
  `0x48F` (nessun pad), Switch 0. **Le 20 export nuove attese ci sono tutte.**
- **Thread:** `ExCreateThread` 25 come sul PC; 15 thread chiamano export su
  tutti e due; 30 XThread sulla Switch (25 guest + 5 host: `Kernel Host
  Tasks`, `XMA Decoder`, `Audio Worker (null)`, `GPU Commands (null)`, `GPU
  VSync (null)`).
- **File:** gli stessi 325 percorsi del PC. `maps\east\bronx_*.img` aperti a
  ~40 s (PC ~10 s). Unica differenza: 78 aperture di `cache:\`/`cache1:\`
  riescono sul PC (copie fatte dai run PC precedenti) e falliscono sulla
  Switch (cartella vuota all'inizio).

### A.2 Fotogrammi, letture, fin dove arriva il caricamento

| Finestra | VdSwap/s | letture ogni 10 s | MB/s | `XamInputGetState`/s |
|---|---|---|---|---|
| 30–160 s | 60 | 280–750 | 0,15–1,5 | 0–11 |
| 170–330 s | 13–51 | 290–560 | 0,9–1,5 | uguale a VdSwap (un pad letto per fotogramma) |
| 340–616 s | 60 | **0** | 0 | 29 fino a 460 s, poi 6–9 |

- **Switch:** 13.423 `NtReadFile`, ~331 MB in 335 s (~25 KB per lettura),
  5.040 `NtWriteFile` (le copie in `cache:\`; un'istantanea mostra un thread
  `IN NtWriteFile for 0.2 s`). La latenza di `NtReadFile` non era misurata
  (aggiunta per il run 18: riga `file I/O` del battito).
- **PC:** 84.335 letture, 2,6 GB in 360 s, picchi di 65 MB/s mentre copia i
  `.img` nella cache.
- **Dove si ferma (~335 s).** Il thread guest id 8 (`F8000C80`) fa 11 letture
  e ricevono tutte `C0000011` (END_OF_FILE) con `bytes=4294967295`; poi chiama
  `XamShowDirtyDiscErrorUI` (LR `829A1250`), che sulla Switch non c'è: `STOP
  at the first function missing`. Quel thread si parcheggia **tenendo una
  sezione critica**: a 420 s il thread delle letture (id 7) è `IN
  RtlEnterCriticalSection for 87.4 s` e da lì non c'è più nessuna lettura. Il
  resto del gioco gira (60 VdSwap/s, pad letto).
- **Causa trovata nel codice (da verificare):** `FileHandle::OpenExisting`
  (`src/core/filesystem_posix.cpp`) costruiva i flag con `|=`; `O_RDONLY` è 0
  e `O_WRONLY` 1, quindi un handle chiesto in lettura **e** scrittura era
  aperto `O_WRONLY` e ogni `read` falliva (EBADF). `PosixFileHandle::Read`
  metteva -1 nei byte letti (da qui 4294967295) e `HostPathFile::ReadSync`
  traduce ogni errore in END_OF_FILE. Il gioco rilegge le copie che scrive in
  `cache:\` (sul PC, con `CreateFile` di Windows, funziona). Il percorso letto
  non è nel log: dal run 18 ogni errore di lettura o scrittura dell'host scrive
  `host read failed: '<percorso>' offset … errno …` (i primi 32).
- **Per dopo (non fatto):** la copia in `cache:\` è un'ottimizzazione della
  360 (disco ottico → HDD); sulla Switch sorgente e cache sono la stessa SD, e
  copiare costa e basta. Una cvar che non monta `cache:`/`cache1:` è da provare
  **dopo** questa correzione, per vedere se il gioco salta la copia.

### A.3 Audio nullo

- Gira sul thread host `Audio Worker (null)` (XThread id 3, `F8000018`).
- **187,1–188,0 callback al secondo da 30 a 120 s** (atteso 48000/256 =
  187,5); `frames submitted` = `callbacks`, `played` = `callbacks` − 7 (gli 8
  crediti): nessuna deriva del timer.
- **Da 130 s cala:** 50–107/s fino a 160 s, 63–78/s fino a 460 s, 44–48/s
  dopo; il PC resta a ~187/s. L'istantanea dei 420 s mostra il worker `IN
  KeWaitForMultipleObjects` **dentro il callback guest**: il callback aspetta
  oggetti del motore audio del gioco e rallenta quando il gioco è sotto
  carico; il timer non è la causa probabile. Dal run 18 il battito scrive
  callback al secondo nella finestra, durata media e massima del callback,
  periodi e `resyncs` (periodi persi dopo più di 100 ms di ritardo).

### A.4 Aperture non riuscite

Tutte `C000000F`, nessun altro codice: 10 in `audio\config` (i sette noti più
`rpf.xml`, `waveslots.xml`, `effects.dat11`, gli stessi del PC), 44 in
`cache:\` e 80 in `cache1:\` (il tentativo sulla cache prima del disco).
Nient'altro.

### B. Crash alla chiusura con + (modo A)

- **Log:** `game-exit.log` ha i passi 1–5 fino a `_exit`; `game.log` finisce
  con `TERMINATION: _exit(0) called from LR … + 0x68d4`; nessuna riga
  `std::terminate`. `game-crash.log` ha **solo l'intestazione** `=== unhandled
  access violation ===`: il gestore è partito, ma le righe successive (stesso
  descrittore, subito dopo) non sono arrivate sulla SD.
- **Letture in corso all'uscita: 0.** Le `NtReadFile` (13.423) sono tante
  quante le completioni `[AsyncIO] complete`, l'ultima a ~335 s; le scritture
  sono sincrone e ferme da prima dei 360 s. Dal run 18 il primo passo d'uscita
  scrive `file operations in flight: N`.
- **Thread host vivi all'uscita:** i 5 XThread host di A.1, il thread
  principale, quelli del logger. Rispetto al run 16 sono nuovi `XMA Decoder` e
  `Audio Worker (null)` (prima non c'era un sistema audio), e c'è un thread
  guest parcheggiato nello STOP.
- **Le due ipotesi.** (2) "letture dalla SD in volo mentre libnx chiude i
  servizi": **non regge per questo run**, nessuna lettura era in corso. (1)
  "un thread host attivo mentre `exit(0)` distrugge gli statici": il passo 5
  (`_exit`) viene **dopo** gli handler `atexit` e i distruttori statici, e
  un'intestazione scritta seguita da righe perse dice che il fault arriva
  mentre `__appExit` smonta il file system (`fsdevUnmountAll`). Il fault è
  quindi di un altro thread, **dopo** la distruzione degli statici e durante la
  chiusura dei servizi di libnx: compatibile con un thread host ancora vivo
  che usa qualcosa già distrutto o già chiuso (una variante della 1); i log
  non bastano a dire di più (nessun PC del fault).
- **Correzione (da verificare); il default torna a D** (`nx_exit_mode`, A
  resta selezionabile):
  1. `file I/O gate closed`: `NtCreateFile`/`NtOpenFile`/`NtReadFile`/
     `NtReadFileScatter`/`NtWriteFile` si contano in corso dall'entrata al
     completamento (una lettura asincrona fino al suo task host) e, chiuso il
     cancello, rifiutano le nuove con `C0000120` (CANCELLED);
     `file_io::{EnterIo, CloseIoGate, WaitIoIdle}` in `rex/system/xfile.h`;
  2. attesa delle operazioni in corso, al più 2 s, con riga `file I/O: idle
     after N ms` o `STILL BUSY`;
  3. `rex::audio::StopNullAudio(2000)`: il worker esce al periodo successivo
     (flag), un callback in corso finisce; riga `null audio worker ended after
     N ms` o `STILL RUNNING`;
  4. il modo D di prima: thread guest fuori alla prossima export, GPU nulla
     ferma, join di 2 s, poi `exit(0)`.
  Il decoder XMA resta acceso (il suo `Pause` aspetta senza limite): se il
  run 18 crasha ancora all'uscita, è il prossimo candidato.

### C. Smoke

- **I cinque thread** non sono quelli del passo 6 (spento): li avvia `main`
  dopo la sonda Vulkan per il test d'uscita, con lo stesso `StartExitWorkers`
  del passo 6: `spin`, `mutex`, `malloc`, `SD read`, `mutex holder` (stack 64
  KiB, priorità 0x3B). Riga: `exit test: 5 threads working in the background;
  + exits with mode A …` (`tools/switch-smoke/main.cpp`, `StartExitWorkers` e
  la fine di `main`).
- **Dal log:** `teardown step 1`–`13` tutti presenti, `console reopened`,
  `VULKAN OK`, la riga del test d'uscita e **nessuna riga `waiting for +`**
  (la prima era a 1 s); `smoke-exit.log` vuoto. Thread del processo prima e
  dopo: `-1` tutte e due le volte (`svcGetThreadList` rifiuta
  `CUR_PROCESS_HANDLE`). **Il crash è nel primo secondo dell'attesa del +**:
  non durante lo smontaggio e non al +.
- **Run 18 (da verificare):** `nx_vk_reopen_console = false` per default
  (variante diagnostica: torna a true se non cambia niente); numero di thread
  con `envGetOwnProcessHandle()` (un fallimento scrive `-codice`); righe dopo
  il primo giro dell'attesa e a 250 e 500 ms con i contatori dei cinque
  thread; cvar `nx_smoke_exit_workers` (vera) per provare l'attesa senza i
  cinque thread.

### Run 18: cosa guardare (`out/console-runs/run18-build/`)

- **Gioco:** nessuna riga `host read failed` (o, se c'è, il percorso e
  l'errno); le righe `file open` ora finiscono con `access 0x…` e si ripetono
  per ogni combinazione nuova di percorso, disposition e accesso: le creazioni
  dei file di `cache:\` con un accesso in lettura e scrittura (per esempio
  `0xC0000000`) sono il caso che prima diventava `O_WRONLY`; `NtReadFile` che
  continua a salire dopo i 335 s; nessuno STOP su `XamShowDirtyDiscErrorUI`;
  righe `file I/O` (MB/s, latenza media e massima) e `null audio`
  (callback/s, durata del callback, `resyncs`).
- **Uscita con + (D):** in `game-exit.log` `file operations in flight`, `file
  I/O gate closed`, `file I/O: idle after`, `null audio worker ended after`,
  `N of M threads ended within 2 s`, poi `atexit` e `_exit`; e se esce pulito.
- **Smoke:** le righe `waiting for +: first pass …`, `250 ms`, `500 ms`;
  `process threads`; se con la console chiusa il crash sparisce.

## 24. Fase (a) del renderer: il backend Vulkan di ReXGlue dentro switch-game (piano, 2026-10-03)

Fase (a) di §11: la grafica imitata di ReXGlue (plugin `xenos`, backend
Vulkan) su NVK, solo per vedere i primi fotogrammi. Base: la sonda del run 16
(11-vulkan-probe.md: presentazione su `NWindow`, FIFO, 3 immagini, 60,6 fps,
API 1.3, niente fragment shader interlock → ReXGlue userà
`Path::kHostRenderTargets`). Riferimento per i pezzi Switch: l'SDK di
nfsmw-nx (`<refs>/nfsmw-nx/sdk`), che ha già `VK_NN_vi_surface`, la
`NWindow` come `Surface`, NVK statico e lo stack glslang/SPIRV-Tools per
aarch64.

**Questo giro: solo fino a "compila e linka".** Niente prova sulla console,
la GPU nulla resta com'è e resta il default.

### Passi

1. **SDK, CMake** (`glue/rexglue-sdk-main/CMakeLists.txt`,
   `thirdparty/CMakeLists.txt`, `src/graphics/CMakeLists.txt`,
   `src/ui/CMakeLists.txt`): opzione `REXGLUE_SWITCH_VULKAN` (spenta), che
   sulla Switch **lascia `REXGLUE_CORE_ONLY` acceso** e in più:
   - aggiunge le dipendenze del backend: `vulkan-headers`,
     `vulkan-memory-allocator`, `spirv-headers`, `spirv-tools`, `glslang`,
     `snappy` (`xxHash` e `imgui` ci sono già);
   - una libreria statica `rexgpu-xenos-static`: i sorgenti di `rexgpu-xenos`
     (nucleo, traduttore SPIR-V, `vulkan/*`) più la parte di `rexui` senza
     SDL (`presenter`, `window`, `windowed_app_context`, `immediate_drawer`,
     `imgui_drawer`, `graphics_util`, `graphics_upload_buffer_pool`,
     `ui/vulkan/*` tranne MoltenVK), con `REX_HAS_VULKAN=1`. Niente
     `window_sdl`, `windowed_app_context_sdl`, `sdl_virtual_key`, overlay.
2. **Loader statico**: `vulkan_instance.cpp` sulla Switch prende
   `vkGetInstanceProcAddr`/`vkDestroyInstance` da `libvulkan.a` (come già fa
   iOS con MoltenVK), senza `DynamicLibrary`; RenderDoc spento.
3. **Surface `NWindow`**: tipo `kTypeIndex_ViNWindow` in `rex/ui/surface.h`,
   `ViNWindowSurface` in `rex/ui/surface_switch.h`, estensione di istanza
   `VK_NN_vi_surface` (funzioni in `functions/instance_nn_vi_surface.inc`),
   ramo `vkCreateViSurfaceNN` in `vulkan_presenter.cpp` con
   `nwindowSetDimensions` prima della prima swapchain (come la sonda e
   nfsmw-nx).
4. **Finestra e contesto minimi per Switch** (nel nostro SDK, sotto
   `REX_PLATFORM_NX`): un `ui::Window` sulla `NWindow` di default
   (`nwindowGetDefault()`, 1280×720) e un `WindowedAppContext` il cui thread
   dell'interfaccia è il ciclo principale di switch-game (esegue le funzioni
   in coda a ogni giro); presi da `window_switch.cpp` e
   `windowed_app_context_switch.cpp` di nfsmw-nx, ridotti.
5. **switch-game**: `libvulkan.a` (`vulkan expat zstd z nx`, due volte) e
   `nx_elf_stubs.c` come lo smoke (target `INTERFACE` come `rex::nvk` di
   nfsmw-nx); `--whole-archive` sulla libreria del backend; cvar `nx_gpu =
   null|vulkan` (**default `null`**): `vulkan` crea `VulkanGraphicsSystem`
   direttamente (come `CreateNullGraphicsSystem`, nessun caricamento di
   plugin), il contesto e la finestra, e li dà al runtime.
6. **Trappole di §12, da fare quando si prova sulla console** (non in questo
   giro): `__nx_nv_transfermem_size` alzato prima di `nvInitialize`,
   sotto-allocazione da blocchi grandi, un submit per fotogramma,
   `MESA_SHADER_CACHE_DISABLE`.

### Esito di questo giro (compila e linka; non provato sulla console)

Come configurarlo (stessa cartella di build, così i sorgenti generati non si
ricompilano: la definizione e gli include del backend vanno solo su
`main.cpp`, la libreria è linkata `$<LINK_ONLY:…>`):

```
export THEFT4_NATIVE_PYTHON=/<drive>/<python nativo>/python.exe
cmake -S tools/switch-game -B out/build/switch-game -DTHEFT4_SWITCH_GAME_VULKAN=ON       -U "_Python3_*" -U Python3_EXECUTABLE       -DPython3_EXECUTABLE=$PWD/tools/switch-game/native_python.sh       -DTHEFT4_HOST_PYTHON=/usr/bin/python3.12.exe
ninja -C out/build/switch-game -j3 switch-game_nro
```

e per tornare al build dei run: `-DTHEFT4_SWITCH_GAME_VULKAN=OFF` e
`Python3_EXECUTABLE` di nuovo sul python di MSYS.

- `rexgpu-xenos-nx` (30 oggetti: nucleo, SPIR-V, `vulkan/*`, parte di `rexui`,
  `window_nx.cpp`) e le dipendenze compilano con devkitA64 GCC 16.1;
  `switch-game.elf` linka: **nessun simbolo SDL**, ci sono
  `vkCreateViSurfaceNN`, `VulkanGraphicsSystem`, `NxWindow`,
  `Window::Create`, glslang. NRO 63,2 MB contro 43,6 MB senza backend.
- Problemi dell'ambiente risolti: l'header precompilato di glslang ha un
  percorso MSYS che GCC nativo non apre (PCH spenti per le dipendenze); il
  python di MSYS non importa `pyexpat`, che serve ai generatori di
  SPIRV-Tools, e un python Windows non riceve convertiti i `/c/…` dentro gli
  argomenti composti (`tools/switch-game/native_python.sh`, con
  `THEFT4_NATIVE_PYTHON`); `missing_imports.py` resta sul python di MSYS
  (`THEFT4_HOST_PYTHON`, chiama `nm` di devkitPro); glslang non crea
  `OSDependent` perché la toolchain non è né WIN32 né UNIX (aggiunta la
  versione pthread); `renderdoc_app.h` non conosceva la piattaforma (Switch
  trattata come Linux per i soli tipi, RenderDoc spento come su iOS).
- **Scelte fatte in questo giro** (dai riferimenti di 11 e nfsmw-nx, da
  confermare): il backend è creato da switch-game come la GPU nulla, senza
  caricatore di plugin; il thread dell'interfaccia è il ciclo principale di
  switch-game (`PumpFromUIThread` a ogni giro di 16 ms); con `nx_gpu =
  vulkan` la console viene chiusa prima della surface; la finestra è fissa a
  1280×720 (niente dock: in dock l'immagine resta a 720p).
- **Aperto per la prova sulla console:** le trappole di §12 (transfer memory
  di nvdrv, sotto-allocazione, un submit per fotogramma), il percorso EDRAM
  senza interlock (`kHostRenderTargets`), memoria condivisa da 512 MB senza
  sparse binding, e la traduzione degli shader (glslang a runtime) sul thread della
  GPU.

## 25. Run 18: il gioco supera il punto del run 17; strumenti del run 19 (2026-10-09)

`out/console-runs/run18/`, NRO del commit `58b4efc`. Smoke e gioco finiscono
senza crash.

### A.1 Chiusura (modo D)

Tutti i 12 passi, nessuna riga anomala, `game-crash.log` vuoto: 09-rules §10
(una sola prova pulita, da riconfermare). Tempi disponibili: I/O fermo dopo
0 ms, audio nullo fermo dopo 4 ms, join limitato a 2 s (9 di 31 thread
finiti). Dal run 19 ogni passo porta `+N ms`.

### A.2 Durata e punto d'arresto

- **Acceso ~762 s** (dal thread guest avviato all'uscita: tick host
  8.076.164.451.002 → 8.090.786.864.568 a 19,2 MHz), di cui **406 s fermo**:
  il battito salta da 310 s a 716 s, il callback dell'audio nullo ha un massimo
  di 403.756 ms, nessuna lettura nel mezzo. Il processo non è stato eseguito
  (sospensione). Nessun tasto premuto prima di 722,7 s: **probabile
  auto-sleep dopo ~5 minuti senza input**. Tempo attivo ≈ **356 s**. Dal run
  19 `appletSetAutoSleepDisabled(true)` all'avvio (cvar
  `nx_disable_auto_sleep`, da verificare). Dopo la ripresa il gioco riparte
  (VdSwap risale fino a 60/s).
- **Il punto del run 17 è superato:** 0 `C0000011`, 0 righe `host read
  failed`, nessun `XamShowDirtyDiscErrorUI`. `cache:\anim.img` creato a ~100 s
  (disposition 5, accesso `0xC0100080`, lettura e scrittura) e riaperto a
  ~225 s con lo stesso accesso, letto senza errori; poi `weapons.img` e
  `icons.img` copiati e riaperti; `cache1:\componentpeds.img` creato a ~232 s,
  scritture finite fra 746 e 756 s. La correzione O_RDWR di 06 §23 regge
  (una prova).
- **Fin dove arriva:** le stesse 117 export del run 17 (nessuna nuova), 30
  XThread, aperture fino alla numero 1247; 14.350 `NtReadFile` (356,1 MB) al
  battito dei 756 s, 14.624 completamenti asincroni (363 MB) all'uscita;
  5.207 scritture (170,2 MB) nella cache. Il sequenziatore è spento dai 120 s
  come nel run 17.

| Finestra (s) | VdSwap/s | letture / 10 s | MB/s | scritture / 10 s | callback audio/s |
|---|---|---|---|---|---|
| 20–120 | 32–60 | 250–750 | 0,14–1,7 | 0 | 187,5 |
| 130–160 | 58–60 | 400–570 | 1,0–1,5 | 0–1 | 51–113 |
| 170–230 | 25–45 | 300–460 | 1,0–1,3 | 200–350 | 62–79 |
| 240–310 | 13–30 | 290–380 | 0,9–1,2 | 260–345 | 62–75 |
| 716–746 (dopo la ripresa) | 23–43 | 270–450 | 0,9–1,3 | 160–270 | 63–79 |
| 746–756 | 60 | 588 | 1,48 | 0 | 84 |

Il gioco legge ancora a 1,5 MB/s alla fine e torna a 60 VdSwap/s quando la
copia nella cache finisce. Senza GPU non si vede in che schermata sia.

### A.3 Caricamento: I/O o CPU

- **Dimensioni delle letture** (14.624 completamenti): < 4 KB 14 %, 4–16 KB
  21 %, 16–32 KB 5 %, **esattamente 32 KB 60 %**, oltre 32 KB 0,4 % (7 letture
  da 1 MB o più). Media 25 KB.
- **Latenza** (dal battito; la distribuzione per fasce c'è dal run 19): media
  0,4–1,9 ms, massimo ≤ 40 ms (198 ms solo nella prima finestra). **Tempo in
  lettura ≤ 0,9 s ogni 10 s**: la SD non è il limite delle letture.
- **Scritture** (la copia in `cache:`): 32 KB alla volta, **11–20 ms l'una**
  (2,3 MB/s), 2,4–5,7 s ogni 10 s sul thread guest che copia. Fino al run 18
  una mutex globale serializzava `pread`/`pwrite` di tutti i file (emulati con
  `lseek` + `read`/`write`): una lettura asincrona poteva aspettare dietro una
  scrittura di 14 ms. Dal run 19 una mutex per descrittore (da verificare).
- **Log:** ogni lettura asincrona scriveva una riga `[AsyncIO] complete` nel
  log durevole (apri, scrivi, `fsync`, chiudi) **dal worker delle letture**,
  subito dopo la lettura: 14.624 scritture sulla SD in serie con le letture.
  Il costo di una riga non è misurato nel run 18 (il run 19 lo scrive nella
  riga `log:` del battito).
- **CPU:** non misurata per thread nel run 18. Indizi: dai 130 s le export
  salgono a ~500.000/s (a 300 s: `KeRaiseIrqlToDpcLevel` 16,0 M,
  `KeAcquireSpinLockAtRaisedIrql` e `KeReleaseSpinLockFromRaisedIrql` 15,7 M,
  `KfLowerIrql` 15,6 M, `RtlEnter/LeaveCriticalSection` 11,1 M); il thread
  guest id 18 da solo ne fa ~200.000/s (14,4 M a 180 s, 38,8 M a 300 s).
- **Conclusione provvisoria:** il caricamento **non è limitato dalla lettura
  dalla SD**. È limitato dal costo per richiesta sul worker unico delle letture
  (il log durevole per riga: da verificare con la riga `log:`), dalla copia in
  `cache:` (scritture lente e, fino al run 18, la mutex globale) e
  probabilmente dalla CPU dei thread guest dai 130 s (da verificare con la
  riga `cpu`). Il run 19 separa le tre cose.

### A.4 Benchmark della SD (smoke, run 19)

Passo 6b dello smoke (`tools/switch-smoke/sd_bench.cpp`, cvar
`nx_smoke_sd_bench`): `xbox360/audio/sfx/speech.rpf` (784 MiB, se manca
`common.rpf`), letture sequenziali da 1 MiB e casuali da 64 KiB, 1 e 3 thread,
10 s ciascuna, con open/lseek/read come le letture del gioco; poi 5 s di
scritture da 32 KiB in coda a un file nuovo (il modello della copia in
`cache:`), cancellato alla fine. Righe: MB/s, operazioni, latenza mediana,
p95 e massima.

### A.5 Audio nullo

- Il worker (host, id 3, `F8000018`) a 180, 300 e 716 s è sempre `IN
  KeWaitForMultipleObjects` con LR guest `82169BA8`, dentro il callback
  `82172BE8` (pila guest non risalibile oltre l'LR: il worker è un thread host
  con contesto guest).
- Il thread guest **id 18** (`F8000CF0`) lavora nella stessa zona di codice
  (`KeWaitForSingleObject` a LR `82169458`,
  `XAudioGetVoiceCategoryVolumeChangeMask` a LR `82169C98`, poi `KfLowerIrql`
  a `8216DB9C`/`82178ED0`): è il thread del motore audio del gioco, quello che
  dovrebbe segnalare gli oggetti attesi dal callback. Dai 130 s fa ~200.000
  export al secondo (spin lock e IRQL).
- Callback: 187,5/s fino a 120 s (atteso 48000/256), poi 51–113/s e 62–84/s
  fino alla fine, durata media 12–16 ms contro un periodo di 5,33 ms; resta a
  84/s anche a 756 s, quando VdSwap è di nuovo a 60.
- **Sintomo, non causa:** il gioco non si ferma (nel run 18 nessuno stop) e il
  calo comincia quando il motore audio comincia a mixare suoni veri (130 s,
  dopo il sequenziatore); il callback aspetta il thread 18, che è carico. Il
  run 19 dà il suo tempo di CPU (riga `cpu`) per dire se è la CPU.

### C. Controlli dalla guida di sa2-nx (porting-guide §7.4)

1. **Handle dei thread:** i thread host del runtime fanno `pthread_join`
   (che chiude l'handle) solo in `post_execution`, cioè dopo un'attesa riuscita
   sul thread; un thread finito su cui nessuno aspetta tiene handle e pila. Dal
   run 19 la riga `threads:` del battito: thread di Horizon contro il limite
   della risorsa del processo, e creati / finiti / joinati / abbandonati del
   runtime. Nel run 18 gli XThread erano 30 fino alla fine (nessuna crescita).
2. **Letture corte:** `PosixFileHandle::Read` faceva **una sola** `read`; una
   lettura corta (non a fine file) arrivava al gioco come corta. Dal run 19
   `Read` e `Write` ripetono fino alla lunghezza chiesta; contatore nella riga
   `file reads in the window`.
3. **`thread_local`:** su devkitA64 ogni accesso è una chiamata a
   `__aarch64_read_tp` (`mrs tpidrro_el0; ldr [x0, #504]; ret`), 121 punti
   nell'ELF del run 18. Nel codice generato e in `GuestToHost`: **nessuno**.
   Nel percorso di ogni export: `ExportEnter` ed `ExportLeave` (uno
   ciascuno), più `ThreadState::Get` (21 chiamanti, fra cui
   `KeRaiseIrqlToDpcLevel`, `KfLowerIrql` e gli spin lock: proprio le export
   più chiamate) e `XThread::GetCurrentThread` (46). Nel gestore dei fault:
   solo `seh_active()`. Il tracciamento costa per export due chiamate di
   funzione, due letture TLS, un `fetch_add` LL/SC su un contatore condiviso
   fra i thread, una lettura di `cntpct_el0` e cinque store. Dal run 19: la
   riga `export trace` all'avvio misura i ns per chiamata e il costo di una
   lettura `thread_local`; cvar **`nx_trace_exports`** (vera) per spegnerlo
   nelle build di misura (resta l'hook d'ingresso, che serve all'uscita D).
4. **Log:** scrittore asincrono, default dal run 19; `nx_log_durable = true`
   per la diagnosi dei crash (09-rules §11).

### F. Primo renderer: preparazione del run 19

Le quattro scelte di §24 restano (conferma provvisoria): backend creato da
switch-game senza caricatore di plugin; thread dell'interfaccia = ciclo
principale; con `nx_gpu = vulkan` la console si chiude; finestra fissa
1280×720 (in dock resta 720p). In più, con `nx_gpu = vulkan`:
`MESA_SHADER_CACHE_DISABLE=true` e la transfer memory di nvdrv dalla cvar
`nx_nvmap_mb` (0 = gli 8 MiB di libnx; nfsmw-nx: `0x235C`
SharedMemoryTooSmall quando finisce).

Battito con Vulkan: riga `vulkan:` con present (al secondo, tempo medio di
fotogramma), draw (`IssueDraw`), submit, fallimenti di submit e present con
l'ultimo codice (e una riga d'errore per ciascuno dei primi 32), memoria
Vulkan per heap (`VK_EXT_memory_budget`, se c'è); dopo 20 s senza present
un'istantanea dei thread. Per la memoria (il guasto da provare per primo):
riga `host memory:` (memoria del processo e heap di newlib, con i picchi) e il
picco del pool guest, già nella riga del battito.

### Run 19: cosa guardare (`out/console-runs/run19-build/`)

- **Prima prova:** `switch-game.nro` (senza Vulkan, `nx_gpu = null`) e
  `switch-smoke.nro`. Poi, a parte, `switch-game-vulkan.nro` rinominato
  `switch-game.nro`, con `nx_gpu = "vulkan"` in `sdmc:/switch/theft4/game.toml`.
- **Battito:** `cpu` (chi consuma, guest contro host), `log:` (costo delle
  scritture sulla SD, ora una al secondo), `file reads in the window`
  (distribuzione di latenza e dimensione, letture corte), `threads:` (handle),
  `host memory:`. Confronto con il run 18: MB/s delle letture e momento in cui
  il sequenziatore si spegne (120 s nel run 18).
- **Avvio:** `export trace: N ns per traced call`, `auto-sleep: … -> 0x0`,
  `log: buffered`.
- **Uscita:** 12 passi con `+N ms`, `game-crash.log` vuoto.
- **Smoke:** righe del passo 6b (tetto della SD).
- **Vulkan:** `vulkan:` con present > 0, oppure gli errori e l'istantanea dopo
  20 s; `host memory:` e il pool guest prima del fallimento, se c'è.

## 26. Run 19: Vulkan non è partito; il caricamento è la copia nella cache (2026-10-09)

`out/console-runs/run19/`, NRO di `run19-build` (commit `b04a53a`). Tre
avvii, nessun crash: smoke; gioco senza Vulkan (`game.prev.log`, 640 s);
gioco con `switch-game-vulkan.nro` (`game.log`, 520 s).

### A. Vulkan: il backend non è stato usato

- `game.log` è davvero del NRO con Vulkan (blocco di codice `0x2da5000`, come
  l'ELF Vulkan; quello senza è `0x2622000`), ma la seconda riga è `Config file
  not found: sdmc:/switch/theft4/game.toml` e poi `graphics: null GPU`:
  `nx_gpu` è rimasto `null`. Le note del run 18 dicevano `switch-game.toml`,
  il codice leggeva solo `game.toml`. **Nessun present, nessun draw, nessun
  VkResult, nessun dato di memoria Vulkan: A.1–A.4 non hanno risposta da
  questo run.** Il secondo avvio vale come secondo avvio con la GPU nulla e la
  cache già in parte piena (B.4).
- Run 20: si leggono tutti e due i nomi (`game.toml`, poi `switch-game.toml`),
  con una riga `[game] config …: found, loaded / not found` per ciascuno e
  `nx_gpu = …` nella riga dei cvar; nel NRO con Vulkan, se `nx_gpu` non è
  `vulkan`, una riga lo dice. Contenuto del file:
  ```toml
  nx_gpu = "vulkan"
  nx_vk_dump_frames = true
  ```
- **Cattura dei fotogrammi** (A.2, da verificare): a 20, 40, 60, 120 e 240 s
  `CaptureGuestOutput` del presenter (l'uscita del gioco prima della scala) e
  una riga `frame at N s: guest output WxH, X% of pixels not black, mean,
  max`, sempre; con `nx_vk_dump_frames = true` anche
  `sdmc:/switch/theft4/frames/frame-NNNs.ppm` (solo sulla SD). La riga dice già
  se è stato disegnato qualcosa senza copiare i file.
- A.3 (confronto con il PC nello stesso punto) e A.4 (lentezza di Vulkan) si
  fanno sul run 20.

### B.1 Caricamento: run 18 contro run 19

| | Run 18 | Run 19 (1° avvio) | Run 19 (2° avvio) |
|---|---|---|---|
| Sequenziatore spento | 120 s | **60 s** | **60 s** |
| Primi `maps\east\bronx_*` | ~40 s (run 17) | fra 20 e 30 s | fra 20 e 30 s |
| MB/s letti durante la copia | 0,9–1,5 | 1,7–2,5 | 1,7–2,9 |
| Latenza media lettura (32 KB) | 1,5–1,9 ms | 1,5–1,9 ms | 1,5–2,0 ms |
| VdSwap/s fino alla copia | 60 | 60 (fino a 70 s) | 60 (fino a 80 s) |
| VdSwap/s durante la copia | 13–45 | 45 → 28 → **9** (640 s) | 30 → 8 → **6** (520 s) |
| Scritture nella cache | 5.207 (170 MB) | 31.470 (1.030 MB) | 25.225 (826 MB) |

CPU per thread (riga `cpu`, 100 % = un core; la Switch ne dà 3 al gioco):

| Fase | Guest | Host | Thread più carichi |
|---|---|---|---|
| 10–50 s (sequenziatore) | 115–200 % | 2 % | id 14 al 100 % (in coda al sequenziatore), id 6 (Main XThread) fino all'86 %, UI 11–23 % |
| 60–80 s | 230–270 % | 6–15 % | id 14 98 %, id 18 (motore audio) 40–63 %, id 7 25–38 % |
| copia (90 s → fine) | 200–225 % | 15–29 % | id 18 54–69 %, id 6 25–57 %, id 19 23–37 %, id 11/12/13 ~25 % ciascuno, id 2 (XMA, host) 13–17 % |

**Quale sospetto del run 18 spiega il run 19:**

1. **Il log durevole per riga: sì.** È l'unica modifica che tocca la fase
   prima della copia, e il sequenziatore passa da 120 a 60 s. Il run 19 scrive
   750–790 righe ogni 10 s; con il log a buffer la loro scrittura costa
   150–430 ms per finestra (picchi di 1,4–1,6 s ogni 60 s, quando esce il
   riepilogo delle export) su un thread a parte invece che sui thread che
   loggano.
2. **La mutex unica: no, non misurabile.** Latenza delle letture uguale prima
   e dopo (1,5–1,9 ms per 32 KB anche con la mutex per file): le letture
   competono con le scritture sulla SD, non sulla mutex.
3. **Le ~500.000 export/s: costo piccolo, misurato.** Media del run 412.000/s;
   150 ns per export tracciata (riga `export trace`, un thread senza
   contesa) → ~62 ms/s, il 6 % di un core su tutto il processo. Per thread
   (istantanee 300–540 s): id 19 205.000/s (≈ 31 ms/s, **~10 % della sua
   CPU**), id 18 178.000/s (≈ 27 ms/s, ~5 %), Main XThread id 6 12.500/s
   (< 1 %). Vedi B.2.

**Il collo di bottiglia ora è la copia nella cache.** Il gioco copia `.img`
da `game:` a `cache:`/`cache1:` per tutta la sessione: ~600 scritture da 32 KB
ogni 10 s, 14 ms l'una (thread guest **id 21**, un'istantanea `IN NtWriteFile
for 1.3 s`), cioè ~2 MB/s; il file è dimensionato prima
(`NtSetInformationFile`, 47 chiamate). VdSwap scende in parallelo da 60 a
6–9/s; nel run 18, a copia finita, era tornato a 60. Il gioco **non** è
limitato dalla CPU totale (≈ 2,2–2,5 core su 3) né dalle letture.

### B.2 Tracciamento delle export

Acceso in tutti e due gli avvii (`tracing on (cvar nx_trace_exports)`
all'avvio; dal run 20 anche in ogni riga `heartbeat`). La misura "acceso
contro spento nello stesso punto" non c'è ancora: va fatta nel run 20 con due
avvii identici, il secondo con `nx_trace_exports = false`, confrontando la
riga `cpu` di id 18 e 19 fra 120 e 300 s. Dalla stima di B.1 il costo supera
il 5 % della CPU dei due thread più chiamanti (id 19, id 18) ma non del Main
XThread; **proposta: spento per default nelle build di misura** dopo la
conferma del run 20 (il default resta acceso nelle build di diagnosi, perché
le istantanee "IN <export>" dipendono dal tracciamento).

### B.3 Export più chiamate e chi le chiama

A 600 s: `KeRaiseIrqlToDpcLevel` 44,7 M, `KeAcquireSpinLockAtRaisedIrql` e
`KeReleaseSpinLockFromRaisedIrql` 44,0 M, `KfLowerIrql` 43,9 M,
`RtlEnter/LeaveCriticalSection` 30,1 M, `KeQueryPerformanceFrequency` 2,7 M,
`KeDelayExecutionThread` 2,2 M, `KeTlsGetValue` 2,0 M,
`NtWaitForSingleObjectEx` 1,5 M. Le quattro della prima riga hanno lo stesso
conteggio: sono sezioni protette da spin lock (alza IRQL, prendi, lascia,
abbassa), non giri a vuoto su un lock conteso. Le fanno soprattutto id 19
(LR `8216BF28`, sleep a `829A9624`) e id 18 (LR `8216DB9C`/`82178ED0`), tutti
e due nella zona `8216xxxx` del motore audio.

Il run 19 non sapeva quale LR chiama ogni export (solo l'ultimo per thread).
Run 20: ogni 256ª export di ogni thread registra l'LR guest nell'export
(8 posti), e ogni minuto una riga `export rates` con le 20 export più chiamate
**al secondo** nella finestra e i loro chiamanti con la quota dei campioni.

**Attese attive:** `KeDelayExecutionThread` (≈ 3.700/s) passa per
`XThread::Delay`, che tronca un intervallo relativo sotto 1 ms a 0 ms e allora
fa solo un yield: uno `Sleep` breve del gioco diventa un giro a vuoto. Dal run
20 la riga `KeDelayExecutionThread since start` conta gli intervalli (zero,
sotto 1 ms, 1–9 ms, ≥ 10 ms, assoluti). Se i "sotto 1 ms" sono la maggioranza,
la correzione (dormire i microsecondi chiesti, come la guida di sa2-nx §9.5:
attese che dormono) è il passo successivo; non è fatta.

### B.4 Cache al secondo avvio

La copia **non è saltata e non è rifatta da capo**: il gioco ricrea
`cache:\valid.txt` e `cache1:\valid.txt` a ogni avvio (disposition 5), riusa
i file già completi (50 aperture in sola lettura al secondo avvio) e ricrea
gli altri (30 creazioni contro 52 del primo). Al secondo avvio ha scritto
ancora 826 MB in 520 s. Il sequenziatore si spegne a 60 s in tutti e due gli
avvii: la cache non accorcia l'avvio, accorcia solo il resto della copia.
Run 20: cvar `nx_mount_cache` (vera); con `false` il gioco non trova
`cache:`/`cache1:`: da provare per vedere se salta la copia e se VdSwap resta
a 60 (sulla Switch sorgente e cache sono la stessa SD).

### B.5 Benchmark della SD (smoke)

| Prova | MB/s | Latenza mediana | p95 | Massima |
|---|---|---|---|---|
| Sequenziale 1 MiB, 1 thread | 85,9 | 12,15 ms | 12,43 ms | 14,97 ms |
| Sequenziale 1 MiB, 3 thread | 86,3 | 12,10 ms | 12,37 ms | 14,31 ms |
| Casuale 64 KiB, 1 thread | 53,6 | 1,20 ms | 1,36 ms | 2,19 ms |
| Casuale 64 KiB, 3 thread | **16,5** | 1,21 ms | 33,85 ms | 67,69 ms |
| Scritture da 32 KiB in coda a un file nuovo | **0,2** | 142 ms | 220 ms | 220 ms |

Il gioco legge ~2 MB/s: il **2–4 % del tetto** della SD (86 MB/s sequenziale,
54 MB/s casuale da 64 KiB). Tre thread sullo stesso file casuale rendono meno
di uno (la SD o fsdev serializzano: niente letture parallele sullo stesso
file). Le scritture che allungano il file costano 142 ms l'una: il gioco, che
dimensiona prima il file, paga "solo" 14 ms, ma è ancora 7 volte il costo di
una lettura da 32 KB.

### B.6 Spegnimento automatico

`appletSetAutoSleepDisabled(true) -> 0x0` in tutti e due gli avvii e il
battito non salta (640 e 520 s continui, nessun buco). Funziona (due prove).

### Altro dal run 19

- **Handle:** `threads: Horizon 33 of 96`, host creati 31, joinati 1
  (30 vivi, sono i thread del runtime): nessuna perdita.
- **Letture:** tutte da 32 KB, 1–4 ms; nessuna lettura o scrittura corta.
- **Memoria:** processo 3185 di 3189 MiB (l'heap preso all'avvio), heap di
  newlib 1.453 MiB in uso, pool guest 266 blocchi su 384 (532 MiB): fermi dai
  60 s.
- **Thread UI 11–23 % di un core:** `ConsoleRefresh` chiamava `consoleUpdate`
  a ogni giro (16 ms). Dal run 20 solo quando stderr ha scritto qualcosa
  (le righe `[game]` aggiornano già da sé). Da verificare.
- **Uscita D** pulita in tutti e due gli avvii del gioco (lo smoke esce con il
  modo A, il suo default), 12 passi con i tempi: I/O fermo a +1,56–1,58 s (il
  riepilogo delle export prima costa ~1,4 s), join 2 s, `_exit` a +3,8 s.

### C.1 Piano: solo edizione europea, base 0.0.0.6 senza TU

Non c'è altro: disco europeo, nessuna TU, nessun disco USA.

- **Cosa funziona già:** il codice generato dalla base europea
  (`config-eu-base`, §2 strada C), il kernel, l'avvio fino alla città
  (sequenziatore, file `bronx`, copia nella cache), input, uscita. Nessun hook
  di Theft4 è attivo in switch-game.
- **Hook di `gta4-recomp/src`** (scritti per la v8 USA, 2.364 indirizzi):
  - *necessari per giocare*: nessuno di sicuro. Il gioco su 360 gira senza;
    i candidati sono quelli che sostituiscono un servizio della 360 che sul
    nostro runtime non c'è o va diverso: `gta4_lzx_hooks` (decompressione,
    12 indirizzi), `gta4_input_hooks` (record del pad, 105: noi passiamo già
    il pad come XInput, quindi solo se l'input dà problemi),
    `gta4_presentation_hooks`/`gta4_native_hooks` (solo per il renderer
    nativo, sotto);
  - *ottimizzazioni o comodità*: `gta4_population_hooks` (densità: è la leva
    CPU di LibertyRecompAndroid, 12 §7), `gta4_aspect_hooks`,
    `gta4_frame_limiter`, `gta4_loading_parallax_fix`, `gta4_quicksave_hooks`,
    `gta4_frontend_hooks`;
  - *solo per il PC o diagnostica*: mouse/tastiera, gyro, touch,
    multiplayer a 64 (1.046 indirizzi), online, trace (`*_trace*`), DLC.
- **Differenze della base senza TU:** il repository non le documenta. Note
  certe: niente contenuti scaricabili (gli Episodi richiedono la TU), niente
  correzioni del multigiocatore della TU; per il resto vanno ricavate con il
  confronto delle funzioni fra la base e la v8 (stesse firme, corpo diverso)
  quando servono. Il gioco in solitario dal disco è il caso d'uso.
- **Ritrovare gli indirizzi sulla base europea** (per i pochi hook utili): per
  ogni funzione agganciata nella v8, prendere la sequenza delle istruzioni
  (togliendo gli spiazzamenti assoluti) e cercarla nella base; il nostro
  codice generato `generated/eu-base` ha già i confini delle funzioni. Uno
  script (stile `tools/compare_export_traces.py`) che produce la tabella
  v8 → EU con il grado di somiglianza; accettare solo corrispondenze uniche,
  verificare le altre a mano.
- **Renderer PM4 di nfsmw-nx con pochi hook:** il loro renderer legge il
  flusso PM4 (come il nostro processore `xenos`) e usa pochi hook per sapere
  dal D3D del gioco *quali* shader sono creati e *quale* draw sta passando
  (`nfsmw_shader_hooks.cpp`, `nfsmw_nativo_ganchos.cpp`). Per GTA IV europeo
  servono gli stessi due punti, ritrovati con il confronto di sequenze:
  creazione degli shader (le funzioni del D3D che ricevono il microcodice
  Xenos: le chiamate a `MmAllocatePhysicalMemoryEx` vicine e il formato del
  microcodice le rendono riconoscibili) e invio dei draw. `gta4_native_hooks`
  di Theft4 ne usa 135 distinti sulla v8: per il primo passo bastano
  quelli della creazione degli shader e del draw. Prima però va visto se il
  processore `xenos` (run 20) disegna: se sì, il renderer nativo è
  un'ottimizzazione e non una condizione.

### Run 20: cosa guardare (`out/console-runs/run20-build/`)

- **Vulkan prima di tutto:** `switch-game-vulkan.nro` rinominato
  `switch-game.nro`, `game.toml` (o `switch-game.toml`) con `nx_gpu =
  "vulkan"` e `nx_vk_dump_frames = true`. Righe: `config …: found`,
  `graphics: ReXGlue's Vulkan backend`, `vulkan:` nel battito, `frame at N s`,
  errori `vkQueueSubmit`/`vkQueuePresentKHR`, `host memory:`.
- **Senza Vulkan, tre avvii** (stesso `switch-game.nro`), uno per variante in
  `game.toml`: (1) niente; (2) `nx_trace_exports = false`; (3)
  `nx_mount_cache = false`. Confronto: VdSwap/s, `cpu` di id 6, 18, 19, 21,
  sequenziatore, scritture.
- **Battito:** `export rates` (le 20 export al secondo con i chiamanti),
  `KeDelayExecutionThread since start`, `tracing on/OFF`, `cpu` della UI
  (attesa sotto il 5 %).

## 27. Run 20: il gioco entra in partita con Vulkan (0,4 FPS); crash con la cache; strumenti del run 21 (2026-10-10)

`out/console-runs/run20/`, NRO di `run20-build` (commit `716936b`, Vulkan).
Tre avvii, file con suffisso 1, 2, 3; `frames/` è dell'avvio 3 (l'unico con
il fotogramma a 240 s; ogni avvio sovrascrive i PPM).

### A.1 I tre avvii

| | Avvio 1 | Avvio 2 | Avvio 3 |
|---|---|---|---|
| File letto | `game.toml` (found, loaded); `switch-game.toml` not found | uguale | uguale |
| `nx_gpu` | vulkan | vulkan | vulkan |
| `nx_trace_exports` | true (default) | **false** | true (default) |
| `nx_mount_cache` | true (default) | true (default) | **false** |
| `nx_vk_dump_frames` | true | true | true |
| Durata (ultimo battito) | 150 s (fault subito dopo) | 130 s | 812 s (righe fino a 824 s) |
| Fine | `[stderr] [rex] Unhandled Switch access violation` pc = far = `0x6ac97ff4`, esr `0x82000005` (instruction abort, lettura) | nessuna riga: il log si ferma fra due `[AsyncIO] complete` | nessuna riga: ultime righe = tasti delle levette a 819–824 s |
| `TERMINATION` / `std::terminate` | no | no | no |
| VkResult ≠ VK_SUCCESS | nessuno (submit 0, present 0) | nessuno | nessuno |
| Pool guest | 266 blocchi, picco 266 di 384 | 266/384 | 266/384 |
| Heap newlib (arena / in uso) | 2207 / 2203 MiB | 2212 / 2203 MiB | **3018 / 2984 MiB** |
| Memoria Vulkan (heap 0, uso / budget) | 700 / 1315 MiB | 700 / 1315 MiB | **1395 / 1482 MiB** |
| Processo (svcGetInfo) | 3185 / 3189 MiB | uguale | uguale (sempre: libnx prende tutto l'heap all'avvio) |
| Errori nvmap | nessun dato (non contati nel run 20) | – | – |
| Scritture su file | 12 (11 all'avvio, 1 a ~120 s) | 12 | 22 (tutte all'avvio) |
| Fault in `game-crash.log` | 358 | 315 | 2282 |

**I 2955 "unhandled access violation" di `game-crash.log` non sono crash.**
Sono i fault del write-watch del backend Vulkan sulla memoria guest
(scritture del gioco e di `copy_and_swap` dell'I/O su pagine sorvegliate,
indirizzo `0xbf…` della memoria fisica guest), gestiti dal gestore MMIO: il
nostro `CrashLogHandler` era installato **prima** di Runtime::Setup, quindi
prima del gestore MMIO, e registrava ogni fault (con un `fsync` sulla SD
ciascuno) prima che il gestore vero lo prendesse. Con la GPU nulla non
c'erano pagine sorvegliate, per questo non si era mai visto. Corretto (D).

**Avvio 1: il fault vero.** Ultimo record completo: pc = far = `0x6ac97ff4`
(fuori dal modulo, non mappato), **sp = `0x6ac97f5d`** (non allineato e quasi
uguale al pc: registri corrotti, non un puntatore sbagliato del gioco),
lr = base + `0x23cd534` = `sub_829B2AB0` subito dopo la chiamata a
`sub_829A0E58` (`gta4_recomp.76.cpp:21868`), cioè una funzione che fa
anch'essa fault di write-watch (31 dei record sono in `sub_829A0E58`). Dopo, un
altro record si interrompe dopo l'intestazione: il processo si è fermato
mentre un secondo thread scriveva il suo fault, come succede quando il primo
arriva a `svcBreak` e il kernel sospende il processo per il report.

### A.2 Perché 1 e 2 crashano e 3 no

**Memoria esaurita: no, per gli avvii 1 e 2.** Nei dati: la copia nella cache
non è mai partita (12 scritture in tutto, una sola dopo l'avvio), l'heap è a
2203 MiB su un massimo di circa 3100 (stima: il heap del processo meno codice
e bss; il run 21 la misura), Vulkan a 700 MiB su 1315 di budget, nessun
VK_ERROR. L'unico gradino è +50 MiB a 120–130 s, uguale nei due avvii.

**Quello che distingue 1 e 2 da 3 è la cache con i file dei run precedenti.**
Con la cache montata il gioco apre in lettura le copie lasciate dai run 18–19
(`cache:\anim.img` #508, `cache1:\props_mp.img`, `level_1.img`, … 50 file) e
le usa al posto di `game:`. A ~120 s ("Starting a new game", in tutti e due
gli avvii) riapre `cache:\anim.img` con disposition 5 (**sovrascrive, cioè
tronca a zero**) per ricopiarlo, mentre l'handle #508 sullo stesso file è
ancora aperto e lo streaming ci legge. I due crash arrivano 10–30 s dopo,
nello stesso punto del gioco. Il run 19 (GPU nulla) non è arrivato a questo
punto con la cache già piena, quindi non lo smentisce. Ipotesi, **da
verificare** nel run 21: con la cache montata ma le cartelle `cache` e
`cache1` svuotate prima dell'avvio, il crash non dovrebbe ripetersi; se si
ripete, la cache in sé (troncamento di un file aperto) è il problema.

**Seconda causa possibile, nota da masseffect-nx** (`docs/performance-history.md`
§1): quando un thread è dentro il gestore delle eccezioni utente e viene
interrotto, Horizon salva solo metà dei registri vettoriali; masseffect-nx ha
misurato il 3,6 % delle letture emulate con registri corrotti. Il nostro
ingresso salva e ripristina q0–q31 da sé, quindi la finestra è solo fra
l'ingresso e i salvataggi; ma il run 20 restava nel gestore per un `fsync`
a ogni fault (2955 volte), allungando di molto ogni passaggio. Un sp non
allineato nel fault dell'avvio 1 è compatibile con registri corrotti. La
correzione di D toglie l'`fsync` dai fault gestiti; il run 21 conta i fault
per finestra.

### A.3 Perché Atmosphère non ha scritto un report

- Avvio 1: il nostro percorso è arrivato fino a `rex_nx_unhandled_break`
  (riga su stderr scritta per intero, record completo, processo poi sospeso a
  metà del record di un altro thread): il report di creport dovrebbe esistere.
  **Da controllare** `sdmc:/atmosphere/crash_reports/` (e `fatal_reports/`)
  con l'ora 17:36–17:37; il run 11 il report l'aveva prodotto, ma non era
  stato copiato nella cartella del run, quindi "nessun report" finora vuol
  dire "nessun file copiato", non "nessun file scritto".
- Avvii 2 e 3: **nessun fault non gestito, nessuna terminazione**: il processo
  non è passato né dal gestore dei fault né da exit/abort. Restano: chiuso dal
  sistema (errore di un servizio, per esempio della GPU), un fault dentro il
  gestore stesso (scrive una riga solo su stderr, che con la console chiusa
  non si vede e può perdersi), o un blocco seguito dalla chiusura con HOME.
  **Serve sapere cosa mostrava lo schermo** (errore del sistema, ritorno al
  menu, immagine ferma) e se c'è un file in `crash_reports/`.
- Run 21: ogni uscita (exit, _exit, abort, svcExitProcess, std::terminate),
  ogni fault non gestito e ogni VK_ERROR di memoria o dispositivo scrive
  subito in `game-crash.log` tempo, memoria del processo, heap libero e le
  righe dell'ultimo battito (D).

### A.4 Il crash dell'avvio 3

In partita, non all'uscita: le ultime righe sono movimenti delle levette a
819–824 s, nessun + (nessun passo in `game-exit3.log`, vuoto). Il run 20 **non
legge i clock**: dal log non si può dire quando c'è stato l'overclock né a
quali valori. L'unico indizio: il thread id 18 (motore audio, carico costante)
resta a 75–76 % fino all'ultimo battito (812 s); con la CPU più veloce
scenderebbe. Quindi l'overclock è dopo 812 s o non ha toccato la CPU; il crash
segue entro ~12 s. Non distinguibile da un crash indipendente, ma due fatti
pesano: 800 s di partita senza crash prima dell'overclock, e la memoria
**quasi piena** (heap 2984 MiB in uso su ~3100, Vulkan 1395 di 1482 MiB),
ferma da 520 s. Dal run 21 il battito legge CPU, GPU e memoria (C) e il heap
libero (D).

### A.5 Fotogrammi

| Tempo | Riga del log (avvii 1 / 2 / 3) | PPM (avvio 3) |
|---|---|---|
| 20 s | 9,65 % / 0,00 % / 9,65 % non neri | logo "grand theft auto IV" su nero |
| 40 s | 92,9 % | schermata di caricamento disegnata (Niko e due uomini fra le casse) |
| 60 s | 100 % | schermata di caricamento disegnata (quattro uomini al tavolo dei domino) |
| 120 s | 0,36 % / 0,00 % / 0,36 % | nero con "STARTING A NEW GAME..." in basso a destra |
| 240 s | – / – / 0,00 % (max 5) | nero (inizio del video "The Cousins Bellic") |

Tutti e cinque sono schermate note del gioco, a 1280×720, cattura 76–668 ms.
**La fase (a) della strategia grafica (§11) è raggiunta:** logica del gioco
fino alla partita con la grafica imitata di ReXGlue su NVK.

### B. Dove va il tempo (avvio 3, Vulkan, cache spenta)

| Fase | Present/s | Draw per fotogramma | CPU guest / host | Thread più carichi |
|---|---|---|---|---|
| 10 s (logo) | 2,2 | 848 | 68 % / 65 % | GPU Commands 64 % (creazione delle pipeline) |
| 20–180 s (caricamento) | 1,4–4,8 | 147–176 | 13–142 % / 13–35 % | id 18 fino all'80 %, id 6 fino al 60 % |
| 190–290 s (video) | 0–0,3 | – | 160–186 % / 22–50 % | id 6 98 %, id 21 90–98 % (decodifica del video) |
| 300–520 s (partita) | 0,3–0,5 | 4.350–5.900 | 200–213 % / 21–34 % | id 6 **96 %**, id 18 77 %, id 19 19–34 % |
| 530–812 s (partita) | **0,4** (2,5 s per fotogramma) | 5.300–6.800 | 103–122 % / 36–43 % | id 18 76 %, id 19 22–36 %, GPU Commands 20–25 %, XMA 15–17 % |

Misure, non deduzioni:

- **GPU Commands** (il processore dei comandi di ReXGlue) usa 20–25 % di un
  core per ~22.500 draw ogni 10 s: **~98 µs di CPU per draw**. Con 5.600 draw
  per fotogramma sono 0,55 s di CPU per fotogramma: da solo permetterebbe
  ~1,8 FPS, non 0,4. Il thread passa tre quarti del tempo fermo.
- Dopo 530 s **nessun thread è saturo** (totale ~1,4 core su 3) e il thread
  principale è dentro `NtWaitForSingleObjectEx` da 3,2 s nell'istantanea a
  780 s, come quasi tutti i thread del gioco: aspettano il fotogramma.
- Il thread id 18 (motore audio, `8216xxxx`) è al 76 %: il callback audio
  costa **6,7 ms di media** (17,8 max) per 149 callback/s, contro 1,0 ms al
  menu; 21 risincronizzazioni. Spin lock: 155.000/s di
  `KeRaiseIrql/KeAcquireSpinLock…`, 1,86 milioni di `KeDelayExecutionThread`
  da zero dall'avvio (yield). Mangia un core, ma non è il limite del
  fotogramma.
- Tracciamento delle export (avvio 1 contro 2, 60–110 s): id 18 8,3–8,5 %
  contro 7,8–8,0 %, id 19 4,0–4,2 % contro 3,6–4,0 %: ~0,5 punti, coerente con
  la stima del run 19 (~5 % del thread).

**Conclusione per ora: il limite non è la CPU né l'I/O** (letture 0–0,3 MB/s
in partita); il processore dei comandi e il gioco aspettano. Resta da sapere
se aspettano la GPU (esecuzione dei draw, i 42 resolve per fotogramma) o una
sincronizzazione: il run 20 non misura né le attese sulle fence né il clock
della GPU. Per questo il run 21 aggiunge:

- riga `vulkan time in the window`: tempo dentro `vkQueueSubmit`,
  `vkQueuePresentKHR` e le attese bloccanti sulle fence del processore dei
  comandi (totale e per fotogramma), CPU per draw del thread GPU Commands,
  draw per fotogramma, chiamate nvdrv di NVK (kickoff, attese e interrogazioni
  delle fence, nvmap vivi);
- riga `clocks`: CPU, GPU e memoria lette con clkrst, modo e configurazione
  apm (C);
- profiler a campioni (cvar `nx_profile`, spento): un campione al
  millisecondo di un thread occupato (pausa, contesto, ripresa), istogramma
  dei pc ogni 10 s come base+offset, `tools/switch-game/where.py --profile`
  li somma per funzione. Forma minima di `switch_perf.cpp` dell'SDK condiviso
  (sa2-nx, BSD-3-Clause, crediti nel file).

**Confronto con nfsmw-nx e Mass Effect** (stesso percorso: GPU imitata di
ReXGlue su NVK). nfsmw-nx: ~80 µs di CPU per draw, 2,0–2,2 FPS in gara, e
togliere tutto il lavoro della GPU non cambiava niente (limite CPU); ogni
resolve costava 3–4 ms di GPU. masseffect-nx: menu a 1,5–2,5 FPS, la partita
mai raggiunta con la GPU imitata; il primo renderer nativo stava a 2003 ms per
fotogramma, **100 % fermo su `vkWaitForFences`**, con la GPU a 307,2 MHz
(configurazione mai chiesta) ed early-Z spento. **Coerenti:** i nostri 98 µs
per draw (nfsmw 80) e 0,4 FPS con 2,5–7 volte i draw di una gara di nfsmw.
**Diverso da nfsmw:** il nostro processore dei comandi non è saturo, quindi
il limite è più a valle, come nel primo Mass Effect nativo; la prima cosa da
escludere è la GPU a 307,2 MHz (il run 20 non chiedeva 460,8). **Anomalo
nostro:** il callback audio a 6,7 ms (un core) e i fault del write-watch
passati per un `fsync` ciascuno (corretto).

### C. Clock (da verificare)

`tools/switch-game/nx_clocks.cpp` (da `switch_apm.cpp` di nfsmw-nx,
BSD-3-Clause): con `nx_apm_clock = true` (default) in portatile si chiede la
configurazione standard `0x92220008` (GPU 460,8 MHz, memoria 1331,2, CPU
1020), su un thread all'avvio e di nuovo a ogni passaggio dock/portatile (hook
`OnPerformanceMode`); in dock non si chiede niente (GPU 768). Il risultato si
controlla per 1,5 s: se la memoria si muove si torna alla configurazione di
prima, se la GPU non arriva a 460,8 la riga dice `NOT applied`. Niente modalità
boost, niente overclock. Ogni battito scrive `clocks: CPU … GPU … memory …;
handheld/docked, apm configuration 0x…`, con un avviso se la GPU è sotto
100 MHz (la modalità boost della CPU porta la GPU a 76,8 MHz) o la CPU sopra
1020 MHz (boost o overclock esterno).

**Nei test non si usano overclock esterni** (sys-clk, overlay e simili): il
log non saprebbe distinguere un effetto del nostro codice da uno del clock,
la modalità boost della CPU abbassa la GPU a 76,8 MHz, e il crash dell'avvio 3
è arrivato subito dopo uno di questi cambi (09 §12).

### D. Memoria e arresti senza traccia (da verificare)

- Battito: `host memory: … free: X MiB in the arena + Y MiB sbrk can add =
  Z MiB headroom`: quello che newlib può ancora dare (Vulkan e pool guest
  vengono da lì; il valore del processo è sempre 3185/3189); sotto
  `nx_mem_warn_mib` (64) una riga `memory LOW` scritta subito. Picco del pool
  guest nella riga `heartbeat`, memoria Vulkan nella riga `vulkan:`, nvmap vivi
  e creati nella riga `vulkan time`.
- `faults: N access violations since start, M in the window`.
- `CrashLogHandler` installato di nuovo dopo Runtime::Setup: scrive solo i
  fault che nessuno gestisce.
- In `game-crash.log`, a ogni fault non gestito, a ogni uscita (exit, _exit,
  _Exit, abort, svcExitProcess, std::terminate) e a ogni
  `VK_ERROR_OUT_OF_HOST_MEMORY` / `OUT_OF_DEVICE_MEMORY` / `DEVICE_LOST`:
  uptime, memoria del processo, heap libero all'ultimo battito e le righe
  clock/memoria/Vulkan dell'ultimo battito, con `fsync`. Dopo un VK_ERROR di
  memoria il gioco continua; `DEVICE_LOST` finisce già in `rex::FatalError`
  (abort, quindi terminazione registrata).
- Messenger `VK_EXT_debug_utils` solo per gli errori, sempre creato su Switch
  (`vulkan_instance.cpp`): Mesa in release dà i suoi errori (`nvkmd-switch:
  channel N lost`, allocazioni fallite) solo lì; finiscono in `game.log`.

### E. Cache

- Con la cache il gioco fa quello che fa una 360 con il disco rigido: copia i
  file di streaming (`.img`) da `game:` a `cache:`/`cache1:` (~1 GB, run 19)
  e poi legge le copie. Senza (`nx_mount_cache = false`) i `NtCreateFile`
  su `cache:\valid.txt` falliscono (`C000000F`) e il gioco legge tutto da
  `game:`, come una 360 senza disco rigido: avvio 3, 22 scritture (tutte
  all'avvio, 0,0 MB), nessuna copia.
- Tempi: identici fino a 120 s (logo a 20 s, schermate a 40 e 60 s, "Starting
  a new game" a 120 s in tutti e tre gli avvii); l'avvio 3 è poi entrato in
  partita (~300 s), gli altri due no.
- **Default ora `false`** (cvar, commit `a9727e1`). Cosa si perde: niente
  sulla Switch. Sulla 360 la copia serve perché il disco rigido è più veloce
  del DVD; qui sorgente e cache sono la stessa SD (86 MB/s sequenziale), e la
  copia occupava la SD per tutta la sessione con scritture da 14 ms l'una. Il secondo avvio non è più rapido nemmeno con la cache (06 §26
  B.4: il sequenziatore si spegne a 60 s in tutti e due gli avvii).

### F. Fase (b) della strategia grafica

Cosa serve per il tracciamento del D3D dell'eseguibile europeo (§11 b):
gli indirizzi delle funzioni D3D (creazione di shader, texture, render target,
`SetRenderState`, draw, `Resolve`, `Present`) ritrovati dalla v8 USA con il
confronto di sequenze (§26 C.1), un wrapper per funzione con una riga per
chiamata, e la conferma "chiamata vista con argomenti plausibili".

**Passo piccolo e sicuro da fare adesso: il censimento** (sa2-nx,
`docs/api-renderer.md` §1): contare per ogni funzione del range D3D le
chiamate e i word PM4 scritti da ogni chiamata esterna (puntatore di
scrittura del device prima e dopo). Non sostituisce niente, dice quante
funzioni D3D il gioco chiama davvero e quante scrivono PM4 per fotogramma
(SA2: 109 chiamate, 14 scrivono PM4). Trappola nota: il "range D3D" contiene
anche XAPI (heap, attese); va trovato il vero inizio. Non fatto in questo
giro.

### Run 21: cosa guardare (`out/console-runs/run21-build/`)

NRO del commit `a9727e1`: `switch-game.nro` (GPU nulla) e
`switch-game-vulkan.nro` (da rinominare `switch-game.nro`). **Nessun
overclock esterno**, console in portatile salvo l'avvio 4.

| Avvio | `game.toml` | Scopo |
|---|---|---|
| 1 | `nx_gpu = "vulkan"` | prestazioni con la GPU a 460,8 (riga `clocks: … applied 0x92220008`), righe `vulkan time` e `host memory … headroom`, fino alla partita, poi + |
| 2 | `nx_gpu = "vulkan"`, `nx_profile = true` | dove va il tempo: profilo in partita (≥ 2 minuti), poi + |
| 3 | `nx_gpu = "vulkan"`, `nx_mount_cache = true`, **cartelle `cache` e `cache1` cancellate prima** | il crash di 1–2 dipende dai file vecchi nella cache? |
| 4 | come 1, con un passaggio in dock e ritorno in portatile durante la partita | `clocks (dock/handheld change)` |

Dopo ogni avvio copiare `game.log`, `game-crash.log`, `game-exit.log`, il
`.toml`, `frames/` e **il contenuto di `sdmc:/atmosphere/crash_reports/`**, e
annotare cosa mostrava lo schermo alla fine.

### Run 21: risultati (2026-10-10, `out/console-runs/run22/1`–`4`, NRO del commit `a9727e1`)

La cartella del run si chiama `run22/` (sottocartelle 1–4). `frames/` è
dell'ultimo avvio che ha scritto i PPM.

| | Avvio 1 | Avvio 2 | Avvio 3 | Avvio 4 |
|---|---|---|---|---|
| `game.toml` | `nx_gpu = "vulkan"` | + `nx_profile = true` | `nx_gpu`, `nx_mount_cache = true` (cartelle dichiarate cancellate) | `nx_gpu`; Horizon OC attivo |
| Clock all'avvio | `applied 0x92220008`, GPU 307,2 → 460,8 MHz, memoria 1331,2 ferma, CPU 1020 | uguale | uguale | uguale, poi a 20 s **CPU 1785, GPU 768, memoria 1862,4**, modo `handheld` (configurazione apm sempre `0x92220008`) |
| Durata | 47,6 s | 938 s (+ alla fine) | 566,8 s | 53,1 s |
| Fine | **fault**: instruction abort a pc = 0, sp = `0x3fe8000016000016`, lr = base + `0x244cdd8` | `exit(0)` del modo D (12 passi in `game-exit.log`) | **`std::terminate (std::bad_alloc)`** → `abort` → `_exit(1)` | **fault**: pc = 0, sp = `0x13`, stesso lr |
| `game-crash.log` | record del fault + `state:` | due blocchi `termination: exit(0)`, `_exit(0)` | `termination: abort(0)`, `_exit(1)` | record del fault + `state:` |
| Heap libero (ultimo battito) | 973 MiB | **20 MiB** | **18 MiB** | 910 MiB |
| Vulkan (uso / budget) | 652 / 1309 MiB | 1467 / 1480 | 1463 / 1475 | 702 / 1316 |
| nvmap vivi | 81 (685 MiB) | 846 (1550 MiB) | 641 (1534 MiB) a 550 s | 83 |
| VkResult ≠ SUCCESS | nessuno | nessuno (submit 0, present 0) | nessuno | nessuno |
| Messenger di Mesa | nessun messaggio | nessuno | nessuno | nessuno |
| Altri errori | – | da ~650 s **5877 righe `Failed to allocate 2097152 bytes of Vulkan upload buffer memory`** + `IssueDraw failed`: i draw si perdono, i present si fermano | `memory LOW: 58 MiB` a ~510 s | – |

Avvio 4: stato di carica non registrato (il log non legge l'alimentazione);
apm dice `handheld` per tutto l'avvio, quindi non in dock. Il cambio dei clock
è fra 20 e 30 s (battito di 20 s: 1020/460,8/1331,2; di 30 s: 1785/768/1862,4)
e la riga lo segnala (`CPU ABOVE 1020 MHz`). Escluso dai confronti.

**Prova dei 307 MHz del run 20:** in tutti e quattro gli avvii la riga
`clocks (start-up)` legge **GPU 307,2 MHz prima della richiesta**: è il clock
di sistema in portatile, quindi il run 20 (che non chiedeva niente) ha girato
a 307,2.

#### Cosa spiega i quattro esiti

1. **Avvii 2 e 3: memoria esaurita.** L'heap scende da ~970 MiB liberi a 20 e
   18 MiB; Vulkan arriva al budget (1467/1480); gli nvmap crescono di continuo
   (81 → 846). Nel 3 una `new` fallisce (`bad_alloc`, abort registrato);
   nel 2 falliscono i buffer di upload Vulkan da 2 MiB, i draw vengono scartati
   e l'immagine si ferma: il "blocco senza crash". La crescita è quella della
   cache delle texture di ReXGlue fino al suo limite rigido
   (`texture_cache_memory_limit_hard` = 768 MB, soft 384), sopra i ~650 MiB di
   partenza (memoria condivisa e EDRAM): 650 + 768 supera quello che resta
   dopo il pool guest da 768 MiB. Il run 20 (GPU a 307 MHz, meno fotogrammi)
   ci arrivava più tardi: a 812 s era a 1395/1482. **Da verificare** nel run
   22 (avvio 7) con il limite rigido a 384 MB.
2. **Avvii 1 e 4: lo stesso fault nello stesso punto.** lr = base +
   `0x244cdd8` è `sub_829D3858` subito dopo la chiamata a `sub_829D76F0`:
   funzioni del D3D del gioco (device in r31, campi +48/+52/+56 = puntatori
   del command buffer, +14916/+14920) che chiedono altro spazio nel command
   buffer, e quindi lanciano il ring e aspettano la GPU. Il ritorno va a pc = 0
   con sp che non è un indirizzo (`0x3fe8000016000016` sembra un dato in
   virgola mobile; `0x13`): **registri dell'host corrotti, non un puntatore del
   gioco**. Il run 20 aveva già un fault di questo tipo (avvio 1: pc = sp
   ≈ `0x6ac97f…` dopo una chiamata in `sub_829B2AB0`) con il codice di
   allora, senza nessuna novità del run 21: il guasto **esisteva prima**.
   Stesso punto, tempo diverso (47 e 53 s; gli avvii 2 e 3, stessa build, lo
   superano): **guasto non deterministico con un punto ricorrente**, non un
   errore di logica che fallisce sempre. Candidato principale, misurato da
   masseffect-nx (`docs/platform-notes.md`, "Half of the vector registers are
   lost"): un thread interrotto dentro il gestore delle eccezioni utente
   riprende con q0–q7 e q16–q31 vecchi; con la GPU a 460,8 MHz il gioco fa più
   fotogrammi e più fault di write-watch al secondo (avvio 1: 87–95 ogni
   10 s; avvio 4 overcloccato: fino a 452), quindi più occasioni. GCC salva a
   volte registri generali nei registri vettoriali, il che spiegherebbe un
   x30/sp corrotti. **Regressione del codice del run 21: non dimostrata e poco
   probabile** (il tipo di fault c'era già); **ambiente**: solo l'avvio 4
   (overclock). La bisezione del run 22 lo separa.
3. **Ipotesi "file vecchi della cache": indebolita.** Nell'avvio 3 la cache
   montata ha aperto 61 file esistenti in sola lettura (anche `cache:\anim.img`
   #508), quindi le cartelle non erano vuote all'avvio; il gioco ha di nuovo
   ricreato `cache:\anim.img` con disposition 5 (#1234) mentre #508 era
   aperto, ha scritto 3911 volte (128 MB) e **non** è crashato in quel punto.
   Il crash degli avvii 1 e 2 del run 20 è più probabilmente lo stesso fault
   non deterministico del punto 2.
4. **Profiler (avvio 2): non è lui il blocco.** Ha campionato fino all'uscita
   (9.888 campioni ogni 10 s anche dopo 660 s, 0 pause rifiutate); il blocco
   coincide con gli errori di allocazione Vulkan. I thread id 22 e 23 al 99 %
   dopo il blocco girano in `sub_829CFE80` (D3D: attesa di spazio nel ring che
   la GPU non consuma più perché i draw falliscono). Nessun cambio di modo
   necessario.

#### Dove va il tempo (a regime, ≥ 120 s dopo il primo fotogramma di gioco)

Primo fotogramma di gioco (≥ 3.000 draw) a ~370–400 s; finestre 520–640 s
(avvio 2) e 520–550 s (avvio 3). Gli avvii 1 e 4 non arrivano in partita.

| | Avvio 2 | Avvio 3 |
|---|---|---|
| FPS / ms per fotogramma | 0,5–0,6 / 1.667–2.000 | 0,4–0,5 / 2.000–2.500 |
| Draw per fotogramma | 4.300–6.100 | 4.600–5.700 |
| `vkQueueSubmit` | 0,10 ms per chiamata | 0,09–0,10 ms |
| `vkQueuePresentKHR` | 0,18–0,22 ms | 0,19–0,23 ms |
| `vkWaitForFences` bloccanti del processore dei comandi | **8,0–9,8 s ogni 10 s** (6 attese da ~1,5 s) | **8,0–8,4 s** |
| CPU di GPU Commands | 0,9–1,3 s ogni 10 s = **39–43 µs per draw** | 0,8–1,7 s = 44–59 µs per draw |
| nvdrv (10 s) | ~25 kickoff, ~190 attese di fence per 8,5–8,8 s, ~50 interrogazioni | ~25 kickoff, ~180 attese per 7,6–9,1 s |

**Il limite è la GPU.** Il processore dei comandi passa 8–10 s su 10 fermo
in `vkWaitForFences`; il profiler lo conferma (GPU Commands: **81,7 % dei
campioni in `svcWaitSynchronization`**, lr `_waitObjectsImpl`); i thread del
D3D (id 21, 22) aspettano spazio nel ring (`sub_829CFE80`, `sub_829D7D40`);
id 19 dorme (73 % in `svcSleepThread`); id 18 (audio) è diviso fra attese e
NEON. Da 307,2 a 460,8 MHz il fotogramma passa da 2,5 s a 1,7–2,0 s, quasi in
proporzione al clock: è il tempo di esecuzione della GPU. La CPU per draw è
scesa da ~98 µs (run 20) a ~40 µs perché il conto del run 20 includeva la
creazione delle pipeline.

**Pipeline e shader** (ipotesi da verificare): nel run 21 non c'erano
contatori; dalle righe `Creating graphics pipeline state` dell'avvio 3: 542 in
tutto, quasi tutte fra 80 e 290 s (16–42 ogni 10 s, caricamento e video), in
partita 0–13 ogni 10 s. Il processore dei comandi in partita aspetta le
fence, non le pipeline. Nessuna cache su disco esiste su Switch (né quella
delle pipeline di ReXGlue, mai inizializzata, né quella di Mesa, spenta). Dal
run 22 la riga `pipelines:` dà conteggio, tempo in `vkCreateGraphicsPipelines`,
traduzioni degli shader e la parte sul thread GPU Commands.

#### Le morti senza report di Atmosphère

| Avvio | Che fine | Traccia |
|---|---|---|
| 1, 4 | fault non gestito → nostro `svcBreak` | record in `game-crash.log`, righe `[stderr]` |
| 2 | `exit(0)` del modo D | blocchi `termination` |
| 3 | `std::bad_alloc` → `abort` → `_exit(1)` | blocchi `termination`, riga `TERMINATION` |

Nessuna fine del run 21 è "senza traccia": tutte e quattro passano dai nostri
percorsi. Il report manca anche dopo `svcBreak` (1 e 4) e dopo `abort` (3, che
in newlib finisce in `_exit`, non in un'eccezione: lì nessun report è atteso).
Percorsi che potrebbero uccidere il processo senza passare da noi, cercati nei
binari linkati: in libnx chiamano `diagAbortWithResult` applet, init, thread,
sm, hid, newlib, virtmem, framebuffer, pad, random, usb_comms, dynamic, env,
default_window; chiamano `svcBreak` diag, mutex, levent, fatal, exception; in
Mesa (`libvulkan.a`) solo `abort` (vk_device, vk_queue, nvk_mem_stream, dag,
simulatori MME, xmlconfig, ASTC) e il runtime Rust di NAK (`abort`, `exit`,
`_exit`), tutti già registrati. Dal run 22 anche `fatalThrow`,
`diagAbortWithResult` e `svcBreak` scrivono nel crash log valore e chiamante
(da verificare). **Serve dall'utente:** il codice dell'errore di sistema
(formato `2XXX-XXXX`, nella schermata o in Impostazioni → Assistenza →
Cronologia errori) per ogni crash, e se in `sdmc:/atmosphere/` esistono
`crash_reports/`, `fatal_reports/` o `erpt_reports/` con file di oggi.

### Run 22: bisezione (`out/console-runs/run22-build/`, commit `6bc6c24`)

Base = configurazione dell'avvio 3 del run 20 (Vulkan, cache spenta, nessuna
novità del run 21): tutte le cvar nuove spente. Ogni avvio differisce dalla
base per **una sola** riga. File pronti: `game1.toml`–`game7.toml` (da
copiare come `sdmc:/switch/theft4/game.toml`). Console in portatile, niente
Horizon OC né altri strumenti, `switch-game-vulkan.nro` rinominato
`switch-game.nro`. Ogni avvio: fino a 5 minuti dopo "Starting a new game" o
fino al crash, poi +.

| Avvio | Differenza dalla base | Cosa dice |
|---|---|---|
| 1 | nessuna (base) | il fault presto c'è anche senza le novità? |
| 2 | `nx_apm_clock = true` | GPU a 460,8 |
| 3 | `nx_crash_log_after_setup = true` | ordine dei gestori |
| 4 | `nx_fault_count_handler = true` | gestore che conta |
| 5 | `nx_nv_wrap = true` | contatori nvdrv |
| 6 | `nx_vk_mesa_messenger = true` | messenger di Mesa |
| 7 | `texture_cache_memory_limit_hard = 384` | la memoria regge oltre i 10 minuti? |

Base (`game1.toml`):

```toml
nx_gpu = "vulkan"
nx_apm_clock = false
nx_nv_wrap = false
nx_fault_count_handler = false
nx_crash_log_after_setup = false
nx_vk_mesa_messenger = false
```

Limiti della base: il binario contiene comunque i wrapper link-time (nvdrv
come passaggio puro, `fatalThrow`/`diagAbortWithResult`/`svcBreak`, solo in
caso di uscita) e le righe di diagnosi nuove del battito; con
`nx_crash_log_after_setup = false` ogni fault di write-watch torna nel crash
log con un `fsync` come nel run 20. Poiché il fault presto è non
deterministico (2 avvii su 4 nel run 21), un avvio senza crash non assolve
una variabile: se la base crasha, la causa non è fra le novità; se nessun
avvio crasha, ripetere la base e il 2.

### Run 22: risultati (2026-10-10, `out/console-runs/run22/1`–`7`, NRO del commit `6bc6c24`)

Sette avvii, uno per variabile. Schermo: errore generico del sistema ("Il
software è stato chiuso a causa di un errore") in tutti e sette; nessun report
di Atmosphère (non si cerca più: si lavora solo con `game-crash.log`).
`nx_mount_cache` spento in tutti (riga `run 21 pieces … nx_mount_cache 0`).
`game-exit.log` vuoto in tutti: nessuna uscita è passata da `exit`/`abort`.
Le cartelle dei log del run 20 e del run 21 non ci sono più (le sottocartelle
`run22/1`–`4` del run 21 sono state riusate): per quei run valgono i valori
scritti sopra.

#### A.1 Avvio per avvio

| Avvio | Differenza dalla base | Durata | Fine | Heap libero / Vulkan (ultimo battito) | Ultimo record di `game-crash.log` |
|---|---|---|---|---|---|
| 1 | nessuna | 155,6 s | **nessun record della morte** | 915 MiB / 700 di 1317 | fault di write-watch gestito (+0x244e614) |
| 2 | `nx_apm_clock` | 195,1 s | nessun record della morte | 912 / 700 di 1315 | gestito (+0x2458dd4) |
| 3 | `nx_crash_log_after_setup` | 58,5 s | **fault**: instruction abort (esr `0x82000005`), pc = 0, sp = `0x13` | 973 / 652 di 1309 | il fault: lr = +0x244e368, x29 = `0x67d818d5d0` |
| 4 | `nx_fault_count_handler` | 57,2 s | **fault**: PC non allineato (esr `0x8a000000`), pc = far = `0x6acab702`, sp = `0x6acab6ca` | 976 / 650 di 1309 | il fault; il precedente (359 ms prima) è un gestito a +0x244e614 con sp `0x57c3e9b5f0` |
| 5 | `nx_nv_wrap` | 122,3 s | **fault**: PC non allineato, pc = `0x6acab7e2`, sp = `0x6acab769` | 925 / 700 di 1324 | precedente (310 ms prima): gestito a +0x244e614, sp `0x1b5aba45d0` |
| 6 | `nx_vk_mesa_messenger` | 186,9 s | **fault**: instruction abort, pc = 0, sp = `0x3638613335303030` (i byte sono il testo ASCII `00053a86`) | 912 / 700 di 1315 | precedente (311 ms prima): gestito a +0x244e970, sp `0x19d3b125d0` |
| 7 | `texture_cache_memory_limit_hard = 384` | 338,5 s | nessun record della morte | **194** / **1369 di 1499**, present 0/s (video) | gestito (+0x244a0e0) |

Il record non ha l'id del thread (aggiunto ora, D.2); il thread si riconosce
dallo sp: in ogni avvio i fault vengono da due stack (avvio 1: `0x22659b…` 388
record, `0x226b19…` 48), e x29 dei record fatali è lo sp del primo. È un thread
guest: registri con indirizzi guest (`0x7030f…` stack guest, `0x4006…`,
`0xbf…` memoria fisica guest), codice del gioco.

**Frequenza dei fault di write-watch:** negli avvii che scrivono ogni fault
(tutti tranne il 3) **circa 3 al secondo, a intervalli regolari di 300–360
ms**; il contatore dell'avvio 4 dice lo stesso (26–32 ogni 10 s). Non è la
frequenza del gioco: è il tetto messo dal record con `fsync` (~300 ms) scritto
**dentro** il gestore, con la claim del kernel tenuta. Il thread che fa il
fault passa quindi quasi tutto il suo tempo nel gestore delle eccezioni, e gli
altri thread che fanno un fault aspettano nel kernel. Avvio 3 (record solo per
i fault non gestiti): frequenza ignota, ma il gioco va il doppio più veloce
(289 present a 50 s contro 150 dell'avvio 4).

#### A.2 Il fault ricorrente

Il fault gestito è nel gioco, `sub_829D3858` (+0x244e614: `str w1, [x2, w8,
uxtw]`, una scrittura su memoria guest sorvegliata; +0x244e970 è nella stessa
funzione). **lr = +0x244e368 non dice che il crash viene da `sub_829D76F0`**: è
l'indirizzo di ritorno rimasto in x30 dalla `bl sub_829D76F0` di +0x244e364,
già tornata; da lì la funzione prosegue e fa la scrittura che va in fault.

| Crash | sp nello stack del thread? | pc, lr, x29 | Chi porta a pc sbagliato | Fault al secondo |
|---|---|---|---|---|
| run 20 avvio 1 | no: `0x6ac97f5d` | pc = far = `0x6ac97ff4` no; lr nel gioco (`sub_829B2AB0`) sì | – (record precedente non più disponibile) | 1 ogni ~300 ms (fsync per fault) |
| run 21 avvio 1 | no: `0x3fe8000016000016` | pc = 0 no; lr = +0x244cdd8 (stesso punto, binario del run 21) sì | – (solo i fault non gestiti nel log) | 87–95 ogni 10 s |
| run 21 avvio 4 | no: `0x13` | pc = 0; lr come sopra | – | fino a 452 ogni 10 s (overclock) |
| run 22 avvio 3 | no: `0x13` | pc = 0; lr +0x244e368 sì; x29 sì | x5, x20, x24… uguali al motivo dei gestiti a +0x244e614 | ignota (nessun record dei gestiti) |
| run 22 avvio 4 | no: `0x6acab6ca` | pc = `0x6acab702` no; lr, x29 sì | **nessuna istruzione eseguita**: x0–x30 identici al gestito precedente | ~3/s |
| run 22 avvio 5 | no: `0x6acab769` | pc = `0x6acab7e2` no; lr, x29 sì | come sopra | ~3/s |
| run 22 avvio 6 | no: testo `00053a86` | pc = 0; lr, x29 sì | come sopra | ~3/s |

Nei tre casi confrontabili (4, 5, 6) il record fatale ha **x0–x30 identici,
registro per registro, a quelli del fault gestito precedente** sullo stesso
thread, 310–360 ms prima: il thread è stato ripreso e non ha eseguito nessuna
istruzione. Sbagliati sono **solo sp e pc**, cioè proprio i due valori che il
kernel ricarica dal frame dell'eccezione (`ThreadExceptionFrameA64`, +0x50 e
+0x58) in `svcReturnFromException`; x0–x8 e lr, che vengono dallo stesso
frame, sono giusti. Nel binario del run 22 il dispatcher scriveva quei due
valori **con un solo registro vettoriale**:

```
rex_nx_exception_dispatch+0x1028:  add  x0, sp, #0x1e8     // &thread_context.sp (sp, pc)
rex_nx_exception_dispatch+0x102c:  ldr  q31, [x0]
rex_nx_exception_dispatch+0x1030:  str  q31, [x20, #80]   // frame->sp, frame->elr_el1
```

e i valori trovati sembrano il contenuto di un registro vettoriale di altro
codice: coppie di numeri a 32 bit vicini (`0x6acab6ca`/`0x6acab702`, già visti
nel run 20 come `0x6ac97f5d`/`0x6ac97ff4`), testo ASCII, piccoli interi.

#### A.3 La matrice

Gli avvii 1, 2, 5 e 6 equivalgono alla base: **4 su 4 morti fra 122 e 195 s**
(due con il record del pc sbagliato, due senza record). Quindi la base crasha
e nessuna delle cinque novità è necessaria al crash. Gli avvii 3 e 4 muoiono
prima (57–58 s, durante le immagini iniziali):

- **3** sposta il record dopo il gestore MMIO: i fault gestiti non passano più
  dal record con `fsync`, il tempo nel gestore per fault scende da ~300 ms a
  qualche decina di µs, il gioco va il doppio più veloce e fa più fault al
  secondo (non contati);
- **4** aggiunge solo un contatore atomico in testa alla lista dei gestori; il
  record con `fsync` per ogni fault resta come nella base. Il percorso dei
  fault è di fatto quello della base.

Con un campione per avvio **non si può dire niente su nessuna variabile**: lo
stesso crash arriva a 57 s con una modifica che non cambia il percorso (4) e a
122–195 s nella base; la dispersione dei tempi è quella di un guasto casuale.
Il run 23 ripete la base per stimare la frequenza.

#### A.4 Le tre cause

- **(a) Registri corrotti nel percorso delle eccezioni: sostenuta dai log.**
  Ogni record fatale è la ripresa di un fault gestito con tutti i registri
  generali giusti tranne la coppia sp/pc, scritta nel frame con un solo
  registro vettoriale (`q31`), e i valori sbagliati hanno l'aspetto di dati.
  Due varianti che questi log non separano: (a1) `q31` cambia fra la `ldr` e
  la `str` (il thread nel gestore perde q0–q7 e q16–q31 se viene interrotto,
  masseffect-nx; finestra di un'istruzione, improbabile a ogni fault ma non
  impossibile); (a2) la copia in memoria (`thread_context.sp/pc`, sullo stack
  delle eccezioni) viene sovrascritta nei ~300 ms in cui il thread resta nel
  gestore. Il run 23 le separa (D.2).
- **(b) Overflow dello stack di un thread host (Vulkan/Mesa/NAK): nessun
  indizio.** Il thread che muore è un thread guest; il suo sp prima del fault
  sta fermo nello stesso KiB per minuti (`…5d0`, `…5f0`, `…3f0`); gli stack
  host degli XThread sono da 16 MiB; nessun fault ha far vicino a sp; i thread
  di Mesa e NAK non compaiono mai come thread del fault. Misurato comunque dal
  run 23 (C.3).
- **(c) Chiamata indiretta attraverso una voce nulla della tabella delle
  funzioni: contraddetta.** Una `blr` a 0 lascia sp valido e lr subito dopo la
  `blr`; qui sp è sbagliato, lr è il ritorno di una `bl` diretta, negli avvii 4
  e 5 il pc non è 0 (è dispari) e fra il fault gestito e il crash non è stata
  eseguita nessuna istruzione.

**Le morti senza record (1, 2, 7).** L'ultimo record è un fault gestito,
scritto prima che il gestore MMIO lo trattasse; della morte nessuna traccia:
né record di fault, né `termination`, né il wrapper di `svcBreak`, né righe
`[stderr]`. È compatibile con un fault **dentro** il gestore: a memoria (da
verificare sul codice di Mesosphere) `KProcess::EnterUserException` rifiuta il
thread che tiene già la claim, quindi un secondo fault sullo stesso thread non
arriva al nostro ingresso (il ramo `rex_nx_exception_nested` non girerebbe
mai) e il kernel chiude il processo. Lo stesso meccanismo di (a) colpirebbe in
quel caso un puntatore del gestore invece della coppia sp/pc. Ipotesi. Avvio 7:
in più la memoria era quasi finita (heap 194 MiB, in uso 2919 MiB contro ~2200
degli altri a 150–190 s; Vulkan 1369/1499): il limite rigido a 384 MB non ha
fermato la crescita, ma con un solo avvio e senza record la causa della morte
resta aperta.

#### C. T10 esteso e misura degli stack (da eseguire)

- **T10** (switch-smoke, `probes.cpp`): prima della scrittura che va in fault
  ogni thread carica valori unici per thread e giro in x0–x30, sp, nzcv,
  fpcr, fpsr, tpidr_el0 e v0–v31; dopo la ripresa li salva tutti e li
  confronta. Il pc di ripresa si controlla con il record per thread
  dell'ingresso (`rex_nx_thread_faults`): un fault per scrittura, preso e
  ripreso all'indirizzo della `str`. Fase A: 3 thread × 10.000 fault, uno
  per core (come prima). Fase B: gli stessi 3 thread per 20 s con un thread
  che tiene occupati q0–q31 e li controlla e uno che scende di 1500 frame e
  li verifica al ritorno; i due girano una priorità sopra, dormono 30–40 µs
  fra un giro e l'altro e cambiano core, quindi interrompono i thread dei
  fault anche dentro il gestore. Riga finale per tipo di registro; un thread
  ripreso con pc/sp sbagliati non fa cadere lo smoke (un gestore lo conta e
  lo rimanda alla fine del giro). Il T10 di prima non vedeva il problema per
  costruzione: un thread per core e nessuna interruzione, e i registri
  controllati erano solo x0–x28 e q8–q15, quelli che il kernel salva comunque.
- **Stack** (switch-game, `nx_threads.cpp`, cvar `nx_stack_watch`, accesa):
  wrap di `threadCreate`, `threadClose` e `pthread_create`, quindi tutti i
  thread, anche quelli di Mesa e del runtime Rust di NAK (passano da
  `pthread_create` di newlib, che usa `threadCreate`). Lo stack di ogni thread
  nuovo (`[stack_mirror, stack_mirror + stack_sz)`, sopra la cima libnx mette
  TLS e reent) viene riempito con un motivo prima della partenza, quello del
  thread principale sotto lo sp all'avvio; ogni minuto una riga per thread
  `stack id … 'nome': size … KiB, deepest use … KiB (…%)` (con `NEAR FULL` dal
  75 %, `FULL` se il motivo è sparito fino in fondo) e la routine di partenza
  dei pthread (`start +0x…`, per addr2line), più una riga `stacks:`.

Risultati: dal run 23.

#### D. Correzioni (da verificare)

1. **Vettori salvati per primi** (`__libnx_exception_entry`): q0–q31, fpsr e
   fpcr salvati in `rex_nx_vector_save` con le primissime istruzioni (prima
   del controllo dell'annidamento e del cambio di stack) e ripristinati come
   ultime, subito prima di `svc 0x28`.
2. **sp e pc del frame scritti con registri generali.** Effetto collaterale
   del codice di D.2: GCC ha smesso da sé di copiare la coppia con `q31`; la
   scrittura è stata resa esplicita (due store a 64 bit da registri generali)
   perché non dipenda dal compilatore. Non si spegne a runtime, come D.1: il
   run 23 prova **D.1 e D.2 insieme**; se il crash sparisce, per separarle
   servirà una build con una delle due tolta.
3. **Record del crash** (`CrashFinalHandler`, installato per ultimo dopo
   Runtime::Setup, quindi solo per i fault che nessuno gestisce): nome e id
   del thread, intervallo del suo stack, se sp e x29 ci stanno dentro,
   l'ultimo fault ripreso su quel thread (sp, pc e lr con cui era arrivato,
   sp e pc con cui è stato ripreso, indirizzo del frame, ms trascorsi),
   i contatori di processo e un'istantanea di tutti gli altri thread (nome,
   pc, lr, sp, routine di partenza: ognuno messo in pausa un attimo, letto e
   ripreso prima di scrivere), poi `fsync`, prima di `svcBreak`.
4. **Record di ripresa** (`rex/platform/nx_fault_record.h`): a ogni fault
   gestito l'ingresso confronta sp e pc della copia con quelli arrivati dal
   kernel (contatore `context changed`, variante a2) e rilegge il frame dopo
   averlo scritto (contatore `frame wrong`, variante a1); battito: riga
   `resume:`. Tutto con load/store a 64 bit volatili, mai in registri
   vettoriali. `nx_resume_guard` (spenta) rimetterebbe nel frame sp e pc di
   ingresso: pronta per un run successivo, non usata nel 23.
5. **Stack dei thread:** nessun overflow nei log, quindi nessuna modifica
   (D.3 non applicata; non esiste ancora una cvar `nx_host_stack_kib`).

Disassemblato della build nuova: l'ingresso comincia con `stp q0, q1, [x2]`
dopo due istruzioni (`adrp`/`add`) e finisce con `ldp q30, q31` e `svc 0x28`;
nel dispatcher tutte le scritture a `[frame, #80]`/`[frame, #88]` sono `str x`.

### Run 23: frequenza del crash con le correzioni (`out/console-runs/run23-build/`)

NRO e ELF del commit indicato in `COMMIT`. `switch-game.nro` è la build
Vulkan; `switch-smoke.nro` è lo smoke con il T10 esteso. Console in
portatile, **niente Horizon OC, sys-clk né altri strumenti**, cache spenta.

**Prima i cinque avvii del gioco, tutti uguali** (`game.toml` della cartella,
copiato come `sdmc:/switch/theft4/game.toml`; è la base del run 22 con le due
cvar nuove scritte esplicitamente):

```toml
nx_gpu = "vulkan"
nx_apm_clock = false
nx_nv_wrap = false
nx_fault_count_handler = false
nx_crash_log_after_setup = false
nx_vk_mesa_messenger = false
nx_mount_cache = false
nx_stack_watch = true
nx_resume_guard = false
```

Per ogni avvio 1–5:

1. Prima dell'avvio cancellare da `sdmc:/switch/theft4/` i file
   `game.log`, `game-crash.log` e `game-exit.log` dell'avvio precedente
   (dopo averli copiati).
2. Avviare `switch-game.nro`, non toccare niente fino a "Starting a new
   game", poi lasciare girare **5 minuti** o fino al crash.
3. Se dopo 5 minuti non è crashato: + (uscita del modo D).
4. Annotare: ora di avvio, cosa mostrava lo schermo alla fine (errore del
   sistema, immagine ferma, ritorno al menu), e il tempo approssimativo del
   crash.
5. Copiare `game.log`, `game-crash.log`, `game-exit.log` e `game.toml` in
   `out/console-runs/run23/N/`.

Confronto: run 22, avvii 1, 2, 5 e 6 (base): 4 su 4 morti fra 122 e 195 s.
Con 5 avvii: 0 crash in 5 avvii sarebbe un segnale forte (con la frequenza di
prima la probabilità di 5 avvii puliti di fila è bassa); 1–2 crash in 5 un
miglioramento possibile ma non dimostrato; 4–5 nessun effetto. In ogni caso
il record nuovo dice dove si rompe la coppia sp/pc.

**Poi lo smoke, una volta** (`switch-smoke.nro` con lo `smoke.toml` della
cartella, che spegne il passo Vulkan e il benchmark della SD): avviarlo,
aspettare la riga `PROBES DONE` (T10 dura circa 30 s più la fase A), poi +;
copiare `smoke.log` in `out/console-runs/run23/smoke/`.

Cosa guardare:

- `game-crash.log`: il blocco `=== unhandled fault: thread, stack, last
  resume ===`. `this fault's pc is the resumed pc: yes` con `resumed with pc`
  sbagliato = il frame era già sbagliato quando l'abbiamo scritto;
  `resumed with` giusto ma il pc del fault diverso = la coppia è cambiata
  dopo la nostra scrittura (kernel o frame sovrascritto); i contatori
  `context changed` / `frame read back wrong` dicono quale delle due varianti.
- `game.log`: righe `resume:` (ogni 10 s) e `stack id …`/`stacks:` (ogni
  minuto).
- `smoke.log`: `T10 phase A RESULT` / `T10 phase B RESULT discrepancies by
  register …`: qualunque valore diverso da 0 dice quale registro il percorso
  delle eccezioni non restituisce.

### Run 23: risultati (2026-10-11, `out/console-runs/run23/1`–`5` e smoke, NRO del commit `9fe69f7`)

Cinque avvii identici (il `game.toml` qui sopra), poi lo smoke. Note
dell'utente: **5 crash su 5**; quattro "subito dopo Starting a new game, circa
nello stesso punto", nel quinto quasi tutto il filmato visto prima del crash.
**Attenzione:** la base aveva `nx_crash_log_after_setup = false`, quindi il
crash log restava primo e scriveva ogni fault gestito con `fsync`: il run 23
ha provato D.1 e D.2 **con il gestore lento**, non il gestore veloce.

#### A.1 Avvio per avvio

La durata è il tempo dell'ultimo record di `game-crash.log` (scritto con
`fsync`, quindi un limite inferiore della morte); `game.log` è bufferizzato e
perde l'ultimo secondo, quindi l'ultimo battito è più vecchio.

| Avvio | Ultimo record | Ultimo battito | Fase (dal log) | Fault ripresi | Heap libero / Vulkan (ultimo battito) | Record della morte |
|---|---|---|---|---|---|---|
| 1 | 92,5 s | 80 s | caricamento, 3,6 present/s | 214 (a 80 s) | 925 MiB / 700 di 1324 | **nessuno** |
| 2 | 139,7 s | 130 s | caricamento, 2,5 present/s | 364 | 918 / 700 di 1319 | nessuno |
| 3 | 274,5 s | 270 s | filmato (present 0,0–0,3/s da ~210 s) | 583 | 438 / 1151 di 1446 | nessuno |
| 4 | 297,9 s | 290 s | filmato | 638 | 227 / 1357 di 1510 | nessuno |
| 5 | 535,2 s | 530 s | filmato (0,2–0,4 present/s) | 837 | 164 / 1371 di 1481 | nessuno |

In tutti e cinque `game-exit.log` è vuoto, nessuna riga `termination`,
nessun blocco `=== unhandled fault: thread, stack, last resume ===`, nessun
`[stderr] [rex]` (l'unica riga `[stderr]` è l'avviso di NVK all'avvio),
nessun `memory LOW`, nessun VkResult di errore. L'ultimo record di ogni avvio
è un fault di write-watch come gli altri (`sub_829D…`: +0x2452c74/+0x2452fd0
negli avvii 1–2, +0x245d40c/+0x2463810/+0x245d434 nel filmato), completo
fino alla riga `vulkan:`. **Tutte e cinque le morti sono "senza record"**: il
fault che uccide il processo, se c'è, non arriva al nostro dispatcher.

Tempi: 92–535 s (run 22, base: 122–195 s). I tempi del log non coincidono con
"quattro nello stesso punto": l'1 e il 2 muoiono durante il caricamento
(present 2–4/s, memoria ferma), il 3 e il 4 nel filmato (present quasi a 0,
memoria Vulkan in crescita da ~210 s), il 5 molto più avanti nel filmato.
Il filmato è lento: nel run 20 durava 190–290 s, qui va da ~210 s oltre i
530 s.

**Record di ripresa e le due varianti di (a):** in tutti gli avvii, a ogni
battito, `sp/pc changed in the handler 0, frame read back wrong 0, repaired
0`, su 214–837 fault ripresi per avvio (2.636 in tutto). Il valore riletto
dal frame è sempre quello scritto, e la copia di sp/pc non è mai cambiata
durante i gestori. **Nessuna delle due varianti si è vista**, e nessuna morte
è stata la ripresa di un fault con sp/pc sbagliati (run 22: 4 su 7, 2 su 4
nella base). Con D.1 e D.2 la forma "sp/pc sbagliati" è sparita: coerente con
la variante a1 (la coppia copiata con `q31`, e `q31` è fra i registri che il
kernel può perdere, A.3), ma con 5 avvii è **da verificare**. Domanda "prima o
dopo il nostro codice": nel run 23 non ci sono valori sbagliati da
localizzare; nel run 22 il valore sbagliato nasceva **nel nostro codice**
(copia della coppia con `q31` prima della scrittura del frame), non nel
kernel dopo.

**Due record rovinati** (3: tre byte `00 11 89` prima di `last heartbeat:`;
5: intestazione tagliata a `=== unhandled access viola` seguita dal record
dopo, e un `module base 0x` senza le cifre seguito da un byte nullo). Con la
claim un solo thread alla volta scrive, quindi non è concorrenza fra due
record. Causa non stabilita; il gestore nuovo (B) non scrive più nel
percorso dei fault gestiti.

#### A.2 Stack dei thread (`nx_stack_watch`)

Uso massimo in tutti e cinque gli avvii (36 thread vivi, 477 MiB di stack):

| Thread | Stack | Uso massimo |
|---|---|---|
| GPU Commands (XThread host) | 16 MiB | 66 KiB (0,4 %) |
| main (UI) | 1 MiB | 58 KiB (5,7 %) |
| 3 thread `rex::thread::Thread` senza nome (routine `ThreadStartRoutine`, `threading_switch.cpp`) | 4 MiB | 55 KiB |
| XMA Decoder | 16 MiB | 16 KiB |
| thread dei clock (`nx_clocks.cpp`) | 31 KiB | 7 KiB (22,7 %) |
| Main XThread, gli XThread del gioco (24) | 16 MiB | 2–7 KiB |
| Kernel Host Tasks, Audio Worker, GPU VSync | 16 MiB | 3–4 KiB |
| log writer | 63 KiB | < 1 KiB |
| un `std::thread` (`execute_native_thread_routine`) | 127 KiB | non dipinto (creato prima del filtro) |

**Nessun thread di Mesa o di NAK** nella tabella: in questo percorso NVK non
crea thread propri (compilazione degli shader sul thread chiamante). Nessuno
stack sopra il 23 %: **(b) overflow di uno stack: escluso per questi thread.**
Resta non misurato lo stack delle eccezioni (`__nx_exception_stack`, 64 KiB),
su cui gira tutto il gestore.

#### A.3 Smoke: T10 esteso

| | Fase A (3 thread × 10.000 fault, nessun disturbo) | Fase B (20 s, thread vettoriale e thread a chiamate profonde a priorità 43) |
|---|---|---|
| Fault | 30.000, 77,7 µs per fault a thread (serializzati), 26 µs ciascuno | 2.219 (111/s) |
| x0–x28, x29, x30, sp | 0 | 0 |
| nzcv, fpcr, fpsr, tpidr_el0 | 0 | 0 |
| v0–v7 | 0 | **32** |
| v8–v15 | 0 | 0 |
| v16–v31 | 0 | **64** |
| pc di ripresa | 0 (sempre la `str`) | 0 (sempre la `str`) |
| Record dell'ingresso | 30.000 ripresi, 0 cambiati, 0 riletti sbagliati | 2.219, 0, 0 |
| Thread di disturbo | – | 35.435 giri vettoriali e 35.427 discese di 1500 frame: 0 errori |

Fase B: **2 giri su 740 del thread 0** sbagliati; in ciascuno **tutti** i
registri q0–q7 e q16–q31 (16 + 32 metà da 64 bit) valgono **0**, v8–v15 e
tutto il resto giusti. Riga finale `T10 RESULT UNEXPECTED`. Il pc di ripresa
è sempre l'istruzione giusta. I thread di disturbo, mai dentro il gestore,
non perdono niente.

Lettura: è la firma di un thread che il kernel ha tolto dalla CPU mentre era
**dentro una SVC**, per la quale salva solo i registri vettoriali
callee-saved (q8–q15) e restituisce gli altri azzerati. D.1 salva i vettori
con le prime istruzioni e li ripristina subito prima di `svc 0x28`, quindi
quello che resta scoperto è **`svc 0x28` stessa** (svcReturnFromException:
rilascia la claim, può svegliare e far girare un thread a priorità più alta)
più le poche istruzioni prima che l'ultima `stp` del salvataggio sia
eseguita. Ipotesi sulla finestra, **da verificare**; il fatto (q0–q7 e
q16–q31 a zero dopo un fault ripreso, 0,09 % dei fault con interruzioni) è
misurato. Nessun ordine di salvataggio dentro l'ingresso chiude una perdita
che avviene dentro la SVC di ritorno.

Conseguenza per il gioco (ipotesi): un thread del gioco ripreso dopo un
write-watch può trovare a zero i registri vettoriali caller-saved che il
codice generato teneva vivi attraverso la `str` (valori in virgola mobile,
copie con `ldp q`, a volte registri generali parcheggiati in `d16`–`d31`):
dati sbagliati senza fault immediato. Non spiega da solo le morti senza
record.

#### A.4 Tempo per fault gestito e gestori nel percorso

Il run 23 non misurava il tempo nel gestore. Dai record: fault gestiti a
intervalli di **min 252–289 ms, mediana 322–331 ms**, ~3 al secondo durante
il caricamento (27–31 ogni 10 s, contatore `resume:`), 0–2 ogni 10 s nel
filmato. Il thread che fa i fault ricomincia subito dopo la ripresa, quindi
~300 ms è il tempo nel gestore per fault, quasi tutto I/O. Riferimento:
smoke T9, un fault gestito senza I/O costa 13,7 µs (percorso delle eccezioni
7,7 µs): **~20.000 volte meno**.

Gestori nel percorso di un fault gestito (configurazione del run 23):

| Ordine | Gestore | Cosa fa | I/O |
|---|---|---|---|
| 1 | `CrashLogHandler` (switch-game, primo perché `nx_crash_log_after_setup = false`) | record di ~1,8 KB con ~40 `write` su `game-crash.log`, `fsync`; poi, se il log è bufferizzato, `open`/`write`/`fsync`/`close` di `game.log` con tutto il buffer (`open` di newlib alloca la voce del descrittore) | **sì** |
| 2 | `MMIOHandler::ExceptionCallbackThunk` (runtime) | `global_critical_region_` (mutex ricorsivo), `QueryProtect`; `Memory::NxPhysicalAccessViolation` → `PhysicalHeap::TriggerCallbacks` dei tre finestre → callback di invalidazione di `SharedMemory` e `PrimitiveProcessor`, `FireWatches` (cache delle texture), `Protect` | no; due righe spdlog sui rami rari (`Recovered stale physical page protection`, gestito; `Unhandled guest physical write fault`, non gestito): 0 nel run 23 |
| 3 | `CrashFinalHandler` | solo per i fault non gestiti | (solo non gestiti) |
| – | dispatcher dell'SDK | nessun I/O per i gestiti; `write` su stderr solo per i non gestiti | no |

Altre cvar di diagnostica che mettevano I/O o altro nel percorso del fault:
solo `nx_crash_log_after_setup = false` (sopra). `nx_fault_count_handler =
true` metteva un gestore **prima** del runtime (solo un contatore atomico, nessun
I/O). `nx_log_durable = true` renderebbe immediata (apertura e `fsync`) ogni
riga di log, comprese le due righe spdlog dei rami rari. `nx_profile` non è
nel percorso, ma mette in pausa thread qualunque, anche uno che tiene la
claim. `nx_stack_watch`, `nx_trace_exports`, `nx_nv_wrap`,
`nx_vk_mesa_messenger`, `nx_apm_clock`: non nel percorso.

#### B. Percorso del fault gestito senza I/O (da verificare)

- **SDK** (`exception_handler_switch.cpp`, `nx_fault_record.h`): il
  dispatcher scrive ogni fault in un anello in memoria di 32 voci
  (`RexNxFaultRing`: numero, tempo, durata, handle del thread, tipo, pc, lr,
  sp, far, esr, x0–x30 all'arrivo, pc e sp di ripresa, gestito o no) e conta
  tempo totale e massimo dei fault gestiti (dall'inizio del dispatcher al
  ritorno; l'assembly dell'ingresso e il kernel restano fuori) e i fault non
  gestiti. Store semplici, nessuna chiamata, nessun lock (la claim basta).
  Ingresso in assembly, D.1 e D.2: **invariati**.
- **SDK** (`xmemory.cpp`): le due righe spdlog di
  `NxPhysicalAccessViolation` diventano contatori
  (`RexNxPhysicalFaultStats`: pagine riprotette, fault fisici inspiegati,
  ultimo indirizzo).
- **switch-game:** `nx_crash_log_after_setup` e `nx_fault_count_handler`
  **tolte** (in un `.toml` vecchio danno solo `Config: unknown cvar`).
  `CrashLogHandler` è installato all'avvio (per un fault prima di Setup, quando
  non esiste un gestore del runtime) e **sempre** spostato dopo i gestori del
  runtime subito dopo Runtime::Setup, seguito da `CrashFinalHandler`: vedono
  solo i fault non gestiti. Su disco l'anello va solo con un fault non gestito
  (dopo il record completo, che resta com'era), `exit`/`_exit`/`abort`/
  `svcExitProcess` (anche l'uscita col +), `fatalThrow`,
  `diagAbortWithResult` e `svcBreak` (anche un fault dentro il gestore, che
  finisce in `svcBreak`): blocco `=== fault ring: … ===`, il più recente per
  primo, con il nome del thread.
- **Battito:** `faults: N handled since start, M in the window (X/s); time per
  handled fault in the window: mean … ns, max … ns; since start mean … ns;
  unhandled …; physical: stale protection recovered …, unexplained …`.
- **Avvio:** `build: commit …, Vulkan NRO; config read: game.toml`, poi
  `build: Switch cvars (n): …` (tutte le cvar della categoria Switch con il
  valore) e `build: cvars not at the default (n): …` (ogni categoria). Il
  commit viene da `build_commit.cmake`, ricontrollato a ogni build
  (`-dirty` se c'erano modifiche non salvate).

Effetto atteso (ipotesi): il gioco smette di passare ~90 % del tempo del
thread dei fault nel gestore e fa molti più fault al secondo (il run 22,
avvio 3, andava il doppio più veloce). Due effetti opposti sulle morti: meno
tempo dentro il gestore (meno occasioni per un fault annidato, ipotesi delle
morti senza record) ma **più `svc 0x28` al secondo**, cioè più occasioni per
la perdita dei vettori di A.3. Il run 24 non separa i due.

#### Run 24: due gruppi di 5 avvii (`out/console-runs/run24-build/`)

Domanda: il run 20 (avvio 3) era entrato in partita e girato 800 s; dal run
21 in poi la base muore entro 1–9 minuti. **Regressione del codice o fortuna
del run 20?** E il gestore senza I/O cambia qualcosa?

| Gruppo | NRO | `game.toml` |
|---|---|---|
| 1 (avvii 1–5) | `gruppo1/switch-game.nro`: lo stesso file del run 20 (`run20-build/switch-game-vulkan.nro`, codice del commit `716936b`; `COMMIT` del run 20 dice `5efe9ad`, che sopra `716936b` cambia solo documenti), con il suo ELF | quello dell'avvio 3 del run 20: `nx_gpu = "vulkan"`, `nx_vk_dump_frames = true` (era nel `game.toml` del run 20 per tutti e tre gli avvii), `nx_mount_cache = false` |
| 2 (avvii 6–10) | `gruppo2/switch-game.nro`, commit in `gruppo2/COMMIT` (con B) | `nx_gpu = "vulkan"`, `nx_mount_cache = false`, `nx_stack_watch = true`; il resto di default, quindi anche `nx_apm_clock = true` (GPU a 460,8 MHz), `nx_nv_wrap = true`, messenger di Mesa acceso |

Nota sul gruppo 2: rispetto al run 23 cambiano B **e** i default che la base
del run 23 spegneva (clock 460,8, contatori nvdrv, messenger). È la scelta
del piano; se il gruppo 2 migliora, la causa non è attribuibile a B da sola.

Il NRO del gruppo 1 non ha la riga `build:`; si riconosce dalla riga
`config sdmc:/switch/theft4/game.toml: found, loaded`, dall'assenza di righe
`run 21 pieces` e dal battito senza `clocks`. Quello del gruppo 2 scrive
`build: commit …` subito dopo le righe `config`.

**Per ogni avvio** (console in portatile, nessun overclock né sys-clk,
nessun overlay):

1. Copiare il `.toml` del gruppo come `sdmc:/switch/theft4/game.toml` e il
   NRO del gruppo come `sdmc:/switch/theft4/switch-game.nro` (o dove lo si
   lancia di solito); cancellare `sdmc:/switch/theft4/switch-game.toml` se
   esiste.
2. Cancellare da `sdmc:/switch/theft4/` `game.log`, `game-crash.log`,
   `game-exit.log` (dopo averli copiati dall'avvio prima) e, per il gruppo 1,
   la cartella `frames/`.
3. Avviare, non toccare niente. Annotare l'ora in cui compare "Starting a new
   game".
4. Lasciare girare **5 minuti dopo "Starting a new game"** o fino al crash.
   Se dopo 5 minuti è vivo: tenere premuto + per 1 s (uscita del modo D).
5. Annotare: **entrato in partita sì/no** (si vede Niko e si può muoverlo, non
   solo il filmato); tempo approssimativo del crash da "Starting a new game";
   **cosa mostra lo schermo alla fine** (errore del sistema col codice
   `2XXX-XXXX` se c'è, immagine ferma, ritorno al menu, schermo nero).
6. Copiare `game.log`, `game-crash.log`, `game-exit.log`, il `game.toml`
   usato e (gruppo 1) `frames/` in `out/console-runs/run24/N/`, con N =
   1–5 per il gruppo 1 e 6–10 per il gruppo 2, più un `note.txt` con le
   annotazioni del punto 5.

Ordine consigliato: alternare i gruppi (1, 6, 2, 7, …) perché temperatura e
stato della SD pesino uguale sui due.

**Come leggere i risultati:**

- Gruppo 1 entra in partita **3 volte su 5 o più**: il codice di allora
  arrivava in partita e quello di oggi no → **regressione**. Si prepara la
  bisezione sui commit fra `716936b` e oggi (sotto).
- Gruppo 1 in partita 0–2 volte su 5: l'avvio 3 del run 20 era **fortuna**
  (lo stesso guasto non deterministico c'era già), e si lavora sul percorso
  delle eccezioni (A.3).
- Gruppo 2: la riga `faults:` dà fault al secondo e tempo per fault reali
  (attesi: decine o centinaia al secondo, decine di µs). Morti con il blocco
  `=== fault ring` (fault non gestito, `svcBreak`, uscita) dicono quali fault
  hanno preceduto la morte; morti senza nessun blocco restano uccisioni dal
  kernel fuori dal nostro codice.

**Bisezione, se serve.** Commit con codice fra il run 20 e oggi (`9fe69f7`
tocca solo lo smoke; i commit di documenti non contano):

| Commit | Cosa tocca | Nel percorso caldo? |
|---|---|---|
| `0c20bf3` | tempi delle chiamate Vulkan (command processor, presenter), contatori nvdrv, profiler (spento) | sì: Vulkan submit/present |
| `6c574a9` | clock apm (GPU 460,8) | sì se `nx_apm_clock = true` |
| `bb26b80` | heap headroom, contatore dei fault (gestore **prima** del runtime), crash log dopo Setup, stato prima di ogni uscita, messenger di Mesa | sì: ordine dei gestori, messenger |
| `a9727e1` | `nx_mount_cache` falso per default | no (nel run 20 era già falsa via `.toml`) |
| `578f5be` | cvar per spegnere le novità del run 21 | no |
| `5a005e4` | tempi di pipeline e shader | sì: creazione delle pipeline |
| `6bc6c24` | wrapper di `fatalThrow`, `diagAbortWithResult`, `svcBreak` | no (solo uscite) |
| `31542c8` | ingresso delle eccezioni: q0–q31 per primi, sp/pc con registri generali, record di ripresa | sì: ogni fault |
| `644bac5` | tabella dei thread e stack watch (wrap di `threadCreate`, `pthread_create`) | alla creazione dei thread |
| (B) | anello in memoria, gestori in ordine fisso | sì: ogni fault |

Ordine di prova (bisezione binaria sui 9 commit di codice, 5 avvii per
punto, configurazione del gruppo 1 con `nx_apm_clock = false` dove esiste):
prima `578f5be` (metà: tutto il run 21, le sue novità spente via cvar); se
entra in partita la regressione è dopo (poi `31542c8`, poi `5a005e4` o
`644bac5`), altrimenti è prima (poi `6c574a9`, poi `0c20bf3` o `bb26b80`).
Tre o quattro punti bastano. Per la
regola di 09 §13 ogni build della bisezione porta le correzioni note: cache
spenta e, se il gruppo 2 le conferma, B e D.1/D.2 (cherry-pick sul commit
provato; nei commit prima di `31542c8` l'ingresso va sostituito per intero).
