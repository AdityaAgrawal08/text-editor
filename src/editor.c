#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <SDL2/SDL.h>
#include <SDL_keyboard.h>
#include <SDL_keycode.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage.h"

#define LINE_INITIAL_CAPACITY 64
#define LINES_INITIAL_CAPACITY 16
#define FONT_SIZE_PX 24
#define LINE_HEIGHT_PX 40
#define GUTTER_X 15
#define FIRST_LINE_Y 40
#define CURSOR_BLINK_MS 500
#define VERTICAL_MOVE_RESET_MS 2000
#define CLIPBOARD_MAX (1 << 20)
#define DEFAULT_FILENAME "untitled.edoc"
#define STATUS_BAR_HEIGHT 28
#define UNDO_STACK_SIZE 100
#define JOURNAL_DEBOUNCE_MS 800

typedef enum {
  MODE_VIEW,
  MODE_EDIT,
  MODE_SAVE_PROMPT,
  MODE_OPEN_PROMPT,
  MODE_FIND,
  MODE_REPLACE,
  MODE_RECOVERY_PROMPT,
} EditorMode;

typedef struct {
  char *data;
  int *advance_cache;
  int gap_start;
  int gap_end;
  int capacity;
} Line;

typedef struct {
  Line *lines;
  int line_count;
  int cursor_row;
  int cursor_col;
} EditorSnapshot;

typedef struct {
  Line *lines;
  int line_count;
  int line_capacity;
  int cursor_row;
  int cursor_col;
  int preferred_x;
  Uint32 last_vertical_move;
  bool selection_active;
  int sel_anchor_row;
  int sel_anchor_col;
  char *clipboard;
  int clipboard_len;
  int scroll_row;
  char filename[512];
  bool dirty;
  char status_msg[1024];
  Uint32 status_msg_time;
  EditorMode mode;
  EditorMode mode_before_recovery_prompt;
  char prompt_buf[512];
  char replace_buf[512];
  int prompt_cursor;
  EditorSnapshot *undo_stack[UNDO_STACK_SIZE];
  int undo_ptr;
  int text_x;

  /* Persistence layer integration */
  StorageSession *storage;
  Uint32 last_journal_write;
  bool pending_journal_write;
  ByteBuffer recovery_candidate_doc;
  StorageMetadata recovery_candidate_meta;
  bool has_recovery_candidate;
  StorageMetadata doc_meta;
} Editor;

static int line_length(const Line *l) {
  return l->capacity - (l->gap_end - l->gap_start);
}

static int logical_to_physical(const Line *l, int logical) {
  int len = line_length(l);
  if (logical < 0 || logical > len)
    return 0;
  if (logical < l->gap_start)
    return logical;
  return logical + (l->gap_end - l->gap_start);
}

static char line_char_at(const Line *l, int logical) {
  int len = line_length(l);
  if (logical < 0 || logical >= len)
    return '\0';
  return l->data[logical_to_physical(l, logical)];
}

static bool line_init(Line *l) {
  l->data = calloc(LINE_INITIAL_CAPACITY, 1);
  if (!l->data)
    return false;
  l->advance_cache = calloc(LINE_INITIAL_CAPACITY + 1, sizeof(int));
  if (!l->advance_cache) {
    free(l->data);
    l->data = NULL;
    return false;
  }
  l->gap_start = 0;
  l->gap_end = LINE_INITIAL_CAPACITY;
  l->capacity = LINE_INITIAL_CAPACITY;
  return true;
}

static void line_free(Line *l) {
  free(l->data);
  free(l->advance_cache);
  l->data = NULL;
  l->advance_cache = NULL;
  l->gap_start = l->gap_end = l->capacity = 0;
}

static bool line_clone(Line *dest, const Line *src) {
  dest->capacity = src->capacity;
  dest->gap_start = src->gap_start;
  dest->gap_end = src->gap_end;
  dest->data = malloc((size_t)src->capacity);
  if (!dest->data)
    return false;
  memcpy(dest->data, src->data, (size_t)src->capacity);
  dest->advance_cache = malloc((size_t)(src->capacity + 1) * sizeof(int));
  if (!dest->advance_cache) {
    free(dest->data);
    return false;
  }
  memcpy(dest->advance_cache, src->advance_cache,
         (size_t)(src->capacity + 1) * sizeof(int));
  return true;
}

static bool line_ensure_gap(Line *l, int needed) {
  int gap_size = l->gap_end - l->gap_start;
  if (gap_size >= needed)
    return true;

  int content = line_length(l);
  int new_capacity = l->capacity;
  while (new_capacity - content < needed) {
    new_capacity *= 2;
    if (new_capacity < 16)
      new_capacity = 16;
  }

  char *new_data = realloc(l->data, (size_t)new_capacity);
  if (!new_data)
    return false;

  int *new_cache =
      realloc(l->advance_cache, (size_t)(new_capacity + 1) * sizeof(int));
  if (!new_cache) {
    l->data = new_data;
    return false;
  }

  int suffix_len = l->capacity - l->gap_end;
  int new_gap_end = new_capacity - suffix_len;

  if (suffix_len > 0)
    memmove(&new_data[new_gap_end], &new_data[l->gap_end], (size_t)suffix_len);

  memset(&new_data[l->gap_start], 0, (size_t)(new_gap_end - l->gap_start));

  l->data = new_data;
  l->advance_cache = new_cache;
  l->gap_end = new_gap_end;
  l->capacity = new_capacity;
  return true;
}

static void move_gap(Line *l, int target_logical) {
  if (target_logical == l->gap_start)
    return;
  int gap_size = l->gap_end - l->gap_start;

  if (target_logical < l->gap_start) {
    memmove(&l->data[target_logical + gap_size], &l->data[target_logical],
            (size_t)(l->gap_start - target_logical));
  } else {
    int target_physical = target_logical + gap_size;
    memmove(&l->data[l->gap_start], &l->data[l->gap_end],
            (size_t)(target_physical - l->gap_end));
  }
  l->gap_end = target_logical + gap_size;
  l->gap_start = target_logical;
  memset(&l->data[l->gap_start], 0, (size_t)gap_size);
}

static bool line_insert_bytes(Line *l, int at, const char *bytes, int len) {
  if (len <= 0)
    return true;
  if (!line_ensure_gap(l, len))
    return false;
  move_gap(l, at);
  memcpy(&l->data[l->gap_start], bytes, (size_t)len);
  l->gap_start += len;
  return true;
}

static void line_delete_bytes(Line *l, int at, int len) {
  if (len <= 0)
    return;
  move_gap(l, at);
  memset(&l->data[l->gap_end], 0, (size_t)len);
  l->gap_end += len;
}

static void line_truncate(Line *l, int new_length) {
  int current = line_length(l);
  if (new_length >= current)
    return;
  line_delete_bytes(l, new_length, current - new_length);
}

static int utf8_seq_len(unsigned char byte) {
  if (byte < 0x80)
    return 1;
  if ((byte & 0xE0) == 0xC0)
    return 2;
  if ((byte & 0xF0) == 0xE0)
    return 3;
  if ((byte & 0xF8) == 0xF0)
    return 4;
  return 1;
}

static bool line_decode_codepoint(const Line *l, int at, uint32_t *cp,
                                  int *bytes) {
  int len = line_length(l);
  if (at < 0 || at >= len)
    return false;

  unsigned char lead = (unsigned char)line_char_at(l, at);
  int seq = utf8_seq_len(lead);
  if (seq > len - at)
    seq = len - at;

  uint32_t codepoint = 0;
  if (seq == 1)
    codepoint = lead & 0x7F;
  else if (seq == 2)
    codepoint = lead & 0x1F;
  else if (seq == 3)
    codepoint = lead & 0x0F;
  else
    codepoint = lead & 0x07;

  for (int i = 1; i < seq; i++) {
    unsigned char cont = (unsigned char)line_char_at(l, at + i);
    if ((cont & 0xC0) != 0x80) {
      seq = i;
      break;
    }
    codepoint = (codepoint << 6) | (cont & 0x3F);
  }

  *cp = codepoint;
  *bytes = seq;
  return true;
}

static int line_utf8_prev_char(const Line *l, int pos) {
  if (pos <= 0)
    return 0;
  int p = pos - 1;
  while (p > 0 && ((unsigned char)line_char_at(l, p) & 0xC0) == 0x80)
    p--;
  return p;
}

static int line_utf8_next_char(const Line *l, int line_len, int pos) {
  if (pos >= line_len)
    return line_len;
  unsigned char lead = (unsigned char)line_char_at(l, pos);
  int seq = utf8_seq_len(lead);
  if (pos + seq > line_len)
    return line_len;
  return pos + seq;
}

static bool is_word_char(uint32_t cp) {
  if (cp >= 'a' && cp <= 'z')
    return true;
  if (cp >= 'A' && cp <= 'Z')
    return true;
  if (cp >= '0' && cp <= '9')
    return true;
  if (cp == '_')
    return true;
  if (cp > 127)
    return true;
  return false;
}

static void rebuild_line_cache(Editor *e, FT_Face face, Line *l) {
  int len = line_length(l);
  if (!l->advance_cache) {
    l->advance_cache = calloc((size_t)(l->capacity + 1), sizeof(int));
    if (!l->advance_cache)
      return;
  }

  int x = e->text_x;
  l->advance_cache[0] = x;

  for (int col = 0; col < len;) {
    uint32_t cp = 0;
    int bytes = 0;
    if (!line_decode_codepoint(l, col, &cp, &bytes) || bytes <= 0) {
      cp = 0xFFFD;
      bytes = 1;
    }

    int adv = 0;
    if (cp == ' ' || cp == '\t') {
      adv = 16;
    } else {
      if (FT_Load_Char(face, cp, FT_LOAD_DEFAULT) == 0)
        adv = (int)(face->glyph->advance.x >> 6);
      else
        adv = 8;
    }

    for (int b = 0; b < bytes; b++) {
      int idx = col + b + 1;
      if (idx <= l->capacity)
        l->advance_cache[idx] = x + adv;
    }
    x += adv;
    col += bytes;
  }
  if (len <= l->capacity)
    l->advance_cache[len] = x;
}

static int get_line_x_position(const Editor *e, const Line *l, int col) {
  if (col <= 0)
    return e->text_x;
  int len = line_length(l);
  if (col > len)
    col = len;
  if (!l->advance_cache)
    return e->text_x;
  return l->advance_cache[col];
}

static int get_closest_column(const Editor *e, const Line *l, int target_x) {
  int len = line_length(l);
  if (len == 0)
    return 0;

  int best_col = 0;
  int best_dist = abs(get_line_x_position(e, l, 0) - target_x);

  for (int col = 0; col < len;) {
    uint32_t cp = 0;
    int bytes = 0;
    if (!line_decode_codepoint(l, col, &cp, &bytes) || bytes <= 0)
      bytes = 1;
    int next_col = col + bytes;
    int dist = abs(get_line_x_position(e, l, next_col) - target_x);
    if (dist < best_dist) {
      best_dist = dist;
      best_col = next_col;
    }
    col = next_col;
  }
  return best_col;
}

