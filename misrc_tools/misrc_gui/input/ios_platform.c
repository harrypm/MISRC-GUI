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
#include <unistd.h>

#include <fcntl.h>
#include <sys/mman.h>

#include <mach/mach.h>

#ifndef MISRC_IOS
#error "ios_platform.c must only be compiled for iOS builds (-DMISRC_IOS)"
#endif

/* TEMPORARY (allocation probe, 2026-11-08): the ringbuffer's double-mirror
 * needs a shared backing object. Desktop/sim use memfd/shm_open; the device
 * sandbox may deny them. Probe every candidate ONCE at library load and
 * write the results to Documents/rb_probe.txt so the host can pick the
 * working path. Removed once the ringbuffer allocation is settled. */
__attribute__((constructor))
static void ios_rb_probe(void)
{
    const char *home = getenv("HOME");
    if (!home) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/Documents/rb_probe.txt", home);
    FILE *f = fopen(path, "a");
    if (!f) return;
    const size_t SZ = 1 << 20; /* 1 MiB */

    /* 1. plain shm_open (what the memfd shim uses on Apple) */
    {
        int fd = shm_open("/misrc-probe", O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            int tr = ftruncate(fd, SZ);
            void *m = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            fprintf(f, "shm_open: fd-ok ftruncate=%d mmap=%s\n", tr,
                    m == MAP_FAILED ? "FAIL" : "OK");
            if (m != MAP_FAILED) munmap(m, SZ);
            close(fd);
            shm_unlink("/misrc-probe");
        } else {
            fprintf(f, "shm_open: FAIL errno=%d\n", errno);
        }
    }

    /* 2. MAP_SHARED | MAP_ANONYMOUS (fd = -1) */
    {
        void *m = mmap(NULL, SZ, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        fprintf(f, "mmap MAP_SHARED|MAP_ANON: %s\n", m == MAP_FAILED ? "FAIL" : "OK");
        if (m != MAP_FAILED) munmap(m, SZ);
    }

    /* 3. mach: vm_allocate + memory entry + two fixed vm_maps (the mirror) */
    {
        task_t task = mach_task_self();
        vm_address_t storage = 0;
        kern_return_t kr = vm_allocate(task, &storage, SZ, VM_FLAGS_ANYWHERE);
        if (kr != KERN_SUCCESS) {
            fprintf(f, "mach-entry: vm_allocate FAIL kr=%d\n", kr);
        } else {
            memory_object_size_t esize = SZ;
            mach_port_t entry = MACH_PORT_NULL;
            kr = mach_make_memory_entry_64(task, &esize,
                                           (memory_object_offset_t)storage,
                                           VM_PROT_READ | VM_PROT_WRITE,
                                           &entry, MACH_PORT_NULL);
            if (kr != KERN_SUCCESS) {
                fprintf(f, "mach-entry: make_entry FAIL kr=%d\n", kr);
                vm_deallocate(task, storage, SZ);
            } else {
                void *reserve = mmap(NULL, 2 * SZ, PROT_NONE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (reserve == MAP_FAILED) {
                    fprintf(f, "mach-entry: reserve mmap FAIL\n");
                } else {
                    vm_address_t m1 = (vm_address_t)reserve;
                    kr = vm_map(task, &m1, SZ, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                                entry, 0, FALSE, VM_PROT_READ | VM_PROT_WRITE,
                                VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_DEFAULT);
                    if (kr != KERN_SUCCESS) {
                        fprintf(f, "mach-entry: vm_map#1 FAIL kr=%d\n", kr);
                    } else {
                        vm_address_t m2 = (vm_address_t)reserve + SZ;
                        kr = vm_map(task, &m2, SZ, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                                    entry, 0, FALSE, VM_PROT_READ | VM_PROT_WRITE,
                                    VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_DEFAULT);
                        /* write through mirror1, read via mirror2 (the mirror test) */
                        ((volatile int *)reserve)[0] = 0x4D495343;
                        int back = (m2 != (vm_address_t)reserve)
                            ? ((volatile int *)m2)[0] : -1;
                        fprintf(f, "mach-entry: map1=%s map2=%s mirror-read=%s\n",
                                kr == KERN_SUCCESS ? "OK" : "FAIL",
                                kr == KERN_SUCCESS ? "OK" : "FAIL",
                                back == 0x4D495343 ? "SAME-PAGES" : "DIFFERENT");
                    }
                    munmap(reserve, 2 * SZ);
                }
                mach_port_deallocate(task, entry);
                vm_deallocate(task, storage, SZ);
            }
        }
    }
    fclose(f);
}

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
    /* TEMPORARY debug (touch-eater investigation): SDL_StartTextInput on
     * iOS creates a hidden UITextField and makes it FIRST RESPONDER,
     * which can swallow all touches. Log every call. */
    {
        const char *home = getenv("HOME");
        if (home) {
            char path[1024];
            snprintf(path, sizeof(path), "%s/misrc_input.log", home);
            FILE *f = fopen(path, "a");
            if (f) {
                fprintf(f, "KBD visible=%d (SDL_%s)\n",
                        visible, visible ? "StartTextInput" : "StopTextInput");
                fclose(f);
            }
        }
    }
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

/* TEMPORARY (net-mode trace): append a line to the Documents input log
 * from the UI layer. Removed with the rest of the debug logging. */
void ios_view_input_log_net(const char *what, float x, float y)
{
    const char *docs = ios_get_storage_path();
    if (!docs || !*docs) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/misrc_input.log", docs);
    FILE *f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "%s at=(%.1f,%.1f)\n", what, x, y);
    fclose(f);
}

/* CPU core detection for the info panel / FLAC thread defaults (the CLI
 * lib's get_num_cores is not linked on iOS; sysconf does the same job). */
uint32_t get_num_cores(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (uint32_t)n : 0;
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
