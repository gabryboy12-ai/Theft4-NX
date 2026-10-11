# Indice dei run su console

Una riga per run. Ogni run nuovo aggiunge la sua riga **prima** di essere
analizzato (commit del NRO, configurazione, cartella dei log), poi la completa
con l'esito. "Da note" = i log non sono più sul disco e l'esito viene da
`06-game-boot-plan.md` §27; "in partita" = l'utente ha visto Niko muoversi.

Base di riferimento: tag locale `baseline-ingame-c1b2858`, file in
`out/console-runs/baseline-ingame/` (NRO, ELF, `COMMIT`, `game.toml`). Ogni
confronto parte da lì con **una sola** differenza (09 §13).

| Run | Data | NRO (commit) | Configurazione | Esito per avvio | Log |
|---|---|---|---|---|---|
| 20 | 2026-10-10 | `716936b` Vulkan (`run20-build/switch-game-vulkan.nro`) | `nx_gpu = "vulkan"`, `nx_vk_dump_frames = true`; avvio 2 `nx_trace_exports = false`, avvio 3 `nx_mount_cache = false`; GPU 307,2 MHz (nessuna richiesta apm) | 1: fault a 150 s (pc = far = `0x6ac97ff4`); 2: morte senza record a 130 s; 3: **in partita**, 0,4 FPS, vivo fino a 824 s | da note (06 §27, "Run 20") |
| 21 | 2026-10-10 | `a9727e1` | `nx_gpu = "vulkan"` + default del run 21 (GPU 460,8, `nx_nv_wrap`, messenger); 2 `nx_profile = true`; 3 `nx_mount_cache = true` con cache cancellata; 4 Horizon OC (escluso) | 1: fault a 47,6 s (pc = 0); 2: in partita, + a 938 s, heap libero 20 MiB e draw persi da ~650 s; 3: `std::bad_alloc` a 566,8 s; 4: fault a 53,1 s | da note (06 §27, "Run 21: risultati"; erano in `run22/1`–`4`, poi sovrascritti) |
| 22 | 2026-10-10 | `6bc6c24` | bisezione: base `game1.toml` (novità del run 21 spente, gestore lento) + una riga per avvio (`game2`–`game7.toml` in `run22-build/`) | 1, 2, 7: morte senza record (155, 195, 338 s); 3, 6: fault pc = 0 (58, 187 s); 4, 5: PC non allineato (57, 122 s); nessuno in partita | da note (06 §27, "Run 22: risultati") |
| 23 | 2026-10-11 | `9fe69f7` (+ smoke) | `nx_gpu = "vulkan"`, `nx_mount_cache = false`, `nx_crash_log_after_setup = false` (gestore lento), D.1/D.2 | 1–5: morte senza record a 92, 140, 275, 298, 535 s (1–2 nel caricamento, 3–5 nel filmato); smoke T10 fase B: 2 fault su 2.219 con q0–q7/q16–q31 a zero | da note (06 §27, "Run 23: risultati"; la cartella `run23/` non c'è più) |
| 24 | 2026-10-11 | gruppo 1: `716936b` (NRO del run 20); gruppo 2: `c1b2858` | gruppo 1: `nx_gpu = "vulkan"`, `nx_vk_dump_frames = true`, `nx_mount_cache = false`; gruppo 2: `nx_gpu = "vulkan"`, `nx_mount_cache = false`, `nx_stack_watch = true` (resto default: GPU 460,8, `nx_nv_wrap`, messenger) | gruppo 1: 1, 2, 4 morte senza record (180, 202, 270 s), 3, 5 PC non allineato (101, 306 s), nessuno in partita; gruppo 2: 1, 4 **in partita**, + a 389 e 338 s; 2, 5 `std::bad_alloc` (memoria esaurita) a 682 e 587 s; 3 draw persi per memoria esaurita a ~600 s, poi fermo, + a 1681 s | `out/console-runs/run24/gruppo1/1`–`5`, `gruppo2/1`–`5` |
| 25 | 2026-10-11 | `c1b2858` (`run25-build/`, uguale alla base) + smoke `7ad61e4` | base + `nx_apm_clock = false` (GPU 307,2 MHz) | 1: in partita a piedi da ~190 s, + a 243 s; 2: filmato intero fino a ~660 s, in partita in macchina, + a 855 s con 21 MiB liberi; 3: in partita a piedi da ~270 s, + a 365 s; 4: `abort` per allocazione fallita in Mesa (Rust) alla fine del filmato, 768 s, 13 MiB liberi; 5: in partita a piedi da ~250 s, + a 273 s; nessuna morte senza record; smoke: vettori persi anche con il trampolino | `out/console-runs/run25/1`–`5`, smoke in `run25/smoke.log` |
| 26 | (da fare) | build nuova sopra `c1b2858` (contabilità `mem:`, cuscinetto, pressioni trattenute; `run26-build/`) | base + `nx_apm_clock = true` (default), 3 avvii fino a 15 minuti di partita | – | `out/console-runs/run26/1`–`3` |