static void refresh_preferred_x(Editor *e) {
  e->preferred_x =
      get_line_x_position(e, &e->lines[e->cursor_row], e->cursor_col);
}

typedef enum {
  TOKEN_TEXT,
  TOKEN_KEYWORD,
  TOKEN_TYPE,
  TOKEN_STRING,
  TOKEN_COMMENT,
  TOKEN_NUMBER,
  TOKEN_PREPROCESSOR,
} TokenType;

static bool is_keyword(const char *s, int len) {
  static const char *keywords[] = {
      "break",  "case",     "continue", "default", "do",     "else",
      "enum",   "extern",   "for",      "goto",    "if",     "register",
      "return", "sizeof",   "static",   "struct",  "switch", "typedef",
      "union",  "volatile", "while",    "auto",    "const"};
  for (int i = 0; i < (int)(sizeof(keywords) / sizeof(keywords[0])); i++) {
    if (len == (int)strlen(keywords[i]) &&
        strncmp(s, keywords[i], (size_t)len) == 0)
      return true;
  }
  return false;
}

static bool is_type(const char *s, int len) {
  static const char *types[] = {
      "int",     "char",    "float",   "double",   "void",     "size_t",
      "ssize_t", "bool",    "uint8_t", "uint16_t", "uint32_t", "uint64_t",
      "int8_t",  "int16_t", "int32_t", "int64_t",  "Uint8",    "Uint16",
      "Uint32",  "Uint64",  "Sint8",   "Sint16",   "Sint32",   "Sint64"};
  for (int i = 0; i < (int)(sizeof(types) / sizeof(types[0])); i++) {
    if (len == (int)strlen(types[i]) && strncmp(s, types[i], (size_t)len) == 0)
      return true;
  }
  return false;
}

static TokenType get_token_at(Line *l, int col, int *token_len) {
  int len = line_length(l);
  if (col >= len)
    return TOKEN_TEXT;

  char c = line_char_at(l, col);

  if (c == '/' && col + 1 < len && line_char_at(l, col + 1) == '/') {
    *token_len = len - col;
    return TOKEN_COMMENT;
  }

  if (c == '#') {
    *token_len = 1;
    while (col + *token_len < len &&
           is_word_char(line_char_at(l, col + *token_len)))
      (*token_len)++;
    return TOKEN_PREPROCESSOR;
  }

  if (c == '"') {
    *token_len = 1;
    while (col + *token_len < len) {
      char sc = line_char_at(l, col + *token_len);
      (*token_len)++;
      if (sc == '"' && line_char_at(l, col + *token_len - 2) != '\\')
        break;
    }
    return TOKEN_STRING;
  }

  if (c >= '0' && c <= '9') {
    *token_len = 0;
    while (col + *token_len < len &&
           (is_word_char(line_char_at(l, col + *token_len)) ||
            line_char_at(l, col + *token_len) == '.'))
      (*token_len)++;
    return TOKEN_NUMBER;
  }

  if (is_word_char(c) &&
      (col == 0 || !is_word_char(line_char_at(l, col - 1)))) {
    int wlen = 0;
    while (col + wlen < len && is_word_char(line_char_at(l, col + wlen)))
      wlen++;

    char word[64];
    if (wlen < 64) {
      for (int i = 0; i < wlen; i++)
        word[i] = line_char_at(l, col + i);
      word[wlen] = '\0';
      if (is_keyword(word, wlen)) {
        *token_len = wlen;
        return TOKEN_KEYWORD;
      }
      if (is_type(word, wlen)) {
        *token_len = wlen;
        return TOKEN_TYPE;
      }
    }
  }

  *token_len = 1;
  return TOKEN_TEXT;
}

static void update_gutter_width(Editor *e, FT_Face face) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%d", e->line_count);
  int width = 0;
  for (int i = 0; buf[i]; i++) {
    if (FT_Load_Char(face, buf[i], FT_LOAD_DEFAULT) == 0)
      width += (int)(face->glyph->advance.x >> 6);
    else
      width += 12;
  }
  int space_adv = 16;
  if (FT_Load_Char(face, ' ', FT_LOAD_DEFAULT) == 0)
    space_adv = (int)(face->glyph->advance.x >> 6);

  e->text_x = GUTTER_X + width + (space_adv * 2);
}

static void render_glyph(SDL_Renderer *renderer, FT_Face face, uint32_t cp,
                         int *x, int y, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
  if (cp == ' ' || cp == '\t') {
    *x += 16;
    return;
  }
  if (FT_Load_Char(face, cp, FT_LOAD_RENDER))
    return;

  FT_GlyphSlot glyph = face->glyph;
  if (glyph->bitmap.width == 0 || glyph->bitmap.rows == 0) {
    *x += (int)(glyph->advance.x >> 6);
    return;
  }

  SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(
      0, (int)glyph->bitmap.width, (int)glyph->bitmap.rows, 32,
      SDL_PIXELFORMAT_RGBA32);
  if (!surf)
    return;

  SDL_LockSurface(surf);
  Uint32 *pixels = (Uint32 *)surf->pixels;
  int pitch_words = surf->pitch / 4;
  for (unsigned int gy = 0; gy < glyph->bitmap.rows; gy++)
    for (unsigned int gx = 0; gx < glyph->bitmap.width; gx++) {
      unsigned char alpha = glyph->bitmap.buffer[gy * glyph->bitmap.pitch + gx];
      pixels[gy * pitch_words + gx] = SDL_MapRGBA(surf->format, r, g, b, alpha);
    }
  SDL_UnlockSurface(surf);

  SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
  SDL_FreeSurface(surf);
  if (!tex)
    return;

  SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
  SDL_Rect dst = {*x + glyph->bitmap_left, y - glyph->bitmap_top,
                  (int)glyph->bitmap.width, (int)glyph->bitmap.rows};
  SDL_RenderCopy(renderer, tex, NULL, &dst);
  SDL_DestroyTexture(tex);
  *x += (int)(glyph->advance.x >> 6);
  (void)a;
}

static void editor_clear_selection(Editor *e) { e->selection_active = false; }

static void editor_start_selection(Editor *e) {
  if (!e->selection_active) {
    e->selection_active = true;
    e->sel_anchor_row = e->cursor_row;
    e->sel_anchor_col = e->cursor_col;
  }
}

typedef struct {
  int row, col;
} TextPos;

static bool sel_ordered(TextPos a, TextPos b) {
  return a.row < b.row || (a.row == b.row && a.col <= b.col);
}

static TextPos sel_start(const Editor *e) {
  TextPos cur = {e->cursor_row, e->cursor_col};
  TextPos anchor = {e->sel_anchor_row, e->sel_anchor_col};
  return sel_ordered(cur, anchor) ? cur : anchor;
}

static TextPos sel_end(const Editor *e) {
  TextPos cur = {e->cursor_row, e->cursor_col};
  TextPos anchor = {e->sel_anchor_row, e->sel_anchor_col};
  return sel_ordered(cur, anchor) ? anchor : cur;
}

static int editor_selection_to_buf(const Editor *e, char *buf, int buf_size) {
  if (!e->selection_active)
    return 0;
  TextPos s = sel_start(e);
  TextPos t = sel_end(e);
  int written = 0;

  for (int row = s.row; row <= t.row && written < buf_size - 1; row++) {
    const Line *l = &e->lines[row];
    int len = line_length(l);
    int c0 = (row == s.row) ? s.col : 0;
    int c1 = (row == t.row) ? t.col : len;
    for (int c = c0; c < c1 && written < buf_size - 1; c++) {
      buf[written++] = line_char_at(l, c);
    }
    if (row < t.row && written < buf_size - 1)
      buf[written++] = '\n';
  }
  buf[written] = '\0';
  return written;
}

static void editor_delete_selection(Editor *e, FT_Face face) {
  if (!e->selection_active)
    return;
  TextPos s = sel_start(e);
  TextPos t = sel_end(e);

  if (s.row == t.row) {
    Line *l = &e->lines[s.row];
    line_delete_bytes(l, s.col, t.col - s.col);
    rebuild_line_cache(e, face, l);
  } else {
    Line *first = &e->lines[s.row];
    Line *last = &e->lines[t.row];
    int last_len = line_length(last);
    int tail_len = last_len - t.col;

    line_truncate(first, s.col);

    if (tail_len > 0) {
      if (line_ensure_gap(first, tail_len)) {
        move_gap(first, s.col);
        for (int i = 0; i < tail_len; i++)
          first->data[first->gap_start + i] = line_char_at(last, t.col + i);
        first->gap_start += tail_len;
      }
    }
    rebuild_line_cache(e, face, first);

    for (int r = s.row + 1; r <= t.row; r++)
      line_free(&e->lines[r]);
    int removed = t.row - s.row;
    for (int r = s.row + 1; r < e->line_count - removed; r++)
      e->lines[r] = e->lines[r + removed];
    for (int r = e->line_count - removed; r < e->line_count; r++)
      memset(&e->lines[r], 0, sizeof(Line));
    e->line_count -= removed;
  }

  e->cursor_row = s.row;
  e->cursor_col = s.col;
  e->selection_active = false;
}

static bool editor_ensure_lines(Editor *e, int needed) {
  if (e->line_count + needed < e->line_capacity)
    return true;
  int new_cap = e->line_capacity;
  while (new_cap <= e->line_count + needed)
    new_cap *= 2;
  Line *new_lines = realloc(e->lines, (size_t)new_cap * sizeof(Line));
  if (!new_lines)
    return false;
  memset(&new_lines[e->line_capacity], 0,
         (size_t)(new_cap - e->line_capacity) * sizeof(Line));
  e->lines = new_lines;
  e->line_capacity = new_cap;
  return true;
}

static int visible_rows(SDL_Window *window) {
  int wh;
  SDL_GetWindowSize(window, NULL, &wh);
  return (wh - FIRST_LINE_Y - STATUS_BAR_HEIGHT) / LINE_HEIGHT_PX;
}

static void editor_set_status(Editor *e, const char *msg) {
  snprintf(e->status_msg, sizeof(e->status_msg), "%s", msg);
  e->status_msg_time = SDL_GetTicks();
}

/* ===================================================================== *
 * Storage integration: serialize the line-based buffer to/from a flat
 * UTF-8 byte stream (the StorageSection_DOCUMENT payload). This is the
 * seam where a future structured document model would plug in a richer
 * serializer without touching anything else in this file or in storage.c.
 * ===================================================================== */

static void editor_serialize_to_buffer(const Editor *e, ByteBuffer *out) {
  bytebuffer_init(out);
  for (int row = 0; row < e->line_count; row++) {
    const Line *l = &e->lines[row];
    int len = line_length(l);
    for (int col = 0; col < len; col++) {
      char c = line_char_at(l, col);
      bytebuffer_append(out, &c, 1);
    }
    if (row < e->line_count - 1) {
      char nl = '\n';
      bytebuffer_append(out, &nl, 1);
    }
  }
}

