# 10 – UnleashedRecomp-NX: cosa vale per GTA IV

Sonic Unleashed ricompilato per Switch: 30 FPS alle frequenze standard, 1152x648
in portatile e 1536x864 in dock (scala 0,9 e 0,8 della finestra). Letto il
2026-09-30.

**Da dove viene cosa.**

| Clone in `<refs>/` | Repository | Stato |
|---|---|---|
| `unleashed-naga` | github.com/NaGaa95/UnleashedRecomp-NX | `84932ed` "0.0.3" (port di NaGa del 2026-07-04 su un lavoro del 19/05) |
| `unleashed-chansey` | github.com/ChanseyIsTheBest/UnleashedRecomp-NX | `b346e37` (tag `1.0.0`): sopra 0.0.3, il ramo `switch-perf-final-0.0.4` fuso con tre PR; dal 2026-10-02 il clone è su `origin/switch-1.0.0` = `1f4803d`, il codice della 1.0.0 (§12) |
| `UnleashedRecomp` | hedge-dev/UnleashedRecomp | upstream PC |

**Il commit `d264d2a` ("Switch: performance work, final build settings,
crash reports (0.0.4)") non è nel repository di NaGaa95**, che è fermo a 0.0.3:
è nel fork di ChanseyIsTheBest (2026-09-30), insieme a `docs/SWITCH-PERFORMANCE.md`
(1178 righe), `SWITCH-PERFORMANCE-AUDIT.md` (792) e `SWITCH-MESA.md` (159), letti
per intero. La memoria guest e il driver audio di partenza sono di NaGa
(`kernel/memory.cpp` è identico nei due repository).

**Architettura diversa dalla nostra.** XenonRecomp (non ReXGlue) per il codice,
un kernel HLE scritto a mano (`kernel/imports.cpp`), e un renderer che
**sostituisce il D3D del gioco** funzione per funzione (`gpu/video.cpp`, 11.000
righe) sopra **plume** (astrazione Vulkan/D3D12 di hedge-dev), non un lettore
del ring PM4. Ogni voce sotto dice se vale così com'è, se va adattata, o se è
legata a plume/XenonRecomp.

## Risultati misurati (loro)

Metodo: ogni round è un NRO più una cartella di `config.toml` per
configurazione, provati negli stessi punti; confronti A/B nella stessa
sessione.

| Test set | 1080p all'hub (limite GPU) | Run limitato dalla CPU | Note |
|---|---|---|---|
| round 4 | frame GPU 29,5 ms (~34 FPS) | — | copie dei resolve ~5,8 ms |
| round 5 | 26,0 ms (38,3 FPS) | 480p: thread principale 70–97 % | 24 copie su 26 diventate hand-over |
| round 6 | 25,45 ms (39,3 FPS) | 480p: 54 FPS (prima 48) | 37 lotti di barriere per frame (prima 60) |
| round 7 | 42 FPS | 480p: 59,3 (56,3 senza le modifiche CPU) | |
| round 8 | 41 FPS | CPU a 1020 MHz: 51; 53,3 con i percorsi veloci del kernel | thread del gioco 17,64 ms di lavoro per frame |
| round 9 | 44,0 FPS (42,4 senza le 4 modifiche poi spente) | 1020 MHz: 53,6 | build PGO + LTO + `-O2` + `-fipa-pta` |

Nessuna tecnica di toolchain ha un numero isolato: sono misurate a gruppi.

## 1. Toolchain

| Tecnica | Come | Guadagno misurato | Per noi |
|---|---|---|---|
| **LTO** | `-flto -fno-fat-lto-objects`; link `-flto=2 -flto-partition=balanced` (`SWITCH_LTO_JOBS`, default 2); `-fno-strict-aliasing` ovunque (xxHash) | nei test NRO dal round 7; round 6: 480p da 48 a 54 FPS insieme alle modifiche CPU del round. Link del gioco intero "circa un'ora" (round 6), build completa del round 10 25 min sul loro PC | **applicabile così com'è**; già raccomandato in 08 §4 (loro e nfsmw-nx usano gli stessi numeri) |
| **`-O2` invece di `-O3`** | tutto il codice (`SWITCH_O2`) o solo il ricompilato (`SWITCH_RECOMP_O2`) | nessun numero isolato; motivo: `-O3` gonfia il codice e l'A57 ha 48 KB di cache istruzioni. Con PGO GCC riaccende l'inlining di `-O3` dove il profilo dice che serve | **applicabile**; da misurare A/B, non da prendere a scatola chiusa |
| **`-fipa-pta`** | in compilazione e al link LTO | nessun numero isolato (round 10) | **applicabile**, solo con LTO |
| **Funzioni calde raggruppate** | 300 funzioni più calde del profilo CPU (`UnleashedRecompLib/config/hot_functions.txt`) con `__attribute__((hot))`, in `.text.hot` (cache istruzioni e TLB) | parte delle modifiche CPU del round 8 (480p: 56,3 → 59,3 FPS tutte insieme) | **adattabile**: serve il profilo CPU su console e un'opzione del nostro generatore per l'attributo; nfsmw-nx fa la stessa cosa con un file di ordinamento del linker (08 §4) |
| **Chiamate dirette** (`tools/switch-direct-calls.py`) | `sub_X` → `__imp__sub_X` quando non c'è hook; 365.513 chiamate verso 41.315 funzioni | presupposto dell'inlining | **già fatto** (`direct_calls.py`: 123.940 su 123.948). Da prendere: riscrivere solo i file che cambiano (la loro prima versione ricompilava sempre tutto) e il controllo sull'ELF finale con i nomi *mangled* (la loro prima versione non trovava nessun hook e non poteva fallire) |
| **Senza frame pointer** | `-fomit-frame-pointer` sul codice ricompilato | nel round 7 | **applicabile così com'è**: il GCC di devkitA64 tiene x29 come frame pointer (lo si vede nel prologo di ogni nostra funzione, anche nella T10 di questo giro). Un registro in più per 36.000 funzioni. L'istantanea dei thread di switch-game non dipende dai frame pointer (scansione dello stack) |
| **`-fno-math-errno -fno-trapping-math`** sul ricompilato | `sqrt()` diventa un solo `fsqrt` invece di `fsqrt` + controllo + salto a libm | non isolato | **applicabile con attenzione**: noi compiliamo con `-ftrapping-math -frounding-math -ffp-contract=off`; `-fno-math-errno` è sicuro (il guest non vede `errno`), `-fno-trapping-math` va verificato contro il nostro modello FPSCR prima di toglierlo |
| **Accessi guest non `volatile`** | `ppc_context.h`: load/store normali, `PPC_LOOP_BARRIER()` (barriera del compilatore) a ogni salto all'indietro perché i cicli d'attesa rileggano la memoria; MMIO resta volatile | parte del round 8 CPU | **adattabile**: il nostro template emette `*(volatile u32*)` in `REX_LOAD_*`/`REX_STORE_*` esattamente come XenonRecomp; serve la barriera sui salti all'indietro nel generatore (`control_flow.cpp`) |
| **Atomiche guest rilassate** | `stwcx.`/`stdcx.` come CAS rilassato (niente `dmb ish` pieno): PowerPC non ordina altro con la prenotazione, `sync`/`lwsync`/`eieio` restano barriere | 10.695 siti; parte del round 7 | **adattabile**: il nostro `emitAtomicStoreConditional` (`builders/helpers.h`) usa `__sync_bool_compare_and_swap`, cioè barriera piena, come il loro prima. Ha un interruttore suo: se un blocco o un problema audio sparisce spegnendolo, il gioco dipende da più ordinamento di quello di PowerPC |
| Store dei registri callee-saved morti eliminati; modo FPSCR seguito oltre le etichette | nel generatore | round 8 | **legato a XenonRecomp** come codice; l'idea vale per i nostri builder (`share_registers` e registri come locali ci sono già) |

## 2. PGO: la procedura completa

1. **Build `generate`** (`SWITCH_PGO=generate`, **senza LTO**: lo script lo
   rifiuta). Opzioni di compilazione:
   `-fprofile-generate=<cartella sul PC> -fprofile-prefix-path=<cartella di build> -fprofile-update=single -fno-profile-values`;
   link `-fprofile-generate -Wl,--wrap=fopen`.
   - `-fno-profile-values` è **obbligatorio con `-mtp=soft`** (nfsmw-nx: la
     profilazione dei valori usa TLS, e con il TLS software di libnx dava 0).
   - `-fprofile-update=single`: contatori senza atomiche (A57 senza LSE); da qui
     i contatori negativi che la build `use` corregge.
   - `-fprofile-prefix-path`: il nome di ogni `.gcda` diventa relativo alla
     cartella di build (`#` al posto di `/`), uguale sul PC e sulla SD.
2. **Sulla console** (`os/switch/pgo_switch.cpp`, preso da nfsmw-nx):
   `__wrap_fopen` ridirige i percorsi della cartella del PC a
   `sdmc:/switch/UnleashedRecomp/pgo/`; un thread a 0x3B ogni **3 minuti** fa
   `__gcov_dump()` + `__gcov_reset()` e scrive il tempo in `dumps.txt`. Non si
   aspetta l'uscita: chiudere da HOME uccide il processo. libgcov somma a quello
   che c'è già nei `.gcda`, quindi le sessioni si accumulano. Giocare più a
   lungo possibile, su tutto il gioco.
3. **Build `use`** (`SWITCH_PGO=use SWITCH_LTO=1`), dalla **stessa cartella di
   build**, senza rigenerare né toccare i sorgenti:
   `-fprofile-use=<cartella> -fprofile-prefix-path=... -fprofile-partial-training -fno-profile-values -fprofile-correction -Wno-missing-profile -Wno-error=coverage-mismatch`;
   link `-fprofile-use -fprofile-partial-training -fprofile-correction`.
   - `-fprofile-correction`: i contatori non sincronizzati possono uscire
     negativi, GCC altrimenti li rifiuta.
   - `-fprofile-partial-training`: il codice non visto durante il gioco resta
     ottimizzato normalmente invece che "freddo".
4. **Il profilo vale solo per lo stesso codice allo stesso `-O`.** Il loro
   round 10 ha usato un profilo raccolto a `-O3` con una build `-O2`: 87 % delle
   funzioni combaciava, 8.846 su 66.629 (due delle sette più calde) senza
   profilo. Raccogliere con le stesse opzioni della build finale. Da rifare
   dopo ogni rigenerazione del codice o cambio dell'elenco degli hook.

**Per noi: applicabile così com'è**, a gioco avviabile (passi preparati in 06
§11). È la stessa procedura di nfsmw-nx (08 §4), con in più
`-fprofile-partial-training` e il dump periodico.

