# Implementation.md — Palimpsest (working title; repo renamed at v0.5)

> Owner-facing plan for verification and validation only.
> Format name stays **EDOC**; editor codename **Palimpsest** applied at the W5 release tag.
> Pace assumption: ~15 hrs/week, ~5 weeks total.
>
> Status legend: `[ ]` pending · `[~]` in progress · `[x]` done · `[!]` blocked/deviation (note why)

---

## Goal statement

Turn the existing text-editor into **Palimpsest — a crash-safe code editor in C where the
file format is the database**: self-versioning container, WAL journaling, checksummed
everything, time-travel replay, fuzzed parsers, measured performance.
Differentiator axis: systems engineering depth (not AI/ML, not feature breadth).

---

## Locked decisions

| Decision | Choice |
|----------|--------|
| Scope | P1 Time-travel + P2 EDOC toolkit/spec + P4 rigor track |
| Deferred | LSP client (next epic), tree-sitter, tabs, soft-wrap, full config system |
| Op-log storage | Sidecar `<file>.timeline`, append-only, reuses journal record machinery |
| Naming | Repo/product → Palimpsest at W5 tag; `.edoc` format unchanged everywhere |
| Replay primitives | Existing `raw_insert/raw_delete/raw_split/raw_join` on a scratch LineBuf |

---

## W0 — Foundation & hygiene (~half week)

### Tasks
- [x] Makefile: add `-MMD -MP` to compile rules and `-include $(DEPS)` so header edits rebuild dependents
- [x] Makefile: fix `debug:` target so `-fsanitize=address,undefined -O0 -g` actually reach `CFLAGS`
      (root cause: `CFLAGS := …$(OPT)…$(SANITIZE)` expands before target-specific vars land;
      fix: recursive `CFLAGS =`/`LDFLAGS =`)
- [x] Add `.gitignore`: `build/`, `*.edoc`, `*.edoc.*` (journal/autosave/bak artifacts), `untitled*`
      (tracked artifacts were also untracked via `git rm --cached`; local files kept)
- [x] CI workflow `.github/workflows/ci.yml`: matrix {gcc, clang} × {O2, ASan+UBSan};
      build with `-Werror`; run `make test`
- [x] Friction fix A: `Ctrl+S` saves directly when filename is set (not `untitled.edoc`);
      prompt only for unnamed new files. Bonus: `Ctrl+Shift+S` opens Save-As
- [x] Friction fix B: ignore mouse clicks while in MODE_QUIT_PROMPT / MODE_VERSION_BROWSER /
      MODE_VERSION_RENAME (extend skip list at the SDL_MOUSEBUTTONDOWN handler)
- [x] Friction fix C: window close while recovery prompt unanswered counts as unsaved
      (`rec_available` added to the quit-guard)

### Validation (owner runs)
```
touch include/storage.h && make        # must recompile storage.c AND its dependents
make debug && ldd build/editor          # sanitizer flags visible in compile log, not just link
git status --short                      # no *.edoc*/build noise after running ./build/editor briefly
```
CI green on push (both compilers, sanitizers). Manual: Ctrl+S = single keypress save;
click-drag during quit dialog does not move cursor; open file with stale `.journal`,
close window → quit prompt appears.

> Implemented-and-verified locally (W0): header-touch rebuild = exactly the 3 objects
> including storage.h; `make debug` shows `-O0 -g … -fsanitize=address,undefined` on
> compile AND link lines; gcc + clang both build clean with `-Wall -Wextra -Werror`
> **and `-D_FORTIFY_SOURCE=2`** (Ubuntu/CI toolchains enable fortify by default —
> this caught a `-Wformat-truncation` in editor_do_save's error path; fixed with
> `%.400s` caps in save/load diagnostics); `make test` = 0 failures. CI-on-push and
> the three manual GUI checks remain for owner.

---

## W1 — P4a: fuzzing the parsers (~1 week)

### Tasks
- [ ] Portable dumb-fuzzer target in-tree: `src/fuzz_harness.c` + `make fuzz N=100000 SEED=…`
      feeding random/mutated bytes into:
      `parse_edoc_image`, `journal_find_last_valid`, `parse_history_section`
- [ ] Expose parsers to harness without changing behavior (compile-time shim, e.g.
      `-DSTORAGE_FUZZING` guarded non-static wrappers inside storage.c)
- [ ] Optional libFuzzer path auto-detected when clang present (`-fsanitize=fuzzer`)
- [ ] Fix every finding; each gets a regression test in `src/test_storage.c`
- [ ] Record exec counts + findings summary (goes into README in W5)

### Validation
```
make fuzz N=1000000 SEED=42     # exits 0, prints exec count + coverage-ish stats
make test                        # suite green incl. new regression tests
```

---

## W2 — P2: EDOC toolkit & specification (~1 week)

### Tasks
- [ ] `docs/EDOC_SPEC.md`: byte-layout diagrams; field tables; endianness policy;
      CRC coverage map (which bytes each CRC protects); atomicity contract
      (tmp→fsync→rename→dir-fsync); WAL design incl. torn-record backward-scan algorithm;
      schema-version migration policy (v1 today, how v2 would land)
