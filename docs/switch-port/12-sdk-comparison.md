# 12 – Il nostro SDK e quello di sa2-nx (2026-10-09)

Solo analisi, nessuna modifica al codice. Riferimenti clonati in `<refs>/`:
`sa2-nx` (ChanseyIsTheBest, `fdd6f14`, 2026-10-08, versione 1.0.0 del port),
`nfsmw-nx` (aggiornato, `4e3ffc2`), ReXGlue upstream (`rexglue/rexglue-sdk`,
con i tag, `rexglue-sdk-upstream` a `c94f5eb` più un worktree `v0.9.0`).
I numeri vengono da uno script che confronta i file uno a uno (righe con
`difflib`), escluse `thirdparty/` e le cartelle di build.

## 1. Che versione è il nostro SDK

- **Il nostro è ReXGlue v0.9.0.** `glue/rexglue-sdk-main` all'import di
  Theft4 (`bd7eb4c`) ha 1.686 file fuori da `gta4-recomp/`; il commit di
  upstream con più file identici (stesso hash git) è il tag **`v0.9.0`
  (`3eb9b51`, 2026-07-22)**: 1.060 file uguali (pari merito con il commit
  successivo `96bee61`). `v0.8.0+4` ne ha 970, `v0.10.0` 948. Lo dicono anche
  `CMakeLists.txt` (`project(ReXGlue VERSION 0.9.0)`) e
  `gta4-recomp/gta4_manifest.toml` (`sdk_version = "0.9.0"`). La "v0.2.1"
  della storia di Theft4 non è la versione dell'SDK (upstream ha i tag da
  `v0.2.1` a `v0.10.0`; probabilmente è il primo SDK usato da Theft4).
- **"0.10" di sa2-nx è lo stesso upstream**, un passo dopo: fork di ReXGlue
  **v0.10.0, commit `c94f5eb`** (2026-08-21, `sdk/README.md`). Le numerazioni
  sono confrontabili: stessa storia, stessi tag. Fra `v0.9.0` e `v0.10.0`
  upstream ci sono 63 commit: 180 file cambiati (−3.207/+4.621 righe), 17 tolti,
  34 nuovi.
- Fra quei 63 commit ce ne sono tre che toccano bug che abbiamo trovato da soli:
  `0bd1fd5` "select the POSIX access mode instead of ORing O_RDONLY and
  O_WRONLY" (la nostra correzione O_RDWR del run 17, 06 §23), `bc34338`
  "report the guest arena base and unhandled access violations", `0e4e996`
  "drain the audio worker thread before falling back to terminating it".
  Più `b0e92d0` "cut per-open cost in GetChild and the lazy path resolve"
  (il nostro problema delle aperture lente sulla SD).

Chi sta sopra upstream:

| Fork | Base | File modificati | File nuovi |
|---|---|---|---|
| Nostro (Theft4 + Switch) | v0.9.0 | 207 (−2.915/+17.596 righe) | 294 (97.011 righe; 66.919 sono `graphics/`, quasi tutto `gta4_native`) |
| sa2-nx (masseffect-nx + nfsmw-nx + sa2) | v0.10.0+2 | 122 (−780/+8.509) | 53 (14.070) |

Il fork di sa2-nx è piccolo e concentrato; il nostro porta con sé il renderer
nativo di Theft4 (`gta4_native`, 67 mila righe) e le aggiunte del gioco
(input mouse e tastiera, Live, achievement).

## 2. Distanza per area (nostro HEAD contro `sa2-nx/sdk`)

Escluso `gta4_native` dal nostro lato (non ha controparte). "Diversi": file
presenti in tutti e due ma con contenuto diverso, con le righe tolte/aggiunte
per passare dal nostro al loro.

