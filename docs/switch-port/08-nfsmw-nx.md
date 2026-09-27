# 08 – nfsmw-nx: confronto con il nostro livello Horizon

[StevensND/nfsmw-nx](https://github.com/StevensND/nfsmw-nx) è un port Switch
completo di Need for Speed: Most Wanted (Xbox 360) sullo stesso stack:
ReXGlue con un livello Horizon nell'SDK, renderer nativo PM4 → Vulkan su NVK,
shader precompilati con XenosRecomp, LTO/PGO. Gira a 30–35 FPS alle frequenze
standard. Clone in `/c/dev/refs/nfsmw-nx`, commit `493d47fc`
(2026-09-27), letto per intero `docs/` più i sorgenti del livello Horizon
(`sdk/src/core/*_switch*`, `sdk/src/system/xthread.cpp`,
`sdk/resources/templates/codegen/pch_h.inja`) e `mesa/`.

**Licenze.** App GPL-3.0 (come noi), modifiche all'SDK BSD-3 (come il nostro
SDK), XenosRecomp e Mesa MIT. Si può prendere codice con attribuzione e
mantenendo gli avvisi di copyright.

**Base diversa.** Il loro SDK parte da ReXGlue v0.10.0; il nostro è il fork
di Theft4 (0.9.x, con molte aggiunte di LibertyRecomp/Theft4). "Adottare" un
loro componente significa portarlo sul nostro albero, non copiarlo: i costi
sotto ne tengono conto. Sono stime in giorni di lavoro, non misure.

## Riepilogo

| Componente | Raccomandazione | Motivo in una riga | Costo stimato |
|---|---|---|---|
| Memoria guest | **Decidere con T11 fra A e D** (alias al commit, 03 §13) | il loro accesso è diretto su Switch (più veloce della nostra tabella), ma paga fault e tetto del mappato; D prende l'accesso diretto senza i fault, se entra nel tetto | probe 1 g |
| Eccezioni e fault | **Tenere il nostro**, prendere il loro crash log | con il design A i fault sono rari; noi serializziamo con una sola eccezione, loro servono 8 slot e due eccezioni per fault | 0,5 g |
| Priorità dei thread | **Adottare il loro** (guest 0x3B, host 0x2B–0x2D) | oggi i nostri thread guest sono a 0x2C, banda senza time-slicing: uno spin guest affama gli altri | 1 g |
| Affinità e core | **Fondere**: tenere la nostra mappa morbida, niente affinità rigida | loro misurano che il core preferito non fissa e la maschera esclusiva peggiora i minimi; è già il nostro default | 0,5 g |
| Clock (460,8 MHz GPU in portatile) | **Adottare il loro** `switch_apm` | configurazione standard `0x92220008`, con le trappole già risolte (apm asincrono, pcv che ripristina la RAM) | 1 g |
| Audio | **Adottare il loro** | noi non abbiamo output su Switch; loro hanno audout + XMA fuori dal thread del gioco + priorità corrette | 3–5 g |
| Presentazione e finestra | **Adottare il loro** | noi non abbiamo niente; i limiti del WSI Horizon sono già documentati e gestiti | 3–5 g |
| Chiamate dirette | **Adottare** (`llamadas_directas.py`) | 96 % delle chiamate diventano dirette: senza, LTO non può fare inlining | 0,5 g |
| Registri come locali + `share_registers` | **Adottare** il patch del generatore | il nostro config li ha tutti a `false`; nel loro gioco `savegpr/restgpr` valeva 14 % del thread principale | 2–3 g |
| LTO | **Adottare** dopo le chiamate dirette | `-flto=2`, partizioni bilanciate, entra in 16 GB | 0,5 g |
| PGO | **Adottare più avanti** | richiede un gioco che giri; trappole note (`-fno-profile-values`, TLS) | 2 g + sessioni di gioco |
| Ordine delle funzioni | **Adottare più avanti** | viene dai profili; `_start` deve restare a 0 | 0,5 g |
| Edizioni | **Adottare gli strumenti** (`tools/editions`) quando si esce dalla strada C | allineamento a 12-grammi + LIS, verifica degli hook, traduzione di `.data`: la versione seria del nostro matcher | 3–5 g per USA v8 → EU |
| Renderer | **Fondere**: nucleo PM4 → Vulkan loro, conoscenza GTA del nostro `gta4_native` | il loro è provato su NVK/Maxwell e quasi indipendente dall'edizione; il nostro dipende da ~135 hook della v8 USA | settimane |
| Mesa | **Fondere**: loro patch sopra il ramo di NaGa, costruito da noi | il Mesa di NaGa installato non ha ZCULL su Vulkan né il fix `FADD32I`+`.SAT` (correttezza) | 2–3 g (ambiente di build) |

