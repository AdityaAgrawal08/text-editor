#include "SDL_keycode.h"
#include <SDL2/SDL.h>
#include <ft2build.h>
#include <stdbool.h>
#include FT_FREETYPE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char *data;
  int length;
  int capacity;
} Line;

typedef struct {
  Line *lines;
  int line_count;
  int line_capacity;
  int cursor_row;
  int cursor_col;
  int preferred_col;
} Editor;

void line_init(Line *line) {
  line->capacity = 32;
  line->length = 0;
  line->data = malloc(line->capacity);
  line->data[0] = '\0';
}

void line_expand(Line *line) {
  line->capacity *= 2;
  line->data = realloc(line->data, line->capacity);
}

void line_insert_char(Line *line, int index, char c) {
  if (line->length + 1 >= line->capacity) {
    line_expand(line);
  }
  memmove(&line->data[index + 1], &line->data[index], line->length - index + 1);
  line->data[index] = c;
  line->length++;
}

int main() {

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    SDL_Log("SDL Init Failed: %s", SDL_GetError());
    return 1;
  }

  SDL_Window *window = SDL_CreateWindow(
      "Text Editor", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 800, 600,
      SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);

  if (!window) {
    SDL_Log("Window Creation Failed: %s", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  SDL_Renderer *renderer = SDL_CreateRenderer(
      window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer) {
    SDL_Log("Renderer Creation Failed: %s", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  FT_Library ft;
  if (FT_Init_FreeType(&ft)) {
    SDL_Log("Failed to initialize FreeType");
    return 1;
  }

  FT_Face face;
  const char *font_paths[] = {"assets/font.ttf", "../assets/font.ttf"};
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
  editor.line_capacity = 16;
  editor.line_count = 1;
  editor.lines = malloc(sizeof(Line) * editor.line_capacity);
  line_init(&editor.lines[0]);
  editor.cursor_row = 0;
  editor.cursor_col = 0;
  editor.preferred_col = 0;
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
        Line *line = &editor.lines[editor.cursor_row];
        line_insert_char(line, editor.cursor_col, event.text.text[0]);
        editor.cursor_col++;
        editor.preferred_col = editor.cursor_col;
        break;
      }

      case SDL_KEYDOWN: {
        Line *line = &editor.lines[editor.cursor_row];
        if (event.key.keysym.sym == SDLK_BACKSPACE) {
          if (editor.cursor_col > 0) {
            memmove(&line->data[editor.cursor_col - 1],
                    &line->data[editor.cursor_col],
                    line->length - editor.cursor_col + 1);
            editor.cursor_col--;
            editor.preferred_col = editor.cursor_col;
            line->length--;
          } else if (editor.cursor_row > 0) {
            int previous_row = editor.cursor_row - 1;
            Line *previous = &editor.lines[previous_row];
            int old_length = previous->length;

            while (previous->length + line->length + 1 >= previous->capacity) {
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
            line_expand(&new_line);
          }

          memcpy(new_line.data, &line->data[editor.cursor_col], split_length);
          new_line.length = split_length;
          new_line.data[split_length] = '\0';
          line->length = editor.cursor_col;
          line->data[line->length] = '\0';
          editor.lines[editor.cursor_row + 1] = new_line;
          editor.line_count++;
          editor.cursor_row++;
          editor.cursor_col = 0;
          editor.preferred_col = 0;
        }

        if (event.key.keysym.sym == SDLK_LEFT) {
          if (editor.cursor_col > 0) {
            editor.cursor_col--;
          } else if (editor.cursor_row > 0) {
            editor.cursor_row--;
            editor.cursor_col = editor.lines[editor.cursor_row].length;
          }
          editor.preferred_col = editor.cursor_col;
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
          editor.preferred_col = editor.cursor_col;
          cursor_moving = true;
          cursor_visible = true;
        }

        if (event.key.keysym.sym == SDLK_UP) {
          if (editor.cursor_row > 0) {
            editor.cursor_row--;
            Line *up_line = &editor.lines[editor.cursor_row];
            if (up_line->length >= editor.preferred_col) {
              editor.cursor_col = editor.preferred_col;
            } else {
              editor.cursor_col = up_line->length;
            }
            cursor_moving = true;
            cursor_visible = true;
          }
        }
        if (event.key.keysym.sym == SDLK_DOWN) {
          if (editor.cursor_row < editor.line_count - 1) {
            editor.cursor_row++;
            Line *down_line = &editor.lines[editor.cursor_row];
            if (down_line->length >= editor.preferred_col) {
              editor.cursor_col = editor.preferred_col;
            } else {
              editor.cursor_col = down_line->length;
            }
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
      Line *line = &editor.lines[row];
      int x = 20;
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

    int cursor_x = 20;
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

    SDL_Rect cursor = {cursor_x, cursor_y - 24, 2, 24};
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
