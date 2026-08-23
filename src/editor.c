#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <SDL2/SDL.h>
#include <SDL_keyboard.h>
#include <SDL_keycode.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "formatter.h"
#include "language.h"
#include "save_pipeline.h"
#include "storage.h"

/* =====================================================================
 * Constants
 * ===================================================================== */
#define FONT_SIZE_PX 18
#define LINE_HEIGHT_PX 28
#define GUTTER_PAD 8
#define FIRST_LINE_Y 8
#define CURSOR_BLINK_MS 500
#define VERTICAL_MOVE_RESET_MS 2000
#define DEFAULT_FILENAME "untitled.edoc"
#define STATUS_BAR_HEIGHT 24
#define JOURNAL_DEBOUNCE_MS 800
#define SCROLL_MARGIN 4

/* Line gap buffer initial size */
#define LINE_INIT_CAP 64
/* Line-array gap buffer: gap of lines, not chars */
#define LINEBUF_INIT_CAP 64
/* Undo/redo ring */
#define UNDO_RING_SIZE 256
/* Glyph atlas */
#define GLYPH_CACHE_SIZE 512
/* Command palette entries */
#define PALETTE_MAX 64
/* Max search results to highlight */
#define SEARCH_MAX_RESULTS 4096

/* =====================================================================
 * UTF-8 helpers
 * ===================================================================== */
static int utf8_seq_len(unsigned char b) {
  if (b < 0x80)
    return 1;
  if ((b & 0xE0) == 0xC0)
    return 2;
  if ((b & 0xF0) == 0xE0)
    return 3;
  if ((b & 0xF8) == 0xF0)
    return 4;
  return 1;
}

static bool is_word_char(uint32_t cp) {
  return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
         (cp >= '0' && cp <= '9') || cp == '_' || cp > 127;
}

/* =====================================================================
 * Glyph cache
 * Each glyph is rasterised once and stored as an SDL_Texture.
 * ===================================================================== */
typedef struct {
  uint32_t codepoint;
  SDL_Texture *tex;
  int bitmap_left;
  int bitmap_top;
  int bitmap_w;
  int bitmap_h;
  int advance_x;
  bool valid;
} GlyphEntry;

typedef struct {
  GlyphEntry entries[GLYPH_CACHE_SIZE];
  FT_Face face;
  SDL_Renderer *renderer;
  int space_advance;
} GlyphCache;

static void glyph_cache_init(GlyphCache *gc, FT_Face face,
                             SDL_Renderer *renderer) {
  memset(gc, 0, sizeof(*gc));
  gc->face = face;
  gc->renderer = renderer;
  gc->space_advance = 10;
  if (FT_Load_Char(face, ' ', FT_LOAD_DEFAULT) == 0)
    gc->space_advance = (int)(face->glyph->advance.x >> 6);
}

static GlyphEntry *glyph_cache_get(GlyphCache *gc, uint32_t cp) {
  uint32_t slot = cp % GLYPH_CACHE_SIZE;
  GlyphEntry *e = &gc->entries[slot];
  if (e->valid && e->codepoint == cp)
    return e;

  /* Evict */
  if (e->tex) {
    SDL_DestroyTexture(e->tex);
    e->tex = NULL;
  }
  e->valid = false;
  e->codepoint = cp;

  if (FT_Load_Char(gc->face, cp, FT_LOAD_RENDER) != 0) {
    e->advance_x = gc->space_advance;
    e->valid = true;
    return e;
  }
  FT_GlyphSlot g = gc->face->glyph;
  e->advance_x = (int)(g->advance.x >> 6);
  e->bitmap_left = g->bitmap_left;
  e->bitmap_top = g->bitmap_top;
  e->bitmap_w = (int)g->bitmap.width;
  e->bitmap_h = (int)g->bitmap.rows;

  if (g->bitmap.width > 0 && g->bitmap.rows > 0) {
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, (int)g->bitmap.width,
                                                       (int)g->bitmap.rows, 32,
                                                       SDL_PIXELFORMAT_RGBA32);
    if (surf) {
      SDL_LockSurface(surf);
      Uint32 *px = (Uint32 *)surf->pixels;
      int pw = surf->pitch / 4;
      for (int gy = 0; gy < e->bitmap_h; gy++)
        for (int gx = 0; gx < e->bitmap_w; gx++) {
          unsigned char a = g->bitmap.buffer[gy * g->bitmap.pitch + gx];
          px[gy * pw + gx] = SDL_MapRGBA(surf->format, 255, 255, 255, a);
        }
      SDL_UnlockSurface(surf);
      e->tex = SDL_CreateTextureFromSurface(gc->renderer, surf);
      if (e->tex)
        SDL_SetTextureBlendMode(e->tex, SDL_BLENDMODE_BLEND);
      SDL_FreeSurface(surf);
    }
  }
  e->valid = true;
  return e;
}

static void glyph_cache_destroy(GlyphCache *gc) {
  for (int i = 0; i < GLYPH_CACHE_SIZE; i++) {
    if (gc->entries[i].tex)
      SDL_DestroyTexture(gc->entries[i].tex);
  }
  memset(gc, 0, sizeof(*gc));
}

static void render_glyph_cached(GlyphCache *gc, uint32_t cp, int *x, int y,
                                Uint8 r, Uint8 g, Uint8 b) {
  if (cp == '\t') {
    *x += gc->space_advance * 4;
    return;
  }
  if (cp == ' ') {
    *x += gc->space_advance;
    return;
  }

  GlyphEntry *e = glyph_cache_get(gc, cp);
  if (!e)
    return;

  if (e->tex) {
    SDL_SetTextureColorMod(e->tex, r, g, b);
    SDL_Rect dst = {*x + e->bitmap_left, y - e->bitmap_top, e->bitmap_w,
                    e->bitmap_h};
    SDL_RenderCopy(gc->renderer, e->tex, NULL, &dst);
  }
  *x += e->advance_x;
}

/* Measure a string's pixel width using the cache (no render). */
static int measure_string(GlyphCache *gc, const char *s, int len) {
  int w = 0;
  for (int i = 0; i < len;) {
    unsigned char lead = (unsigned char)s[i];
    int seq = utf8_seq_len(lead);
    uint32_t cp = 0;
    if (seq == 1)
      cp = lead & 0x7F;
    else if (seq == 2)
      cp = lead & 0x1F;
    else if (seq == 3)
      cp = lead & 0x0F;
    else
      cp = lead & 0x07;
    for (int k = 1; k < seq && i + k < len; k++) {
      unsigned char c = (unsigned char)s[i + k];
      if ((c & 0xC0) != 0x80) {
        seq = k;
        break;
      }
      cp = (cp << 6) | (c & 0x3F);
    }
    if (cp == ' ' || cp == '\t')
      w += (cp == '\t') ? gc->space_advance * 4 : gc->space_advance;
    else {
      GlyphEntry *e = glyph_cache_get(gc, cp);
      if (e)
        w += e->advance_x;
    }
    i += seq;
  }
  return w;
}

/* =====================================================================
 * Line: gap buffer over bytes
 * ===================================================================== */
typedef struct {
  char *buf;
  int gap_start;
  int gap_end;
  int cap;
  /* x-position cache: advance_x[i] = pixel x at logical byte i.
     Length = cap + 1; rebuilt lazily (dirty flag) or when base_x
     (gutter width) changes. */
  int *xcache;
  bool xcache_dirty;
  int xcache_base_x; /* absolute origin the cache was built against */
} Line;

static int line_len(const Line *l) {
  return l->cap - (l->gap_end - l->gap_start);
}
static int line_gap(const Line *l) { return l->gap_end - l->gap_start; }
static int line_phys(const Line *l, int log) {
  if (log < 0 || log > line_len(l))
    return 0;
  return log < l->gap_start ? log : log + line_gap(l);
}
static char line_at(const Line *l, int log) {
  if (log < 0 || log >= line_len(l))
    return '\0';
  return l->buf[line_phys(l, log)];
}

static bool line_init(Line *l) {
  l->buf = calloc(LINE_INIT_CAP, 1);
  if (!l->buf)
    return false;
  l->xcache = calloc(LINE_INIT_CAP + 1, sizeof(int));
  if (!l->xcache) {
    free(l->buf);
    l->buf = NULL;
    return false;
  }
  l->gap_start = 0;
  l->gap_end = LINE_INIT_CAP;
  l->cap = LINE_INIT_CAP;
  l->xcache_dirty = true;
  l->xcache_base_x = -1;
  return true;
}

static void line_free(Line *l) {
  free(l->buf);
  free(l->xcache);
  memset(l, 0, sizeof(*l));
}

static bool line_grow(Line *l, int needed) {
  int gap = line_gap(l);
  if (gap >= needed)
    return true;
  int content = line_len(l);
  int new_cap = l->cap ? l->cap * 2 : LINE_INIT_CAP;
  while (new_cap - content < needed)
    new_cap *= 2;
  char *nb = realloc(l->buf, (size_t)new_cap);
  if (!nb)
    return false;
  int *nc = realloc(l->xcache, (size_t)(new_cap + 1) * sizeof(int));
  if (!nc) {
    l->buf = nb;
    return false;
  }
  int suffix = l->cap - l->gap_end;
  int new_gap_end = new_cap - suffix;
  if (suffix > 0)
    memmove(&nb[new_gap_end], &nb[l->gap_end], (size_t)suffix);
  memset(&nb[l->gap_start], 0, (size_t)(new_gap_end - l->gap_start));
  l->buf = nb;
  l->xcache = nc;
  l->gap_end = new_gap_end;
  l->cap = new_cap;
  l->xcache_dirty = true;
  return true;
}

static void line_move_gap(Line *l, int target) {
  if (target == l->gap_start)
    return;
  int gap = line_gap(l);
  if (target < l->gap_start) {
    memmove(&l->buf[target + gap], &l->buf[target],
            (size_t)(l->gap_start - target));
  } else {
    int tp = target + gap;
    memmove(&l->buf[l->gap_start], &l->buf[l->gap_end],
            (size_t)(tp - l->gap_end));
  }
  l->gap_end = target + gap;
  l->gap_start = target;
  memset(&l->buf[l->gap_start], 0, (size_t)gap);
  l->xcache_dirty = true;
}

static bool line_insert(Line *l, int at, const char *s, int n) {
  if (n <= 0)
    return true;
  if (!line_grow(l, n))
    return false;
  line_move_gap(l, at);
  memcpy(&l->buf[l->gap_start], s, (size_t)n);
  l->gap_start += n;
  l->xcache_dirty = true;
  return true;
}

static void line_delete(Line *l, int at, int n) {
  if (n <= 0)
    return;
  line_move_gap(l, at);
  memset(&l->buf[l->gap_end], 0, (size_t)n);
  l->gap_end += n;
  l->xcache_dirty = true;
}

static void line_truncate(Line *l, int newlen) {
  int cur = line_len(l);
  if (newlen >= cur)
    return;
  line_delete(l, newlen, cur - newlen);
}

/* Copy logical content into a flat buffer (no NUL). Returns bytes written. */
static int line_flatten(const Line *l, char *out, int out_cap) {
  int len = line_len(l);
  int n = len < out_cap ? len : out_cap;
  for (int i = 0; i < n; i++)
    out[i] = line_at(l, i);
  return n;
}

/* =====================================================================
 * X-position cache rebuild
 * ===================================================================== */
static void line_rebuild_xcache(Line *l, GlyphCache *gc, int base_x) {
  if (!l->xcache_dirty && l->xcache_base_x == base_x)
    return;
  int len = line_len(l);
  int x = base_x;
  l->xcache[0] = x;
  for (int i = 0; i < len;) {
    unsigned char lead = (unsigned char)line_at(l, i);
    int seq = utf8_seq_len(lead);
    uint32_t cp = 0;
    if (seq == 1)
      cp = lead & 0x7F;
    else if (seq == 2)
      cp = lead & 0x1F;
    else if (seq == 3)
      cp = lead & 0x0F;
    else
      cp = lead & 0x07;
    for (int k = 1; k < seq; k++) {
      unsigned char c = (unsigned char)line_at(l, i + k);
      if ((c & 0xC0) != 0x80) {
        seq = k;
        break;
      }
      cp = (cp << 6) | (c & 0x3F);
    }
    int adv;
    if (cp == '\t')
      adv = gc->space_advance * 4;
    else if (cp == ' ')
      adv = gc->space_advance;
    else {
      GlyphEntry *e = glyph_cache_get(gc, cp);
      adv = e ? e->advance_x : gc->space_advance;
    }
    for (int k = 0; k < seq; k++) {
      int idx = i + k + 1;
      if (idx <= l->cap)
        l->xcache[idx] = x + adv;
    }
    x += adv;
    i += seq;
  }
  if (len <= l->cap)
    l->xcache[len] = x;
  l->xcache_dirty = false;
  l->xcache_base_x = base_x;
}

static int line_x_at(Line *l, GlyphCache *gc, int base_x, int col) {
  line_rebuild_xcache(l, gc, base_x);
  int len = line_len(l);
  if (col <= 0)
    return base_x;
  if (col > len)
    col = len;
  return l->xcache[col];
}

static int line_col_at_x(Line *l, GlyphCache *gc, int base_x, int target_x) {
  line_rebuild_xcache(l, gc, base_x);
  int len = line_len(l);
  int best = 0, best_dist = abs(l->xcache[0] - target_x);
  for (int i = 0; i < len;) {
    unsigned char lead = (unsigned char)line_at(l, i);
    int seq = utf8_seq_len(lead);
    int nc = i + seq;
    if (nc > l->cap)
      nc = len;
    int dist = abs(l->xcache[nc] - target_x);
    if (dist < best_dist) {
      best_dist = dist;
      best = nc;
    }
    i = nc;
  }
  return best;
}

static int line_utf8_prev(const Line *l, int pos) {
  if (pos <= 0)
    return 0;
  int p = pos - 1;
  while (p > 0 && ((unsigned char)line_at(l, p) & 0xC0) == 0x80)
    p--;
  return p;
}

static int line_utf8_next(const Line *l, int pos) {
  int len = line_len(l);
  if (pos >= len)
    return len;
  unsigned char lead = (unsigned char)line_at(l, pos);
  int seq = utf8_seq_len(lead);
  return (pos + seq > len) ? len : pos + seq;
}

static int line_word_start(const Line *l, int pos) {
  while (pos > 0) {
    int p = line_utf8_prev(l, pos);
    unsigned char lead = (unsigned char)line_at(l, p);
    uint32_t cp = lead; /* ASCII sufficient for word boundary */
    if (!is_word_char(cp))
      break;
    pos = p;
  }
  return pos;
}

static int line_word_end(const Line *l, int pos) {
  int len = line_len(l);
  while (pos < len) {
    unsigned char lead = (unsigned char)line_at(l, pos);
    uint32_t cp = lead;
    if (!is_word_char(cp))
      break;
    pos = line_utf8_next(l, pos);
  }
  return pos;
}

/* =====================================================================
 * LineBuf: gap buffer of Line* pointers
 * Gives O(1) amortised line insert/delete near cursor.
 * ===================================================================== */
typedef struct {
  Line **lines; /* array of Line* with a gap */
  int gs;       /* gap start (in index units) */
  int ge;       /* gap end */
  int cap;      /* total allocated slots */
} LineBuf;

static int lb_count(const LineBuf *lb) { return lb->cap - (lb->ge - lb->gs); }

static Line *lb_get(const LineBuf *lb, int row) {
  int n = lb_count(lb);
  if (row < 0 || row >= n)
    return NULL;
  return lb->lines[row < lb->gs ? row : row + (lb->ge - lb->gs)];
}

static bool lb_grow(LineBuf *lb, int needed) {
  int count = lb_count(lb);
  int gap = lb->ge - lb->gs;
  if (gap >= needed)
    return true;
  int new_cap = lb->cap ? lb->cap * 2 : LINEBUF_INIT_CAP;
  while (new_cap - count < needed)
    new_cap *= 2;
  Line **nb = realloc(lb->lines, (size_t)new_cap * sizeof(Line *));
  if (!nb)
    return false;
  int suffix = lb->cap - lb->ge;
  int new_ge = new_cap - suffix;
  if (suffix > 0)
    memmove(&nb[new_ge], &nb[lb->ge], (size_t)suffix * sizeof(Line *));
  memset(&nb[lb->gs], 0, (size_t)(new_ge - lb->gs) * sizeof(Line *));
  lb->lines = nb;
  lb->ge = new_ge;
  lb->cap = new_cap;
  return true;
}

static void lb_move_gap(LineBuf *lb, int target) {
  if (target == lb->gs)
    return;
  int gap = lb->ge - lb->gs;
  if (target < lb->gs) {
    memmove(&lb->lines[target + gap], &lb->lines[target],
            (size_t)(lb->gs - target) * sizeof(Line *));
  } else {
    memmove(&lb->lines[lb->gs], &lb->lines[lb->ge],
            (size_t)(target - lb->gs) * sizeof(Line *));
  }
  lb->ge = target + gap;
  lb->gs = target;
  memset(&lb->lines[lb->gs], 0, (size_t)gap * sizeof(Line *));
}

/* Insert a NEW Line at row. Takes ownership of `l`. */
static bool lb_insert_line(LineBuf *lb, int row, Line *l) {
  if (!lb_grow(lb, 1))
    return false;
  lb_move_gap(lb, row);
  lb->lines[lb->gs++] = l;
  return true;
}

/* Remove and free the Line at row. */
static void lb_delete_line(LineBuf *lb, int row) {
  int n = lb_count(lb);
  if (row < 0 || row >= n)
    return;
  lb_move_gap(lb, row);
  line_free(lb->lines[lb->ge]);
  free(lb->lines[lb->ge]);
  lb->lines[lb->ge] = NULL;
  lb->ge++;
}

static void lb_init(LineBuf *lb) { memset(lb, 0, sizeof(*lb)); }

static void lb_destroy(LineBuf *lb) {
  int n = lb_count(lb);
  for (int i = 0; i < n; i++) {
    Line *l = lb_get(lb, i);
    if (l) {
      line_free(l);
      free(l);
    }
  }
  free(lb->lines);
  memset(lb, 0, sizeof(*lb));
}

