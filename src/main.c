#include <SDL2/SDL.h>
#include <stdbool.h>

int main() {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    SDL_Log("SDL Init Failed: %s", SDL_GetError());
    return 1;
  }

  SDL_Window *window = SDL_CreateWindow(
      "Text Editor", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 800, 600,
      SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_FULLSCREEN);

  if (!window) {
    SDL_Log("Window Creation Failed: %s", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  SDL_Renderer *renderer =
      SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
  // -1 is the Driver Index. It automatically choose best rendering driver
  // use GPU acceleration if possible

  bool running = true;

  while (running) {
    bool running = true;
    bool dirty = true;

    while (running) {

      SDL_Event event;

      while (SDL_PollEvent(&event)) {

        switch (event.type) {

        case SDL_QUIT:
          running = false;
          break;

        case SDL_KEYDOWN:
          dirty = true;
          break;

        case SDL_MOUSEMOTION:
          dirty = true;
          break;

        case SDL_WINDOWEVENT:
          dirty = true;
          break;
        }
      }

      if (dirty) {

        SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
        SDL_RenderClear(renderer);

        SDL_RenderPresent(renderer);

        dirty = false;
      }

      SDL_Delay(1);
    }
  }

  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);

  SDL_Quit();

  return 0;
}
