# 11. Sonda Vulkan: cosa può fare NVK su questa console

Passo 7 dello smoke (`tools/switch-smoke/vulkan_probe.cpp`, cvar
`nx_smoke_vulkan`, attiva per default), aggiunto il 2026-10-02 per il run 16.
Il driver è il `libvulkan.a` statico di mesa-switch (NaGa) già nei portlibs
(`portlibs/switch/lib`, Mesa 26.2.1, ICD che dichiara API 1.4.354). Obiettivo:
sapere cosa c'è davvero prima di costruire il renderer. **Niente di questa
pagina è provato sulla console finché il run 16 non torna**: la colonna
"disponibile" della tabella va riempita dal `smoke.log`.

## Cosa fa la sonda

Ogni operazione rischiosa è preceduta da una riga `about to …` resa durevole
(fsync) prima della chiamata: dopo un crash l'ultima riga del log è
l'operazione che è caduta.

| Fase | Cosa | Righe nel `smoke.log` |
|---|---|---|
| a | `vkEnumerateInstanceVersion`, `vkCreateInstance` (con `VK_KHR_surface`, `VK_NN_vi_surface`, `VK_KHR_get_surface_capabilities2` se ci sono), dispositivi fisici: nome, tipo, API, versione del driver, vendor/device, `driverID`, `driverName`, `driverInfo`, conformità | `loader instance version`, `vkCreateInstance: … ms`, `device 0: …`, `driver id …` |
| b | tutte le estensioni di istanza e di dispositivo; le feature in tre gruppi (`features YES`, `features NO`, `not queryable` = estensione assente); limiti; heap e tipi di memoria (con `VK_EXT_memory_budget` se c'è); famiglie di code; `vkCreateDevice` con una coda grafica e `VK_KHR_swapchain` | `instance extensions (N)`, `device extensions (N)`, `features …`, `limits: …`, `memory heap …`, `memory type …`, `queue family …` |
| c | la console si chiude (`consoleExit`: il suo framebuffer tiene la `NWindow` di default), `vkCreateViSurfaceNN` sulla `nwindowGetDefault()`, capacità della surface, formati, **modalità di presentazione**, swapchain FIFO di 3 immagini (nfsmw-nx: esattamente tre su questo WSI), render pass, framebuffer | `surface: …`, `surface formats …`, `present modes: …`, `vkCreateSwapchainKHR: … ms`, `swapchain images: N` |
| d | pipeline grafica semplice (vertex `fullscreen_cw_vs` dell'SDK, fragment magenta assemblato a mano) creata quattro volte: **a freddo** (cache dell'applicazione vuota), **stessa cache piena**, **cache nuova dai dati della prima** (come se tornasse dalla SD), **senza cache dell'applicazione** (cache in memoria del driver); poi il **primo `vkQueueSubmit`** con fence (e un secondo per confronto) | `pipeline creation: cold … ms, same cache … ms, cache from data (N bytes) … ms, driver cache … ms`, `first vkQueueSubmit: … ms, fence wait … ms` |
| c | **5 secondi** di fotogrammi: lo schermo cancellato a un colore che cambia (rosso che cicla, verde che cresce, blu complementare), con un quadrato magenta di 200×200 al centro disegnato dalla pipeline di prova | `first frame presented`, `frames: N in 5.00 s = X fps, worst frame … ms, … suboptimal; per frame: fence wait, acquire, submit, present` |
| e | smontaggio in ordine (`vkDeviceWaitIdle`, oggetti, swapchain, device, surface, istanza), la console si riapre | `about to vkDestroy…`, `console reopened`, poi **`VULKAN OK`** o **`VULKAN FAIL at <operazione>: VkResult <n> (<nome>)`** |

La cache degli shader di Mesa su SD è spenta (`MESA_SHADER_CACHE_DISABLE`):
senza, il secondo avvio troverebbe la pipeline "a freddo" già compilata. Il
verdetto Vulkan è separato da `SMOKE OK`. Lo shader è minimo: i tempi di
pipeline misurano il costo fisso del driver, non quello di uno shader del
gioco (nfsmw-nx: ~59 ms per pipeline del gioco).

**Costruzione:** `libvulkan.a` + `expat`, `zstd`, `z`, `nx` (dal `vulkan.pc`
del pacchetto), in un gruppo del linker; `libelf` non esiste in devkitPro e
serve solo al parser CUBIN (mai usato su Switch): `nx_elf_stubs.c` definisce
le 13 funzioni `elf_*` che `libvulkan.a` chiede (come
`switch_elf_stubs.c` di nfsmw-nx). Niente `find_package(Vulkan)`: la config
del pacchetto richiede libelf. Il simbolo `vkCreateViSurfaceNN` si prende con
`vkGetInstanceProcAddr`. L'NRO dello smoke passa da 2,4 a 17 MB.

## Cosa guardare nel log del run 16

1. L'ultima riga: `VULKAN OK`, o `VULKAN FAIL at …`, o (crash) l'ultima
   `about to …`.
2. `device 0:` e `driver id …`: nome del dispositivo (atteso "NVK GM20B"),
   API (attesa 1.4), versione del driver.
3. `features NO` e `not queryable`: le righe della tabella sotto.
4. `present modes:` (nfsmw-nx: solo FIFO e IMMEDIATE) e `swapchain images:`.
5. `memory heap` / `memory type` (nfsmw-nx: tipo 0 `HOST_CACHED`, tipo 1
   `HOST_COHERENT` senza cache) e il budget.
6. `pipeline creation:` e `first vkQueueSubmit:`.
7. `frames: … fps`: atteso ~60 (FIFO a 60 Hz), con `worst frame` vicino a
   16,7 ms. Sullo schermo: 5 s di colori che cambiano con un quadrato magenta.
8. `console reopened` e il ritorno della console sullo schermo (se la console
   non torna, il log continua comunque).

## Richiesto da ReXGlue / disponibile su NVK Switch / alternativa

Fonte delle richieste: `src/ui/vulkan/vulkan_device.cpp` (estensioni e
feature abilitate quando c'è emulazione della GPU, `with_gpu_emulation`) e
gli usi in `src/graphics/vulkan/*.cpp`. ReXGlue **non pretende** quasi niente:
abilita ciò che c'è e sceglie percorsi alternativi; i casi in cui manca una
funzione e il backend non parte sono indicati. "Atteso" = da quello che si sa
di NVK su Maxwell (GM20B) e da nfsmw-nx, **da confermare col run 16**.

| Richiesto da ReXGlue | Per cosa | Disponibile su NVK Switch | Alternativa / come fa nfsmw-nx |
|---|---|---|---|
| `fragmentShaderPixelInterlock` / `SampleInterlock` (`VK_EXT_fragment_shader_interlock`) | percorso EDRAM "pixel shader interlock" (ROV) di `render_target_cache.cpp` | atteso **no** su Maxwell | ReXGlue passa a `Path::kHostRenderTargets` (render target dell'host, trasferimenti EDRAM). nfsmw-nx non emula mai l'EDRAM: render target Vulkan normali (renderer nativo) |
| rasterization order attachment access (`VK_EXT/ARM_rasterization_order_attachment_access`) | non richiesto dal codice di ReXGlue (è l'alternativa a FSI di altri emulatori) | atteso **no** | nessuna: non serve a ReXGlue |
| `shaderFloat64` | non abilitato da ReXGlue (il traduttore non usa double) | atteso sì (NAK lo emula) | — |
| `geometryShader` | primitive rettangolo/punti/quad dell'Xbox 360 (`primitive_processor`, `pipeline_cache`) | atteso **sì** su Maxwell | senza: conversione su CPU (più lenta); nfsmw-nx espande sulla CPU nel renderer nativo |
| `tessellationShader` | tassellazione Xenos; `shared_memory` lo controlla | atteso sì | GTA IV non usa la tassellazione Xenos (da controllare) |
| `fillModeNonSolid` | `PA_SU_SC_MODE_CNTL` wireframe/punti | atteso sì | senza: poligoni pieni |
| `drawIndirect` / `multiDrawIndirect` | non usati da ReXGlue | atteso sì | nfsmw-nx/Unleashed: `vkCmdDrawIndirect` sul percorso pre-Turing del fork Mesa spinge l'header della macro prima del segmento indiretto e può uccidere il canale con una sync MME pendente (round 15 di Unleashed): evitarlo |
| `imageCubeArray` | non richiesto | atteso sì | — |
| `sampleRateShading` | MSAA per campione (2x/4x della 360) | atteso sì | senza: MSAA per pixel |
| `independentBlend` | blend diverso per render target | atteso sì | senza: stesso blend su tutti |
| `depthClamp` | `PA_CL_CLIP_CNTL` | atteso sì | — |
| `samplerAnisotropy` | filtro anisotropico | atteso sì | — |
| `shaderClipDistance` / `shaderCullDistance` | piani utente | atteso sì | — |
| `fullDrawIndexUint32` | indici a 32 bit | atteso sì | — |
| `vertexPipelineStoresAndAtomics`, `fragmentStoresAndAtomics` | memexport, percorso FSI | atteso sì | — |
| `occlusionQueryPrecise` | query di occlusione | atteso sì | — |
| `sparseBinding`, `sparseResidencyBuffer` | memoria condivisa guest come buffer sparso (`shared_memory.cpp`) | atteso **no** sul winsys Horizon | ReXGlue usa un buffer normale da 512 MB: **pesante** con ~1,4 GB di GPU (nfsmw-nx: budget 1,4-1,5 GB) |
| `robustBufferAccess` | sicurezza degli accessi | atteso sì | — |
| `samplerMirrorClampToEdge` (1.2) | modalità di indirizzamento | atteso sì | — |
| `uniformBufferStandardLayout`, `scalarBlockLayout` (1.2) | layout delle costanti | atteso sì | — |
| `shaderDemoteToHelperInvocation` (1.3) | `kill` dei pixel shader | atteso sì | senza: `OpKill` |
| `dynamicRendering` (1.3) | render pass dinamici al posto dei `VkRenderPass` | atteso sì | nfsmw-nx (round 15 di Unleashed: "stable framebuffers") lavora con render pass classici; NVK li gestisce entrambi |
| `VK_EXT_custom_border_color` | colori di bordo YCbCr come D3D12 | atteso sì | senza: bordo nero |
| `VK_EXT_robustness2` (`nullDescriptor`) | descrittori nulli nei binding bindless | atteso sì | — |
| `VK_EXT_depth_clip_control` | profondità [-1,1] | atteso sì | — |
| `VK_EXT_non_seamless_cube_map` | cube map come la 360 | atteso sì | — |
| `VK_EXT_shader_stencil_export` | scrittura dello stencil dal pixel shader (trasferimenti EDRAM) | da vedere | senza: certi trasferimenti di stencil sul percorso host RT |
| `VK_EXT_memory_budget` | conti della memoria | atteso sì (nfsmw-nx lo usa) | — |
| `VK_KHR_swapchain` | presentazione | atteso sì (con `VK_NN_vi_surface`) | **obbligatorio**: senza, `VulkanDevice::Create` ritorna `nullptr` |
| shader float controls (`shaderSignedZeroInfNanPreserveFloat32`, `shaderDenormFlushToZeroFloat32`, `shaderRoundingModeRTEFloat32`) | fedeltà del traduttore | da vedere | — |

Limiti che ReXGlue legge: `maxImageDimension2D/3D/Cube`, `maxStorageBufferRange`
(la memoria condivisa da 512 MB è un storage buffer: se il limite è minore il
backend la divide), `maxPerStageDescriptorStorageBuffers`, i conteggi di
campioni (`framebufferColorSampleCounts`, `sampledImageIntegerSampleCounts`),
`maxViewportDimensions`, `minUniformBufferOffsetAlignment`,
`nonCoherentAtomSize`. La sonda li stampa tutti.

Due cose che la tabella non mostra e che pesano più delle feature:

- **Costo per draw su CPU**: nfsmw-nx ha misurato ~80 µs di CPU per draw con
  l'emulazione della GPU di ReXGlue (2 FPS in gara) e l'ha sostituita con un
  renderer nativo (`docs/native-renderer.md` di nfsmw-nx; 06 §11 fase c).
  GTA IV ha 850 draw per fotogramma già al primo schermo (null GPU, run 14):
  con l'emulazione piena è da aspettarsi lo stesso muro.
- **Correzioni di Mesa obbligatorie** (06 §12): `FADD32I` + `.SAT` in NAK,
  ZCULL, latenze di scheduling. Il `libvulkan.a` dei portlibs è quello di
  NaGa: va controllato quali contiene prima della fase c.

## Cosa serve per costruire il backend Vulkan di ReXGlue per Switch (solo analisi)

### Cosa lo esclude oggi

- `glue/rexglue-sdk-main/CMakeLists.txt`: con
  `LIBERTY_RECOMP_TARGET_PLATFORM = switch` forza `REXGLUE_CORE_ONLY`,
  `REXGLUE_RUNTIME_ONLY`, `REXGLUE_HEADLESS_KERNEL`,
  `REXGLUE_HEADLESS_RENDER_AUDIO`, `REXGLUE_HEADLESS_XMA_DECODER`,
  `REXGLUE_HEADLESS_INPUT`. Con `REXGLUE_CORE_ONLY` = ON:
  `REXGLUE_USE_VULKAN` e `REXGLUE_USE_D3D12` vanno a OFF, e il file fa
  `return()` dopo `src/core`, `src/filesystem`, `src/system`, `src/audio`,
  `src/kernel`: **`src/ui`, `src/input`, `src/graphics` (con `rexgpu-xenos`)
  non sono mai aggiunti**.
- `src/graphics/CMakeLists.txt`: `REXGLUE_USE_VULKAN AND
  REXGLUE_BUILD_GTA4_NATIVE` per il plugin `gta4-native` (richiede la cache
  degli shader di `LibertyRecompLib`, fuori dal repository: per Switch
  `REXGLUE_BUILD_GTA4_NATIVE=OFF`, come in `tools/pc-eu-base`).
- `rexgpu-xenos` è una `SHARED` (plugin caricato a runtime): su Switch non ci
  sono librerie dinamiche; nfsmw-nx la linka con `--whole-archive` nella
  stessa immagine (`rexglue_switch_configure_target`, `GPU_PLUGINS`) e il
  plugin si registra da un costruttore statico.

### Cosa serve

1. **Un modo "Switch con grafica"** nel `CMakeLists.txt` dell'SDK: non
   forzare `REXGLUE_CORE_ONLY` quando si vuole il renderer (opzione nuova, per
   esempio `REXGLUE_SWITCH_GPU`), `REXGLUE_USE_VULKAN=ON`,
   `REXGLUE_USE_D3D12=OFF`, `REXGLUE_BUILD_GTA4_NATIVE=OFF`,
   `REXGLUE_ENABLE_FIDELITYFX=OFF`, Tracy e perf counter spenti.
2. **Linkare NVK**: `libvulkan.a` con `expat zstd z nx` (due volte o in un
   gruppo), più `nx_elf_stubs.c` per libelf, come la sonda; target
   `INTERFACE` tipo `rex::nvk` di nfsmw-nx (`sdk/cmake/rexglue_switch.cmake`).
   Niente `find_package(Vulkan)`.
3. **volk senza loader dinamico**: `volk` cerca `libvulkan` con `dlopen`; su
   Switch serve `vkGetInstanceProcAddr` statico (nfsmw-nx lo dichiara e lo
   assegna a `ifn.vkGetInstanceProcAddr` in `vulkan_instance.cpp`, perché
   `api.h` definisce `VK_NO_PROTOTYPES`).
4. **Dipendenze del backend da compilare per aarch64**: `glslang` (+ `SPIRV`,
   `MachineIndependent`, `GenericCodeGen`, `OSDependent`, `OGLCompiler`: il
   command processor compila GLSL in SPIR-V a runtime), `SPIRV-Tools-static`,
   `VulkanMemoryAllocator`, `snappy`, `xxHash`, `imgui` (overlay di `rexui`).
5. **Le parti che assumono SDL3** (tutte in `src/ui`, nessuna in
   `src/graphics/vulkan`):
   - `rexui` linka `SDL3::SDL3` `PUBLIC`; `rexaudio` e `rexinput` anche;
   - finestra e ciclo degli eventi: `window_sdl.cpp`,
     `windowed_app_context_sdl.cpp`, `windowed_app_main_sdl.cpp`,
     `sdl_virtual_key.cpp` (e `rex_app.cpp` che li usa);
   - surface: `VulkanPresenter` conosce i tipi di surface delle piattaforme
     desktop/Android; per Horizon serve il tipo `ViNWindow`
     (`VK_NN_vi_surface`, `VkViSurfaceCreateInfoNN` con la `NWindow*`) e
     l'estensione di istanza `VK_NN_vi_surface` nella lista delle richieste.
   nfsmw-nx ha scritto i sostituti, da prendere come riferimento:
   `window_switch.cpp` (la `NWindow` di default a schermo intero),
   `windowed_app_context_switch.cpp` (ciclo con `appletMainLoop`,
   `appletHook` per dock/focus/uscita), `windowed_app_main_switch.cpp`,
   `ViNWindowSurface` in `surface.h`, il ramo `kTypeIndex_ViNWindow` di
   `vulkan_presenter.cpp` (con `nwindowSetDimensions` prima della prima
   swapchain). La nostra `ReXApp` andrebbe sostituita dal `main` di
   switch-game, che già crea il runtime da sé.
6. **Le trappole di 06 §12** al primo giorno: `__nx_nv_transfermem_size`
   alzato prima di `nvInitialize`, sotto-allocazione da blocchi grandi, un
   submit per fotogramma, `VkPipelineCache` su SD con l'elenco delle pipeline
   ricreate all'avvio, `MESA_SHADER_CACHE_DISABLE`, timestamp × 1,627.

Ordine consigliato: prima il run 16 (cosa c'è davvero), poi il plugin xenos
linkato staticamente con una finestra Horizon minima (fase a di 06 §11), per
contare i draw e il loro costo; la decisione sul renderer nativo viene da lì.