## 1. Memoria guest

### Il loro modello

`sdk/src/core/guest_memory_switch.{h,cpp}`:

- **Una finestra da 0x120000000 (4,5 GB)** riservata con `virtmemAddReservation`,
  vuota. Il codice generato è `base + addr + REX_PHYS_HOST_OFFSET(addr)`
  (`pch_h.inja`), ma su Switch `REX_PHYS_HOST_OFFSET` vale 0 (il +0x1000 si
  applica solo su Windows e Mac ARM64): **accesso diretto**. Il +4 KiB della
  finestra 0xE0 sta nella posizione della vista (offset fisico `0x100001000`
  in `map_info` di `xmemory.cpp`).
- **Alias veri.** Il commit è "memalign → `virtmemFindCodeMemory` →
  `svcMapProcessCodeMemory` → `svcSetProcessMemoryPermission(RW)` →
  `svcMapProcessMemory` in ogni vista". `svcMapProcessMemory` rifiuta come
  sorgente la memoria del heap (0xD401): passa per l'alias di codice. **È la
  SVC che il nostro 03 non ha provato**: le nostre conclusioni "nessun alias
  possibile" valevano per `svcMapProcessCodeMemory`, `CodeMemory` e
  `svcMapPhysicalMemory`.
- **Finestre fisiche 0xA/0xC/0xE e 0x7F.** Viste registrate con
  `RexGmAddView` sullo stesso offset: il commit una volta riempie tutte le
  viste. Ogni blocco però è mappato in una vista **solo al primo accesso da
  quella vista**, dal gestore di eccezioni (`RexGmFaultIn`). Mappare subito in
  tutte e cinque le viste portava 472 MB di memoria reale a 1782 MB mappati e
  il kernel rifiutava con 2001-0103 (`LimitableResource_Memory`).
- **Commit su richiesta** a blocchi di 4 MB; la memoria fisica guest non è più
  impegnata tutta all'avvio (1 MB alla volta dal gestore).
- **Protezione.** Le viste sono in stato `ProcessMem`, senza
  `PermChangeAllowed`: "proteggere" una pagina = smapparla. Le letture di una
  pagina sorvegliata vanno in fault e sono **emulate** leggendo dall'alias
  ombra; permessi logici in una tabella propria.
- **MMIO** con il percorso del fault e la decodifica dell'istruzione
  (`rex_decode.h`), come upstream.
- **Volume dei fault:** 30–2.500 al secondo su una ventina di thread.

### Il nostro (03-memory.md)

Design A: una riservazione senza alias; la memoria fisica e le finestre
0x7F/0xA/0xC/0xE puntano allo stesso host tramite `rex_guest_table` (256 voci
da 16 MiB); blocchi da 2 MiB spostati con `svcMapProcessCodeMemory` e resi RW
con `svcSetProcessMemoryPermission` (stato `AliasCodeData`), conteggio dei
riferimenti per pagina; protezione con `svcSetMemoryPermission` a 4 KiB.

### Confronto

| | nfsmw-nx | Theft4-NX |
|---|---|---|
| Alias | sì, `svcMapProcessMemory` (una mappatura per vista) | no, traduzione a tabella |
| Memoria mappata | cresce con le viste toccate (fino a 5×) | = memoria impegnata |
| Fault per mappare | uno per blocco e per vista | nessuno |
| Pagina in sola lettura | smappata: anche le letture vanno in fault | leggibile (`svcSetMemoryPermission` R, run 6 T9) |
| SVC nel gestore | evitate ("il gestore non può fare SVC") | usate e verificate (T9: 1000 round trip, 12,15 µs) |
| Accesso nel codice generato | `base + addr` (su Switch) | `addr + rex_guest_table[addr >> 24]` |
| Aritmetica host oltre 16 MiB | contigua (4,5 GB di finestra) | non contigua fra finestre (03 §8) |