| Area | Uguali | Diversi (righe −nostre/+loro) | Solo nostri (righe) | Solo loro (righe) |
|---|---|---|---|---|
| Memoria | 17 | 16 (−1.745/+1.216) | 6 (1.817) | 2 (960) |
| Thread | 15 | 14 (−1.858/+1.526) | 7 (3.500) | 1 (187) |
| Eccezioni | 7 | 4 (−485/+1.424) | 1 (487) | 1 (114) |
| Audio | 9 | 15 (−958/+1.934) | 21 (3.787) | 2 (123) |
| Input | 1 | 14 (−2.261/+2.373) | 20 (2.765) | 6 (344) |
| Grafica | 287 | 44 (−4.144/+945) | 16 (4.116) | 4 (2.673) |
| Interfaccia (`ui/`) | 69 | 55 (−3.453/+2.695) | 18 (816) | 17 (4.131) |
| Generatore PPC (`codegen`, `ppc`, template) | 56 | 44 (−1.128/+1.427) | 5 (1.024) | 14 (2.415) |
| File system | 25 | 21 (−1.329/+2.120) | 7 (1.518) | 8 (2.473) |
| Kernel HLE | 52 | 20 (−3.485/+853) | 7 (821) | 0 |
| Build (CMake) | 3 | 7 (−419/+256) | 0 | 7 (269) |
| Altro | 100 | 56 (−2.111/+1.471) | 41 (10.166) | 8 (926) |
| **Totale** | **641** | **310 (−23.376/+18.240)** | **149 (30.817)** | **70 (14.615)** |

Lettura: un terzo dei file condivisi è diverso, e la differenza è fatta da
due cose sommate: v0.9.0 → v0.10.0 (≈ 4.600 righe) e i due fork
(il nostro ≈ 17.600 righe cambiate, il loro ≈ 8.500). Le aree più lontane sono
l'input (quasi ogni file diverso: noi abbiamo mouse/tastiera e giroscopio di
Theft4, loro HID con Joy-Con), l'interfaccia (loro: finestra, contesto,
SaltyNX, apm, sysclk, perf; noi: la finestra minima `window_nx` del run 18) e
il kernel (le nostre modifiche a xboxkrnl/xam per Theft4 e per il tracciamento
delle export).

## 3. Cosa ha il loro livello Horizon che noi non abbiamo, e viceversa

Il loro livello Horizon è di StevensND (nfsmw-nx), BSD-3 come l'SDK
(`THIRD_PARTY_NOTICES.md` di sa2-nx).

| Pezzo | sa2-nx | Noi |
|---|---|---|
| Chiusura degli handle dei thread (`switch_reaper`) | sì (porting-guide §7.4: senza, gli handle finivano fino al limite di Horizon) | **no**. Dal run 19 il battito conta creati/finiti/joinati e i thread di Horizon contro il limite (06 §25). Il nostro `post_execution` fa il join solo dopo un'attesa riuscita sul thread |
| Asset pack (`game_root.nxpack`, `pack_storage_switch.cpp`) | sì: un file solo, `ReadAt` ripete le letture corte | **no**: file sciolti sulla SD (`host_path`) |
| Letture corte | ripetute nel pack | **da run 19**: `FileHandle::Read/Write` ripetono fino alla fine, con contatore |
| Profiler (`switch_perf.cpp`): CPU per thread dal kernel, clock, contatori IPC, campioni di pila ogni ms | sì | in parte: dal run 19 CPU per thread nel battito; niente campioni di pila (06 §20, da fare con la GPU vera) |
| Scrittore del log (`log_file_writer`, una volta al secondo) | sì | **da run 19** (`nx_log_durable = false` per default) |
| Input con Joy-Con di lato, per giocatore | sì (`switch_input_driver.cpp`) | no: un pad, utente 0 (`nx_input.cpp`) |
| SaltyNX/ReverseNX, dock/portatile, overlay FPS | sì (`switch_saltynx.cpp`, `switch_apm.cpp`, `switch_sysclk.cpp`) | no: finestra fissa 1280×720 |
| Audio vero (`audout`) | sì (`switch_audio_system.cpp`) | no: audio nullo (06 §21) |
| Renderer | nativo (D3D sostituito, `app/src/me`), Vulkan su NVK | ReXGlue `xenos` su NVK, da provare (06 §24) |
| Alias della memoria guest | `guest_memory_switch.cpp` (viste) | **modello A risolto e verificato** (03-memory.md): tabella e `svcMapProcessMemory` |
| Gestore dei fault | `exception_handler_switch.cpp` + `switch_crash_hooks.c` | **verificato sulla console** (run 9–18): log del crash, `svcBreak` sulla pila del thread |
| Tracciamento delle export con confronto col PC | no | **sì**: `export_stubs`, `tools/compare_export_traces.py`, la build PC `pc-eu-base` |
| Istantanee dei thread con pila guest e host | no (campioni di pila del profiler) | sì (`LogThreadSnapshot` di switch-game) |
| Uscita senza crash con `+` | (non documentata) | modo D, run 18 pulito (09 §10) |
| Opzioni del generatore (argomenti in registro, locali, "diet", registrazione a pezzi) | sì (`codegen_flags.*`) | no |

