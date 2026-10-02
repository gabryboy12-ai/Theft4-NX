# 11. Sonda Vulkan: cosa può fare NVK su questa console

Passo 7 dello smoke (`tools/switch-smoke/vulkan_probe.cpp`, cvar
`nx_smoke_vulkan`, attiva per default), aggiunto il 2026-10-02 per il run 16.
Il driver è il `libvulkan.a` statico di mesa-switch (NaGa) già nei portlibs
(`portlibs/switch/lib`, Mesa 26.2.1, ICD che dichiara API 1.4.354). Obiettivo:
sapere cosa c'è davvero prima di costruire il renderer. **Il run 16
(2026-10-02, `out/console-runs/run16/smoke.log`) l'ha provata**: `VULKAN OK`,
immagine visibile (sfondi che cambiano, quadrato magenta al centro). I numeri
sotto sono quelli del run 16. Dopo `VULKAN OK`, durante l'attesa del +, è
comparso un crash, senza crash report di Atmosphère nella cartella del run
(sezione "Il crash dopo la sonda (run 16)").

## Risultati del run 16

| Voce | Valore |
|---|---|
| Istanza | loader 1.4.354, 15 estensioni di istanza (tra cui `VK_NN_vi_surface`, `VK_KHR_surface`, `VK_EXT_debug_utils`, `VK_EXT_surface_maintenance1`, `VK_EXT_swapchain_colorspace`), 0 layer; `vkCreateInstance` 0,07 ms |
| Dispositivo | `NVIDIA Tegra X1 (GM20B) (NVK GM20B)`, tipo 1 (integrata), **API 1.3.354** (l'ICD dichiara 1.4 ma il dispositivo 1.3), driver `0x6802001`, vendor `0x10DE`, device `0x0FE0`; driver id 24 (`NVK`), `Mesa 26.2.1 (git-012c67677c)`, conformità 0.0.0.0 |
| Estensioni di dispositivo | **169** (elenco completo nel log), tra cui `VK_KHR_swapchain`, `VK_KHR_dynamic_rendering`, `VK_EXT_memory_budget`, `VK_EXT_custom_border_color`, `VK_EXT_robustness2`, `VK_EXT_depth_clip_control`, `VK_EXT_non_seamless_cube_map`, `VK_EXT_extended_dynamic_state` 1/2/3, `VK_EXT_shader_object`, `VK_EXT_graphics_pipeline_library`, `VK_EXT_descriptor_buffer`, `VK_EXT_transform_feedback`, `VK_EXT_conditional_rendering`, `VK_KHR_pipeline_binary` |
| Estensioni **assenti** che contano | `VK_EXT_fragment_shader_interlock`, `VK_EXT/ARM_rasterization_order_attachment_access`, `VK_EXT_shader_stencil_export` |
| Feature sì (52) | robustBufferAccess, fullDrawIndexUint32, imageCubeArray, independentBlend, **geometryShader**, **tessellationShader**, sampleRateShading, dualSrcBlend, logicOp, multiDrawIndirect, drawIndirectFirstInstance, depthClamp, depthBiasClamp, **fillModeNonSolid**, depthBounds, wideLines, largePoints, alphaToOne, multiViewport, samplerAnisotropy, textureCompressionBC, textureCompressionASTC_LDR, occlusionQueryPrecise, pipelineStatisticsQuery, vertexPipelineStoresAndAtomics, fragmentStoresAndAtomics, shaderStorageImageExtendedFormats, shaderStorageImageWriteWithoutFormat, shaderClipDistance, shaderCullDistance, **shaderFloat64**, shaderInt64, shaderInt16, samplerMirrorClampToEdge, descriptorIndexing, runtimeDescriptorArray, descriptorBindingPartiallyBound, descriptorBindingSampledImageUpdateAfterBind, descriptorBindingVariableDescriptorCount, scalarBlockLayout, uniformBufferStandardLayout, timelineSemaphore, bufferDeviceAddress, shaderDemoteToHelperInvocation, dynamicRendering, synchronization2, maintenance4, customBorderColors, customBorderColorWithoutFormat, nullDescriptor, depthClipControl, nonSeamlessCubeMap |
| Feature **no** (6) | textureCompressionETC2, **sparseBinding**, **sparseResidencyBuffer**, sparseResidencyImage2D, drawIndirectCount, shaderFloat16 |
| Non interrogabili (estensione assente) | **fragmentShaderSample/Pixel/ShadingRateInterlock**, **rasterizationOrderColor/DepthAttachmentAccess** |
| Limiti | maxImageDimension2D 16384, 3D 16384, Cube 32768, maxImageArrayLayers 2048; maxStorageBufferRange 4294967295; maxUniformBufferRange 65536; maxPushConstantsSize 256; maxMemoryAllocationCount 4096; maxSamplerAllocationCount 4000; maxPerStageDescriptor* 1048576 (tutti), maxPerStageResources 4294967295, maxDescriptorSet* 1048576, maxBoundDescriptorSets 32; maxVertexInputAttributes/Bindings 32, maxVertexOutputComponents 128, maxFragmentInputComponents 128, maxColorAttachments 8, framebuffer 16384×16384; maxComputeSharedMemorySize 49152, maxComputeWorkGroupInvocations 1024; timestampPeriod 1,000 ns (ma il contatore avanza ogni 1,627 ns, 06 §12); allineamento UBO/SSBO 256/16, nonCoherentAtomSize 128; campioni colore e profondità `0xF` (1, 2, 4, 8) |
| Memoria | un heap da **2391 MiB** (`DEVICE_LOCAL`), budget 2128 MiB, uso 0; tipo 0 `DEVICE_LOCAL HOST_VISIBLE HOST_CACHED`, tipo 1 `DEVICE_LOCAL HOST_VISIBLE HOST_COHERENT` |
| Code | una famiglia, flag `0x7` (grafica, calcolo, trasferimento), **1 coda**, timestamp a 64 bit; `vkCreateDevice` 6,38 ms |
| Surface | `NWindow` di default 1280×720; supporto presentazione sì; **immagini 3–3**; extent 1280×720 (min 1×1, max 16384×16384); trasformazioni `0x1`; alpha composito `0x3`; uso `0x8009F`; formati 37 (`R8G8B8A8_UNORM`) e 44 (`B8G8R8A8_UNORM`), spazio colore 0 |
| Modalità di presentazione | **FIFO e IMMEDIATE** (niente MAILBOX né FIFO_RELAXED) |
| Swapchain | 3 immagini, formato 37, FIFO; `vkCreateSwapchainKHR` 6,09 ms |
| Pipeline (coppia di shader minima) | a freddo **3,376 ms**; stessa cache piena 0,037 ms; cache nuova dai 3952 byte di dati 0,053 ms; senza cache dell'applicazione (cache del driver) 2,752 ms. Per gli shader del gioco nfsmw-nx misura ~59 ms: il costo fisso del driver è piccolo, quello vero è la compilazione |
| Submit | primo `vkQueueSubmit` 0,118 ms + attesa della fence 0,284 ms; secondo 0,062 + 0,329 ms |
| Fotogrammi | **304 in 5,01 s = 60,6 fps**, fotogramma peggiore 16,91 ms, 0 suboptimal; per fotogramma: fence 0,086 ms, **acquire 15,967 ms** (il FIFO blocca nell'acquire fino al vsync), submit 0,112 ms, present 0,233 ms |

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

## Cosa guardare nel log (dal run 16)

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
funzione e il backend non parte sono indicati. Colonna "disponibile": il run
16.

| Richiesto da ReXGlue | Per cosa | Disponibile su NVK Switch (run 16) | Alternativa / come fa nfsmw-nx |
|---|---|---|---|
| `fragmentShaderPixelInterlock` / `SampleInterlock` (`VK_EXT_fragment_shader_interlock`) | percorso EDRAM "pixel shader interlock" (ROV) di `render_target_cache.cpp` | **no** (estensione assente) | ReXGlue passa a `Path::kHostRenderTargets` (render target dell'host, trasferimenti EDRAM). nfsmw-nx non emula mai l'EDRAM: render target Vulkan normali (renderer nativo) |
| rasterization order attachment access | non richiesto dal codice di ReXGlue | **no** (estensione assente) | non serve a ReXGlue |
| `shaderFloat64` | non abilitato da ReXGlue | sì | — |
| `geometryShader` | primitive rettangolo/punti/quad dell'Xbox 360 (`primitive_processor`, `pipeline_cache`) | **sì** | — |
| `tessellationShader` | tassellazione Xenos; `shared_memory` lo controlla | sì | — |
| `fillModeNonSolid` | `PA_SU_SC_MODE_CNTL` wireframe/punti | sì | — |
| `drawIndirect` / `multiDrawIndirect` | non usati da ReXGlue | `multiDrawIndirect` sì, `drawIndirectCount` **no** | `vkCmdDrawIndirect` sul percorso pre-Turing del fork Mesa di Unleashed può uccidere il canale con una sync MME pendente (round 15): evitarlo |
| `imageCubeArray` | non richiesto | sì | — |
| `sampleRateShading` | MSAA per campione | sì (campioni 1/2/4/8) | — |
| `independentBlend` | blend per render target | sì | — |
| `depthClamp` | `PA_CL_CLIP_CNTL` | sì | — |
| `samplerAnisotropy` | filtro anisotropico | sì | — |
| `shaderClipDistance` / `shaderCullDistance` | piani utente | sì / sì | — |
| `fullDrawIndexUint32` | indici a 32 bit | sì | — |
| `vertexPipelineStoresAndAtomics`, `fragmentStoresAndAtomics` | memexport, percorso FSI | sì / sì | — |
| `occlusionQueryPrecise` | query di occlusione | sì | — |
| `sparseBinding`, `sparseResidencyBuffer` | memoria condivisa guest come buffer sparso (`shared_memory.cpp`) | **no / no** | ReXGlue usa un buffer normale da 512 MB, su un heap da 2391 MiB con budget 2128 MiB: possibile ma pesante |
| `robustBufferAccess` | sicurezza degli accessi | sì | — |
| `samplerMirrorClampToEdge` (1.2) | indirizzamento | sì | — |
| `uniformBufferStandardLayout`, `scalarBlockLayout` (1.2) | layout delle costanti | sì / sì | — |
| `shaderDemoteToHelperInvocation` (1.3) | `kill` dei pixel shader | sì | — |
| `dynamicRendering` (1.3) | render pass dinamici | sì | anche i render pass classici funzionano (la sonda li usa) |
| `VK_EXT_custom_border_color` | colori di bordo come D3D12 | sì (anche senza formato) | — |
| `VK_EXT_robustness2` (`nullDescriptor`) | descrittori nulli | sì | — |
| `VK_EXT_depth_clip_control` | profondità [-1,1] | sì | — |
| `VK_EXT_non_seamless_cube_map` | cube map come la 360 | sì | — |
| `VK_EXT_shader_stencil_export` | stencil dal pixel shader (trasferimenti EDRAM) | **no** (estensione assente) | certi trasferimenti di stencil sul percorso host RT vanno fatti in altro modo |
| `VK_EXT_memory_budget` | conti della memoria | sì | — |
| `VK_KHR_swapchain` | presentazione | sì (con `VK_NN_vi_surface`) | obbligatorio |
| shader float controls | fedeltà del traduttore | `VK_KHR_shader_float_controls` e `_controls2` presenti; le proprietà non sono state stampate | da stampare in una prossima sonda |

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

## Il crash dopo la sonda (run 16)

**Cosa dice il log.** Lo smontaggio è arrivato fino in fondo: `about to
vkDeviceWaitIdle and destroy the device objects`, `vkDestroySwapchainKHR`,
`vkDestroyDevice`, `vkDestroySurfaceKHR`, `vkDestroyInstance`, `about to
reopen the console`, `console reopened`, **`VULKAN OK`**, poi `exit test: 5
threads working in the background; + exits with mode A …`. Quest'ultima è
l'ultima riga: il crash viene **dopo** l'avvio dei cinque thread di lavoro e
**prima** del + (`smoke-exit.log` è vuoto). **Nessun crash report di
Atmosphère** nella cartella del run 16, quindi non c'è un indirizzo da
decodificare con `addr2line`; serve il file di
`sdmc:/atmosphere/crash_reports/` (o `fatal_reports/`) di quel momento, e il
testo esatto dell'errore sullo schermo.

**Cosa si esclude dal log.**

- Non è lo smontaggio di Vulkan in sé: tutti i passi sono stati scritti e
  `VULKAN OK` viene dopo.
- Non è un `consoleInit` fallito in modo da rompere subito la console: dopo la
  riapertura tre righe sono state stampate sullo schermo e poi scritte nel log
  (`Report` stampa prima e scrive dopo). Dal disassemblato di libnx 4.12:
  `consoleInit` non ritorna `NULL` se il renderer non parte (lascia il flag
  di console inizializzata spento, e allora `consoleUpdate` non fa niente),
  `ConsoleSwRenderer_init` chiama `nwindowSetDimensions` e `framebufferCreate`
  (2 buffer) e `framebufferMakeLinear` sulla `NWindow` di default.
- Lo stesso test dei cinque thread, con la stessa attesa del +, nel run 15 non
  dava crash: la differenza è la sonda Vulkan prima.

**Candidati (da provare, non da assumere).**

1. La console riaperta sulla `NWindow` che il WSI di NVK ha usato: il primo
   `consoleUpdate` dell'attesa (`framebufferEnd`/`Begin`, accodamento e
   prelievo di un buffer nella coda di vi) con slot o dimensioni lasciati dal
   WSI. È il candidato principale, perché l'attesa chiama `consoleUpdate` ogni
   16 ms e prima della sonda non lo faceva su una finestra già usata.
2. Thread di NVK ancora vivi dopo `vkDestroyDevice`/`vkDestroyInstance`
   (`libvulkan.a` crea thread in `vk_queue.c`, `u_queue.c`, `threads_posix.c`).
3. Presentazioni ancora in volo al momento della distruzione della swapchain.
4. L'ordine di distruzione (già quello della specifica: oggetti, swapchain,
   device, surface, istanza).

**Run 17 (da verificare).**

- Ogni passo dello smontaggio ha una riga `teardown step N: …` durevole
  prima di essere eseguito.
- **Numero di thread del processo** (`svcGetThreadList`) prima della sonda e
  dopo lo smontaggio: se dopo sono di più, NVK ha lasciato thread vivi
  (candidato 2).
- **Varianti con cvar** (in `sdmc:/switch/theft4/smoke.toml`):
  - `nx_vk_reopen_console = false`: la console resta chiusa, niente
    `consoleInit` né `consoleUpdate` dopo Vulkan (candidato 1);
  - `nx_vk_wait_idle = false`: niente `vkQueueWaitIdle`/`vkDeviceWaitIdle`
    prima della distruzione (per confronto: nel run 16 c'era già
    `vkDeviceWaitIdle`; ora per default anche `vkQueueWaitIdle`, candidato 3);
  - `nx_vk_destroy_instance = false`: surface e istanza non distrutte.
- Nell'attesa del +: una riga durevole a 1, 2, 3, 5, 10 s e poi ogni 10 s
  (`waiting for +: N s, M console updates`) e ogni cambio dei tasti: dice
  **quando** arriva il crash e quanti `consoleUpdate` sono passati.
- Con la console chiusa non si stampa niente sullo schermo (anche
  `Press + to exit.` è protetto: scrivere su una console chiusa era il crash
  del run 11).

**Come provarlo:** un run con i valori di default (per riprodurlo con le righe
nuove), poi uno con `nx_vk_reopen_console = false`. Se il secondo non va in
crash, la causa è la riapertura della console sulla finestra usata da Vulkan;
la correzione sarebbe allora non riaprirla (il gioco non userà la console
insieme a Vulkan) o ricrearla con una `NWindow` nuova.

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
