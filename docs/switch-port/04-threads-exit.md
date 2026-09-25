# Switch port — 04: thread e uscita

Aggiornato: 2026-09-25 · Ramo `switch-port`

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
| `XThread::SetActiveCpu` (`xthread.cpp:915`) | `< 6`: warning "Too few processor cores", l'affinità non viene mai applicata | uguale (`3 < 6`): vedi §3 |
| `vulkan/pipeline_cache.cpp` (thread di creazione delle pipeline) | ricade su 6 → `6 * 3 / 4` = 4 thread | `max(3 * 3 / 4, 1)` = 2 thread |
| `d3d12/pipeline_cache.cpp` | ricade su 6 | non compilato su Switch |
| `threading_mac.cpp` | — | non compilato su Switch |

Fuori dall'SDK, `LibertyRecomp/install/thread_pool.cpp` (ricade su 4) e `LibertyRecomp/gpu/video.cpp` (`max(2, 0)`) chiamano ancora `hardware_concurrency()` direttamente. Non fanno parte del runtime Switch e non sono stati modificati.

## 3. `XThread::SetActiveCpu` su Switch: proposta (NON implementata)

La Xbox 360 ha 3 core con 2 thread hardware ciascuno: 6 CPU logiche, 0–5. I giochi scelgono la CPU con `XSetThreadProcessor` o con i flag di creazione. `GetFakeCpuNumber` assegna un indice a rotazione se il gioco non lo specifica. Oggi:

- con meno di 6 processori logici `SetActiveCpu` non applica niente e stampa un warning;
- anche con 6 o più, la cvar `ignore_thread_affinities` vale `true` per default su tutte le piattaforme, quindi l'affinità del gioco non viene mai applicata.

### Proposta

1. **Mappa** dalle 6 CPU logiche ai core disponibili. Ogni coppia di thread hardware della 360 condivide un core e la sua cache L1, quindi la coppia va sullo stesso core host.

   | CPU guest | 3 core (mask 0x7) | 4 core (mask 0xF) |
   |---|---|---|
   | 0, 1 | core 0 | core 0 |
   | 2, 3 | core 1 | core 1 |
   | 4 | core 2 | core 2 |
   | 5 | core 2 | core 3 |

2. **Affinità morbida per default.** Core ideale = core della mappa, maschera = tutti i core del processo. Il kernel preferisce il core ideale ma può spostare il thread se quel core è occupato. Serve una piccola API NX, per esempio `Thread::set_ideal_core(core, mask)`, perché `set_affinity_mask` oggi accetta solo la maschera.
   - Una cvar a parte, ad esempio `nx_hard_thread_affinity`, fissa il thread al solo core della mappa, per gli esperimenti.
   - Il comportamento di migrazione dello scheduler Horizon con maschere a più core è da verificare sulla console con un test nello smoke [HW].
3. **Niente soglia "< 6" su NX:** la mappa si applica sempre, salvo quando `ignore_thread_affinities` è attiva. Il default della cvar su NX va deciso insieme alla proposta: con `true` la mappa non si userebbe mai.
4. **Thread host** (command processor GPU, audio, `TimerQueue`): vanno tenuti lontani dal core dei thread guest più pesanti. Va deciso dopo aver visto su quali CPU guest il gioco mette i suoi thread (log di `GetFakeCpuNumber` / `XSetThreadProcessor` in gioco).

Da approvare prima dell'implementazione.

## 4. Verifica in `switch-smoke`

Lo step 4 ora controlla:

- che `logical_processor_count()` sia uguale al popcount della core mask;
- che, dopo `set_affinity_mask(1 << i)`, `affinity_mask()` restituisca `1 << i`;
- che ogni thread fissato su un core ci abbia davvero girato (`svcGetCurrentProcessorNumber` a fine lavoro).

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