static bool editor_load_lines_from_bytes(Editor *e, FT_Face face,
                                         const uint8_t *data, size_t len) {
  for (int i = 0; i < e->line_count; i++)
    line_free(&e->lines[i]);
  e->line_count = 0;

  if (!editor_ensure_lines(e, 1))
    return false;
  if (!line_init(&e->lines[0]))
    return false;
  e->line_count = 1;

  for (size_t i = 0; i < len; i++) {
    char c = (char)data[i];
    if (c == '\n') {
      if (!editor_ensure_lines(e, 1))
        return false;
      if (!line_init(&e->lines[e->line_count]))
        return false;
      e->line_count++;
    } else {
      Line *l = &e->lines[e->line_count - 1];
      line_insert_bytes(l, line_length(l), &c, 1);
    }
  }

  update_gutter_width(e, face);
  for (int i = 0; i < e->line_count; i++)
    rebuild_line_cache(e, face, &e->lines[i]);

  e->cursor_row = 0;
  e->cursor_col = 0;
  e->scroll_row = 0;
  e->preferred_x = e->text_x;
  e->selection_active = false;
  return true;
}

static bool editor_save(Editor *e) {
  if (!e->storage) {
    editor_set_status(e, "ERR: no storage session bound");
    return false;
  }

  ByteBuffer doc;
  editor_serialize_to_buffer(e, &doc);

  if (e->doc_meta.title[0] == '\0') {
    const char *base = strrchr(e->filename, '/');
    base = base ? base + 1 : e->filename;
    size_t base_len = strlen(base);
    size_t copy_len = base_len < sizeof(e->doc_meta.title) - 1
                          ? base_len
                          : sizeof(e->doc_meta.title) - 1;
    memcpy(e->doc_meta.title, base, copy_len);
    e->doc_meta.title[copy_len] = '\0';
  }

  StorageStatus st = storage_save(e->storage, e->filename, &doc, &e->doc_meta);
  bytebuffer_free(&doc);

  if (st != STORAGE_OK) {
    char msg[1024];
    snprintf(msg, sizeof(msg), "ERR: save failed (%s)",
             storage_status_string(st));
    editor_set_status(e, msg);
    return false;
  }

  e->dirty = false;
  e->pending_journal_write = false;
  char msg[1024];
  snprintf(msg, sizeof(msg), "Saved: %s", e->filename);
  editor_set_status(e, msg);
  return true;
}

/* Appends the current in-memory buffer to the crash-recovery journal.
 * Called on a debounce timer from the main loop (not on every keystroke)
 * so heavy typing doesn't thrash the disk, while still bounding how much
 * work could be lost to a crash to roughly JOURNAL_DEBOUNCE_MS. */
static void editor_journal_if_due(Editor *e, Uint32 now) {
  if (!e->storage || !e->pending_journal_write)
    return;
  if (now - e->last_journal_write < JOURNAL_DEBOUNCE_MS)
    return;

  ByteBuffer doc;
  editor_serialize_to_buffer(e, &doc);
  storage_journal_append(e->storage, "edit", &doc);
  bytebuffer_free(&doc);

  e->last_journal_write = now;
  e->pending_journal_write = false;
}

static void editor_push_undo(Editor *e) {
  if (e->undo_ptr >= UNDO_STACK_SIZE) {
    EditorSnapshot *oldest = e->undo_stack[0];
    for (int i = 0; i < oldest->line_count; i++) {
      line_free(&oldest->lines[i]);
    }
    free(oldest->lines);
    free(oldest);
    for (int i = 0; i < UNDO_STACK_SIZE - 1; i++) {
      e->undo_stack[i] = e->undo_stack[i + 1];
    }
    e->undo_ptr--;
  }

  EditorSnapshot *s = malloc(sizeof(EditorSnapshot));
  s->line_count = e->line_count;
  s->cursor_row = e->cursor_row;
  s->cursor_col = e->cursor_col;
  s->lines = malloc(sizeof(Line) * e->line_count);
  for (int i = 0; i < e->line_count; i++) {
    line_clone(&s->lines[i], &e->lines[i]);
  }
  e->undo_stack[e->undo_ptr++] = s;

  /* Any structural edit is journal-worthy; mark pending so the next
     debounce tick persists it for crash recovery. */
  e->pending_journal_write = true;
  if (e->storage)
    storage_mark_dirty(e->storage);
}

static void editor_undo(Editor *e, FT_Face face) {
  if (e->undo_ptr <= 0)
    return;
  EditorSnapshot *s = e->undo_stack[--e->undo_ptr];

  for (int i = 0; i < e->line_count; i++) {
    line_free(&e->lines[i]);
  }

  e->line_count = s->line_count;
  e->cursor_row = s->cursor_row;
  e->cursor_col = s->cursor_col;
  free(e->lines);
  e->lines = s->lines;
  e->line_capacity = s->line_count;
  update_gutter_width(e, face);

  for (int i = 0; i < e->line_count; i++) {
    rebuild_line_cache(e, face, &e->lines[i]);
  }

  free(s);
  refresh_preferred_x(e);

  e->dirty = true;
  e->pending_journal_write = true;
  if (e->storage)
    storage_mark_dirty(e->storage);
}

static bool editor_load(Editor *e, FT_Face face, const char *path) {
  StorageSession *new_session = NULL;
  ByteBuffer doc;
  StorageMetadata meta;
  StorageOpenResult open_result;

  StorageStatus st =
      storage_session_open(path, &new_session, &doc, &meta, &open_result);
  if (st != STORAGE_OK) {
    char msg[600];
    snprintf(msg, sizeof(msg), "ERR: could not open '%s' (%s)", path,
             storage_status_string(st));
    editor_set_status(e, msg);
    return false;
  }

  if (!editor_load_lines_from_bytes(e, face, doc.data, doc.len)) {
    bytebuffer_free(&doc);
    storage_session_close(new_session);
    editor_set_status(e, "ERR: failed to materialize document into buffer");
    return false;
  }
  bytebuffer_free(&doc);

  if (e->storage)
    storage_session_close(e->storage);
  e->storage = new_session;
  e->doc_meta = meta;
  snprintf(e->filename, sizeof(e->filename), "%s", path);
  e->dirty = false;
  e->pending_journal_write = false;
  e->last_journal_write = SDL_GetTicks();

  bytebuffer_free(&e->recovery_candidate_doc);
  e->has_recovery_candidate = false;

  if (open_result == STORAGE_OPEN_RECOVERED) {
    ByteBuffer rec_doc;
    StorageMetadata rec_meta;
    if (storage_recovery_get(e->storage, &rec_doc, &rec_meta) == STORAGE_OK) {
      e->recovery_candidate_doc = rec_doc;
      e->recovery_candidate_meta = rec_meta;
      e->has_recovery_candidate = true;
      e->mode_before_recovery_prompt = e->mode;
      e->mode = MODE_RECOVERY_PROMPT;
      editor_set_status(e,
                        "Unsaved changes from a previous session were found");
    } else {
      editor_set_status(e, "Opened (a previous save was repaired from backup)");
    }
  } else if (open_result == STORAGE_OPEN_NEW) {
    editor_set_status(e, "New file");
  } else {
    editor_set_status(e, "Opened");
  }

  return true;
}

static void editor_accept_recovery(Editor *e, FT_Face face) {
  if (!e->has_recovery_candidate)
    return;
  editor_load_lines_from_bytes(e, face, e->recovery_candidate_doc.data,
                               e->recovery_candidate_doc.len);
  e->doc_meta = e->recovery_candidate_meta;
  e->dirty = true;
  e->pending_journal_write = true;
  bytebuffer_free(&e->recovery_candidate_doc);
  e->has_recovery_candidate = false;
  e->mode = e->mode_before_recovery_prompt;
  editor_set_status(e,
                    "Recovered unsaved changes -- press Ctrl+S to commit them");
}

static void editor_discard_recovery(Editor *e) {
  if (!e->has_recovery_candidate)
    return;
  bytebuffer_free(&e->recovery_candidate_doc);
  e->has_recovery_candidate = false;
  if (e->storage)
    storage_recovery_discard(e->storage);
  e->mode = e->mode_before_recovery_prompt;
  editor_set_status(e, "Recovery data discarded");
}

static void editor_duplicate_line(Editor *e, FT_Face face) {
  if (!editor_ensure_lines(e, 1))
    return;

  Line *src = &e->lines[e->cursor_row];
  int src_len = line_length(src);

  for (int i = e->line_count; i > e->cursor_row + 1; i--)
    e->lines[i] = e->lines[i - 1];

  Line new_line;
  if (!line_init(&new_line))
    return;

  if (src_len > 0 && line_ensure_gap(&new_line, src_len)) {
    for (int i = 0; i < src_len; i++)
      new_line.data[i] = line_char_at(src, i);
    new_line.gap_start = src_len;
    new_line.gap_end = new_line.capacity;
  }
  rebuild_line_cache(e, face, &new_line);
  e->lines[e->cursor_row + 1] = new_line;
  e->line_count++;
  update_gutter_width(e, face);
  e->cursor_row++;
  e->dirty = true;
}

static void editor_move_line_up(Editor *e, FT_Face face) {
  if (e->cursor_row == 0)
    return;
  Line tmp = e->lines[e->cursor_row];
  e->lines[e->cursor_row] = e->lines[e->cursor_row - 1];
  e->lines[e->cursor_row - 1] = tmp;
  rebuild_line_cache(e, face, &e->lines[e->cursor_row]);
  rebuild_line_cache(e, face, &e->lines[e->cursor_row - 1]);
  e->cursor_row--;
  int new_len = line_length(&e->lines[e->cursor_row]);
  if (e->cursor_col > new_len)
    e->cursor_col = new_len;
  if (e->cursor_row < e->scroll_row)
    e->scroll_row = e->cursor_row;
  e->dirty = true;
}

static void editor_move_line_down(Editor *e, FT_Face face) {
  if (e->cursor_row >= e->line_count - 1)
    return;
  Line tmp = e->lines[e->cursor_row];
  e->lines[e->cursor_row] = e->lines[e->cursor_row + 1];
  e->lines[e->cursor_row + 1] = tmp;
  rebuild_line_cache(e, face, &e->lines[e->cursor_row]);
  rebuild_line_cache(e, face, &e->lines[e->cursor_row + 1]);
  e->cursor_row++;
  int new_len = line_length(&e->lines[e->cursor_row]);
  if (e->cursor_col > new_len)
    e->cursor_col = new_len;
  e->dirty = true;
}

