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

#define LINE_INITIAL_CAPACITY 64
#define LINES_INITIAL_CAPACITY 16
#define FONT_SIZE_PX 24
#define LINE_HEIGHT_PX 40
#define TEXT_X_ORIGIN 80
#define GUTTER_X 20
#define FIRST_LINE_Y 40
#define CURSOR_BLINK_MS 500
#define VERTICAL_MOVE_RESET_MS 2000
#define CLIPBOARD_MAX (1 << 20)

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

static void rebuild_line_cache(FT_Face face, Line *l) {
  int len = line_length(l);
  if (!l->advance_cache) {
    l->advance_cache = calloc((size_t)(l->capacity + 1), sizeof(int));
    if (!l->advance_cache)
      return;
  }

  int x = TEXT_X_ORIGIN;
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

static int get_line_x_position(const Line *l, int col) {
  if (col <= 0)
    return TEXT_X_ORIGIN;
  int len = line_length(l);
  if (col > len)
    col = len;
  if (!l->advance_cache)
    return TEXT_X_ORIGIN;
  return l->advance_cache[col];
}

static int get_closest_column(const Line *l, int target_x) {
  int len = line_length(l);
  if (len == 0)
    return 0;

  int best_col = 0;
  int best_dist = abs(get_line_x_position(l, 0) - target_x);

  for (int col = 0; col < len;) {
    uint32_t cp = 0;
    int bytes = 0;
    if (!line_decode_codepoint(l, col, &cp, &bytes) || bytes <= 0)
      bytes = 1;
    int next_col = col + bytes;
    int dist = abs(get_line_x_position(l, next_col) - target_x);
    if (dist < best_dist) {
      best_dist = dist;
      best_col = next_col;
    }
    col = next_col;
  }
  return best_col;
}

static void refresh_preferred_x(Editor *e) {
  e->preferred_x = get_line_x_position(&e->lines[e->cursor_row], e->cursor_col);
}

static void render_glyph(SDL_Renderer *renderer, FT_Face face, uint32_t cp,
                         int *x, int y, Uint8 r, Uint8 g, Uint8 b) {
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
    rebuild_line_cache(face, l);
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
    rebuild_line_cache(face, first);

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
  return (wh - FIRST_LINE_Y) / LINE_HEIGHT_PX;
}

int main(void) {
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

  editor.lines = calloc((size_t)editor.line_capacity, sizeof(Line));
  if (!editor.lines || !editor.clipboard)
    goto cleanup;

  if (!line_init(&editor.lines[0]))
    goto cleanup;
  rebuild_line_cache(face, &editor.lines[0]);
  editor.preferred_x = TEXT_X_ORIGIN;

  bool running = true;
  Uint32 last_blink = SDL_GetTicks();
  bool cursor_visible = true;
  bool cursor_moving = false;

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
              get_closest_column(&editor.lines[clicked_row], mx);
          editor.preferred_x = get_line_x_position(
              &editor.lines[editor.cursor_row], editor.cursor_col);
          editor_clear_selection(&editor);
          cursor_moving = true;
          cursor_visible = true;
        }
        break;
      }

      case SDL_TEXTINPUT: {
        if (editor.selection_active)
          editor_delete_selection(&editor, face);
        Line *line = &editor.lines[editor.cursor_row];
        int inserted_bytes = (int)strlen(ev.text.text);
        if (inserted_bytes > 0 &&
            line_insert_bytes(line, editor.cursor_col, ev.text.text,
                              inserted_bytes)) {
          editor.cursor_col += inserted_bytes;
          rebuild_line_cache(face, line);
          editor.preferred_x = get_line_x_position(line, editor.cursor_col);
        }
        break;
      }

      case SDL_KEYDOWN: {
        Line *line = &editor.lines[editor.cursor_row];
        SDL_Keymod mod = SDL_GetModState();
        SDL_Keycode sym = ev.key.keysym.sym;

        bool shift = (mod & KMOD_SHIFT) != 0;
        bool ctrl = (mod & KMOD_CTRL) != 0;

        if (sym == SDLK_BACKSPACE) {
          if (editor.selection_active) {
            editor_delete_selection(&editor, face);
            line = &editor.lines[editor.cursor_row];
            rebuild_line_cache(face, line);
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
            rebuild_line_cache(face, line);
            editor.preferred_x = get_line_x_position(line, editor.cursor_col);
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
            rebuild_line_cache(face, prev_line);
            line_free(line);

            for (int i = editor.cursor_row; i < editor.line_count - 1; i++)
              editor.lines[i] = editor.lines[i + 1];
            memset(&editor.lines[editor.line_count - 1], 0, sizeof(Line));
            editor.line_count--;
            editor.cursor_row = prev_row;
            editor.cursor_col = prev_len;
            editor.preferred_x = get_line_x_position(
                &editor.lines[editor.cursor_row], editor.cursor_col);
          }
          cursor_moving = true;
          cursor_visible = true;
        }

        else if (sym == SDLK_DELETE) {
          if (editor.selection_active) {
            editor_delete_selection(&editor, face);
            line = &editor.lines[editor.cursor_row];
            rebuild_line_cache(face, line);
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
              rebuild_line_cache(face, line);
              editor.preferred_x = get_line_x_position(line, editor.cursor_col);
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
              rebuild_line_cache(face, line);
              line_free(next_line);
              for (int i = editor.cursor_row + 1; i < editor.line_count - 1;
                   i++)
                editor.lines[i] = editor.lines[i + 1];
              memset(&editor.lines[editor.line_count - 1], 0, sizeof(Line));
              editor.line_count--;
            }
          }
          cursor_moving = true;
          cursor_visible = true;
        }

        else if (sym == SDLK_TAB) {
          if (editor.selection_active)
            editor_delete_selection(&editor, face);
          line = &editor.lines[editor.cursor_row];
          const char *indent = "    ";
          if (line_insert_bytes(line, editor.cursor_col, indent, 4)) {
            editor.cursor_col += 4;
            rebuild_line_cache(face, line);
            editor.preferred_x = get_line_x_position(line, editor.cursor_col);
          }
        }

        else if (sym == SDLK_RETURN) {
          if (editor.selection_active)
            editor_delete_selection(&editor, face);
          line = &editor.lines[editor.cursor_row];

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

          if (split_len > 0) {
            if (!line_ensure_gap(&new_line, split_len)) {
              line_free(&new_line);
              running = false;
              break;
            }
            for (int i = 0; i < split_len; i++)
              new_line.data[i] = line_char_at(line, editor.cursor_col + i);
            new_line.gap_start = split_len;
            new_line.gap_end = new_line.capacity;
          }

          rebuild_line_cache(face, &new_line);
          line_truncate(line, editor.cursor_col);
          rebuild_line_cache(face, line);

          editor.lines[editor.cursor_row + 1] = new_line;
          editor.line_count++;
          editor.cursor_row++;
          editor.cursor_col = 0;
          editor.preferred_x = TEXT_X_ORIGIN;
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
            editor.cursor_col = 0;
          }
          editor.preferred_x = get_line_x_position(
              &editor.lines[editor.cursor_row], editor.cursor_col);
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
              &editor.lines[editor.cursor_row], editor.cursor_col);
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
              &editor.lines[editor.cursor_row], editor.preferred_x);
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
              &editor.lines[editor.cursor_row], editor.preferred_x);
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
              &editor.lines[editor.cursor_row], editor.cursor_col);
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
              &editor.lines[editor.cursor_row], editor.cursor_col);
          cursor_moving = true;
          cursor_visible = true;
        }

        else if (sym == SDLK_UP) {
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
            editor.cursor_col = get_closest_column(
                &editor.lines[editor.cursor_row], editor.preferred_x);
            if (editor.cursor_row < editor.scroll_row)
              editor.scroll_row = editor.cursor_row;
            cursor_moving = true;
            cursor_visible = true;
          }
        }

        else if (sym == SDLK_DOWN) {
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
            editor.cursor_col = get_closest_column(
                &editor.lines[editor.cursor_row], editor.preferred_x);
            int vis = visible_rows(window);
            if (editor.cursor_row >= editor.scroll_row + vis)
              editor.scroll_row = editor.cursor_row - vis + 1;
            cursor_moving = true;
            cursor_visible = true;
          }
        }

        else if (ctrl && sym == SDLK_a) {
          editor.selection_active = true;
          editor.sel_anchor_row = 0;
          editor.sel_anchor_col = 0;
          editor.cursor_row = editor.line_count - 1;
          editor.cursor_col = line_length(&editor.lines[editor.cursor_row]);
          editor.preferred_x = get_line_x_position(
              &editor.lines[editor.cursor_row], editor.cursor_col);
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
          if (editor.selection_active && editor.clipboard) {
            editor_selection_to_buf(&editor, editor.clipboard, CLIPBOARD_MAX);
            editor.clipboard_len = (int)strlen(editor.clipboard);
            SDL_SetClipboardText(editor.clipboard);
            editor_delete_selection(&editor, face);
            line = &editor.lines[editor.cursor_row];
            rebuild_line_cache(face, line);
          }
          cursor_moving = true;
          cursor_visible = true;
        }

        else if (ctrl && sym == SDLK_v) {
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
                rebuild_line_cache(face, line);
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
                rebuild_line_cache(face, &new_line);
                line_truncate(line, editor.cursor_col);
                rebuild_line_cache(face, line);

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
            editor.preferred_x = get_line_x_position(line, editor.cursor_col);
          }
          cursor_moving = true;
          cursor_visible = true;
        }

        else if (ctrl && sym == SDLK_d) {
          line = &editor.lines[editor.cursor_row];
          int len = line_length(line);
          if (editor.cursor_row < editor.line_count - 1) {
            line_free(line);
            for (int i = editor.cursor_row; i < editor.line_count - 1; i++)
              editor.lines[i] = editor.lines[i + 1];
            memset(&editor.lines[editor.line_count - 1], 0, sizeof(Line));
            editor.line_count--;
            if (editor.cursor_row >= editor.line_count)
              editor.cursor_row = editor.line_count - 1;
            int new_len = line_length(&editor.lines[editor.cursor_row]);
            if (editor.cursor_col > new_len)
              editor.cursor_col = new_len;
          } else {
            line_truncate(line, 0);
            editor.cursor_col = 0;
            rebuild_line_cache(face, line);
          }
          (void)len;
          line = &editor.lines[editor.cursor_row];
          rebuild_line_cache(face, line);
          editor.preferred_x = get_line_x_position(line, editor.cursor_col);
          cursor_moving = true;
          cursor_visible = true;
        }

        else if (ctrl && sym == SDLK_k) {
          line = &editor.lines[editor.cursor_row];
          int len = line_length(line);
          if (editor.cursor_col < len) {
            line_truncate(line, editor.cursor_col);
            rebuild_line_cache(face, line);
          } else if (editor.cursor_row < editor.line_count - 1) {
            Line *next = &editor.lines[editor.cursor_row + 1];
            int next_len = line_length(next);
            move_gap(line, len);
            move_gap(next, next_len);
            if (line_ensure_gap(line, next_len)) {
              memcpy(&line->data[line->gap_start], next->data,
                     (size_t)next_len);
              line->gap_start += next_len;
              rebuild_line_cache(face, line);
            }
            line_free(next);
            for (int i = editor.cursor_row + 1; i < editor.line_count - 1; i++)
              editor.lines[i] = editor.lines[i + 1];
            memset(&editor.lines[editor.line_count - 1], 0, sizeof(Line));
            editor.line_count--;
          }
          editor.preferred_x = get_line_x_position(line, editor.cursor_col);
          cursor_moving = true;
          cursor_visible = true;
        }

        break;
      }

      default:
        break;
      }
    }

    if (!cursor_moving) {
      if (SDL_GetTicks() - last_blink >= CURSOR_BLINK_MS) {
        cursor_visible = !cursor_visible;
        last_blink = SDL_GetTicks();
      }
    } else {
      last_blink = SDL_GetTicks();
      cursor_visible = true;
    }

    SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
    SDL_RenderClear(renderer);

    int vis_rows = visible_rows(window);

    TextPos sel_s = sel_start(&editor);
    TextPos sel_t = sel_end(&editor);

    int y = FIRST_LINE_Y;
    for (int row = editor.scroll_row;
         row < editor.line_count && row < editor.scroll_row + vis_rows; row++) {

      int screen_y = FIRST_LINE_Y + (row - editor.scroll_row) * LINE_HEIGHT_PX;

      if (editor.selection_active) {
        bool row_selected = (row >= sel_s.row && row <= sel_t.row);
        if (row_selected) {
          Line *sl = &editor.lines[row];
          int slen = line_length(sl);
          int x0 = (row == sel_s.row) ? get_line_x_position(sl, sel_s.col)
                                      : TEXT_X_ORIGIN;
          int x1 = (row == sel_t.row) ? get_line_x_position(sl, sel_t.col)
                                      : get_line_x_position(sl, slen) + 8;
          SDL_Rect sel_rect = {x0, screen_y - 20, x1 - x0, 28};
          SDL_SetRenderDrawColor(renderer, 60, 100, 180, 120);
          SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
          SDL_RenderFillRect(renderer, &sel_rect);
          SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        }
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
        render_glyph(renderer, face, cp, &num_x, screen_y, r, g, b);
      }

      Line *l = &editor.lines[row];
      int x = TEXT_X_ORIGIN;
      int ll = line_length(l);
      for (int col = 0; col < ll;) {
        uint32_t cp = 0;
        int bytes = 0;
        if (!line_decode_codepoint(l, col, &cp, &bytes) || bytes <= 0) {
          cp = 0xFFFD;
          bytes = 1;
        }
        render_glyph(renderer, face, cp, &x, screen_y, 255, 255, 255);
        col += bytes;
      }

      y += LINE_HEIGHT_PX;
    }
    (void)y;

    if (editor.cursor_row >= editor.scroll_row &&
        editor.cursor_row < editor.scroll_row + vis_rows) {
      int cursor_screen_y =
          FIRST_LINE_Y +
          (editor.cursor_row - editor.scroll_row) * LINE_HEIGHT_PX;
      Line *cursor_line = &editor.lines[editor.cursor_row];
      int cursor_x = get_line_x_position(cursor_line, editor.cursor_col);

      if (cursor_visible) {
        SDL_Rect rect = {cursor_x, cursor_screen_y - 20, 2, 28};
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_RenderFillRect(renderer, &rect);
      }
    }

    SDL_RenderPresent(renderer);
    SDL_Delay(1);
  }

cleanup:
  for (int i = 0; i < editor.line_count; i++)
    line_free(&editor.lines[i]);
  free(editor.lines);
  free(editor.clipboard);
  SDL_StopTextInput();
  FT_Done_Face(face);
  FT_Done_FreeType(ft);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
