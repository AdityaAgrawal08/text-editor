# editor

A custom text editor written in C using SDL2 and FreeType.

## Building

### Dependencies

Arch Linux:
```bash
sudo pacman -S sdl2 freetype2
```

Debian/Ubuntu:
```bash
sudo apt install libsdl2-dev libfreetype6-dev
```

### Compile

```bash
make          # optimised build  →  build/editor
make debug    # ASan + UBSan     →  build/editor
make test     # storage tests    →  build/test_storage
```

### Run

```bash
./build/editor                  # new file (untitled.edoc)
./build/editor path/to/file     # open existing file
```

Place a monospace TTF at `assets/font.ttf`. If absent, the editor
falls back to DejaVu Sans Mono at the system font path.

---

## Project layout

```
editor/
├── Makefile
├── include/
│   └── storage.h          # persistence layer API
└── src/
    ├── storage.c           # EDOC format, atomic save, journal, autosave
    ├── editor.c            # everything else
    └── test_storage.c      # 34 storage-layer tests
```

---

## Architecture

### Text buffer — LineBuf + per-line gap buffer

Each line is a **gap buffer** over bytes (`Line`). Lines are stored in a
**LineBuf** — a gap buffer whose elements are `Line*` pointers — so
inserting or deleting a line near the cursor is O(1) amortised rather
than the O(n) `memmove` a flat array requires.

### Glyph cache

Every codepoint is rasterised once by FreeType into a persistent
`SDL_Texture`. Subsequent frames reuse the texture via
`SDL_SetTextureColorMod` for syntax colouring. Per-frame SDL surface
and texture allocation drops to zero for already-seen glyphs.

### Undo / redo — operation ring

A 256-slot circular ring stores typed records (`OP_INSERT`, `OP_DELETE`,
`OP_SPLIT`, `OP_JOIN`). Each record holds only the bytes it touched —
not a full document clone. Multi-line operations (paste, selection
delete, replace-all) push `OP_SNAPSHOT` (serialised flat text). Pushing
a new op truncates the redo tail; redo is always available until a new
edit is made.

### Storage layer

Documents are saved in the **EDOC** container format:

```
[FileHeader]   magic + format version + timestamp
[Section]+     type + length + CRC32 payload  (document, metadata, journal)
[FileFooter]   section count + whole-file CRC32
```

Every save writes to a uniquely-named temp file, fsyncs it, then
`rename()`s it over the destination — a crash mid-write leaves the
previous file untouched. A separate append-only **journal** captures
in-progress state between autosaves (debounced to 800 ms). On open,
the editor checks for a newer journal or autosave and offers to restore
it. Five rotating numbered backups are maintained automatically.

---

## Keyboard reference

### Modes

| Key | Action |
|-----|--------|
| `i` | Enter Insert mode |
| `a` | Enter Insert mode, advance cursor one right |
| `Esc` | Return to Normal mode, clear selection |

### Navigation

| Key | Action |
|-----|--------|
| `Arrow keys` | Move cursor |
| `Ctrl + Left / Right` | Word left / right |
| `Home` | First non-whitespace (press again → column 0) |
| `End` | End of line |
| `Ctrl + Home / End` | Start / end of file |
| `Page Up / Down` | Scroll one page |
| `Ctrl + G` | Go to line (number prompt) |

### Selection

| Key | Action |
|-----|--------|
| `Shift + arrows` | Extend selection |
| `Ctrl + A` | Select all |
| `Ctrl + W` | Select word under cursor |
| Single click | Place cursor, clear selection |
| Double click | Select word |
| Triple click | Select line |
| Click + drag | Extend selection |

### Editing (Insert mode)

| Key | Action |
|-----|--------|
| Type | Insert text |
| `Enter` | New line, preserve indentation |
| `Backspace` | Delete char left (+ `Ctrl` → delete word) |
| `Delete` | Delete char right (+ `Ctrl` → delete word) |
| `Tab` | Insert 4 spaces |
| `Shift + Tab` | Unindent current line |
| `Tab` with selection | Indent selected lines |
| `Shift + Tab` with selection | Unindent selected lines |

### Editing (both modes)

| Key | Action |
|-----|--------|
| `Ctrl + Z` | Undo |
| `Ctrl + Shift + Z` / `Ctrl + Y` | Redo |
| `Ctrl + C` | Copy selection |
| `Ctrl + X` | Cut selection |
| `Ctrl + V` | Paste |
| `Ctrl + D` | Duplicate current line |
| `Ctrl + K` | Kill to end of line |
| `Ctrl + /` | Toggle `// ` line comment |
| `Alt + Up / Down` | Move current line up / down |

### File operations

| Key | Action |
|-----|--------|
| `Ctrl + N` | New file |
| `Ctrl + O` | Open file (path prompt) |
| `Ctrl + S` | Save (filename prompt) |

### Search

| Key | Action |
|-----|--------|
| `Ctrl + F` | Find — all matches highlighted, `Enter` cycles forward, `Shift + Enter` backward |
| `Ctrl + H` | Find & Replace — `Enter` replaces current match, `Ctrl + A` replaces all, `Tab` switches fields |
| `Esc` | Close search / clear highlights |

### Command palette

| Key | Action |
|-----|--------|
| `Ctrl + P` | Open palette |
| Type | Filter commands |
| `Up / Down` | Navigate |
| `Enter` | Execute |
| `Esc` | Close |

Built-in palette commands: Save, Open, Find, Find & Replace, Go to
Line, Toggle Line Comment, Duplicate Line.

---

## File format

Files are saved as `.edoc` (EDOC container). The document body is plain
UTF-8 text — opening a `.edoc` in any text editor shows the raw content
directly. The container adds versioning, metadata, and integrity
checksums around it.

On disk next to `yourfile.edoc`:

| File | Purpose |
|------|---------|
| `yourfile.edoc.journal` | Crash-recovery WAL, cleared on clean save |
| `yourfile.edoc.autosave` | Periodic background save (every ~15 s while dirty) |
| `yourfile.edoc.bak.0` … `.bak.4` | Five most recent explicit-save backups |

---

## Status bar

```
INSERT  path/to/file.edoc [+]      Saved: file.edoc      [3/12]  Ln 42  Col 7
│       │                  │        │                      │        │
mode    filename           dirty    status message         search   position
```

`[+]` appears when there are unsaved changes.
`[n/N]` appears during an active search (current match / total matches).
`~ autosaved` flashes top-right after a background save.