static void editor_toggle_comment(Editor *e, FT_Face face) {
  Line *l = &e->lines[e->cursor_row];
  int len = line_length(l);

  int first_non_space = 0;
  while (first_non_space < len) {
    char c = line_char_at(l, first_non_space);
    if (c != ' ' && c != '\t')
      break;
    first_non_space++;
  }

  bool has_comment =
      (first_non_space + 1 < len && line_char_at(l, first_non_space) == '/' &&
       line_char_at(l, first_non_space + 1) == '/');

  if (has_comment) {
    int skip = 2;
    if (first_non_space + 2 < len &&
        line_char_at(l, first_non_space + 2) == ' ')
      skip = 3;
    line_delete_bytes(l, first_non_space, skip);
    if (e->cursor_col > first_non_space)
      e->cursor_col -= skip;
    if (e->cursor_col < 0)
      e->cursor_col = 0;
  } else {
    const char *prefix = "// ";
    line_insert_bytes(l, first_non_space, prefix, 3);
    if (e->cursor_col >= first_non_space)
      e->cursor_col += 3;
  }
  rebuild_line_cache(e, face, l);
  e->dirty = true;
}

static void editor_indent_selection(Editor *e, FT_Face face, bool unindent) {
  if (!e->selection_active) {
    Line *l = &e->lines[e->cursor_row];
    if (unindent) {
      int removed = 0;
      for (int i = 0; i < 4; i++) {
        if (line_char_at(l, 0) == ' ') {
          line_delete_bytes(l, 0, 1);
          removed++;
        } else
          break;
      }
      e->cursor_col -= removed;
      if (e->cursor_col < 0)
        e->cursor_col = 0;
    } else {
      line_insert_bytes(l, 0, "    ", 4);
      e->cursor_col += 4;
    }
    rebuild_line_cache(e, face, l);
    e->dirty = true;
    return;
  }

  TextPos s = sel_start(e);
  TextPos t = sel_end(e);
  for (int row = s.row; row <= t.row; row++) {
    Line *l = &e->lines[row];
    if (unindent) {
      int removed = 0;
      for (int i = 0; i < 4; i++) {
        if (line_char_at(l, 0) == ' ') {
          line_delete_bytes(l, 0, 1);
          removed++;
        } else
          break;
      }
      if (row == s.row) {
        s.col -= removed;
        if (s.col < 0)
          s.col = 0;
      }
      if (row == t.row) {
        t.col -= removed;
        if (t.col < 0)
          t.col = 0;
      }
    } else {
      line_insert_bytes(l, 0, "    ", 4);
      if (row == s.row)
        s.col += 4;
      if (row == t.row)
        t.col += 4;
    }
    rebuild_line_cache(e, face, l);
  }
  e->sel_anchor_row = s.row;
  e->sel_anchor_col = s.col;
  e->cursor_row = t.row;
  e->cursor_col = t.col;
  e->dirty = true;
}

static void editor_find(Editor *e, bool forward) {
  if (e->prompt_buf[0] == '\0')
    return;

  int start_row = e->cursor_row;
  int start_col = forward ? e->cursor_col + 1 : e->cursor_col - 1;
  if (start_col < 0)
    start_col = 0;

  for (int i = 0; i < e->line_count; i++) {
    int row = (forward) ? (start_row + i) % e->line_count
                        : (start_row - i + e->line_count) % e->line_count;
    Line *l = &e->lines[row];
    int ll = line_length(l);

    char *line_str = malloc((size_t)ll + 1);
    for (int c = 0; c < ll; c++)
      line_str[c] = line_char_at(l, c);
    line_str[ll] = '\0';

    char *match = NULL;
    if (row == start_row) {
      if (forward) {
        if (start_col < ll)
          match = strstr(line_str + start_col, e->prompt_buf);
      } else {
        char *last_match = NULL;
        char *current_match = strstr(line_str, e->prompt_buf);
        while (current_match && (current_match - line_str) < start_col) {
          last_match = current_match;
          current_match = strstr(current_match + 1, e->prompt_buf);
        }
        match = last_match;
      }
    } else {
      match = strstr(line_str, e->prompt_buf);
    }

    if (match) {
      e->cursor_row = row;
      e->cursor_col = (int)(match - line_str);
      e->scroll_row = e->cursor_row - 5;
      if (e->scroll_row < 0)
        e->scroll_row = 0;
      refresh_preferred_x(e);
      free(line_str);
      return;
    }
    free(line_str);
  }
}

static void editor_replace(Editor *e, FT_Face face) {
  if (e->prompt_buf[0] == '\0')
    return;

  Line *l = &e->lines[e->cursor_row];
  int ll = line_length(l);
  char *line_str = malloc((size_t)ll + 1);
  for (int c = 0; c < ll; c++)
    line_str[c] = line_char_at(l, c);
  line_str[ll] = '\0';

  char *match = strstr(line_str + e->cursor_col, e->prompt_buf);
  if (match && (match - line_str) == e->cursor_col) {
    editor_push_undo(e);
    line_delete_bytes(l, e->cursor_col, (int)strlen(e->prompt_buf));
    line_insert_bytes(l, e->cursor_col, e->replace_buf,
                      (int)strlen(e->replace_buf));
    rebuild_line_cache(e, face, l);
    e->dirty = true;
    editor_find(e, true);
  } else {
    editor_find(e, true);
  }
  free(line_str);
}

static void render_dialog(SDL_Renderer *renderer, FT_Face face, Editor *e,
                          SDL_Window *window) {
  int ww, wh;
  SDL_GetWindowSize(window, &ww, &wh);

  int dw = 650;
  int dh = (e->mode == MODE_REPLACE) ? 260 : 180;
  int dx = (ww - dw) / 2;
  int dy = (wh - dh) / 2;

  SDL_Rect overlay = {0, 0, ww, wh};
  SDL_SetRenderDrawColor(renderer, 5, 5, 10, 200);
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SDL_RenderFillRect(renderer, &overlay);

  SDL_Rect shadow = {dx + 5, dy + 5, dw, dh};
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 100);
  SDL_RenderFillRect(renderer, &shadow);

  SDL_Rect rect = {dx, dy, dw, dh};
  SDL_SetRenderDrawColor(renderer, 30, 30, 45, 255);
  SDL_RenderFillRect(renderer, &rect);

  SDL_Rect header = {dx, dy, dw, 4};
  SDL_SetRenderDrawColor(renderer, 100, 150, 255, 255);
  SDL_RenderFillRect(renderer, &header);

  SDL_SetRenderDrawColor(renderer, 60, 60, 80, 255);
  SDL_RenderDrawRect(renderer, &rect);

  const char *title = "DIALOG";
  if (e->mode == MODE_SAVE_PROMPT)
    title = "SAVE FILE AS";
  else if (e->mode == MODE_OPEN_PROMPT)
    title = "OPEN FILE";
  else if (e->mode == MODE_FIND)
    title = "FIND IN FILE";
  else if (e->mode == MODE_REPLACE)
    title = "FIND AND REPLACE";

  int tx = dx + 30;
  int ty = dy + 45;
  for (int i = 0; title[i]; i++) {
    render_glyph(renderer, face, (uint32_t)(unsigned char)title[i], &tx, ty,
                 255, 255, 255, 255);
  }

  SDL_Rect input_rect = {dx + 30, dy + 75, dw - 60, 45};
  SDL_SetRenderDrawColor(renderer, 15, 15, 25, 255);
  SDL_RenderFillRect(renderer, &input_rect);

  if (e->mode != MODE_REPLACE || e->prompt_cursor == 0)
    SDL_SetRenderDrawColor(renderer, 100, 150, 255, 255);
  else
    SDL_SetRenderDrawColor(renderer, 60, 60, 80, 255);
  SDL_RenderDrawRect(renderer, &input_rect);

  int ix = dx + 45;
  int iy = dy + 105;
  for (int i = 0; e->prompt_buf[i]; i++) {
    render_glyph(renderer, face, (uint32_t)(unsigned char)e->prompt_buf[i], &ix,
                 iy, 200, 220, 255, 255);
  }

  if (e->mode == MODE_REPLACE) {
    SDL_Rect input_rect2 = {dx + 30, dy + 155, dw - 60, 45};
    SDL_SetRenderDrawColor(renderer, 15, 15, 25, 255);
    SDL_RenderFillRect(renderer, &input_rect2);

    if (e->prompt_cursor == 1)
      SDL_SetRenderDrawColor(renderer, 100, 255, 150, 255);
    else
      SDL_SetRenderDrawColor(renderer, 60, 60, 80, 255);
    SDL_RenderDrawRect(renderer, &input_rect2);

    int ix2 = dx + 45;
    int iy2 = dy + 185;
    for (int i = 0; e->replace_buf[i]; i++) {
      render_glyph(renderer, face, (uint32_t)(unsigned char)e->replace_buf[i],
                   &ix2, iy2, 200, 255, 200, 255);
    }

    int lx = dx + 35;
    int ly = dy + 70;
    const char *find_label = "FIND";
    for (int i = 0; find_label[i]; i++)
      render_glyph(renderer, face, find_label[i], &lx, ly, 120, 150, 180, 255);

    lx = dx + 35;
    ly = dy + 150;
    const char *replace_label = "REPLACE WITH";
    for (int i = 0; replace_label[i]; i++)
      render_glyph(renderer, face, replace_label[i], &lx, ly, 120, 180, 150,
                   255);
  }

  if ((SDL_GetTicks() / 500) % 2) {
    if (e->mode == MODE_REPLACE && e->prompt_cursor == 1) {
      int rx = dx + 45;
      for (int i = 0; e->replace_buf[i]; i++) {
        uint32_t cp = (uint32_t)(unsigned char)e->replace_buf[i];
        if (cp == ' ' || cp == '\t')
          rx += 16;
        else if (FT_Load_Char(face, cp, FT_LOAD_DEFAULT) == 0)
          rx += (int)(face->glyph->advance.x >> 6);
        else
          rx += 8;
      }
      SDL_Rect c_rect = {rx, dy + 162, 2, 30};
      SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
      SDL_RenderFillRect(renderer, &c_rect);
    } else {
      int rx = dx + 45;
      for (int i = 0; e->prompt_buf[i]; i++) {
        uint32_t cp = (uint32_t)(unsigned char)e->prompt_buf[i];
        if (cp == ' ' || cp == '\t')
          rx += 16;
        else if (FT_Load_Char(face, cp, FT_LOAD_DEFAULT) == 0)
          rx += (int)(face->glyph->advance.x >> 6);
        else
          rx += 8;
      }
      SDL_Rect c_rect = {rx, dy + 82, 2, 30};
      SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
      SDL_RenderFillRect(renderer, &c_rect);
    }
  }

  int ox = dx + 30;
  int oy = dy + dh - 15;
  const char *opt1 = "[ENTER] CONFIRM";
  if (e->mode == MODE_FIND)
    opt1 = "[ENTER] FIND NEXT";
  if (e->mode == MODE_REPLACE)
    opt1 = "[ENTER] REPLACE";
  const char *opt_close = "[ESC] CANCEL";

  for (int i = 0; opt1[i]; i++) {
    render_glyph(renderer, face, (uint32_t)(unsigned char)opt1[i], &ox, oy, 150,
                 255, 150, 255);
  }
  ox += 40;
  if (e->mode == MODE_REPLACE) {
    const char *opt_tab = "[TAB] SWITCH FIELD";
    for (int i = 0; opt_tab[i]; i++)
      render_glyph(renderer, face, (uint32_t)(unsigned char)opt_tab[i], &ox, oy,
                   150, 150, 255, 255);
    ox += 40;
  }
  for (int i = 0; opt_close[i]; i++) {
    render_glyph(renderer, face, (uint32_t)(unsigned char)opt_close[i], &ox, oy,
                 255, 150, 150, 255);
  }
}

