# Switch port — 04: thread e uscita

Aggiornato: 2026-09-26 · Ramo `switch-port`

## 1. Affinità: tutti i thread sul core 0

**Sintomo** (`switch-smoke` step 4, run del commit `203cbc6`): i thread assegnati ai core 0, 1 e 2 hanno finito tutti sul core 0. Il thread 3, lasciato con l'affinità di default, ha girato sul core 1.

**Causa.** `PosixCondition<Thread>::set_affinity_mask` in `threading_switch.cpp` chiamava:

```cpp
svcSetThreadCoreMask(nx_handle_, -2, mask & 0x7);
```

- Per Horizon il core ideale `-2` è *IdealCoreUseProcessValue*: il kernel **ignora la maschera** e usa il core di default del processo (0) con maschera `1 << 0`. Il commento nel codice diceva "-2 = auto-select from affinity set", ed era sbagliato.
- Il valore restituito non veniva controllato.
- Il thread 3 finiva altrove per un motivo diverso: `pthread_create` di libnx (`__syscall_thread_create`, verificato nel disassemblato) crea il thread con `threadCreate(..., 0x3B, -2)` e poi chiama `svcSetThreadCoreMask(h, -1, core mask del processo)`. Quel thread può quindi migrare su 0–2.

**Correzione:**

- il core ideale diventa il bit più basso della maschera richiesta;
- la maschera viene intersecata con la core mask del processo (`svcGetInfo(InfoType_CoreMask)`), invece del fisso `& 0x7`;
- il `Result` viene controllato e, se fallisce, finisce nel log;
- una maschera senza core validi viene rifiutata con un warning.

## 2. Numero di processori logici = 0

**Causa.** `rex::thread::logical_processor_count()` (`threading.cpp`) usa `std::thread::hardware_concurrency()`. Nella libstdc++ di devkitA64 quella funzione è `mov w0, #0; ret` (verificato nel disassemblato di `libstdc++.a`).

**Correzione (solo NX):** popcount della core mask del processo. Vale 3 per un'applicazione, 4 se il core 3 è concesso.

Chi usa il valore nel runtime:

| Punto | Con 0 (prima) | Con 3 (ora) |
|---|---|---|
| `XThread::SetActiveCpu` (`xthread.cpp:915`) | `< 6`: warning "Too few processor cores", l'affinità non viene mai applicata | su NX non conta più: mappa CPU → core di §3 |
| `vulkan/pipeline_cache.cpp` (thread di creazione delle pipeline) | ricade su 6 → `6 * 3 / 4` = 4 thread | `max(3 * 3 / 4, 1)` = 2 thread |
| `d3d12/pipeline_cache.cpp` | ricade su 6 | non compilato su Switch |
| `threading_mac.cpp` | — | non compilato su Switch |

Fuori dall'SDK, `LibertyRecomp/install/thread_pool.cpp` (ricade su 4) e `LibertyRecomp/gpu/video.cpp` (`max(2, 0)`) chiamano ancora `hardware_concurrency()` direttamente. Non fanno parte del runtime Switch e non sono stati modificati.

## 3. `XThread::SetActiveCpu` su Switch (implementato)

La Xbox 360 ha 3 core con 2 thread hardware ciascuno: 6 CPU logiche, 0–5. I giochi scelgono la CPU con `XSetThreadProcessor` o con i flag di creazione. `GetFakeCpuNumber` assegna un indice a rotazione se il gioco non lo specifica.

Prima di questa modifica, su NX:

- con meno di 6 processori logici `SetActiveCpu` non applicava niente e stampava un warning;
- anche con 6 o più, `ignore_thread_affinities` valeva `true` per default, quindi l'affinità del gioco non veniva mai applicata.

### Strategia approvata

1. **Mappa** dalle 6 CPU logiche ai core. Ogni coppia di thread hardware della 360 condivide un core e la sua L1, quindi la coppia va sullo stesso core host.

   | CPU guest | core |
   |---|---|
   | 0, 1 | 0 |
   | 2, 3 | 1 |
   | 4, 5 | 2 |

   - **Il core 3 non va mai ai thread guest**, anche quando il processo ce l'ha (mask 0xF). È riservato ai worker host: compilazione di shader e pipeline, decodifica audio, streaming.
   - Se il core della mappa non è nella core mask del processo, si ripiega sul core guest più basso.