/* =====================================================================
 * Syntax highlighting
 * ===================================================================== */
typedef enum {
  TK_PLAIN,
  TK_KEYWORD,
  TK_TYPE,
  TK_STRING,
  TK_CHAR_LIT,
  TK_COMMENT,
  TK_NUMBER,
  TK_PREPROC,
  TK_BRACKET,
} TokKind;

static const char *const C_KEYWORDS[] = {
    "auto",     "break",      "case",      "const",          "continue",
    "default",  "do",         "else",      "enum",           "extern",
    "for",      "goto",       "if",        "inline",         "register",
    "restrict", "return",     "sizeof",    "static",         "struct",
    "switch",   "typedef",    "union",     "volatile",       "while",
    "_Alignas", "_Alignof",   "_Atomic",   "_Bool",          "_Complex",
    "_Generic", "_Imaginary", "_Noreturn", "_Static_assert", "_Thread_local",
    NULL};
static const char *const C_TYPES[] = {"bool",          "char",
                                      "double",        "float",
                                      "int",           "long",
                                      "short",         "signed",
                                      "unsigned",      "void",
                                      "size_t",        "ssize_t",
                                      "ptrdiff_t",     "intptr_t",
                                      "uintptr_t",     "int8_t",
                                      "int16_t",       "int32_t",
                                      "int64_t",       "uint8_t",
                                      "uint16_t",      "uint32_t",
                                      "uint64_t",      "int_fast8_t",
                                      "int_fast16_t",  "int_fast32_t",
                                      "int_fast64_t",  "uint_fast8_t",
                                      "uint_fast16_t", "uint_fast32_t",
                                      "uint_fast64_t", "FILE",
                                      "Uint8",         "Uint16",
                                      "Uint32",        "Uint64",
                                      "Sint8",         "Sint16",
                                      "Sint32",        "Sint64",
                                      "FT_Face",       "FT_Library",
                                      "SDL_Renderer",  "SDL_Window",
                                      "SDL_Texture",   NULL};

static bool word_matches(const char *const *table, const char *s, int len) {
  for (int i = 0; table[i]; i++) {
    if ((int)strlen(table[i]) == len && memcmp(table[i], s, (size_t)len) == 0)
      return true;
  }
  return false;
}

/* Returns kind + length in bytes for token starting at `col`. */
static TokKind tok_at(const Line *l, int col, int *out_len) {
  int len = line_len(l);
  if (col >= len) {
    *out_len = 1;
    return TK_PLAIN;
  }
  char c = line_at(l, col);

  /* Comment */
  if (c == '/' && col + 1 < len && line_at(l, col + 1) == '/') {
    *out_len = len - col;
    return TK_COMMENT;
  }
  /* Block comment */
  if (c == '/' && col + 1 < len && line_at(l, col + 1) == '*') {
    int n = 2;
    while (col + n < len - 1) {
      if (line_at(l, col + n) == '*' && line_at(l, col + n + 1) == '/') {
        n += 2;
        break;
      }
      n++;
    }
    *out_len = n;
    return TK_COMMENT;
  }
  /* Preprocessor */
  if (c == '#') {
    int n = 1;
    while (col + n < len && (is_word_char((unsigned char)line_at(l, col + n)) ||
                             line_at(l, col + n) == ' '))
      n++;
    *out_len = n;
    return TK_PREPROC;
  }
  /* String */
  if (c == '"') {
    int n = 1;
    while (col + n < len) {
      char sc = line_at(l, col + n++);
      if (sc == '\\')
        n++;
      else if (sc == '"')
        break;
    }
    *out_len = n;
    return TK_STRING;
  }
  /* Char literal */
  if (c == '\'') {
    int n = 1;
    while (col + n < len) {
      char sc = line_at(l, col + n++);
      if (sc == '\\')
        n++;
      else if (sc == '\'')
        break;
    }
    *out_len = n;
    return TK_CHAR_LIT;
  }
  /* Number */
  if ((c >= '0' && c <= '9') ||
      (c == '.' && col + 1 < len && line_at(l, col + 1) >= '0' &&
       line_at(l, col + 1) <= '9')) {
    int n = 0;
    while (col + n < len) {
      char nc = line_at(l, col + n);
      if (!((nc >= '0' && nc <= '9') || nc == '.' || nc == 'x' || nc == 'X' ||
            nc == 'a' || nc == 'b' || nc == 'c' || nc == 'd' || nc == 'e' ||
            nc == 'f' || nc == 'A' || nc == 'B' || nc == 'C' || nc == 'D' ||
            nc == 'E' || nc == 'F' || nc == 'u' || nc == 'U' || nc == 'l' ||
            nc == 'L' || nc == '_'))
        break;
      n++;
    }
    *out_len = n ? n : 1;
    return TK_NUMBER;
  }
  /* Bracket */
  if (c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}') {
    *out_len = 1;
    return TK_BRACKET;
  }
  /* Identifier / keyword / type */
  if (is_word_char((unsigned char)c) &&
      (col == 0 || !is_word_char((unsigned char)line_at(l, col - 1)))) {
    int wlen = 0;
    while (col + wlen < len &&
           is_word_char((unsigned char)line_at(l, col + wlen)))
      wlen++;
    char word[128] = {0};
    if (wlen < 128) {
      for (int i = 0; i < wlen; i++)
        word[i] = line_at(l, col + i);
      *out_len = wlen;
      if (word_matches(C_KEYWORDS, word, wlen))
        return TK_KEYWORD;
      if (word_matches(C_TYPES, word, wlen))
        return TK_TYPE;
    } else {
      *out_len = wlen;
    }
    return TK_PLAIN;
  }
  *out_len = 1;
  return TK_PLAIN;
}

typedef struct {
  Uint8 r, g, b;
} Col3;

/* =====================================================================
 * Theme — single source of truth for the visual design language.
 * Dark, calm, high-hierarchy: one hue family for surfaces, four
 * semantic accents (blue/green/red/amber) used consistently everywhere.
 * ===================================================================== */
typedef struct {
  Uint8 r, g, b;
} Color;

typedef struct {
  /* Surfaces */
  Color bg;            /* editor background */
  Color bg_elevated;   /* panels/dialogs/status bar */
  Color bg_input;      /* text inputs */
  Color bg_band;       /* current-line highlight */
  Color scrim;         /* modal backdrop */
  /* Lines */
  Color border_subtle;
  Color border_strong;
  /* Text */
  Color text_primary;
  Color text_secondary;
  Color text_muted;
  /* Semantic accents */
  Color accent;        /* interactive blue */
  Color success;       /* save/recover green */
  Color danger;        /* delete/error red */
  Color warning;       /* dirty/search amber */
  /* Editor chrome */
  Color selection;
  Color cursor;
  Color gutter_rule;
  Color search_hl;
  Color search_current;
  Color bracket;
} Theme;

static const Theme THEMET = {
    .bg = {18, 20, 26},
    .bg_elevated = {26, 29, 38},
    .bg_input = {21, 24, 33},
    .bg_band = {25, 28, 37},
    .scrim = {8, 9, 14},
    .border_subtle = {38, 43, 56},
    .border_strong = {58, 65, 82},
    .text_primary = {230, 233, 240},
    .text_secondary = {169, 177, 194},
    .text_muted = {92, 101, 119},
    .accent = {91, 157, 255},
    .success = {76, 195, 138},
    .danger = {242, 113, 115},
    .warning = {232, 179, 75},
    .selection = {62, 99, 170},
    .cursor = {230, 233, 240},
    .gutter_rule = {32, 36, 47},
    .search_hl = {120, 96, 40},
    .search_current = {201, 145, 44},
    .bracket = {72, 88, 130},
};

static Col3 tok_color(TokKind k) {
  switch (k) {
  case TK_KEYWORD:
    return (Col3){198, 120, 221}; /* purple */
  case TK_TYPE:
    return (Col3){108, 182, 255}; /* light blue */
  case TK_STRING:
    return (Col3){152, 195, 121}; /* green */
  case TK_CHAR_LIT:
    return (Col3){209, 154, 102}; /* orange */
  case TK_COMMENT:
    return (Col3){112, 125, 140}; /* cool gray */
  case TK_NUMBER:
    return (Col3){209, 154, 102};
  case TK_PREPROC:
    return (Col3){191, 143, 255}; /* violet */
  case TK_BRACKET:
    return (Col3){171, 178, 191}; /* fg-dim */
  default:
    return (Col3){220, 223, 228};
  }
}

/* =====================================================================
 * Bracket matching
 * ===================================================================== */
static char bracket_pair(char c) {
  switch (c) {
  case '(':
    return ')';
  case ')':
    return '(';
  case '[':
    return ']';
  case ']':
    return '[';
  case '{':
    return '}';
  case '}':
    return '{';
  }
  return 0;
}
static bool bracket_is_open(char c) { return c == '(' || c == '[' || c == '{'; }

/* =====================================================================
 * Text position
 * ===================================================================== */
typedef struct {
  int row, col;
} Pos;

static bool pos_lt(Pos a, Pos b) {
  return a.row < b.row || (a.row == b.row && a.col < b.col);
}
static Pos pos_min(Pos a, Pos b) { return pos_lt(a, b) ? a : b; }
static Pos pos_max(Pos a, Pos b) { return pos_lt(a, b) ? b : a; }

/* =====================================================================
 * Undo/redo: operation log
 * Each record captures minimal information to invert one edit.
 * Full-document snapshots only on paste/replace-all; char inserts
 * and deletes store the actual bytes.
 * ===================================================================== */
typedef enum {
  OP_INSERT,   /* inserted bytes at (row,col) */
  OP_DELETE,   /* deleted bytes that were at (row,col) */
  OP_SPLIT,    /* split line at (row,col) -> new line row+1 */
  OP_JOIN,     /* join line row+1 onto end of row */
  OP_SNAPSHOT, /* full document snapshot (paste etc.) */
} OpKind;

typedef struct {
  OpKind kind;
  Pos pos;           /* position of operation */
  Pos cursor_before; /* cursor before this op */
  Pos cursor_after;  /* cursor after this op (for redo) */
  /* For INSERT/DELETE: the actual bytes */
  char *bytes;
  int nbytes;
  /* For SNAPSHOT: serialized flat text */
  char *snap;
  size_t snap_len;
} UndoOp;

typedef struct {
  UndoOp *ops;
  int cap;
  int len;       /* total ops ever pushed (used as write head mod cap) */
  int undo_head; /* how many can be undone: ops[(undo_head-1) % cap] */
} UndoRing;

static void uring_init(UndoRing *r) {
  r->cap = UNDO_RING_SIZE;
  r->ops = calloc((size_t)r->cap, sizeof(UndoOp));
  r->len = 0;
  r->undo_head = 0;
}

static void uring_free_op(UndoOp *op) {
  free(op->bytes);
  op->bytes = NULL;
  free(op->snap);
  op->snap = NULL;
}

static void uring_destroy(UndoRing *r) {
  if (!r->ops)
    return;
  for (int i = 0; i < r->cap; i++)
    uring_free_op(&r->ops[i]);
  free(r->ops);
  memset(r, 0, sizeof(*r));
}

static UndoOp *uring_push(UndoRing *r) {
  /* Evict the slot we're about to overwrite */
  int slot = r->undo_head % r->cap;
  uring_free_op(&r->ops[slot]);
  UndoOp *op = &r->ops[slot];
  memset(op, 0, sizeof(*op));
  r->undo_head++;
  r->len = r->undo_head; /* pushing clears redo history */
  return op;
}

/* Index of the oldest still-intact op: once more ops have been pushed
   than the ring holds, the oldest slots were overwritten by newer pushes
   and must not be replayed by undo. */
static int uring_oldest_valid(const UndoRing *r) {
  return r->len > r->cap ? r->len - r->cap : 0;
}

static bool uring_can_undo(const UndoRing *r) {
  return r->undo_head > uring_oldest_valid(r);
}
static bool uring_can_redo(const UndoRing *r) { return r->len > r->undo_head; }

/* =====================================================================
 * Search result
 * ===================================================================== */
typedef struct {
  Pos start;
  int len;
} SearchResult;

/* =====================================================================
 * Command palette entry
 * ===================================================================== */
typedef void (*CmdFn)(void *editor);
typedef struct {
  const char *label;
  CmdFn fn;
} PaletteEntry;

/* =====================================================================
 * Editor modes
 * ===================================================================== */
typedef enum {
  MODE_NORMAL,
  MODE_INSERT,
  MODE_SAVE_PROMPT,
  MODE_OPEN_PROMPT,
  MODE_FIND,
  MODE_REPLACE,
  MODE_RECOVERY_PROMPT,
  MODE_QUIT_PROMPT,
  MODE_VERSION_BROWSER,
  MODE_VERSION_RENAME,
  MODE_PALETTE,
  MODE_GOTO_LINE,
} EditorMode;

/* =====================================================================
 * Main Editor struct
 * ===================================================================== */
typedef struct {
  LineBuf lb;
  Pos cursor;
  Pos sel_anchor;
  bool sel_active;

  int preferred_x; /* pixel x to maintain across up/down */
  Uint32 last_vert_move;

  int scroll_row;
  int text_x; /* pixel x where text starts (after gutter) */

  char filename[1024];
  bool dirty;
  LangID lang; /* detected language for current file */

  char status_msg[1024];
  Uint32 status_msg_time;

  EditorMode mode;
  EditorMode mode_pre_overlay; /* mode before dialog/palette opened */

  /* Prompt buffers — must be at least as wide as filename */
  char prompt[1024];
  char replace[1024];
  int prompt_field; /* 0=find,1=replace in replace mode */

  bool quit_requested; /* set by the quit prompt; main loop exits */

  /* Swallows the TEXTINPUT generated by the very keypress that entered
     Insert mode ('i'/'a'), so switching modes never types a character. */
  bool suppress_text_input;

  /* Undo/redo */
  UndoRing uring;

  /* Storage */
  StorageSession *storage;
  Uint32 last_journal_ms;
  bool journal_pending;
  bool journal_error_shown; /* throttles repeated journal-failure warnings */
  ByteBuffer rec_doc;
  StorageMetadata rec_meta;
  bool rec_available;
  StorageMetadata doc_meta;

  /* Bracket match highlight */
  Pos bmatch_open;
  Pos bmatch_close;
  bool bmatch_valid;

  /* Search */
  SearchResult search_results[SEARCH_MAX_RESULTS];
  int search_count;
  int search_current; /* index into search_results of cursor match */
  bool search_active; /* highlights visible */

  /* Pointer back to the glyph cache — palette command callbacks need it */
  GlyphCache *gc;

  /* Command palette */
  PaletteEntry palette[PALETTE_MAX];
  int palette_count;
  char palette_filter[128];
  int palette_sel;

  /* Goto line */
  char goto_buf[16];

  /* Version browser (Ctrl+Alt+V) */
  int ver_sel;      /* public index, 0 = newest */
  int ver_scroll;   /* first visible row */
  bool ver_delete_armed;
  char ver_rename_buf[STORAGE_VERSION_NAME_MAX];
  int ver_rename_target;
} Editor;

/* Forward declarations */
static void editor_set_status(Editor *e, const char *msg);
static void editor_update_search(Editor *e);
static void editor_bracket_match(Editor *e);
static void editor_update_gutter(Editor *e, GlyphCache *gc);
static void editor_push_snapshot(Editor *e);
static void editor_do_undo(Editor *e, GlyphCache *gc);
static void editor_do_redo(Editor *e, GlyphCache *gc);
static const char *stristr(const char *hay, const char *needle);
static void prompt_backspace(char *s);

/* =====================================================================
 * Editor helpers
 * ===================================================================== */
static int editor_line_count(const Editor *e) { return lb_count(&e->lb); }

static Line *editor_line(const Editor *e, int row) {
  return lb_get(&e->lb, row);
}

static void editor_clamp_cursor(Editor *e) {
  int n = editor_line_count(e);
  if (e->cursor.row < 0)
    e->cursor.row = 0;
  if (e->cursor.row >= n)
    e->cursor.row = n - 1;
  int len = line_len(editor_line(e, e->cursor.row));
  if (e->cursor.col < 0)
    e->cursor.col = 0;
  if (e->cursor.col > len)
    e->cursor.col = len;
}

static void editor_set_status(Editor *e, const char *msg) {
  snprintf(e->status_msg, sizeof(e->status_msg), "%s", msg);
  e->status_msg_time = SDL_GetTicks();
}

static void editor_mark_dirty(Editor *e) {
  e->dirty = true;
  e->journal_pending = true;
  if (e->storage)
    storage_mark_dirty(e->storage);
}

static void refresh_preferred_x(Editor *e, GlyphCache *gc) {
  Line *l = editor_line(e, e->cursor.row);
  if (l)
    e->preferred_x = line_x_at(l, gc, e->text_x, e->cursor.col);
}

/* =====================================================================
 * Scroll management
 * ===================================================================== */
static void editor_scroll_to_cursor(Editor *e, SDL_Window *win) {
  int wh;
  SDL_GetWindowSize(win, NULL, &wh);
  int vis = (wh - FIRST_LINE_Y - STATUS_BAR_HEIGHT) / LINE_HEIGHT_PX;
  if (vis < 1)
    vis = 1;
  if (e->cursor.row < e->scroll_row + SCROLL_MARGIN)
    e->scroll_row = e->cursor.row - SCROLL_MARGIN;
  if (e->cursor.row >= e->scroll_row + vis - SCROLL_MARGIN)
    e->scroll_row = e->cursor.row - vis + SCROLL_MARGIN + 1;
  if (e->scroll_row < 0)
    e->scroll_row = 0;
  int max_scroll = editor_line_count(e) - 1;
  if (e->scroll_row > max_scroll)
    e->scroll_row = max_scroll;
}

static int visible_rows(SDL_Window *win) {
  int wh;
  SDL_GetWindowSize(win, NULL, &wh);
  int v = (wh - FIRST_LINE_Y - STATUS_BAR_HEIGHT) / LINE_HEIGHT_PX;
  return v < 1 ? 1 : v;
}

/* =====================================================================
 * Gutter width (updates e->text_x)
 * ===================================================================== */