static void render_recovery_dialog(SDL_Renderer *renderer, FT_Face face,
                                   Editor *e, SDL_Window *window) {
  (void)e;
  int ww, wh;
  SDL_GetWindowSize(window, &ww, &wh);

  int dw = 700;
  int dh = 220;
  int dx = (ww - dw) / 2;
  int dy = (wh - dh) / 2;

  SDL_Rect overlay = {0, 0, ww, wh};
  SDL_SetRenderDrawColor(renderer, 5, 5, 10, 210);
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SDL_RenderFillRect(renderer, &overlay);

  SDL_Rect shadow = {dx + 5, dy + 5, dw, dh};
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 120);
  SDL_RenderFillRect(renderer, &shadow);

  SDL_Rect rect = {dx, dy, dw, dh};
  SDL_SetRenderDrawColor(renderer, 35, 28, 20, 255);
  SDL_RenderFillRect(renderer, &rect);

  SDL_Rect header = {dx, dy, dw, 4};
  SDL_SetRenderDrawColor(renderer, 255, 170, 60, 255);
  SDL_RenderFillRect(renderer, &header);

  SDL_SetRenderDrawColor(renderer, 90, 70, 40, 255);
  SDL_RenderDrawRect(renderer, &rect);

  int tx = dx + 30, ty = dy + 45;
  const char *title = "RECOVERY AVAILABLE";
  for (int i = 0; title[i]; i++)
    render_glyph(renderer, face, (uint32_t)(unsigned char)title[i], &tx, ty,
                 255, 220, 180, 255);

  const char *line1 = "Unsaved changes from a previous session were found.";
  const char *line2 =
      "Restore them, or discard and keep the last saved version?";
  int lx = dx + 30, ly = dy + 90;
  for (int i = 0; line1[i]; i++)
    render_glyph(renderer, face, (uint32_t)(unsigned char)line1[i], &lx, ly,
                 220, 220, 220, 255);
  lx = dx + 30;
  ly = dy + 125;
  for (int i = 0; line2[i]; i++)
    render_glyph(renderer, face, (uint32_t)(unsigned char)line2[i], &lx, ly,
                 180, 180, 180, 255);

  int ox = dx + 30, oy = dy + dh - 25;
  const char *opt_r = "[R] RESTORE RECOVERED VERSION";
  const char *opt_d = "[D] DISCARD AND KEEP SAVED VERSION";
  for (int i = 0; opt_r[i]; i++)
    render_glyph(renderer, face, (uint32_t)(unsigned char)opt_r[i], &ox, oy,
                 150, 255, 150, 255);
  ox += 50;
  for (int i = 0; opt_d[i]; i++)
    render_glyph(renderer, face, (uint32_t)(unsigned char)opt_d[i], &ox, oy,
                 255, 150, 150, 255);
}

static void render_status_bar(SDL_Renderer *renderer, FT_Face face,
                              const Editor *e, SDL_Window *window) {
  int ww, wh;
  SDL_GetWindowSize(window, &ww, &wh);
  int bar_y = wh - STATUS_BAR_HEIGHT;

  SDL_Rect bar = {0, bar_y, ww, STATUS_BAR_HEIGHT};
  SDL_SetRenderDrawColor(renderer, 35, 35, 55, 255);
  SDL_RenderFillRect(renderer, &bar);

  char left[1024];
  const char *dirty_mark = e->dirty ? " [+]" : "";
  snprintf(left, sizeof(left), " %s%s", e->filename, dirty_mark);

  char right[128];
  snprintf(right, sizeof(right), "Ln %d, Col %d  ", e->cursor_row + 1,
           e->cursor_col + 1);

  int x = 10;
  int text_y = bar_y + STATUS_BAR_HEIGHT - 6;

  for (int i = 0; left[i]; i++) {
    uint32_t cp = (uint32_t)(unsigned char)left[i];
    render_glyph(renderer, face, cp, &x, text_y, 200, 200, 220, 255);
  }

  Uint32 elapsed = SDL_GetTicks() - e->status_msg_time;
  if (elapsed < 3000 && e->status_msg[0]) {
    int mx = ww / 2 - 100;
    for (int i = 0; e->status_msg[i]; i++) {
      uint32_t cp = (uint32_t)(unsigned char)e->status_msg[i];
      render_glyph(renderer, face, cp, &mx, text_y, 100, 220, 120, 255);
    }
  }

  char mode_text[32];
  snprintf(mode_text, sizeof(mode_text), "[ %s ]",
           (e->mode == MODE_EDIT)   ? "EDIT"
           : (e->mode == MODE_VIEW) ? "VIEW"
                                    : "PROMPT");
  int mode_rx = ww - 350;
  for (int i = 0; mode_text[i]; i++) {
    uint32_t cp = (uint32_t)(unsigned char)mode_text[i];
    render_glyph(renderer, face, cp, &mode_rx, text_y,
                 (e->mode == MODE_EDIT) ? 255 : 100,
                 (e->mode == MODE_EDIT) ? 100 : 200,
                 (e->mode == MODE_EDIT) ? 100 : 255, 255);
  }

  int rx = ww - (int)strlen(right) * 14;
  for (int i = 0; right[i]; i++) {
    uint32_t cp = (uint32_t)(unsigned char)right[i];
    render_glyph(renderer, face, cp, &rx, text_y, 160, 160, 180, 255);
  }
}