2. **Affinità morbida per default.** Il core ideale è quello della mappa, la maschera comprende tutti i core guest (`core mask & 0x7`). Il kernel preferisce il core ideale ma può spostare il thread su un altro core guest.
3. **Affinità rigida con la cvar `nx_hard_thread_affinity`** (default `false`): la maschera è il solo core della mappa.
4. **`ignore_thread_affinities` vale `false` di default su NX.** Sulle altre piattaforme resta `true`. Su NX non c'è più la soglia "< 6 processori".

### Codice

- **`rex/thread.h`** (solo NX):
  - `Thread::set_ideal_core(core, mask)` e `Thread::ideal_core()`: le due parti dell'affinità Horizon;
  - `nx_guest_core_mask()`: core mask del processo `& 0x7`;
  - `nx_host_worker_core_mask()`: core mask del processo `& 0x8`, cioè 0 senza core 3;
  - `nx_guest_cpu_core(cpu)`: la mappa descritta sopra.
- **`threading_switch.cpp`.** `set_ideal_core` controlla che il core ideale sia nella maschera ∩ core mask del processo e chiama `svcSetThreadCoreMask(handle, core, mask)`. `set_affinity_mask` ora passa di lì, con il core più basso della maschera come ideale: comportamento invariato.
- **`xthread.cpp` `SetActiveCpu`, ramo NX.** Se `ignore_thread_affinities` è falsa: `set_ideal_core(nx_guest_cpu_core(cpu), nx_hard_thread_affinity ? 1 << core : nx_guest_core_mask())`. Il ramo delle altre piattaforme non cambia.

### Worker host sul core 3: prossimo passo

L'API c'è (`nx_host_worker_core_mask()`), ma nessun worker host viene ancora spostato. I candidati:

- i thread di creazione delle pipeline di `vulkan/pipeline_cache.cpp`: il loro numero dipende da `logical_processor_count()`, che vale 3 o 4;
- il decoder XMA;
- lo streaming dei file.

Vanno spostati sul core 3 quando quei percorsi gireranno su Switch. Senza core 3 (mask 0x7) restano con l'affinità di default del processo.

Resta da decidere dopo i primi log in gioco (`GetFakeCpuNumber` / `XSetThreadProcessor`): il posto del command processor GPU e del thread audio rispetto ai thread guest più pesanti.

## 4. Verifica in `switch-smoke`

Lo step 4 ora controlla:

- che `logical_processor_count()` sia uguale al popcount della core mask;
- che, dopo `set_affinity_mask(1 << i)`, `affinity_mask()` restituisca `1 << i`;
- che ogni thread fissato su un core ci abbia davvero girato (`svcGetCurrentProcessorNumber` a fine lavoro).

Poi la mappa di `SetActiveCpu` (`StepGuestCpuMapping`):

- `nx_guest_core_mask()` = core mask `& 0x7` e `nx_host_worker_core_mask()` = core mask `& 0x8`;
- `ignore_thread_affinities` è `false` per default;
- un thread per CPU guest 0–5 con affinità morbida e uno per core con affinità rigida. Per ciascuno: core atteso, `ideal_core()` e `affinity_mask()` dopo `set_ideal_core`, e il core su cui ha girato davvero. Quelli morbidi devono restare sui core guest, quelli rigidi sul loro core.

Ogni differenza fa fallire lo step (`SMOKE FAIL at threads`).

## 5. Crash all'uscita (Instruction Abort dopo il tasto +)

### Crash report (run del commit `203cbc6`)

PC `0xdee56a0`, LR `0xdee3a9c`, SP `0x58910e20`, ritorni `0xdeafee4 0xdeb021c 0xdf2639c 0xdee33b4 0xdedef78`. È elencato un solo modulo, `0x61600000–0x61606000`: è hbl, perché l'NRO era già stato scaricato.

L'ELF di `203cbc6` è stato ricostruito dagli stessi sorgenti (`out/build/switch-smoke/switch-smoke-203cbc6.elf`). Con `crash_match.py` risulta **una sola base** coerente con tutti e 7 gli indirizzi: `0xde8d000`. Per ogni base candidata lo script controlla che:

