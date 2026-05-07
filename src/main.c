#include "SDL_keycode.h"
#include <SDL2/SDL.h> //Simple DirectMedia Layer
#include <ft2build.h> //FreeType
#include <stdbool.h>
#include FT_FREETYPE_H
#include <stdio.h>
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
  char text[1024] = {};
  int text_length = 0;
  int cursor_position = 0;

  Uint32 last_blink = SDL_GetTicks();
  bool cursor_visible = true;
  while (running) {
    SDL_Event event;

    while (SDL_PollEvent(&event)) { // SDL internally already maintains queue.
                                    // Poll Event Fetch next event from queue
      switch (event.type) {
      case SDL_QUIT:
        running = false;
        break;
      case SDL_TEXTINPUT: // Supports insertion in middle which shifts text
                          // safely.
        if (text_length < (int)sizeof(text) - 1) {
          memmove(&text[cursor_position + 1], &text[cursor_position],
                  text_length - cursor_position + 1);
          text[cursor_position] = event.text.text[0];
          cursor_position++;
          text_length++;
        }
        break;
      case SDL_KEYDOWN:
        if (event.key.keysym.sym == SDLK_BACKSPACE && cursor_position > 0) {
          memmove(&text[cursor_position - 1], &text[cursor_position],
                  text_length - cursor_position + 1);
          cursor_position--;
          text_length--;
        }

        if (event.key.keysym.sym == SDLK_TAB) {
          if (text_length < (int)sizeof(text) - 1) {
            memmove(&text[cursor_position + 1], &text[cursor_position],
                    text_length - cursor_position + 1);
            text[cursor_position] = '\t';
            cursor_position++;
            text_length++;
          }
        }

        if (event.key.keysym.sym == SDLK_LEFT && cursor_position > 0) {
          cursor_position--;
        }

        if (event.key.keysym.sym == SDLK_RIGHT &&
            cursor_position < text_length) {
          cursor_position++;
        }

        if (event.key.keysym.sym == SDLK_RETURN) {
          if (text_length < (int)sizeof(text) - 1) {
            memmove(&text[cursor_position + 1], &text[cursor_position],
                    text_length - cursor_position + 1);

            text[cursor_position] = '\n';
            cursor_position++;
            text_length++;
          }
        }

        break;

      case SDL_WINDOWEVENT:
        break;
      }
    }

    // Add cursor blinking.
    if (SDL_GetTicks() - last_blink >= 500) {
      cursor_visible = !cursor_visible;
      last_blink = SDL_GetTicks();
    }

    {
      // These are background rendering operations.
      SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
      SDL_RenderClear(
          renderer); // Without clearing: deleted characters stay visible

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
        if (glyph->bitmap.width == 0 ||
            glyph->bitmap.rows == 0) { // For Spaces and Tabs

          // Spacebar and Tab logic
          if (text[i] == '\t') {
            x += (glyph->advance.x >> 6) * 4; // Add 4 for Tabs
          } else {
            x += glyph->advance.x >> 6; // For SpaceBar
          }
          continue;

          if (text[i] == '\n') {
            y += 40;
            x = 20;
            continue;
          }
        }

        // Surface is NOT text-editor surface. It is temporary image buffer in
        // RAM. For each glyph => Character 'A' -> Bitmap -> Surface created for
        // only 'A' Next character gets another surface.
        SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(
            0, glyph->bitmap.width, glyph->bitmap.rows, 32,
            SDL_PIXELFORMAT_RGBA32);

        if (!surface) {
          continue;
        }

        // Now we do surface locking because SDL asks you to lock the surface
        // before directly accessing pixel memory. Some surfaces may internally
        // store pixels differently or use hardware-managed memory  and use
        // temporary synchronization mechanisms. Meaning -> "SDL, prepare this
        // surface so I can safely read/write raw pixels." After locking:
        // surface->pixels becomes safe to access directly.
        SDL_LockSurface(surface);

        Uint32 *pixels =
            (Uint32 *)surface
                ->pixels; // surface->pixel gives general void* we convert it
                          // into Uint32 because each pixel is 32 bit.
        int pitch_pixels =
            surface->pitch /
            4; // pitch means actual bytes occupied by one row. Division by 4
               // because pitch is in bytes but pixel is 32 bit or 4 bytes.

        // Now put FreeStyle bitmap pixels into SDL surface pixel
        for (unsigned int row = 0; row < glyph->bitmap.rows; row++) {
          for (unsigned int col = 0; col < glyph->bitmap.width; col++) {
            unsigned char alpha =
                glyph->bitmap.buffer[row * glyph->bitmap.pitch + col];

            pixels[row * pitch_pixels + col] =
                SDL_MapRGBA(surface->format, 255, 255, 255, alpha);
          }
        }
        SDL_UnlockSurface(surface);

        // Surface is CPU memory. GPU can't efficiently render surface. So SDL
        // convert Suface(RAM) to Texture (GPU memory). Textures are optimized
        // for rendering speed.
        SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
        if (!texture) {
          SDL_Log("Texture Creation Failed: %s", SDL_GetError());
          SDL_FreeSurface(surface);
          continue;
        }
        SDL_SetTextureBlendMode(
            texture, SDL_BLENDMODE_BLEND); // Enables transparency. Use alpha
                                           // blending while drawing texture

        if (!texture) {
          SDL_Log("Texture Creation Failed: %s", SDL_GetError());
          SDL_FreeSurface(surface);
          continue;
        }

        SDL_Rect dst = {
            x + glyph->bitmap_left, y - glyph->bitmap_top, glyph->bitmap.width,
            glyph->bitmap.rows}; // Horizontal, Vertical, Width, Height

        if (SDL_RenderCopy(renderer, texture, NULL, &dst) != 0) {
          // Copies texture onto renderer.
          SDL_Log("RenderCopy Failed: %s", SDL_GetError());
        }

        x += glyph->advance.x >>
             6; // Moves cursor for next character. FreeType store values in
                // 1/64 pixel precision. So divide by 64 to get real pixel value

        SDL_DestroyTexture(texture); // Avoid GPU memory leak
        SDL_FreeSurface(surface);    // CPU side image buffer. Avoid RAM leak
      }

      int cursor_x = 20;
      for (int i = 0; i < cursor_position; i++) {
        if (FT_Load_Char(face, text[i], FT_LOAD_RENDER)) {
          continue;
        }
        if (text[i] == '\t') {
          cursor_x += (face->glyph->advance.x >> 6) * 4;
        } else {
          cursor_x += face->glyph->advance.x >> 6;
        }
      }
      SDL_Rect cursor = {cursor_x, 16, 2, 24};
      // font size is: FT_Set_Pixel_Sizes(face, 0, 24).Cursor height should
      // visually match glyph height.

      if (cursor_visible) {
        SDL_SetRenderDrawColor(renderer, 255, 255, 255,
                               255); // Sets drawing color to white.
        SDL_RenderFillRect(renderer, &cursor);
        // Draws solid rectangle. That will be the cursor.
        // Everything until now was drawn in hidden backbuffer. Now show
        // completed frame on actual screen
      }
      SDL_RenderPresent(renderer);
    }
    SDL_Delay(1); // Prevents infinite loop from consuming 100% CPU.
  }

  SDL_StopTextInput();
  FT_Done_Face(face);   // Prevents memory/resource leaks during shutdown.
  FT_Done_FreeType(ft); // Prevents memory/resource leaks during shutdown.
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();

  return 0;
}