static void editor_update_gutter(Editor *e, GlyphCache *gc) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", editor_line_count(e));
  int w = measure_string(gc, buf, (int)strlen(buf));
  e->text_x = GUTTER_PAD + w + gc->space_advance * 2;
}

/* =====================================================================
 * Serialization seam (storage ↔ LineBuf)
 * ===================================================================== */
static void editor_serialize(const Editor *e, ByteBuffer *out) {
  bytebuffer_init(out);
  int n = editor_line_count(e);
  for (int r = 0; r < n; r++) {
    Line *l = editor_line(e, r);
    int len = line_len(l);
    for (int c = 0; c < len; c++) {
      char ch = line_at(l, c);
      bytebuffer_append(out, &ch, 1);
    }
    if (r < n - 1) {
      char nl = '\n';
      bytebuffer_append(out, &nl, 1);
    }
  }
}

static bool editor_deserialize(Editor *e, GlyphCache *gc, const uint8_t *data,
                               size_t len) {
  lb_destroy(&e->lb);
  lb_init(&e->lb);

  Line *cur = calloc(1, sizeof(Line));
  if (!cur || !line_init(cur)) {
    free(cur);
    return false;
  }
  if (!lb_insert_line(&e->lb, 0, cur)) {
    line_free(cur);
    free(cur);
    return false;
  }

  for (size_t i = 0; i < len; i++) {
    char c = (char)data[i];
    if (c == '\n') {
      cur = calloc(1, sizeof(Line));
      if (!cur || !line_init(cur)) {
        free(cur);
        goto fail;
      }
      if (!lb_insert_line(&e->lb, lb_count(&e->lb), cur))
        goto fail;
    } else {
      Line *l = lb_get(&e->lb, lb_count(&e->lb) - 1);
      line_insert(l, line_len(l), &c, 1);
    }
  }
  editor_update_gutter(e, gc);
  e->cursor = (Pos){0, 0};
  e->scroll_row = 0;
  e->sel_active = false;
  return true;

fail:
  /* Leave a valid (single empty line) buffer rather than a torn one. */
  lb_destroy(&e->lb);
  lb_init(&e->lb);
  cur = calloc(1, sizeof(Line));
  if (!cur || !line_init(cur)) {
    free(cur);
    return false;
  }
  if (!lb_insert_line(&e->lb, 0, cur)) {
    line_free(cur);
    free(cur);
  }
  return false;
}

/* =====================================================================
 * Undo/redo implementation
 * ===================================================================== */
static void editor_push_snapshot(Editor *e) {
  ByteBuffer buf;
  editor_serialize(e, &buf);
  UndoOp *op = uring_push(&e->uring);
  op->kind = OP_SNAPSHOT;
  op->cursor_before = e->cursor;
  op->cursor_after = e->cursor;
  op->snap = (char *)buf.data;
  op->snap_len = buf.len;
  /* buf.data ownership transferred to op */
}

/* Record a single char insert (fast path for typing). */
static void editor_push_insert(Editor *e, Pos at, const char *bytes, int n,
                               Pos after) {
  UndoOp *op = uring_push(&e->uring);
  op->kind = OP_INSERT;
  op->pos = at;
  op->cursor_before = at;
  op->cursor_after = after;
  op->bytes = malloc((size_t)n);
  if (op->bytes)
    memcpy(op->bytes, bytes, (size_t)n);
  else
    n = 0; /* OOM: record a no-op rather than a NULL-backed delete */
  op->nbytes = n;
}

static void editor_push_delete(Editor *e, Pos at, const char *bytes, int n,
                               Pos after) {
  UndoOp *op = uring_push(&e->uring);
  op->kind = OP_DELETE;
  op->pos = at;
  op->cursor_before = at;
  op->cursor_after = after;
  op->bytes = malloc((size_t)n);
  if (op->bytes)
    memcpy(op->bytes, bytes, (size_t)n);
  else
    n = 0; /* OOM: record a no-op rather than a NULL-backed insert */
  op->nbytes = n;
}

static void editor_push_split(Editor *e, Pos at, Pos after,
                              const char *indent, int indent_len) {
  UndoOp *op = uring_push(&e->uring);
  op->kind = OP_SPLIT;
  op->pos = at;
  op->cursor_before = at;
  op->cursor_after = after;
  /* The indentation copied onto the new line is part of this operation;
     without it undo/redo would leave stray whitespace mid-line. */
  if (indent && indent_len > 0) {
    op->bytes = malloc((size_t)indent_len);
    if (op->bytes) {
      memcpy(op->bytes, indent, (size_t)indent_len);
      op->nbytes = indent_len;
    }
  }
}

static void editor_push_join(Editor *e, int row, Pos after) {
  UndoOp *op = uring_push(&e->uring);
  op->kind = OP_JOIN;
  op->pos = (Pos){row, 0};
  op->cursor_before = (Pos){row + 1, 0};
  op->cursor_after = after;
}

/* Raw document mutations (no undo recording) */
static void raw_insert(Editor *e, Pos at, const char *bytes, int n) {
  Line *l = editor_line(e, at.row);
  if (!l)
    return;
  line_insert(l, at.col, bytes, n);
}

static void raw_delete(Editor *e, Pos at, int n) {
  Line *l = editor_line(e, at.row);
  if (!l)
    return;
  line_delete(l, at.col, n);
}

/* Split line at.row at column at.col. On allocation failure the document
   is left untouched. */
static void raw_split(Editor *e, Pos at) {
  Line *l = editor_line(e, at.row);
  if (!l)
    return;
  int len = line_len(l);
  if (at.col < 0 || at.col > len)
    return;
  int tail = len - at.col;
  Line *nl = calloc(1, sizeof(Line));
  if (!nl)
    return;
  if (!line_init(nl)) {
    free(nl);
    return;
  }
  if (tail > 0) {
    char *tmp = malloc((size_t)tail);
    if (!tmp) {
      line_free(nl);
      free(nl);
      return;
    }
    for (int i = 0; i < tail; i++)
      tmp[i] = line_at(l, at.col + i);
    if (!line_insert(nl, 0, tmp, tail)) {
      free(tmp);
      line_free(nl);
      free(nl);
      return;
    }
    free(tmp);
    line_truncate(l, at.col);
  }
  if (!lb_insert_line(&e->lb, at.row + 1, nl)) {
    line_free(nl);
    free(nl);
  }
}

/* Join line row+1 onto end of row. Aborts on allocation failure without
   mutating either line. */
static void raw_join(Editor *e, int row) {
  int n = editor_line_count(e);
  if (row < 0 || row + 1 >= n)
    return;
  Line *a = editor_line(e, row);
  Line *b = editor_line(e, row + 1);
  int blen = line_len(b);
  if (blen > 0) {
    char *tmp = malloc((size_t)blen);
    if (!tmp)
      return;
    for (int i = 0; i < blen; i++)
      tmp[i] = line_at(b, i);
    if (!line_insert(a, line_len(a), tmp, blen)) {
      free(tmp);
      return;
    }
    free(tmp);
  }
  lb_delete_line(&e->lb, row + 1);
}

static void editor_do_undo(Editor *e, GlyphCache *gc) {
  if (!uring_can_undo(&e->uring)) {
    editor_set_status(e, "Nothing to undo");
    return;
  }
  int slot = (e->uring.undo_head - 1) % e->uring.cap;
  e->uring.undo_head--;
  UndoOp *op = &e->uring.ops[slot];
  switch (op->kind) {
  case OP_INSERT:
    raw_delete(e, op->pos, op->nbytes);
    e->cursor = op->cursor_before;
    break;
  case OP_DELETE:
    raw_insert(e, op->pos, op->bytes, op->nbytes);
    e->cursor = op->cursor_before;
    break;
  case OP_SPLIT:
    if (op->nbytes > 0 && op->bytes)
      raw_delete(e, (Pos){op->pos.row + 1, 0}, op->nbytes);
    raw_join(e, op->pos.row);
    e->cursor = op->cursor_before;
    break;
  case OP_JOIN:
    raw_split(e, (Pos){op->pos.row, line_len(editor_line(e, op->pos.row))});
    e->cursor = op->cursor_before;
    break;
  case OP_SNAPSHOT: {
    lb_destroy(&e->lb);
    lb_init(&e->lb);
    editor_deserialize(e, gc, (uint8_t *)op->snap, op->snap_len);
    e->cursor = op->cursor_before;
    break;
  }
  }
  editor_update_gutter(e, gc);
  editor_clamp_cursor(e);
  editor_mark_dirty(e);
}

static void editor_do_redo(Editor *e, GlyphCache *gc) {
  if (!uring_can_redo(&e->uring)) {
    editor_set_status(e, "Nothing to redo");
    return;
  }
  int slot = e->uring.undo_head % e->uring.cap;
  e->uring.undo_head++;
  UndoOp *op = &e->uring.ops[slot];
  switch (op->kind) {
  case OP_INSERT:
    raw_insert(e, op->pos, op->bytes, op->nbytes);
    e->cursor = op->cursor_after;
    break;
  case OP_DELETE:
    raw_delete(e, op->pos, op->nbytes);
    e->cursor = op->cursor_after;
    break;
  case OP_SPLIT:
    raw_split(e, op->pos);
    if (op->nbytes > 0 && op->bytes)
      raw_insert(e, (Pos){op->pos.row + 1, 0}, op->bytes, op->nbytes);
    e->cursor = op->cursor_after;
    break;
  case OP_JOIN:
    raw_join(e, op->pos.row);
    e->cursor = op->cursor_after;
    break;
  case OP_SNAPSHOT: {
    lb_destroy(&e->lb);
    lb_init(&e->lb);
    editor_deserialize(e, gc, (uint8_t *)op->snap, op->snap_len);
    e->cursor = op->cursor_after;
    break;
  }
  }
  editor_update_gutter(e, gc);
  editor_clamp_cursor(e);
  editor_mark_dirty(e);
}

/* =====================================================================
 * Selection helpers
 * ===================================================================== */
static void sel_clear(Editor *e) { e->sel_active = false; }
static void sel_start_at(Editor *e) {
  if (!e->sel_active) {
    e->sel_anchor = e->cursor;
    e->sel_active = true;
  }
}
static Pos sel_lo(const Editor *e) { return pos_min(e->cursor, e->sel_anchor); }
static Pos sel_hi(const Editor *e) { return pos_max(e->cursor, e->sel_anchor); }

/* Copy selection to a malloc'd buffer. Caller frees. Returns byte count. */
static int sel_to_buf(const Editor *e, char **out) {
  if (!e->sel_active) {
    *out = NULL;
    return 0;
  }
  Pos a = sel_lo(e), b = sel_hi(e);
  /* Estimate size */
  int total = 0;
  for (int r = a.row; r <= b.row; r++) {
    Line *l = editor_line(e, r);
    int c0 = (r == a.row) ? a.col : 0;
    int c1 = (r == b.row) ? b.col : line_len(l);
    total += c1 - c0;
    if (r < b.row)
      total++; /* newline */
  }
  char *buf = malloc((size_t)(total + 1));
  if (!buf) {
    *out = NULL;
    return 0;
  }
  int w = 0;
  for (int r = a.row; r <= b.row; r++) {
    Line *l = editor_line(e, r);
    int c0 = (r == a.row) ? a.col : 0;
    int c1 = (r == b.row) ? b.col : line_len(l);
    for (int c = c0; c < c1; c++)
      buf[w++] = line_at(l, c);
    if (r < b.row)
      buf[w++] = '\n';
  }
  buf[w] = '\0';
  *out = buf;
  return w;
}

/* Delete selection, leave cursor at sel_lo. */
static void sel_delete(Editor *e, GlyphCache *gc) {
  if (!e->sel_active)
    return;
  Pos a = sel_lo(e), b = sel_hi(e);
  editor_push_snapshot(e); /* selection delete is multi-op; snapshot is safer */
  if (a.row == b.row) {
    Line *l = editor_line(e, a.row);
    line_delete(l, a.col, b.col - a.col);
  } else {
    Line *fa = editor_line(e, a.row);
    Line *fb = editor_line(e, b.row);
    int blen = line_len(fb);
    int tail = blen - b.col;
    char *tmp = NULL;
    if (tail > 0) {
      tmp = malloc((size_t)tail);
      if (!tmp)
        return; /* abort before mutating: snapshot already pushed */
    }
    /* Trim first line to a.col */
    line_truncate(fa, a.col);
    /* Append tail of last line onto first */
    if (tail > 0) {
      for (int i = 0; i < tail; i++)
        tmp[i] = line_at(fb, b.col + i);
      line_insert(fa, a.col, tmp, tail);
      free(tmp);
    }
    /* Delete rows a.row+1 .. b.row */
    for (int r = b.row; r > a.row; r--)
      lb_delete_line(&e->lb, r);
  }
  e->cursor = a;
  e->sel_active = false;
  editor_update_gutter(e, gc);
  editor_mark_dirty(e);
}

/* =====================================================================
 * Core editing operations (record undo, mutate, advance cursor)
 * ===================================================================== */

static void ed_insert_text(Editor *e, GlyphCache *gc, const char *text, int n,
                           bool preserve_indent);
static void ed_backspace(Editor *e, GlyphCache *gc, bool word);
static void ed_delete_fwd(Editor *e, GlyphCache *gc, bool word);
static void ed_newline(Editor *e, GlyphCache *gc);
static void ed_newline_raw(Editor *e, GlyphCache *gc);

static void ed_insert_text(Editor *e, GlyphCache *gc, const char *text, int n,
                           bool preserve_indent) {
  if (e->sel_active)
    sel_delete(e, gc);
  /* Split on embedded newlines */
  int start = 0;
  for (int i = 0; i <= n; i++) {
    if (i == n || text[i] == '\n') {
      int seg = i - start;
      if (seg > 0) {
        Pos at = e->cursor;
        Pos after = {at.row, at.col + seg};
        editor_push_insert(e, at, text + start, seg, after);
        raw_insert(e, at, text + start, seg);
        e->cursor = after;
      }
      if (i < n) {
        if (preserve_indent)
          ed_newline(e, gc);
        else
          ed_newline_raw(e, gc);
      }
      start = i + 1;
    }
  }
  editor_update_gutter(e, gc);
  editor_mark_dirty(e);
}

static void ed_newline(Editor *e, GlyphCache *gc) {
  Line *l = editor_line(e, e->cursor.row);
  /* Preserve indentation */
  int indent = 0;
  while (indent < line_len(l) &&
         (line_at(l, indent) == ' ' || line_at(l, indent) == '\t'))
    indent++;

  /* Capture the indent bytes BEFORE mutating so the undo record and the
     actual insertion use identical data. */
  char *ind = NULL;
  if (indent > 0) {
    ind = malloc((size_t)indent);
    if (ind)
      for (int i = 0; i < indent; i++)
        ind[i] = line_at(l, i);
    else
      indent = 0; /* OOM: proceed without indentation */
  }

  Pos at = e->cursor;
  Pos after = {at.row + 1, indent};
  editor_push_split(e, at, after, ind, indent);
  raw_split(e, at);
  if (indent > 0 && ind) {
    line_insert(editor_line(e, at.row + 1), 0, ind, indent);
  }
  free(ind);
  e->cursor = after;
  editor_update_gutter(e, gc);
  editor_mark_dirty(e);
}

/* Split at the cursor with no indentation handling; used by paste so
   clipboard content lands byte-for-byte. */
static void ed_newline_raw(Editor *e, GlyphCache *gc) {
  (void)gc;
  Pos at = e->cursor;
  Pos after = {at.row + 1, 0};
  editor_push_split(e, at, after, NULL, 0);
  raw_split(e, at);
  e->cursor = after;
}

static void ed_backspace(Editor *e, GlyphCache *gc, bool word) {
  if (e->sel_active) {
    sel_delete(e, gc);
    return;
  }
  Line *l = editor_line(e, e->cursor.row);
  if (e->cursor.col > 0) {
    int col = e->cursor.col;
    int del_start;
    if (word) {
      /* Skip whitespace, then word chars */
      del_start = col;
      while (del_start > 0 &&
             !is_word_char((unsigned char)line_at(l, del_start - 1)))
        del_start = line_utf8_prev(l, del_start);
      while (del_start > 0 &&
             is_word_char((unsigned char)line_at(l, del_start - 1)))
        del_start = line_utf8_prev(l, del_start);
    } else {
      del_start = line_utf8_prev(l, col);
    }
    int nbytes = col - del_start;
    char *tmp = malloc((size_t)nbytes);
    if (!tmp)
      return; /* abort rather than delete without an undo record */
    for (int i = 0; i < nbytes; i++)
      tmp[i] = line_at(l, del_start + i);
    Pos at = {e->cursor.row, del_start};
    Pos after = at;
    editor_push_delete(e, at, tmp, nbytes, after);
    free(tmp);
    raw_delete(e, at, nbytes);
    e->cursor.col = del_start;
  } else if (e->cursor.row > 0) {
    int prev_len = line_len(editor_line(e, e->cursor.row - 1));
    Pos after = {e->cursor.row - 1, prev_len};
    editor_push_join(e, e->cursor.row - 1, after);
    raw_join(e, e->cursor.row - 1);
    e->cursor = after;
  }
  editor_update_gutter(e, gc);
  editor_mark_dirty(e);
}

static void ed_delete_fwd(Editor *e, GlyphCache *gc, bool word) {
  if (e->sel_active) {
    sel_delete(e, gc);
    return;
  }
  Line *l = editor_line(e, e->cursor.row);
  int len = line_len(l);
  if (e->cursor.col < len) {
    int col = e->cursor.col;
    int del_end;
    if (word) {
      del_end = col;
      while (del_end < len && !is_word_char((unsigned char)line_at(l, del_end)))
        del_end = line_utf8_next(l, del_end);
      while (del_end < len && is_word_char((unsigned char)line_at(l, del_end)))
        del_end = line_utf8_next(l, del_end);
    } else {
      del_end = line_utf8_next(l, col);
    }
    int nbytes = del_end - col;
    char *tmp = malloc((size_t)nbytes);
    if (!tmp)
      return; /* abort rather than delete without an undo record */
    for (int i = 0; i < nbytes; i++)
      tmp[i] = line_at(l, col + i);
    Pos at = e->cursor;
    editor_push_delete(e, at, tmp, nbytes, at);
    free(tmp);
    raw_delete(e, at, nbytes);
  } else if (e->cursor.row < editor_line_count(e) - 1) {
    Pos after = e->cursor;
    editor_push_join(e, e->cursor.row, after);
    raw_join(e, e->cursor.row);
  }
  editor_update_gutter(e, gc);
  editor_mark_dirty(e);
}

