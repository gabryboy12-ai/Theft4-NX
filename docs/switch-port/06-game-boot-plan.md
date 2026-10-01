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
  configure di glslang; si usa `D:/Python/python.exe`. 939 passi; i file
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
      "--game_data_root=D:/Mod Switch/gta4/Grand Theft Auto IV (Europe) (En,Fr,De,Es,It)"
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
