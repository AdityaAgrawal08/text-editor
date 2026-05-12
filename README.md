# Text Editor

A custom C-based text editor built with SDL2 and FreeType.

## Features

- **Standard Editing**: Insert, delete, and modify text with ease.
- **Undo System**: Use `Ctrl+Z` to undo your last changes (up to 100 steps).
- **File Operations**:
# Custom C Text Editor

A lightweight, premium text editor built with C, SDL2, and FreeType. Featuring syntax highlighting, find and replace, and modal editing.

## Key Features

- **Modal Editing**: Toggle between **View Mode** (navigation) and **Edit Mode** (typing).
- **Syntax Highlighting**: Real-time C/C++ syntax highlighting (Keywords, Types, Strings, Comments).
- **Find and Replace**: Robust search and replace system with a premium UI.
- **Dynamic Gutter**: Auto-adjusting line numbers with precise spacing.
- **File Operations**: Open, Save, Save As, and New File.
- **Undo System**: Full Ctrl+Z support for all operations.
- **Selection**: Shift + Arrow keys to select text, Ctrl+C/Ctrl+V support.

## Keyboard Bindings

### Modes
- `i`: Enter **Edit Mode** (from View Mode).
- `Esc`: Enter **View Mode** (from Edit Mode or Dialogs).

### Navigation (View & Edit Modes)
- `Arrow Keys`: Move cursor.
- `Home` / `End`: Jump to start/end of line.
- `Ctrl + Home` / `Ctrl + End`: Jump to start/end of file.
- `Page Up` / `Page Down`: Scroll page.

### Editing (Edit Mode Only)
- `Typing`: Insert text.
- `Backspace` / `Delete`: Remove characters.
- `Enter`: New line.
- `Tab`: Insert 4 spaces.
- `Ctrl + Z`: Undo.
- `Ctrl + D`: Duplicate current line.
- `Ctrl + /`: Toggle line comment.
- `Alt + Up/Down`: Move current line up/down.

### Selection & Clipboard
- `Shift + Arrows`: Select text.
- `Ctrl + C`: Copy selection.
- `Ctrl + X`: Cut selection.
- `Ctrl + V`: Paste.
- `Ctrl + A`: Select All.

### File & Search
- `Ctrl + N`: New File.
- `Ctrl + O`: Open File.
- `Ctrl + S`: Save File.
- `Ctrl + F`: Find in File.
- `Ctrl + H`: Find and Replace.

## Search/Replace Dialog Controls
- `Enter`: Find Next / Replace.
- `Tab`: Switch between Find and Replace fields (Replace mode only).
- `Esc`: Close dialog and return to View Mode.

## Building and Running

### Prerequisites
- GCC
- SDL2 development libraries
- FreeType development libraries

### Build
```bash
make
```

### Run
```bash
./build/editor [filename]
```