- sia allineata a pagina;
- ogni indirizzo di ritorno cada subito dopo un `bl`/`blr`;
- il PC cada in `.text`.

Risultato di `aarch64-none-elf-addr2line`:

| Indirizzo | Offset ELF | Funzione |
|---|---|---|
| PC `0xdee56a0` | `+0x586a0` | `svcSleepThread` (libnx `svc.s:75`), subito dopo la `svc` |
| LR `0xdee3a9c` | `+0x56a9c` | `__syscall_nanosleep` (libnx `newlib.c:417`) |
| `0xdeafee4` | `+0x22ee4` | `std::this_thread::sleep_for` ← `disruptorplus::spin_wait::spin_once` ← `TimerQueue::TimerThreadMain` (`timer_queue.cpp:70`) |
| `0xdeb021c` | `+0x2321c` | lambda del thread di `TimerQueue::TimerQueue()` (`timer_queue.cpp:43`) |
| `0xdf2639c` | `+0x9939c` | `execute_native_thread_routine` (libstdc++) |
| `0xdee33b4` | `+0x563b4` | `__thread_entry` (libnx `newlib.c:162`) |
| `0xdedef78` | `+0x51f78` | `_EntryWrap` (libnx `thread.c:58`) |

### Il thread vivo e la causa

Il thread vivo è il dispatcher di `rex::thread::TimerQueue`: un `std::jthread` avviato dal costruttore dell'oggetto statico `timer_queue_` (`timer_queue.cpp`). Esiste quindi in ogni processo che linka l'SDK. Quando non ha timer attivi aspetta con uno spin-wait che dorme 1 ms alla volta.

Il distruttore di `timer_queue_` lo fermerebbe e farebbe il join, ma **su libnx non viene mai eseguito**. Verificato nel binario:

- `__libnx_init` esegue `__libc_init_array`;
- `__libnx_exit` chiama solo `__appExit` e `__nx_exit`, che torna al loader;
- niente percorre `.fini_array`, dove GCC mette `_GLOBAL__sub_D_timer_queue.cpp`.

Alla pressione di + lo smoke esce da `main`: `exit` → `__call_exitprocs` → `_exit` → `__libnx_exit` → ritorno a hbl, che scarica l'NRO. Al risveglio successivo da `svcSleepThread` il thread ritorna in codice smontato: Instruction Abort.

### Correzione (solo NX, `timer_queue.cpp`)

- Nuovo metodo `TimerQueue::Shutdown()`: fa quello che fa il distruttore (stop, kick, join) ed è idempotente.
- È registrato con `std::atexit` subito dopo la costruzione di `timer_queue_`. Gli handler `atexit` girano su NX (`exit` → `__call_exitprocs`), prima che il modulo venga scaricato.
- Il distruttore e le altre piattaforme non cambiano.

Anche gli altri oggetti statici con distruttore non vengono mai distrutti su NX. Finché non tengono thread o risorse del kernel questo non causa crash, ma va tenuto presente per il runtime completo.

### Base del modulo nello smoke

Nel run 4 lo smoke stampava `module base (__start__) 0x0`. Il motivo: `__start__` è un simbolo **assoluto** di `switch.ld` (`PROVIDE_HIDDEN(__start__ = 0x0)`, tipo `a` in `nm`), e un link PIE non lo riloca.

Ora lo smoke usa `_start`, l'etichetta d'ingresso del crt0 di libnx:

- sta in `.text` all'indirizzo ELF 0 (tipo `T`);
- il suo slot nella GOT ha una rilocazione relativa (`.relr.dyn`), quindi a runtime vale base + 0.

Lo smoke verifica anche che:

- il blocco di codice R-X restituito da `svcQueryMemory` inizi esattamente alla base;
- la base sia allineata a pagina;
- a +0x10 ci sia la magic `NRO0`, che elf2nro scrive nell'header del file caricato.

Se uno di questi controlli fallisce: `SMOKE FAIL at module base`.

Verifica sui numeri del run 4: `main` a runtime `0x39f379420` meno `main` nell'ELF (`0x2420`) dà `0x39f377000`, cioè l'inizio del blocco R-X di `main`. Con `crash_match.py` (PC = `main`, ritorno dopo un `bl` in `main`) `0x39f377000` è fra le basi coerenti. Da ora la base stampata dallo smoke permette di scegliere direttamente quella giusta.

