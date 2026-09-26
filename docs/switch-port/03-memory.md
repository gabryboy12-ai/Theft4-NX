# Switch port — 03: memoria guest

Aggiornato: 2026-09-25 · Ramo `switch-port` · Progetto deciso e fase 1 implementata (§8–§10). Le sezioni 1–7 sono l'analisi che ha portato alla decisione.

Legenda: **[codice]** = verificato nei sorgenti del repo. **[HW]** = comportamento del kernel Horizon da confermare sulla console, con `switch-smoke` o con un test mirato.

## Progetto definitivo (2026-09-25)

Deciso dopo `switch-smoke` T1–T8 sulla console:

- **Traduzione con tabella a 256 voci** per tutti gli accessi alla memoria guest: `host = addr + table[addr >> 24]`. In T7 costa +48,5% nel sequenziale (contro +92,3% del `csel` di T6) e +4,1% nel casuale (contro +7,4%).
- **Backing a blocchi da 2 MiB** presi con `aligned_alloc` e spostati al loro offset con `svcMapProcessCodeMemory` + `svcSetProcessMemoryPermission(RW)`:
  - il blocco viene preso **su richiesta**, quando il gioco impegna una pagina;
  - viene **restituito** quando non contiene più pagine guest impegnate.

  T8: 0,023 ms per blocco, 512 MiB in 5,86 ms, dati e atomiche corretti, nessun dato perso allo smontaggio.
- **Nessun alias.** 0x90 → 0x80, 0x7F → fisica, e le finestre 0xA/0xC/0xE → fisica sono voci della tabella.
- **Elisione della traduzione per stack (r1) e indirizzi costanti:** è un'**ottimizzazione futura** (§7). Non è implementata.

L'implementazione della fase 1 (correttezza prima delle prestazioni) è descritta in §8–§10.

## Decisione (2026-09-25, dopo `switch-smoke` T1–T6 sulla console)

**Si procede con il progetto A: traduzione degli indirizzi, con la memoria presa da `malloc`.** I motivi vengono dallo `smoke.log` della console (hbl in modalità applicazione, tutte le SVC di memoria segnalate come disponibili):

- **Alias RW veri: impossibili.**
  - `svcMapProcessCodeMemory` **sposta** la memoria: dopo il mapping la sorgente passa a `perm=--- attr=0x1` e la destinazione diventa l'unica vista (T1).
  - CodeMemory dà solo **owner RW + slave R-X**: l'owner accetta solo RW, lo slave solo R-- o R-X. Un secondo `MapOwner` o `MapSlave` sullo stesso oggetto viene rifiutato con `2001-0125` (T2).
- **`svcMapPhysicalMemory` non disponibile.** Il processo non ha system resource (`total 0x0`), e la chiamata fallisce con `2001-0125` sia in ASLR sia nella regione alias (T3). Quindi `memory_switch.cpp` oggi non può funzionare in nessun caso (§4).
- **Heap già tutto assegnato dal loader.** C'è l'heap override, il pool libero è di 3 MiB e `svcSetHeapSize` non può far crescere l'heap (T5). La memoria del gioco va presa da `malloc`, che dispone di tutto il pool: 512 MiB con `malloc` + `memset` in 88 ms.
- **Progetto B (memoria condivisa):** resta escluso finché non si prova `svcCreateSharedMemory` con più mapping. T2 fa pensare che il kernel rifiuti i mapping multipli anche lì.

Le due domande che restavano aperte hanno avuto risposta sulla console: il costo della tabella (T7) e il commit su richiesta con `svcMapProcessCodeMemory` (T8). Vedi il progetto definitivo qui sopra. La §7 conta quanti accessi possono saltare la traduzione.

## Sintesi

- `xmemory.cpp` apre **4,5 GiB di spazio di indirizzamento host** divisi in **9 viste**. Dietro ci sono **2800 MiB distinti** di un unico file di mapping, e **5 viste su 9 sono alias** di memoria che compare anche in un'altra vista.
- Il codice ricompilato accede alla memoria con `base + indirizzo guest`, **senza consultare gli heap** [codice]. Quindi gli alias devono essere indirizzi host reali oppure va cambiata la traduzione; non c'è una terza possibilità.
- Oggi `memory_switch.cpp` **non può funzionare**: impegna subito tutta la memoria fisica del mapping, rifiuta la dimensione richiesta dal runtime e lascia le viste alias senza memoria dietro. Inoltre la vista primaria non finisce all'indirizzo che il runtime usa come base.
- **Raccomandazione: progetto A**, cioè togliere gli alias traducendo gli indirizzi dentro le macro di accesso, con impegno della memoria su richiesta. Non dipende da SVC privilegiate. Il progetto B, con memoria condivisa, va considerato solo se `switch-smoke` mostra le SVC disponibili.

## 1. Le viste create da `xmemory.cpp`

`Memory::Initialize()` (`src/system/xmemory.cpp:136`) [codice]:

1. crea un mapping di `round_up(0x120000000 + G, G)` byte, dove `G` = `allocation_granularity()`: 4 KiB su Linux, 16 KiB su iOS, 64 KiB su Windows, **2 MiB su Switch**. Su Switch sono quindi `0x120200000` byte. Il `+ G` serve alla vista 7, che finisce a `0x120001000`;
2. cerca una base `mapping_base = 1 << n`, con `n` da 32 a 63, alla quale `MapViews()` riesca a mappare tutte le 9 viste di `map_info[]` a `mapping_base + inizio guest`, con offset nel file `target & ~(G-1)`;
3. imposta `virtual_membase_ = mapping_base` e `physical_membase_ = mapping_base + 0x100000000`.

| # | Intervallo guest (host = `mapping_base` + questo) | Dimensione | Offset nel file (`target`) | Heap / pagina | Alias di |
|---|---|---|---|---|---|
| 0 | `0x00000000–0x3FFFFFFF` | 1024 MiB | `0x000000000` | `v00000000` virtuale, 4 KiB | — |
| 1 | `0x40000000–0x7EFFFFFF` | 1008 MiB | `0x040000000` | `v40000000` virtuale, 64 KiB | — |
| 2 | `0x7F000000–0x7FFFFFFF` | 16 MiB | `0x100000000` | nessun heap (`LookupHeap` restituisce `nullptr`); writeback GPU | **fisica `0x00000000–0x00FFFFFF`** |
| 3 | `0x80000000–0x8FFFFFFF` | 256 MiB | `0x080000000` | `v80000000` XEX, 64 KiB | stesso backing della 4 |
| 4 | `0x90000000–0x9FFFFFFF` | 256 MiB | `0x080000000` | `v90000000` XEX, 4 KiB | **vista 3** |
| 5 | `0xA0000000–0xBFFFFFFF` | 512 MiB | `0x100000000` | `vA0000000` fisico, 64 KiB | **fisica** |
| 6 | `0xC0000000–0xDFFFFFFF` | 512 MiB | `0x100000000` | `vC0000000` fisico, 16 MiB | **fisica** |
| 7 | `0xE0000000–0xFFFFFFFF` | 512 MiB | `0x100001000` (vedi sotto) | `vE0000000` fisico, 4 KiB (heap di `0x1FD00000`) | **fisica, spostata di +4 KiB** |
| 8 | `0x100000000–0x11FFFFFFF` | 512 MiB | `0x100000000` | `physical` (`physical_membase_`) | vista canonica della fisica |

**Permessi.** `MapViews` mappa tutte le viste in `kReadWrite`. La protezione per pagina viene poi dagli heap (`BaseHeap::AllocFixed`/`Protect` → `rex::memory::AllocFixed`/`Protect`). Al momento dell'avvio [codice, `xmemory.cpp:201-222`]:

- `0x00000000–0x0000FFFF`: `NoAccess`, salvo `protect_zero=false`;
- fisica `0x1FFF0000–0x1FFFFFFF`: riservata `NoAccess`;
- `0xC0000000–0xC0FFFFFF`: impegnata `RW` (writeback GPU);
- **tutto l'heap fisico (512 MiB) impegnato `RW`** con `rex::memory::AllocFixed(kCommit)`. È il pre-commit per la GPU (xenia-canary `5f5be0668`);
- `0x80000000 + 0x1C`: costante richiesta da un titolo.

**Lo spostamento di 4 KiB della vista 7.** Sulla Xbox 360 la finestra `0xE0000000` vede la memoria fisica spostata di `0x1000`. Se `G > 4 KiB` l'offset nel file viene arrotondato per difetto (`0x100000000`), e lo spostamento lo applica `PhysicalHeap::Initialize` con `host_address_offset = 0x1000` (`xmemory.cpp:1684`) [codice]. Il codice generato lo applica con `REX_PHYS_HOST_OFFSET`, che però vale `0x1000` **solo** su Win32 e Darwin (`gta4_init.h:128`). **Su NX, con `G` = 2 MiB, l'heap usa +4 KiB mentre il codice generato usa 0: è un disallineamento già presente**, da correggere in qualunque progetto.

