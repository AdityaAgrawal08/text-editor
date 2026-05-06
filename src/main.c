#include <SDL2/SDL.h>
#include <ft2build.h>
#include <stdbool.h>
#include FT_FREETYPE_H
#include <stdlib.h>
#include <stdio.h>
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
  const char *font_paths[] = {"assets/font.ttf", "../assets/font.ttf"};
  int font_loaded = 0;

  for (int i = 0; i < (int)(sizeof(font_paths) / sizeof(font_paths[0])); i++) {
    if (FT_New_Face(ft, font_paths[i], 0, &face) == 0) {
      font_loaded = 1;
      break;
    }
  }

  if (!font_loaded) {
    SDL_Log("Failed to load font from assets/font.ttf or ../assets/font.ttf");
    FT_Done_FreeType(ft);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  // Sets glyph render size.
  FT_Set_Pixel_Sizes(face, 0, 24);

  SDL_StartTextInput();
  bool running = true;
  char text[1024] = "Hello";
  int text_length = 5;
  int cursor_position = 5;

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
        }
        break;

      case SDL_KEYDOWN:
        if (event.key.keysym.sym == SDLK_BACKSPACE && cursor_position > 0) {
          cursor_position--;
          text_length--;
          text[cursor_position] = '\0';
        }
        break;

      case SDL_WINDOWEVENT:
        break;
      }
    }

    {
      SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
      SDL_RenderClear(renderer);

      int x = 20;
      int y = 40;
      for (int i = 0; i < text_length; i++) {
        // FT_Load_Char(...) This is where FreeType converts: character into
        // bitmap glyph
        if (FT_Load_Char(face, text[i], FT_LOAD_RENDER)) {
          SDL_Log("Failed glyph load: %c", text[i]);
          continue;
        }

        FT_GlyphSlot glyph = face->glyph;
        if (glyph->bitmap.width == 0 || glyph->bitmap.rows == 0) {
          continue;
        }

        SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(
            0, glyph->bitmap.width, glyph->bitmap.rows, 32,
            SDL_PIXELFORMAT_RGBA32);

        if (!surface) {
          continue;
        }

        SDL_LockSurface(surface);
        Uint32 *pixels = (Uint32 *)surface->pixels;
        int pitch_pixels = surface->pitch / 4;
        for (unsigned int row = 0; row < glyph->bitmap.rows; row++) {
          for (unsigned int col = 0; col < glyph->bitmap.width; col++) {
            unsigned char alpha =
                glyph->bitmap.buffer[row * glyph->bitmap.pitch + col];

            pixels[row * pitch_pixels + col] =
                SDL_MapRGBA(surface->format, 255, 255, 255, alpha);
          }
        }

        SDL_UnlockSurface(surface);

        SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
        if (!texture) {
          SDL_Log("Texture Creation Failed: %s", SDL_GetError());

          SDL_FreeSurface(surface);

          continue;
        }
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);

        if (!texture) {
          SDL_Log("Texture Creation Failed: %s", SDL_GetError());

          SDL_FreeSurface(surface);

          continue;
        }

        SDL_Rect dst = {x + glyph->bitmap_left, y - glyph->bitmap_top,
                        glyph->bitmap.width, glyph->bitmap.rows};

        if (SDL_RenderCopy(renderer, texture, NULL, &dst) != 0) {

          SDL_Log("RenderCopy Failed: %s", SDL_GetError());
        }

        x += glyph->advance.x >> 6;

        SDL_DestroyTexture(texture);
        SDL_FreeSurface(surface);
      }

      SDL_Rect cursor = {x, 16, 2, 24};
      // font size is: FT_Set_Pixel_Sizes(face, 0, 24).Cursor height should
      // visually match glyph height.

      SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
      SDL_RenderFillRect(renderer, &cursor);
      SDL_RenderPresent(renderer);
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
