/* raylib-only iOS render test: same cross-built libraylib.a (PLATFORM=SDL,
 * GRAPHICS_API_OPENGL_ES3) as the MISRC app. Clears RAYWHITE and draws text;
 * if this is black on the simulator while the SDL-only test renders, the
 * breakage is inside our raylib cross-build's ES3 init (not MISRC code). */
#include <raylib.h>
#include <OpenGLES/ES3/gl.h>
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
    /* Probe FBO completeness + GL errors for the raylib-created RT. */
    {
        const char *home = getenv("HOME");
        char path[1024];
        snprintf(path, sizeof(path), "%s/rt_probe.txt", home ? home : "/tmp");
        FILE *f = fopen(path, "a");
        if (f) {
            glBindFramebuffer(GL_FRAMEBUFFER, rt.id);
            GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            GLenum e1 = glGetError();
            /* color-only manual FBO for comparison */
            GLuint mFbo = 0, mTex = 0;
            glGenFramebuffers(1, &mFbo);
            glGenTextures(1, &mTex);
            glBindTexture(GL_TEXTURE_2D, mTex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glBindFramebuffer(GL_FRAMEBUFFER, mFbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mTex, 0);
            GLenum st2 = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            GLenum e2 = glGetError();
            fprintf(f, "raylibRT: id=%u status=0x%x err=0x%x | manualColorOnly: status=0x%x err=0x%x\n",
                    rt.id, st, e1, st2, e2);
            fclose(f);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &mFbo);
            glDeleteTextures(1, &mTex);
        }
    }
    int frame = 0;
    while (!WindowShouldClose()) {
        BeginTextureMode(rt);
        ClearBackground(RAYWHITE);
        DrawRectangle(100, 300, 800, 200, DARKBLUE);
        DrawText("MISRC RAYLIB RT TEST", 40, 100, 40, MAROON);
        EndTextureMode();
        if (frame == 3) {
            /* Read the RT back directly: content here means the RT render
             * works and the BLIT is broken; black here means draws into the
             * RT fail. */
            const char *home = getenv("HOME");
            char path[1024];
            snprintf(path, sizeof(path), "%s/rt_probe.txt", home ? home : "/tmp");
            FILE *f = fopen(path, "a");
            if (f) {
                glBindFramebuffer(GL_FRAMEBUFFER, rt.id);
                unsigned char p1[4] = {0}, p2[4] = {0};
                glReadPixels(rt.texture.width/2, rt.texture.height/2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p1);
                glReadPixels(400, 400, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p2);
                GLenum e = glGetError();
                fprintf(f, "frame3 RT readback: center=(%d,%d,%d) 400,400=(%d,%d,%d) err=0x%x tex=%ux%u\n",
                        p1[0], p1[1], p1[2], p2[0], p2[1], p2[2], e, rt.texture.width, rt.texture.height);
                fclose(f);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
            }
        }
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