**Offset del file che nessuna vista usa:** `0x7F000000–0x7FFFFFFF` e `0x90000000–0xFFFFFFFF`, cioè 1,77 GiB di buco.

## 2. Come lo fanno i backend esistenti

| | Linux / POSIX (`memory_posix.cpp`) | iOS (`memory_ios.cpp`) | Switch oggi (`memory_switch.cpp`) |
|---|---|---|---|
| Oggetto di backing | `shm_open` + `ftruncate64` (Android: `ASharedMemory` / ashmem) | `mkstemp` in una cartella temporanea della sandbox, `unlink` subito, `ftruncate` | `svcMapPhysicalMemory` su tutto il mapping, in una regione trovata con `virtmemFindAslr` |
| Viste | `mmap64(MAP_SHARED \| MAP_FIXED, fd, offset)`, una per vista | uguale, `mmap(MAP_SHARED \| MAP_FIXED)` | solo la vista a offset 0 punta al backing; le altre sono prenotazioni `virtmem` **senza memoria dietro** |
| Base | percorso comune: `MapViews(1<<n)` | percorso Darwin: `MapViewsMac` riserva prima tutti i 4,5 GiB con `mmap(PROT_NONE)`, poi fa `MAP_FIXED` dentro | percorso comune, ma la vista 0 **ignora** la base richiesta |
| Alias | veri: stesse pagine a più indirizzi | veri | assenti: le viste alias vanno in fault oppure vengono coperte da memoria separata (vedi §4) |
| Commit | su richiesta: le pagine di `shm` si allocano al primo accesso; `AllocFixed` fa solo `mprotect` | su richiesta; `AllocFixed` fa `mprotect`, `DeallocFixed` fa `madvise` | **immediato**: `svcMapPhysicalMemory` alloca subito tutta la memoria fisica e la azzera con `memset` |
| Granularità | 4 KiB | 16 KiB (pagine host), protezione allargata alla pagina da 16 KiB | 2 MiB per mappare, 4 KiB per i permessi |

Su Linux e iOS tutti i 4,5 GiB sono solo **spazio di indirizzamento**: la RAM consumata è quella delle pagine effettivamente toccate. È per questo che il layout di xenia costa poco sui desktop.

## 3. Memoria da allocare davvero

La console Xbox 360 ha **512 MB di memoria fisica unificata**: tutto quello che un gioco impegna, heap virtuali, XEX, texture e buffer GPU, arriva da lì. Il layout di xenia invece separa il backing degli heap virtuali (offset `0x00000000–0x8FFFFFFF`) da quello della fisica (offset `0x100000000+`), quindi il guest può impegnare in teoria più di 512 MB. Un gioco scritto per la 360 però non lo fa.

| Categoria | Spazio di indirizzamento | Backing distinto | Memoria fisica host necessaria |
|---|---|---|---|
| Heap virtuali `v00000000` + `v40000000` | 2032 MiB | 2032 MiB | solo quanto il gioco impegna |
| XEX `v80000000` (+ alias `v90000000`) | 512 MiB | 256 MiB | immagine XEX più allocazioni XEX |
| Fisica (`physical` + 3 finestre + writeback) | 2064 MiB | 512 MiB | **512 MiB**, oggi pre-impegnati all'avvio |
| **Totale** | **4608 MiB** (`0x120000000`) | **2800 MiB** | **≈ 512 MiB + le allocazioni virtuali del gioco** |

Stima da misurare in gioco: circa 0,6-1,0 GiB, se il pre-commit dei 512 MiB fisici resta. Il pool di un'applicazione Switch è di circa 3,2 GiB (Erista) [HW: il valore reale lo stampa `switch-smoke` al passo 2, "total memory"]. Bisogna quindi avviare il gioco **in modalità applicazione (title takeover)**: in modalità applet (hbmenu dall'album) il pool è di poche centinaia di MiB.

## 4. `memory_switch.cpp` oggi

**Primitive usate** [codice]:

| Primitiva | Uso |
|---|---|
| `virtmemLock`/`virtmemFindAslr`/`virtmemAddReservation`/`virtmemRemoveReservation` | trovano e prenotano intervalli nella regione ASLR. Sono solo contabilità di libnx: nessuna SVC |
| `svcMapPhysicalMemory` / `svcUnmapPhysicalMemory` | alloca o libera memoria fisica a un indirizzo; indirizzo e dimensione allineati a 2 MiB |
| `svcSetMemoryPermission` | protezione per pagina da 4 KiB (`Perm_None`/`R`/`Rw`) |
| `svcQueryMemory` | `QueryProtect`, controllo "già mappato" |

**Perché il limite `0x90000000`.** Dal commento nel file: 2048 MiB per lo spazio virtuale "senza gli alias" più 512 MiB di fisica fanno 2560 MiB, meno 256 MiB di margine = **2304 MiB**. Il limite esiste perché `CreateFileMappingHandle` **impegna subito tutto il mapping** con `svcMapPhysicalMemory`: i 4,5 GiB richiesti dal runtime non entrano nel pool (§3) e vengono rifiutati prima di arrivare a un `LimitReached` del kernel. `switch-smoke` verifica sulla console sia il rifiuto sia la diagnostica a 2304 MiB.

**Problemi** [codice, salvo dove indicato]:

1. **Dimensione.** Il runtime chiede `0x120200000` e riceve `kFileMappingHandleInvalid`: `Memory::Initialize` fallisce subito.
2. **Commit immediato.** Anche a 2304 MiB, tutta la memoria viene allocata e azzerata all'avvio, anche quella che il gioco non userà mai.
3. **Alias senza memoria.** Le viste 1-8 (offset diverso da 0) sono solo prenotazioni `virtmem`: il primo accesso va in fault. Il codice generato però ci accede direttamente (`REX_LOAD_U32(x)` = `base + x`), per esempio a ogni lettura in `0xA0000000+`.
4. **`svcMapPhysicalMemory` fuori dalla regione alias** [HW]. Il kernel dovrebbe accettare `svcMapPhysicalMemory` solo dentro la regione alias del processo, e solo se il processo ha una "system resource" (heap personale del gestore di memoria). `memory_switch.cpp` invece prende gli indirizzi da `virtmemFindAslr`, cioè dalla regione ASLR. Se è così, anche il tentativo a 2304 MiB fallisce: la diagnostica 3b di `switch-smoke` lo mostrerà, con il suo `rc`.

### La vista primaria e `mapping_base`

`MapViews(mapping_base)` chiede la vista 0 a `mapping_base + 0`. Su NX, per l'offset 0, `MapFileView` **restituisce `info.base`**, l'indirizzo ASLR del backing, e accetta la richiesta registrando solo un warning. Per le altre viste restituisce l'indirizzo richiesto (prenotazione senza memoria). `MapViews` controlla solo che il risultato non sia nullo, quindi riesce al primo tentativo (`n = 32`). `Memory::Initialize` poi fa:

```cpp
virtual_membase_  = mapping_base_;                 // = 1 << 32
physical_membase_ = mapping_base_ + 0x100000000;   // = 1 << 33
```

Nessuno dei due indirizzi è `info.base`. Conseguenze:

- i 2,25 GiB impegnati in `info.base` **non vengono mai usati**: memoria sprecata;
- `virtual_membase_ + 0x00000000…0x3FFFFFFF` non è mappato e non è nemmeno prenotato in `virtmem`: la vista primaria non ha prenotazione propria, perché `UnmapFileView` la considera parte del mapping;
- **peggio:** su NX `rex::memory::AllocFixed` con base fissa e `kReserve` chiama `svcMapPhysicalMemory` se l'intervallo non è mappato. Ogni heap, allocando, crea quindi **memoria separata** nella propria finestra. La scrittura della GPU o del gioco in `0xA0000000+x` e la lettura in `physical_membase_+x` finiscono su pagine **diverse**, senza nessun fault. Invece di un crash si ottengono dati divergenti in silenzio: il bug peggiore da diagnosticare;
- `1 << 32` non viene scelto guardando lo spazio di indirizzamento reale: il sistema può avere già altro in quell'area [HW: `switch-smoke` stampa le regioni].

Tutti i progetti qui sotto devono quindi far scegliere la base al backend, per esempio con un nuovo `ReserveGuestAddressSpace()` di piattaforma, invece del ciclo `1 << n`.

## 5. Progetti possibili per Horizon

### Primitive e perché gli alias "facili" non esistono