static void ed_kill_line(Editor *e, GlyphCache *gc) {
  (void)gc;
  Line *l = editor_line(e, e->cursor.row);
  int len = line_len(l);
  if (e->cursor.col < len) {
    int nbytes = len - e->cursor.col;
    char *tmp = malloc((size_t)nbytes);
    if (!tmp)
      return; /* abort rather than delete without an undo record */
    for (int i = 0; i < nbytes; i++)
      tmp[i] = line_at(l, e->cursor.col + i);
    Pos at = e->cursor;
    editor_push_delete(e, at, tmp, nbytes, at);
    raw_delete(e, at, nbytes);
    SDL_SetClipboardText(tmp);
    free(tmp);
  } else if (e->cursor.row < editor_line_count(e) - 1) {
    Pos after = e->cursor;
    editor_push_join(e, e->cursor.row, after);
    raw_join(e, e->cursor.row);
  }
  editor_mark_dirty(e);
}

/* =====================================================================
 * Indentation
 * ===================================================================== */
static void ed_indent_lines(Editor *e, GlyphCache *gc, bool unindent) {
  (void)gc;
  editor_push_snapshot(e);
  int r0, r1;
  if (e->sel_active) {
    r0 = sel_lo(e).row;
    r1 = sel_hi(e).row;
  } else {
    r0 = r1 = e->cursor.row;
  }
  for (int r = r0; r <= r1; r++) {
    Line *l = editor_line(e, r);
    if (unindent) {
      int rm = 0;
      for (int i = 0; i < 4 && line_at(l, 0) == ' '; i++) {
        line_delete(l, 0, 1);
        rm++;
      }
      if (r == e->cursor.row) {
        e->cursor.col -= rm;
        if (e->cursor.col < 0)
          e->cursor.col = 0;
      }
      if (e->sel_active && r == e->sel_anchor.row) {
        e->sel_anchor.col -= rm;
        if (e->sel_anchor.col < 0)
          e->sel_anchor.col = 0;
      }
    } else {
      line_insert(l, 0, "    ", 4);
      if (r == e->cursor.row)
        e->cursor.col += 4;
      if (e->sel_active && r == e->sel_anchor.row)
        e->sel_anchor.col += 4;
    }
  }
  editor_mark_dirty(e);
}

/* =====================================================================
 * Toggle // comment on current line
 * ===================================================================== */
static void ed_toggle_comment(Editor *e, GlyphCache *gc) {
  editor_push_snapshot(e);
  Line *l = editor_line(e, e->cursor.row);
  int len = line_len(l);
  int fs = 0;
  while (fs < len && (line_at(l, fs) == ' ' || line_at(l, fs) == '\t'))
    fs++;
  bool commented =
      (fs + 1 < len && line_at(l, fs) == '/' && line_at(l, fs + 1) == '/');
  if (commented) {
    int skip = (fs + 2 < len && line_at(l, fs + 2) == ' ') ? 3 : 2;
    line_delete(l, fs, skip);
    e->cursor.col -= skip;
    if (e->cursor.col < 0)
      e->cursor.col = 0;
  } else {
    line_insert(l, fs, "// ", 3);
    if (e->cursor.col >= fs)
      e->cursor.col += 3;
  }
  editor_mark_dirty(e);
  (void)gc;
}

/* =====================================================================
 * Duplicate / move lines
 * ===================================================================== */
static void ed_duplicate_line(Editor *e, GlyphCache *gc) {
  editor_push_snapshot(e);
  Line *src = editor_line(e, e->cursor.row);
  int slen = line_len(src);
  char *tmp = NULL;
  if (slen > 0) {
    tmp = malloc((size_t)slen);
    if (!tmp)
      return;
    for (int i = 0; i < slen; i++)
      tmp[i] = line_at(src, i);
  }
  Line *nl = calloc(1, sizeof(Line));
  if (!nl || !line_init(nl)) {
    free(tmp);
    free(nl);
    return;
  }
  bool copied = true;
  if (slen > 0)
    copied = line_insert(nl, 0, tmp, slen);
  free(tmp);
  if (!copied || !lb_insert_line(&e->lb, e->cursor.row + 1, nl)) {
    line_free(nl);
    free(nl);
    return;
  }
  e->cursor.row++;
  editor_update_gutter(e, gc);
  editor_mark_dirty(e);
}

static void ed_move_line(Editor *e, GlyphCache *gc, int dir) {
  int n = editor_line_count(e);
  int r = e->cursor.row + dir;
  if (r < 0 || r >= n)
    return;
  editor_push_snapshot(e);
  Line *a = lb_get(&e->lb, e->cursor.row);
  Line *b = lb_get(&e->lb, r);
  int pa = e->cursor.row < e->lb.gs ? e->cursor.row
                                    : e->cursor.row + (e->lb.ge - e->lb.gs);
  int pb = r < e->lb.gs ? r : r + (e->lb.ge - e->lb.gs);
  e->lb.lines[pa] = b;
  e->lb.lines[pb] = a;
  e->cursor.row = r;
  editor_mark_dirty(e);
  (void)gc;
}

/* =====================================================================
 * Clipboard
 * ===================================================================== */
static void ed_copy(Editor *e) {
  if (!e->sel_active)
    return;
  char *buf;
  int n = sel_to_buf(e, &buf);
  if (n > 0) {
    SDL_SetClipboardText(buf);
    free(buf);
  }
}

static void ed_cut(Editor *e, GlyphCache *gc) {
  if (!e->sel_active)
    return;
  ed_copy(e);
  sel_delete(e, gc);
}

static void ed_paste(Editor *e, GlyphCache *gc) {
  char *text = SDL_GetClipboardText();
  if (!text || !*text) {
    SDL_free(text);
    return;
  }
  editor_push_snapshot(e);
  if (e->sel_active)
    sel_delete(e, gc);
  ed_insert_text(e, gc, text, (int)strlen(text), false);
  SDL_free(text);
}

/* =====================================================================
 * Search
 * ===================================================================== */
/* Smart-case: a pattern with no uppercase letters matches
   case-insensitively, mirroring the palette filter's behaviour. */
static bool needle_has_upper(const char *s) {
  for (; *s; s++)
    if (isupper((unsigned char)*s))
      return true;
  return false;
}

static void editor_update_search(Editor *e) {
  e->search_count = 0;
  e->search_active = false;
  if (e->prompt[0] == '\0')
    return;
  int qlen = (int)strlen(e->prompt);
  bool fold = !needle_has_upper(e->prompt);
  int n = editor_line_count(e);
  for (int r = 0; r < n && e->search_count < SEARCH_MAX_RESULTS; r++) {
    Line *l = editor_line(e, r);
    int len = line_len(l);
    /* Build flat string */
    char *flat = malloc((size_t)len + 1);
    if (!flat)
      break; /* OOM: show what we have rather than crash */
    line_flatten(l, flat, len);
    flat[len] = '\0';
    char *p = flat;
    while (e->search_count < SEARCH_MAX_RESULTS) {
      char *hit = (char *)(fold ? stristr(p, e->prompt)
                                : strstr(p, e->prompt));
      if (!hit)
        break;
      int col = (int)(hit - flat);
      e->search_results[e->search_count++] = (SearchResult){{r, col}, qlen};
      /* Advance past this match: results never overlap, which keeps
         replace-all positions consistent for self-overlapping needles. */
      p = hit + qlen;
    }
    free(flat);
  }
  e->search_active = e->search_count > 0;
}

static void editor_find_next(Editor *e, bool forward) {
  if (!e->search_active || e->search_count == 0)
    return;
  int best = -1;
  if (forward) {
    /* First result strictly AFTER the cursor (a match already under the
       cursor is skipped so Enter cycles forward); wrap to the start. */
    for (int i = 0; i < e->search_count; i++) {
      Pos p = e->search_results[i].start;
      if (pos_lt(e->cursor, p)) {
        best = i;
        break;
      }
    }
    if (best < 0)
      best = 0; /* wrap */
  } else {
    /* Last result before cursor */
    for (int i = e->search_count - 1; i >= 0; i--) {
      Pos p = e->search_results[i].start;
      if (pos_lt(p, e->cursor)) {
        best = i;
        break;
      }
    }
    if (best < 0)
      best = e->search_count - 1;
  }
  e->search_current = best;
  e->cursor = e->search_results[best].start;
}

static void editor_replace_current(Editor *e, GlyphCache *gc) {
  if (!e->search_active || e->search_count == 0)
    return;
  editor_find_next(e, true);
  Pos at = e->cursor;
  int qlen = (int)strlen(e->prompt);
  int rlen = (int)strlen(e->replace);
  editor_push_snapshot(e);
  raw_delete(e, at, qlen);
  raw_insert(e, at, e->replace, rlen);
  e->cursor = (Pos){at.row, at.col + rlen};
  editor_update_search(e);
  editor_mark_dirty(e);
  (void)gc;
}

static void editor_replace_all(Editor *e, GlyphCache *gc) {
  if (!e->search_active || e->search_count == 0)
    return;
  editor_push_snapshot(e);
  int qlen = (int)strlen(e->prompt);
  int rlen = (int)strlen(e->replace);
  /* Walk backward to keep positions stable */
  for (int i = e->search_count - 1; i >= 0; i--) {
    Pos at = e->search_results[i].start;
    raw_delete(e, at, qlen);
    raw_insert(e, at, e->replace, rlen);
  }
  char msg[128];
  snprintf(msg, sizeof(msg), "Replaced %d occurrences", e->search_count);
  editor_set_status(e, msg);
  editor_update_search(e);
  editor_update_gutter(e, gc);
  editor_mark_dirty(e);
}

/* =====================================================================
 * Bracket matching
 * ===================================================================== */
static void editor_bracket_match(Editor *e) {
  e->bmatch_valid = false;
  Line *l = editor_line(e, e->cursor.row);
  if (!l)
    return;
  int col = e->cursor.col;
  int len = line_len(l);
  char c = (col < len) ? line_at(l, col) : '\0';
  char pair = bracket_pair(c);
  if (!pair) {
    /* Try one column before */
    if (col > 0) {
      col--;
      c = line_at(l, col);
      pair = bracket_pair(c);
    }
    if (!pair)
      return;
  }
  bool fwd = bracket_is_open(c);
  int depth = 1;
  int r = e->cursor.row;
  int start_col = col;
  while (r >= 0 && r < editor_line_count(e)) {
    Line *rl = editor_line(e, r);
    int rlen = line_len(rl);
    int ci = (r == e->cursor.row) ? (fwd ? col + 1 : start_col - 1)
                                  : (fwd ? 0 : rlen - 1);
    while (fwd ? ci < rlen : ci >= 0) {
      char rc = line_at(rl, ci);
      if (rc == pair)
        depth--;
      else if (rc == c)
        depth++;
      if (depth == 0) {
        e->bmatch_open = (Pos){e->cursor.row, start_col};
        e->bmatch_close = (Pos){r, ci};
        e->bmatch_valid = true;
        return;
      }
      if (fwd)
        ci++;
      else
        ci--;
    }
    if (fwd)
      r++;
    else
      r--;
  }
}

/* =====================================================================
 * Save / load
 * ===================================================================== */

/* Reload the LineBuf from a flat byte buffer without touching storage.
   Used after formatting changes the document text. */
static bool editor_reload_from_bytes(Editor *e, GlyphCache *gc,
                                     const char *text, size_t len) {
  return editor_deserialize(e, gc, (const uint8_t *)text, len);
}

static bool editor_do_save(Editor *e, GlyphCache *gc) {
  if (!e->storage) {
    StorageSession *ses = NULL;
    ByteBuffer loaded;
    StorageMetadata loaded_meta;
    StorageOpenResult open_res;
    StorageStatus st = storage_session_open(e->filename, &ses, &loaded,
                                            &loaded_meta, &open_res);
    if (st != STORAGE_OK) {
      char msg[512];
      snprintf(msg, sizeof(msg), "Cannot save '%s': %s", e->filename,
               storage_status_string(st));
      editor_set_status(e, msg);
      return false;
    }
    bytebuffer_free(&loaded);
    e->storage = ses;
  }

  /* Flatten current buffer */
  ByteBuffer raw;
  editor_serialize(e, &raw);

  /* Keep metadata title in sync with the current filename (Save-As
     renames must not leave a stale title behind). */
  {
    const char *base = strrchr(e->filename, '/');
    base = base ? base + 1 : e->filename;
    size_t bl = strlen(base);
    size_t cl =
        bl < sizeof(e->doc_meta.title) - 1 ? bl : sizeof(e->doc_meta.title) - 1;
    memmove(e->doc_meta.title, base, cl);
    e->doc_meta.title[cl] = '\0';
  }

  /* Build pipeline context */
  PipelineCtx ctx = {
      .text = (const char *)raw.data,
      .text_len = raw.len,
      .lang = e->lang,
      .filepath = e->filename,
      .storage = e->storage,
      .meta = &e->doc_meta,
      .flags = PIPE_FLAG_NONE,
  };

  char *formatted_text = NULL;
  size_t formatted_len = 0;
  size_t hist_before =
      e->storage ? storage_history_count(e->storage) : 0;
  PipelineResult pr = pipeline_run(&ctx, &formatted_text, &formatted_len);
  bytebuffer_free(&raw);

  /* Stage 6: if formatter changed the document, reload the LineBuf */
  if (pr.formatted && formatted_text) {
    editor_push_snapshot(e);
    Pos saved_cursor = e->cursor;
    editor_reload_from_bytes(e, gc, formatted_text, formatted_len);
    free(formatted_text);
    /* Restore cursor (clamp to new content) */
    e->cursor = saved_cursor;
    editor_clamp_cursor(e);
    editor_update_gutter(e, gc);
    refresh_preferred_x(e, gc);
    /* Dirty all xcaches */
    int n = editor_line_count(e);
    for (int i = 0; i < n; i++) {
      Line *l = editor_line(e, i);
      if (l)
        l->xcache_dirty = true;
    }
  } else {
    free(formatted_text);
  }

  /* Report result */
  /* Version tag: a new snapshot name, or "no changes since <v>" on dedupe. */
  char vtag[96] = "";
  if (e->storage) {
    size_t hist_after = storage_history_count(e->storage);
    const StorageVersion *nv = storage_history_at(e->storage, 0);
    if (nv) {
      if (hist_after != hist_before)
        snprintf(vtag, sizeof(vtag), " — %s", nv->name);
      else
        snprintf(vtag, sizeof(vtag), " — no changes since %s", nv->name);
    }
  }

  switch (pr.status) {
  case PIPE_OK:
  case PIPE_OK_FORMAT_UNCHANGED:
  case PIPE_OK_FORMAT_SKIPPED: {
    e->dirty = false;
    e->journal_pending = false;
    e->journal_error_shown = false;
    /* Show language + formatter info in status */
    const char *fmt_cmd = fmt_command_name(e->lang);
    char msg[512];
    if (pr.formatted) {
      snprintf(msg, sizeof(msg), "Saved + formatted with %s%s",
               fmt_cmd ? fmt_cmd : "formatter", vtag);
    } else if (pr.status == PIPE_OK_FORMAT_SKIPPED ||
               pr.status == PIPE_OK_FORMAT_UNCHANGED) {
      snprintf(msg, sizeof(msg), "Saved [%s]%s  %s", lang_name(e->lang), vtag,
               pr.status == PIPE_OK_FORMAT_SKIPPED ? ""
                                                   : "(no format changes)");
    } else {
      snprintf(msg, sizeof(msg), "Saved [%s]%s", lang_name(e->lang), vtag);
    }
    editor_set_status(e, msg);
    return true;
  }
  case PIPE_OK_FORMAT_FAILED: {
    e->dirty = false;
    e->journal_pending = false;
    e->journal_error_shown = false;
    char msg[768];
    snprintf(msg, sizeof(msg), "Saved (formatter error: %.240s)%s", pr.diag,
             vtag);
    editor_set_status(e, msg);
    return true;
  }
  case PIPE_ERR_VALIDATE:
  case PIPE_ERR_STORAGE: {
    char msg[768];
    snprintf(msg, sizeof(msg), "Save failed: %.240s", pr.diag);
    editor_set_status(e, msg);
    return false;
  }
  }
  return false;
}

static bool editor_do_load(Editor *e, GlyphCache *gc, const char *path) {
  StorageSession *ses = NULL;
  ByteBuffer doc;
  StorageMetadata meta;
  StorageOpenResult res;
  StorageStatus st = storage_session_open(path, &ses, &doc, &meta, &res);
  if (st != STORAGE_OK) {
    char msg[512];
    snprintf(msg, sizeof(msg), "Cannot open '%s': %s", path,
             storage_status_string(st));
    editor_set_status(e, msg);
    return false;
  }
  if (!editor_deserialize(e, gc, doc.data, doc.len)) {
    bytebuffer_free(&doc);
    storage_session_close(ses);
    editor_set_status(e, "Failed to load document");
    return false;
  }
  bytebuffer_free(&doc);
  if (e->storage)
    storage_session_close(e->storage);
  e->storage = ses;
  e->doc_meta = meta;
  snprintf(e->filename, sizeof(e->filename), "%s", path);
  e->dirty = false;
  e->journal_pending = false;
  e->journal_error_shown = false;
  e->last_journal_ms = SDL_GetTicks();
  uring_destroy(&e->uring);
  uring_init(&e->uring);
  bytebuffer_free(&e->rec_doc);
  e->rec_available = false;

  /* Language detection: extension + content heuristic */
  {
    ByteBuffer content_buf;
    editor_serialize(e, &content_buf);
    e->lang =
        lang_detect(path, (const char *)content_buf.data, content_buf.len);
    bytebuffer_free(&content_buf);
  }
  if (res == STORAGE_OPEN_RECOVERED) {
    ByteBuffer rd;
    StorageMetadata rm;
    if (storage_recovery_get(e->storage, &rd, &rm) == STORAGE_OK) {
      e->rec_doc = rd;
      e->rec_meta = rm;
      e->rec_available = true;
      e->mode_pre_overlay = e->mode;
      e->mode = MODE_RECOVERY_PROMPT;
      editor_set_status(
          e, "Unsaved changes found — press R to restore, D to discard");
    }
  } else if (res == STORAGE_OPEN_NEW) {
    editor_set_status(e, "New file");
  } else {
    editor_set_status(e, "Opened");
  }
  return true;
}