## 6. Run 5: User Break dopo il primo fault gestito

### Crash report

- User Break, Break Reason 0x0, modulo `switch-smoke` a base `0x13f0a5000`;
- PC +0xd793c, LR +0xdd90c, ritorni +0x1e7e0, +0x35dc, +0x3040.

ELF: la build del commit `51798c4f` (i commit successivi toccano solo documenti e README). Con `crash_match.py`, PC, LR e i primi due ritorni danno **una sola base, `0x13f0a5000`**. L'ultimo ritorno, +0x3040, cade dopo un `udf` e non dopo un `bl`: è la fine del percorso sui frame pointer, non un chiamante reale.

| Offset | Funzione (`addr2line -f -C -i`) |
|---|---|
| PC +0xd793c | `svcBreak` (libnx `svc.s:234`) |
| LR +0xdd90c | `__libnx_exception_returnentry` (libnx `exception.s:188`) |
| +0x1e7e0 | `RunProbes`: il `bl` a +0x1e7dc chiama `ProbeFaultRoundTrip` (T9) |
| +0x35dc | `main` → `RunProbes` |

### Causa

T9 scrive sulla sua pagina resa di sola lettura. Il fault passa da `MMIOHandler` (nessun range) a `T9Handler`, che rimette RW e restituisce `true`. Il nostro `__libnx_exception_handler` ritorna, e lì si ferma tutto: nel disassemblato di libnx 4.12.0, `__libnx_exception_returnentry` è

```
bl  __libnx_exception_handler
mov w0, wzr ; mov x1, #0 ; mov x2, #0
bl  svcBreak          // sempre
```

Il ritorno dal gestore non riprende niente. `__libnx_exception_entry` ha già riscritto pc/sp del frame del kernel verso `returnentry` e chiamato `svcReturnFromException` prima di eseguire il gestore. Il commento in `exception_handler_switch.cpp` ("returning ... causes libnx to svcReturnFromException and resume at ctx->pc") era sbagliato.

Di conseguenza, su Switch nessun fault era mai stato ripreso: né MMIO né write-watch né T9/T10. Inoltre il gestore girava fuori dalla zona esclusiva del kernel, su stack e dump condivisi (vedi `03-memory.md` §10).

### Correzione (`exception_handler_switch.cpp`, solo NX)

`__libnx_exception_entry` è weak in libnx (`nm libnx.a`: `W`). Il file ne definisce uno proprio, in assembly. Il kernel entra con x0 = tipo e x1 = `ThreadExceptionFrameA64` nella process-local region (x0–x8, lr, sp, pc, pstate, esr, far). x9–x29 e i registri SIMD sono ancora quelli del thread.

