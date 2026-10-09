# 09 – Regole per il codice NX

Regole ricavate dai guasti visti qui e in nfsmw-nx / UnleashedRecomp-NX
(`platform-notes.md`, `porting-another-game.md`, 10-unleashed-nx.md). Ogni
regola ha il motivo, e sotto le eccezioni che il codice NX aveva al
2026-09-30 (controllo fatto su `src/core/*switch*`, `src/audio/switch`,
`threading_switch.cpp`, `export_stubs.cpp`, `tools/switch-game`,
`tools/switch-smoke`, e sui punti del codice condiviso che su Switch girano).

## 1. Ogni attesa ha un timeout e scrive nel log quanto sta aspettando

Condvar, eventi, semafori, spin, `svcWaitSynchronization`, `eventWait`: con un
limite, e una riga di log quando l'attesa supera una soglia. Un risveglio perso
deve diventare una riga nel log, non un blocco silenzioso (nfsmw-nx: un thread
di copia dei vertici bloccato da un risveglio perso su milioni; UnleashedRecomp:
il thread audio di SDL fermo per sempre).

**Eccezioni, e perché:**

| Dove | Attesa | Stato |
|---|---|---|
| `threading_switch.cpp` `PosixCondition::Wait` con timeout infinito (`cond_.wait`) | le attese **INFINITE del gioco** (`KeWaitForSingleObject` & co.) | lasciata: è la semantica che il gioco chiede. Resta visibile: dal run 12 l'istantanea dei thread di switch-game dice "IN KeWaitForSingleObject for 40 s" |
| `threading_switch.cpp` `WaitStarted` (`state_signal_.wait`) | l'avvio di un thread appena creato | senza limite; il thread parte in microsecondi. Da limitare se mai compare in un'istantanea |
| `threading_switch.cpp` `ThreadStartRoutine`, thread creato sospeso (`state_signal_.wait` fino al Resume) | un thread guest creato sospeso | senza limite per definizione (aspetta il Resume del gioco) |
| `threading_switch.cpp` `WaitSuspended` (`sem_wait`) | un thread che si è sospeso da solo | idem |
| `switch-game` `StopAtMissingFunction` (`svcSleepThread(INT64_MAX)`) | il thread che ha raggiunto il punto di arresto | voluto: il thread resta fermo fino al + |
| il lock globale del kernel (`global_critical_region`, un `std::recursive_mutex`), preso da IRQL alzato, spin lock, tabella degli oggetti, sistema audio | semantica del kernel della 360 | lasciato per il codice del kernel. **Gli strumenti di diagnosi non lo aspettano più**: `ObjectTable::TryGetObjectsByType` (200 ms) nell'istantanea di switch-game e nel battito del PC, che sul PC si era bloccato proprio lì. E niente codice host esterno sotto quel lock: `AudioSystem::RegisterClient` creava il driver (SDL) tenendolo, e sul PC il gioco è rimasto lì dentro (06 §14) |

**Corretto in questo giro:** `switch_audio_driver.cpp`
`WaitForCurrentSlotReusable` aspettava in `audrenWaitFrame()` senza limite (vedi
§4 di 10-unleashed-nx.md): ora sonda ogni ms, scrive un warning dopo 100 ms e
dopo 500 ms scarta il frame. `Thread::Terminate` aveva già un limite (5 s).

## 2. Hook e codice host toccano la memoria guest solo con `GuestToHost` / `TranslateVirtual`

Mai `base + addr`: nel modello A le finestre 0x7F/0xA0/0xC0/0xE0 e la memoria
fisica sono raggiunte attraverso `rex_guest_table`, e sopra 16 MiB l'aritmetica
host non è contigua fra finestre (03 §8). Sul PC, inoltre, ≥ 0xE0000000 c'è
lo scarto di 0x1000.

**Eccezioni trovate** (nessuna su un percorso attivo in switch-game oggi):