- [ ] Read-only introspection API in storage (no side effects: no journal fd creation,
      no tmp sweep): `storage_inspect_file()` returning section table
- [ ] CLI `src/edoc_cli.c` → `build/edoc` (links storage.o only):
      `verify FILE` · `dump FILE` · `history FILE` · `export FILE [-v N|name] OUT`
      · `import TXT FILE` · `recover FILE`
- [ ] Roundtrip property test: export→import→verify byte-equal
      (cases: empty doc, unicode, large ~10 MB)

### Validation
```
./build/edoc verify untitled.edoc            # exit 0 + per-section report
./build/edoc dump untitled.edoc              # section table w/ lengths + CRC status
./build/edoc history <any saved file>        # id/name/date/size rows
./build/edoc export f.edoc /tmp/out.txt && diff <(original bytes) /tmp/out.txt   # equal
printf 'hello' > /tmp/t.txt && ./build/edoc import /tmp/t.txt /tmp/t.edoc \
  && ./build/edoc verify /tmp/t.edoc         # exit 0
make test                                     # roundtrip tests green
```

---

## W3–W4 — P1: time-travel timeline ⭐ (~2 weeks)

### Design constants
| Item | Value |
|------|-------|
| Sidecar path | `<file>.timeline` next to the document |
| Record framing | length-prefix u64 + payload + trailing u64 len + record CRC32 (mirrors journal) |
| Record body | magic, op-kind, pos(row,col), cursor_before, cursor_after, payload len+bytes, timestamp_ms |
| Checkpoints | full snapshot record every 256 ops → any scrub point ≤ O(256) replay |
| Capture points | the five `editor_push_*` functions in src/editor.c (single choke point) |
| Base state | document as opened (session start); restored/recovery states start new segments |

### Tasks
- [ ] Generalize journal writer/scanner into shared record helpers; timeline module `src/timeline.c`
- [ ] Write-through hooks in all five `editor_push_*` call sites
- [ ] Reader/replayer: load sidecar → materialize state at op index k onto scratch LineBuf
- [ ] Checkpoint records + nearest-checkpoint seek
- [ ] UI overlay `MODE_TIMELINE`: scrubber bar; ←/→ step ops; PgUp/PgDn ×100;
      mouse drag; dimmed preview + `t=k/N` badge; Enter restores as unsaved edit
      (same undoable snapshot-push flow as version restore); Esc closes
- [ ] Torn-tail tolerance (reuse backward-scan semantics; last valid record wins)
- [ ] Determinism property test: scripted random edit session applied live ≡ replayed-from-timeline
      serialization, checked at every index k
- [ ] Cap/grooming: refuse unbounded growth (rotate or stop appending past threshold; log once)

### Validation
```
make test                                   # determinism + torn-tail tests green
./build/editor demo.txt                     # manual script below
```
Manual script: type sentences → Enter splits → delete words → paste block → Ctrl+S mid-way
→ keep editing → Ctrl+Alt+V style entry into timeline (binding finalized in-code):
scrub to t=0 shows initial state; scrub to latest matches live buffer byte-for-byte;
Enter at mid-point restores; Ctrl+Z after restore returns pre-restore state;
kill -9 the editor mid-session → reopen → timeline intact, scrubbing works.
GIF captured for README.

---

## W5 — P4b: numbers, story, release (~1 week)

### Tasks
- [ ] Bench harness `make bench` (malloc/free counters compiled behind a flag; assert
      zero steady-state frame allocations after warmup):
      startup→first-frame ms · open 10/50/100 MB synthetic EDOC ms · typing latency p99
      · glyph-cache hit rate
- [ ] Results + methodology committed (`docs/BENCHMARKS.md`), summarized in README table
- [ ] README overhaul: positioning line; hero GIF (timeline scrubber); benchmark table;
      fuzz stats; module diagram; CI badges
- [ ] Article drafts under `docs/articles/`: (1) self-versioning file format design,
      (2) crash-testing a WAL with torn writes, (3) zero-allocation rendering in C/SDL
- [ ] Rename: GitHub repo → `palimpsest` (old URLs auto-redirect), binary/title updates,
      spec keeps EDOC identity
- [ ] Tag `v0.5`

### Validation
```
make bench        # prints table; allocation counter asserts 0/frame post-warmup
```
README renders correctly on GitHub (images, badges); fresh clone → `make && make test &&
make bench` succeeds from clean checkout.

---

## Global acceptance gates (checked every milestone)

- [ ] `make clean && make` warning-free under `-Wall -Wextra`
- [ ] `make test` green under ASan/UBSan
- [ ] No regression in existing behaviors: undo/redo, autosave/journal recovery,
      version browser, formatter pipeline (spot-check per W0 checklist habits)
- [ ] Dogfood note: one real editing session per week using only Palimpsest; friction logged
      (feeds future epics, not this scope)

## Explicitly out of scope this cycle

LSP client · tree-sitter · tabs/buffers · soft-wrap · themes/config system ·
any AI/chatbot feature · new languages in the tokenizer (beyond what exists).
