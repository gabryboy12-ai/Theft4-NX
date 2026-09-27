# 07 – Riferimenti esterni: rexglue-nx e UnleashedRecomp

Solo analisi: nessun codice di questi progetti è stato copiato qui. I cloni
stanno fuori dal repository, in `/c/dev/refs/` (clone superficiale, letti il
2026-09-27):

| Progetto | Commit | Licenza |
|---|---|---|
| [KawaiiBunga/rexglue-nx](https://github.com/KawaiiBunga/rexglue-nx) | `135a2ad2` "Switch runtime foundation", 2026-09-26 | BSD 3 clausole (Tom Clay; porzioni Xenia, Ben Vanik e contributori), come l'SDK ReXGlue che usiamo |
| [hedge-dev/UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) | `cf829a9e`, 2026-06-29 | GPL-3.0 |
| [hedge-dev/XenosRecomp](https://github.com/hedge-dev/XenosRecomp) (submodule di UnleashedRecomp) | `990d03b2`, 2025-08-03 | MIT |

Il clone di rexglue-nx ha profondità 1: la storia precedente non è stata
letta, quindi "cosa c'è" vale per quel commit.

## 1. rexglue-nx

### Cosa contiene per Switch

- **CMake.** `REXGLUE_PLATFORM_SWITCH` (toolchain devkitPro obbligatoria, GCC
  o Clang AArch64), `REX_PLATFORM_SWITCH=1`, output nel binary dir,
  packaging NRO (`cmake/rexglue_switch_nro.cmake`), FFmpeg
  (`cmake/rexglue_switch_ffmpeg.cmake`). Su Switch non vengono costruiti il
  generatore e `rexglue`.
- **SDL3 3.4.10 patchato** (`switch/patches/`, applicato a una copia nel build
  dir): backend libnx per video, audio e joystick, più un'opzione
  `SDL_SWITCH_EXTERNAL_GRAPHICS` che toglie EGL/GLES.
- **Plugin GPU collegato staticamente.** `gpu_plugin_loader.cpp`, ramo
  `REX_PLATFORM_SWITCH`: niente caricamento dinamico, il plugin `xenos` è
  linkato nell'eseguibile e creato con `rex_gpu_create`.
- **rexcore su Switch:** solo `atomic_posix`, `clock_switch`,
  `filesystem_posix`, `math_gcc`, `env_posix`, `threading_switch`.
- **Test:** NRO di prova della toolchain (senza ReXGlue), `thread-smoke`
  (eventi), `link-probe` (link del runtime senza titolo).

### Confronto punto per punto

| Area | rexglue-nx | Theft4-NX (03, 04) |
|---|---|---|
| **Memoria guest** | Assente. `memory_posix`, `mapped_memory_posix` ed `exception_handler_posix` si compilano solo nel ramo Linux e su Switch non c'è un'alternativa: niente alias, niente traduzione, nessuna SVC di memoria. | Design A: una riservazione, nessun alias, traduzione con la tabella `rex_guest_table` da 256 voci, blocchi da 2 MiB spostati con `svcMapProcessCodeMemory` + `svcSetProcessMemoryPermission`, protezione con `svcSetMemoryPermission`. Verificato su console (run 6). |
| **Thread** | Solo parti di base: `Event` su `UEvent` + `waitSingle`, `Sleep` con `svcSleepThread`, `MaybeYield` con `YieldType_WithoutCoreMigration`, ID del thread da `svcGetThreadId`. Attese alertable non implementate (restituiscono `kFailed`). `EnableAffinityConfiguration` vuota; nessun `Thread::Create`, affinità o sospensione per Switch in questo commit. | Thread completi su pthread di libnx: `svcSetThreadCoreMask` (core ideale + maschera), CPU guest → core 0–2, core 3 per i worker host, sospensione con `svcSetThreadActivity`, coda di callback utente e attese alertable, conteggio dei core dalla maschera del processo, timer fermato all'uscita. |
| **Fault ed eccezioni** | Nessun gestore Switch. | `__libnx_exception_entry` nostro, `svcReturnFromException` per riprendere; fault ripresi e serializzati (run 6, T9/T10). |
| **Finestra e grafica** | Finestra SDL3 sulla `NWindow` di default di libnx con EGL/GLES di switch-mesa. In alternativa `SDL_SWITCH_EXTERNAL_GRAPHICS`, con il commento "Dawn/NVK own the default NWindow". Backend Vulkan acceso per default su Switch. Il driver Vulkan per Horizon **non** è nel repository, e il commit non dice da dove verrebbe. Nessuna traccia di deko3d. | Nessuna grafica ancora (06 §4: prima un NRO senza renderer). |
| **Audio** | Backend SDL3 su `audren`/`audrv` (audio renderer di libnx): una voce, PCM Int16, mix stereo/mono. | Nessuno ancora; XMA decoder e render-audio headless compilati. |
| **Input** | Backend joystick SDL3 su HID npad (tabella pulsanti → `HidNpadButton_*`). | Nessuno ancora (lo smoke usa solo `padGetButtonsDown` per uscire). |

In sintesi, rexglue-nx è all'inizio: toolchain, packaging e periferiche SDL,
ma memoria, eccezioni e thread veri, cioè le parti su cui si è concentrato
questo port, mancano.

### Cosa conviene prendere

In ordine di utilità:

1. **La patch SDL3 per audio e input**, quando servirà una finestra o l'audio:
   `audren`/`audrv` e HID npad sono le API giuste. Va valutata insieme al
   renderer, perché il video EGL/GLES non ci serve.
2. **`gpu_plugin_loader` statico**: su Horizon non c'è `dlopen` dei plugin
   GPU, quindi dovremo fare la stessa cosa.
3. **ID del thread da `svcGetThreadId`.** Il nostro
   `current_thread_system_id()` restituisce l'handle (`threadGetCurHandle()`),
   un valore per-processo che non identifica il thread nei crash report. Il
   loro è l'ID kernel: correzione piccola e utile.
4. **`Event` su `UEvent`** come alternativa più leggera al nostro
   `pthread_cond`. Da misurare prima di cambiare: il nostro supporta attese
   alertable e multiple, il loro no.
5. **Packaging NRO** in CMake: confrontarlo con il nostro
   `nx_create_nro` di switch-smoke quando nasce `tools/switch-game`.

Non c'è niente da prendere per memoria, fault e scheduling.

**Licenza e crediti.** Il codice è BSD 3 clausole: si può includere in un
progetto GPL-3.0 mantenendo nei file copiati l'avviso di copyright e il testo
della licenza, e citando rexglue-nx (KawaiiBunga) nei crediti di
`README-SWITCH.md`. I file del backend SDL portano l'avviso zlib di SDL (Sam
Lantinga) e la patch lo definisce "Private fork backend": prima di
riusarli va chiarito l'autore del backend libnx originale, per citarlo.

## 2. UnleashedRecomp

### Shader: XenosRecomp in fase di build

- **Ingresso.** `UnleashedRecompLib/CMakeLists.txt` decomprime `shader.ar`
  dei dati di gioco (`x_decompress`) e passa a XenosRecomp la cartella
  `private/` (archivi decompressi più `default_patched.xex`, che contiene
  shader incorporati).
- **Scansione.** XenosRecomp cerca nei file i container di shader Xenos,
  converte ogni shader in HLSL e lo compila con DXC in DXIL e SPIR-V.
- **Uscita.** Genera `shader/shader_cache.cpp`, che finisce
  nell'eseguibile: SPIR-V compresso con smol-v e poi zstd, DXIL solo zstd.
- **Uso a runtime.** Lo shader guest si cerca per hash XXH3-64 dei suoi byte
  (ricerca binaria su voci ordinate). Nessuna traduzione a runtime: uno
  shader mancante è un errore.
- **Varianti.** Costanti di specializzazione (alpha test, spacchettamento di
  `R11G11B10` nel vertex shader) invece di più shader; in DXIL con il linking
  di librerie.
- **Limiti dichiarati** (README di XenosRecomp): indicizzazione dinamica dei
  registri, costanti intere, memexport, point size, mini vertex fetch;
  semantiche e istanziazione specifiche di Sonic Unleashed.

### Pipeline e stutter

Una pipeline è `PipelineState`: shader, dichiarazione dei vertici,
blend/depth/raster, formati dei render target, costanti di specializzazione.

1. **Elenco registrato** (`gpu/cache/pipeline_state_cache.h`): stati raccolti
   in build di sviluppo (`PSO_CACHING` scrive gli stati mancanti in un file
   da inviare agli sviluppatori), compilati all'avvio durante i loghi
   (`PrecompilePipelines`).
2. **Precompilazione guidata dal gioco.** Quando il gioco carica dati
   (modelli, terreno, particelle riconosciuti dalle vtable), il renderer
   enumera i materiali e accoda le loro pipeline.
3. **Thread di compilazione**: `max(2, 2/3 dei thread hardware)`, priorità
   minima, massima durante i caricamenti. La pipeline pronta arriva al thread
   di rendering tramite la coda dei comandi. L'uscita dal caricamento (hook
   su `CGameModeStage::ExitLoading`) aspetta che il contatore delle
   compilazioni torni a zero; le pipeline dei loghi non lo incrementano.
