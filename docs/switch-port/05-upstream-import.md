# 05 – Importare gli aggiornamenti da upstream

Theft4-NX (`github.com/gabryboy12-ai/Theft4-NX`) non condivide la storia di
upstream ([KoreanSeats1/Theft4](https://github.com/KoreanSeats1/Theft4)): il
primo commit, `bd7eb4c` (tag `upstream-6ba83faa`), è una **fotografia filtrata**
di upstream al commit `6ba83faa`, senza i file derivati dal gioco e senza gli
strumenti di terze parti inclusi nel repository. Il lavoro Switch sta sopra.

Ogni aggiornamento da upstream ripete la stessa operazione: nuova fotografia
filtrata, commit con il precedente import come genitore, merge in `main`. Così
la base del merge è sempre l'ultimo import e git applica solo le modifiche di
upstream, senza mai vedere i percorsi esclusi.

## Percorsi esclusi

| Percorso | Contenuto |
|---|---|
| `LibertyRecompLib/aes_key.bin` | chiave |
| `LibertyRecompLib/private/*` (tranne `.gitignore`) | pulsanti e texture del gioco (`button_prompts/`) |
| `LibertyRecompLib/shader/*_cache.cpp` | shader del gioco (`shader_cache.cpp`, `postprocess_cache.cpp`) |
| `LibertyRecompLib/font_atlases/` | font del gioco |
| `LibertyRecomp/res/ps4/` | risorse PS4 |
| `tools/xtd_tools/` | texture estratte e convertite |
| `tools/ghidra_projects/` | progetto Ghidra dell'eseguibile |
| `tools/ghidra_*/` | Ghidra 11.2.1 incluso (`tools/ghidra_11.2.1_PUBLIC/`) |
| `tools/GTAIV.EFLC.FusionFix-master/` | copia di FusionFix |
| `LibertyRecomp/gpu/shader/**/*.spv`, `*.ir`, `*.metallib` | shader compilati (81 file) |
| `glue/rexglue-sdk-main/gta4-recomp/generated/` | C++ tradotto dall'eseguibile, da rigenerare in locale |

Il `.gitignore` alla radice elenca tutti i percorsi tranne `generated/` (sezione "Theft4-NX: paths left
out of the upstream import"), `generated/` ha la sua regola più su. I due
`.gitignore` annidati di `LibertyRecompLib/private/` e `LibertyRecompLib/shader/`
sono ridotti di conseguenza: in git le regole di un `.gitignore` più profondo
vincono su quelle della radice, e i `!button_prompts/`, `!shader_cache.cpp`,
`!postprocess_cache.cpp` di upstream avrebbero riammesso quei file.

Il `.gitignore` protegge solo da `git add` di file non tracciati. **Il filtro che
conta è quello dell'import qui sotto**: un merge diretto di un commit di
upstream riporterebbe dentro tutto.

## Procedura

Una volta sola, nel clone di Theft4-NX:

```sh
git remote add upstream https://github.com/KoreanSeats1/Theft4.git
git config remote.upstream.tagOpt --no-tags     # i tag di upstream non entrano
git config remote.upstream.pushurl DISABLED     # impossibile fare push verso upstream
```

Per ogni aggiornamento (shell MSYS2 o qualunque bash, dalla radice del repo):

```sh
git fetch upstream
NEW=$(git rev-parse upstream/main)
SHORT=$(git rev-parse --short=8 "$NEW")
PREV=$(git describe --tags --abbrev=0 --match 'upstream-*' main)   # ultimo import

# 1. Fotografia filtrata in un indice temporaneo (l'indice e il working tree
#    del repo non vengono toccati).
export GIT_INDEX_FILE="$(git rev-parse --git-dir)/upstream-import-index"
rm -f "$GIT_INDEX_FILE"
git read-tree "$NEW"
git rm --cached -r -q --ignore-unmatch -- \
    LibertyRecompLib/aes_key.bin \
    'LibertyRecompLib/private/*' ':!LibertyRecompLib/private/.gitignore' \
    'LibertyRecompLib/shader/*_cache.cpp' \
    LibertyRecompLib/font_atlases \
    LibertyRecomp/res/ps4 \
    tools/xtd_tools \
    tools/ghidra_projects \
    'tools/ghidra_*' \
    tools/GTAIV.EFLC.FusionFix-master \
    'LibertyRecomp/gpu/shader/*.spv' \
    'LibertyRecomp/gpu/shader/*.ir' \
    'LibertyRecomp/gpu/shader/*.metallib' \
    glue/rexglue-sdk-main/gta4-recomp/generated
TREE=$(git write-tree)
rm -f "$GIT_INDEX_FILE"; unset GIT_INDEX_FILE

# 2. Commit d'import sopra il precedente, e tag.
IMPORT=$(git commit-tree "$TREE" -p "$PREV" \
    -m "Import Theft4 $SHORT (KoreanSeats1/Theft4) without game-derived files and bundled tools")
git tag "upstream-$SHORT" "$IMPORT"

# 3. Merge in main.
git switch main
git merge --no-ff "upstream-$SHORT" -m "Merge upstream Theft4 $SHORT"
```

Nei pathspec di `git rm` senza magia `glob` l'asterisco attraversa anche le
`/`: `'LibertyRecomp/gpu/shader/*.spv'` prende i file in tutte le
sottocartelle, `'tools/ghidra_*'` prende tutto il contenuto di
`tools/ghidra_11.2.1_PUBLIC/`.

### Verifiche prima del push

```sh
# Nessun percorso escluso nel nuovo import (deve stampare 0).
git ls-tree -r --name-only "upstream-$SHORT" | grep -cE \
  '^(LibertyRecompLib/aes_key\.bin|LibertyRecompLib/private/[^.]|LibertyRecompLib/shader/.*_cache\.cpp|LibertyRecompLib/font_atlases/|LibertyRecomp/res/ps4/|tools/xtd_tools/|tools/ghidra_|tools/GTAIV\.EFLC\.FusionFix-master/|glue/rexglue-sdk-main/gta4-recomp/generated/)|^LibertyRecomp/gpu/shader/.*\.(spv|ir|metallib)$'

# Cosa porta l'aggiornamento.
git diff --stat "$PREV" "upstream-$SHORT"

# Percorsi nuovi di upstream: se uno è derivato dal gioco o è uno strumento
# incluso, va aggiunto all'elenco qui sopra e al .gitignore, poi l'import va
# rifatto (git tag -d upstream-$SHORT e ripetere dal punto 1).
git diff --name-only --diff-filter=A "$PREV" "upstream-$SHORT" | cut -d/ -f1-3 | sort -u
```

Poi rifare la build Switch (SDK e `switch-smoke`, vedi `README-SWITCH.md`).
Il push va fatto con il tag: `git push origin main "upstream-$SHORT"`. Mai
`git push --all`/`--mirror`: i ref `upstream/*` puntano a commit che
contengono i file esclusi.

### Conflitti attesi

- `.gitignore` alla radice: tenere le righe di upstream **e** la sezione
  Theft4-NX in fondo.
- `LibertyRecompLib/private/.gitignore`, `LibertyRecompLib/shader/.gitignore`:
  tenere la versione di Theft4-NX (senza le negazioni) e riportare a mano solo
  eventuali regole nuove che non riammettano file esclusi.
- File dell'SDK toccati anche dal port Switch (`glue/rexglue-sdk-main/...`):
  risolvere normalmente, poi ricompilare.

## Nota sulla verifica della procedura

La procedura è stata provata su `6ba83faa` stesso: il tree prodotto dal punto 1
è `b9db0bdd2b1ff706917bcc4d41904b4008afb699`, identico al tree di `bd7eb4c`
(`upstream-6ba83faa`). L'elenco dei percorsi esclusi è quindi completo per
quell'import.

`git fetch upstream` scarica nel repository locale anche i blob esclusi. Non
finiscono su `origin` finché non viene fatto il push di un ref che li
contiene: `main` e i tag `upstream-*` non li contengono.