/* =====================================================================
 * Journal
 * ===================================================================== */
static void editor_journal_tick(Editor *e, Uint32 now) {
  if (!e->storage || !e->journal_pending)
    return;
  if (now - e->last_journal_ms < JOURNAL_DEBOUNCE_MS)
    return;
  ByteBuffer doc;
  editor_serialize(e, &doc);
  StorageStatus st = storage_journal_append(e->storage, "edit", &doc);
  bytebuffer_free(&doc);
  e->last_journal_ms = now; /* pace retries regardless of outcome */
  if (st != STORAGE_OK) {
    /* Journaling is best-effort, but it must not die silently: warn once
       per failure episode and keep journal_pending set so we retry. */
    if (!e->journal_error_shown) {
      char msg[256];
      snprintf(msg, sizeof(msg), "Warning: crash journal unavailable (%s)",
               storage_status_string(st));
      editor_set_status(e, msg);
      e->journal_error_shown = true;
    }
    return;
  }
  e->journal_error_shown = false;
  e->journal_pending = false;
}

/* =====================================================================
 * Quit flow
 * ===================================================================== */

/* Drop crash-recovery sidecars: truncates the journal, discards any
   staged recovery candidate and removes the autosave. Called when the
   user explicitly quits (saving or discarding), so the next session
   never sees a spurious "unsaved changes" prompt for state the user
   deliberately abandoned. */
static void editor_discard_recovery_state(Editor *e) {
  if (!e->storage)
    return;
  storage_journal_clear(e->storage);
  storage_recovery_discard(e->storage);
  char autosave[STORAGE_PATH_MAX];
  if (storage_autosave_path(e->storage, autosave, sizeof(autosave)))
    unlink(autosave);
}

static void editor_request_quit(Editor *e) { e->quit_requested = true; }

/* =====================================================================
 * Version browser (Ctrl+Alt+V)
 * ===================================================================== */

#define VER_BROWSER_VISIBLE 12

static void open_version_browser(Editor *e) {
  if (!e->storage) {
    editor_set_status(e, "Save the file first to create versions");
    return;
  }
  e->mode_pre_overlay = e->mode;
  e->mode = MODE_VERSION_BROWSER;
  e->ver_sel = 0;
  e->ver_scroll = 0;
  e->ver_delete_armed = false;
}

/* Load the selected version into the buffer as an unsaved edit; the
   pre-restore document is snapshot-pushable via Ctrl+Z. */
static void restore_selected_version(Editor *e, GlyphCache *gc) {
  const StorageVersion *v =
      storage_history_at(e->storage, (size_t)e->ver_sel);
  if (!v)
    return;
  editor_push_snapshot(e);
  editor_deserialize(e, gc, v->doc.data, v->doc.len);
  editor_mark_dirty(e);
  e->mode = e->mode_pre_overlay;
  char msg[256];
  snprintf(msg, sizeof(msg), "Restored %s — review, then Ctrl+S", v->name);
  editor_set_status(e, msg);
}

/* Keep the selection inside the visible window. */
static void ver_sync_scroll(Editor *e) {
  if (e->ver_sel < e->ver_scroll)
    e->ver_scroll = e->ver_sel;
  if (e->ver_sel >= e->ver_scroll + VER_BROWSER_VISIBLE)
    e->ver_scroll = e->ver_sel - VER_BROWSER_VISIBLE + 1;
  if (e->ver_scroll < 0)
    e->ver_scroll = 0;
}

/* Trim to STORAGE_VERSION_NAME_MAX-1 bytes without splitting a codepoint. */
static void ver_name_sanitize(char *s) {
  while (strlen(s) >= STORAGE_VERSION_NAME_MAX)
    prompt_backspace(s);
}

/* =====================================================================
 * Command palette registration + built-in commands
 * ===================================================================== */
static void palette_register(Editor *e, const char *label, CmdFn fn) {
  if (e->palette_count >= PALETTE_MAX)
    return;
  e->palette[e->palette_count++] = (PaletteEntry){label, fn};
}

static void cmd_save(void *ev) {
  Editor *e = ev;
  e->mode = MODE_SAVE_PROMPT;
  snprintf(e->prompt, sizeof(e->prompt), "%s", e->filename);
}
static void cmd_open(void *ev) {
  Editor *e = ev;
  e->mode = MODE_OPEN_PROMPT;
  e->prompt[0] = '\0';
}
static void cmd_find(void *ev) {
  Editor *e = ev;
  e->mode = MODE_FIND;
  e->prompt[0] = '\0';
  editor_update_search(e);
}
static void cmd_replace(void *ev) {
  Editor *e = ev;
  e->mode = MODE_REPLACE;
  e->prompt[0] = '\0';
  e->replace[0] = '\0';
  e->prompt_field = 0;
}
static void cmd_goto_line(void *ev) {
  Editor *e = ev;
  e->mode = MODE_GOTO_LINE;
  e->goto_buf[0] = '\0';
}
static void cmd_toggle_comment(void *ev) {
  Editor *e = ev;
  ed_toggle_comment(e, e->gc);
}
static void cmd_duplicate_line(void *ev) {
  Editor *e = ev;
  ed_duplicate_line(e, e->gc);
}

static void editor_register_commands(Editor *e) {
  palette_register(e, "File: Save", cmd_save);
  palette_register(e, "File: Open", cmd_open);
  palette_register(e, "Find in File", cmd_find);
  palette_register(e, "Find and Replace", cmd_replace);
  palette_register(e, "Go to Line", cmd_goto_line);
  palette_register(e, "Toggle Line Comment", cmd_toggle_comment);
  palette_register(e, "Duplicate Line", cmd_duplicate_line);
}

/* =====================================================================
 * Rendering primitives
 * ===================================================================== */
static void render_str(GlyphCache *gc, const char *s, int *x, int y, Uint8 r,
                       Uint8 g, Uint8 b) {
  for (int i = 0; s[i];) {
    unsigned char lead = (unsigned char)s[i];
    int seq = utf8_seq_len(lead);
    uint32_t cp = 0;
    if (seq == 1)
      cp = lead & 0x7F;
    else if (seq == 2)
      cp = lead & 0x1F;
    else if (seq == 3)
      cp = lead & 0x0F;
    else
      cp = lead & 0x07;
    for (int k = 1; k < seq; k++) {
      unsigned char c = (unsigned char)s[i + k];
      if ((c & 0xC0) != 0x80) {
        seq = k;
        break;
      }
      cp = (cp << 6) | (c & 0x3F);
    }
    render_glyph_cached(gc, cp, x, y, r, g, b);
    i += seq;
  }
}

static void render_str_c(GlyphCache *gc, const char *s, int *x, int y,
                         Color c) {
  render_str(gc, s, x, y, c.r, c.g, c.b);
}

static void fill(SDL_Renderer *ren, int x, int y, int w, int h, Color c,
                 Uint8 alpha) {
  SDL_SetRenderDrawBlendMode(ren, alpha < 255 ? SDL_BLENDMODE_BLEND
                                              : SDL_BLENDMODE_NONE);
  SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, alpha);
  SDL_Rect rc = {x, y, w, h};
  SDL_RenderFillRect(ren, &rc);
  SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
}

static void stroke(SDL_Renderer *ren, int x, int y, int w, int h, Color c) {
  SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
  SDL_Rect rc = {x, y, w, h};
  SDL_RenderDrawRect(ren, &rc);
}

/* Backwards-compatible wrappers for existing call sites. */

/* Lerp a color toward another by t in [0,1] — used for message fades. */
static Color color_lerp(Color from, Color to, float t) {
  if (t < 0)
    t = 0;
  if (t > 1)
    t = 1;
  Color o;
  o.r = (Uint8)(from.r + (to.r - from.r) * t);
  o.g = (Uint8)(from.g + (to.g - from.g) * t);
  o.b = (Uint8)(from.b + (to.b - from.b) * t);
  return o;
}

static int text_width(GlyphCache *gc, const char *s) {
  return measure_string(gc, s, (int)strlen(s));
}

/* =====================================================================
 * Dialog / overlay components
 * ===================================================================== */
static void render_overlay_bg(SDL_Renderer *ren, int ww, int wh) {
  fill(ren, 0, 0, ww, wh, THEMET.scrim, 216);
}

/* Professional dialog chrome: soft layered shadow, elevated panel,
   hairline border, accent spine on the left edge, title row with a
   hairline separator beneath. */
static void render_dialog_box(SDL_Renderer *ren, GlyphCache *gc, int dx, int dy,
                              int dw, int dh, const char *title, Color accent) {
  /* Soft shadow: expanding translucent layers */
  fill(ren, dx + 3, dy + 4, dw, dh, THEMET.bg, 90);
  fill(ren, dx + 6, dy + 8, dw, dh, THEMET.bg, 40);
  /* Panel */
  fill(ren, dx, dy, dw, dh, THEMET.bg_elevated, 255);
  stroke(ren, dx, dy, dw, dh, THEMET.border_subtle);
  /* Accent spine + top highlight */
  fill(ren, dx, dy, 3, dh, accent, 255);
  fill(ren, dx + 3, dy, dw - 3, 1, THEMET.border_strong, 255);
  /* Title */
  int tx = dx + 24, ty = dy + 34;
  render_str_c(gc, title, &tx, ty, THEMET.text_secondary);
  fill(ren, dx + 1, dy + 48, dw - 2, 1, THEMET.border_subtle, 255);
}

/* Text input with an optional small caps label ABOVE the field,
   accent focus ring and blinking caret. */
static void render_input_field(SDL_Renderer *ren, GlyphCache *gc, int x, int y,
                               int w, int h, const char *label,
                               const char *text, bool active, Uint32 ticks) {
  if (label && label[0]) {
    int lx = x, ly = y - 8;
    render_str_c(gc, label, &lx, ly, THEMET.text_muted);
  }
  fill(ren, x, y, w, h, THEMET.bg_input, 255);
  stroke(ren, x, y, w, h, active ? THEMET.accent : THEMET.border_subtle);
  if (active) {
    /* inner focus glow line under top edge */
    fill(ren, x + 1, y + h - 2, w - 2, 1, THEMET.accent, 120);
  }
  int tx = x + 12, ty = y + h - 9;
  render_str_c(gc, text, &tx, ty,
               active ? THEMET.text_primary : THEMET.text_secondary);
  if (active && (ticks / CURSOR_BLINK_MS) % 2 == 0)
    fill(ren, tx + 1, y + 7, 2, h - 14, THEMET.accent, 255);
}

/* Footer hint as a key chip + description. Returns x advanced past the
   pair so callers can chain hints horizontally. */
static int render_hint_chip(GlyphCache *gc, SDL_Renderer *ren, int x, int y,
                            const char *key, const char *desc, Color key_col) {
  int kw = text_width(gc, key);
  /* chip */
  fill(ren, x, y - 13, kw + 12, 20, THEMET.bg_input, 255);
  stroke(ren, x, y - 13, kw + 12, 20, key_col);
  int tx = x + 6;
  render_str_c(gc, key, &tx, y, key_col);
  tx += 6;
  render_str_c(gc, desc, &tx, y, THEMET.text_muted);
  return tx + 18;
}

static void render_prompt_dialog(SDL_Renderer *ren, GlyphCache *gc, Editor *e,
                                 SDL_Window *win) {
  int ww, wh;
  SDL_GetWindowSize(win, &ww, &wh);
  render_overlay_bg(ren, ww, wh);

  bool is_replace = (e->mode == MODE_REPLACE);
  int dw = 660, dh = is_replace ? 268 : 176;
  int dx = (ww - dw) / 2, dy = (wh - dh) / 2;
  Uint32 ticks = SDL_GetTicks();

  const char *title = "DIALOG";
  const char *f_label = "PATH";
  if (e->mode == MODE_SAVE_PROMPT)
    title = "SAVE FILE AS";
  else if (e->mode == MODE_OPEN_PROMPT)
    title = "OPEN FILE";
  else if (e->mode == MODE_FIND) {
    title = "FIND";
    f_label = "SEARCH";
  } else if (e->mode == MODE_REPLACE) {
    title = "FIND & REPLACE";
    f_label = "SEARCH";
  }

  render_dialog_box(ren, gc, dx, dy, dw, dh, title, THEMET.accent);

  int fx0 = dx + 24;
  int fy0 = dy + 84; /* leaves room for the label above the field */
  render_input_field(ren, gc, fx0, fy0, dw - 48, 40,
                     is_replace ? f_label : NULL, e->prompt,
                     !is_replace || e->prompt_field == 0, ticks);

  if (is_replace) {
    int ry = fy0 + 40 + 30;
    render_input_field(ren, gc, fx0, ry, dw - 48, 40, "REPLACE WITH",
                       e->replace, e->prompt_field == 1, ticks);
  }

  int fy = dy + dh - 22;
  int fx = dx + 24;
  if (e->mode == MODE_FIND) {
    fx = render_hint_chip(gc, ren, fx, fy, "Enter", "find next",
                          THEMET.success);
    fx = render_hint_chip(gc, ren, fx, fy, "Shift+Enter", "find previous",
                          THEMET.accent);
  } else if (e->mode == MODE_REPLACE) {
    fx = render_hint_chip(gc, ren, fx, fy, "Enter", "replace next",
                          THEMET.success);
    fx = render_hint_chip(gc, ren, fx, fy, "Ctrl+A", "replace all",
                          THEMET.warning);
    fx = render_hint_chip(gc, ren, fx, fy, "Tab", "switch field",
                          THEMET.text_secondary);
  } else {
    fx = render_hint_chip(gc, ren, fx, fy, "Enter", "confirm",
                          THEMET.success);
  }
  render_hint_chip(gc, ren, fx, fy, "Esc", "cancel", THEMET.danger);
}

static void render_recovery_dialog(SDL_Renderer *ren, GlyphCache *gc,
                                   Editor *e, SDL_Window *win) {
  (void)e;
  int ww, wh;
  SDL_GetWindowSize(win, &ww, &wh);
  render_overlay_bg(ren, ww, wh);
  int dw = 720, dh = 210, dx = (ww - dw) / 2, dy = (wh - dh) / 2;
  render_dialog_box(ren, gc, dx, dy, dw, dh, "UNSAVED CHANGES FOUND",
                    THEMET.warning);

  /* Warning glyph block */
  fill(ren, dx + 24, dy + 72, 34, 34, THEMET.warning, 36);
  stroke(ren, dx + 24, dy + 72, 34, 34, THEMET.warning);
  int ix = dx + 37, iy = dy + 97;
  render_str_c(gc, "!", &ix, iy, THEMET.warning);

  int lx = dx + 76, ly = dy + 88;
  render_str_c(gc, "A previous session ended without saving.", &lx, ly,
               THEMET.text_primary);
  lx = dx + 76;
  ly += 26;
  render_str_c(gc, "Restore the recovered version, or discard it.", &lx, ly,
               THEMET.text_secondary);

  int fy = dy + dh - 22, fx = dx + 24;
  fx = render_hint_chip(gc, ren, fx, fy, "R", "Restore recovered text",
                        THEMET.success);
  render_hint_chip(gc, ren, fx, fy, "D / Esc", "Discard it", THEMET.danger);
}

static void render_quit_dialog(SDL_Renderer *ren, GlyphCache *gc, Editor *e,
                               SDL_Window *win) {
  (void)e;
  int ww, wh;
  SDL_GetWindowSize(win, &ww, &wh);
  render_overlay_bg(ren, ww, wh);
  int dw = 640, dh = 190, dx = (ww - dw) / 2, dy = (wh - dh) / 2;
  render_dialog_box(ren, gc, dx, dy, dw, dh, "QUIT WITHOUT SAVING?",
                    THEMET.danger);

  /* Danger glyph */
  fill(ren, dx + 24, dy + 74, 34, 34, THEMET.danger, 36);
  stroke(ren, dx + 24, dy + 74, 34, 34, THEMET.danger);
  int ix = dx + 35, iy = dy + 99;
  render_str_c(gc, "?", &ix, iy, THEMET.danger);

  int lx = dx + 76, ly = dy + 90;
  render_str_c(gc, "The document has unsaved changes.", &lx, ly,
               THEMET.text_primary);

  int fy = dy + dh - 22, fx = dx + 24;
  fx = render_hint_chip(gc, ren, fx, fy, "S", "Save & quit", THEMET.success);
  fx = render_hint_chip(gc, ren, fx, fy, "D", "Discard & quit",
                        THEMET.danger);
  render_hint_chip(gc, ren, fx, fy, "Esc", "Stay", THEMET.accent);
}

/* Column layout for the version browser (relative to dialog left edge) */
#define VER_COL_NAME 40
#define VER_COL_DATE 260
#define VER_COL_SIZE 520
#define VER_ROW_H 30