4. **Ricompilazione** di tutte le pipeline asincrone quando cambiano le
   opzioni (MSAA ecc.).

### Quanto si applica a ReXGlue su Switch

Buona parte c'è già nel nostro albero:

- **Cache degli shader XenosRecomp.** `src/graphics/gta4_native` legge
  `LibertyRecompLib/shader/shader_cache.cpp` (`g_shaderCacheEntries`,
  SPIR-V smol-v + zstd; `docs/SHADER_PIPELINE.md`: 1132 shader da 79 `.fxc`).
  Quel file deriva dal gioco ed è escluso da questo repository (05): va
  rigenerato in locale dai file del proprio disco
  (`tools/rage_fxc_extractor` + XenosRecomp). Per il disco europeo le chiavi
  sono gli hash dei byte degli shader: se gli `.fxc` europei differiscono da
  quelli USA, serve una cache propria.
- **Pipeline cache persistente.** `gta4_native` crea una `VkPipelineCache`,
  la carica e la salva su file (`GetNativePipelineCachePath`).
- **Compilazione asincrona.** `native_pipeline_compiler.h` ha una coda e un
  thread di compilazione con callback a coda vuota.
- **Percorso generico Xenia** (`src/graphics/vulkan/pipeline_cache.cpp`):
  traduce gli shader a runtime e li salva in `cache/shaders/shareable/<title>.xsh`.
  Su Switch è il ripiego costoso.