**Costo per accesso.** I nostri numeri (run 6, 100 M letture+scritture su 64
MiB):

| | sequenziale | casuale |
|---|---|---|
| diretto, `base + off` (T6/T7) | 3,49 ns | 34,0 ns |
| tabella, tutto < 0xA0000000 (T7) | 5,20 ns (+48,6 %) | 35,7 ns (+4,8 %) |
| tabella, 50 % finestre (T7) | 5,94 ns (+69,9 %) | 37,2 ns (+9,3 %) |
| confronto + `csel` (T6 "A") | 6,72 / 7,83 ns (+92 / +124 %) | 36,6 / 39,2 ns (+7 / +15 %) |

**Corretto dopo la prima versione di questo documento:** su Switch la loro
macro è il "diretto" di T6/T7, non la variante con confronto e selezione
(quella è la forma Windows/Mac). Il loro accesso costa quindi come il diretto:
nel run 6 la nostra tabella è +48,6 % in sequenziale e +4,8 % in casuale
rispetto a esso. Il vantaggio per accesso è loro; il costo lo pagano in fault
e mappature (sotto). T11.3 lo misura sulla memoria vera, con le viste.

### Raccomandazione: tenere il design A, con un probe T11

- Il loro modello ha avuto bisogno di fault per mappare, del soffitto del
  mappato, di letture emulate e di un gestore concorrente. Il nostro evita
  tutte e quattro le cose, e i numeri di T8/T9 sono buoni.
- Il loro vantaggio vero è la **contiguità**: hook e `function.h` possono
  fare aritmetica sui puntatori host come su desktop. Nel nostro caso resta
  una fonte di bug da tenere a mente (03 §8).
- **T11, prima di decidere definitivamente** (circa 1 giorno):
  1. `svcMapProcessMemory` da un nostro blocco `AliasCodeData` verso una
     seconda vista: funziona? e `svcSetMemoryPermission` sulla vista?
  2. la loro macro esatta (= diretto su Switch) contro la nostra tabella, sulla
     memoria vera con le viste;
  3. il tetto `LimitableResource_Memory` letto con `svcGetInfo`.

  Se (1) funziona con permessi sulla vista, un modello ibrido (alias solo per
  0xA/0xC/0xE, tabella altrove) diventa possibile; altrimenti il design A
  resta la scelta migliore per noi.

## 2. Eccezioni e fault

**Sì, anche loro sostituiscono `__libnx_exception_entry`**, ma in modo diverso:

| | nfsmw-nx | Theft4-NX |
|---|---|---|
| Entry | quella di libnx 4.12 istruzione per istruzione, più slot | nostra, scritta da zero |
| Dove gira il gestore | fuori dalla claim del kernel (torna al kernel e salta a `RexExceptionReturnEntry`) | dentro la claim (`KProcess::EnterUserException`) |
| Concorrenza | più thread insieme: 8 slot di pila (64 KB) e dump, presi con `ldaxr/stlxr` | serializzato dal kernel (T10: mai più di 1) |
| Ripresa | seconda eccezione voluta (`udf` in `RexResumeTrap`), poi `svcReturnFromException` | `svcReturnFromException` diretto con il frame corretto |
| Eccezioni per fault | 2 | 1 |
| Rischio | stato globale del percorso di fault (hanno trovato un mutex globale preso 3 volte per fault) | un gestore che aspetta un lock tenuto da un thread che poi va in fault si blocca |

Il loro bug principale (due thread nello stesso dump, un thread ripreso con i
registri di un altro) nel nostro schema non può esistere: il kernel non fa
entrare un secondo thread. Ci serviva la concorrenza solo con un volume di
fault alto, che il design A non produce.