static void render_version_browser(SDL_Renderer *ren, GlyphCache *gc,
                                   Editor *e, SDL_Window *win) {
  int ww, wh;
  SDL_GetWindowSize(win, &ww, &wh);
  render_overlay_bg(ren, ww, wh);

  size_t n = e->storage ? storage_history_count(e->storage) : 0;
  int vis = VER_BROWSER_VISIBLE;
  int shown_rows = (int)(n < (size_t)vis ? n : (size_t)vis);
  int dw = 780;
  int list_h = (shown_rows > 0 ? shown_rows : 3) * VER_ROW_H;
  int dh = 48 + 26 + list_h + 56; /* title | header | rows | footer */
  if (dh > wh - 40) {
    dh = wh - 40;
    vis = (dh - 48 - 26 - 56) / VER_ROW_H;
    if (vis < 1)
      vis = 1;
  }
  int dx = (ww - dw) / 2, dy = (wh - dh) / 2;

  Color accent = e->ver_delete_armed ? THEMET.danger : THEMET.accent;
  render_dialog_box(ren, gc, dx, dy, dw, dh, "VERSION HISTORY", accent);

  int list_top = dy + 74;
  int list_w = dw - 24;

  /* Column headers */
  int hx = dx + VER_COL_NAME, hy = list_top - 6;
  render_str_c(gc, "VERSION", &hx, hy, THEMET.text_muted);
  hx = dx + VER_COL_DATE;
  render_str_c(gc, "SAVED", &hx, hy, THEMET.text_muted);
  hx = dx + VER_COL_SIZE;
  render_str_c(gc, "SIZE", &hx, hy, THEMET.text_muted);
  fill(ren, dx + 1, list_top + 4, dw - 2, 1, THEMET.border_subtle, 255);

  if (n == 0) {
    int mx = dx + 40, my = list_top + 60;
    render_str_c(gc, "No versions yet — save the file to create one.", &mx,
                 my, THEMET.text_secondary);
    mx = dx + 40;
    my += 26;
    render_str_c(gc, "Every Ctrl+S records an immutable snapshot in this file.",
                 &mx, my, THEMET.text_muted);
    int fy = dy + dh - 22;
    render_hint_chip(gc, ren, dx + 24, fy, "Esc", "close", THEMET.danger);
    return;
  }

  /* Rows: zebra + selection bar + columns */
  int end = e->ver_scroll + vis;
  if (end > (int)n)
    end = (int)n;
  int y = list_top + 10;
  for (int i = e->ver_scroll; i < end; i++) {
    const StorageVersion *v = storage_history_at(e->storage, (size_t)i);
    bool selected = (i == e->ver_sel);
    bool deleting = selected && e->ver_delete_armed;

    if ((i % 2) == 1 && !selected)
      fill(ren, dx + 12, y - 5, list_w - 8, VER_ROW_H - 2, THEMET.bg_input,
           120);
    if (selected) {
      fill(ren, dx + 12, y - 5, list_w - 8, VER_ROW_H - 2,
           deleting ? THEMET.danger : THEMET.accent, deleting ? 46 : 34);
      fill(ren, dx + 12, y - 5, 3, VER_ROW_H - 2,
           deleting ? THEMET.danger : THEMET.accent, 255);
    }

    time_t tt = (time_t)v->created_at;
    struct tm tmv;
    localtime_r(&tt, &tmv);
    char name_col[96], date_col[40], size_col[32];
    snprintf(name_col, sizeof(name_col), "%s", v->name);
    snprintf(date_col, sizeof(date_col), "%04d-%02d-%02d %02d:%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
             tmv.tm_min);
    snprintf(size_col, sizeof(size_col), "%6.1f KB",
             (double)v->doc.len / 1024.0);

    Color tcol = selected ? THEMET.text_primary : THEMET.text_secondary;
    int tx = dx + VER_COL_NAME;
    render_str_c(gc, name_col, &tx, y + 14, tcol);
    tx = dx + VER_COL_DATE;
    render_str_c(gc, date_col, &tx, y + 14,
                 selected ? THEMET.text_secondary : THEMET.text_muted);
    tx = dx + VER_COL_SIZE;
    render_str_c(gc, size_col, &tx, y + 14,
                 selected ? THEMET.text_secondary : THEMET.text_muted);

    if (i == 0) {
      const char *badge = "LATEST";
      int bw = text_width(gc, badge) + 14;
      int bx = dx + dw - bw - 28;
      stroke(ren, bx, y - 1, bw, 20, THEMET.success);
      fill(ren, bx + 1, y, bw - 2, 18, THEMET.success, 30);
      int btx = bx + 7;
      render_str_c(gc, badge, &btx, y + 13, THEMET.success);
    } else if (deleting) {
      const char *badge = "DELETE?";
      int bw = text_width(gc, badge) + 14;
      int bx = dx + dw - bw - 28;
      stroke(ren, bx, y - 1, bw, 20, THEMET.danger);
      int btx = bx + 7;
      render_str_c(gc, badge, &btx, y + 13, THEMET.danger);
    } else if (selected) {
      const char *badge = "RESTORE?";
      int bw = text_width(gc, badge) + 14;
      int bx = dx + dw - bw - 28;
      stroke(ren, bx, y - 1, bw, 20, THEMET.accent);
      int btx = bx + 7;
      render_str_c(gc, badge, &btx, y + 13, THEMET.accent);
    }

    y += VER_ROW_H;
  }

  /* Scrollbar */
  if ((int)n > vis) {
    int track_x = dx + dw - 16;
    int track_y = list_top + 8;
    int track_h = vis * VER_ROW_H - 6;
    fill(ren, track_x, track_y, 4, track_h, THEMET.border_subtle, 160);
    float frac = (float)vis / (float)n;
    int thumb_h = (int)(track_h * frac);
    if (thumb_h < 12)
      thumb_h = 12;
    int max_off = track_h - thumb_h;
    int off = (int)(((float)e->ver_scroll / (float)(n - vis)) * max_off);
    if (off < 0)
      off = 0;
    if (off > max_off)
      off = max_off;
    fill(ren, track_x, track_y + off, 4, thumb_h, THEMET.text_muted, 220);
  }

  int fy = dy + dh - 22, fx = dx + 24;
  if (e->ver_delete_armed) {
    fx = render_hint_chip(gc, ren, fx, fy, "Enter", "confirm delete",
                          THEMET.danger);
    render_hint_chip(gc, ren, fx, fy, "Esc", "cancel", THEMET.text_secondary);
  } else {
    fx = render_hint_chip(gc, ren, fx, fy, "Enter", "restore", THEMET.success);
    fx = render_hint_chip(gc, ren, fx, fy, "R", "rename", THEMET.accent);
    fx = render_hint_chip(gc, ren, fx, fy, "D", "delete", THEMET.danger);
    render_hint_chip(gc, ren, fx, fy, "Esc", "close", THEMET.text_muted);
  }
}

static void render_version_rename(SDL_Renderer *ren, GlyphCache *gc, Editor *e,
                                  SDL_Window *win) {
  int ww, wh;
  SDL_GetWindowSize(win, &ww, &wh);
  render_overlay_bg(ren, ww, wh);
  int dw = 520, dh = 168, dx = (ww - dw) / 2, dy = (wh - dh) / 2;
  render_dialog_box(ren, gc, dx, dy, dw, dh, "RENAME VERSION", THEMET.accent);
  render_input_field(ren, gc, dx + 24, dy + 92, dw - 48, 40, "VERSION NAME",
                     e->ver_rename_buf, true, SDL_GetTicks());
  int fy = dy + dh - 22, fx = dx + 24;
  fx = render_hint_chip(gc, ren, fx, fy, "Enter", "save name",
                        THEMET.success);
  render_hint_chip(gc, ren, fx, fy, "Esc", "cancel", THEMET.danger);
}

static void render_palette(SDL_Renderer *ren, GlyphCache *gc, Editor *e,
                           SDL_Window *win) {
  int ww, wh;
  SDL_GetWindowSize(win, &ww, &wh);
  render_overlay_bg(ren, ww, wh);

  /* Count visible entries first so the dialog hugs its content. */
  int visible_count = 0;
  for (int i = 0; i < e->palette_count; i++) {
    if (e->palette_filter[0] &&
        stristr(e->palette[i].label, e->palette_filter) == NULL)
      continue;
    visible_count++;
  }
  int max_show = 10;
  if (visible_count > max_show)
    visible_count = max_show;

  int dw = 600;
  int dh = 48 + 52 + (visible_count > 0 ? visible_count : 1) * 30 + 18;
  int dx = (ww - dw) / 2, dy = wh / 6;
  render_dialog_box(ren, gc, dx, dy, dw, dh, "COMMAND PALETTE",
                    THEMET.accent);
  render_input_field(ren, gc, dx + 16, dy + 84, dw - 32, 38, NULL,
                     e->palette_filter, true, SDL_GetTicks());
  { /* prompt glyph inside the field */
    int px = dx + 28, py = dy + 84 + 27 - 14;
    render_str_c(gc, ">", &px, py, THEMET.text_muted);
  }

  int shown = 0;
  int y = dy + 84 + 38 + 10;
  for (int i = 0; i < e->palette_count && shown < max_show; i++) {
    if (e->palette_filter[0] &&
        stristr(e->palette[i].label, e->palette_filter) == NULL)
      continue;
    bool selected = (shown == e->palette_sel);
    if (selected) {
      fill(ren, dx + 12, y, dw - 24, 28, THEMET.accent, 34);
      fill(ren, dx + 12, y, 3, 28, THEMET.accent, 255);
    }
    int tx = dx + 28;
    render_str_c(gc, e->palette[i].label, &tx, y + 18,
                 selected ? THEMET.text_primary : THEMET.text_secondary);
    y += 30;
    shown++;
  }
}

/* =====================================================================
 * Status bar
 * Mode pill | filename ● dirty | fading message | language · position
 * ===================================================================== */
static void render_status_bar(SDL_Renderer *ren, GlyphCache *gc, Editor *e,
                              SDL_Window *win) {
  int ww, wh;
  SDL_GetWindowSize(win, &ww, &wh);
  int by = wh - STATUS_BAR_HEIGHT;
  fill(ren, 0, by, ww, STATUS_BAR_HEIGHT, THEMET.bg_elevated, 255);
  fill(ren, 0, by, ww, 1, THEMET.border_strong, 255);

  int ty = by + STATUS_BAR_HEIGHT - 7;

  /* Left: mode pill */
  const char *mode_str = (e->mode == MODE_INSERT)   ? "INSERT"
                         : (e->mode == MODE_NORMAL) ? "NORMAL"
                                                    : "PROMPT";
  Color mode_col = (e->mode == MODE_INSERT) ? THEMET.success
                   : (e->mode == MODE_NORMAL)
                       ? THEMET.accent
                       : (Color){169, 177, 194};
  int lx = 8;
  int mw = text_width(gc, mode_str);
  fill(ren, lx - 6, by + 3, mw + 18, STATUS_BAR_HEIGHT - 6, mode_col, 36);
  fill(ren, lx - 6, by + 3, 2, STATUS_BAR_HEIGHT - 6, mode_col, 255);
  int mx = lx - 2;
  render_str_c(gc, mode_str, &mx, ty, mode_col);
  lx += mw + 22;

  /* Separator + filename (+ dirty dot) */
  render_str_c(gc, "│", &lx, ty, THEMET.border_strong);
  lx += 12;
  {
    char fname[1100];
    snprintf(fname, sizeof(fname), "%s", e->filename);
    render_str_c(gc, fname, &lx, ty,
                 e->dirty ? THEMET.text_primary : THEMET.text_secondary);
    if (e->dirty || storage_history_dirty(e->storage)) {
      lx += 8;
      render_str_c(gc, "●", &lx, ty, THEMET.warning);
      lx += 4;
    }
    lx += 14;
  }

  /* Center: status message with fade-out */
  Uint32 elapsed = SDL_GetTicks() - e->status_msg_time;
  if (elapsed < 3000 && e->status_msg[0]) {
    float t = elapsed <= 2000.0f ? 0.0f : (elapsed - 2000.0f) / 1000.0f;
    Color msg_col =
        color_lerp(THEMET.success, THEMET.bg_elevated, t);
    int msgw = text_width(gc, e->status_msg);
    int cx = (ww - msgw) / 2;
    render_str_c(gc, e->status_msg, &cx, ty, msg_col);
  }

  /* Right: search position / language / line:column */
  char right[192];
  char pos[64];
  snprintf(pos, sizeof(pos), "Ln %d, Col %d", e->cursor.row + 1,
           e->cursor.col + 1);
  if (e->search_active && e->mode == MODE_FIND)
    snprintf(right, sizeof(right), "%d/%d   %s   %s", e->search_current + 1,
             e->search_count, lang_name(e->lang), pos);
  else
    snprintf(right, sizeof(right), "%s   %s", lang_name(e->lang), pos);
  int rw = text_width(gc, right);
  int rx = ww - rw - 10;
  render_str_c(gc, right, &rx, ty, THEMET.text_muted);
}

/* =====================================================================
 * Main document rendering
 * ===================================================================== */
static void render_document(SDL_Renderer *ren, GlyphCache *gc, Editor *e,
                            SDL_Window *win, bool show_cursor) {
  int ww, wh;
  SDL_GetWindowSize(win, &ww, &wh);
  int vis = visible_rows(win);
  Pos sel_lo_p = sel_lo(e), sel_hi_p = sel_hi(e);

  /* Gutter / text separator hairline */
  {
    int rule_x = e->text_x - gc->space_advance * 2 - GUTTER_PAD;
    if (rule_x > 4)
      fill(ren, rule_x, FIRST_LINE_Y - 4, 1,
           wh - STATUS_BAR_HEIGHT - FIRST_LINE_Y + 2, THEMET.gutter_rule, 255);
  }

  for (int r = e->scroll_row;
       r < editor_line_count(e) && r < e->scroll_row + vis; r++) {
    Line *l = editor_line(e, r);
    int sy = FIRST_LINE_Y + (r - e->scroll_row) * LINE_HEIGHT_PX +
             LINE_HEIGHT_PX - 4;

    /* Current line highlight */
    if (r == e->cursor.row) {
      fill(ren, 0, sy - LINE_HEIGHT_PX + 4, ww, LINE_HEIGHT_PX,
           THEMET.bg_band, 255);
    }

    /* Selection highlight */
    if (e->sel_active && r >= sel_lo_p.row && r <= sel_hi_p.row) {
      int c0 = (r == sel_lo_p.row) ? sel_lo_p.col : 0;
      int c1 = (r == sel_hi_p.row) ? sel_hi_p.col : line_len(l);
      int x0 = line_x_at(l, gc, e->text_x, c0);
      int x1 = line_x_at(l, gc, e->text_x, c1);
      if (r < sel_hi_p.row)
        x1 += gc->space_advance; /* show newline */
      fill(ren, x0, sy - LINE_HEIGHT_PX + 4, x1 - x0, LINE_HEIGHT_PX,
           THEMET.selection, 130);
    }

    /* Search result highlights */
    if (e->search_active) {
      for (int si = 0; si < e->search_count; si++) {
        if (e->search_results[si].start.row != r)
          continue;
        int c0 = e->search_results[si].start.col;
        int c1 = c0 + e->search_results[si].len;
        int x0 = line_x_at(l, gc, e->text_x, c0);
        int x1 = line_x_at(l, gc, e->text_x, c1);
        bool current = (si == e->search_current);
        fill(ren, x0, sy - LINE_HEIGHT_PX + 4, x1 - x0, LINE_HEIGHT_PX,
             current ? THEMET.search_current : THEMET.search_hl,
             current ? 215 : 120);
      }
    }

    /* Bracket match highlight */
    if (e->bmatch_valid) {
      Pos bpairs[2] = {e->bmatch_open, e->bmatch_close};
      for (int bi = 0; bi < 2; bi++) {
        if (bpairs[bi].row == r) {
          int bx = line_x_at(l, gc, e->text_x, bpairs[bi].col);
          stroke(ren, bx, sy - LINE_HEIGHT_PX + 5, gc->space_advance,
                 LINE_HEIGHT_PX - 2, THEMET.bracket);
          fill(ren, bx, sy - LINE_HEIGHT_PX + 5, gc->space_advance,
               LINE_HEIGHT_PX - 2, THEMET.bracket, 70);
        }
      }
    }

    /* Gutter */
    char num_buf[16];
    if (r == e->cursor.row)
      snprintf(num_buf, sizeof(num_buf), "%d", r + 1);
    else
      snprintf(num_buf, sizeof(num_buf), "%d", abs(r - e->cursor.row));
    int gx = GUTTER_PAD;
    int gw = measure_string(gc, num_buf, (int)strlen(num_buf));
    gx = e->text_x - gc->space_advance * 2 - gw;
    Color gcol =
        (r == e->cursor.row) ? THEMET.text_primary : THEMET.text_muted;
    render_str_c(gc, num_buf, &gx, sy, gcol);

    /* Text with syntax highlighting */
    int tx = e->text_x;
    int ll = line_len(l);
    int col = 0;
    while (col < ll) {
      int tlen;
      TokKind tk = tok_at(l, col, &tlen);
      Col3 color = tok_color(tk);
      for (int bi = 0; bi < tlen && col < ll;) {
        unsigned char lead = (unsigned char)line_at(l, col);
        int seq = utf8_seq_len(lead);
        uint32_t cp = 0;
        if (seq == 1)
          cp = lead & 0x7F;
        else if (seq == 2)
          cp = lead & 0x1F;
        else if (seq == 3)
          cp = lead & 0x0F;
        else
          cp = lead & 0x07;
        for (int ki = 1; ki < seq; ki++) {
          unsigned char c = (unsigned char)line_at(l, col + ki);
          if ((c & 0xC0) != 0x80) {
            seq = ki;
            break;
          }
          cp = (cp << 6) | (c & 0x3F);
        }
        render_glyph_cached(gc, cp, &tx, sy, color.r, color.g, color.b);
        col += seq;
        bi += seq;
      }
      if (tlen <= 0)
        col++;
    }
  }

  /* Cursor */
  if (show_cursor && e->cursor.row >= e->scroll_row &&
      e->cursor.row < e->scroll_row + vis) {
    Line *cl = editor_line(e, e->cursor.row);
    int cx = line_x_at(cl, gc, e->text_x, e->cursor.col);
    int cy =
        FIRST_LINE_Y + (e->cursor.row - e->scroll_row) * LINE_HEIGHT_PX + 4;
    bool block = (e->mode == MODE_NORMAL);
    if (block) {
      fill(ren, cx, cy, gc->space_advance, LINE_HEIGHT_PX - 2, THEMET.accent,
           60);
      stroke(ren, cx, cy, gc->space_advance, LINE_HEIGHT_PX - 2,
             THEMET.accent);
    } else {
      fill(ren, cx, cy, 2, LINE_HEIGHT_PX - 2, THEMET.cursor, 255);
    }
  }
}