Cosa manca rispetto a UnleashedRecomp, e conta di più con 4 core A57:

1. **Elenco di stati registrato e precompilazione all'avvio**, per non
   compilare nulla durante il gioco la prima volta.
2. **Precompilazione guidata dai caricamenti**: servono hook sui caricamenti
   di RAGE, legati agli indirizzi dell'eseguibile (quindi non per la strada C
   di 06).
3. **Più thread di compilazione sul core 3** (`nx_host_worker_core_mask()`,
   04 §3), priorità alta durante i caricamenti.

Resta un punto aperto: **quale driver Vulkan** su Horizon. NVK compila
SPIR-V → NAK dentro `vkCreateGraphicsPipelines`, quindi la precompilazione e
la `VkPipelineCache` persistente sulla SD sono ancora più importanti che su
desktop. Nessuno dei due repository contiene un NVK per Switch o dice come
ottenerlo. L'alternativa nativa della scena homebrew è deko3d, che non parla
Vulkan: il renderer ReXGlue andrebbe riscritto. Va verificato prima di
scegliere.

**Licenza.** UnleashedRecomp è GPL-3.0 come questo repository: il codice si
può riusare con attribuzione. XenosRecomp è MIT (avviso di copyright da
mantenere). La cache generata deriva dal gioco e non va committata (05).
