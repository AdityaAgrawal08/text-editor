#include <SDL2/SDL.h>
#include <ft2build.h>
#include <stdbool.h>
#include FT_FREETYPE_H
#include <stdlib.h>
#include <string.h>

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
  // -1 is the Driver Index. It automatically choose best rendering driver
  // use GPU acceleration if possible

  if (!renderer) {
    SDL_Log("Renderer Creation Failed: %s", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  // Equivalent conceptually to: SDL_Init(...) but for fonts.
  FT_Library ft;
  if (FT_Init_FreeType(&ft)) {
    SDL_Log("Failed to initialize FreeType");
    return 1;
  }

  // Loads actual .ttf font file."assets/font.ttf is the actual path to font
  // size and &face -> FreeType writes loaded font object here.
  FT_Face face;
  if (FT_New_Face(ft, "assets/font.ttf", 0, &face)) {
    SDL_Log("Failed to load font");
    return 1;
  }

  // Sets glyph render size.
  FT_Set_Pixel_Sizes(face, 0, 24);

  SDL_StartTextInput();
  bool running = true;
  bool dirty = true;
  char text[1024] = {0};
  int text_length = 0;
  int cursor_position = 0;

  while (running) {
    SDL_Event event;

    while (SDL_PollEvent(&event)) {
      switch (event.type) {
      case SDL_QUIT:
        running = false;
        break;
      case SDL_TEXTINPUT:
        if (text_length < (int)sizeof(text) - 1) {
          text[cursor_position] = event.text.text[0];
          cursor_position++;
          text_length++;
          text[text_length] = '\0';
          dirty = true;
        }
        break;

      case SDL_KEYDOWN:
        if (event.key.keysym.sym == SDLK_BACKSPACE && cursor_position > 0) {
          cursor_position--;
          text_length--;
          text[cursor_position] = '\0';
          dirty = true;
        }
        break;

      case SDL_WINDOWEVENT:
        dirty = true;
        break;
      }
    }

    if (dirty) {
      SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
      SDL_RenderClear(renderer);

      int x = 20;
      int y = 40;
      for (int i = 0; i < text_length; i++) {
        // FT_Load_Char(...) This is where FreeType converts: character into
        // bitmap glyph
        if (FT_Load_Char(face, text[i], FT_LOAD_RENDER)) {
          continue;
        }

        FT_GlyphSlot glyph = face->glyph;
        int width = glyph->bitmap.width;
        int height = glyph->bitmap.rows;

        if (width == 0 || height == 0) {
          x += glyph->advance.x >> 6;
          continue;
        }

        Uint32 *pixels = malloc(width * height * sizeof(Uint32));

        if (!pixels) {
          continue;
        }

        for (int row = 0; row < height; row++) {
          for (int col = 0; col < width; col++) {
            unsigned char alpha =
                glyph->bitmap.buffer[row * glyph->bitmap.pitch + col];
            pixels[row * width + col] =
                (alpha << 24) | (255 << 16) | (255 << 8) | 255;
          }
        }

        SDL_Texture *texture =
            SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                              SDL_TEXTUREACCESS_STATIC, width, height);

        if (!texture) {
          free(pixels);
          continue;
        }

        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_UpdateTexture(texture, NULL, pixels, width * sizeof(Uint32));
        free(pixels);

        SDL_Rect dst = {x + glyph->bitmap_left, y - glyph->bitmap_top, width,
                        height};

        SDL_RenderCopy(renderer, texture, NULL, &dst);
        x += glyph->advance.x >> 6;
        SDL_DestroyTexture(texture);
      }

      SDL_Rect cursor = {x, 16, 2, 24};
      // font size is: FT_Set_Pixel_Sizes(face, 0, 24).Cursor height should
      // visually match glyph height.

      SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
      SDL_RenderFillRect(renderer, &cursor);
      SDL_RenderPresent(renderer);
      dirty = false;
    }
    SDL_Delay(1);
  }

  SDL_StopTextInput();
  FT_Done_Face(face);   // Prevents memory/resource leaks during shutdown.
  FT_Done_FreeType(ft); // Prevents memory/resource leaks during shutdown.
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();

  return 0;
}
