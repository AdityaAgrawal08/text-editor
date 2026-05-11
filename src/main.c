#include "SDL_keycode.h" //keyboard key constants used for event handling
#include <SDL2/SDL.h> //Simple DirectMedia Layer. SDL becomes the platform abstraction layer.
#include <ft2build.h> //FreeType
#include <stdbool.h>
#include FT_FREETYPE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Line Structure
typedef struct {
  char *data;
  int length;
  int capacity;
} Line;

// Editor Structure
typedef struct {
  Line *lines;
  int line_count;
  int line_capacity;
  int cursor_row;
  int cursor_col;
  int preferred_x; // desired horizontal visual position

  Uint32 last_vertical_move; // Tracks timing of vertical navigation. After
                             // enough delay, vertical movement should establish
                             // a new preferred_x.
} Editor;

void line_init(Line *line) {
  line->data = malloc(line->capacity);
  line->data[0] = '\0';
  line->length = 0;
  line->capacity = 32;
}

bool line_expand(Line *line) {
  int new_capacity = line->capacity *= 2;
  char *new_data = realloc(line->data, line->capacity);
  if (!new_data) {
    fprintf(stderr, "Failed to expand line buffer\n");
    return false;
  }
  line->data = new_data;
  line->capacity = new_capacity;
  return true;
}

// actual insertion engine. Improvement in future: gap buffers, ropes, piece
// tables.
void line_insert_char(Line *line, int index, char c) {
  if (line->length + 1 >= line->capacity) {
    if (!line_expand(line)) {
      fprintf(stderr, "Failed to expand line buffer\n");
      return;
    }
    line_expand(line);
  }
  memmove(&line->data[index + 1], &line->data[index], line->length - index + 1);
  line->data[index] = c;
  line->length++;
}

// Important Limitation: Current editor assumes: 1 char = 1 byte, which will
// fail for UTF-8, emoji's, multibyte glyph.

// The editor internally stores cursor as: cursor_row, cursor_col, But rendering
// works in: pixel coordinates. So the editor constantly converts: text position
// to screen position This phase handles that conversion.
int get_line_x_position(FT_Face face, Line *line, int col) {
  int x = 80;
  for (int i = 0; i < col; i++) {
    char c = line->data[i];
    if (c == ' ') {
      x += 16;
      continue;
    }
    if (c == '\t') {
      x += 64;
      continue;
    }

    // Future Implementations: Use tab stops, or align tabs to columns, or
    // compute nearest tab boundary.

    if (FT_Load_Char(face, c, FT_LOAD_RENDER)) {
      continue; // Find glyph, rasterizes bitmap, populates glyph slot.
    }
    x += face->glyph->advance.x >> 6;
  }
  return x; // pixel x-coordinate of cursor at given column
}

// Convert pixel position → logical column. Used during: UP arrow and DOWN arrow
int get_closest_column(FT_Face face, Line *line, int target_x) {
  int best_col = 0;
  int best_distance = 999999;
  for (int col = 0; col <= line->length; col++) {
    int current_x = get_line_x_position(face, line, col);
    int distance = abs(current_x - target_x);
    if (distance < best_distance) {
      best_distance = distance;
      best_col = col;
    }
  }
  return best_col;
}

// remember desired horizontal cursor position
void refresh_preferred_x(Editor *editor, FT_Face face) {
  editor->preferred_x = get_line_x_position(
      face, &editor->lines[editor->cursor_row], editor->cursor_col);
}

