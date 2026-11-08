/* Minimal SDL2 iOS render test v2 — GL attribute bisect.
 * Mode comes from argv[1]; each mode clears a distinct color so a screenshot
 * identifies BOTH the mode and whether the framebuffer presented:
 *   0: no GL attrs, FULLSCREEN_DESKTOP (baseline, known RED)
 *   1: + SDL_GL_CONTEXT_PROFILE_ES, major 3, minor 0
 *   2: mode 1 + DEPTH_SIZE 24, STENCIL_SIZE 8, DOUBLEBUFFER 1
 *   3: mode 2 + MULTISAMPLEBUFFERS 1, MULTISAMPLESAMPLES 4 (raylib MSAA)
 *   4: mode 2, windowed 1425x720 (no FULLSCREEN_DESKTOP), like raylib
 *   5: mode 3 + windowed (raylib's exact combination)
 */
#include <SDL.h>
#include <OpenGLES/ES3/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>

typedef struct { float r, g, b; } RGB;
static const RGB kColors[6] = {
    {1,0,0}, {0,1,0}, {0,0,1}, {1,1,0}, {1,0,1}, {0,1,1}
};

int SDL_main(int argc, char **argv)
{
    /* NOTE: simctl launch [args] do NOT reach iOS app argv; use the
     * SIMCTL_CHILD_SDLTEST_MODE env var (SIMCTL_CHILD_* passthrough). */
    (void)argc; (void)argv;
    const char *ms = getenv("SDLTEST_MODE");
    int mode = ms ? atoi(ms) : 0;
    if (mode < 0 || mode > 5) mode = 0;
    RGB c = kColors[mode];
    fprintf(stderr, "[sdltest] mode %d\n", mode);

    if (mode >= 1) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    }
    if (mode >= 2) {
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    }
    if (mode == 3 || mode == 5) {
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "[sdltest] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    Uint32 wf = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI;
    Uint32 extra = (mode == 4 || mode == 5)
        ? 0
        : SDL_WINDOW_FULLSCREEN_DESKTOP;
    int wx = (mode == 4 || mode == 5) ? SDL_WINDOWPOS_CENTERED : SDL_WINDOWPOS_UNDEFINED;
    int wy = (mode == 4 || mode == 5) ? SDL_WINDOWPOS_CENTERED : SDL_WINDOWPOS_UNDEFINED;
    int ww = (mode == 4 || mode == 5) ? 1425 : 640;
    int wh = (mode == 4 || mode == 5) ? 720 : 480;
    SDL_Window *w = SDL_CreateWindow("sdltest", wx, wy, ww, wh, wf | extra);
    if (!w) {
        fprintf(stderr, "[sdltest] no window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GLContext gl = SDL_GL_CreateContext(w);
    if (!gl) {
        fprintf(stderr, "[sdltest] ctx fail (mode %d): %s\n", mode, SDL_GetError());
        return 1;
    }
    fprintf(stderr, "[sdltest] ctx ok mode %d; swap interval rc %d\n",
            mode, SDL_GL_SetSwapInterval(1));
    int frame = 0;
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) { if (e.type == SDL_QUIT) return 0; }
        glClearColor(c.r, c.g, c.b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        if ((frame % 120) == 0) {
            GLint fb = 0;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
            unsigned char px[4] = {0};
            glReadPixels(100, 100, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            const char *home = getenv("HOME");
            if (home) {
                char path[1024];
                snprintf(path, sizeof(path), "%s/sdltest_fbo.txt", home);
                FILE *f = fopen(path, "a");
                if (f) {
                    fprintf(f, "mode %d frame %d: fbo=%d read100=(%d,%d,%d)\n",
                            mode, frame, fb, px[0], px[1], px[2]);
                    fclose(f);
                }
            }
        }
        frame++;
        SDL_GL_SwapWindow(w);
        SDL_Delay(16);
    }
}