/* =====================================================================
 * stristr — case-insensitive substring search (for palette filter)
 * ===================================================================== */
static const char *stristr(const char *hay, const char *needle) {
  if (!needle || !*needle)
    return hay;
  for (; *hay; hay++) {
    const char *h = hay, *n = needle;
    while (*h && *n &&
           tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
      h++;
      n++;
    }
    if (!*n)
      return hay;
  }
  return NULL;
}

/* =====================================================================
 * Cursor motion
 * ===================================================================== */
static void move_left(Editor *e, GlyphCache *gc, bool sel) {
  if (sel)
    sel_start_at(e);
  else
    sel_clear(e);
  if (e->cursor.col > 0)
    e->cursor.col =
        line_utf8_prev(editor_line(e, e->cursor.row), e->cursor.col);
  else if (e->cursor.row > 0) {
    e->cursor.row--;
    e->cursor.col = line_len(editor_line(e, e->cursor.row));
  }
  refresh_preferred_x(e, gc);
}

static void move_right(Editor *e, GlyphCache *gc, bool sel) {
  if (sel)
    sel_start_at(e);
  else
    sel_clear(e);
  Line *l = editor_line(e, e->cursor.row);
  if (e->cursor.col < line_len(l))
    e->cursor.col = line_utf8_next(l, e->cursor.col);
  else if (e->cursor.row < editor_line_count(e) - 1) {
    e->cursor.row++;
    e->cursor.col = 0;
  }
  refresh_preferred_x(e, gc);
}

static void move_up(Editor *e, GlyphCache *gc, bool sel) {
  if (sel)
    sel_start_at(e);
  else
    sel_clear(e);
  if (e->cursor.row > 0) {
    Uint32 now = SDL_GetTicks();
    if (now - e->last_vert_move > VERTICAL_MOVE_RESET_MS)
      refresh_preferred_x(e, gc);
    e->last_vert_move = now;
    e->cursor.row--;
    Line *l = editor_line(e, e->cursor.row);
    e->cursor.col = line_col_at_x(l, gc, e->text_x, e->preferred_x);
  }
}

static void move_down(Editor *e, GlyphCache *gc, bool sel) {
  if (sel)
    sel_start_at(e);
  else
    sel_clear(e);
  if (e->cursor.row < editor_line_count(e) - 1) {
    Uint32 now = SDL_GetTicks();
    if (now - e->last_vert_move > VERTICAL_MOVE_RESET_MS)
      refresh_preferred_x(e, gc);
    e->last_vert_move = now;
    e->cursor.row++;
    Line *l = editor_line(e, e->cursor.row);
    e->cursor.col = line_col_at_x(l, gc, e->text_x, e->preferred_x);
  }
}

static void move_word_left(Editor *e, GlyphCache *gc, bool sel) {
  if (sel)
    sel_start_at(e);
  else
    sel_clear(e);
  Line *l = editor_line(e, e->cursor.row);
  if (e->cursor.col > 0) {
    int p = e->cursor.col;
    while (p > 0 &&
           !is_word_char((unsigned char)line_at(l, line_utf8_prev(l, p))))
      p = line_utf8_prev(l, p);
    p = line_word_start(l, p);
    e->cursor.col = p;
  } else if (e->cursor.row > 0) {
    e->cursor.row--;
    e->cursor.col = line_len(editor_line(e, e->cursor.row));
  }
  refresh_preferred_x(e, gc);
}

static void move_word_right(Editor *e, GlyphCache *gc, bool sel) {
  if (sel)
    sel_start_at(e);
  else
    sel_clear(e);
  Line *l = editor_line(e, e->cursor.row);
  int len = line_len(l);
  if (e->cursor.col < len) {
    int p = e->cursor.col;
    while (p < len && !is_word_char((unsigned char)line_at(l, p)))
      p = line_utf8_next(l, p);
    p = line_word_end(l, p);
    e->cursor.col = p;
  } else if (e->cursor.row < editor_line_count(e) - 1) {
    e->cursor.row++;
    e->cursor.col = 0;
  }
  refresh_preferred_x(e, gc);
}

static void move_home(Editor *e, GlyphCache *gc, bool sel, bool ctrl) {
  if (sel)
    sel_start_at(e);
  else
    sel_clear(e);
  if (ctrl) {
    e->cursor.row = 0;
    e->cursor.col = 0;
  } else {
    Line *l = editor_line(e, e->cursor.row);
    int len = line_len(l);
    int fs = 0;
    while (fs < len && (line_at(l, fs) == ' ' || line_at(l, fs) == '\t'))
      fs++;
    e->cursor.col = (e->cursor.col == fs) ? 0 : fs;
  }
  refresh_preferred_x(e, gc);
}

static void move_end(Editor *e, GlyphCache *gc, bool sel, bool ctrl) {
  if (sel)
    sel_start_at(e);
  else
    sel_clear(e);
  if (ctrl) {
    e->cursor.row = editor_line_count(e) - 1;
    e->cursor.col = line_len(editor_line(e, e->cursor.row));
  } else {
    e->cursor.col = line_len(editor_line(e, e->cursor.row));
  }
  refresh_preferred_x(e, gc);
}

static void move_page(Editor *e, GlyphCache *gc, SDL_Window *win, bool up,
                      bool sel) {
  if (sel)
    sel_start_at(e);
  else
    sel_clear(e);
  int rows = visible_rows(win);
  e->cursor.row += up ? -rows : rows;
  e->scroll_row += up ? -rows : rows;
  editor_clamp_cursor(e);
  Line *l = editor_line(e, e->cursor.row);
  e->cursor.col = line_col_at_x(l, gc, e->text_x, e->preferred_x);
  if (e->scroll_row < 0)
    e->scroll_row = 0;
}

/* =====================================================================
 * Select word under cursor
 * ===================================================================== */
static void select_word(Editor *e) {
  Line *l = editor_line(e, e->cursor.row);
  int col = e->cursor.col;
  int ws = line_word_start(l, col);
  int we = line_word_end(l, col);
  if (we > ws) {
    e->sel_anchor = (Pos){e->cursor.row, ws};
    e->cursor.col = we;
    e->sel_active = true;
  }
}

/* =====================================================================
 * Select all
 * ===================================================================== */
static void select_all(Editor *e) {
  e->sel_anchor = (Pos){0, 0};
  int last = editor_line_count(e) - 1;
  e->cursor = (Pos){last, line_len(editor_line(e, last))};
  e->sel_active = true;
}

/* =====================================================================
 * Goto line
 * ===================================================================== */
static void goto_line(Editor *e, GlyphCache *gc, int target) {
  target--; /* 1-based to 0-based */
  if (target < 0)
    target = 0;
  int n = editor_line_count(e);
  if (target >= n)
    target = n - 1;
  e->cursor.row = target;
  e->cursor.col = 0;
  refresh_preferred_x(e, gc);
  editor_set_status(e, "Jumped to line");
}

/* =====================================================================
 * Event handling: overlay / prompt modes
 * ===================================================================== */

/* Remove one trailing UTF-8 codepoint (not one byte) from a
   NUL-terminated prompt buffer. */
static void prompt_backspace(char *s) {
  size_t l = strlen(s);
  if (!l)
    return;
  size_t k = l - 1;
  while (k > 0 && ((unsigned char)s[k] & 0xC0) == 0x80)
    k--;
  s[k] = '\0';
}

static bool handle_overlay_textinput(Editor *e, GlyphCache *gc, SDL_Window *win,
                                     const char *text) {
  (void)gc;
  (void)win;
  switch (e->mode) {
  case MODE_RECOVERY_PROMPT:
    if (text[0] == 'r' || text[0] == 'R') {
      editor_deserialize(e, gc, e->rec_doc.data, e->rec_doc.len);
      e->doc_meta = e->rec_meta;
      /* Route through the standard dirty path so the restored content is
         journaled and autosaved like any user edit. */
      editor_mark_dirty(e);
      bytebuffer_free(&e->rec_doc);
      e->rec_available = false;
      e->mode = e->mode_pre_overlay;
      editor_set_status(e, "Recovered — Ctrl+S to save");
    } else if (text[0] == 'd' || text[0] == 'D') {
      bytebuffer_free(&e->rec_doc);
      e->rec_available = false;
      if (e->storage)
        storage_recovery_discard(e->storage);
      e->mode = e->mode_pre_overlay;
      editor_set_status(e, "Recovery discarded");
    }
    return true;
  case MODE_VERSION_RENAME: {
    size_t rl = strlen(e->ver_rename_buf);
    size_t tl = strlen(text);
    if (rl + tl < sizeof(e->ver_rename_buf) - 1)
      memcpy(e->ver_rename_buf + rl, text, tl + 1);
    ver_name_sanitize(e->ver_rename_buf);
    return true;
  }
  case MODE_PALETTE: {
    size_t fl = strlen(e->palette_filter);
    size_t tl = strlen(text);
    if (fl + tl < sizeof(e->palette_filter) - 1) {
      memcpy(e->palette_filter + fl, text, tl + 1);
    }
    e->palette_sel = 0;
    return true;
  }
  case MODE_GOTO_LINE: {
    size_t gl = strlen(e->goto_buf);
    if (text[0] >= '0' && text[0] <= '9' && gl < sizeof(e->goto_buf) - 1) {
      e->goto_buf[gl] = text[0];
      e->goto_buf[gl + 1] = '\0';
    }
    return true;
  }
  case MODE_SAVE_PROMPT:
  case MODE_OPEN_PROMPT:
  case MODE_FIND:
  case MODE_REPLACE: {
    char *target = (e->mode == MODE_REPLACE && e->prompt_field == 1)
                       ? e->replace
                       : e->prompt;
    size_t tlen = strlen(target);
    size_t ilen = strlen(text);
    size_t cap = sizeof(e->prompt); /* both prompt and replace are same size */
    if (tlen + ilen < cap)
      memcpy(target + tlen, text, ilen + 1);
    if (e->mode == MODE_FIND || e->mode == MODE_REPLACE)
      editor_update_search(e);
    return true;
  }
  default:
    return false;
  }
}

static bool handle_overlay_keydown(Editor *e, GlyphCache *gc, SDL_Window *win,
                                   SDL_Keycode sym, SDL_Keymod mod) {
  bool shift = (mod & KMOD_SHIFT) != 0;
  bool ctrl = (mod & KMOD_CTRL) != 0;

  if (e->mode == MODE_RECOVERY_PROMPT) {
    if (sym == SDLK_ESCAPE) {
      bytebuffer_free(&e->rec_doc);
      e->rec_available = false;
      if (e->storage)
        storage_recovery_discard(e->storage);
      e->mode = e->mode_pre_overlay;
    }
    return true;
  }

  if (e->mode == MODE_QUIT_PROMPT) {
    /* SDL keycodes are lowercase; Shift is reported via the modifier. */
    if (sym == SDLK_s) {
      if (editor_do_save(e, gc)) {
        editor_discard_recovery_state(e); /* tidy stale autosave */
        editor_request_quit(e);
      }
      /* save failed: stay open, status bar shows the error */
      return true;
    }
    if (sym == SDLK_d) {
      editor_discard_recovery_state(e);
      editor_request_quit(e);
      return true;
    }
    if (sym == SDLK_ESCAPE) {
      e->mode = e->mode_pre_overlay;
      editor_set_status(e, "Quit cancelled");
      return true;
    }
    return true;
  }

  if (e->mode == MODE_VERSION_BROWSER) {
    size_t n = e->storage ? storage_history_count(e->storage) : 0;
    if (sym == SDLK_ESCAPE) {
      if (e->ver_delete_armed) {
        e->ver_delete_armed = false;
        return true;
      }
      e->mode = e->mode_pre_overlay;
      return true;
    }
    if (e->ver_delete_armed) {
      if (sym == SDLK_RETURN) {
        storage_history_delete(e->storage, (size_t)e->ver_sel);
        e->ver_delete_armed = false;
        size_t remaining =
            e->storage ? storage_history_count(e->storage) : 0;
        if ((size_t)e->ver_sel >= remaining && remaining > 0)
          e->ver_sel = (int)remaining - 1;
        ver_sync_scroll(e);
        editor_set_status(e, "Version deleted — Ctrl+S to persist");
      } else if (sym != SDLK_d && sym != SDLK_RETURN) {
        e->ver_delete_armed = false;
      }
      return true;
    }
    if (n == 0)
      return true; /* empty state: only Esc leaves */
    if (sym == SDLK_UP && e->ver_sel > 0)
      e->ver_sel--;
    else if (sym == SDLK_DOWN && (size_t)e->ver_sel + 1 < n)
      e->ver_sel++;
    else if (sym == SDLK_PAGEUP)
      e->ver_sel -= VER_BROWSER_VISIBLE;
    else if (sym == SDLK_PAGEDOWN)
      e->ver_sel += VER_BROWSER_VISIBLE;
    else if (sym == SDLK_r) {
      const StorageVersion *v =
          storage_history_at(e->storage, (size_t)e->ver_sel);
      snprintf(e->ver_rename_buf, sizeof(e->ver_rename_buf), "%s", v->name);
      e->ver_rename_target = e->ver_sel;
      e->mode = MODE_VERSION_RENAME;
    } else if (sym == SDLK_d) {
      e->ver_delete_armed = true;
    } else if (sym == SDLK_RETURN) {
      restore_selected_version(e, gc);
      return true;
    }
    if (e->ver_sel < 0)
      e->ver_sel = 0;
    if ((size_t)e->ver_sel >= n)
      e->ver_sel = (int)n - 1;
    ver_sync_scroll(e);
    return true;
  }

  if (e->mode == MODE_VERSION_RENAME) {
    if (sym == SDLK_ESCAPE) {
      e->mode = MODE_VERSION_BROWSER;
      return true;
    }
    if (sym == SDLK_BACKSPACE) {
      prompt_backspace(e->ver_rename_buf);
      return true;
    }
    if (sym == SDLK_RETURN) {
      if (e->ver_rename_buf[0]) {
        ver_name_sanitize(e->ver_rename_buf);
        storage_history_rename(e->storage, (size_t)e->ver_rename_target,
                               e->ver_rename_buf);
        char msg[192];
        snprintf(msg, sizeof(msg), "Renamed to %s — Ctrl+S to persist",
                 e->ver_rename_buf);
        editor_set_status(e, msg);
      }
      e->mode = MODE_VERSION_BROWSER;
      return true;
    }
    return true;
  }

  if (e->mode == MODE_PALETTE) {
    if (sym == SDLK_ESCAPE) {
      e->mode = e->mode_pre_overlay;
      return true;
    }
    if (sym == SDLK_BACKSPACE) {
      prompt_backspace(e->palette_filter);
      e->palette_sel = 0;
      return true;
    }
    if (sym == SDLK_UP) {
      if (e->palette_sel > 0)
        e->palette_sel--;
      return true;
    }
    if (sym == SDLK_DOWN) {
      e->palette_sel++;
      return true;
    }
    if (sym == SDLK_RETURN) {
      /* Find the sel-th visible entry */
      int shown = 0;
      for (int i = 0; i < e->palette_count; i++) {
        if (e->palette_filter[0] &&
            stristr(e->palette[i].label, e->palette_filter) == NULL)
          continue;
        if (shown == e->palette_sel) {
          e->mode = e->mode_pre_overlay;
          e->palette[i].fn(e);
          return true;
        }
        shown++;
      }
      e->mode = e->mode_pre_overlay;
      return true;
    }
    return true;
  }

  if (e->mode == MODE_GOTO_LINE) {
    if (sym == SDLK_ESCAPE) {
      e->mode = e->mode_pre_overlay;
      return true;
    }
    if (sym == SDLK_BACKSPACE) {
      prompt_backspace(e->goto_buf);
      return true;
    }
    if (sym == SDLK_RETURN) {
      int ln = atoi(e->goto_buf);
      e->mode = e->mode_pre_overlay;
      if (ln > 0)
        goto_line(e, gc, ln);
      return true;
    }
    return true;
  }

  /* Save/open/find/replace */
  if (sym == SDLK_ESCAPE) {
    e->mode = e->mode_pre_overlay;
    e->search_active = false;
    return true;
  }
  if (sym == SDLK_BACKSPACE) {
    bool in_replace = (e->mode == MODE_REPLACE && e->prompt_field == 1);
    prompt_backspace(in_replace ? e->replace : e->prompt);
    if (e->mode == MODE_FIND || e->mode == MODE_REPLACE)
      editor_update_search(e);
    return true;
  }
  if (sym == SDLK_TAB && e->mode == MODE_REPLACE) {
    e->prompt_field ^= 1;
    return true;
  }
  if (sym == SDLK_RETURN) {
    if (e->mode == MODE_SAVE_PROMPT) {
      if (e->prompt[0]) {
        snprintf(e->filename, sizeof(e->filename), "%s", e->prompt);
        /* Re-detect language if extension changed */
        ByteBuffer cb;
        editor_serialize(e, &cb);
        e->lang = lang_detect(e->filename, (const char *)cb.data, cb.len);
        bytebuffer_free(&cb);
        e->mode = e->mode_pre_overlay;
        editor_do_save(e, gc);
      }
    } else if (e->mode == MODE_OPEN_PROMPT) {
      if (e->prompt[0]) {
        char path[1024];
        snprintf(path, sizeof(path), "%s", e->prompt);
        e->mode = e->mode_pre_overlay;
        editor_do_load(e, gc, path);
      }
    } else if (e->mode == MODE_FIND) {
      editor_find_next(e, !shift);
      editor_scroll_to_cursor(e, win);
    } else if (e->mode == MODE_REPLACE) {
      if (ctrl)
        editor_replace_all(e, gc);
      else
        editor_replace_current(e, gc);
      editor_scroll_to_cursor(e, win);
    }
    return true;
  }
  if (ctrl && sym == SDLK_a && e->mode == MODE_REPLACE) {
    editor_replace_all(e, gc);
    return true;
  }
  return true;
}

/* =====================================================================
 * Main keyboard handler (normal/insert mode)
 * ===================================================================== */