int main() {

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    SDL_Log("SDL Init Failed: %s", SDL_GetError());
    return 1;
  }

  // Create an actual application window for this editor
  SDL_Window *window = SDL_CreateWindow(
      "Text Editor", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 800, 600,
      SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);

  if (!window) {
    SDL_Log("Window Creation Failed: %s", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  // A window is only: display surface owned by OS. A renderer is: drawing
  // engine attached to that surface. Separation is important because: multiple
  // rendering backend is required or redering lifecycle differs from window
  // lifecycle.
  SDL_Renderer *renderer = SDL_CreateRenderer(
      window, -1,
      SDL_RENDERER_ACCELERATED |      // the renderer will use the graphics
                                      // hardware if available.
          SDL_RENDERER_PRESENTVSYNC); // Synchronizes frame presentation with
                                      // monitor refresh rate.

  if (!renderer) {
    SDL_Log("Renderer Creation Failed: %s", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  FT_Library ft; // initializing FreeType.
  if (FT_Init_FreeType(&ft)) {
    SDL_Log("Failed to initialize FreeType");
    return 1;
  }

  FT_Face face;
  const char *font_paths[] = {
      "assets/font.ttf",
      "../assets/font.ttf"}; // The .ttf file provides the mathematical
                             // blueprints to draw it. FreeType reads to turn
                             // the typed character into a picture on the
                             // screen:
  int font_loaded = 0;
  for (int i = 0; i < (int)(sizeof(font_paths) / sizeof(font_paths[0])); i++) {
    if (FT_New_Face(ft, font_paths[i], 0, &face) == 0) {
      font_loaded = 1;
      break;
    }
  }

  if (!font_loaded) {
    SDL_Log("Failed to load font");
    FT_Done_FreeType(ft);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  FT_Set_Pixel_Sizes(face, 0, 24);
  SDL_StartTextInput();
  Editor editor;
  editor.last_vertical_move = 0;
  editor.line_capacity = 16;
  editor.line_count = 1;
  editor.lines = malloc(sizeof(Line) * editor.line_capacity);
  line_init(&editor.lines[0]);
  editor.cursor_row = 0;
  editor.cursor_col = 0;
  editor.preferred_x = 20;
  bool running = true;
  Uint32 last_blink = SDL_GetTicks();
  bool cursor_visible = true;
  bool cursor_moving = false;

  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      switch (event.type) {
      case SDL_QUIT:
        running = false;
        break;

      case SDL_TEXTINPUT: {
        Line *line =
            &editor.lines[editor.cursor_row]; // The editor first
                                              // determines,which line is cursor
                                              // currently on. If the cursor is
                                              // at line 2, then editor.lines[2]
                                              // becomes the active line.
                                              //
        line_insert_char(
            line, editor.cursor_col,
            event.text
                .text[0]); // SDL stores the typed text in event.text.text. The
                           // current code uses 0 -> meaning single-byte
                           // character only. This is a limitation. UTF-8
                           // characters may occupy multiple bytes.
        editor.cursor_col++;
        editor.preferred_x = get_line_x_position(
            face, &editor.lines[editor.cursor_row], editor.cursor_col);
        break;
      }

      case SDL_KEYDOWN: {
        Line *line =
            &editor.lines[editor.cursor_row]; // Same -> The editor first
                                              // determines which line is the
                                              // currently active.
        if (event.key.keysym.sym ==
            SDLK_BACKSPACE) { // Case1: Delete character inside current Line.
                              // Case2: Merge current Line with previous line.
          // Logic:
          // Case1: If the cursor is NOT at the beginning of line -> delete
          // charater.
          // Case2: Merge Line.
          if (editor.cursor_col > 0) {
            memmove(&line->data[editor.cursor_col - 1],
                    &line->data[editor.cursor_col],
                    line->length - editor.cursor_col + 1);
            editor.cursor_col--;
            editor.preferred_x = get_line_x_position(
                face, &editor.lines[editor.cursor_row], editor.cursor_col);
            line->length--;
          } else if (editor.cursor_row > 0) {
            int previous_row = editor.cursor_row - 1;
            Line *previous = &editor.lines[previous_row];
            int old_length = previous->length;

            while (previous->length + line->length + 1 >= previous->capacity) {
              if (!line_expand(previous)) {
                fprintf(stderr, "Failed to expand line buffer\n");
                return 1;
              }
              line_expand(previous);
            }

            memcpy(&previous->data[previous->length], line->data,
                   line->length + 1);
            previous->length += line->length;
            free(line->data);

            for (int i = editor.cursor_row; i < editor.line_count - 1; i++) {
              editor.lines[i] = editor.lines[i + 1];
            }
            editor.line_count--;
            editor.cursor_row--;
            editor.cursor_col = old_length;
          }
        }

        if (event.key.keysym.sym == SDLK_TAB) {
          for (int i = 0; i < 4; i++) {
            line_insert_char(line, editor.cursor_col, ' ');
            editor.cursor_col++;
          }
          editor.preferred_x = get_line_x_position(
              face, &editor.lines[editor.cursor_row], editor.cursor_col);
        }

        if (event.key.keysym.sym == SDLK_RETURN) {
          if (editor.line_count + 1 >= editor.line_capacity) {
            editor.line_capacity *= 2;
            editor.lines =
                realloc(editor.lines, sizeof(Line) * editor.line_capacity);
          }
          for (int i = editor.line_count; i > editor.cursor_row + 1; i--) {
            editor.lines[i] = editor.lines[i - 1];
          }
          Line new_line;
          line_init(&new_line);
          int split_length = line->length - editor.cursor_col;

          while (new_line.capacity <= split_length) {
            if (!line_expand(&new_line)) {
              fprintf(stderr, "Failed to expand line buffer\n");
              return 1;
            }
            line_expand(&new_line);
          }

          memcpy(new_line.data, &line->data[editor.cursor_col], split_length);
          new_line.length = split_length;
          new_line.data[split_length] = '\0';
          int old_length = line->length;
          line->length = editor.cursor_col;
          line->data[line->length] = '\0';
          memeset(&line->data[line->length + 1], 0, old_length - line->length);
          editor.lines[editor.cursor_row + 1] = new_line;
          editor.line_count++;
          editor.cursor_row++;
          editor.cursor_col = 0;
          editor.preferred_x = 20;
        }

        if (event.key.keysym.sym == SDLK_LEFT) {
          if (editor.cursor_col > 0) {
            editor.cursor_col--;
          } else if (editor.cursor_row > 0) {
            editor.cursor_row--;
            editor.cursor_col = editor.lines[editor.cursor_row].length;
          }
          editor.preferred_x = get_line_x_position(
              face, &editor.lines[editor.cursor_row], editor.cursor_col);
          cursor_moving = true;
          cursor_visible = true;
        }

        if (event.key.keysym.sym == SDLK_RIGHT) {
          if (editor.cursor_col < line->length) {
            editor.cursor_col++;
          } else if (editor.cursor_row < editor.line_count - 1) {
            editor.cursor_row++;
            editor.cursor_col = 0;
          }
          editor.preferred_x = get_line_x_position(
              face, &editor.lines[editor.cursor_row], editor.cursor_col);
          cursor_moving = true;
          cursor_visible = true;
        }

        if (event.key.keysym.sym == SDLK_UP) {
          Uint32 now = SDL_GetTicks();
          if (now - editor.last_vertical_move > 2000) {
            refresh_preferred_x(&editor, face);
          }
          editor.last_vertical_move = now;
          if (editor.cursor_row > 0) {
            editor.cursor_row--;
            Line *up_line = &editor.lines[editor.cursor_row];
            editor.cursor_col =
                get_closest_column(face, up_line, editor.preferred_x);
            cursor_moving = true;
            cursor_visible = true;
          }
        }

        if (event.key.keysym.sym == SDLK_DOWN) {
          Uint32 now = SDL_GetTicks();
          if (now - editor.last_vertical_move > 2000) {
            refresh_preferred_x(&editor, face);
          }
          editor.last_vertical_move = now;
          if (editor.cursor_row < editor.line_count - 1) {
            editor.cursor_row++;
            Line *down_line = &editor.lines[editor.cursor_row];
            editor.cursor_col =
                get_closest_column(face, down_line, editor.preferred_x);
            cursor_moving = true;
            cursor_visible = true;
          }
        }
        break;
      }

      default:
        break;
      }
    }

    if (!cursor_moving) {
      if (SDL_GetTicks() - last_blink >= 500) {
        cursor_visible = !cursor_visible;
        last_blink = SDL_GetTicks();
      }
    } else {
      last_blink = SDL_GetTicks();
      cursor_visible = true;
    }

    SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
    SDL_RenderClear(renderer);

    int y = 40;
    for (int row = 0; row < editor.line_count; row++) {
      char line_number_text[16];
      if (row == editor.cursor_row) {
        snprintf(line_number_text, sizeof(line_number_text), "%d", row + 1);
      } else {
        snprintf(line_number_text, sizeof(line_number_text), "%d",
                 abs(row - editor.cursor_row));
      }

      int number_x = 20;
      for (int i = 0; line_number_text[i] != '\0'; i++) {
        if (FT_Load_Char(face, line_number_text[i], FT_LOAD_RENDER)) {
          continue;
        }

        FT_GlyphSlot glyph = face->glyph;
        SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(
            0, glyph->bitmap.width, glyph->bitmap.rows, 32,
            SDL_PIXELFORMAT_RGBA32);

        if (!surface) {
          continue;
        }

        SDL_LockSurface(surface);
        Uint8 r = 120;
        Uint8 g = 120;
        Uint8 b = 120;
        if (row == editor.cursor_row) {
          r = 255;
          g = 200;
          b = 60;
        }
        Uint32 *pixels = (Uint32 *)surface->pixels;
        int pitch_pixels = surface->pitch / 4;
        for (unsigned int gy = 0; gy < glyph->bitmap.rows; gy++) {
          for (unsigned int gx = 0; gx < glyph->bitmap.width; gx++) {
            unsigned char alpha =
                glyph->bitmap.buffer[gy * glyph->bitmap.pitch + gx];
            pixels[gy * pitch_pixels + gx] =
                SDL_MapRGBA(surface->format, r, g, b, alpha);
          }
        }

        SDL_UnlockSurface(surface);
        SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_Rect dst = {number_x + glyph->bitmap_left, y - glyph->bitmap_top,
                        glyph->bitmap.width, glyph->bitmap.rows};

        SDL_RenderCopy(renderer, texture, NULL, &dst);
        number_x += glyph->advance.x >> 6;
        SDL_DestroyTexture(texture);
        SDL_FreeSurface(surface);
      }

      Line *line = &editor.lines[row];
      int x = 80;
      for (int col = 0; col < line->length; col++) {
        char c = line->data[col];
        if (c == ' ') {
          x += 16;
          continue;
        }
        if (FT_Load_Char(face, c, FT_LOAD_RENDER)) {
          continue;
        }

        FT_GlyphSlot glyph = face->glyph;
        SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(
            0, glyph->bitmap.width, glyph->bitmap.rows, 32,
            SDL_PIXELFORMAT_RGBA32);

        if (!surface) {
          continue;
        }

        SDL_LockSurface(surface);
        Uint32 *pixels = (Uint32 *)surface->pixels;
        int pitch_pixels = surface->pitch / 4;
        for (unsigned int gy = 0; gy < glyph->bitmap.rows; gy++) {
          for (unsigned int gx = 0; gx < glyph->bitmap.width; gx++) {
            unsigned char alpha =
                glyph->bitmap.buffer[gy * glyph->bitmap.pitch + gx];
            pixels[gy * pitch_pixels + gx] =
                SDL_MapRGBA(surface->format, 255, 255, 255, alpha);
          }
        }

        SDL_UnlockSurface(surface);
        SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);

        if (!texture) {
          SDL_FreeSurface(surface);
          continue;
        }

        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_Rect dst = {x + glyph->bitmap_left, y - glyph->bitmap_top,
                        glyph->bitmap.width, glyph->bitmap.rows};
        SDL_RenderCopy(renderer, texture, NULL, &dst);
        x += glyph->advance.x >> 6;
        SDL_DestroyTexture(texture);
        SDL_FreeSurface(surface);
      }
      y += 40;
    }

    int cursor_x = 80;
    int cursor_y = 40 + (editor.cursor_row * 40);
    Line *cursor_line = &editor.lines[editor.cursor_row];

    for (int i = 0; i < editor.cursor_col; i++) {
      char c = cursor_line->data[i];
      if (c == ' ') {
        cursor_x += 16;
        continue;
      }

      if (FT_Load_Char(face, c, FT_LOAD_RENDER)) {
        continue;
      }
      cursor_x += face->glyph->advance.x >> 6;
    }

    SDL_Rect cursor = {cursor_x, cursor_y - 20, 2, 28};
    if (cursor_visible) {
      SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
      SDL_RenderFillRect(renderer, &cursor);
    }

    SDL_RenderPresent(renderer);
    SDL_Delay(1);
  }

  for (int i = 0; i < editor.line_count; i++) {
    free(editor.lines[i].data);
  }

  free(editor.lines);
  SDL_StopTextInput();
  FT_Done_Face(face);
  FT_Done_FreeType(ft);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