## 3. LZX nativo e GTA IV

**Loro** (`os/switch/lzx_switch.cpp`, `misc_impl.cpp`): `XMemDecompress` (il
decoder LZX dell'XDK linkato nel gioco, `sub_831CE0D0`) prendeva il 42 % del
thread di caricamento. Un port nativo dello stesso framing e decoder, **solo per
i contesti senza il flag di streaming** (che il gioco resetta a ogni chiamata);
in ogni caso incerto gira il decoder del gioco. Verificato identico su 320
archivi (323 MB); 370 MB/s sul PC. Con i percorsi veloci del kernel, da 50 a
53,3 FPS nel run CPU del round 8.

**GTA IV Xbox 360 usa due formati:**

- **Archivi RPF2**: le voci compresse sono **deflate grezzo** (zlib,
  `inflateInit2(-MAX_WBITS)`, bit `0x40000000` nella voce). Lo legge solo
  l'installer (`gta4-recomp/src/install/gta4_rpf_extractor.cpp`,
  `LibertyRecomp/install/rpf_extractor.cpp`).
- **Risorse in memoria: LZX** con un decoder **persistente** (finestra
  `0x20000`, frame fino a `0x8000`, stato nel contesto a `+0x14`), cioè il caso
  *con* stato fra le chiamate, quello che UnleashedRecomp lascia al gioco. Il
  gioco lo decomprime in `sub_82A21BF0` (con `sub_82A21680`, `sub_82A21F70`, il
  parser dei blocchi `sub_82A21700`; indirizzi della **v8 USA**).

**Theft4 ha già un LZX nativo**: `gta4-recomp/src/gta4_lzx_hooks.cpp` aggancia
quelle funzioni con `rex::lzx::PersistentDecoder` e tre modi
(`REX_GTA4_LZX_MODE=guest|verify|host`, default `guest`), cioè il loro stesso
schema "nativo con verifica". Per Switch: **adattabile**. Gli indirizzi vanno
ritrovati nell'eseguibile europeo (strada C, 06 §0) e il modo `verify` va
provato sulla console prima di `host`. Il loro dato dice che vale la pena (42 %
del thread di caricamento), e in GTA IV lo streaming è continuo.

## 4. Kernel guest: percorsi veloci della sincronizzazione