| Primitiva | Dà un alias RW multiplo? |
|---|---|
| `svcMapMemory(dst, src)` | **No.** Sposta la memoria: `src` diventa inaccessibile finché `dst` resta mappato [HW: comportamento documentato del kernel, da confermare]. È un trasloco, non uno specchio |
| `svcCreateTransferMemory` + `svcMapTransferMemory` | **No**, per lo stesso motivo: il proprietario perde o riduce l'accesso a `src` finché la memoria è trasferita. Serve a passarla a un altro processo |
| `svcCreateCodeMemory` + `svcControlCodeMemory` | **Solo 2 viste**: owner RW e slave RX, come nella JIT di libnx. Ne servono fino a 5 in RW |
| `svcCreateSharedMemory` + `svcMapSharedMemory` più volte | **Sì, in teoria**: stesse pagine a più indirizzi. Ma `svcCreateSharedMemory` di solito non è concessa alle applicazioni, e mappare lo stesso oggetto più volte nello stesso processo è da provare [HW] |
| `svcMapPhysicalMemory` | Memoria fisica nuova a un indirizzo: **nessun alias** |

`switch-smoke` stampa ora le SVC disponibili (`envIsSyscallHinted`) per `MapMemory`, `MapSharedMemory`, `CreateTransferMemory`, `MapPhysicalMemory`, `CreateCodeMemory`, `ControlCodeMemory`, `CreateSharedMemory` e `MapTransferMemory`. Stampa anche l'esito di una **prenotazione virtuale pura di `0x120200000` byte** (`virtmemFindAslr` + `virtmemAddReservation`, senza memoria né SVC), e se la regione alias è abbastanza grande da contenerla.

### A. Nessun alias: traduzione nelle macro di accesso (consigliato)

Un solo backing, senza duplicati:

```
host = base + a                              se a <  0x90000000        (virtuale + XEX 64K)
host = base + (a - 0x10000000)               se 0x90000000 <= a < 0xA0000000  (XEX 4K = stessa memoria)
host = phys + (a - 0x7F000000)               se 0x7F000000 <= a < 0x80000000  (writeback GPU)
host = phys + (a & 0x1FFFFFFF) [+0x1000 se a >= 0xE0000000]   se a >= 0xA0000000
```

La base viene da una sola prenotazione `virtmem` di circa 2,75 GiB (`0x90000000` virtuale + `0x20000000` fisica). La memoria si impegna **su richiesta** a blocchi da 2 MiB, quando gli heap fanno `AllocFixed(kCommit)`: `memory_switch.cpp` già lo fa per le basi fisse. Il pre-commit della fisica diventa facoltativo.

Da modificare, tutto dietro `REX_PLATFORM_NX`:

- `REX_RAW_ADDR`/`REX_LOAD_*`/`REX_STORE_*`, cioè il template codegen che produce `gta4_init.h`: il codice generato va ricompilato, ma non rigenerato dal XEX;
- `TranslateVirtual` e `HostToGuestVirtual`;
- `MapViews` o `Initialize`;
- gli intervalli dell'`MMIOHandler`;
- `REX_PHYS_HOST_OFFSET`, che ora corrisponderebbe per costruzione.

| Pro | Contro | Rischi |
|---|---|---|
| Nessuna SVC privilegiata: funziona con qualunque NPDM | Un confronto e un salto (o `csel`) in più a ogni accesso guest; il caso comune (`a < 0x90000000`) resta un solo confronto ben predetto | Codice host che fa aritmetica di puntatori **attraverso** il confine di una finestra: `memcpy` di un buffer guest a cavallo di `0x9FFFFFFF/0xA0000000`, oppure `TranslateVirtual(x) + n` con `x + n` in un'altra finestra |
| Memoria usata = memoria impegnata; niente sprechi | Più codice da cambiare (macro, xmemory, MMIO) | Prestazioni: da misurare su una funzione calda; peggio nei cicli che accedono alle finestre fisiche |
| Deterministico: niente dipende da `rc` del kernel | Diverge dalle altre piattaforme | Codice SDK che usa `virtual_membase()` direttamente senza `TranslateVirtual` (13 punti fuori dalla grafica) da controllare uno per uno |

### B. Alias veri con memoria condivisa

