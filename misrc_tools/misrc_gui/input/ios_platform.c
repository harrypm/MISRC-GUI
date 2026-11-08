/*
 * iOS platform bridge — storage path + soft keyboard
 * Copyright (C) 2026 — MISRC GUI ios-support
 *
 * Mirrors the surface of input/android_usb_jni.c that the GUI actually
 * consumes on a no-USB platform:
 *   - ios_get_storage_path(): app Documents directory (sandbox HOME +
 *     /Documents; created on demand). Info.plist ships
 *     UIFileSharingEnabled + LSSupportsOpeningDocumentsInPlace, so this
 *     directory is user-visible in the Finder/Files app for moving
 *     capture files and playback FLACs in and out.
 *   - ios_set_keyboard_visible(): SDL2 on-screen keyboard control. Text
 *     itself flows through the normal raylib path — raylib's SDL backend
 *     maps SDL_TEXTINPUT into the GetCharPressed() queue that the Clay
 *     text fields already poll, so no character injection bridge is
 *     needed (unlike Android's nativePushTextInput JNI path).
 *
 * USB capture does not exist on iOS (no libusb backend, no public USB
 * host API); the hsdaoh stub library reports zero devices, so there is
 * no USB bridge surface here at all.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <SDL.h>
#include <OpenGLES/ES3/gl.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef MISRC_IOS
#error "ios_platform.c must only be compiled for iOS builds (-DMISRC_IOS)"
#endif

static char s_documents_path[1024] = "";

const char *ios_get_storage_path(void)
{
    if (s_documents_path[0]) return s_documents_path;

    const char *home = getenv("HOME");
    if (!home || !*home) {
        /* Should not happen inside an app sandbox; fail loudly to stderr
         * and fall back to the CWD rather than a NULL deref downstream. */
        fprintf(stderr, "[ios] HOME unset; falling back to \".\"\n");
        return ".";
    }
    snprintf(s_documents_path, sizeof(s_documents_path), "%s/Documents", home);

    /* Create on demand (first launch of a fresh sandbox). */
    struct stat st;
    if (stat(s_documents_path, &st) != 0) {
        if (mkdir(s_documents_path, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "[ios] mkdir(%s) failed; using HOME\n", s_documents_path);
            snprintf(s_documents_path, sizeof(s_documents_path), "%s", home);
        }
    }
    return s_documents_path;
}

void ios_set_keyboard_visible(int visible)
{
    if (visible) {
        SDL_StartTextInput();
    } else {
        SDL_StopTextInput();
    }
}

int ios_open_url(const char *url)
{
    if (!url || !*url) return 0;
    /* SDL2 hands off to the system browser on iOS (no shell to spawn). */
    return SDL_OpenURL(url) == 0;
}

/* TEMPORARY DEBUG (2026-11-08 black-screen investigation): dump the GL state
 * after a frame to <app-home>/misrc_gldump.txt so the host can read it from
 * the simulator container. Remove once the render bug is fixed. */
void ios_gl_debug_dump(int frame)
{
    const char *home = getenv("HOME");
    if (!home) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/misrc_gldump.txt", home);
    FILE *f = fopen(path, "a");
    if (!f) return;
    GLint vp[4] = {0}, sc[4] = {0}, fb = 0, cm[4] = {0};
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_SCISSOR_BOX, sc);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    glGetIntegerv(GL_COLOR_WRITEMASK, cm);
    /* Read back actual framebuffer content (post-draw, pre/post swap):
     * if these show the background color while the SCREEN is black, the
     * UIKit window/layer compositing is broken, not the GL drawing. */
    unsigned char px[4][4] = {{0}};
    GLint rx[4] = { vp[0] + vp[2]/2, vp[0] + 4, vp[0] + vp[2] - 5, vp[0] + vp[2]/2 };
    GLint ry[4] = { vp[1] + vp[3]/2, vp[1] + vp[3] - 5, vp[1] + vp[3] - 5, vp[1] + 4 };
    for (int i = 0; i < 4; i++) {
        glReadPixels(rx[i], ry[i], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px[i]);
    }
    fprintf(f, "frame %d: viewport=[%d %d %d %d] scissor_box=[%d %d %d %d] "
               "scissor_test=%d fbo=%d colormask=[%d %d %d %d] "
               "fbread: c=(%d,%d,%d) bl=(%d,%d,%d) br=(%d,%d,%d) tl=(%d,%d,%d)\n",
            frame, vp[0], vp[1], vp[2], vp[3], sc[0], sc[1], sc[2], sc[3],
            (int)glIsEnabled(GL_SCISSOR_TEST), fb, cm[0], cm[1], cm[2], cm[3],
            px[0][0], px[0][1], px[0][2], px[1][0], px[1][1], px[1][2],
            px[2][0], px[2][1], px[2][2], px[3][0], px[3][1], px[3][2]);
    fclose(f);
}