| Dove | Codice | Perché regge oggi | Da fare |
|---|---|---|---|
| `include/rex/ppc/stack.h` `stack_limit_from_pcr` | `base + ctx.r13.u32 + 0x74` | il KPCR viene da `SystemHeapAlloc` nell'heap virtuale da 4 KiB (< 0xA0000000, contiguo); `stack_bounds_check` non ha chiamanti | passare a `GuestToHost` se torna in uso |
| `src/kernel/crt/heap.cpp` `ReXHeap::GuestToHost` | `membase_ + guest_addr` | usato solo con `[rexcrt]` (sostituzione dell'heap del CRT), assente in `config-eu-base` | passare a `memory->TranslateVirtual` prima di usare `[rexcrt]` su Switch |
| `src/system/ppc_generated.cpp` `GuestString` | `base + guest_address` | ramo solo desktop; su NX usa già `GuestToHost` | — |

## 3. Niente `std::thread::detach()` (né `pthread_detach`) su NX

Con libnx 4.12 `detach()` può lanciare `std::system_error` se il thread è già
finito, e si finisce in `std::terminate` (nfsmw-nx). Su NX: thread persistenti
con una coda di lavori e `join` all'uscita.

**Controllo:** nessun `.detach()` nel runtime, nell'SDK (`src/`, `include/`),
in `tools/switch-game`, `tools/switch-smoke`, né nelle librerie di terze parti
linkate su Switch (spdlog, fmt, toml++, xxHash, snappy, o1heap: i `detach()` di
toml++ sono metodi del suo formatter, non thread). Nessun `std::async`.

**Corretto in questo giro:** `threading_switch.cpp` `Thread::Terminate` (thread
diverso dal chiamante) faceva `pthread_detach` e poi, in `post_execution`,
`pthread_join` sullo stesso thread: una join dopo una detach è indefinita, e su
libnx il record del thread può essere già liberato. Ora: niente detach; se il
thread è uscito entro 5 s viene joinato (una volta sola, flag `joined_`: anche
due attese successive sullo stesso thread facevano due join), altrimenti è
abbandonato (`abandoned_`) e mai joinato.

**Thread `std::thread`/`std::jthread` compilati su Switch:** `TimerQueue`
(`std::jthread`, fermato da `Shutdown()` registrato con `atexit`, 04 §5),
`xam_voice.cpp` (due `std::jthread`), `live_compatibility.cpp` e
`arbitration_async.h` (`std::thread`, joinati nei distruttori). Nessuno è
staccato. Attenzione diversa ma collegata: su libnx i distruttori statici non
girano (04 §5), e uno `std::thread` joinable distrutto chiama
`std::terminate`.

## 4. Niente atomiche per contatori scritti da un solo thread

L'A57 non ha LSE: `fetch_add` è un ciclo `ldxr`/`stxr`, con barriere se non è
relaxed. Un contatore con un solo scrittore (verificato: tutti gli scrittori)
si scrive con load + store; se un altro thread lo legge, `std::atomic` con
`load`/`store` relaxed (stesso codice di un `ldr`/`str`), mai
read-modify-write. UnleashedRecomp-NX ha misurato questi cicli sul thread del
gioco (round 9, "Statistics without atomic read-modify-writes").

**Percorsi caldi, stato:**

| Percorso | Contatori | Scrittori | Stato |
|---|---|---|---|
| Fault (`exception_handler_switch.cpp`) | `rex_nx_exception_depth` | il thread nel gestore (il kernel serializza, T10: mai 2 insieme) | intero semplice, `ldr`/`str` in asm: ok |
| Commit memoria (`memory_switch.cpp`) | `GuestArenaStats` (blocchi, pagine, map/unmap, tick) | sotto `g_arena.mutex` | interi semplici: ok. `g_trace_sink` è un puntatore atomico letto per chiamata (load, non RMW) |
| Stub (`export_stubs.cpp`) | `ExportStubSite::hits` | **ogni** thread che chiama lo stub | multi-scrittore: `fetch_add` relaxed giustificato |
| Export implementate (nuovo, run 12) | `ExportCallSite::calls` | ogni thread | multi-scrittore: `fetch_add` relaxed |
| Export implementate, per thread (nuovo) | `ExportThreadTrace::calls`, `current`, `last`, `enter_ticks` | il thread stesso | load + store relaxed (`BumpOwn`), niente RMW: la regola applicata |
| Audio (`switch_audio_driver.cpp`) | `slot_wait_timeouts_` | sotto `audren_mutex_` | intero semplice |

Quindi **sui percorsi caldi non c'erano contatori atomici a thread singolo**.
Le uniche RMW a thread singolo sono nel codice di prova di switch-smoke (T10:
`bad_reads[i]` e `slots[i].calls`, ciascuno di un solo thread): codice di
misura, lasciato.

## 5. `static_assert(std::has_unique_object_representations_v<T>)` su chiavi hash e confronti byte per byte

Una struttura con padding confrontata con `memcmp` o hashata sui byte dà
risultati diversi per valori uguali (nfsmw-nx: chiave delle pipeline "senza
padding"; UnleashedRecomp: xxHash con strict aliasing).

**Stato:**

- Il template generico `rex::XXHasher<Key>` (`include/rex/hash.h`) hasha i byte
  di `Key`. Oggi lo usano solo `TextureKey` (`pipeline/texture/cache.h`) e
  `FramebufferKey` (`vulkan/render_target_cache.h`), entrambi nella grafica
  desktop, non compilata su Switch. **Il posto giusto per l'assert è dentro
  `XXHasher`**, così ogni chiave futura (il renderer Switch) è coperta. Non
  aggiunto ora: compilerebbe per la prima volta quelle due chiavi con l'assert
  nella build PC di C, e se hanno padding va corretta la chiave, non spento
  l'assert. Da fare insieme al primo codice grafico su Switch.
- `ThunkKey` (`kernel_module.h`) e la chiave di `live_compatibility.cpp`
  hanno hash per campi e `operator==` per campi: non servono.
- I `memcmp` del runtime (`xex_module.cpp`, `stfs_container_device.cpp`,
  `disc_image_device.cpp`) confrontano digest e magic, array di byte: non
  servono.

## 6. Nei cicli caldi, copiare in locali i campi letti da strutture passate per riferimento

GCC non può tenere in un registro un campo di `const T&` letto in un ciclo se
nel ciclo c'è una scrittura che potrebbe fare alias (e con
`-fno-strict-aliasing`, che l'SDK usa, quasi ogni scrittura lo è): lo rilegge
dalla memoria a ogni giro. Si copia prima del ciclo.

Controllato su `memory_switch.cpp` (`MapBlock`/commit: i cicli usano già
indici locali), sul gestore dei fault (asm e dispatch senza cicli caldi),
`export_stubs.cpp` (nessun ciclo sul percorso della chiamata),
`ConvertAndSubmit` dell'audio (legge da un buffer locale). Nessuna eccezione
trovata; la regola conta soprattutto per il renderer e gli hook futuri.

## 7. Build: mai due build insieme

Prima di ogni build: `ps -ef | grep -E "ninja|cc1plus"` (escludendo la riga del
comando stesso) non deve mostrare niente. Mai due build insieme, nemmeno in
cartelle diverse: smoke e gioco scrivono le stesse librerie dell'SDK
(`glue/rexglue-sdk-main/out/switch-aarch64/*.a`). **Al massimo `-j3` sui file
generati** (circa 2 MB di C++ l'uno, 1–2 GB di RAM per processo GCC): il
2026-09-29 due Ninja nella stessa cartella, 6 GCC insieme, hanno esaurito i
16 GB. Se un Ninja viene interrotto a metà, `ninja -t recompact` ripara
`.ninja_deps`.

## 8. Una correzione non provata sulla console è "da verificare"

Nei documenti, nei commit e nei riepiloghi una correzione è "corretta" solo
dopo un run sulla console che la mostra funzionare; prima è "da verificare".
Il run 12 ha smentito la correzione dell'uscita del run 11, dichiarata
corretta senza prova.

## 9. Uscita: sotto hbloader si esce con `exit(0)`

**Sotto hbloader non si chiude mai con `svcExitProcess` né con la pausa dei
thread: si esce con `exit(0)`.**

Dati del run 15 (`out/console-runs/run15/`, stesso NRO, quattro modi scelti
all'uscita, nello smoke e nel gioco): il modo A (`exit(0)`, il ritorno da
`main`, con i thread vivi) e il modo D (thread fermati in modo cooperativo,
join per 2 s, poi `exit(0)`) escono puliti; il modo B (pausa di tutti i thread
con `svcSetThreadActivity`, riepilogo, `__appExit`, `svcExitProcess`) e il
modo C (come B senza `__appExit`) danno "The software was closed because an
error occurred", senza report. La causa più probabile è `svcExitProcess`; il
run 15 non la separa dalla pausa dei thread, perché B e C fanno tutti e due
l'una e l'altra.

In switch-game e switch-smoke l'uscita è A per default; D resta con la cvar
`nx_exit_mode = D`. La pausa resta solo come diagnostica (istantanee dei
thread del gioco; passo 6 dello smoke con la cvar `nx_smoke_pause_test`), mai
sul percorso d'uscita. Il `--wrap=svcExitProcess` resta come traccia di una
chiamata da libnx. Da qui la chiusura non è più una priorità.

## 10. Uscita del gioco: modo D (una sola prova pulita, da riconfermare nei prossimi run)

Run 18 (`out/console-runs/run18/`, commit `58b4efc`), `+` tenuto 1 s dopo
~762 s dall'avvio del thread guest: `game-exit.log` ha tutti i 12 passi, in
quest'ordine e senza righe anomale:

1. scelta del modo D;
2. `file operations in flight: 1`;
3. cancello I/O chiuso (nuove aperture, letture e scritture → `C0000120`),
   attesa dell'operazione in corso, al più 2 s;
4. `file I/O: idle after 0 ms`;
5. stop del worker dell'audio nullo (flag, al più 2 s);
6. `null audio worker ended after 4 ms`;
7. ogni thread guest esce alla prossima export (`svcExitThread`), i thread
   della GPU nulla si fermano;
8. join, al più 2 s;
9. `9 of 31 threads ended within 2 s` (gli altri 22 sono thread guest fermi
   in un'attesa che non arriva mai a un'altra export: il join usa tutti i 2 s);
10. `exit(0)`; 11. handler `atexit`; 12. `_exit`.

`game-crash.log` vuoto, nessun report di Atmosphère, la console torna al
menu. Il run 18 non aveva i tempi dei singoli passi (solo le durate di 4 e 6
e il limite di 8); dal run 19 ogni passo porta `+N ms` dalla scelta del modo.
Due righe da notare, non errori: il passo 8 conta 59 handle (XThread più
tracce delle export, con i doppioni) mentre il passo 9 conta i 31 distinti; la
riga `exit: 5 host XThreads running:` aveva i nomi vuoti
(`XObject::name()` è vuoto per i thread; dal run 19 `XThread::thread_name()`).

**Regola:** il modo D resta il default di switch-game; il modo A
(`exit(0)` con i thread vivi) aveva crashato dopo `_exit` nel run 17. Una
sola uscita pulita non basta: ogni run successivo deve riportare i 12 passi e
`game-crash.log` vuoto prima di considerarla stabile.

## 11. Log: niente scrittura sulla SD nel thread che logga (run 19)

Il log durevole di switch-game (apri, scrivi, `fsync`, chiudi per ogni riga,
nel thread che logga) è giusto per cercare un crash e sbagliato per misurare:
nel run 18 il worker delle letture asincrone scriveva così una riga
`[AsyncIO] complete` per ogni lettura (14.624 righe). Dal run 19 il default è
un thread di scrittura che svuota il buffer una volta al secondo; i passi
d'uscita, le terminazioni, lo stop e i fault scrivono il buffer subito. La
modalità durevole resta con `nx_log_durable = true` per la diagnosi dei crash.
Lo stesso vale per `thread_local` (su devkitA64 è una chiamata a
`__aarch64_read_tp`): fuori dai cicli caldi.