1. Passa a `__nx_exception_stack`. Condividerlo è sicuro, perché la zona esclusiva del kernel è ancora attiva.
2. Salva x9–x29, q0–q31, fpsr e fpcr.
3. Chiama `rex_nx_exception_dispatch`, che costruisce `HostThreadContext` da frame e registri salvati, esegue la lista dei gestori e riscrive i registri modificati (x0–x8, lr, sp, pc nel frame; il resto nell'area salvata).
4. Ripristina i registri e chiama `svc 0x28` (`svcReturnFromException`):
   - `0`: il kernel ricarica il frame e riprende al pc, eventualmente fatto avanzare dal gestore;
   - `0xF801` (non gestito): il kernel tratta l'eccezione come non gestita con il contesto **originale**. Il crash report mostra quindi il PC vero invece di `svcBreak`.

Verificato nel link: crt0 salta al nuovo `__libnx_exception_entry` e `libnx.a(exception.o)` non viene più incluso.

### Log del run 5

Il log conteneva solo 3 righe, tutte dell'SDK. Dal run 5 lo smoke configura la policy di diagnostica, quindi il file sink dell'SDK tiene `smoke.log` aperto in scrittura. Il FS di Horizon rifiuta di aprire in scrittura un file già aperto in scrittura da un altro handle, quindi ogni `open()` di `DurableAppend` falliva in silenzio.

Ora l'SDK non ha un file sink proprio: un sink (`DurableSink`) scrive ogni riga con lo stesso percorso open/write/fsync/close delle righe dello smoke, sotto un mutex, nello stesso ordine. Un'apertura fallita viene segnalata a schermo.

## 7. Priorità dei thread (schema di nfsmw-nx)

### Il problema

Su Horizon **solo la priorità 0x3B ha il time-slicing** (fette di circa 10 ms).
Ogni altra banda è cooperativa: un thread che non si blocca tiene il suo core
contro i thread di pari o minore priorità, anche se altri core sono liberi
(misure di nfsmw-nx, `docs/platform-notes.md`; vedi 08). Il codice ricompilato
fa spin (attese attive del D3D del gioco, `Sleep(0)`), quindi i thread guest
devono stare nell'unica banda con time-slicing, e i thread host sopra di loro
per non restare affamati.

Prima di questa modifica `MapToSwitchPriority` metteva 0x2C a ogni thread
senza priorità esplicita e saliva verso 0x20 con quella generica dell'SDK: i
thread guest finivano a 0x2C o sopra, cioè proprio nella situazione descritta.

### Bande

| Priorità | Thread | Perché |
|---|---|---|
| 0x2B | `Audio Worker`, `XMA Decoder` | la scadenza più dura: il server audio del gioco ha un ring di due pacchetti da 256 campioni (10,7 ms); se il worker arriva tardi il mix esce con la voce muta ("audio robotico" in nfsmw-nx). Deve poter interrompere tutto il resto del runtime |
| 0x2C | `GPU VSync`, `Kernel Host Tasks`, ogni altro thread host (anche quelli creati con `rex::thread::Thread` senza priorità, per esempio il TimerQueue) | lavoro breve su cui i thread guest aspettano: l'interrupt di vblank a 60 Hz (se ritarda si sposta il pacing), APC/DPC e compiti del kernel |
| 0x2D | `GPU Commands` | lavoro CPU pesante per frame: sotto vblank, audio e compiti del kernel per non affamarli; sopra i guest che lo aspettano |
| 0x3B | tutti i thread guest; `Vulkan Pipelines`, `D3D12 Pipelines`, `Shader Translation`, `* Storage writer` | i guest per il time-slicing; la creazione delle pipeline e la scrittura delle cache sono lavoro di massa: nella banda cooperativa terrebbero un core intero contro i guest |

I nomi si confrontano per **prefisso**: `XThread::set_name` aggiunge
` (F80000xx)` con l'handle, e con un confronto esatto (bug trovato da nfsmw-nx)
audio e XMA restavano nella banda 0x2C. I thread delle pipeline non esistono
ancora su Switch (renderer non portato): la regola è già pronta.

### Codice

- **`rex/thread.h`** (solo NX): `kNxPriorityAudio` 0x2B, `kNxPriorityHost` 0x2C,
  `kNxPriorityGpu` 0x2D, `kNxPriorityGuest` 0x3B;
  `nx_priority_for_thread(guest, name)`; `Thread::set_nx_priority(p)`, che
  imposta la priorità Horizon così com'è, senza la mappatura generica.
- **`threading_switch.cpp`.** `ThreadStartRoutine` imposta **sempre** una
  priorità esplicita: quella richiesta con `initial_priority` (mappata come
  prima) oppure 0x2C, invece del default di pthread di libnx.
- **`xthread.cpp`.** `XThread::Create`, ramo NX:
  `set_nx_priority(nx_priority_for_thread(is_guest_thread(), thread_name_))`,
  dopo che il nome è stato assegnato. `XThread::SetPriority` (le chiamate
  `KeSetBasePriorityThread` del gioco) su NX aggiorna solo il `KTHREAD`: ogni
  XThread resta nella banda del suo ruolo, i guest a 0x3B qualunque cosa
  chiedano. Le altre
  piattaforme non cambiano.

### Verifica in `switch-smoke` (step 4)

- la tabella delle bande su nomi con suffisso d'handle;
- un `rex::thread::Thread` senza priorità parte a 0x2C (`svcGetThreadPriority`
  dal thread stesso);
- `set_nx_priority(0x3B)` letto con `priority()` e dal thread;
- **la premessa, misurata:** due thread che fanno spin per 200 ms sullo stesso
  core. A 0x3B il secondo deve partire entro 50 ms dal primo (time-slicing);
  a 0x2C deve partire dopo almeno 150 ms, cioè quando il primo ha finito.