| Loro | Cosa | Nel nostro kernel (ReXGlue, derivato da Xenia) |
|---|---|---|
| `SwitchFastCriticalSections` | chi sta per aspettare si conta in `LockCount`; chi esce chiama `svcSignalToAddress` solo se c'è qualcuno | **già così**: `RtlLeaveCriticalSection` (`xboxkrnl_rtl.cpp`) segnala l'evento solo se `atomic_dec(lock_count) != -1` |
| `SwitchFastEvents` | eventi e semafori contano i waiter sotto il mutex dell'oggetto e saltano i due notify se sono 0 | **si applica**: `threading_switch.cpp` fa `cond_.notify_all()` a ogni `Set`/`Release`, e su Horizon ogni notify è una SVC. Adattamento piccolo e locale (contatore sotto `mutex_`) |
| `SwitchGuestSpinBeforeSleep` | `KfAcquireSpinLock` legge la parola del lock fino a 512 volte (~2 µs) prima di dormire 10 µs | **da verificare** su `xeKeKfAcquireSpinLock` quando il gioco gira: dipende da come dorme il nostro |
| r13 dal contesto invece che da TLS | `RtlEnterCriticalSection` prende l'id del thread da `ctx.r13`, non da un `thread_local` | **si applica**: con `-mtp=soft` ogni `thread_local` è una chiamata (`__aarch64_read_tp`, 0,7 % del thread principale da loro). Il nostro `XThread::GetCurrentThread()` è un `thread_local`, chiamato in `RtlLeaveCriticalSection` (assert) e in molte export |
| Critical section che fanno spin, lookup senza lock, array TLS | provati e **tolti**: l'audio si fermava per sempre alla prima cutscene | lezione: ogni modifica al kernel guest ha il suo interruttore e un test fisso (per GTA IV: una cutscene con audio) |

Guadagno misurato: −1,2 ms di lavoro per frame del thread del gioco a
1020 MHz (config 7 del round 10), 51 → 53,3 FPS nel round 8 insieme al LZX
nativo.

## 5. Patch ai sottomoduli

| Patch | Contenuto | Per noi |
|---|---|---|
| `patches/XenonRecomp.patch` (643 righe cambiate) | chiamate dirette, accessi non volatili, atomiche rilassate, store morti, FPSCR, `.text.hot` | **adattabile** al nostro generatore (§1); il codice è di XenonRecomp |
| `patches/XenosRecomp-switch-perf.patch` (3.602) | costanti in UBO dinamici, blocchi di predicato condivisi, `max(a,a)`, gather per le ombre, specializzazione dello skinning, uscite di vertice e canali colore non usati, uscita anticipata e "sinking" dell'alpha test (646 e poi 853 pixel shader), `if/else` invece della macchina a stati | **adattabile**: il nostro XenosRecomp è lo stesso strumento (`tools/XenosRecomp`); l'uscita dipende da come il renderer lega costanti e specializzazioni. Il più generale: costanti via UBO dinamico (NVK le promuove ai banchi di costanti anche su Maxwell), che nfsmw-nx fa già |
| `patches/plume-switch-perf.patch` (1.139) | bind ridondanti filtrati, buffer mappati in modo persistente, barriere con maschere dai layout, `VkPipelineCache` persistente, depth senza `TRANSFER_DST`, `S8_UINT`, riferimento stencil dinamico, messenger `VK_EXT_debug_utils` | **legato a plume**, ma le regole valgono per qualunque renderer Vulkan su NVK (§8) |

## 6. Memoria guest

**NaGa** (`kernel/memory.cpp`, identico nei due fork):

- una finestra da 4 GiB riservata con `virtmemFindAslr` + `virtmemAddReservation`;
  accesso **diretto** `base + addr`, come nfsmw-nx su Switch;
- **commit esplicito, non su fault**: `CommitRange` a pagine da 4 KiB, chiamato
  dall'heap del gioco e da poche export (`heap.cpp`, `imports.cpp`); ogni tratto
  non impegnato diventa `memalign` → `virtmemFindCodeMemory` →
  `svcMapProcessCodeMemory` → `svcSetProcessMemoryPermission(RW)` →
  `svcMapProcessMemory` nella finestra. È la primitiva del nostro T11.1 e di
  nfsmw-nx;
- **una sola vista**: il kernel di UnleashedRecomp non emula le finestre
  fisiche 0xA/0xC/0xE della 360, quindi niente mirror e niente tetto del mappato
  moltiplicato;
- `__nx_heap_size = 0` (tutto l'heap a newlib); all'avvio impegna il primo
  128 KiB, immagine + tabella delle funzioni e l'area XMA.

| | UnleashedRecomp-NX | nfsmw-nx | Theft4 (modello A) |
|---|---|---|---|
| Accesso nel codice | `base + addr` | `base + addr` | `addr + rex_guest_table[addr >> 24]` |
| Viste | 1 | fino a 5, mappate al primo accesso di ciascuna (fault) | nessun alias: tabella |
| Commit | esplicito, 4 KiB, a ogni allocazione | su fault, blocchi da 4 MB (da 1 MB il 29/09) | esplicito, blocchi da 2 MiB da un pool preso all'avvio |
| Fault per mappare | no | sì | no |
| Protezione di pagina | non usata | smappare | `svcSetMemoryPermission` a 4 KiB |

Per GTA IV le viste fisiche servono (il gioco e il D3D usano 0xA/0xC/0xE),
quindi il loro modello a vista unica non si applica. **Il modello A resta la
scelta**: i run 8 e 10 (T11) lo sostengono, e questo port non aggiunge dati
contrari. Da loro vale l'osservazione che un commit esplicito a ogni
allocazione non ha bisogno di fault.

## 7. Thread, priorità, streaming e I/O su SD

**Priorità:**

| Priorità | Thread |
|---|---|
| 0x1E | campionatore CPU e watchdog degli stalli (solo lettura) |
| 0x20 | pompa audio che alimenta `audout` |
| 0x2C | thread principale del gioco (presentazione) |
| 0x2D | thread di render (`SwitchRenderThreadPriority`; provato 0x2B) |
| 0x3B | thread guest (default di libnx), compilatori di pipeline |

Come le nostre bande (04 §7), con due differenze: la pompa audio più in alto
(0x20 contro il nostro 0x2B) e il campionatore a 0x1E. Loro hanno
`KeSetBasePriorityThread` che mappa intorno a 0x2C (lo segnalano come limite
noto); noi l'abbiamo già risolto: un XThread guest resta a 0x3B qualunque
priorità chieda.

**Core:** render thread sul core 1 e compilatori sul core 2
(`SwitchHostThreadCores`, 480p: 59,0 FPS senza contro 59,3). Coerente con la
nostra mappa (worker host sul core 3 quando c'è).

**Spin prima di dormire:** la coda di render faceva 10.000 giri prima di
dormire; a 0x2D (senza time-slicing) sono scesi a 1.000.

**TLS:** con `-mtp=soft` ogni `thread_local` è una chiamata. Hanno tolto i
`thread_local` dai percorsi caldi (test del thread D3D con il registro
`TPIDRRO_EL0`, critical section, RTTI, cache della UI). **Per noi vale**: la
nuova traccia delle export di questo giro legge un `thread_local` per chiamata
(accettato per la diagnosi; da togliere o spegnere quando il gioco gira).

**SD e streaming:**

- niente scritture sulla SD da thread su cui altri aspettano: i loro blocchi a
  480p erano il campionatore che scriveva il report riga per riga tenendo un
  mutex che il thread del gioco prendeva. Ora copia sotto il lock e scrive
  fuori, in una sola `write`. Il nostro log durevole scrive riga per riga con
  `fsync`: va bene per la diagnosi, non per il gioco (da rivedere prima delle
  misure di prestazioni);
- cache delle pipeline salvata alla fine delle schermate di caricamento, non
  ogni minuto; niente salvataggio all'uscita (HOME uccide il processo);
- `MESA_SHADER_CACHE_DISABLE` quando c'è la propria cache (duplicato e thread
  di scrittura su SD);
- opportunità non fatta: upload delle texture di streaming raggruppati nel
  command buffer del frame invece di un submit e un'attesa ciascuno (una sola
  coda, ogni submit è IPC).