static void handle_keydown(Editor *e, GlyphCache *gc, SDL_Window *win,
                           SDL_Keycode sym, SDL_Keymod mod) {
  bool shift = (mod & KMOD_SHIFT) != 0;
  bool ctrl = (mod & KMOD_CTRL) != 0;
  bool alt = (mod & KMOD_ALT) != 0;

  /* Mode toggle */
  if (sym == SDLK_ESCAPE) {
    sel_clear(e);
    e->mode = MODE_NORMAL;
    e->search_active = false;
    return;
  }
  /* Mode toggle — guarded against Ctrl/Alt chords so Ctrl+A / Alt+letters
     fall through to their real bindings below. */
  if (e->mode == MODE_NORMAL && !ctrl && !alt && sym == SDLK_i) {
    e->mode = MODE_INSERT;
    return;
  }
  if (e->mode == MODE_NORMAL && !ctrl && !alt && sym == SDLK_a) {
    e->mode = MODE_INSERT;
    move_right(e, gc, false);
    return;
  }

  /* Open command palette */
  if (ctrl && sym == SDLK_p) {
    e->mode_pre_overlay = e->mode;
    e->mode = MODE_PALETTE;
    e->palette_filter[0] = '\0';
    e->palette_sel = 0;
    return;
  }

  /* Version history browser */
  if (ctrl && alt && sym == SDLK_v) {
    open_version_browser(e);
    return;
  }

  /* Navigation (both modes) */
  if (sym == SDLK_LEFT) {
    ctrl ? move_word_left(e, gc, shift) : move_left(e, gc, shift);
    return;
  }
  if (sym == SDLK_RIGHT) {
    ctrl ? move_word_right(e, gc, shift) : move_right(e, gc, shift);
    return;
  }
  if (sym == SDLK_UP) {
    alt ? ed_move_line(e, gc, -1) : move_up(e, gc, shift);
    return;
  }
  if (sym == SDLK_DOWN) {
    alt ? ed_move_line(e, gc, +1) : move_down(e, gc, shift);
    return;
  }
  if (sym == SDLK_HOME) {
    move_home(e, gc, shift, ctrl);
    return;
  }
  if (sym == SDLK_END) {
    move_end(e, gc, shift, ctrl);
    return;
  }
  if (sym == SDLK_PAGEUP) {
    move_page(e, gc, win, true, shift);
    return;
  }
  if (sym == SDLK_PAGEDOWN) {
    move_page(e, gc, win, false, shift);
    return;
  }

  /* Selection */
  if (ctrl && sym == SDLK_a) {
    select_all(e);
    return;
  }

  /* Ctrl shortcuts (both modes) */
  /* Save As must be checked before the plain-save test: Ctrl+Shift+S also
     matches Ctrl+S. Plain save is direct when the file already has a real
     name; the prompt is only needed to name a brand-new file. */
  if (ctrl && shift && sym == SDLK_s) {
    e->mode_pre_overlay = e->mode;
    cmd_save(e);
    return;
  }
  if (ctrl && sym == SDLK_s) {
    if (strcmp(e->filename, DEFAULT_FILENAME) == 0) {
      e->mode_pre_overlay = e->mode;
      cmd_save(e);
    } else {
      editor_do_save(e, gc);
    }
    return;
  }
  if (ctrl && sym == SDLK_o) {
    e->mode_pre_overlay = e->mode;
    cmd_open(e);
    return;
  }
  if (ctrl && sym == SDLK_f) {
    e->mode_pre_overlay = e->mode;
    e->search_active = false;
    cmd_find(e);
    return;
  }
  if (ctrl && sym == SDLK_h) {
    e->mode_pre_overlay = e->mode;
    cmd_replace(e);
    return;
  }
  if (ctrl && sym == SDLK_g) {
    e->mode_pre_overlay = e->mode;
    cmd_goto_line(e);
    return;
  }
  /* Redo must be checked before undo: Ctrl+Shift+Z also matches the
     plain Ctrl+Z test. */
  if (ctrl && shift && sym == SDLK_z) {
    editor_do_redo(e, gc);
    return;
  }
  if (ctrl && sym == SDLK_z) {
    editor_do_undo(e, gc);
    return;
  }
  if (ctrl && sym == SDLK_y) {
    editor_do_redo(e, gc);
    return;
  }
  if (ctrl && sym == SDLK_c) {
    ed_copy(e);
    return;
  }
  if (ctrl && sym == SDLK_x) {
    ed_cut(e, gc);
    return;
  }
  if (ctrl && sym == SDLK_v) {
    ed_paste(e, gc);
    return;
  }
  if (ctrl && sym == SDLK_d) {
    ed_duplicate_line(e, gc);
    return;
  }
  if (ctrl && sym == SDLK_k) {
    ed_kill_line(e, gc);
    return;
  }
  if (ctrl && sym == SDLK_SLASH) {
    ed_toggle_comment(e, gc);
    return;
  }
  if (ctrl && sym == SDLK_n) {
    Line *fl = calloc(1, sizeof(Line));
    if (!fl || !line_init(fl)) {
      free(fl);
      editor_set_status(e, "New file failed: out of memory");
      return;
    }
    lb_destroy(&e->lb);
    lb_init(&e->lb);
    if (!lb_insert_line(&e->lb, 0, fl)) {
      line_free(fl);
      free(fl);
      editor_set_status(e, "New file failed: out of memory");
      return;
    }
    if (e->storage) {
      storage_session_close(e->storage);
      e->storage = NULL;
    }
    memset(&e->doc_meta, 0, sizeof(e->doc_meta));
    e->doc_meta.created_at = (uint64_t)time(NULL);
    e->doc_meta.modified_at = e->doc_meta.created_at;
    e->doc_meta.schema_version = 1;
    e->cursor = (Pos){0, 0};
    e->scroll_row = 0;
    snprintf(e->filename, sizeof(e->filename), "%s", DEFAULT_FILENAME);
    e->dirty = false;
    e->journal_pending = false;
    e->journal_error_shown = false;
    e->last_journal_ms = SDL_GetTicks();
    bytebuffer_free(&e->rec_doc);
    e->rec_available = false;
    uring_destroy(&e->uring);
    uring_init(&e->uring);
    e->lang = lang_detect(DEFAULT_FILENAME, NULL, 0);
    editor_update_gutter(e, gc);
    editor_set_status(e, "New file");
    return;
  }

  /* Word selection with double-click feel: Ctrl+W */
  if (ctrl && sym == SDLK_w) {
    select_word(e);
    return;
  }

  /* Edit operations (insert mode only) */
  if (e->mode != MODE_INSERT)
    return;

  if (sym == SDLK_RETURN) {
    ed_newline(e, gc);
    return;
  }
  if (sym == SDLK_BACKSPACE) {
    ed_backspace(e, gc, ctrl);
    return;
  }
  if (sym == SDLK_DELETE) {
    ed_delete_fwd(e, gc, ctrl);
    return;
  }
  if (sym == SDLK_TAB) {
    if (e->sel_active) {
      ed_indent_lines(e, gc, shift);
    } else if (shift) {
      ed_indent_lines(e, gc, true);
    } else {
      ed_insert_text(e, gc, "    ", 4, true);
    }
    return;
  }
}

/* =====================================================================
 * Main
 * ===================================================================== */
int main(int argc, char *argv[]) {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }

  SDL_Window *win =
      SDL_CreateWindow("editor", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                       1000, 700, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
  if (!win) {
    SDL_Quit();
    return 1;
  }

  SDL_Renderer *ren = SDL_CreateRenderer(
      win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!ren) {
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 1;
  }

  FT_Library ft;
  if (FT_Init_FreeType(&ft)) {
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 1;
  }

  FT_Face face;
  const char *fpaths[] = {
      "assets/font.ttf", "../assets/font.ttf",
      "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"};
  bool font_loaded = false;
  for (int i = 0; i < (int)(sizeof(fpaths) / sizeof(fpaths[0])); i++) {
    if (FT_New_Face(ft, fpaths[i], 0, &face) == 0) {
      font_loaded = true;
      break;
    }
  }
  if (!font_loaded) {
    fprintf(stderr, "No font found\n");
    FT_Done_FreeType(ft);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 1;
  }
  FT_Set_Pixel_Sizes(face, 0, FONT_SIZE_PX);

  GlyphCache gc;
  glyph_cache_init(&gc, face, ren);

  Editor editor;
  memset(&editor, 0, sizeof(editor));
  editor.gc = &gc;
  lb_init(&editor.lb);
  uring_init(&editor.uring);
  bytebuffer_init(&editor.rec_doc);
  editor.mode = MODE_NORMAL;
  snprintf(editor.filename, sizeof(editor.filename), "%s", DEFAULT_FILENAME);
  editor_register_commands(&editor);

  /* Bootstrap with one empty line */
  Line *first = calloc(1, sizeof(Line));
  if (!first || !line_init(first)) {
    fprintf(stderr, "fatal: out of memory initializing buffer\n");
    return 1;
  }
  if (!lb_insert_line(&editor.lb, 0, first)) {
    fprintf(stderr, "fatal: out of memory initializing buffer\n");
    line_free(first);
    free(first);
    return 1;
  }
  editor_update_gutter(&editor, &gc);

  const char *target = (argc >= 2) ? argv[1] : DEFAULT_FILENAME;
  if (!editor_do_load(&editor, &gc, target)) {
    /* New file — leave the empty line in place */
    snprintf(editor.filename, sizeof(editor.filename), "%s", target);
    editor.lang = lang_detect(target, NULL, 0);
    /* Open storage session for new file anyway (creates journal etc.) */
    StorageSession *ses = NULL;
    ByteBuffer dummy;
    StorageMetadata dmeta;
    StorageOpenResult dres;
    if (storage_session_open(target, &ses, &dummy, &dmeta, &dres) ==
        STORAGE_OK) {
      bytebuffer_free(&dummy);
      editor.storage = ses;
    }
  }

  SDL_StartTextInput();

  Uint32 last_blink = SDL_GetTicks();
  bool cursor_vis = true;
  bool autosave_flash = false;
  Uint32 autosave_flash_until = 0;

  while (1) {
    bool cursor_moved = false;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_QUIT) {
        bool has_unsaved =
            editor.dirty ||
            (editor.storage && storage_history_dirty(editor.storage)) ||
            editor.rec_available;
        if (editor.mode == MODE_QUIT_PROMPT || !has_unsaved) {
          /* Second close request, or nothing to lose: discard any
             crash-recovery sidecars so the next open is clean. */
          editor_discard_recovery_state(&editor);
          goto shutdown;
        }
        editor.mode_pre_overlay = editor.mode;
        editor.mode = MODE_QUIT_PROMPT;
        continue;
      }

      if (ev.type == SDL_MOUSEWHEEL) {
        editor.scroll_row -= ev.wheel.y * 3;
        int ms = editor_line_count(&editor) - 1;
        if (editor.scroll_row < 0)
          editor.scroll_row = 0;
        if (editor.scroll_row > ms)
          editor.scroll_row = ms;
        continue;
      }

      if (ev.type == SDL_MOUSEBUTTONDOWN &&
          ev.button.button == SDL_BUTTON_LEFT) {
        if (editor.mode == MODE_RECOVERY_PROMPT ||
            editor.mode == MODE_PALETTE || editor.mode == MODE_SAVE_PROMPT ||
            editor.mode == MODE_OPEN_PROMPT || editor.mode == MODE_FIND ||
            editor.mode == MODE_REPLACE || editor.mode == MODE_GOTO_LINE ||
            editor.mode == MODE_QUIT_PROMPT ||
            editor.mode == MODE_VERSION_BROWSER ||
            editor.mode == MODE_VERSION_RENAME)
          continue;
        int mx = ev.button.x, my = ev.button.y;
        int r = (my - FIRST_LINE_Y) / LINE_HEIGHT_PX + editor.scroll_row;
        if (r < 0)
          r = 0;
        if (r >= editor_line_count(&editor))
          r = editor_line_count(&editor) - 1;
        editor.cursor.row = r;
        Line *l = editor_line(&editor, r);
        editor.cursor.col = line_col_at_x(l, &gc, editor.text_x, mx);
        if (ev.button.clicks == 2)
          select_word(&editor);
        else if (ev.button.clicks == 3) {
          editor.sel_anchor = (Pos){r, 0};
          editor.cursor.col = line_len(l);
          editor.sel_active = true;
        } else
          sel_clear(&editor);
        refresh_preferred_x(&editor, &gc);
        cursor_moved = true;
        continue;
      }

      /* Mouse drag for selection */
      if (ev.type == SDL_MOUSEMOTION && (ev.motion.state & SDL_BUTTON_LMASK)) {
        if (editor.mode == MODE_NORMAL || editor.mode == MODE_INSERT) {
          int r =
              (ev.motion.y - FIRST_LINE_Y) / LINE_HEIGHT_PX + editor.scroll_row;
          if (r < 0)
            r = 0;
          if (r >= editor_line_count(&editor))
            r = editor_line_count(&editor) - 1;
          if (!editor.sel_active) {
            editor.sel_anchor = editor.cursor;
            editor.sel_active = true;
          }
          editor.cursor.row = r;
          Line *l = editor_line(&editor, r);
          editor.cursor.col = line_col_at_x(l, &gc, editor.text_x, ev.motion.x);
          cursor_moved = true;
        }
        continue;
      }

      if (ev.type == SDL_TEXTINPUT) {
        bool in_overlay =
            (editor.mode != MODE_NORMAL && editor.mode != MODE_INSERT);
        if (in_overlay) {
          handle_overlay_textinput(&editor, &gc, win, ev.text.text);
        } else if (editor.mode == MODE_INSERT) {
          ed_insert_text(&editor, &gc, ev.text.text,
                         (int)strlen(ev.text.text), true);
          cursor_moved = true;
        }
        continue;
      }

      if (ev.type == SDL_KEYDOWN) {
        SDL_Keycode sym = ev.key.keysym.sym;
        SDL_Keymod mod = SDL_GetModState();
        bool in_overlay =
            (editor.mode != MODE_NORMAL && editor.mode != MODE_INSERT);
        if (in_overlay) {
          handle_overlay_keydown(&editor, &gc, win, sym, mod);
        } else {
          handle_keydown(&editor, &gc, win, sym, mod);
        }
        editor_clamp_cursor(&editor);
        editor_bracket_match(&editor);
        editor_scroll_to_cursor(&editor, win);
        cursor_moved = true;
      }
    }

    Uint32 now = SDL_GetTicks();
    if (editor.quit_requested)
      goto shutdown;
    editor_journal_tick(&editor, now);
    /* Only serialize the document when an autosave can actually fire;
       the old path built a full-document ByteBuffer every frame. */
    if (editor.storage && storage_should_autosave(editor.storage, now)) {
      ByteBuffer asav;
      editor_serialize(&editor, &asav);
      bool fired =
          storage_autosave_tick(editor.storage, &asav, &editor.doc_meta, now);
      bytebuffer_free(&asav);
      if (fired) {
        autosave_flash = true;
        autosave_flash_until = now + 1500;
      }
    }
    if (autosave_flash && now > autosave_flash_until)
      autosave_flash = false;

    /* Cursor blink */
    if (cursor_moved) {
      cursor_vis = true;
      last_blink = now;
    } else if (now - last_blink >= CURSOR_BLINK_MS) {
      cursor_vis = !cursor_vis;
      last_blink = now;
    }
    /* Cursor visibility gates only the caret; the current-line highlight
       and relative gutter stay stable across blink frames. */
    SDL_SetRenderDrawColor(ren, 18, 18, 22, 255);
    SDL_RenderClear(ren);
    render_document(ren, &gc, &editor, win, cursor_vis);
    render_status_bar(ren, &gc, &editor, win);

    if (autosave_flash) {
      int ww;
      SDL_GetWindowSize(win, &ww, NULL);
      int ax = ww - 140, ay = 18;
      Color flash = THEMET.accent;
      float ft = (autosave_flash_until > now)
                     ? 1.0f - (float)(autosave_flash_until - now) / 1500.0f
                     : 1.0f;
      flash = color_lerp(THEMET.accent, THEMET.bg, ft * 0.6f);
      render_str_c(&gc, "~ autosaved", &ax, ay, flash);
    }

    /* Overlay modes */
    switch (editor.mode) {
    case MODE_RECOVERY_PROMPT:
      render_recovery_dialog(ren, &gc, &editor, win);
      break;
    case MODE_QUIT_PROMPT:
      render_quit_dialog(ren, &gc, &editor, win);
      break;
    case MODE_VERSION_BROWSER:
      render_version_browser(ren, &gc, &editor, win);
      break;
    case MODE_VERSION_RENAME:
      render_version_rename(ren, &gc, &editor, win);
      break;
    case MODE_PALETTE:
      render_palette(ren, &gc, &editor, win);
      break;
    case MODE_SAVE_PROMPT:
    case MODE_OPEN_PROMPT:
    case MODE_FIND:
    case MODE_REPLACE:
      render_prompt_dialog(ren, &gc, &editor, win);
      break;
    case MODE_GOTO_LINE: {
      int ww, wh;
      SDL_GetWindowSize(win, &ww, &wh);
      render_overlay_bg(ren, ww, wh);
      int dw = 340, dh = 160, dx = (ww - dw) / 2, dy = wh / 3;
      render_dialog_box(ren, &gc, dx, dy, dw, dh, "GO TO LINE", THEMET.accent);
      render_input_field(ren, &gc, dx + 20, dy + 84, dw - 40, 38, "LINE NUMBER",
                         editor.goto_buf, true, now);
      {
        int fy = dy + dh - 22;
        render_hint_chip(&gc, ren, dx + 20, fy, "Enter", "jump",
                         THEMET.success);
        render_hint_chip(&gc, ren, dx + 150, fy, "Esc", "cancel",
                         THEMET.danger);
      }
      break;
    }
    default:
      break;
    }

    SDL_RenderPresent(ren);
    SDL_Delay(1);
  }

shutdown:
  SDL_StopTextInput();
  lb_destroy(&editor.lb);
  uring_destroy(&editor.uring);
  bytebuffer_free(&editor.rec_doc);
  if (editor.storage)
    storage_session_close(editor.storage);
  glyph_cache_destroy(&gc);
  FT_Done_Face(face);
  FT_Done_FreeType(ft);
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
  SDL_Quit();
  return 0;
}