- 1 oggetto `SharedMemory` da 512 MiB per la fisica, mappato 4 volte: `physical`, `0xA0000000`, `0xC0000000`, `0xE0000000` con `host_address_offset` come su Windows/Darwin;
- 1 da 256 MiB per l'XEX, mappato 2 volte;
- heap virtuali con `svcMapPhysicalMemory` su richiesta;
- la vista 2 (writeback GPU, 16 MiB che fanno alias dell'inizio della fisica) **non si può fare**, perché `svcMapSharedMemory` mappa tutto l'oggetto: serve comunque la traduzione per `0x7F000000`.

| Pro | Contro | Rischi |
|---|---|---|
| Nessun costo per accesso; codice generato quasi invariato | Richiede `svcCreateSharedMemory`, di norma negata alle applicazioni | Disponibilità della SVC diversa tra hbl, forwarder e NSP: dipende dall'NPDM [HW] |
| Stesso modello delle altre piattaforme | 768 MiB impegnati subito (gli oggetti condivisi non sono su richiesta) | Non è detto che il kernel permetta più mapping dello stesso oggetto nello stesso processo [HW] |
| | La vista 2 resta un'eccezione | Limiti di risorse del kernel sul numero e sulla dimensione degli oggetti condivisi [HW] |

### C. Ibrido con fault: prenotazione piena e mapping pigro nel gestore di eccezioni

Si prenotano tutti i 4,5 GiB. `__libnx_exception_handler`, al primo accesso a un blocco da 2 MiB, fa `svcMapPhysicalMemory` e riprende l'esecuzione. Per gli alias il gestore dovrebbe però **emulare l'istruzione** (lettura o scrittura sulla vista canonica), come l'`MMIOHandler` fa per l'MMIO.

| Pro | Contro | Rischi |
|---|---|---|
| Nessuna modifica alle macro | Ogni accesso a un alias è un'eccezione: da migliaia a milioni di trap al secondo | Prestazioni inaccettabili per texture e buffer; fragile con le istruzioni NEON |

Serve solo come passo intermedio di debug, non come soluzione.

### Raccomandazione

**Progetto A.** È l'unico che non dipende da permessi del kernel che non controlliamo e che usa solo la memoria richiesta dal gioco. Il costo per accesso è piccolo e misurabile. Chiude insieme anche il disallineamento di `REX_PHYS_HOST_OFFSET` su NX.

Il progetto B si potrà valutare dopo, come ottimizzazione, se `switch-smoke` mostra `svcCreateSharedMemory` e `svcMapSharedMemory` disponibili e un test mirato conferma che si possono fare più mapping dello stesso oggetto.

Ordine suggerito:

1. eseguire `switch-smoke` sulla console per ottenere regioni, pool, SVC e rc di `svcMapPhysicalMemory` fuori dalla regione alias;
2. nuova API di piattaforma per prenotare lo spazio guest (addio al ciclo `1 << n` su NX);
3. traduzione in `TranslateVirtual`/`HostToGuestVirtual` e macro codegen;
4. commit su richiesta e misura della memoria in gioco.

## 6. Test sulla console: `switch-smoke` T1–T9

Dopo i passi 1–4 e il verdetto `SMOKE OK/FAIL`, `switch-smoke.nro` esegue nove sonde (`tools/switch-smoke/probes.cpp`). Sono **misure**, non test con esito positivo o negativo, e non cambiano il verdetto.

**Log resistente ai crash.** Ogni riga di `smoke.log` scritta dallo smoke segue questi passi:

1. flush del logger SDK;
2. `open(O_APPEND)`, `write`, `fsync`, `close` su un descrittore separato.

Ogni sonda scrive `Tn BEGIN` prima di iniziare e una riga `Tn about to …` prima di ogni SVC o accesso a memoria che può terminare il processo. Dopo un crash, l'ultima riga del log indica l'operazione tentata. Ogni SVC è registrata con il result code nel formato `rc=0x… (2xxx-yyyy)`.

**Sicurezza delle sonde:**

- una SVC che il loader non segnala come disponibile (`envIsSyscallHinted`) **non viene chiamata**: una SVC non permessa solleva un'eccezione invece di restituire un errore;
- la memoria viene letta o scritta solo dopo che `svcQueryMemory` ne mostra il permesso.

Limite: `fsync` sul descrittore separato forza i dati di quel descrittore. Le righe interne dell'SDK (spdlog, stesso file in append) vengono flushate subito prima, ma non passano da un `fsync` proprio.

| Sonda | Domanda | Cosa fa | Come leggerla |
|---|---|---|---|
| T1 | `svcMapProcessCodeMemory` crea un alias RW? | Buffer heap `src` da 64 KiB, `dst` nella regione codice (`virtmemFindCodeMemory`). `svcMapProcessCodeMemory(own, dst, src)`, poi `svcSetProcessMemoryPermission(dst, RW)`. Stampa tipo e permessi di `src` e `dst` dopo ogni passo. Poi scrive da una vista e rilegge dall'altra, in entrambe le direzioni se i permessi lo consentono. Infine smonta e rilegge `src` | Se `src` passa a `perm=---` dopo il mapping, è un **trasloco, non un alias**, e il progetto B non può usarlo |
| T2 | Quali permessi hanno le viste di CodeMemory? Si può mappare due volte? | `svcCreateCodeMemory`, poi `MapOwner`/`MapSlave` provati con R, RW, R-X e RWX uno alla volta. Poi coppia owner RW + slave R-X con scrittura e rilettura, **secondo owner** RW e secondo slave sullo stesso oggetto | Solo con un secondo owner RW riuscito CodeMemory darebbe più di un alias scrivibile |
| T3 | `svcMapPhysicalMemory` fuori o dentro la regione alias; quanto si può mappare? | Stampa la system resource del processo e il pool libero. Prova 2 MiB in ASLR (come oggi `memory_switch.cpp`). Poi, nella regione alias, ricerca binaria a blocchi da 2 MiB della dimensione massima, con probe sulle estremità e tempo per ogni `svcMapPhysicalMemory`, 512 MiB compresi | L'`rc` in ASLR conferma o smentisce il punto 4 di §4 |
| T4 | Quale backend sceglie `jitCreate`? | Stampa gli hint coinvolti e chiama `jitCreate(0x1000)`; registra il tipo scelto | Vedi sotto |
| T5 | Heap massimo con `svcSetHeapSize` e tempo per allocare 512 MiB | Stampa se il loader fornisce l'heap (override) e l'heap attuale. Ricerca binaria del massimo, **solo per crescita** (malloc usa l'heap attuale), poi il tempo di `svcSetHeapSize(+512 MiB)` e del primo `memset`. Ripristina la dimensione iniziale. Se la crescita non è possibile, misura `malloc` + `memset` di 512 MiB | |
| T6 | Costo della traduzione del progetto A | 100 M letture + scritture u32 big-endian (volatili, `rev`, come `REX_LOAD/STORE_U32`) su 64 MiB. 3 modalità × 2 schemi di accesso (sequenziale, pseudo-casuale LCG) con gli stessi indirizzi | Differenza % rispetto all'accesso diretto |
| T7 | Costo del progetto A con una tabella | Come T6, con gli stessi schemi e indirizzi, ma con `host = addr + table[addr >> 24]` su 256 `u64`. Rifà le misure dirette nello stesso run. Prima verifica la codifica della tabella su 9 indirizzi | Differenza % rispetto al diretto; ms a confronto con T6 |
| T8 | Commit su richiesta con `svcMapProcessCodeMemory` | Riserva 0xA0000000 byte con `virtmemFindCodeMemory`. Sposta al suo interno 256 blocchi da 2 MiB presi con `aligned_alloc(2 MiB)`, uno ogni 10 MiB, e li porta a RW con `svcSetProcessMemoryPermission`. Scrive in ogni blocco un motivo diverso e rilegge tutto. Esegue ldaxr/stlxr e CAS a 32 e 64 bit sul primo e sull'ultimo blocco. Smonta i blocchi e controlla le sorgenti | Tempi per blocco e totali, esito di dati e atomiche. "LOST" se i dati scritti tramite la destinazione non tornano nella sorgente. Una riga per blocco solo per errori o tempi oltre 3× la media; "about to" solo per il primo e l'ultimo blocco di ogni serie |
| T9 | Un fault su memoria guest si gestisce e si riprende? Quanto costa? | Sulla memoria del runtime (step 3): porta una pagina da 4 KiB a R-- con `svcSetProcessMemoryPermission` e ci scrive. Un gestore installato con `rex::arch::ExceptionHandler::Install` (dopo quello dell'`MMIOHandler`) riporta la pagina a RW e riprende l'istruzione. Poi 1000 giri completi, le sole SVC per confronto, e un fault in lettura su una pagina `---`. Infine il write-watch del runtime: `EnablePhysicalMemoryAccessCallbacks` su memoria fisica e scrittura tramite la finestra 0xC | Numero di fault, valore scritto, µs per giro. Per il write-watch: chiamate al callback di invalidazione e valore letto tramite la finestra 0xA (§10) |

### T4: cosa fa `jitCreate` (libnx 4.12.0 installata)

Ho verificato sul disassemblato di `jit.o` in `/opt/devkitpro/libnx/lib/libnx.a`, e coincide con `nx/source/kernel/jit.c` di libnx. **Non controlla la versione del firmware**: guarda solo gli hint delle SVC forniti dal loader.

1. Se `envIsSyscallHinted(0x4B)` e `(0x4C)` → **`JitType_CodeMemory`**. Crea `svcCreateCodeMemory` su un buffer heap, poi `MapOwner` con **RW** (`perm=3`) e `MapSlave` con **R-X** (`perm=5`), ciascuno a un indirizzo di `virtmemFindCodeMemory`.
2. Altrimenti, se `0x73`, `0x77`, `0x78` sono disponibili e `envGetOwnProcessHandle() != 0` → **`JitType_SetProcessMemoryPermission`**. `rw_addr` è il buffer sorgente stesso; il passaggio a eseguibile fa `svcMapProcessCodeMemory` + `svcSetProcessMemoryPermission(R-X)`, e quello a scrivibile smonta. Le due viste **non sono mai attive insieme**: è coerente con l'ipotesi che MapProcessCodeMemory sia un trasloco.
3. Altrimenti restituisce `0x4759` (libnx `LibnxError_JitUnavailable`).

Quale dei due scelga su questa console dipende quindi dagli hint forniti dal loader (hbl, forwarder o NSP) e non dal firmware. T4 lo stampa.

### T6: codice misurato

Dal disassemblato di `probes.o`:

- **diretto:** un `ldr`/`str` con indirizzamento `[base, off, uxtw]`;
- **progetto A:** `and` + `cmp` + 2 `add` + `csel`, senza salti, quindi nessun errore di predizione anche con finestre miste.

Nella variante sequenziale "50% finestre" c'è un contatore in più (la finestra è `i & 1`), da considerare leggendo quel numero.

### Risultati T1–T6 sulla console (2026-09-25)

| Sonda | Risultato |
|---|---|
| T1 | `svcMapProcessCodeMemory` riesce. La sorgente passa a `---` con `attr=0x1` (bloccata), la destinazione diventa `type=0x09 RW-` dopo `svcSetProcessMemoryPermission`. Nessuna scrittura incrociata è stata possibile: **è un trasloco**. Il valore letto nella sorgente dopo lo smontaggio non dimostra niente, perché tramite `dst` non era stato scritto nulla. Lo misura T8 |
| T2 | Owner: solo RW. Slave: solo R-- o R-X. Owner RW e slave R-X vedono la stessa memoria. La sorgente resta bloccata finché l'oggetto esiste; dopo la chiusura contiene i dati scritti. Un secondo owner o slave: `2001-0125` |
| T3 | System resource 0: `svcMapPhysicalMemory` fallisce con `2001-0125` sia in ASLR sia nella regione alias |
| T4 | `jitCreate` sceglie `JitType_CodeMemory` |
| T5 | Heap override dal loader, pool libero 3 MiB. T5 legge la dimensione dell'heap dal primo blocco (1 MiB) invece che dall'heap intero, quindi la ricerca ha provato a **ridurre** l'heap (`svcSetHeapSize(2 MiB)` → `2001-0106`). Il kernel ha rifiutato e non ci sono danni, ma con l'heap override la ricerca del massimo di T5 non è attendibile. `malloc(512 MiB)` + `memset`: 88 ms |
| T6 | Sequenziale: diretto 3,50 ns/iter; progetto A +92% (tutto sotto 0xA0000000) e +124% (50% nelle finestre). Casuale: diretto 34,5 ns/iter; progetto A +7,4% e +12,9% |

Il +92% sequenziale è il caso peggiore: il ciclo contiene solo l'accesso, e le 5 istruzioni di traduzione lo raddoppiano. Nel caso casuale domina il cache miss e la traduzione quasi non si vede.

### Risultati T7 e T8 sulla console (2026-09-25)

- **T7:** con la tabella il sequenziale costa +48,5% (T6 `csel`: +92,3%) e il casuale +4,1% (T6: +7,4%). Il progetto definitivo usa quindi la tabella.
- **T8:** 0,023 ms per blocco da 2 MiB, 512 MiB in 5,86 ms. Dati e atomiche (ldaxr/stlxr, CAS a 32 e 64 bit) sono corretti. Allo smontaggio nessun dato è andato perso: la sorgente contiene quanto scritto tramite la destinazione.

### T7: codice generato a confronto con T6

Disassemblato di `probes.o` (GCC 16.1, `-O2 -mtune=cortex-a57`). Ciclo interno sequenziale, con tutti gli accessi sotto 0xA0000000:

```
T6 diretto                  T6 progetto A (csel)               T7 tabella
add  w1, w1, #4             add  w3, w3, #4                    add  w2, w2, #4
subs x4, x4, #1             and  w3, w3, #0x3fffffc            and  w2, w2, #0x3fffffc
and  w1, w1, #0x3fffffc     mov  w2, w3           ; barriera   mov  w1, w2           ; barriera
ldr  w2, [x5, w1, uxtw]     and  x5, x2, #0x1fffffff           lsr  w3, w1, #24
rev  w2, w2                 cmp  w2, w7           ; 0x9fffffff subs x5, x5, #1
add  x0, x0, w2, uxtw       add  x4, x8, w2, uxtw              ldr  x6, [x7, x3, lsl #3] ; tabella
add  w3, w2, #1             add  x5, x1, x5                    ldr  w3, [x6, w1, uxtw]
rev  w3, w3                 csel x4, x4, x5, ls                rev  w3, w3
str  w3, [x5, w1, uxtw]     ldr  w2, [x4]                      add  x0, x0, w3, uxtw
b.ne                        subs x6, x6, #1                    add  w4, w3, #1
                            rev / add / add / rev              rev  w4, w4
                            str  w5, [x4]                      str  w4, [x6, w1, uxtw]
                            b.ne                               b.ne
```

- **T6:** 5 istruzioni di traduzione (`and`, `cmp`, `add`, `add`, `csel`), tutte ALU. `ldr` e `str` usano `[x4]`, cioè l'indirizzo già calcolato.
- **T7:** 2 istruzioni, `lsr` e il `ldr` dalla tabella. La somma `addr + entry` entra nell'indirizzamento `[x6, w1, uxtw]` di `ldr` e `str`, come nell'accesso diretto. In cambio c'è un **load dipendente** sul percorso dell'indirizzo: un hit in L1, circa 4 cicli sull'A57.
- **Finestre miste:** il ciclo di T7 resta uguale, perché è la tabella a scegliere la finestra, non un confronto. T6 invece ha bisogno anche lì di `cmp` e `csel`.
- **Nel codice ricompilato** la tabella deve stare in un registro: come secondo parametro accanto a `base`, oppure caricata con `adrp` + `ldr` da una variabile globale all'inizio di ogni funzione. I suoi 2 KiB restano in L1 nei cicli caldi.

La scelta tra le due forme dipende dai numeri di T7 sulla console.

## 7. Accessi che possono saltare la traduzione

Conteggio statico sul codice generato presente nel repo (`gta4-recomp/generated/gta4_recomp.*.cpp`, 714.912 accessi). Uno script classifica il primo argomento di ogni `REX_LOAD_*`, `REX_STORE_*` e `REX_RAW_ADDR`; quando l'argomento è `ea`, risale all'assegnazione `ea = ...`. Il codice non è ancora compilato per Switch, ma le macro sono le stesse.

| Classe | Accessi | % | Traduzione |
|---|---|---|---|
| **relativi a r1** (stack guest) | 196.399 | **27,5%** | si può saltare |
| **indirizzo costante** (catena `li`/`lis`/`addi`/`ori` nello stesso blocco) | 65.418 | **9,2%** | si può risolvere durante la generazione |
| costante, con analisi lineare sull'intera funzione (limite superiore) | 94.239 | 13,2% | come sopra, ma richiede un'analisi del flusso |
| relativi a r13 (KPCR) | 647 | 0,1% | — |
| altro | 452.448 | 63,3% | a runtime |

Per tipo di accesso:

| Tipo | Accessi | r1 | costanti |
|---|---|---|---|
| load | 398.481 | 20,0% | 12,7% |
| store | 247.608 | **46,4%** | 4,2% |
| `REX_RAW_ADDR` (vettori e atomiche) | 68.823 | 2,4% | 6,5% |

**Stack (r1).** Gli stack guest stanno in `kStackAddressRangeBegin..End` = `0x70000000–0x7EFFFFFF` (`xthread.h:294`), sotto `0x7F000000`. Nel progetto A sono quindi sempre il percorso diretto `base + a`.

Il generatore sa quando la base è r1:

- `emit_load_d_form`/`emit_store_d_form` (`src/codegen/builders/context.cpp:460-538`) ricevono il registro base in `insn.operands[2]` (d-form) o `operands[1]` (x-form);
- `stwu r1` passa da `ea = -N + ctx.r1.u32`.

Basta emettere una macro diversa, per esempio `REX_LOAD_U32_STACK`, uguale all'attuale `base + a`. Condizione: il gioco usa r1 solo come stack pointer (ABI PowerPC), come in tutto il codice analizzato.

**Costanti.** Il 99,6% degli indirizzi costanti è in `0x80000000–0x8FFFFFFF` (dati del XEX), quindi percorso diretto. Il resto: 277 accessi sotto `0x7F000000`, 8 in `0x7F` e 1 da `0xA0000000` in su.

Il generatore traccia già i `lis` per l'MMIO, ma solo come bit "è MMIO": `mmio_base_regs` in `builder_context.h:44`, impostato da `build_lis` in `memory.cpp:27`. Se tracciasse il valore (`lis` + `addi`/`ori`), potrebbe scegliere la finestra durante la generazione. Il valore andrebbe azzerato alle etichette, e dopo le chiamate per r0 e r3–r12. `mmio_base_regs` oggi non si azzera alle etichette: per decidere la traduzione serve la versione conservativa.

**In tutto, il 36,7% degli accessi (27,5% + 9,2%) può evitare la traduzione a runtime; tra gli store, la metà.** Il costo misurato in T6/T7 resta sul 63% degli accessi, cioè quelli basati su puntatori.

**Ottimizzazione futura, non implementata.** Con la tabella, un accesso relativo a r1 o a un indirizzo costante nella fascia diretta può saltare `lsr` + `ldr` della voce e tornare a `base + a`. Per gli indirizzi costanti in un'altra finestra la voce si può risolvere durante la generazione. Richiede una macro dedicata nel generatore (§9) e la verifica che r1 resti sempre nello stack guest. La fase 1 traduce tutti gli accessi.


## 8. Implementazione, fase 1: memoria guest su NX

Tutto dietro `REX_PLATFORM_NX`; le altre piattaforme non cambiano.

| File | Ruolo |
|---|---|
| `src/core/memory_switch.cpp` | riscritto: prenotazione dell'arena, tabella, commit/decommit a blocchi da 2 MiB con conteggio dei riferimenti, protezione |
| `include/rex/memory/guest_table.h` | nuovo: `rex_guest_table[256]`, `GuestToHost`, `HostToGuest` |
| `include/rex/memory/utils.h` | API `rex::memory::nx::ReserveGuestArena` / `ReleaseGuestArena` / `GetGuestArenaStats` |
| `src/system/xmemory.cpp`, `include/rex/system/xmemory.h` | ramo NX di `Memory::Initialize`, membase degli heap, transizioni di commit degli heap, `TranslateVirtual`, `HostToGuestVirtual`, write-watch (§10) |
| `include/rex/ppc/function.h` | conversione host → guest dei puntatori restituiti o passati al guest |
| `src/core/exception_handler_switch.cpp` | stack di eccezione da 64 KiB (§10) |

### Arena e tabella

L'arena è una sola prenotazione `virtmem` nella regione codice (`virtmemFindCodeMemory`, dove `svcMapProcessCodeMemory` accetta le destinazioni), allineata a 2 MiB. L'indirizzo lo sceglie il backend: su NX il ciclo `1 << n` di `Initialize` non si usa più. Questo risolve il problema di `mapping_base` di §4.

| Host | Contenuto |
|---|---|
| `V = arena + 0` … `+0x8FFFFFFF` | guest `0x00000000–0x8FFFFFFF` (`virtual_membase`) |
| `P = arena + 0x90000000` … `+512 MiB + 4 KiB` | memoria fisica guest (`physical_membase`), arrotondata a 2 MiB |

| Voce `rex_guest_table[i]` | Valore |
|---|---|
| `0x00–0x7E`, `0x80–0x8F` | `V` |
| `0x7F` | `P - 0x7F000000` (writeback GPU → fisica 0) |
| `0x90–0x9F` | `V - 0x10000000` (XEX 4 KiB → stessa memoria di 0x80) |
| `0xA0–0xBF` | `P - 0xA0000000` |
| `0xC0–0xDF` | `P - 0xC0000000` |
| `0xE0–0xFF` | `P - 0xE0000000 + 0x1000` (come `host_address_offset` dell'heap 0xE) |

Ogni heap riceve una membase tale che `membase + heap_base` sia l'indirizzo host della sua prima pagina:

- `v90000000`: `V - 0x10000000`;
- `vA0000000`: `P - 0xA0000000`;
- `vC0000000`: `P - 0xC0000000`;
- `vE0000000`: `P - 0xE0000000`; i 4 KiB li aggiunge l'heap stesso.

Così `TranslateRelative`, le protezioni e il write-watch degli heap cadono sulle stesse pagine della tabella. Lo scostamento di 4 KiB di 0xE è lo stesso per heap, runtime e codice generato, quindi il vecchio disallineamento di `REX_PHYS_HOST_OFFSET` su NX (§1) sparisce.

### Commit e decommit

- Il backend tiene un contatore di riferimenti per ogni pagina host da 4 KiB e, per ogni blocco da 2 MiB, il numero di pagine referenziate.
- **Primo riferimento a un blocco non mappato:** `aligned_alloc(2 MiB)`, `memset(0)`, `svcMapProcessCodeMemory`, `svcSetProcessMemoryPermission(RW)`. Una pagina che torna referenziata in un blocco rimasto mappato viene azzerata.
- **Ultima pagina rilasciata:** RW, `svcUnmapProcessCodeMemory`, `free`.
- **Un riferimento per heap e per pagina.** Più heap condividono le stesse pagine host: l'heap fisico e le finestre 0xA/0xC/0xE, oppure 0x80 e 0x90. Per questo `xmemory.cpp` chiama il backend solo alle **transizioni di stato del singolo heap** (`BaseHeap::NxCommitPages` / `NxDecommitPages`). Una pagina resta impegnata finché almeno un heap la usa.
- **Punti agganciati:** `AllocFixed`, `AllocRange`, `Decommit`, `Release`, `Dispose`, `Reset`, `Restore`.
- **Protezione:** `svcSetProcessMemoryPermission` a 4 KiB, solo sui blocchi mappati.

### Differenze rispetto al desktop da tenere presenti

| Punto | Desktop | NX |
|---|---|---|
| Pre-commit dei 512 MiB fisici all'avvio | sì | no, su richiesta come il resto |
| `Decommit` / `Release` | solo tabella (+ `Protect` NoAccess con `protect_on_release`) | restituiscono la memoria quando il blocco si svuota |
| Accesso a memoria mai impegnata | riesce (le viste sono mappate RW per intero) | in un blocco mappato riesce, RW come sul desktop; in un blocco non mappato va in **fault** |
| Aritmetica di puntatori host oltre il confine di una finestra da 16 MiB (es. `TranslateVirtual(0x8FFFFF00) + 0x200`) | contigua | **non contigua**: dopo `V + 0x90000000` c'è la memoria fisica |
| `HostToGuestVirtual` e `function.h` su un puntatore alla memoria fisica | la finestra da cui viene il puntatore | sempre la finestra 0xA, perché la vista d'origine non è ricostruibile |
| Range MMIO (`AddVirtualMappedRange`) | pagine protette NoAccess | nessuna protezione (§10) |

## 9. Macro del codice generato su NX

Le macro vengono dal template del generatore, `resources/templates/codegen/init_h.inja`. Il ramo `#if REX_PLATFORM_NX` include `rex/memory/guest_table.h` e definisce ogni accesso come `rex::memory::GuestToHost(addr)`: `REX_RAW_ADDR`, `REX_LOAD_*`, `REX_STORE_*`, `REX_LOAD_STRING` e il ramo non-MMIO di `REX_MM_*`.

- **Altre piattaforme.** Il ramo `#else` è il testo di prima. Le `REX_MM_*` ora passano da `REX_MMIO_FALLBACK_HOST(addr)`, che fuori da NX si espande esattamente in `base + (addr) + REX_PHYS_HOST_OFFSET(addr)`.
- **`gta4-recomp/generated/gta4_init.h` non è modificato.** È un file generato: le nuove macro entrano nel codice del gioco quando `init_h.inja` viene rigenerato con `rexglue codegen`. Il codice `gta4_recomp.*.cpp` resta com'è, perché usa solo i nomi delle macro.
- **`base` resta il parametro di ogni funzione**, cioè l'indirizzo host del guest 0. Lo usano `REX_LOOKUP_FUNC` (la tabella delle funzioni sta a `0x83300000`, nella fascia diretta) e le funzioni host che ricevono `base`.

### Dove sta il puntatore alla tabella

| Opzione | Costo per accesso | Perché no / sì |
|---|---|---|
| Campo in `PPCContext` | un load dal contesto, ripetuto dopo ogni chiamata (il contesto è un riferimento `__restrict` passato a tutte le funzioni) | cambia il layout del contesto e va impostato in ogni punto che crea un contesto |
| Nuovo parametro delle funzioni | nessuno | cambia la firma `REX_FUNC` e tutte le chiamate generate `sub_X(ctx, base)`: richiede di rigenerare tutto |
| `base` = tabella | nessuno | `function.h` e gli hook usano `base` come membase (`GuestPtr`, `v - base`) |
| **Array globale `hidden`** (scelta) | nessun load per l'indirizzo della tabella: `adrp` + `add` PC-relative, calcolati una volta per funzione | nessuna modifica a firme o contesto |

### Disassemblato di funzioni generate

Due funzioni vere di `gta4_recomp.0.cpp`, compilate in scratch con l'header generato e le macro nuove sostituite (GCC 16.1, `-O3 -march=armv8-a -mtune=cortex-a57`, stessi flag dell'SDK).

`sub_821465B0`, inizio (un `lis`/`lwz` da indirizzo costante e due load dipendenti):

```
NX (tabella)                                   Prima (base + addr)
adrp x6, rex_guest_table                       mov  x2, #0xc000
add  x2, x6, :lo12:rex_guest_table             movk x2, #0x82c6, lsl #16
mov  x1, #0xc3d8                               add  x2, x1, x2
movk x1, #0x82c6, lsl #16                      ldr  w2, [x2, #984]        ; lwz r11,-15400(r11)
ldr  x3, [x2, #1040]      ; table[0x82]        rev  w6, w2
ldr  w4, [x3, x1]         ; lwz r11,-15400(r11) ...
rev  w5, w4                                    add  w2, w3, #0x4
...                                            ldr  w2, [x1, w2, uxtw]    ; lwz r10,4(r11)
add  w1, w5, #0x4
lsr  w3, w1, #24
ldr  x3, [x2, x3, lsl #3] ; table[addr >> 24]
ldr  w3, [x3, w1, uxtw]   ; lwz r10,4(r11)
```

Ciclo di `sub_821463B0` (ricerca binaria, `lwzx r11,r11,r7`):

```
NX                                             Prima
add  w5, w11, w4                               add  w9, w11, w5
lsr  w2, w5, #24                               ldr  w3, [x1, w9, uxtw]
ldr  x2, [x10, x2, lsl #3]
ldr  w2, [x2, w5, uxtw]
```

- **Tabella:** l'indirizzo si ottiene con `adrp` + `add` una sola volta per funzione (`x2`/`x10`), senza load di puntatori.
- **Per accesso:** `lsr` + `ldr` della voce. La somma `addr + voce` entra nell'indirizzamento `[x, w, uxtw]` di load e store, come nell'accesso diretto.
- **Indirizzi costanti:** GCC calcola già l'indice, per esempio `ldr x3, [x2, #1040]` = `table[0x82]` e `ldr x3, [x6]` = `table[0]` per `li r11,0`. Resta solo il load della voce, che è l'ottimizzazione futura di §7.
- **Dimensioni:** `sub_821465B0` passa da 57 a 71 istruzioni, `sub_821463B0` da 86 a 96.

## 10. MMIO e write-watch

### Come funziona oggi (desktop)

- **MMIO riconosciuto dal generatore.** Il generatore marca come base MMIO i registri caricati con `lis` ≥ `0x7F00` (`mmio_base_regs`) e gli accessi seguiti da `eieio`. Per questi emette `REX_MM_LOAD_*`/`REX_MM_STORE_*`, che per `0x7F000000–0x7FFFFFFF` chiamano `MMIOHandler::CheckLoad`/`CheckStore`. Non c'è nessun fault.
- **MMIO non riconosciuto.** `Memory::AddVirtualMappedRange` porta il range a NoAccess nella vista 0x7F, quindi un accesso host o guest non riconosciuto va in fault. `MMIOHandler::ExceptionCallback` trova il range, decodifica l'istruzione ARM64/x86 (`TryDecodeLoadStore`), chiama il callback e riprende dopo l'istruzione.
- **Write-watch della memoria fisica.** La GPU (`shared_memory.cpp`, `primitive_processor.cpp`) chiama `EnablePhysicalMemoryAccessCallbacks`, che mette a sola lettura le pagine nelle viste 0xA/0xC/0xE. Quando il guest ci scrive:
  1. l'accesso va in fault;
  2. `MMIOHandler` non trova il range;
  3. `Memory::AccessViolationCallback` chiama `PhysicalHeap::TriggerCallbacks`;
  4. i callback di invalidazione girano, la pagina torna RW e l'esecuzione riprende.

  Le scritture tramite `physical_membase_` **non** vanno in fault, perché su desktop sono un'altra vista delle stesse pagine: GPU e data provider scrivono lì apposta.
- **Consegna delle eccezioni:** segnali su POSIX, VEH su Windows, Mach su macOS. Su Switch passa da `__libnx_exception_handler` in `exception_handler_switch.cpp`, che inoltra alla stessa lista di gestori.

### Cosa cambia su Switch (implementato)

- **Una sola pagina host.** Senza viste separate la protezione colpisce l'unica pagina host. Vanno quindi in fault sia le scritture guest da qualunque finestra, sia quelle host tramite `physical_membase_`.
  - `Memory::NxPhysicalAccessViolation` riceve il fault nell'intervallo `P…P+512 MiB+4 KiB` e chiama `TriggerCallbacks` su **tutte e tre** le finestre. `EnablePhysicalMemoryAccessCallbacks` arma tutte e tre, e chiamandone una sola rimarrebbe un bit di watch su una pagina già tornata RW.
  - Se nessuna finestra osserva la pagina e l'heap fisico la dà per scrivibile, la riporta a RW: è lo stesso recupero del desktop.
- **MMIO.** La finestra 0x7F è tradotta sulla memoria fisica. Proteggere il range MMIO vorrebbe dire proteggere la fisica da `0x00C80000` in poi. Su NX `AddVirtualMappedRange` quindi registra solo il range.
  - Gli accessi riconosciuti dal generatore (`REX_MM_*`) funzionano come prima.
  - **Quelli non riconosciuti finiscono in silenzio nella memoria fisica**, invece di andare in fault.
- **Stack di eccezione.** libnx esegue il gestore su `__nx_exception_stack`: uno stack debole di **0x400 byte**, con un solo `ThreadExceptionDump` globale (verificato nel disassemblato di `__libnx_exception_entry`).
  - Il gestore occupa già da solo circa 0x320 byte per `HostThreadContext`, più l'`Exception`, e i callback prendono lock e girano nel sistema di memoria. Ogni fault gestito sforava quindi lo stack.
  - Ora `exception_handler_switch.cpp` definisce uno stack da 64 KiB.

### Rischi aperti

- **Fault contemporanei su due thread.** Il kernel non fa entrare due thread nel gestore insieme. In mesosphere (`kern_exception_handlers.cpp`, `KProcess::EnterUserException`) un thread in fault diventa l'`exception thread` del processo prima di ricevere l'eccezione in user mode. Un secondo thread in fault aspetta nel kernel finché il primo non esegue `svcReturnFromException` (`LeaveUserException`).

  Con l'ingresso di libnx però la protezione non valeva per il gestore: `__libnx_exception_entry` chiama `svcReturnFromException` **prima** di eseguire `__libnx_exception_handler`, quindi il gestore girava fuori dalla zona esclusiva, su uno stack e un dump globali. In più, al suo ritorno libnx chiama sempre `svcBreak` (run 5, `04-threads-exit.md` §6).

  Ora `exception_handler_switch.cpp` fornisce il proprio `__libnx_exception_entry` (in libnx è weak). Il gestore gira dentro la zona esclusiva e riprende con `svcReturnFromException(0)`. Stack e contesto sono usati da un thread alla volta: **nessuna corruzione, ma i fault di tutto il processo vengono serializzati**.

  Il rischio vero è il **deadlock**. Se il thread A è nel gestore e aspetta un lock tenuto dal thread B, e B va in fault mentre tiene quel lock, B aspetta nel kernel che A esca e A aspetta B. Il gestore prende `global_critical_region_` e il mutex dell'arena: nessun codice deve andare in fault su una pagina osservata mentre tiene uno dei due.

  **T10** (`switch-smoke`) lo misura: 3 thread, uno per core, 10.000 fault di scrittura ciascuno su pagine fisiche osservate diverse. Verifica:
  - che ogni scrittura riletta abbia il valore giusto;
  - che ogni pagina abbia esattamente 10.000 callback di invalidazione;
  - il numero massimo di thread presenti insieme nel callback, con un'attesa di 2 µs per allargare la finestra. Atteso 1: con 2 o più lo stack unico sarebbe un problema reale;
  - il costo per fault quando i core sono in competizione.

  Un blocco oltre 60 s viene riportato come `TIMEOUT` (possibile deadlock).
- **Lock nei callback.** Una scrittura host su una pagina osservata ora va in fault, e i callback di invalidazione girano sul thread che scrive. Se quel thread tiene un lock che serve al callback (per esempio il mutex di `SharedMemory`), si ha un deadlock. Da verificare nei percorsi GPU che scrivono memoria guest tramite `TranslatePhysical`.
- **Costo.** T9 misura il giro completo fault → gestore → ripresa.

## 11. Run 4 sulla console: il commit fallisce dopo il primo RW

Run dei commit `08ce38d8..b60da3d1` (`out/console-runs/run4/smoke.log`):

- `Alloc(4 MiB)` nell'heap `0x40000000` fallisce, e T9 non riesce ad allocare 64 KiB;
- dopo `~Memory` il log riporta "0 blocks mapped" ma "5200 pages committed".

### Causa (dal codice di mesosphere, `kern_k_page_table_base.cpp`)

`svcSetProcessMemoryPermission` verifica lo stato con `CheckMemoryState(..., KMemoryState_FlagCode, KMemoryState_FlagCode, ...)`: lo accettano solo gli stati con `FlagCode`, cioè `Code` e `AliasCode`.

1. `svcMapProcessCodeMemory` lascia il blocco in `AliasCode` (tipo 0x08, `---`).
2. `MapBlock` lo porta a RW con `svcSetProcessMemoryPermission`: il blocco diventa `AliasCodeData` (tipo 0x09). T1 lo mostra già nel run 4.
3. `AliasCodeData` ha il gruppo di flag `FlagsData`: c'è `FlagCanReprotect`, **manca `FlagCode`**. Da qui in poi ogni `svcSetProcessMemoryPermission` sul blocco fallisce con `InvalidCurrentMemory` (0xD401), anche se chiede lo stesso RW.

`AllocFixed` del backend, dopo il commit, chiama `ProtectArena`, che usava proprio `svcSetProcessMemoryPermission`. Quindi **ogni commit falliva dopo aver mappato i blocchi e preso i riferimenti**.

Perché l'init sembrava funzionare:

- `Memory::Initialize` ignora il risultato delle sue tre allocazioni con commit: i primi 64 KiB di `0x00000000`, i 16 MiB di writeback in `0xC0000000` (con la fisica sotto) e i 256 KiB XEX in `0x80000000`.
- I blocchi erano comunque mappati RW, quindi la scrittura a `0x8000001C` e la traduzione passavano.
- In quelle pagine la tabella degli heap però non registrava il commit.

### Contabilità: una perdita, non un contatore cumulativo

`committed_pages` è un valore corrente: scende in `DecommitArena` quando una pagina perde l'ultimo riferimento. Il 5200 del run 4 è formato così:

| Riferimenti presi e mai rilasciati | Pagine |
|---|---|
| init: 16 (`0x00000000`) + 4096 (fisica 0–16 MiB) + 64 (XEX) | 4176 |
| `Alloc(4 MiB)` fallita | 1024 |
| **totale dopo `~Memory`** | **5200** |

I rollback mancavano in due punti:

- `AllocFixed` del backend restituiva `nullptr` dopo `ProtectArena` senza rilasciare i riferimenti;
- `NxCommitPages` restituiva `false` dopo il `Protect` finale senza annullare il proprio commit.

La tabella degli heap non aveva lo stato di commit, quindi `Dispose` non rilasciava niente. `ReleaseGuestArena` smontava poi i blocchi senza toccare i contatori: da qui "0 blocks mapped" con 5200 pagine.

T9 (64 KiB in `0x40000000`) cadeva sugli stessi indirizzi, già referenziati dal commit perso. Il suo `Protect` falliva per la stessa ragione.

### Correzione

- **`memory_switch.cpp`.** `svcSetProcessMemoryPermission` resta solo in `MapBlock`, per il passaggio `AliasCode` → RW. Ogni altra protezione passa da `svcSetMemoryPermission` (serve `FlagCanReprotect`, che `AliasCodeData` ha) tramite `SetArenaPermission`.
  - `SetArenaPermission` divide l'intervallo sui blocchi del kernel (`svcQueryMemory`), perché anche `svcSetMemoryPermission` richiede stato e permesso uniformi su tutto l'intervallo.
  - Si applica ai percorsi `ProtectArena`, al riuso di una pagina e alla rimessa a RW prima dello smontaggio.
- **Rollback.** `AllocFixed` del backend e `NxCommitPages` rilasciano i riferimenti presi se la protezione fallisce.
- **Statistiche.** Nuovi campi `page_references` (somma dei conteggi) e `blocks_mapped_at_release` (blocchi ancora referenziati trovati da `ReleaseGuestArena`). `map_calls`, `unmap_calls` e `map_ticks` sono marcati come cumulativi.
- **Trace.** `rex::memory::nx::SetMemoryTraceSink` riceve ogni SVC del percorso commit/protect con argomenti e `Result`, e il passo che fallisce (`AllocFixed`, `Protect`, `NxCommitPages` con heap e pagine).

### Verifica in `switch-smoke`

- **Step 1.** Configura la policy di diagnostica (`logging`). Senza, `InitLogging` e tutte le righe `REXSYS_*` erano no-op: il run 4 non ha nessuna riga dell'SDK.
- **Step 3.** Gira con il trace attivo, quindi il log contiene la catena completa di SVC. Controlla anche:
  - che `0x80000000` e `0xC0000000` risultino committed nella tabella dell'heap;
  - che dopo il rilascio pagine e riferimenti tornino ai valori post-`Initialize`.
- **Dopo `~Memory`.** Pagine committed, riferimenti, blocchi mappati e blocchi ancora referenziati al rilascio devono essere tutti 0. Altrimenti lo smoke stampa `SMOKE FAIL at guest memory shutdown`.
- **T9, passo 0.** Chiama `svcSetProcessMemoryPermission` sulla pagina RW dell'arena e registra il codice (atteso 0xD401): conferma la causa sulla console. Il resto di T9 usa `svcSetMemoryPermission`.

## 12. MMIO nella finestra 0x7F con la tabella: analisi e proposta (NON implementata)

### Cosa c'è nella finestra 0x7F

- **Desktop** (`map_info` in `xmemory.cpp`): `0x7F000000–0x7FFFFFFF` è una vista dello stesso backing della fisica. L'offset del file è `0x100000000`, cioè fisica `0x00000000–0x00FFFFFF`, come i primi 16 MiB delle finestre 0xA/0xC/0xE. Nessun heap la gestisce: `LookupHeap` restituisce `nullptr` per `0x7F…`. Il commento di xenia dice "GPU writeback + 15mb of XPS?": nemmeno a monte è documentato con precisione cosa il gioco ci legga.
- **`Memory::Initialize`** riserva e committa `0xC0000000` + 16 MiB ("GPU writeback"). La fisica 0–16 MiB è quindi presa dall'emulatore e nessuna allocazione del gioco ci finisce.
- **Range MMIO registrati** (`AddVirtualMappedRange`, maschera `0xFFFF0000`, 64 KiB ciascuno):
  - `0x7FC80000`: registri GPU (`graphics_system.cpp`; il commento dice fino a `0x7FCFFFFF`, ma ne viene registrato 64 KiB);
  - `0x7FEA0000`: registri XMA/APU (`xma_decoder.cpp`).

  Sotto questi indirizzi c'è la fisica `0x00C80000–0x00C8FFFF` e `0x00EA0000–0x00EAFFFF`, dentro i 16 MiB di writeback.
- **Codice host.** Nessun punto dell'SDK accede a `0x7F…` tramite `TranslateVirtual`: ci sono solo le due registrazioni.

### Come xenia separa MMIO e memoria

La protezione è **per vista**. `AddVirtualMappedRange` porta a NoAccess i 64 KiB nella sola vista 0x7F: un accesso a `0x7FC8xxxx` va in fault, mentre la stessa fisica tramite `0xC0C8xxxx` resta memoria normale, perché è un'altra vista.

`MMIOHandler::ExceptionCallback` cerca i range solo per fault sotto `physical_membase_`, cioè nelle viste virtuali. Converte host → guest con `HostToGuestVirtual` e confronta con `address/mask`.

### Su NX oggi

`rex_guest_table[0x7F]` punta alla fisica: la pagina host di `0x7FC80000` è la stessa di `0xC0C80000`. `AddVirtualMappedRange` registra solo il range, e `MMIOHandler` non cercherebbe comunque un range per un fault sopra `physical_membase_`.

Gli accessi non riconosciuti dal generatore (senza `REX_MM_*`) leggono e scrivono **in silenzio** la fisica `0xC8xxxx` / `0xEAxxxx`.

Granularità disponibili:

| Meccanismo | Granularità |
|---|---|
| Voce della tabella | 16 MiB (un'intera finestra 0x7F) |
| Protezione host (`svcSetMemoryPermission`, §11) | 4 KiB |

### Opzione A: voce di tabella dedicata verso una zona senza backing

`rex_guest_table[0x7F]` punta a un buco di 16 MiB riservato e mai mappato, per esempio 16 MiB in più nella prenotazione dell'arena senza commit. Ogni accesso a `0x7F…` va in fault. Il gestore:

- per un range MMIO chiama il callback, come su desktop;
- per il resto della finestra decodifica l'istruzione (`TryDecodeLoadStore` esiste già), esegue il load/store sulla fisica `addr & 0xFFFFFF` e avanza il PC.

Serve anche:

- `HostToGuest` deve mappare il buco su `0x7F…`, e la ricerca dei range in `MMIOHandler` deve coprirlo;
- su NX `REX_MMIO_FALLBACK_HOST` degli `REX_MM_*` deve tradurre `0x7F…` direttamente sulla fisica, altrimenti anche il percorso riconosciuto andrebbe in fault.

| Pro | Contro |
|---|---|
| Stessa semantica del desktop. La fisica `0xC8xxxx` resta memoria normale dalle altre finestre. | Ogni accesso al writeback tramite `0x7F` costa un fault (T9 misura il giro). Il gestore deve emulare load/store non MMIO: più codice nel percorso d'eccezione. Il fallback degli `REX_MM_*` cambia nel template. |

### Opzione B: proteggere solo le pagine MMIO della fisica condivisa (consigliata)

NoAccess a granularità 4 KiB su fisica `0x00C80000–0x00C8FFFF` e `0x00EA0000–0x00EAFFFF`. Si fa in `AddVirtualMappedRange` su NX, con `rex::memory::Protect` sulla pagina host di `physical_membase_`.

Nel gestore:

- un fault in quelle pagine da qualunque finestra è MMIO: si converte host → `0x7F000000 + offset fisico` e si cerca il range;
- servono un caso in `HostToGuest` (o nel thunk `HostToGuestVirtual`) e l'estensione della condizione di ricerca range in `MMIOHandler` a quelle pagine fisiche;
- il resto della finestra 0x7F resta tradotto sulla fisica, senza costi.

| Pro | Contro |
|---|---|
| Nessun costo sul writeback. Nessun cambio al generatore o alla tabella. 128 KiB protetti in tutto. | Divergenza dal desktop: `0xA0C8xxxx` / `0xC0C8xxxx` / `0xE0C8…` diventano anch'essi MMIO. |

La divergenza dovrebbe restare teorica: quella fisica è dentro i 16 MiB che l'emulatore si riserva all'init, quindi il gioco non la riceve da un'allocazione. Il rischio concreto è il codice host che copia un intervallo fisico che include quelle pagine: finirebbe nei callback dei registri.

Salvaguardie proposte:

1. mantenere quelle pagine fuori da qualunque allocazione fisica (oggi lo sono già, grazie al commit dei 16 MiB di writeback);
2. nel gestore, accettare come MMIO solo fault con PC nel codice generato e trattare gli altri come errore con log;
3. un controllo di debug in `TranslatePhysical` / `TranslateVirtual` per quegli intervalli.

### Raccomandazione

Partire da **B**: costo nullo sul writeback, granularità sufficiente, nessun cambio al codice generato. Passare ad **A** solo se i log in gioco mostrano accessi a quelle pagine come memoria tramite 0xA/0xC/0xE, oppure codice host che le tocca.

In entrambi i casi il gestore dovrà prendere il minimo di lock (vedi §10, deadlock).