## 8. Renderer: cosa vale su NVK a prescindere da plume

Valgono per qualunque renderer Vulkan su NVK/Maxwell, quindi anche per il
nucleo PM4 di nfsmw-nx che vogliamo portare (06 §10):

- **Costanti in UBO dinamici**, non puntatori letti con `RawBufferLoad`: NVK li
  promuove ai banchi di costanti (anche su Maxwell).
- **Heap dei descrittori delle texture ≤ 64 KB** (16.384 voci da 4 byte): NVK
  lo legge da un banco di costanti; oltre va in memoria. 22,65 → 21,30 ms di
  frame GPU all'hub.
- **Bind ridondanti filtrati**: su NVK ribindare la stessa pipeline riemette
  tutto il suo stato.
- **Barriere con maschere derivate dai layout**: `MEMORY_READ|WRITE` su ogni
  barriera diventa flush + invalidazione di texture, costanti, dati shader e
  MME. Da validare sul PC con il layer di sincronizzazione.
- **Resolve pigri e hand-over** (l'immagine passa alla texture invece di una
  copia): 24 copie su 26 per frame eliminate, 29,5 → 26,0 ms a 1080p.
  È la stessa idea del "resolve senza copia" di nfsmw-nx.
- **Depth senza `TRANSFER_DST`** (condizione di ZCULL); ZCULL in modo `greater`
  con Z inverso (−0,17 ms).
- **Draw solo-depth senza fragment shader**, uscite di vertice e canali colore
  non letti tolti, draw che non scrivono niente saltati.
- **Timestamp GPU ×1,627** (NVK dice 1 ns): il loro profiler leggeva il 60 %
  del vero.
- **Un solo submit per frame**, presentazione sul thread di render
  (round 10: il thread del gioco non aspetta più 1,06 ms in Present).

### Le quattro ottimizzazioni spente (perdita della GPU)

`SwitchSkipDeadCopies`, `SwitchCarryClears`,
`SwitchSkipOverwrittenDepthClears`, `SwitchEagerDepthTransitions`:

- con tutte e quattro attive, due sessioni lunghe (21.040 e ~28.000 frame,
  15–20 minuti, 1080p) sono finite con `VK_ERROR_DEVICE_LOST` da
  `vkQueueSubmit`, poi il blocco del gioco dentro `vkQueuePresentKHR`; con le
  quattro spente, stessa partita senza perdita. Valgono 1,6 FPS (44,0 contro
  42,4);
- quale delle quattro non si sa. Sospette le due sulla depth: lo ZCULL del
  driver fa `LOAD_ZCULL`/`STORE_ZCULL` a ogni pass e azzera il piano solo
  all'uscita da `UNDEFINED`, e il suo stesso commento dice che `LOAD_ZCULL` su
  dati sbagliati uccide il contesto GPU;
- **lezione per noi**: le ottimizzazioni che cambiano quando una depth esce dal
  layout di attachment o come viene pulita vanno provate in sessioni lunghe
  (30 minuti o più) e una alla volta, con ZCULL attivo.

## 9. Diagnostica: perdita della GPU e watchdog

