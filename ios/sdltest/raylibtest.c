/* raylib-only iOS render test: same cross-built libraylib.a (PLATFORM=SDL,
 * GRAPHICS_API_OPENGL_ES3) as the MISRC app. Clears RAYWHITE and draws text;
 * if this is black on the simulator while the SDL-only test renders, the
 * breakage is inside our raylib cross-build's ES3 init (not MISRC code). */
#include <raylib.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int SDL_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    fprintf(stderr, "[raylibtest] SDL_main entered\n");
    /* TIMING TEST: our full app does ~1s of setup before InitWindow and
     * never presents; raylibtest presented when InitWindow was immediate.
     * If the delay reproduces the black screen, iOS 26 only composites
     * windows created early in the launch lifecycle. */
    const char *delay = getenv("RAYLIBTEST_DELAY");
    if (delay) { int ms = atoi(delay); struct timespec ts = { ms/1000, (ms%1000)*1000000L }; nanosleep(&ts, NULL); }
    InitWindow(1200, 800, "raylibtest");
    fprintf(stderr, "[raylibtest] InitWindow done\n");
    SetTargetFPS(60);
    /* v3: emulate the app's touch canvas — render into an offscreen
     * RenderTexture every frame, then blit it full-screen. If the screen
     * goes black while the plain path worked, raylib's LoadRenderTexture /
     * FBO path is broken on iOS GLES3. */
    RenderTexture2D rt = LoadRenderTexture(GetRenderWidth(), GetRenderHeight());
    fprintf(stderr, "[raylibtest] RT id=%u tex=%u depth=%u\n", rt.id, rt.texture.id, rt.depth.id);
    int frame = 0;
    while (!WindowShouldClose()) {
        BeginTextureMode(rt);
        ClearBackground(RAYWHITE);
        DrawRectangle(100, 300, 800, 200, DARKBLUE);
        DrawText("MISRC RAYLIB RT TEST", 40, 100, 40, MAROON);
        EndTextureMode();
        BeginDrawing();
        ClearBackground(BLACK);
        DrawTexturePro(rt.texture,
            (Rectangle){ 0, 0, (float)rt.texture.width, (float)-rt.texture.height },
            (Rectangle){ 0, 0, (float)GetScreenWidth(), (float)GetScreenHeight() },
            (Vector2){ 0, 0 }, 0.0f, WHITE);
        DrawFPS(40, 40);
        EndDrawing();
        if (frame++ % 300 == 0) fprintf(stderr, "[raylibtest] frame %d\n", frame);
    }
    CloseWindow();
    return 0;
}