int main(int argc, char *argv[]) {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    SDL_Log("SDL_Init failed: %s", SDL_GetError());
    return 1;
  }

  SDL_Window *window = SDL_CreateWindow(
      "Text Editor", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 800, 600,
      SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
  if (!window) {
    SDL_Quit();
    return 1;
  }

  SDL_Renderer *renderer = SDL_CreateRenderer(
      window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer) {
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  FT_Library ft;
  if (FT_Init_FreeType(&ft)) {
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  FT_Face face;
  const char *font_paths[] = {"assets/font.ttf", "../assets/font.ttf"};
  bool font_loaded = false;
  for (int i = 0; i < (int)(sizeof(font_paths) / sizeof(font_paths[0])); i++) {
    if (FT_New_Face(ft, font_paths[i], 0, &face) == 0) {
      font_loaded = true;
      break;
    }
  }
  if (!font_loaded) {
    FT_Done_FreeType(ft);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  FT_Set_Pixel_Sizes(face, 0, FONT_SIZE_PX);
  SDL_StartTextInput();

  Editor editor;
  memset(&editor, 0, sizeof(editor));
  editor.line_capacity = LINES_INITIAL_CAPACITY;
  editor.line_count = 1;
  editor.clipboard = calloc(CLIPBOARD_MAX, 1);
  snprintf(editor.filename, sizeof(editor.filename), "%s", DEFAULT_FILENAME);
  editor.storage = NULL;
  bytebuffer_init(&editor.recovery_candidate_doc);

  editor.lines = calloc((size_t)editor.line_capacity, sizeof(Line));
  if (!editor.lines || !editor.clipboard)
    goto cleanup;

  {
    const char *target = (argc >= 2) ? argv[1] : DEFAULT_FILENAME;
    if (!editor_load(&editor, face, target)) {
      if (!line_init(&editor.lines[0]))
        goto cleanup;
      rebuild_line_cache(&editor, face, &editor.lines[0]);
      snprintf(editor.filename, sizeof(editor.filename), "%s", target);
      editor_set_status(&editor, "New file (storage unavailable)");
    }
  }

  editor.preferred_x = editor.text_x;
  update_gutter_width(&editor, face);
  editor.preferred_x = editor.text_x;
  rebuild_line_cache(&editor, face, &editor.lines[0]);

  bool running = true;
  Uint32 last_blink = SDL_GetTicks();
  bool cursor_visible = true;
  bool cursor_moving = false;
  bool autosave_flash = false;
  Uint32 autosave_flash_until = 0;

  while (running) {
    cursor_moving = false;

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      switch (ev.type) {

      case SDL_QUIT:
        running = false;
        break;

      case SDL_MOUSEWHEEL: {
        int delta = -ev.wheel.y * 3;
        editor.scroll_row += delta;
        if (editor.scroll_row < 0)
          editor.scroll_row = 0;
        int max_scroll = editor.line_count - visible_rows(window);
        if (max_scroll < 0)
          max_scroll = 0;
        if (editor.scroll_row > max_scroll)
          editor.scroll_row = max_scroll;
        break;
      }

      case SDL_MOUSEBUTTONDOWN: {
        if (editor.mode == MODE_RECOVERY_PROMPT)
          break;
        if (ev.button.button == SDL_BUTTON_LEFT) {
          int mx = ev.button.x;
          int my = ev.button.y;
          int clicked_row =
              (my - FIRST_LINE_Y) / LINE_HEIGHT_PX + editor.scroll_row;
          if (clicked_row < 0)
            clicked_row = 0;
          if (clicked_row >= editor.line_count)
            clicked_row = editor.line_count - 1;
          editor.cursor_row = clicked_row;
          editor.cursor_col =
              get_closest_column(&editor, &editor.lines[clicked_row], mx);
          editor.preferred_x = get_line_x_position(
              &editor, &editor.lines[editor.cursor_row], editor.cursor_col);
          editor_clear_selection(&editor);
          cursor_moving = true;
          cursor_visible = true;
        }
        break;
      }

      case SDL_TEXTINPUT: {
        if (editor.mode == MODE_RECOVERY_PROMPT) {
          const char *t = ev.text.text;
          if (t[0] == 'r' || t[0] == 'R') {
            editor_accept_recovery(&editor, face);
          } else if (t[0] == 'd' || t[0] == 'D') {
            editor_discard_recovery(&editor);
          }
          break;
        }
        if (editor.mode != MODE_EDIT && editor.mode != MODE_VIEW) {
          if (editor.mode == MODE_REPLACE && editor.prompt_cursor == 1) {
            strncat(editor.replace_buf, ev.text.text,
                    sizeof(editor.replace_buf) - strlen(editor.replace_buf) -
                        1);
          } else {
            strncat(editor.prompt_buf, ev.text.text,
                    sizeof(editor.prompt_buf) - strlen(editor.prompt_buf) - 1);
          }
          break;
        }
        if (editor.mode != MODE_EDIT)
          break;
        editor_push_undo(&editor);
        if (editor.selection_active)
          editor_delete_selection(&editor, face);
        Line *line = &editor.lines[editor.cursor_row];
        int inserted_bytes = (int)strlen(ev.text.text);
        if (inserted_bytes > 0 &&
            line_insert_bytes(line, editor.cursor_col, ev.text.text,
                              inserted_bytes)) {
          editor.cursor_col += inserted_bytes;
          rebuild_line_cache(&editor, face, line);
          editor.preferred_x =
              get_line_x_position(&editor, line, editor.cursor_col);
          editor.dirty = true;
        }
        break;
      }

      case SDL_KEYDOWN: {
        if (editor.mode == MODE_RECOVERY_PROMPT) {
          SDL_Keycode sym = ev.key.keysym.sym;
          if (sym == SDLK_r) {
            editor_accept_recovery(&editor, face);
          } else if (sym == SDLK_d || sym == SDLK_ESCAPE) {
            editor_discard_recovery(&editor);
          }
          break;
        }

        Line *line = &editor.lines[editor.cursor_row];
        SDL_Keymod mod = SDL_GetModState();
        SDL_Keycode sym = ev.key.keysym.sym;

        bool shift = (mod & KMOD_SHIFT) != 0;
        bool ctrl = (mod & KMOD_CTRL) != 0;
        bool alt = (mod & KMOD_ALT) != 0;

        if (editor.mode != MODE_VIEW && editor.mode != MODE_EDIT) {
          if (sym == SDLK_RETURN) {
            if (editor.mode == MODE_SAVE_PROMPT) {
              if (editor.prompt_buf[0]) {
                if (strcmp(editor.prompt_buf, editor.filename) != 0) {
                  /* Saving under a new name: rebind storage session to
                     the new path so future autosaves/journal entries
                     target the right sibling files too. */
                  if (editor.storage) {
                    storage_session_close(editor.storage);
                    editor.storage = NULL;
                  }
                  StorageSession *s = NULL;
                  ByteBuffer dummy_doc;
                  StorageMetadata dummy_meta;
                  StorageOpenResult dummy_res;
                  storage_session_open(editor.prompt_buf, &s, &dummy_doc,
                                       &dummy_meta, &dummy_res);
                  bytebuffer_free(&dummy_doc);
                  editor.storage = s;
                }
                snprintf(editor.filename, sizeof(editor.filename), "%s",
                         editor.prompt_buf);
                editor_save(&editor);
              }
            } else if (editor.mode == MODE_OPEN_PROMPT) {
              if (editor.prompt_buf[0]) {
                editor_load(&editor, face, editor.prompt_buf);
              }
            } else if (editor.mode == MODE_FIND) {
              editor_find(&editor, true);
              continue;
            } else if (editor.mode == MODE_REPLACE) {
              editor_replace(&editor, face);
              continue;
            }
            if (editor.mode != MODE_RECOVERY_PROMPT)
              editor.mode = MODE_VIEW;
          } else if (sym == SDLK_ESCAPE) {
            editor.mode = MODE_VIEW;
          } else if (sym == SDLK_BACKSPACE) {
            if (editor.mode == MODE_REPLACE && editor.prompt_cursor == 1) {
              size_t len = strlen(editor.replace_buf);
              if (len > 0)
                editor.replace_buf[len - 1] = '\0';
            } else {
              size_t len = strlen(editor.prompt_buf);
              if (len > 0)
                editor.prompt_buf[len - 1] = '\0';
            }
          } else if (sym == SDLK_TAB && editor.mode == MODE_REPLACE) {
            editor.prompt_cursor = (editor.prompt_cursor + 1) % 2;
          }
          break;
        }

        if (editor.mode == MODE_VIEW) {
          if (sym == SDLK_i) {
            editor.mode = MODE_EDIT;
            editor_set_status(&editor, "-- EDIT MODE --");
            break;
          }
        } else if (editor.mode == MODE_EDIT) {
          if (sym == SDLK_ESCAPE) {
            editor.mode = MODE_VIEW;
            editor_set_status(&editor, "-- VIEW MODE --");
            break;
          }
        }

        if (ctrl && sym == SDLK_f) {
          editor.mode = MODE_FIND;
          editor.prompt_buf[0] = '\0';
          break;
        }

        if (ctrl && sym == SDLK_h) {
          editor.mode = MODE_REPLACE;
          editor.prompt_buf[0] = '\0';
          editor.replace_buf[0] = '\0';
          editor.prompt_cursor = 0;
          break;
        }

        if (editor.mode == MODE_VIEW && !ctrl && !alt) {
          if (sym != SDLK_UP && sym != SDLK_DOWN && sym != SDLK_LEFT &&
              sym != SDLK_RIGHT && sym != SDLK_HOME && sym != SDLK_END &&
              sym != SDLK_PAGEUP && sym != SDLK_PAGEDOWN) {
            break;
          }
        }

        if (ctrl && sym == SDLK_z) {
          editor_undo(&editor, face);
          break;
        }

        if (ctrl && sym == SDLK_s) {
          editor.mode = MODE_SAVE_PROMPT;
          snprintf(editor.prompt_buf, sizeof(editor.prompt_buf), "%s",
                   editor.filename);
          break;
        }

        if (ctrl && sym == SDLK_o) {
          editor.mode = MODE_OPEN_PROMPT;
          editor.prompt_buf[0] = '\0';
          break;
        }

        if (ctrl && sym == SDLK_n) {
          editor_push_undo(&editor);
          for (int i = 0; i < editor.line_count; i++)
            line_free(&editor.lines[i]);
          editor.line_count = 1;
          line_init(&editor.lines[0]);
          rebuild_line_cache(&editor, face, &editor.lines[0]);
          editor.cursor_row = 0;
          editor.cursor_col = 0;
          editor.scroll_row = 0;
          snprintf(editor.filename, sizeof(editor.filename), "%s",
                   DEFAULT_FILENAME);
          editor.dirty = false;
          memset(&editor.doc_meta, 0, sizeof(editor.doc_meta));
          editor_set_status(&editor, "New File");
          break;
        }

        if (sym == SDLK_BACKSPACE) {
          editor_push_undo(&editor);
          if (editor.selection_active) {
            editor_delete_selection(&editor, face);
            line = &editor.lines[editor.cursor_row];
            rebuild_line_cache(&editor, face, line);
          } else if (editor.cursor_col > 0) {
            int delete_start = editor.cursor_col;
            if (ctrl) {
              while (delete_start > 0) {
                int prev = line_utf8_prev_char(line, delete_start);
                uint32_t cp = 0;
                int nb = 0;
                if (!line_decode_codepoint(line, prev, &cp, &nb))
                  break;
                if (is_word_char(cp))
                  break;
                delete_start = prev;
              }
              while (delete_start > 0) {
                int prev = line_utf8_prev_char(line, delete_start);
                uint32_t cp = 0;
                int nb = 0;
                if (!line_decode_codepoint(line, prev, &cp, &nb))
                  break;
                if (!is_word_char(cp))
                  break;
                delete_start = prev;
              }
            } else {
              delete_start = line_utf8_prev_char(line, editor.cursor_col);
            }
            int delete_len = editor.cursor_col - delete_start;
            line_delete_bytes(line, delete_start, delete_len);
            editor.cursor_col = delete_start;
            rebuild_line_cache(&editor, face, line);
            editor.preferred_x =
                get_line_x_position(&editor, line, editor.cursor_col);
            editor.dirty = true;
          } else if (editor.cursor_row > 0) {
            int prev_row = editor.cursor_row - 1;
            Line *prev_line = &editor.lines[prev_row];
            int prev_len = line_length(prev_line);
            int cur_len = line_length(line);

            move_gap(prev_line, prev_len);
            move_gap(line, cur_len);

            if (!line_ensure_gap(prev_line, cur_len)) {
              running = false;
              break;
            }
            memcpy(&prev_line->data[prev_line->gap_start], line->data,
                   (size_t)cur_len);
            prev_line->gap_start += cur_len;
            rebuild_line_cache(&editor, face, prev_line);
            line_free(line);

            for (int i = editor.cursor_row; i < editor.line_count - 1; i++)
              editor.lines[i] = editor.lines[i + 1];
            memset(&editor.lines[editor.line_count - 1], 0, sizeof(Line));
            editor.line_count--;
            update_gutter_width(&editor, face);
            editor.cursor_row = prev_row;
            editor.cursor_col = prev_len;
            editor.preferred_x = get_line_x_position(
                &editor, &editor.lines[editor.cursor_row], editor.cursor_col);
            editor.dirty = true;
          }
          cursor_moving = true;
          cursor_visible = true;
        }

        else if (sym == SDLK_DELETE) {
          editor_push_undo(&editor);
          if (editor.selection_active) {
            editor_delete_selection(&editor, face);
            line = &editor.lines[editor.cursor_row];
            rebuild_line_cache(&editor, face, line);
          } else {
            int len = line_length(line);
            if (editor.cursor_col < len) {
              int delete_end = editor.cursor_col;
              if (ctrl) {
                while (delete_end < len) {
                  uint32_t cp = 0;
                  int nb = 0;
                  if (!line_decode_codepoint(line, delete_end, &cp, &nb))
                    break;
                  if (is_word_char(cp))
                    break;
                  delete_end += nb;
                }
                while (delete_end < len) {
                  uint32_t cp = 0;
                  int nb = 0;
                  if (!line_decode_codepoint(line, delete_end, &cp, &nb))
                    break;
                  if (!is_word_char(cp))
                    break;
                  delete_end += nb;
                }
              } else {
                delete_end = line_utf8_next_char(line, len, editor.cursor_col);
              }
              line_delete_bytes(line, editor.cursor_col,
                                delete_end - editor.cursor_col);
              rebuild_line_cache(&editor, face, line);
              editor.preferred_x =
                  get_line_x_position(&editor, line, editor.cursor_col);
              editor.dirty = true;
            } else if (editor.cursor_row < editor.line_count - 1) {
              Line *next_line = &editor.lines[editor.cursor_row + 1];
              int next_len = line_length(next_line);
              move_gap(line, len);
              move_gap(next_line, next_len);
              if (!line_ensure_gap(line, next_len)) {
                running = false;
                break;
              }
              memcpy(&line->data[line->gap_start], next_line->data,
                     (size_t)next_len);
              line->gap_start += next_len;
              rebuild_line_cache(&editor, face, line);
              line_free(next_line);
              for (int i = editor.cursor_row + 1; i < editor.line_count - 1;
                   i++)
                editor.lines[i] = editor.lines[i + 1];
              memset(&editor.lines[editor.line_count - 1], 0, sizeof(Line));
              editor.line_count--;
              editor.dirty = true;
            }
          }
          cursor_moving = true;
          cursor_visible = true;
        }

        else if (sym == SDLK_TAB) {
          editor_push_undo(&editor);
          if (ctrl || alt) {
            editor_indent_selection(&editor, face, shift);
          } else if (editor.selection_active && shift) {
            editor_indent_selection(&editor, face, true);
          } else if (editor.selection_active) {
            editor_indent_selection(&editor, face, false);
          } else {
            if (shift) {
              editor_indent_selection(&editor, face, true);
            } else {
              if (editor.selection_active)
                editor_delete_selection(&editor, face);
              line = &editor.lines[editor.cursor_row];
              const char *indent = "    ";
              if (line_insert_bytes(line, editor.cursor_col, indent, 4)) {
                editor.cursor_col += 4;
                rebuild_line_cache(&editor, face, line);
                editor.preferred_x =
                    get_line_x_position(&editor, line, editor.cursor_col);
                editor.dirty = true;
              }
            }
          }
        }

        else if (sym == SDLK_RETURN) {
          editor_push_undo(&editor);
          if (editor.selection_active)
            editor_delete_selection(&editor, face);
          line = &editor.lines[editor.cursor_row];

          int indent_len = 0;
          while (indent_len < line_length(line)) {
            char c = line_char_at(line, indent_len);
            if (c != ' ' && c != '\t')
              break;
            indent_len++;
          }

          if (!editor_ensure_lines(&editor, 1)) {
            running = false;
            break;
          }
          line = &editor.lines[editor.cursor_row];

          for (int i = editor.line_count; i > editor.cursor_row + 1; i--)
            editor.lines[i] = editor.lines[i - 1];

          Line new_line;
          if (!line_init(&new_line)) {
            running = false;
            break;
          }

          int split_len = line_length(line) - editor.cursor_col;
          if (split_len < 0)
            split_len = 0;

          int total_new = split_len + indent_len;
          if (total_new > 0 && !line_ensure_gap(&new_line, total_new)) {
            line_free(&new_line);
            running = false;
            break;
          }

          for (int i = 0; i < indent_len; i++)
            new_line.data[new_line.gap_start + i] = line_char_at(line, i);
          new_line.gap_start += indent_len;

          if (split_len > 0) {
            for (int i = 0; i < split_len; i++)
              new_line.data[new_line.gap_start + i] =
                  line_char_at(line, editor.cursor_col + i);
            new_line.gap_start += split_len;
          }

          rebuild_line_cache(&editor, face, &new_line);
          line_truncate(line, editor.cursor_col);
          rebuild_line_cache(&editor, face, line);

          editor.lines[editor.cursor_row + 1] = new_line;
          editor.line_count++;
          update_gutter_width(&editor, face);
          editor.cursor_row++;
          editor.cursor_col = indent_len;
          editor.preferred_x = get_line_x_position(
              &editor, &editor.lines[editor.cursor_row], editor.cursor_col);
          editor.dirty = true;
          cursor_moving = true;
          cursor_visible = true;
        }

        else if (sym == SDLK_HOME) {
          if (shift)
            editor_start_selection(&editor);
          else
            editor_clear_selection(&editor);
          line = &editor.lines[editor.cursor_row];
          if (ctrl) {
            editor.cursor_row = 0;
            editor.cursor_col = 0;
          } else {
            int first_non_space = 0;
            int ll = line_length(line);
            while (first_non_space < ll) {
              char c = line_char_at(line, first_non_space);
              if (c != ' ' && c != '\t')
                break;
              first_non_space++;
            }
            if (editor.cursor_col == first_non_space)
              editor.cursor_col = 0;
            else
              editor.cursor_col = first_non_space;
          }
          editor.preferred_x = get_line_x_position(
              &editor, &editor.lines[editor.cursor_row], editor.cursor_col);
          cursor_moving = true;
          cursor_visible = true;
        }

        else if (sym == SDLK_END) {
          if (shift)
            editor_start_selection(&editor);
          else
            editor_clear_selection(&editor);
          if (ctrl) {
            editor.cursor_row = editor.line_count - 1;
            editor.cursor_col = line_length(&editor.lines[editor.cursor_row]);
          } else {
            editor.cursor_col = line_length(&editor.lines[editor.cursor_row]);
          }
          editor.preferred_x = get_line_x_position(
              &editor, &editor.lines[editor.cursor_row], editor.cursor_col);
          cursor_moving = true;
          cursor_visible = true;
        }

        else if (sym == SDLK_PAGEUP) {
          if (shift)
            editor_start_selection(&editor);
          else
            editor_clear_selection(&editor);
          int rows = visible_rows(window);
          editor.cursor_row -= rows;
          if (editor.cursor_row < 0)
            editor.cursor_row = 0;
          editor.scroll_row -= rows;
          if (editor.scroll_row < 0)
            editor.scroll_row = 0;
          editor.cursor_col = get_closest_column(
              &editor, &editor.lines[editor.cursor_row], editor.preferred_x);
          cursor_moving = true;
          cursor_visible = true;
        }

        else if (sym == SDLK_PAGEDOWN) {
          if (shift)
            editor_start_selection(&editor);
          else
            editor_clear_selection(&editor);
          int rows = visible_rows(window);
          editor.cursor_row += rows;
          if (editor.cursor_row >= editor.line_count)
            editor.cursor_row = editor.line_count - 1;
          editor.scroll_row += rows;
          int max_scroll = editor.line_count - rows;
          if (max_scroll < 0)
            max_scroll = 0;
          if (editor.scroll_row > max_scroll)
            editor.scroll_row = max_scroll;
          editor.cursor_col = get_closest_column(
              &editor, &editor.lines[editor.cursor_row], editor.preferred_x);
          cursor_moving = true;
          cursor_visible = true;
        }

        else if (sym == SDLK_LEFT) {
          if (shift)
            editor_start_selection(&editor);
          else
            editor_clear_selection(&editor);
          line = &editor.lines[editor.cursor_row];
          if (editor.cursor_col > 0) {
            if (ctrl) {
              int pos = editor.cursor_col;
              while (pos > 0) {
                int prev = line_utf8_prev_char(line, pos);
                uint32_t cp = 0;
                int nb = 0;
                if (!line_decode_codepoint(line, prev, &cp, &nb))
                  break;
                if (is_word_char(cp))
                  break;
                pos = prev;
              }
              while (pos > 0) {
                int prev = line_utf8_prev_char(line, pos);
                uint32_t cp = 0;
                int nb = 0;
                if (!line_decode_codepoint(line, prev, &cp, &nb))
                  break;
                if (!is_word_char(cp))
                  break;
                pos = prev;
              }
              editor.cursor_col = pos;
            } else {
              editor.cursor_col = line_utf8_prev_char(line, editor.cursor_col);
            }
          } else if (editor.cursor_row > 0) {
            editor.cursor_row--;
            editor.cursor_col = line_length(&editor.lines[editor.cursor_row]);
          }
          editor.preferred_x = get_line_x_position(
              &editor, &editor.lines[editor.cursor_row], editor.cursor_col);
          cursor_moving = true;
          cursor_visible = true;
        }

        else if (sym == SDLK_RIGHT) {
          if (shift)
            editor_start_selection(&editor);
          else
            editor_clear_selection(&editor);
          line = &editor.lines[editor.cursor_row];
          int line_len = line_length(line);
          if (editor.cursor_col < line_len) {
            if (ctrl) {
              int pos = editor.cursor_col;
              while (pos < line_len) {
                uint32_t cp = 0;
                int nb = 0;
                if (!line_decode_codepoint(line, pos, &cp, &nb))
                  break;
                if (is_word_char(cp))
                  break;
                pos += nb;
              }
              while (pos < line_len) {
                uint32_t cp = 0;
                int nb = 0;
                if (!line_decode_codepoint(line, pos, &cp, &nb))
                  break;
                if (!is_word_char(cp))
                  break;
                pos += nb;
              }
              editor.cursor_col = pos;
            } else {
              editor.cursor_col =
                  line_utf8_next_char(line, line_len, editor.cursor_col);
            }
          } else if (editor.cursor_row < editor.line_count - 1) {
            editor.cursor_row++;
            editor.cursor_col = 0;
          }
          editor.preferred_x = get_line_x_position(
              &editor, &editor.lines[editor.cursor_row], editor.cursor_col);
          cursor_moving = true;
          cursor_visible = true;
        }

        else if (sym == SDLK_UP) {
          if (alt && !shift) {
            editor_push_undo(&editor);
            editor_move_line_up(&editor, face);
            cursor_moving = true;
            cursor_visible = true;
          } else {
            if (shift)
              editor_start_selection(&editor);
            else
              editor_clear_selection(&editor);
            Uint32 now = SDL_GetTicks();
            if (now - editor.last_vertical_move > VERTICAL_MOVE_RESET_MS)
              refresh_preferred_x(&editor);
            editor.last_vertical_move = now;
            if (editor.cursor_row > 0) {
              editor.cursor_row--;
              editor.cursor_col =
                  get_closest_column(&editor, &editor.lines[editor.cursor_row],
                                     editor.preferred_x);
              if (editor.cursor_row < editor.scroll_row)
                editor.scroll_row = editor.cursor_row;
              cursor_moving = true;
              cursor_visible = true;
            }
          }
        }

        else if (sym == SDLK_DOWN) {
          if (alt && !shift) {
            editor_push_undo(&editor);
            editor_move_line_down(&editor, face);
            cursor_moving = true;
            cursor_visible = true;
          } else {
            if (shift)
              editor_start_selection(&editor);
            else
              editor_clear_selection(&editor);
            Uint32 now = SDL_GetTicks();
            if (now - editor.last_vertical_move > VERTICAL_MOVE_RESET_MS)
              refresh_preferred_x(&editor);
            editor.last_vertical_move = now;
            if (editor.cursor_row < editor.line_count - 1) {
              editor.cursor_row++;
              editor.cursor_col =
                  get_closest_column(&editor, &editor.lines[editor.cursor_row],
                                     editor.preferred_x);
              int vis = visible_rows(window);
              if (editor.cursor_row >= editor.scroll_row + vis)
                editor.scroll_row = editor.cursor_row - vis + 1;
              cursor_moving = true;
              cursor_visible = true;
            }
          }
        }

        else if (ctrl && sym == SDLK_a) {
          editor.selection_active = true;
          editor.sel_anchor_row = 0;
          editor.sel_anchor_col = 0;
          editor.cursor_row = editor.line_count - 1;
          editor.cursor_col = line_length(&editor.lines[editor.cursor_row]);
          editor.preferred_x = get_line_x_position(
              &editor, &editor.lines[editor.cursor_row], editor.cursor_col);
          cursor_moving = true;
          cursor_visible = true;
        }

        else if (ctrl && sym == SDLK_c) {
          if (editor.selection_active && editor.clipboard) {
            editor_selection_to_buf(&editor, editor.clipboard, CLIPBOARD_MAX);
            editor.clipboard_len = (int)strlen(editor.clipboard);
            SDL_SetClipboardText(editor.clipboard);
          }
        }

        else if (ctrl && sym == SDLK_x) {
          editor_push_undo(&editor);
          if (editor.selection_active && editor.clipboard) {
            editor_selection_to_buf(&editor, editor.clipboard, CLIPBOARD_MAX);
            editor.clipboard_len = (int)strlen(editor.clipboard);
            SDL_SetClipboardText(editor.clipboard);
            editor_delete_selection(&editor, face);
            line = &editor.lines[editor.cursor_row];
            rebuild_line_cache(&editor, face, line);
            editor.dirty = true;
          }
          cursor_moving = true;
          cursor_visible = true;
        }

        else if (ctrl && sym == SDLK_v) {
          editor_push_undo(&editor);
          char *text = SDL_GetClipboardText();
          if (text && *text) {
            if (editor.selection_active)
              editor_delete_selection(&editor, face);
            line = &editor.lines[editor.cursor_row];

            const char *p = text;
            while (*p) {
              const char *nl = strchr(p, '\n');
              int seg_len = nl ? (int)(nl - p) : (int)strlen(p);

              if (seg_len > 0 &&
                  line_insert_bytes(line, editor.cursor_col, p, seg_len)) {
                editor.cursor_col += seg_len;
                rebuild_line_cache(&editor, face, line);
              }

              if (nl) {
                if (!editor_ensure_lines(&editor, 1))
                  break;
                line = &editor.lines[editor.cursor_row];
                for (int i = editor.line_count; i > editor.cursor_row + 1; i--)
                  editor.lines[i] = editor.lines[i - 1];

                Line new_line;
                if (!line_init(&new_line))
                  break;

                int split_len = line_length(line) - editor.cursor_col;
                if (split_len > 0 && line_ensure_gap(&new_line, split_len)) {
                  for (int i = 0; i < split_len; i++)
                    new_line.data[i] =
                        line_char_at(line, editor.cursor_col + i);
                  new_line.gap_start = split_len;
                  new_line.gap_end = new_line.capacity;
                }
                rebuild_line_cache(&editor, face, &new_line);
                line_truncate(line, editor.cursor_col);
                rebuild_line_cache(&editor, face, line);

                editor.lines[editor.cursor_row + 1] = new_line;
                editor.line_count++;
                editor.cursor_row++;
                editor.cursor_col = 0;
                line = &editor.lines[editor.cursor_row];
                p = nl + 1;
              } else {
                break;
              }
            }
            SDL_free(text);
            editor.preferred_x =
                get_line_x_position(&editor, line, editor.cursor_col);
            editor.dirty = true;
          }
          cursor_moving = true;
          cursor_visible = true;
        }

        else if (ctrl && sym == SDLK_d) {
          editor_push_undo(&editor);
          editor_duplicate_line(&editor, face);
          editor.preferred_x = get_line_x_position(
              &editor, &editor.lines[editor.cursor_row], editor.cursor_col);
          cursor_moving = true;
          cursor_visible = true;
        }

        else if (ctrl && sym == SDLK_k) {
          editor_push_undo(&editor);
          line = &editor.lines[editor.cursor_row];
          int len = line_length(line);
          if (editor.cursor_col < len) {
            line_truncate(line, editor.cursor_col);
            rebuild_line_cache(&editor, face, line);
          } else if (editor.cursor_row < editor.line_count - 1) {
            Line *next = &editor.lines[editor.cursor_row + 1];
            int next_len = line_length(next);
            move_gap(line, len);
            move_gap(next, next_len);
            if (line_ensure_gap(line, next_len)) {
              memcpy(&line->data[line->gap_start], next->data,
                     (size_t)next_len);
              line->gap_start += next_len;
              rebuild_line_cache(&editor, face, line);
            }
            line_free(next);
            for (int i = editor.cursor_row + 1; i < editor.line_count - 1; i++)
              editor.lines[i] = editor.lines[i + 1];
            memset(&editor.lines[editor.line_count - 1], 0, sizeof(Line));
            editor.line_count--;
          }
          editor.preferred_x =
              get_line_x_position(&editor, line, editor.cursor_col);
          editor.dirty = true;
          cursor_moving = true;
          cursor_visible = true;
        }

        else if (ctrl && sym == SDLK_l) {
          editor.cursor_col = 0;
          int vis = visible_rows(window);
          int target_scroll = editor.cursor_row - vis / 2;
          if (target_scroll < 0)
            target_scroll = 0;
          int max_scroll = editor.line_count - vis;
          if (max_scroll < 0)
            max_scroll = 0;
          if (target_scroll > max_scroll)
            target_scroll = max_scroll;
          editor.scroll_row = target_scroll;
          editor.preferred_x = get_line_x_position(
              &editor, &editor.lines[editor.cursor_row], editor.cursor_col);
          cursor_moving = true;
          cursor_visible = true;
        }

        else if (ctrl && sym == SDLK_SLASH) {
          editor_push_undo(&editor);
          editor_toggle_comment(&editor, face);
          editor.preferred_x = get_line_x_position(
              &editor, &editor.lines[editor.cursor_row], editor.cursor_col);
          cursor_moving = true;
          cursor_visible = true;
        }

        break;
      }

      default:
        break;
      }
    }

    Uint32 frame_now = SDL_GetTicks();

    editor_journal_if_due(&editor, frame_now);

    if (editor.storage) {
      ByteBuffer doc_for_autosave;
      editor_serialize_to_buffer(&editor, &doc_for_autosave);
      bool fired = storage_autosave_tick(editor.storage, &doc_for_autosave,
                                         &editor.doc_meta, frame_now);
      bytebuffer_free(&doc_for_autosave);
      if (fired) {
        autosave_flash = true;
        autosave_flash_until = frame_now + 1500;
      }
    }
    if (autosave_flash && frame_now > autosave_flash_until)
      autosave_flash = false;

    if (!cursor_moving) {
      if (frame_now - last_blink >= CURSOR_BLINK_MS) {
        cursor_visible = !cursor_visible;
        last_blink = frame_now;
      }
    } else {
      last_blink = frame_now;
      cursor_visible = true;
    }

    SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
    SDL_RenderClear(renderer);

    int vis_rows = visible_rows(window);

    TextPos sel_s = sel_start(&editor);
    TextPos sel_t = sel_end(&editor);

    for (int row = editor.scroll_row;
         row < editor.line_count && row < editor.scroll_row + vis_rows; row++) {

      int screen_y = FIRST_LINE_Y + (row - editor.scroll_row) * LINE_HEIGHT_PX;

      if (editor.selection_active) {
        bool row_selected = (row >= sel_s.row && row <= sel_t.row);
        if (row_selected) {
          Line *sl = &editor.lines[row];
          int slen = line_length(sl);
          int x0 = (row == sel_s.row)
                       ? get_line_x_position(&editor, sl, sel_s.col)
                       : editor.text_x;
          int x1 = (row == sel_t.row)
                       ? get_line_x_position(&editor, sl, sel_t.col)
                       : get_line_x_position(&editor, sl, slen) + 8;
          SDL_Rect sel_rect = {x0, screen_y - 20, x1 - x0, 28};
          SDL_SetRenderDrawColor(renderer, 60, 100, 180, 120);
          SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
          SDL_RenderFillRect(renderer, &sel_rect);
          SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        }
      }

      if (row == editor.cursor_row) {
        int ww;
        SDL_GetWindowSize(window, &ww, NULL);
        SDL_Rect hl = {0, screen_y - 20, ww, 28};
        SDL_SetRenderDrawColor(renderer, 40, 40, 55, 255);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_RenderFillRect(renderer, &hl);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
      }

      char num_buf[16];
      if (row == editor.cursor_row)
        snprintf(num_buf, sizeof(num_buf), "%d", row + 1);
      else
        snprintf(num_buf, sizeof(num_buf), "%d", abs(row - editor.cursor_row));

      int num_x = GUTTER_X;
      for (int i = 0; num_buf[i] != '\0'; i++) {
        uint32_t cp = (uint32_t)(unsigned char)num_buf[i];
        Uint8 r = 120, g = 120, b = 120;
        if (row == editor.cursor_row) {
          r = 255;
          g = 200;
          b = 60;
        }
        render_glyph(renderer, face, cp, &num_x, screen_y, r, g, b, 255);
      }

      Line *l = &editor.lines[row];
      int x = editor.text_x;
      int ll = line_length(l);
      int token_len = 0;
      TokenType current_token = TOKEN_TEXT;
      for (int col = 0; col < ll;) {
        if (token_len <= 0) {
          current_token = get_token_at(l, col, &token_len);
        }
        uint32_t cp = 0;
        int bytes = 0;
        if (!line_decode_codepoint(l, col, &cp, &bytes) || bytes <= 0) {
          cp = 0xFFFD;
          bytes = 1;
        }
        Uint8 r = 220, g = 220, b = 220;
        switch (current_token) {
        case TOKEN_KEYWORD:
          r = 255;
          g = 100;
          b = 150;
          break;
        case TOKEN_TYPE:
          r = 100;
          g = 200;
          b = 255;
          break;
        case TOKEN_STRING:
          r = 230;
          g = 230;
          b = 100;
          break;
        case TOKEN_COMMENT:
          r = 100;
          g = 180;
          b = 100;
          break;
        case TOKEN_NUMBER:
          r = 255;
          g = 180;
          b = 100;
          break;
        case TOKEN_PREPROCESSOR:
          r = 200;
          g = 150;
          b = 255;
          break;
        default:
          break;
        }
        render_glyph(renderer, face, cp, &x, screen_y, r, g, b, 255);
        col += bytes;
        token_len -= bytes;
      }
    }

    if (editor.cursor_row >= editor.scroll_row &&
        editor.cursor_row < editor.scroll_row + vis_rows) {
      int cursor_screen_y =
          FIRST_LINE_Y +
          (editor.cursor_row - editor.scroll_row) * LINE_HEIGHT_PX;
      Line *cursor_line = &editor.lines[editor.cursor_row];
      int cursor_x =
          get_line_x_position(&editor, cursor_line, editor.cursor_col);

      if (cursor_visible) {
        SDL_Rect rect = {cursor_x, cursor_screen_y - 20, 2, 28};
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_RenderFillRect(renderer, &rect);
      }
    }

    render_status_bar(renderer, face, &editor, window);

    if (autosave_flash) {
      int ww;
      SDL_GetWindowSize(window, &ww, NULL);
      int ax = ww - 120;
      int ay = visible_rows(window) > 0 ? 20 : 20;
      const char *txt = "AUTOSAVED";
      for (int i = 0; txt[i]; i++)
        render_glyph(renderer, face, (uint32_t)(unsigned char)txt[i], &ax, ay,
                     120, 200, 255, 255);
    }

    if (editor.mode == MODE_RECOVERY_PROMPT) {
      render_recovery_dialog(renderer, face, &editor, window);
    } else if (editor.mode != MODE_EDIT && editor.mode != MODE_VIEW) {
      render_dialog(renderer, face, &editor, window);
    }

    SDL_RenderPresent(renderer);
    SDL_Delay(1);
  }

cleanup:
  for (int i = 0; i < editor.line_count; i++)
    line_free(&editor.lines[i]);
  free(editor.lines);
  free(editor.clipboard);
  bytebuffer_free(&editor.recovery_candidate_doc);
  if (editor.storage)
    storage_session_close(editor.storage);
  SDL_StopTextInput();
  FT_Done_Face(face);
  FT_Done_FreeType(ft);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