- **Crash report CPU** (`os/switch/crash_switch.cpp`, da `nx_crash_handler.c`
  di battd_nx): thread, pc/lr/far/esr, registri, catena dei frame pointer,
  indirizzi di ritorno trovati sullo stack e un dump dello stack, scritti con il
  servizio file system direttamente (il thread in fault può tenere il lock di
  stdio o dell'heap), poi `svcBreak`. È quello che fanno il nostro
  `game-crash.log` + trampolino verso `svcBreak` (run 10–11); **da prendere**:
  gli indirizzi di ritorno dallo stack (fatto in questo giro per l'istantanea dei
  thread) e la scrittura senza stdio.
- **Perdita della GPU**: plume richiama al primo `VK_ERROR_DEVICE_LOST` (submit,
  attesa di fence, lettura di query, present, acquire). Una Mesa release dà i
  suoi messaggi di errore **solo a un messenger `VK_EXT_debug_utils`**, quindi
  plume ne crea uno su Switch (solo errori): il report contiene
  `nvkmd-switch: channel N lost: notification={type info status} error={...}`;
  tipo 8 = timeout di inattività della GPU (un blocco), gli altri sono fault.
  Poi `svcBreak` invece del blocco. **Applicabile così com'è** a qualunque
  renderer Vulkan: da mettere dal primo giorno della grafica.
- **Watchdog degli stalli** (`stall_watch_switch.cpp`, da battd_nx): thread a
  0x1E che, se nessun frame è presentato da un secondo, scrive per ogni thread
  registrato PC, indirizzi di ritorno sullo stack, tempo CPU dall'ultima
  occhiata, code del renderer (`[stall]`), e una volta al secondo i frame sopra
  100 ms (`[hitch]`). Dal round 10 vede le SVC dove Horizon mette il PC di un
  thread bloccato (sull'istruzione `svc`, non dopo). **Adattato in questo giro**
  per switch-game: istantanea di tutti gli XThread a 5, 20, 60 s e poi ogni 2
  minuti (senza frame non c'è ancora il criterio "nessun frame da 1 s").
- **Campionatore CPU**: ogni 2 ms mette in pausa ogni thread registrato, legge
  il PC, lo riprende; report ogni 30 s con le 150 righe di codice da 16 byte più
  calde, nominate offline (`tools/switch-cpu-profile.py`, funzioni ricompilate
  come `sub_82XXXXXX`). Stesso meccanismo della nostra istantanea
  (`svcSetThreadActivity` + `svcGetThreadContext3`): **adattabile**.

## 10. Audio

**Il guasto:** il backend Switch di SDL suona da un thread suo, creato con
`SDL_THREAD_PRIORITY_TIME_CRITICAL`, che l'SDL di devkitPro mappa a 0x3B, la
priorità più bassa. Dopo aver accodato un buffer, il suo ciclo aspetta che
risulti *in riproduzione*; un thread affamato per più di un buffer lo trova già
*finito* e aspetta per sempre. La pompa vede la coda piena e smette di chiamare
il mixer del gioco: silenzio permanente, niente nel log.

**La correzione** (`apu/driver/sdl2_driver.cpp`, `SwitchAudioOut`): la pompa
(0x20) alimenta `audout` direttamente, 16 buffer da 0x1000 byte, 3 blocchi in
coda, stessa conversione; un underrun è un buco contato e la riproduzione
continua. 0 buchi in tutti i run del round 6.

**Il nostro `switch_audio_driver`** (controllato in questo giro): non usa SDL,
quindi niente thread audio a 0x3B; usa `audren` (non `audout`), alimentato dal
thread `Audio Worker` del runtime (0x2B per nome) che chiama `SubmitFrame`.
Aveva però **lo stesso tipo di rischio** e un secondo problema:

1. **Attesa senza limite:** con i due buffer d'onda occupati,
   `WaitForCurrentSlotReusable` aspettava in `audrenWaitFrame()` senza timeout.
   Se il renderer audio smette di consumare, il worker resta bloccato per
   sempre, in silenzio. **Corretto:** attesa a passi di 1 ms, warning dopo
   100 ms, frame scartato dopo 500 ms (il buffer è ancora nella coda della voce e
   non si può riusare).
2. **Priorità sbagliata:** `Initialize` faceva
   `svcSetThreadPriority(CUR_THREAD_HANDLE, 0x28)` sul thread chiamante, che è
   il thread **guest** che chiama `XAudioRegisterRenderDriverClient`: lo portava
   da 0x3B a 0x28, banda cooperativa, dove un thread guest che fa spin tiene il
   suo core. **Corretto:** tolto; il worker ha già 0x2B per nome.

Resta un margine stretto: due buffer da 256 campioni (10,7 ms in tutto) davanti
a un renderer che lavora a frame da 5 ms. nfsmw-nx usa `audout` con tre buffer
da 1024 campioni, una pompa a 187,5 Hz e una coda di 10 frame (53 ms). Quando
l'audio del gioco girerà su Switch: passare allo schema di nfsmw-nx (08 §3), che
è quello misurato.

## 11. Nota: il percorso plume di `LibertyRecomp/` per GTA IV

Theft4 contiene ancora, in `LibertyRecomp/` + `LibertyRecompLib/`, il
percorso in stile UnleashedRecomp: codice da XenonRecomp, kernel HLE proprio,
`gpu/video.cpp` di 10.900 righe su plume che **sostituisce il D3D di GTA IV**
(46 `GUEST_FUNCTION_HOOK` su `sub_8…`, più altri 66 commentati, tutti indirizzi
della v8 USA).

**Pro:**

- **plume su NVK è provato**: UnleashedRecomp-NX arriva a 30 FPS stabili e
  44 FPS a 1080p all'hub, con tutte le regole NVK già applicate nel patch.
- **Niente emulazione della GPU**: né ring PM4 né EDRAM; il lavoro per draw è
  una chiamata di funzione, non la decodifica di pacchetti.
- `LibertyRecomp/gpu` ha già la conoscenza di GTA IV (stato di render,
  post-processing, ombre, upscaler).

**Contro:**

- **È un'altra pila.** XenonRecomp e il kernel HLE di LibertyRecomp, non
  ReXGlue: il nostro livello Horizon (memoria modello A, fault, priorità, run
  6–11) non gli serve così com'è, e il codice generato per Switch (36.643
  funzioni, chiamate dirette, registri come locali) andrebbe rifatto con
  XenonRecomp.
- **Legato alla v8 USA**: ogni hook del D3D è un indirizzo della v8; con
  l'eseguibile europeo (strada C) sono tutti da ritrovare, come i 401 hook di
  `gta4-recomp/src` e i ~135 di `gta4_native`. Il nucleo PM4 di nfsmw-nx invece
  ha bisogno di pochissimi indirizzi (creazione degli shader, annotazione dei
  draw).
- **La memoria guest** di UnleashedRecomp è a vista unica; GTA IV su
  LibertyRecomp usa il suo modello, non verificato su Horizon.
- Lo stesso lavoro di prestazioni (resolve, hand-over, UBO, barriere) va fatto
  comunque: plume non lo porta da solo, è nel loro patch.

**Valutazione:** è un'alternativa realistica **solo se** si decide di
abbandonare ReXGlue e la strada C per la v8 USA con XenonRecomp. Restando su
ReXGlue (la strada di tutti i run fin qui), la sostituzione del D3D equivale a
`gta4_native` con indirizzi della v8, e 06 §10 sceglie il nucleo PM4 di
nfsmw-nx per non legarsi agli indirizzi. Da UnleashedRecomp-NX si prendono le
**regole NVK** (§8), la **diagnostica** (§9) e la **procedura PGO** (§2), che
valgono con qualunque renderer.

## 12. Release 1.0.0 (letta il 2026-10-02)

Note della release: più prestazioni, corretta la regressione degli occhi scuri
della 0.0.3 (in realtà presente da 0.0.4), funziona in qualsiasi cartella,
clock GPU a 460 MHz in portatile. Chansey (Discord): il guadagno viene da un
profiler CPU nuovo centrato sui cali di frame (esplosioni, scontri con molti
oggetti) e dalle ottimizzazioni trovate con quello nel codice del gioco.

### Dove sta il codice

**Il tag `1.0.0` punta a `b346e37`, ma il codice della 1.0.0 non è nei commit
precedenti al tag.** Fra il merge `1aa574c` (0.0.4) e il tag ci sono solo:

| Commit | Data | File | Descrizione | Tipo |
|---|---|---|---|---|
| `8ecdf2e` | 2026-09-30 | `docs/SWITCH-PERFORMANCE-AUDIT.md` (+109/−9) | audit della build finale 0.0.4 | documentazione |
| `347ee17` | 2026-09-30 | — | merge della PR #2 | merge |
| `4a69709` | 2026-09-30 | `docs/SWITCH-PERFORMANCE-AUDIT.md` (+158/−19) | sezione completa sul driver nell'audit | documentazione |
| `b346e37` | 2026-09-30 | — | merge della PR #3 (= tag `1.0.0`) | merge |

Il codice è in **un solo commit, `1f4803d`** (2026-10-02, "Switch 1.0.0: dark
eyes fixed, rounds 11-16, new PGO profile, any SD folder"), sul ramo
**`switch-1.0.0`**, figlio di `4a69709`: il tag non lo contiene. 439 file (366 sono i `.gcda` del profilo PGO), tra
cui `gpu/video.cpp` (+1390), `patches/XenonRecomp.patch` (+2589),
`patches/native_hot_patches.cpp` (1288, nuovo), `patches/audio_dsp_patches.cpp`
(698, nuovo), `patches/message_dispatch.cpp` (600, nuovo),
`os/switch/cpu_profiler_switch.cpp` (+409), `docs/SWITCH-PERFORMANCE.md`
(+450), `docs/SWITCH-DARK-EYES.md` (293, nuovo) e il profilo PGO del round 16.
Clone aggiornato in `<refs>/unleashed-chansey`, ora su
`origin/switch-1.0.0`. Le modifiche sotto vengono dai round 11–16 di
`docs/SWITCH-PERFORMANCE.md`, tutte con una chiave `[Switch]` propria e
"image-identical".

### Le modifiche, una per una

CPU/GPU; Sonic = legata al codice o ai dati di Sonic Unleashed (indirizzi,
strutture, librerie CRI/Havok), generale = vale per qualunque gioco
ricompilato; noi = applicabile a switch-game.

| Modifica (round) | CPU/GPU | Sonic o generale | Applicabile a noi |
|---|---|---|---|
| Dispatch dei messaggi a tabella (11): 227 dispatcher letti all'avvio, una ricerca invece di 200 confronti | CPU | Sonic (motore Hedgehog) | idea sì (trovare con il profiler i dispatcher lineari di GTA IV), codice no |
| Insieme esatto dei `type_info` (11): niente confronti di stringhe | CPU | generale per giochi MSVC con RTTI | sì, da verificare che GTA IV confronti `type_info` per nome |
| Funzioni calde native con hash FNV e modo di verifica (11–13, 15) | CPU | Sonic (indirizzi), il metodo è generale | il metodo sì (hash del codice guest all'avvio, verifica registri/memoria): è quello da usare per qualunque funzione nativa di GTA IV |
| Present senza attesa della registrazione (11) | CPU/GPU | legata al loro renderer | quando avremo un renderer |
| Kernel del mixer CRI con registri locali (11, 15: ADX, resampler, riverbero) | CPU | Sonic (CRI) | no (GTA IV ha il suo motore audio); il metodo `switch-localize.py` sì |
| LZX con stato nei registri, cache delle pipeline scritta solo dopo i miss, elenchi di cartelle senza una query per file (11) | CPU, I/O | generale | LZX: GTA IV lo usa con stato persistente, già coperto da `gta4_lzx_hooks.cpp` (§3); gli altri due sì |
| Passo di generazione del codice (11): funzioni foglia con i registri in locali, accessi D-form a 64 bit, tabelle VMX costanti | CPU | generale (XenonRecomp) | da riportare in `rexglue` (06 §13): è il lavoro più trasferibile |
| Attese di risorsa a 0,5 ms invece di `Sleep(5)` (12) | CPU | Sonic (cinque cicli del loader) | idea sì: cercare i cicli `Sleep` del caricamento di GTA IV (ne abbiamo appena trovato uno all'avvio, 06 §19) |
| Confronti in virgola mobile inline (12): 11.669 chiamate rimosse | CPU | generale | sì, nel generatore |
| Uscita dalla sezione critica senza fence (12) | CPU | generale (kernel HLE) | sì, nel nostro `RtlLeaveCriticalSection`, da verificare con l'audio |
| Profiler dei fotogrammi lenti (12–13) | diagnostica | generale | sì, piano in 06 §20 |
| Light field nativo (13), prefetch del render walk (13), prefetch del grafo di scena (15) | CPU | Sonic | no; i prefetch `dcbt` come prefetch host (13) sì, nel generatore |
| Attese dei lock per oggetto (13) | CPU | legata al loro D3D | no |
| Vibrazione deduplicata (13): uno stop per IPC HID solo una volta | CPU | generale | sì in `nx_input` quando ci sarà la vibrazione |
| Gamma saltata con esponente 1 (13) | GPU | legata al renderer | quando avremo un renderer |
| Meno cambi di modo FPCR (14) | CPU | generale (XenonRecomp) | sì, nel generatore (5 % del thread principale da loro) |
| `memcpy`/`memset` guest di dimensione costante inline (14) | CPU | generale | sì, nel generatore |
| Risvegli mirati del dispatcher e semafori che svegliano uno solo (14) | CPU | generale (kernel HLE) | sì: da confrontare con `KeWaitForMultipleObjects` e i semafori di ReXGlue |
| Sezioni critiche: CAS forte e breve spin (14) | CPU | generale | sì, da verificare |
| Indice dei modificatori dell'interfaccia, cache più grande (14, 15) | CPU | Sonic | no |
| Multiply-add espliciti (`-ffp-contract=off` + `fma` solo dove il guest le ha) (15) | CPU, fedeltà | generale | sì, nel generatore: rende identiche le ricompilazioni |
| `1/sqrt` scalare dei prodotti scalari (15) | CPU | generale | sì, nel generatore |
| Allocatore a pool nativo, animazione dei materiali, cluster localizzati (15) | CPU | Sonic | no (il metodo sì) |
| Copie dei resolve decise al submit, campionamento di profondità in sola lettura, ripristino di profondità in alias, costanti dei vertici conservate, reset delle query uno per uno (15) | GPU | legate al loro renderer (D3D sostituito) | solo come idee per il renderer nativo; il reset delle query uno per uno (NVK usa il copy engine per più query) vale per qualunque renderer su NVK |
| `vkCmdDrawIndirect` evitato sul percorso pre-Turing del fork Mesa (15) | GPU | generale (driver) | sì, regola NVK (11-vulkan-probe.md) |
| Occhi scuri (16): bug del loro traduttore di shader (alpha-test sinking che stampava le righe non rinominate di un filtro d'ombra); verifica di 1.385 shader contro la 0.0.3 | GPU, fedeltà | loro (XenosRecomp modificato) | no; il metodo di verifica (tradurre con due versioni e confrontare su ingressi uguali) sì |
| PGO col profilo del round 16 (1.0.0) | CPU | generale | sì, procedura in §2 e 06 §13 |
| Boost GPU in portatile 460,8 MHz (1.0.0) | GPU | generale | sì, sotto |
| Nessun file di log per default (1.0.0) | I/O | generale | non ora (servono i log); per una release sì |
| `config.toml` con tutte le opzioni (1.0.0) | — | generale | sì per una release (`game.toml`) |
| Qualsiasi cartella della SD (1.0.0) | — | generale | sì, sotto |

### Il profiler

Descritto in 06 §20 con il piano per switch-game. In breve: thread
registrati; un campionatore ogni 2 ms che scarta i thread che non hanno girato
(`InfoType_ThreadTickCount`), mette in pausa gli altri
(`svcSetThreadActivity` + `svcGetThreadContext3`), legge PC e, se in una SVC,
gli indirizzi di ritorno sullo stack, e li riprende; report ogni 30 s in una
sola scrittura, nominato offline con `nm` (`tools/switch-cpu-profile.py`). I
fotogrammi lenti: `FrameWorkEnd`/`FrameWorkStart` attorno a `Present`, il
thread del gioco campionato sempre (anche in attesa) in un anello di 2 s, i
campioni dei fotogrammi oltre soglia in una sezione propria del report. Lo
stall watchdog (`stall_watch_switch.cpp`): nessun fotogramma da N s → per ogni
thread "ha girato?", PC, LR, frame pointer, ritorni sullo stack, SVC in corso;
`[hitch]` per i fotogrammi oltre 100 ms.

### "Funziona in qualsiasi cartella"

`os/switch/process_switch.cpp`: legge `__system_argv[0]` (il percorso
dell'NRO che hbmenu e i forwarder passano come `argv[0]`, impostato da
`argvSetup()` prima dei costruttori statici), normalizza `\` in `/`, aggiunge
`sdmc:` se manca, e lo accetta solo se comincia con `sdmc:/` e finisce in
`.nro`; altrimenti `sdmc:/switch/UnleashedRecomp/UnleashedRecomp.nro`. La
cartella dell'NRO diventa la radice di file del gioco, `config.toml`,
salvataggi, cache, `install/` e log (`user/paths.cpp`). NRO alla radice della
SD: `sdmc:/`.

**Da noi** `kRootDir = "sdmc:/switch/theft4"` e
`kGameDir = "sdmc:/switch/theft4/game"` sono fissi in
`tools/switch-game/main.cpp` (con log, crash log, `game.toml`, `cache`,
`cache1`, `user`, `saves`, `marketplace`, `runtime-cache` sotto). **Applicabile
così com'è**: stessa funzione, radice = cartella dell'NRO, gioco in
`<radice>/game`, ripiego su `sdmc:/switch/theft4` se `argv[0]` non serve
(nxlink passa un percorso diverso). Va fatto prima di `StartLogging()` (il log
sta nella radice). Non urgente: una riga in più nel log con la radice scelta.

### "Clock GPU a 460 MHz in portatile"

`os/switch/perf_switch.cpp` (dichiaratamente da `sdk/src/ui/switch_apm.cpp` di
nfsmw-nx), su un thread libnx proprio (0x10000 di stack, priorità 0x3B):
`apmInitialize`; se `apmGetPerformanceMode` non è `Normal` (portatile) non fa
niente; legge la configurazione attuale e i clock reali con `clkrst` (GPU,
EMC); `apmSetPerformanceConfiguration(Normal, 0x92220008)` (GPU 460,8 MHz, RAM
1331,2: la coppia dei giochi commerciali; 0x92220007 alzerebbe la RAM a 1600);
poi controlla i clock ogni 150 ms per 1,5 s, perché apm ritorna prima che pcv
li applichi; se la RAM si è mossa di più di 20 MHz rimette la configurazione
di prima. La sessione apm resta aperta. nfsmw-nx fa lo stesso con una tabella
di configurazioni e un controllo periodico della RAM
(`RexSwitchApmVigilar`), e in dock non chiede niente; nessuno dei due la
riapplica al cambio dock/portatile.

**Da noi:** in `GameMain`, dopo `StartLogging()` (per scrivere il risultato)
e prima di creare il runtime, su un thread libnx proprio come loro, dietro una
cvar (`nx_handheld_gpu_boost`). **Quando**: solo con una GPU vera; con la GPU
nulla non c'è lavoro di GPU da accelerare e si cambierebbe solo il consumo.
**Al cambio dock/portatile**: un `appletHook` su
`AppletHookType_OnPerformanceMode` che, entrando in `Normal`, la applica se non
è già stata applicata (caso: avvio in dock, poi sganciata); in dock la
configurazione di `Boost` resta quella del sistema.

### Tabella finale

| Modifica | Guadagno dichiarato | Applicabile a noi | Quando |
|---|---|---|---|
| Round 11 (dispatch, natives, mixer, generazione del codice) | 57,7 FPS contro 52 (858x482, CPU a 1020 MHz) | generazione del codice e metodo dei natives sì; dispatch e mixer no | generatore: quando il gioco gira (06 §13); natives: dopo il profiler |
| Round 12 (attese di risorsa, FP compare inline, profiler dei fotogrammi lenti) | 59 contro 58 FPS all'hub, +5 FPS in esplosioni e impatti | FP compare e profiler sì; attese di risorsa come idea | generatore: con la grafica; profiler: con la GPU vera (06 §20) |
| Round 13 (light field, prefetch, lock per oggetto) | "nettamente più veloce" nelle scene intense di Jungle Joyride | prefetch `dcbt` nel generatore | con la grafica |
| Round 14 (FPCR, memcpy inline, risvegli mirati, sezioni critiche) | non isolato (FPCR: 5,3 % dei campioni del thread principale; risvegli: 2/3 dei campioni del sound server) | sì (generatore e kernel HLE) | kernel: dopo l'audio (06 §19); generatore: con la grafica |
| Round 15 CPU (ADX, multiply-add espliciti, pool, cluster) | non isolato | multiply-add espliciti e `1/sqrt` scalare sì | con la grafica |
| Round 15 GPU (copie al submit, profondità in sola lettura, …) | 39,7 contro 37,7 FPS in un punto limitato dalla GPU | solo idee (renderer diverso); reset delle query uno per uno sì | renderer nativo |
| Round 16: occhi scuri | correzione (fedeltà) | no (loro traduttore) | — |
| PGO col profilo del round 16 | non isolato | sì | quando il gioco gira (06 §13) |
| Boost GPU 460,8 MHz in portatile | non misurato nelle note (GPU da 307,2 a 460,8 MHz) | sì | con la GPU vera |
| Qualsiasi cartella della SD | comodità | sì | quando si vuole (piccolo) |
| Nessun log per default, `config.toml` completo | comodità | sì | per una release |

## 13. masseffect-nx e sa2-nx: compilatore (LTO, PGO, ordine, -O2/-O3) e renderer (letti il 2026-10-09)

Fonti: `masseffect-nx/docs/{optimization-paths,performance-history,measuring,
audio-and-video,known-issues}.md` (nebadasSwifty), `sa2-nx/docs/porting-guide.md`
§9 (ChanseyIsTheBest), §12 qui sopra per Unleashed. Solo documenti; niente è
stato attivato da noi.

### Cosa dichiara ciascuno

| Leva | Mass Effect (masseffect-nx) | Sonic Adventure 2 (sa2-nx) | Sonic Unleashed (unleashed-nx) |
|---|---|---|---|
| LTO | **nessun guadagno** (t122: 226 core-ms per fotogramma contro 218–226 senza; build molto più lunghe, 16 GB) | usata (`-flto=N`, partizione bilanciata, `-ffp-contract=off -fno-strict-aliasing` ripetuti al link; xxHash rotto sotto LTO senza `XXH_FORCE_MEMORY_ACCESS=0`) | usata nel round 9 |
| PGO | **nessun guadagno** (t160 19,6 FPS contro 18,7–18,9, ma la coppia della stessa sessione t161/t162 dà 16,7/16,6) | **circa +15 %** da una sessione sulla console (il giocatore singolo non scende più sotto 60); flusso `-fprofile-info-section` + `__gcov_info_to_gcda`, `-fprofile-correction`, `-fprofile-partial-training`, profilo spedito in `pgo/` | usata (round 9: 44,0 FPS con PGO + LTO + `-O2` + `-fipa-pta`) |
| Ordine delle funzioni calde | **nessun guadagno** (t158: 18,7 contro 18,9), tenuto | usato (`hot_functions.ld`, `KEEP(*(.crt0))` primo) | — |
| `-O2`/`-O3`/`-Os` | `-O3`; `-Os` peggio (t234: 21,8 contro 24,9 FPS) | `-O3` | `-O2` (motivo: `-O3` gonfia il codice, l'A57 ha 48 KB di cache istruzioni; con PGO GCC riaccende `-O3` dove serve); nessun numero isolato |
| Cosa ha pagato davvero | togliere `volatile` dagli accessi guest (t153: **+15–20 %**), argomenti in registro (−2 ms sul thread principale, poi nessun guadagno sopra le altre), risoluzione 960×540 | registri come locali, niente store dell'LR, mosse float senza cambio di modo FPCR (20–26 % dei campioni del thread di gioco dopo un `msr fpcr`), chiamate dirette, niente `volatile` | §12 |

La lezione di Mass Effect (optimization-paths §2): con 7 milioni di righe
generate e le funzioni calde già chiamate direttamente, al compilatore resta
poco da vedere fra i file; i guadagni sono venuti dal cambiare **che cosa fa**
il codice generato, non dalle opzioni del compilatore. Le due osservazioni non
si contraddicono per forza: SA2 è più piccolo e limitato da un thread per
fotogramma, dove il layout del codice caldo pesa di più (ipotesi, non
misurata da nessuno dei due).

Altro di masseffect-nx utile per noi: la regola "misurare solo sulla console,
ai clock di serie, una variabile per run dietro una cvar, mai fidarsi di un
intervallo da 10 s (rumore ≈ 3 FPS), confrontare due build nella stessa
sessione in core-ms per fotogramma" (measuring.md); "non leggere mai
`cntvct_el0`, usare `armGetSystemTick`"; il gioco è limitato dalla CPU (tre
core saturi, ~290 %) e i 30 FPS non sono raggiunti (~25, known-issues §1); il
quarto core e il boost ufficiale della CPU (che porta la GPU a 76,8 MHz) sono
esclusi; audio: il mixer del gioco ricompilato in 5.1 da 256 campioni a
48 kHz, ridotto a stereo su `audout`, XMA con FFmpeg su un thread sopra quelli
del gioco, versioni NEON del mixer esatte ma spente (nessun guadagno).

### Come misurarli su GTA IV (proposta, niente attivato)

Prerequisiti: la GPU vera (o almeno una scena stabile e ripetibile), la riga
`cpu` del battito (run 19) e, per le opzioni del compilatore, spegnere il
tracciamento delle export (`nx_trace_exports = false`) perché il suo costo non
copra le differenze.

1. **Una scena fissa:** stesso salvataggio, nessun input per 60 s (o un
   percorso registrato), clock di serie, 3 intervalli da 10 s per build,
   due build nella stessa sessione. Metrica: core-ms per fotogramma dei
   thread guest più carichi (riga `cpu` × 10 s / fotogrammi), non gli FPS.
2. **Prima ciò che ha pagato altrove** (codice generato): accessi guest senza
   `volatile` (+15–20 % su ME), registri come locali, mosse float senza FPCR.
3. **`-O2` contro `-O3` solo sul codice generato** (cvar di build come
   `SWITCH_RECOMP_O2` di Unleashed): misura diretta del peso della cache
   istruzioni.
4. **LTO** sul codice generato più l'app (attenzione a xxHash e a
   `-ffp-contract=off` al link).
5. **PGO** col metodo di sa2-nx (contatori in sezione, salvati ogni 2 minuti e
   all'uscita, `-fprofile-correction`); nessuna modifica di codice fra la
   build strumentata e quella d'uso.
6. **Ordine delle funzioni calde** solo dopo PGO (con PGO GCC separa già
   caldo e freddo).

Regole d'arresto di Mass Effect: sotto 1,0× si torna indietro, fra 1,0 e
1,2× si tiene solo se semplifica, sopra 1,2× si spedisce.

### Renderer: masseffect-nx (`app/src/native`) e nfsmw-nx (`app/src`)

Lettura dei nomi dei file e dei documenti, non del codice. Tutti e due
sostituiscono il processore di comandi `xenos` di ReXGlue con un renderer
nativo scritto per il gioco, che legge il PM4 (e poi, in sa2-nx, direttamente
le chiamate D3D) e registra Vulkan su NVK.

- **Generale** (si ritrova in tutti e due e non dipende dal gioco): decodifica
  dei pacchetti PM4 e delle sequenze di registri (`me_pm4_runs.h`), tabelle dei
  record di draw e degli oggetti (`me_record_table.h`, `me_object_table.h`),
  identità e libreria degli shader (`me_shader_identity.h`,
  `nfsmw_shader_library.cpp`, `nfsmw_nativo_shaders.cpp`), pool di texture e
  destinazioni di render (`nfsmw_nativo_texturas_pool.cpp`,
  `nfsmw_nativo_destinos.cpp`), EDRAM a riquadri con conversioni in compute o
  fragment, attesa dell'anello (`me_ring_wait.cpp`, `nfsmw_espera_anillo.cpp`),
  ambiente di Mesa (`nfsmw_entorno_mesa.cpp`), PGO (`me_pgo.cpp`,
  `nfsmw_pgo.cpp`), watchdog.
- **Specifico del gioco:** gli hook sul D3D del titolo
  (`me_d3d_trace_all.inc`, `nfsmw_d3d_trace.cpp`), stime e liste per shader
  e draw (`me_native_draw_extent_estimator.h`, `me_native_ps_no_kill.h`),
  scenario, viste e materiali di NFSMW (`nfsmw_escenario_*`,
  `nfsmw_eview_nativo.cpp`, `nfsmw_material_nativo.cpp`), tagli di gara e
  ombre (`nfsmw_recortes_carrera.cpp`, `nfsmw_recorte_sombras.cpp`), PhysX,
  folle e minigiochi di ME.
- Per Theft4: il renderer nativo di GTA IV esiste già (`gta4_native`, 67 mila
  righe nel nostro SDK, 12 §1); quello che si può prendere da loro è la parte
  generale (la decodifica PM4 veloce, la libreria degli shader, l'ambiente
  di Mesa), non i renderer.