**Raccomandazione:** tenere il nostro. Da prendere: `rex_crash.log` con
registri, indirizzo e pila, e l'ordine delle decisioni del gestore
(commit, lettura sorvegliata, gestori dell'SDK, SEH, fatale). Costo 0,5 g.

## 3. Thread, core, clock, audio, presentazione

### Priorità: da adottare subito

- Su Horizon **solo 0x3B ha il time-slicing** (~10 ms); ogni altra banda è
  cooperativa e un thread che non si blocca tiene il suo core contro quelli di
  pari o minore priorità, anche con altri core liberi.
- Loro: guest 0x3B; host sopra (0x2B audio e XMA, 0x2C presentazione e altri
  host, 0x2D ring GPU, 0x2A profiler), con i nomi confrontati per prefisso.
- **Noi oggi:** `MapToSwitchPriority` mette 0x2C come default e sale verso
  0x20; i thread guest finiscono a 0x2C o sopra. Il codice ricompilato fa
  spin (loro: due loop del D3D del gioco al 100 %), quindi è il caso che
  descrivono. Da portare prima del primo boot del gioco. Costo 1 g.
- Altre lezioni da prendere: `std::thread::detach()` può lanciare e chiudere
  il gioco; decidere "dormo/sveglio" sotto lock (wake-up perso con atomici
  stile Dekker); ogni attesa con timeout e log.

### Core e affinità: fondere

- Loro non danno affinità ai thread guest con meno di 6 core. Misure: il core
  preferito non fissa il thread (0,24 migrazioni per ciclo), la maschera
  esclusiva peggiora i minimi (16,3 contro ~21 FPS), le migrazioni costano
  ~0,05 % di CPU.
- Noi: mappa morbida (core ideale + maschera dei core guest), rigida solo
  con `nx_hard_thread_affinity`. È coerente con le loro misure: tenere, non
  attivare mai la rigida per default.
- Core 3: loro dicono "tre core su quattro" per l'applicazione. Il nostro
  test con forwarder (maschera 0xF) resta da fare; il loro README chiede un
  forwarder con **spazio di indirizzamento a 39 bit**, da impostare anche nel
  nostro test.

### Clock: adottare `switch_apm`

In portatile un homebrew riceve 307,2 MHz di GPU. Loro chiedono con `apm` la
configurazione standard `0x92220008` (GPU 460,8 MHz, RAM 1331,2 MHz), non un
overclock. Trappole già risolte: `apmSetPerformanceConfiguration` ritorna
prima del cambio (verifica ritardata), `pcv` ripristina la frequenza della RAM
(controllo periodico), gli ID vanno letti dalla tabella reale. Nel loro
gioco: scena da 22,3 a ~12 ms insieme a un'altra modifica. Costo 1 g.

### Audio: adottare

`switch_audio_system.cpp`: audout 48 kHz stereo, tre buffer da 1024 campioni,
pompa a 187,5 Hz, coda di 10 frame (53 ms). Poi i tre fix dell'audio
"robotico": priorità del server audio, versioni native delle funzioni audio
più calde, decodifica XMA spostata dal thread del gioco al worker XMA (da
17,3 % a 1,5 % di un core). La parte generica (driver, XMA asincrono, cache
delle tabelle MDCT di FFmpeg) vale anche per GTA IV; i thread audio del gioco
e le funzioni native sono di NFS. Noi su Switch abbiamo solo il runtime audio
headless. Costo 3–5 g.

### Presentazione e finestra: adottare

`window_switch`, `surface_switch`, `windowed_app_context_switch`,
`runtime_switch`: superficie Vulkan sulla `NWindow` con il WSI di NVK. Limiti
documentati: una sola coda Vulkan, esattamente tre immagini, un solo acquire
per volta, solo FIFO e IMMEDIATE (che non strappa). Un thread di
presentazione separato ha alzato la media e peggiorato il pacing: è spento.
Noi non abbiamo niente. Costo 3–5 g.

## 4. Toolchain

Nessuna delle quattro tecniche ha un numero separato in
`performance-history.md`: insieme hanno portato una corsa da ~30,3 a **~34 FPS**
(+12 %), sopra un gioco già limitato dalla CPU.

| Tecnica | Come | Dati | Per noi |
|---|---|---|---|
| Registri come locali | `cr_as_local`, `xer_as_local`, `ctr_as_local`, `non_volatile_as_local` (no `non_argument_as_local`, no `skip_lr`) | 172.403 accessi a `ctx.xer` e 32.637 a `ctx.ctr` rimossi, 23.857 chiamate `__savegprlr_*`/`__restgprlr_*` rimosse; quelle due funzioni valevano ~14 % del thread principale | il nostro `gta4_config.toml` ha tutto a `false`. Richiede il loro patch `share_registers` del generatore (`builders/context.cpp`, `control_flow.cpp`) e `lee_antes.py`, altrimenti i frammenti di funzione ricevono registri a zero |
| Chiamate dirette | `llamadas_directas.py`: `sub_X(ctx, base)` → `__imp__sub_X(ctx, base)` se `sub_X` non ha hook | 79.612 di 83.077 chiamate; con GCC i `sub_` sono alias weak e non si possono inlinare | adottare; stessa situazione (`DEFINE_REX_FUNC` weak) |
| LTO | `-flto -fno-fat-lto-objects`, link `-flto=2 -flto-partition=balanced` | ~10 min di link, 16 GB bastano | adottare dopo le chiamate dirette |
| PGO | due build dalla stessa cartella; `-fno-profile-values` obbligatorio (TLS a 0 con `-mtp=soft`), `-fprofile-correction`, dump ogni 3 min | build strumentata 66 min, NRO 82,7 MB | più avanti, a gioco avviabile |
| Ordine delle funzioni | file di ordinamento; `KEEP(*(.crt0))` prima riga di `.text`, `_start` a 0 | quattro build non partivano senza quel controllo | più avanti |

Trappola da ricordare: xxHash con `XXH_FORCE_MEMORY_ACCESS 1` + LTO/PGO ha
rotto le chiavi delle texture (strict aliasing). Noi usiamo xxHash: impostare
`XXH_FORCE_MEMORY_ACCESS 0` prima di attivare LTO.

## 5. Edizioni

- Ogni `default.xex` è un programma diverso e ha la sua build.
- PAL spagnolo/tedesco/italiano: stessa compilazione, cinque istruzioni
  diverse. PAL inglese, USA, Giappone: compilazioni diverse (in Giappone si
  sposta anche `.data`).
- Gli strumenti `tools/editions/`: `emparejar.py` (12-grammi di istruzioni
  normalizzate come ancore + sottosequenza crescente più lunga; `.rdata` per
  contenuto), `verificar_ganchos.py` (confronto completo delle funzioni con
  hook), `datos_por_referencias.py` e `verificar_parejas.py` (coppie `lis` +
  parte bassa per `.data`), `crear_arbol.py` (albero dell'edizione con tutti
  gli indirizzi tradotti, partizione delle funzioni seminata dalla
  riferimento), traduzione del profilo PGO.

**Applicabilità al nostro caso.** È la versione completa del matcher usato per
la strada C (06 §6). Differenza importante: loro traducono fra **compilazioni**
diverse dello stesso gioco finito; noi fra la **v8 USA** (base + TU8) e la
**base europea 0.0.0.6 senza TU**. Il codice che la TU8 ha cambiato non ha un
equivalente nella base: `verificar_ganchos.py` lo segnalerebbe funzione per
funzione. Per i 142 indirizzi del config e i ~400 degli hook gli strumenti
darebbero la mappa e l'elenco di cosa non torna. Da adottare quando si esce
dalla strada C (3–5 g, più il lavoro sugli hook che non tornano).

## 6. Renderer

| | nfsmw-nx | `gta4_native` di Theft4 |
|---|---|---|
| Ingresso | consuma il **ring PM4** scritto dal D3D del gioco (registri, draw, copie, Swap) | **sostituisce il D3D**: ~135 funzioni guest agganciate emettono comandi (`SetRenderState`, `SetTexture`, `DrawIndexedPrimitive`, `Resolve`, `Present`) |
| Hook sul gioco | pochi: identità degli shader (creazione), sincronizzazione `SCRATCH_REG` | molti, tutti su indirizzi della v8 USA |
| Piattaforma provata | NVK su Maxwell, Switch, 30–35 FPS | desktop (Vulkan/D3D12) e iOS (Metal) |
| Shader | XenosRecomp → HLSL → DXC → SPIR-V, libreria `.nfsp` costruita dal disco | XenosRecomp, cache SPIR-V smol-v+zstd (`shader_cache.cpp`, escluso dal repo) |
| Pipeline | chiave senza padding, `VkPipelineCache` su SD + elenco delle pipeline ricreate all'avvio (113 in 0,3 s) | `VkPipelineCache` su disco, thread di compilazione asincrona |
| Texture | hash dei byte tiled, untiling NEON solo al cambio, slab da 16 MB, cache limitata a 384 MB | cache propria, nessun vincolo NVK |

**Generale** (vale per GTA IV): decoder del ring e stato dei registri,
render target come immagini Vulkan, resolve senza copia, cache texture con
untiling al caricamento, slab di memoria GPU, cache e prewarm delle pipeline,
costanti via UBO dinamico, le regole sui costi NVK (fence = ioctl, submit =
IPC, allocazioni lente, 8 MB di transfer memory per nvdrv), il pattern
"funzione nativa + guardia che confronta".

**Specifico di NFS:** indirizzi degli hook, riconoscimento dei passi (ombre,
riflessi, cubemap), readback dell'esposizione, funzioni native del gioco,
correzioni del traduttore per il container 2005 e le semantiche dei vertici.

**Raccomandazione: fondere.** Base Switch = il loro nucleo PM4 → Vulkan, che è
provato su NVK e ha bisogno di pochissimi indirizzi, quindi va bene anche per
l'eseguibile europeo. Dal nostro `gta4_native` si porta la conoscenza GTA IV
(shader, passi, post-processing) man mano che serve. Tenere `gta4_native` come
renderer principale su Switch vorrebbe dire legarsi alla v8 USA e rifare da
capo il lavoro sui costi NVK. Costo: settimane, a partire da un renderer che
disegna i primi frame.

## 7. Mesa

| | nfsmw-nx | installato nei portlibs (NaGa) |
|---|---|---|
| Versione | danfromtico/mesa-switch `1a8c1a66d6f` (2026-09-11), Mesa 26.2.1, + `mesa-switch-nfsmw.patch` (35 file) | Mesa 26.2.1 `git-012c67677c` (19/09 nei portlibs; il commit non è pubblico). Il ramo pubblico NaGaa95/mesa-switch parte da `5dba7886c56` (23/09) più 4 commit |
| Fix di upstream fino al 23/09 (L2 solo per fence CPU, pool di query non in cache, hang LDG su GM20B, flush del descriptor table, copy engine multi-riga, memoria CPU-write-only non in cache) | riportati a mano nel patch | già inclusi nella base |
| ZCULL sul percorso Vulkan | abilitato: geometria chiesta al kernel, contesto legato al canale 3D, depth senza `TRANSFER_DST` (~2 ms in una corsa) | **no**: `nvkmd_switch_pdev.c` non chiede la geometria ZCULL, quindi `has_zcull_info` resta falso |
| NAK: latenza texture/global per lo scheduling | 200 cicli | 32 (verificato) |
| NAK: `peephole_select` | 8 | non verificato |
| NAK: `FADD32I` + `.SAT` su SM50 | corretto | **non corretto** (la legalizzazione guarda solo la modalità di arrotondamento): lo stesso bug del "mare nero", un bug di correttezza |
| Percorso di draw (`nvk_switch_dibujo`, set 4 per differenze, rebind UBO ridotti) | sì, con autoverifica | no |
| Revisione del compilatore nella chiave della cache | sì (numero di revisione NAK) | chiave sul build id (commit di danfromtico del 16/09) |
| Extra | — | `VK_EXT_host_image_copy`, allocazioni di memoria condivise, UBO del fragment mantenuti nei cbuf su GM20B |
| EGL/GLES | non servono | presenti (non ci servono) |

**Raccomandazione: fondere.** Costruire noi Mesa: ramo di NaGa (o
danfromtico aggiornato) più le parti del patch di nfsmw che mancano, in
quest'ordine: fix `FADD32I` + `.SAT` (correttezza), ZCULL, latenze di
scheduling e `peephole_select`, percorso di draw. Ambiente di build: MSYS2
MINGW64 + Rust GNU, con i sette problemi già risolti in `docs/mesa.md`. Da
decidere anche la dimensione della transfer memory di nvdrv
(`__nx_nv_transfermem_size`), prima di `nvInitialize()`. Costo 2–3 g per
l'ambiente e il primo build, poi incrementale.

## 8. Conseguenze per la decisione sul modello di memoria

- Il design A resta la raccomandazione. T11 (1 g) chiude i dubbi: alias con
  `svcMapProcessMemory` dai nostri blocchi, costo della loro macro nello
  stesso run, tetto del mappato.
- Indipendentemente dalla memoria, prima di compilare il codice generato per
  Switch conviene portare le priorità 0x3B e rigenerare con i registri come
  locali (patch `share_registers`): cambiano il codice generato, e rigenerare
  costa meno di un minuto.
