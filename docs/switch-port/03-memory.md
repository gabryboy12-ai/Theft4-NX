# Switch port — 03: memoria guest (analisi)

Aggiornato: 2026-09-24 · Ramo `switch-port` · Solo analisi: in questo passo non è stato implementato niente nel runtime.

Legenda: **[codice]** = verificato nei sorgenti del repo. **[HW]** = comportamento del kernel Horizon da confermare sulla console, con `switch-smoke` o con un test mirato.

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

## 6. Test sulla console: `switch-smoke` T1–T6

Dopo i passi 1–4 e il verdetto `SMOKE OK/FAIL`, `switch-smoke.nro` esegue sei sonde (`tools/switch-smoke/probes.cpp`). Sono **misure**, non test con esito positivo o negativo, e non cambiano il verdetto.

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