## 4. Quanto codice GTA IV dipende dall'API del nostro SDK

| Parte | Righe | Dipendenze dall'SDK |
|---|---|---|
| `gta4-recomp/src` (hook, app, installer) | 46.387 | 161 usi di `rex::system::`, 126 `rex::ui::`, 97 `rex::diagnostics::`, 80 `rex::input::`, 63 `rex::cvar::`, 55 `rex::filesystem::`, 40 `rex::graphics::`; 2.364 indirizzi `sub_8…`/`0x8…` della v8 USA |
| `gta4_native` (renderer nativo, dentro l'SDK) | 66.791 | tutto il backend Vulkan e `ui/vulkan` dell'SDK |
| `gta4_manifest.toml`, `gta4_config.toml` | — | formato del generatore v0.9.0 (156 indirizzi nel config) |

Header del nostro SDK che il codice GTA IV include e che **nell'SDK di sa2-nx
non esistono**: 35, di cui 17 di `gta4_native` (stanno nell'SDK solo perché
Theft4 ce li ha messi), `diagnostics/{policy,gpu_flight_recorder,gta4_transition}.h`,
`audio/handoff_trace.h`, `crypto/sha256.h`, `graphics/primitive_restart.h`,
sette di `input/` (mouse, motion, trace), cinque di `system/xam/` (Live,
sessioni, arbitraggio). Header che esistono in tutti e due **ma diversi**: 36
(`cvar.h`, `hook.h`, `runtime.h`, `kernel_state.h`, `filesystem/*`,
`ui/vulkan/*`, `input/input_system.h`, `thread.h`, `memory/utils.h`…).

Costo di portare GTA IV sul loro SDK (stima): spostare `gta4_native` e gli
header di Theft4 come modulo a parte (2–3 g), adattare le 36 API diverse
(3–5 g), rigenerare il codice con il loro generatore e le sue opzioni (1 g più
la verifica: il nostro confronto export col PC andrebbe rifatto), riportare
sopra il loro livello Horizon quello che abbiamo verificato noi (memoria A,
fault, tracciamento, uscita D: 3–5 g), riprovare sulla console da capo fino al
punto del run 18 (5–10 run). **Totale 2–4 settimane**, con il rischio di
perdere per strada correzioni verificate.

## 5. Licenze

- **ReXGlue SDK** (nostro e loro): BSD-3-Clause, Tom Clay, con parti di
  Xenia (Ben Vanik e contributori) (`LICENSE` dell'SDK).
- **Livello Horizon di sa2-nx/nfsmw-nx** dentro l'SDK: BSD-3-Clause "come
  parte dell'SDK", dichiarato apposta perché altri port possano riusarlo.
- **sa2-nx** fuori dall'SDK (`app/`, `tools/`, `docs/`, `patches/`):
  GPL-3.0; **nfsmw-nx**: GPL-3.0 (BSD-3 per le parti dell'SDK);
  **masseffect-nx**: GPL-3.0; `XenosRecomp`: MIT; Mesa/NVK: MIT.
- **Theft4** (radice del repository, `COPYING`): GPL-3.0.
- Conseguenza: i pezzi del loro SDK (reaper, pack, profiler, log writer,
  input) si possono copiare nel nostro SDK mantenendo la BSD-3 e gli avvisi;
  gli script e il codice di `app/` (GPL-3.0) si possono usare in Theft4, che è
  già GPL-3.0, citandone l'origine.

## 6. Raccomandazione

**Restare sul nostro SDK e portare i loro pezzi uno a uno**, in quest'ordine
(ognuno con una cvar e una misura sulla console):

1. letture: asset pack o almeno la cache di lettura (il caricamento del run 18
   legge 25 KB alla volta), 2–3 g — **superato dal run 19: vedi §9, il pack
   non serve per GTA IV**;
2. reaper degli handle, se il contatore del run 19 mostra handle che restano
   aperti, 0,5 g;
3. profiler con campioni di pila (`switch_perf.cpp`), quando c'è la GPU vera,
   1–2 g;
4. input con Joy-Con, SaltyNX/dock, audio `audout`: quando il gioco si vede,
   2–4 g;
5. dai 63 commit upstream v0.9.0 → v0.10.0, prendere quelli che correggono
   (filesystem, audio, eccezioni), 1–2 g; i 4 già fatti a modo nostro restano.

Totale **≈ 1,5–2,5 settimane**, distribuite, senza fermare i run. Il
ribasamento costerebbe 2–4 settimane in blocco (§4) e butterebbe via la
verifica fatta in 18 run (memoria A, fault, uscita, tracciamento con
confronto PC), che sul loro SDK andrebbe rifatta. Da riconsiderare solo se il
renderer di ReXGlue (`xenos`) si rivela inutilizzabile e serve il loro
renderer nativo: lì il ribasamento porterebbe anche `app/src/me`, ma il loro
renderer è scritto per il D3D di un altro gioco (vedi 10 §"masseffect-nx") e
Theft4 ha già il suo (`gta4_native`).

## 7. LibertyRecompAndroid (vaduur): GTA IV a 30 FPS su Snapdragon 865

Fonte: `docs/android/PORTING.md`, `ADVANCED.md` e, per gli indirizzi, i
sorgenti in `gta4-recomp/src/android/` del loro fork. Gli indirizzi sono della
**v8 USA** (0.0.8.5), non del nostro eseguibile europeo di base.

- **Attesa del fotogramma** (`android_frame_wait_hook.cpp`, cvar
  `gta4_frame_wait_sleep`, spenta): `sub_82A46D70(device r3, out r4)` scrive
  `{0, fotogramma inviato, inviati − completati}`; il suo unico chiamante,
  `sub_828BF420`, la interroga in un ciclo senza pausa finché due fotogrammi
  sono in sospeso (≈ 7 % dei campioni del thread principale). Il hook dorme
  250 µs quando il gioco sta per girare a vuoto. `gta4_extra_frames_in_flight`
  (0–2) riporta meno fotogrammi in sospeso per lasciare il gioco più avanti.
  **Spento per default**: nessun guadagno misurabile, e il crash in
  `sub_8296E480` è comparso nelle build che lo avevano.
- **Fissaggio dei thread** (`android_thread_pinning.cpp`, `gta4_thread_pinning`
  0–4, spento): ogni 2 s classifica i thread per tempo di CPU
  (`/proc/self/task/*/stat`) e dà ai primi N un core grande ciascuno. Spento
  perché cambiare i tempi del gioco provoca la corsa qui sotto.
- **Corsa del gioco** (`android_stale_object_guard.cpp`, acceso): `sub_8296E480`
  e `sub_8296E310` copiano la matrice di un oggetto che un altro thread può
  liberare nello stesso momento; la guardia controlla che la catena di
  puntatori porti a memoria mappata e altrimenti prende il ramo "nessun
  oggetto" del gioco.
- **Preset** (`ADVANCED.md`): 720p nativo (25–29 FPS nel punto peggiore, 30
  altrove), 1080p con FSR 1 da 720p, 1080p nativo (20–25), "veloce"
  (FSR performance, niente AA, distanza dinamica fino a 0,55), "mondo come la
  console" (densità 1,0).
- **Impostazioni che tolgono CPU:** densità di traffico, auto parcheggiate,
  pedoni e pedoni di scenario a **0,4** (PC 1,25, console 1,0); distanza di
  disegno dinamica (giù fino a 0,75 quando si perdono i 30 FPS); "fps guard"
  sui riflessi. Mai scendere sotto 1,0 con l'ombra (le ombre di auto e pedoni
  affondano) e mai toccare `gta4_shadow_cascade_count`.
- **Shader saltato:** pixel shader `2673E2AF` (volume di luce differito),
  blocca la GPU vicino all'acqua sotto Turnip; `gta4_native_skip_pixel_shaders`.
  Più un watchdog sui cicli SPIR-V e attese di fence limitate. È un bug del
  driver di Adreno: su NVK va provato prima di saltarlo.
- **Atomiche LSE:** la build Android usa `-march=armv8.2-a` per le atomiche LSE
  in linea (invece delle chiamate "outline-atomics") e `-fno-emulated-tls` per
  il TLS ELF nativo, "caldi sul thread guest". **L'A57 della Switch non ha LSE:**
  da quella build non si può prendere niente che assuma `-march=armv8.2-a`; il
  nostro codice usa già LL/SC (`ldxr/stxr`, si vede in `ExportEnter`) e, in
  export_stubs.cpp, store semplici per i contatori a scrittore unico. Il loro
  TLS nativo non esiste su devkitA64 (`__aarch64_read_tp`, 06 §25).
- **Altro utile:** audio che gracchia sotto carico perché le priorità dei thread
  audio non arrivano ad Android (`nice -16` a "Audio Worker" e "XMA Decoder");
  vblank pilotato dal vsync reale; `WaitMultiple` che dorme invece di girare a
  vuoto.

## 8. Versione della TU: accettare la PAL 0.0.8.6 sul disco europeo

Dove è codificata (senza scaricare niente):

| Punto | Dove | Cosa controlla |
|---|---|---|
| Installer di Theft4 (gta4-recomp) | `gta4-recomp/src/install/gta4_installer.cpp`: `kRequiredTargetVersion = 0x00000805`, `kRequiredBaseXexXxh3`, `kRequiredPatchSha256`, `kEmbeddedTargetXexSha256`; `ValidatePatch` | la `.xexp` deve avere `delta_target_version` 0.0.8.5, `delta_source_version` = versione del `default.xex`, SHA-256 della patch fissa, digest della sorgente = SHA-1 della firma RSA del `default.xex`; il `default.xex` deve avere l'XXH3 della USA retail 1.00 ("does not match GTA IV USA retail 1.00") |
| Ispettore delle sorgenti | `gta4-recomp/src/install/gta4_source_inspector.cpp`: stesso `kRequiredBaseXexXxh3` | il disco USA |
| Installer di LibertyRecomp (vecchio) | `LibertyRecomp/install/hashes/title_update.cpp` (v4, v5, v6, v8 per dimensione, hash 0 = non controllato), `installer.cpp::detectUpdateVersion` (dal nome del file) | debole, non blocca la PAL |
| Media ID | `LibertyRecomp/install/installer.cpp` (intestazione STFS) | solo il tipo di DLC |
| Codice generato | `gta4-recomp/gta4_manifest.toml` (`assets/default_v8.xex`), `gta4_config.toml` (156 indirizzi), `gta4-recomp/src` (2.364 indirizzi) | tutto è della v8 USA |

Il formato della versione è quello dell'XEX (`major.minor.build.qfe` →
`0x00000805` per 0.0.8.5): la PAL 0.0.8.6 è `0x00000806`, la nostra base
europea 0.0.0.6 è `0x00000006`.

Cosa servirebbe per accettare la PAL 0.0.8.6 sul nostro disco europeo:

1. **Installer** (poco): una seconda tabella di costanti "EU" (`0x00000806`,
   SHA-256 della `.xexp` PAL, XXH3 del `default.xex` europeo base,
   SHA-256 dell'XEX risultante) e la scelta della tabella dal titolo/regione
   del disco; i messaggi d'errore oggi dicono "USA retail 1.00". 0,5–1 g.
2. **Codice generato** (molto): il codice C++ è tradotto dall'eseguibile; con
   la PAL 0.0.8.6 cambia l'eseguibile e quindi gli indirizzi. Serve una
   configurazione nuova (come `config-eu-base`, che oggi è la base 0.0.0.6
   senza patch, 06 §2), le sue lacune di funzioni da ritrovare con la build PC
   (`tools/pc-eu-base`), e il confronto export da rifare.
3. **Hook e indirizzi** (moltissimo): i 2.364 indirizzi della v8 USA in
   `gta4-recomp/src` e i 156 di `gta4_config.toml` vanno rimappati alla PAL
   (firme di funzione o confronto delle due versioni, una per una). È lo
   stesso lavoro che separa oggi switch-game (base EU, quasi senza hook) dal
   gioco completo di Theft4.

Finché switch-game gira sulla base europea senza patch (06 §2, opzione C),
la PAL 0.0.8.6 non serve; diventa necessaria solo per usare gli hook di
Theft4 sul disco europeo.

**Aggiornamento 2026-10-09:** c'è solo il disco europeo, nessuna TU e nessun
disco USA. Si lavora sulla base europea 0.0.0.6 senza TU (06 §26 C.1): questa
sezione resta come riferimento, non come lavoro da fare.

## 9. I pezzi da portare uno a uno, per valore e costo (dopo il run 19)

Ordine deciso dai numeri del run 19 (06 §26): il caricamento è limitato dalla
copia nella cache e dalle scritture sulla SD, non dalle letture né dagli
handle. Nessuno è implementato.

| # | Pezzo | Cosa risolve | File toccati | Rischio | Verifica |
|---|---|---|---|---|---|
| 1 | Profiler con campioni di pila (`switch_perf.cpp` di sa2-nx, parte CPU) | dice *dove* spendono id 18, 19, 6 (oggi solo quanto) | `tools/switch-game/main.cpp` (thread di campionamento, già c'è la pausa per le istantanee), `tools/switch-game/where.py` | basso: pausa di un thread per volta, come le istantanee | smoke (thread noti che girano in funzioni note) e poi gioco: le funzioni calde di id 18/19 devono stare in `8216xxxx` |
| 2 | Attese che dormono (porting-guide §9.5) | `KeDelayExecutionThread` sotto 1 ms che diventa un yield (06 §26 B.3) | `src/system/xthread.cpp` (`Delay`) | medio: cambia i tempi dei thread del gioco (LibertyRecompAndroid: una corsa del gioco è comparsa cambiandoli) | contatori del run 20 prima; poi A/B sulla console con la CPU di id 19 e VdSwap/s; il PC per l'ordine delle export |
| 3 | Audio vero (`switch_audio_system.cpp`, `audout`) | il gioco ha già il suo mixer (id 18); oggi i frame vanno persi | `src/audio/switch/*`, `tools/switch-game/main.cpp` | medio: sincronizzazione del callback | smoke (tono) e gioco (callback/s a 187,5) |
| 4 | Input con Joy-Con e più giocatori | comodità | `src/input/switch/*` o `tools/switch-game/nx_input.cpp` | basso | gioco (pulsanti nel log) |
| 5 | SaltyNX/dock/display, sysclk/apm | 720p in dock, clock | `src/ui/switch_*.cpp` | basso | gioco con la GPU vera |
| 6 | Correzioni upstream v0.9.0 → v0.10.0 (filesystem, audio, eccezioni) | allineamento; tre sono già fatte a modo nostro | file diversi per commit | basso, uno per uno | confronto delle export col PC (`pc-eu-base`) |
| 7 | Reaper degli handle | non serve oggi: `threads: Horizon 33 of 96`, nessuna crescita in 640 s | `src/core/threading_switch.cpp` | — | il contatore resta nel battito; si porta se cresce |
| 8 | Asset pack (`pack_storage_switch.cpp`) | **non serve per GTA IV**: il pack risolve il costo di aprire migliaia di file piccoli. GTA IV apre ~330 percorsi in tutto, quasi tutti `.rpf`/`.img` grandi, e legge 32 KB alla volta a 1–4 ms; la SD dà 86 MB/s sequenziale e 54 MB/s casuale, il gioco ne usa il 2–4 %. Dal pack resterebbe utile solo il ciclo sulle letture corte, che c'è già (run 19: 0 letture corte) | — | — | — |

Fuori dalla tabella perché non è un pezzo di sa2-nx: la **copia nella cache**
(06 §26 B.4). Prima `nx_mount_cache = false` (run 20); se il gioco salta la
copia e VdSwap resta a 60, non serve altro; se invece il gioco la vuole, la
strada è scrivere meglio (blocchi più grandi raccolti per handle prima della
scrittura sulla SD), da progettare dopo quella prova.
