#include "../input/gui_cxadc_macos.h"
/*
 * MISRC GUI - Graphical Capture Interface
 *
 * Real-time waveform display and capture control using raylib + Clay UI
 *
 * Copyright (C) 2023-2026 Harry Munday, AlessandroAU, Stefan O, Vrunk11, machcnz
 * Licensed under GNU GPL v3 or later
 */

// Window title the native window is created with. On X11 this becomes the
// WM_CLASS class, which the desktop matches against StartupWMClass in the
// launcher, so it must stay version-independent. See the InitWindow() call.
#define MISRC_WINDOW_CLASS_TITLE "MISRC Capture"

// Clay UI library (header-only, implementation here)
#define CLAY_IMPLEMENTATION
#include "../ui/clay.h"

#include "raylib.h"
#include "rlgl.h"              // rlDrawRenderBatchActive (the modal frame flush)
#include "gui_win32_modal.h"   // Windows modal move/size render driver (no-op elsewhere)
#include "../assets/inter_font_data.h"
#include "../assets/misrc_icon_png_data.h"
#include "../assets/space_mono_font_data.h"

#include "gui_app.h"
#include "gui_updater.h"
#if !defined(__ANDROID__)
#include "../../misrc_capture/misrc_capture_cli.h"
#endif
#include "../ui/gui_ui.h"
#if defined(__ANDROID__)
#define GUI_ANDROID_VIEWPORT_IMPLEMENTATION
#include "gui_android_viewport.h"
#endif
#include "../input/gui_playback.h"
#include "../visualization/gui_text.h"
#include "../input/gui_capture.h"
#include "../processing/gui_extract.h"
#include "../signal/gui_cvbs.h"
#include "../visualization/gui_panel.h"
#include "../ui/gui_dropdown.h"
#include "../ui/gui_popup.h"
#include "../output/gui_record.h"
#include "../output/gui_audio.h"
#include "../net/gui_net.h"
#include "../../common/threading.h"
#include "../../common/utf8_argv.h"
#include "version.h"
#include "gui_font.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#if defined(__linux__) && !defined(__ANDROID__)
#include <signal.h>
#endif
#if defined(__APPLE__)
#include <unistd.h>
#include <limits.h>
#include <spawn.h>
#include <sys/wait.h>
#include <mach-o/dyld.h>
extern char **environ;
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOGDI
#define NOGDI
#endif
#ifndef NOUSER
#define NOUSER
#endif
#include <windows.h>
#endif

#ifndef MIRSC_TOOLS_VERSION
#define MIRSC_TOOLS_VERSION "dev"
#endif

// Global exit flag (shared with capture thread)
volatile atomic_int do_exit = 0;

// Font array for Clay
// Index 0: Inter (general UI), Index 1: Space Mono (monospace sections)
#define FONT_COUNT 2
#define UI_FONT_ATLAS_SIZE 64
static Font fonts[FONT_COUNT];

// Clay error handler
void clay_error_handler(Clay_ErrorData error) {
    fprintf(stderr, "Clay Error: %s\n", error.errorText.chars);
}
static void print_usage(const char *program_name) {
    fprintf(stdout,
            "MISRC GUI %s\n"
            "Usage:\n"
            "  %s [--help] [--version] [--smoke-test] [--debug-view] [--config <path>] [--auto-connect]\n"
            "     [--select-device <index-or-name>] [--auto-capture] [--auto-record <seconds>]\n"
            "     [--check-update] [--update-dry-run] [--install-update [--yes]]\n"
            "     [--update-manifest <url-or-path>]\n"
            "\n"
            "Headless update mode (the same pipeline as the in-app updater;\n"
            "prints key: value lines for humans and automated tests):\n"
            "--check-update  fetch the update manifest + latest release tag, print\n"
            "                them and exit 0 (prints update_available true/false).\n"
            "--update-dry-run  also resolve this platform's asset URL, the running\n"
            "                executable, the updates dir and the archive path; no\n"
            "                download, nothing is replaced.\n"
            "--install-update  perform the confirmed self-update: download, archive\n"
            "                the old binary by rename (never deleted), install the\n"
            "                new one in place. Requires --yes (the CLI confirmation).\n"
            "--update-manifest <url-or-path>  use this manifest source instead of the\n"
            "                compiled-in raw URL: an https URL, http://127.0.0.1 (loopback\n"
            "                only, for automated tests), or a local file path.\n"
            "--yes           explicit confirmation for --install-update.\n"
            "\n"
            "No arguments launch the GUI.\n"
            "--debug-view enables verbose runtime logs.\n"
            "--config <path> loads settings from <path> instead of the default\n"
            "           location (useful for automated GUI testing).\n"
            "--select-device <sel> pick the capture device at launch: a device-list\n"
            "           index, or a case-insensitive name substring (first match,\n"
            "           e.g. \"CXADC\"). Exits 1 if nothing matches. Needed for\n"
            "           automated runs because startup otherwise prefers\n"
            "           hsdaoh > MISRC Clockgen > CXADC.\n"
            "--auto-capture starts capture unattended ~1 s after launch (Local mode;\n"
            "           no --config required).\n"
            "--auto-record <seconds> runs a full unattended record cycle: start\n"
            "           capture, settle 2 s, record <seconds> seconds, wait for the\n"
            "           output file finalize, stop capture, exit 0 (exit 1 on\n"
            "           failure). Set overwrite_files true (or timestamped\n"
            "           auto-names) in the config so the overwrite confirmation\n"
            "           popup cannot stall the run. Progress is logged with\n"
            "           [AUTO] prefixes on stderr.\n"
            "\n"
            "Positional playback files (no flag needed):\n"
            "  %s <rf-file> [<file2> ...]   loads playback files at launch through\n"
            "           the same content-classifying pipeline as drag-and-drop\n"
            "           (RF files -> channel A by default, audio-rate file -> the\n"
            "           monitor source) and starts playback. Works on every\n"
            "           platform - including macOS while elevated, where Finder\n"
            "           drag-and-drop cannot land in the root-owned window.\n"
            "\n"
            "Headless network server (Linux):\n"
            "  --headless-server [--config <path>] [--select-device <selector>]\n"
            "       One headless Server: legacy RF streams and MISRC-M/2 monitor\n"
            "       status/control/preview on the same TCP port.\n"
            "\n"
            "Headless CLI capture mode:\n"
            "  Pass any capture option (e.g. --device-list, -a FILE) to run this\n"
            "  binary as the full misrc_capture CLI without opening a window.\n"
            "  --device-list / --devices  list available capture devices and exit.\n"
            "\n",
            MIRSC_TOOLS_VERSION,
            program_name ? program_name : "misrc_gui",
            program_name ? program_name : "misrc_gui");
#if !defined(__ANDROID__)
    /* Append the full misrc_capture CLI option list (single source of truth). */
    misrc_capture_print_usage();
#else
    fprintf(stdout, "(Headless CLI capture mode is not available in the Android build.)\n");
#endif
}
// Headless update CLI (--check-update / --update-dry-run / --install-update):
// the same gui_updater pipeline the GUI's Releases Page / Install buttons use,
// runnable without a window and assertable by automated tests (the update_cli
// meson test drives all modes against a local test server; see
// misrc_tools/test/update_cli_test.py).
typedef enum {
    GUI_UPDATE_CLI_NONE = 0,
    GUI_UPDATE_CLI_CHECK,
    GUI_UPDATE_CLI_DRY_RUN,
    GUI_UPDATE_CLI_INSTALL,
} gui_update_cli_mode_t;

static int gui_run_update_cli(gui_update_cli_mode_t mode, const char *manifest_source, bool confirmed)
{
    const char *mode_name = (mode == GUI_UPDATE_CLI_CHECK) ? "check"
                          : (mode == GUI_UPDATE_CLI_DRY_RUN) ? "dry-run"
                          : "install";
    printf("update_mode: %s\n", mode_name);
    const char *source = (manifest_source && manifest_source[0]) ? manifest_source
                                                                 : GUI_UPDATER_MANIFEST_URL;
    printf("manifest_source: %s\n", source);

    gui_updater_manifest_t manifest;
    char manifest_error[GUI_UPDATER_ERROR_MAX] = {0};
    if (!gui_updater_load_manifest_from(source, &manifest,
                                        manifest_error, sizeof(manifest_error))) {
        printf("manifest: failed\n");
        printf("manifest_error: %s\n", manifest_error[0] ? manifest_error : "unknown error");
        return 1;
    }
    printf("manifest: ok\n");

    char latest_tag[GUI_UPDATER_TAG_MAX] = {0};
    char tag_error[GUI_UPDATER_ERROR_MAX] = {0};
    if (!gui_updater_fetch_latest_release_tag(manifest.tag_lookup_url,
                                              latest_tag, sizeof(latest_tag),
                                              tag_error, sizeof(tag_error))) {
        printf("tag_lookup: failed\n");
        printf("tag_error: %s\n", tag_error[0] ? tag_error : "unknown error");
        return 1;
    }
    printf("latest: %s\n", latest_tag);
    printf("current: %s\n", MIRSC_TOOLS_VERSION);
    /* Dev/unversioned builds are never comparable with a release tag: the
     * compare returns 0 (never "older"), and the flag below tells scripts
     * why no update is claimed for this binary. */
    bool current_comparable = gui_updater_version_is_comparable(MIRSC_TOOLS_VERSION);
    printf("current_comparable: %s\n", current_comparable ? "true" : "false");
    int cmp = gui_updater_compare_versions(MIRSC_TOOLS_VERSION, latest_tag);
    printf("update_available: %s\n", (cmp < 0 && current_comparable) ? "true" : "false");

    if (mode == GUI_UPDATE_CLI_CHECK) {
        return 0;
    }

    char asset_url[GUI_UPDATER_URL_MAX] = {0};
    char asset_name[192] = {0};
    if (!gui_updater_build_asset_url(&manifest, latest_tag,
                                    asset_url, sizeof(asset_url),
                                    asset_name, sizeof(asset_name))) {
        printf("asset_url: failed (no asset mapping for this platform)\n");
        return 1;
    }
    printf("asset_url: %s\n", asset_url);

    if (mode == GUI_UPDATE_CLI_DRY_RUN) {
        char exe_path[GUI_UPDATER_PATH_MAX];
        char updates_dir[GUI_UPDATER_PATH_MAX];
        char archive_path[GUI_UPDATER_PATH_MAX];
        bool exe_ok = gui_updater_get_executable_path(exe_path, sizeof(exe_path));
        bool dir_ok = exe_ok && gui_updater_get_updates_dir(updates_dir, sizeof(updates_dir));
        printf("exe_path: %s\n", exe_ok ? exe_path : "unresolved");
        printf("updates_dir: %s\n", dir_ok ? updates_dir : "unresolved");
        if (exe_ok && gui_updater_build_archive_path(exe_path, MIRSC_TOOLS_VERSION,
                                                    archive_path, sizeof(archive_path))) {
            printf("archive_path: %s\n", archive_path);
        } else {
            printf("archive_path: unresolved\n");
        }
        return (exe_ok && dir_ok) ? 0 : 1;
    }

    // --install-update: the explicit CLI invocation + --yes is the user
    // confirmation (the GUI equivalent is the two-click Install confirm).
    if (!confirmed) {
        fprintf(stderr, "--install-update requires --yes (explicit confirmation; the old version is archived, not deleted)\n");
        return 1;
    }
    char install_status[GUI_UPDATER_STATUS_MAX] = {0};
    char archive_path[GUI_UPDATER_PATH_MAX] = {0};
    bool install_ok = gui_updater_install_asset(asset_url, asset_name, MIRSC_TOOLS_VERSION,
                                                install_status, sizeof(install_status),
                                                archive_path, sizeof(archive_path));
    printf("install: %s\n", install_ok ? "ok" : "failed");
    printf("install_status: %s\n", install_status[0] ? install_status : "-");
    printf("archive: %s\n", archive_path[0] ? archive_path : "-");
    return install_ok ? 0 : 1;
}

static bool gui_status_is_permission_denied(const gui_app_t *app) {
    if (!app) return false;
    return strstr(app->status_message, "Permission denied") != NULL ||
           strstr(app->status_message, "permission denied") != NULL ||
           strstr(app->status_message, "device not granted") != NULL ||
           strstr(app->status_message, "USB open failed") != NULL;
}

static bool gui_primary_modifier_down(void)
{
    bool down = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
#if defined(__APPLE__)
    down = down || IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
#endif
    return down;
}
static const char *gui_dropout_reason_status(gui_dropout_reason_t reason) {
    switch (reason) {
        case GUI_DROPOUT_MISSED_FRAME:
            return "Capture stopped: dropout (missed frames)";
        case GUI_DROPOUT_FRAME_ERROR:
            return "Capture stopped: dropout (frame errors)";
        case GUI_DROPOUT_ERROR_BURST:
            return "Capture stopped: dropout (error burst)";
        case GUI_DROPOUT_CALLBACK_GAP:
            return "Capture stopped: dropout (callback gap)";
        case GUI_DROPOUT_DEVICE_ERROR:
            return "Capture stopped: dropout (device error)";
        case GUI_DROPOUT_BACKPRESSURE:
            return "Capture stopped: dropout (backpressure drops)";
        case GUI_DROPOUT_DISK_SPACE:
            return "Capture stopped: low disk space (dynamic guard)";
        case GUI_DROPOUT_LOW_SIGNAL:
            return "Recording stopped: sustained low/no signal (tape end)";
        case GUI_DROPOUT_NONE:
        default:
            return "Capture stopped: dropout detected";
    }
}
// --auto-record <seconds> unattended record cycle phases (local automated
// load/capture testing; see the state machine in the main loop).
typedef enum {
    AUTO_REC_IDLE = 0,      // no --auto-record: normal interactive session
    AUTO_REC_WAIT_CAPTURE,  // armed, waiting for capture to start
    AUTO_REC_SETTLE,        // capture running, 2 s settle before recording
    AUTO_REC_RECORDING,     // recording until the deadline
    AUTO_REC_FINALIZING,    // recording stopped, waiting for the file finalize
} auto_rec_phase_t;

typedef struct {
    bool valid;
    device_type_t type;
    int index;
    char name[64];
    char serial[64];
#ifdef ENABLE_DDD
    ddd_device_profile_t ddd_profile;
    char ddd_usb_path[DDD_STABLE_ID_MAX];
#endif
} gui_reconnect_target_t;
// Portable case-insensitive substring search (POSIX strcasestr is not
// available on every target; keeps the binary dependency-light).
static const char *gui_strcasestr_local(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    if (!needle[0]) return haystack;
    for (const char *h = haystack; *h; h++) {
        const char *hi = h;
        const char *ni = needle;
        while (*hi && *ni &&
               tolower((unsigned char)*hi) == tolower((unsigned char)*ni)) {
            hi++;
            ni++;
        }
        if (!*ni) return h;
    }
    return NULL;
}

static int gui_find_first_device_of_type(const gui_app_t *app, device_type_t type) {
    if (!app) return -1;
    for (int i = 0; i < app->device_count; i++) {
        if (app->devices[i].type == type) {
            return i;
        }
    }
    return -1;
}
static void gui_set_reconnect_target_from_selected(const gui_app_t *app, gui_reconnect_target_t *target) {
    if (!target) return;
    target->valid = false;
    target->type = DEVICE_TYPE_HSDAOH;
    target->index = -1;
    target->name[0] = '\0';
    target->serial[0] = '\0';
#ifdef ENABLE_DDD
    target->ddd_profile = DDD_DEVICE_NOT_DDD;
    target->ddd_usb_path[0] = '\0';
#endif
    if (!app) return;
    if (app->selected_device < 0 || app->selected_device >= app->device_count) return;
    const device_info_t *dev = &app->devices[app->selected_device];
    target->valid = true;
    target->type = dev->type;
    target->index = dev->index;
    snprintf(target->name, sizeof(target->name), "%s", dev->name);
    snprintf(target->serial, sizeof(target->serial), "%s", dev->serial);
#ifdef ENABLE_DDD
    if (dev->type == DEVICE_TYPE_DDD) {
        target->ddd_profile = dev->ddd_profile;
        snprintf(target->ddd_usb_path, sizeof(target->ddd_usb_path), "%s",
                 dev->ddd_usb_path);
    }
#endif
}
static int gui_find_reconnect_device(const gui_app_t *app, const gui_reconnect_target_t *target) {
    if (!app || !target || !target->valid) return -1;
    int fallback_same_type = -1;
    for (int i = 0; i < app->device_count; i++) {
        const device_info_t *dev = &app->devices[i];
        if (dev->type != target->type) {
            continue;
        }
        if (fallback_same_type < 0) {
            fallback_same_type = i;
        }
        if (target->type == DEVICE_TYPE_HSDAOH) {
            if (target->name[0] && strcmp(dev->name, target->name) == 0) {
                return i;
            }
            if (target->index >= 0 && dev->index == target->index) {
                return i;
            }
        } else if (target->type == DEVICE_TYPE_SIMPLE_CAPTURE) {
            if (target->serial[0] && strcmp(dev->serial, target->serial) == 0) {
                return i;
            }
            if (target->name[0] && strcmp(dev->name, target->name) == 0) {
                return i;
            }
        } else if (target->type == DEVICE_TYPE_CXADC) {
            // Multiple synthetic CXADC entries can exist (e.g. CXADC Clockgen).
            // Match by marker serial/name first so reconnect preserves the
            // selected variant. (MISRC Clockgen now has its own device type.)
            if (target->serial[0] && strcmp(dev->serial, target->serial) == 0) {
                return i;
            }
            if (target->name[0] && strcmp(dev->name, target->name) == 0) {
                return i;
            }
            if (target->index >= 0 && dev->index == target->index) {
                return i;
            }
        } else if (target->type == DEVICE_TYPE_MISRC_CLOCKGEN) {
            // MISRC Clockgen is a single synthetic entry; match by marker
            // serial/name so reconnect stays on it across re-enumeration.
            if (target->serial[0] && strcmp(dev->serial, target->serial) == 0) {
                return i;
            }
            if (target->name[0] && strcmp(dev->name, target->name) == 0) {
                return i;
            }
        }
#ifdef ENABLE_DDD
        else if (target->type == DEVICE_TYPE_DDD) {
            // The name keeps the synthetic Clockgen variant separate. A
            // protocol-v1 device must also retain its exact USB topology path;
            // never switch to another same-name DdD during auto-reconnect.
            if (!target->name[0] || strcmp(dev->name, target->name) != 0 ||
                dev->ddd_profile != target->ddd_profile) {
                continue;
            }
            if (!ddd_profile_requires_usb_path(target->ddd_profile) ||
                ddd_reconnect_path_matches(
                    target->ddd_profile, target->ddd_usb_path,
                    dev->ddd_profile, dev->ddd_usb_path)) {
                return i;
            }
        }
#endif
        else {
            return i;
        }
    }
#ifdef ENABLE_DDD
    // DdD profiles are not interchangeable. If the selected profile/path is
    // absent, wait for it instead of falling back to another DdD device.
    if (target->type == DEVICE_TYPE_DDD) return -1;
#endif
    return fallback_same_type;
}

#if defined(__APPLE__)
static bool gui_append_text(char *dst, size_t dst_cap, size_t *len, const char *src)
{
    if (!dst || !len || !src || dst_cap == 0) return false;
    while (*src) {
        if ((*len + 1) >= dst_cap) {
            return false;
        }
        dst[(*len)++] = *src++;
    }
    dst[*len] = '\0';
    return true;
}

static bool gui_append_shell_quoted_arg(char *dst, size_t dst_cap, size_t *len, const char *arg)
{
    if (!dst || !len || !arg) return false;
    if (!gui_append_text(dst, dst_cap, len, "'")) return false;
    for (const char *p = arg; *p; ++p) {
        if (*p == '\'') {
            if (!gui_append_text(dst, dst_cap, len, "'\\''")) return false;
        } else {
            char ch[2] = { *p, '\0' };
            if (!gui_append_text(dst, dst_cap, len, ch)) return false;
        }
    }
    return gui_append_text(dst, dst_cap, len, "'");
}

static bool gui_get_executable_path(const char *argv0, char *out, size_t out_cap)
{
    if (!out || out_cap == 0) return false;
    out[0] = '\0';

    uint32_t n = (uint32_t)out_cap;
    if (_NSGetExecutablePath(out, &n) == 0) {
        char resolved[PATH_MAX];
        if (realpath(out, resolved)) {
            snprintf(out, out_cap, "%s", resolved);
        }
        return true;
    }

    if (argv0 && realpath(argv0, out)) {
        return true;
    }
    if (argv0 && argv0[0] != '\0') {
        snprintf(out, out_cap, "%s", argv0);
        return true;
    }
    return false;
}

static bool gui_build_elevated_command(int argc, char **argv, char *out, size_t out_cap)
{
    if (!argv || !out || out_cap == 0) return false;
    out[0] = '\0';
    size_t len = 0;

    char exe_path[PATH_MAX];
    if (!gui_get_executable_path(argv[0], exe_path, sizeof(exe_path))) {
        return false;
    }

    if (!gui_append_text(out, out_cap, &len, "MISRC_GUI_ELEVATED=1 ")) return false;
    if (!gui_append_shell_quoted_arg(out, out_cap, &len, exe_path)) return false;

    for (int i = 1; i < argc; i++) {
        if (!gui_append_text(out, out_cap, &len, " ")) return false;
        if (!gui_append_shell_quoted_arg(out, out_cap, &len, argv[i])) return false;
    }

    return true;
    return true;
}

static int gui_macos_relaunch_as_admin_if_needed(int argc, char **argv)
{
    if (getenv("MISRC_GUI_NO_ADMIN") || geteuid() == 0) {
        return 0;
    }

    const char *already_elevated = getenv("MISRC_GUI_ELEVATED");
    if (already_elevated && strcmp(already_elevated, "1") == 0) {
        return -1;
    }

    char command[4096];
    if (!gui_build_elevated_command(argc, argv, command, sizeof(command))) {
        return -1;
    }

    char *const osascript_argv[] = {
        "osascript",
        "-e", "on run argv",
        "-e", "do shell script (item 1 of argv) with administrator privileges",
        "-e", "end run",
        command,
        NULL
    };

    pid_t pid = 0;
    int spawn_rc = posix_spawn(&pid, "/usr/bin/osascript", NULL, NULL, osascript_argv, environ);
    if (spawn_rc != 0) {
        return -1;
    }

    (void)pid;
    return 1;
}
#endif
#if defined(_WIN32)
static void gui_enable_debug_console(void) {
    if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()) {
        FILE *stream = freopen("CONOUT$", "w", stdout);
        (void)stream;
        stream = freopen("CONOUT$", "w", stderr);
        (void)stream;
        stream = freopen("CONIN$", "r", stdin);
        (void)stream;
    }
}
#endif

// Shared Clay layout build (the main loop + the Windows modal frames).
// Extracted so the modal-loop render driver renders the SAME layout the
// main loop renders, from one source of truth.
static Clay_RenderCommandArray app_build_render_commands(gui_app_t *app)
{
    Clay_BeginLayout();
    gui_video_update_rf_routes(app);
    gui_render_layout(app);
    return Clay_EndLayout();
}

// One render-only frame driven from INSIDE the Windows modal move/size loop
// (WM_TIMER in the subclassed window proc; see gui_win32_modal.c). The
// panels' data threads keep running during the drag, so re-rendering the
// current state keeps the scopes (and the audio monitor) alive. This frame
// must NEVER poll events and must not run the normal frame tail: a nested
// PeekMessage inside DefWindowProc's modal loop is reentrancy, and the
// timer already paces (no FPS wait). The flush + swap below is exactly the
// normal tail's core minus its polling.
static void app_modal_frame(void *ctx)
{
    gui_app_t *app = (gui_app_t *)ctx;

    // Keep the audio monitor fed while the main loop is blocked
    gui_audio_update_playback(app);

    // Fresh layout dimensions (the window size changes live during a
    // resize; raylib's WindowSizeCallback already ran SetupViewport on this
    // thread, so frames render at the live size).
    Clay_SetLayoutDimensions((Clay_Dimensions){
        (float)gui_ui_get_layout_width(), (float)gui_ui_get_layout_height() });

    Clay_RenderCommandArray render_commands = app_build_render_commands(app);

    BeginDrawing();
    ClearBackground(COLOR_BG);
    Clay_Raylib_Render(render_commands, fonts);
    rlDrawRenderBatchActive();
    SwapScreenBuffer();
}

#if defined(__linux__) && !defined(__ANDROID__)
static volatile sig_atomic_t s_headless_stop = 0;
static void gui_headless_signal(int signo) { (void)signo; s_headless_stop = 1; }

/* One headless listener serves both RF and MISRC-M/2 monitor clients. */
static int gui_run_role_headless_server(const char *config_path,
                                        const char *selector, bool debug_view) {
    s_headless_stop = 0;
    gui_runtime_set_headless(true);
    SetTraceLogLevel(debug_view ? LOG_INFO : LOG_WARNING);
    if (config_path) {
        FILE *f = fopen(config_path, "rb");
        if (!f) {
            fprintf(stderr, "[HEADLESS] Can't read configuration: %s\n", config_path);
            gui_runtime_set_headless(false);
            return 1;
        }
        fclose(f);
    }
    gui_app_t app = {0};
    app.headless_server = true;
    gui_settings_set_override_path(config_path);
    gui_settings_load(&app.settings);
    app.settings.net_mode = GUI_NET_MODE_SERVER;
    app.settings.capture_limit_seconds = 0;
    gui_app_init(&app);
    if (!gui_net_active(&app)) {
        fprintf(stderr, "[HEADLESS] Couldn't start unified server on %u\n",
                (unsigned)app.settings.net_server_port);
        gui_app_cleanup(&app);
        gui_runtime_set_headless(false);
        return 1;
    }
    struct sigaction sa = {0};
    sa.sa_handler = gui_headless_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    fprintf(stderr, "[HEADLESS] Server listening on TCP %u (RF + MISRC-M/2)\n",
            (unsigned)app.settings.net_server_port);
    bool started_once = false;
    double next_probe = gui_runtime_time();
    while (!s_headless_stop && !atomic_load(&do_exit)) {
        gui_net_poll_commands(&app);
        if (app.is_capturing) started_once = true;
        gui_app_poll_capture_start(&app);
        gui_capture_poll_hsdaoh_status(&app);
        gui_app_update_vu_meters(&app, 0.05f);
        /* First-start only; a remote /stop must not auto-restart capture. */
        double now = gui_runtime_time();
        if (!started_once && now >= next_probe) {
            gui_app_enumerate_devices(&app);
            int idx = -1;
            if (selector && *selector) {
                char *end = NULL;
                long parsed = strtol(selector, &end, 10);
                if (end != selector && *end == '\0') {
                    if (parsed >= 0 && parsed < app.device_count) idx = (int)parsed;
                } else {
                    for (int i = 0; i < app.device_count; i++) {
                        if (gui_strcasestr_local(app.devices[i].name, selector)) { idx = i; break; }
                    }
                }
            } else {
                const device_type_t types[] = {DEVICE_TYPE_HSDAOH, DEVICE_TYPE_MISRC_CLOCKGEN,
                                               DEVICE_TYPE_CXADC, DEVICE_TYPE_SIMPLE_CAPTURE};
                for (size_t j = 0; j < sizeof(types)/sizeof(types[0]); j++) {
                    idx = gui_find_first_device_of_type(&app, types[j]);
                    if (idx >= 0) break;
                }
            }
            if (idx >= 0) {
                app.selected_device = idx;
                if (gui_app_start_capture(&app) == 0) {
                    started_once = true;
                    fprintf(stderr, "[HEADLESS] Capture started: %s\n", app.devices[idx].name);
                }
            }
            next_probe = now + 3.0;
        }
        thrd_sleep_ms(50);
    }
    fprintf(stderr, "[HEADLESS] Stopping unified server\n");
    if (app.is_recording) gui_app_stop_recording(&app);
    if (app.is_capturing) gui_app_stop_capture(&app);
    gui_app_cleanup(&app);
    gui_runtime_set_headless(false);
    return 0;
}
#endif

int main(int argc, char **argv) {
    if (!file_init_utf8_argv(&argc, &argv)) return 1;
    bool debug_view = false;
    bool show_help = false;
    bool show_version = false;
    bool smoke_test = false;
    bool auto_connect = false;
    bool headless_server = false;
    bool has_capture_arg = false;
    const char *config_path = NULL;  /* --config <path> override */
    const char *select_device_arg = NULL;  /* --select-device <index-or-name> */
    bool auto_capture = false;             /* --auto-capture */
    bool auto_record_given = false;        /* --auto-record <seconds> seen */
    int auto_record_seconds = 0;           /* --auto-record seconds */
    gui_update_cli_mode_t update_cli_mode = GUI_UPDATE_CLI_NONE; /* headless update CLI */
    bool update_cli_mode_error = false;   /* more than one update mode flag given */
    const char *update_manifest_source = NULL;  /* --update-manifest <url-or-path> */
    bool update_cli_yes = false;                 /* --install-update confirmation */
    // Positional playback files (see print_usage): bare paths with no capture
    // flags seen, collected for the launch-time drop-pipeline load.
    const char *positional_files[8] = {0};
    int positional_file_count = 0;
    // First pass: classify args. Any arg that isn't a pure GUI flag is treated
    // as a capture arg and routes the process into headless CLI capture mode
    // (the full misrc_capture CLI), so the GUI binary doubles as the CLI tool
    // when invoked from a terminal with capture options. EXCEPTION: a bare
    // path (not a flag, no capture flag seen) is a positional playback file.
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if ((strcmp(a, "--help") == 0) || (strcmp(a, "-h") == 0)) {
            show_help = true;
            continue;
        }
        if (strcmp(a, "--version") == 0) {
            show_version = true;
            continue;
        }
        if (strcmp(a, "--smoke-test") == 0) {
            smoke_test = true;
            continue;
        }
        if (strcmp(a, "--headless-server") == 0) {
            headless_server = true;
            continue;
        }
        if (strcmp(a, "--auto-connect") == 0) {
            auto_connect = true;
            continue;
        }
        if (strcmp(argv[i], "--debug-view") == 0) {
            debug_view = true;
            continue;
        }
        if (strcmp(a, "--config") == 0) {
            // --config <path>: load settings from <path> instead of the
            // platform default. Consumes the next arg. Recognized as a GUI
            // flag so it is NOT treated as a capture arg (which would route
            // the process into headless CLI capture mode).
            if (i + 1 < argc) {
                config_path = argv[++i];
            }
            continue;
        }
        if (strcmp(a, "--select-device") == 0) {
            // --select-device <index-or-name>: explicit device selection for
            // automated runs. Consumes the next arg.
            if (i + 1 < argc) {
                select_device_arg = argv[++i];
            }
            continue;
        }
        if (strcmp(a, "--auto-capture") == 0) {
            auto_capture = true;
            continue;
        }
        if (strcmp(a, "--auto-record") == 0) {
            // --auto-record <seconds>: unattended record cycle. Consumes the
            // next arg.
            if (i + 1 < argc) {
                auto_record_given = true;
                auto_record_seconds = atoi(argv[++i]);
            }
            continue;
        }
        if (strcmp(a, "--check-update") == 0 ||
            strcmp(a, "--update-dry-run") == 0 ||
            strcmp(a, "--install-update") == 0) {
            // Headless update mode flags (GUI flags: must NOT route into the
            // capture CLI). At most one mode per invocation.
            gui_update_cli_mode_t wanted =
                (strcmp(a, "--check-update") == 0) ? GUI_UPDATE_CLI_CHECK
                : (strcmp(a, "--update-dry-run") == 0) ? GUI_UPDATE_CLI_DRY_RUN
                : GUI_UPDATE_CLI_INSTALL;
            if (update_cli_mode != GUI_UPDATE_CLI_NONE) {
                update_cli_mode_error = true;
            }
            update_cli_mode = wanted;
            continue;
        }
        if (strcmp(a, "--update-manifest") == 0) {
            // --update-manifest <url-or-path>: manifest source override for
            // diagnostics/automated tests. Consumes the next arg.
            if (i + 1 < argc) {
                update_manifest_source = argv[++i];
            } else {
                fprintf(stderr, "--update-manifest requires a <url-or-path> value\n");
                return 1;
            }
            continue;
        }
        if (strcmp(a, "--yes") == 0) {
            update_cli_yes = true;
            continue;
        }
        if (a[0] != '\0' && a[0] != '-' && !has_capture_arg) {
            // Bare path with no capture flag seen: a positional playback file
            // (misrc_gui <file> [...] / Finder "Open With"). Capture-flag
            // values never land here (their flags set has_capture_arg first),
            // so the headless CLI routing is unchanged.
            if (positional_file_count <
                (int)(sizeof(positional_files) / sizeof(positional_files[0]))) {
                positional_files[positional_file_count++] = a;
            } else {
                fprintf(stderr, "Too many positional files (max %d); ignoring: %s\n",
                        (int)(sizeof(positional_files) / sizeof(positional_files[0])), a);
            }
            continue;
        }
        has_capture_arg = true;
    }
#if !defined(__ANDROID__)
    // Headless CLI capture mode: run as the full misrc_capture CLI (no window).
    // misrc_capture_main handles --device-list/--devices, -a/-b/-r/-x, -f, -n/-t,
    // resample/audio opts, and prints full CLI usage on an incomplete combo.
    if (has_capture_arg && !headless_server) {
        return misrc_capture_main(argc, argv);
    }
#endif
    if (show_help) {
        print_usage(argv[0]);
        return 0;
    }
    if (show_version) {
        fprintf(stdout, "%s\n", MIRSC_TOOLS_VERSION);
        return 0;
    }
    if (smoke_test) {
        return 0;
    }
    if (update_cli_mode != GUI_UPDATE_CLI_NONE || update_cli_mode_error) {
        // Headless update mode: fully windowless, before settings/elevation.
        if (update_cli_mode_error) {
            fprintf(stderr, "pass at most one of --check-update/--update-dry-run/--install-update\n");
            return 1;
        }
        return gui_run_update_cli(update_cli_mode, update_manifest_source, update_cli_yes);
    }
    if (headless_server) {
        if (has_capture_arg || auto_connect || auto_capture || auto_record_given ||
            update_cli_mode != GUI_UPDATE_CLI_NONE) {
            fprintf(stderr, "Headless server flags cannot be combined with capture/update/auto flags\n");
            return 2;
        }
#if defined(__linux__) && !defined(__ANDROID__)
        return gui_run_role_headless_server(config_path, select_device_arg, debug_view);
#else
        fprintf(stderr, "Headless server currently supported on Linux only\n");
        return 2;
#endif
    }
    if (auto_record_given && auto_record_seconds < 1) {
        fprintf(stderr, "--auto-record requires a positive <seconds> value\n");
        return 1;
    }
    if (auto_connect && !config_path) {
        // --auto-connect requires --config so the net_mode/host are known.
        // Without a config the stock loadup is Local and there is nothing to
        // auto-connect to.
        fprintf(stderr, "--auto-connect requires --config <path> (need a server/client config)\n");
        return 1;
    }
#if defined(__APPLE__)
    int elevate_rc = gui_macos_relaunch_as_admin_if_needed(argc, argv);
    if (elevate_rc > 0) {
        return 0;
    }
    if (elevate_rc < 0) {
        fprintf(stderr, "Administrator permissions are required for MS2130 hsdaoh/libusb capture.\n");
        return 1;
    }
    /* On macOS (especially Apple Silicon), mark the process as a foreground
     * application and clear any inherited darwin-background throttling bit.
     * When misrc_gui is launched via osascript "do shell script ... with
     * administrator privileges", the child process often inherits a task
     * role that biases every thread to the efficiency cluster regardless of
     * thread-level QoS. This call fixes that before any capture threads
     * are created. */
    macos_process_prefer_p_cores();
#endif

#if defined(_WIN32)
    if (debug_view) {
        gui_enable_debug_console();
    }
#endif
    // Initialize application state
    gui_app_t app = {0};
    app.fonts = fonts;
    gui_reconnect_target_t reconnect_target = {0};

    // Initialize sample rate early (before any capture/rendering can occur)
    atomic_store(&app.sample_rate, DEFAULT_SAMPLE_RATE);

    // Load persistent settings (includes desktop path defaults). If --config
    // <path> was passed, load from that file instead of the platform default
    // so automated tests can launch the GUI with a pre-written config.
    gui_settings_set_override_path(config_path);
    gui_settings_load(&app.settings);
#if defined(__ANDROID__)
    // Render the existing UI without layout zoom on Android. The touch
    // viewport scales the whole frame, including its outer borders.
    app.settings.ui_scale_percent = GUI_UI_SCALE_DEFAULT_PERCENT;
#endif
    gui_ui_set_scale_percent(app.settings.ui_scale_percent);
    // Push the persisted CVBS System selection (0=PAL/1=NTSC/2=SECAM) into
    // the video panel decoders: every created panel starts in the user's
    // last-selected system instead of hardcoded PAL.
    gui_cvbs_set_default_system(app.settings.video_system);

    // Capture limit should not persist across relaunches.
    app.settings.capture_limit_seconds = 0;

    // Initialize raylib window
    unsigned int window_flags = FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT;
    SetConfigFlags(window_flags);
    // Keep defaults usable while fitting common laptop screens.
    const int default_window_width = 1425;
    const int default_window_height = 720;
    // Allow quarter-screen tiling and compact desktop snapping on common displays.
    const int min_window_width = 640;
    const int min_window_height = 360;
    char window_title[128];
    snprintf(window_title, sizeof(window_title), "MISRC Capture %s", MIRSC_TOOLS_VERSION);
#if defined(__linux__) && !defined(__ANDROID__)
    /* X11 window identity for launcher/taskbar matching. GLFW (raylib) sets
     * WM_CLASS = { RESOURCE_NAME env (else the title), the title } when the
     * native window is created, so the class used to be the versioned title
     * ("MISRC Capture v1.2.2") and changed every release, which left the
     * desktop launcher's StartupWMClass stale after every update. Pin the
     * instance name here (AppRun exports the same value) and create the window
     * with the fixed title below; the versioned title is applied afterwards and
     * only changes the title bar. Must match StartupWMClass in
     * assets/appimage/misrc_gui.desktop and AppRun (checked by ci_guard_tests). */
    setenv("RESOURCE_NAME", "misrc_gui", 0);
#endif
    InitWindow(default_window_width, default_window_height, MISRC_WINDOW_CLASS_TITLE);
    SetWindowTitle(window_title);
    Image app_icon = LoadImageFromMemory(".png", misrc_icon_png_data, misrc_icon_png_data_size);
    if (app_icon.data != NULL) {
        SetWindowIcon(app_icon);
        UnloadImage(app_icon);
    }
    SetWindowMinSize(min_window_width, min_window_height);
    SetTraceLogLevel(debug_view ? LOG_INFO : LOG_WARNING);
    SetTargetFPS(60);
    SetExitKey(0);  // Disable escape key auto-close

    // Load embedded Inter font directly from memory (Apache 2.0 licensed)
    // Font data is ~342KB and embedded as a C array for complete portability
    fonts[0] = LoadFontFromMemory(".ttf", inter_font_data, inter_font_data_size,
                                  UI_FONT_ATLAS_SIZE, NULL, 256);
    if (fonts[0].texture.id == 0) {
        fprintf(stderr, "Error: Failed to load embedded Inter font data\n");
        CloseWindow();
        return 1;
    }
    SetTextureFilter(fonts[0].texture, TEXTURE_FILTER_BILINEAR);

    // Load embedded Space Mono font directly from memory (SIL Open Font License)
    // Font data is embedded as a C array for complete portability
    fonts[1] = LoadFontFromMemory(".ttf", space_mono_font_data, space_mono_font_data_size,
                                  UI_FONT_ATLAS_SIZE, NULL, 256);
    if (fonts[1].texture.id == 0) {
        fprintf(stderr, "Error: Failed to load embedded Space Mono font data\n");
        CloseWindow();
        return 1;
    }
    SetTextureFilter(fonts[1].texture, TEXTURE_FILTER_BILINEAR);

    // Initialize Clay
    uint64_t clay_memory_size = Clay_MinMemorySize();
    void *clay_memory = malloc(clay_memory_size);
    if (!clay_memory) {
        fprintf(stderr, "Failed to allocate Clay memory\n");
        CloseWindow();
        return 1;
    }

    Clay_Arena clay_arena = Clay_CreateArenaWithCapacityAndMemory(clay_memory_size, clay_memory);
    Clay_Initialize(clay_arena,
                    (Clay_Dimensions){ (float)gui_ui_get_layout_width(),
                                       (float)gui_ui_get_layout_height() },
                    (Clay_ErrorHandler){
                        .errorHandlerFunction = clay_error_handler,
                        .userData = NULL,
                    });
    Clay_SetMeasureTextFunction(Raylib_MeasureText, fonts);

    // Initialize application
    gui_app_init(&app);

    // Set app for text rendering font access
    gui_text_set_app(&app);

#if defined(__APPLE__)
    /* raylib/Metal/dispatch workqueues have now created their internal
     * helper threads. Walk the whole task and force every one of them off
     * the timeshare class with a USER_INTERACTIVE QoS override so the
     * Apple Silicon CLPC scheduler stops migrating them onto the E-cluster. */
    macos_promote_all_task_threads();
#endif

    // Enumerate available devices
    gui_app_enumerate_devices(&app);
    {
        int hs_idx = gui_find_first_device_of_type(&app, DEVICE_TYPE_HSDAOH);
        if (hs_idx >= 0) {
            app.selected_device = hs_idx;
        } else {
            int misrc_cg_idx = gui_find_first_device_of_type(&app, DEVICE_TYPE_MISRC_CLOCKGEN);
            if (misrc_cg_idx >= 0) {
                app.selected_device = misrc_cg_idx;
            } else {
                int cxadc_idx = gui_find_first_device_of_type(&app, DEVICE_TYPE_CXADC);
                if (cxadc_idx >= 0) {
                    app.selected_device = cxadc_idx;
                }
            }
        }
    }

    // Positional file arguments (misrc_gui <file> [...] / Finder "Open
    // With"): route through the same content-classifying drop pipeline as
    // drag-and-drop. Zone defaults to channel A (no panel bounds are rendered
    // yet, so the drop-zone picker resolves to A). Unlike drag-and-drop this
    // also works on macOS while elevated (root windows never receive Finder
    // drops; the elevated relaunch forwards argv, so the file argument
    // survives it).
    if (positional_file_count > 0) {
        (void)gui_playback_handle_dropped_files(&app,
            (const char *const *)positional_files,
            (size_t)positional_file_count, 0.0f, 0.0f);
    }

    // Enable auto-reconnect by default
    app.auto_reconnect_enabled = true;

    // --auto-connect: when launched with a server/client config, trigger the
    // full connection chain without human intervention. Server mode
    // auto-starts capture so RF data flows to clients; Client mode
    // auto-connects to the configured server (the worker already starts
    // from gui_app_init when a host is set, but set the status so the
    // user knows the connection is automatic, not waiting for a click).
    if (auto_connect) {
        if (app.settings.net_mode == GUI_NET_MODE_SERVER) {
            // Server: auto-start capture after a short delay so devices
            // have time to enumerate. The reconnect logic in the main
            // loop will pick this up and start capture.
            app.reconnect_pending = true;
            app.reconnect_attempt_time = GetTime() + 1.0;  // 1s grace
            app.reconnect_attempts = 0;
            gui_set_reconnect_target_from_selected(&app, &reconnect_target);
            gui_app_set_status(&app, "Auto-connect: starting server capture...");
            fprintf(stderr, "[GUI] --auto-connect: server mode, auto-starting capture\n");
        } else if (app.settings.net_mode == GUI_NET_MODE_CLIENT) {
            // Client: the worker already started from gui_app_init.
            // Just confirm the connection is automatic.
            gui_app_set_status(&app, "Auto-connect: client connecting to server...");
            fprintf(stderr, "[GUI] --auto-connect: client mode, auto-connecting to %s:%u\n",
                    app.settings.net_client_host, (unsigned)app.settings.net_client_port);
        } else {
            fprintf(stderr, "[GUI] --auto-connect: net_mode is Local, nothing to auto-connect to\n");
        }
    } else if (app.device_count > 0) {
        int hs_idx = gui_find_first_device_of_type(&app, DEVICE_TYPE_HSDAOH);
        if (hs_idx >= 0) {
            app.selected_device = hs_idx;
            gui_set_reconnect_target_from_selected(&app, &reconnect_target);
            gui_app_set_status(&app, "Ready. Click Connect to start capture.");
        } else {
            int misrc_cg_idx = gui_find_first_device_of_type(&app, DEVICE_TYPE_MISRC_CLOCKGEN);
            if (misrc_cg_idx >= 0) {
                app.selected_device = misrc_cg_idx;
                gui_set_reconnect_target_from_selected(&app, &reconnect_target);
                gui_app_set_status(&app, "Ready. Click Connect to start capture.");
            } else {
                int cxadc_idx = gui_find_first_device_of_type(&app, DEVICE_TYPE_CXADC);
                if (cxadc_idx >= 0) {
                    app.selected_device = cxadc_idx;
                    gui_set_reconnect_target_from_selected(&app, &reconnect_target);
                    gui_app_set_status(&app, "Ready. Click Connect to start capture.");
                } else {
                    int sc_idx = gui_find_first_device_of_type(&app, DEVICE_TYPE_SIMPLE_CAPTURE);
                    if (sc_idx >= 0) {
                        app.selected_device = sc_idx;
                        gui_set_reconnect_target_from_selected(&app, &reconnect_target);
                        gui_app_set_status(&app, "Ready. Click Connect to start capture.");
                    } else {
                        gui_app_set_status(&app, "No hsdaoh/CXADC devices found. Select device and click Connect.");
                    }
                }
            }
        }
    } else {
        gui_app_set_status(&app, "No devices found. Connect a device and retry.");
    }

    // --select-device <index-or-name>: explicit device selection for automated
    // runs (startup otherwise prefers hsdaoh > MISRC Clockgen > CXADC).
    // Accepts a device-list index or a case-insensitive name substring
    // (first match). Fails fast so scripts get a clear error.
    if (select_device_arg) {
        int sel = -1;
        char *endp = NULL;
        long idx = strtol(select_device_arg, &endp, 10);
        if (endp && endp != select_device_arg && *endp == '\0') {
            if (idx >= 0 && idx < app.device_count) {
                sel = (int)idx;
            }
        } else {
            for (int i = 0; i < app.device_count; i++) {
                if (gui_strcasestr_local(app.devices[i].name, select_device_arg)) {
                    sel = i;
                    break;
                }
            }
        }
        if (sel < 0) {
            fprintf(stderr,
                    "[AUTO] --select-device: no device matches '%s' (%d device(s) enumerated)\n",
                    select_device_arg, app.device_count);
            CloseWindow();
            return 1;
        }
        app.selected_device = sel;
        fprintf(stderr, "[AUTO] --select-device: %d: %s\n", sel, app.devices[sel].name);
    }

    // --auto-capture / --auto-record: unattended local capture start. Uses the
    // same reconnect-pending mechanism as --auto-connect in server mode: the
    // main loop's reconnect logic starts capture ~1 s after launch (giving
    // enumeration a stable window first). Local mode, no --config required.
    if ((auto_capture || auto_record_seconds > 0) && !auto_connect &&
        app.device_count > 0) {
        app.reconnect_pending = true;
        app.reconnect_attempt_time = GetTime() + 1.0;  // 1 s grace
        app.reconnect_attempts = 0;
        gui_set_reconnect_target_from_selected(&app, &reconnect_target);
        gui_app_set_status(&app, "Auto-capture: starting capture...");
        fprintf(stderr, "[AUTO] auto-capture armed (device %d: %s)\n",
                app.selected_device, app.devices[app.selected_device].name);
    }
    int last_layout_width = -1;
    int last_layout_height = -1;
    bool recording_fps_throttle = false;
    // --auto-record state (phases above). auto_rec_failed drives the exit
    // code so scripts can detect a failed cycle without parsing logs.
    auto_rec_phase_t auto_rec_phase =
        (auto_record_seconds > 0) ? AUTO_REC_WAIT_CAPTURE : AUTO_REC_IDLE;
    double auto_rec_phase_start = GetTime();
    double auto_rec_deadline = 0.0;
    bool auto_rec_failed = false;
    // One-time startup warning: if FLAC threads is 0 (auto, e.g. loaded from
    // saved settings), warn once on launch so the user knows auto may under-use
    // cores. The FlacThreadsMinus handler only fires on the 1->0 click
    // transition, so a session that starts at 0 would never warn without this.
    bool flac_threads_zero_warned = false;
    // Same pattern for FLAC level 1-3: warn once on launch if the loaded
    // settings have a low compression level.
    bool flac_level_low_warned = false;
    gui_ui_zoom_state_t ui_zoom_state = {0};
    bool ui_scale_save_pending = false;
    double ui_scale_save_deadline = 0.0;
#if defined(__APPLE__) && (defined(__arm64__) || defined(__aarch64__))
    double last_thread_promotion_time = 0.0;
    const double thread_promotion_interval_s = 0.25;
#endif

    // Windows modal move/size render driver: keeps the window rendering
    // while the user drags/resizes it (no-op on Linux/macOS - no modal loop
    // there; see gui_win32_modal.c). Registered after the app exists; the
    // frame callback renders only (never polls events).
    gui_win32_modal_set_frame(app_modal_frame, &app);
    if (!gui_win32_modal_install()) {
        fprintf(stderr, "[WIN32] modal move/size render driver unavailable\n");
    }

    // Main loop
    while (!WindowShouldClose() && !atomic_load(&do_exit)) {
        bool was_capturing = app.is_capturing;
        gui_app_poll_capture_start(&app);
        if (app.is_recording) {
            if (!recording_fps_throttle) {
                SetTargetFPS(30);
                recording_fps_throttle = true;
            }
        } else if (recording_fps_throttle) {
            SetTargetFPS(60);
            recording_fps_throttle = false;
        }
        float dt = GetFrameTime();
        Vector2 wheel_delta = GetMouseWheelMoveV();
        bool primary_modifier_down = gui_primary_modifier_down();
        gui_ui_zoom_result_t ui_zoom_result =
            gui_ui_zoom_process(&ui_zoom_state,
                                app.settings.ui_scale_percent,
                                primary_modifier_down,
                                wheel_delta.x,
                                wheel_delta.y);

        bool zoom_reset_pressed = primary_modifier_down &&
            (IsKeyPressed(KEY_ZERO) || IsKeyPressed(KEY_KP_0));
        bool zoom_in_pressed = primary_modifier_down &&
            (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD));
        bool zoom_out_pressed = primary_modifier_down &&
            (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT));
        bool keyboard_step_pressed = zoom_in_pressed != zoom_out_pressed;
        bool keyboard_zoom_pressed = zoom_reset_pressed || keyboard_step_pressed;
        bool show_ui_scale_hud =
            ui_zoom_result.step_attempted || keyboard_zoom_pressed;

        if (keyboard_zoom_pressed) {
            ui_zoom_state.wheel_remainder = 0.0f;
            if (zoom_reset_pressed) {
                ui_zoom_result.percent = GUI_UI_SCALE_DEFAULT_PERCENT;
            } else {
                int direction = zoom_in_pressed ? 1 : -1;
                ui_zoom_result.percent =
                    gui_ui_scale_step_percent(app.settings.ui_scale_percent,
                                              direction);
            }
        }

        ui_zoom_result.changed =
            ui_zoom_result.percent != app.settings.ui_scale_percent;

        if (ui_zoom_result.changed) {
            app.settings.ui_scale_percent = ui_zoom_result.percent;
            gui_ui_set_scale_percent(ui_zoom_result.percent);
            last_layout_width = -1;
            last_layout_height = -1;
            ui_scale_save_pending = true;
            ui_scale_save_deadline = GetTime() + 0.4;
        }

        if (show_ui_scale_hud) {
            gui_ui_show_scale_hud(ui_zoom_result.percent);
        }

        if (ui_scale_save_pending && GetTime() >= ui_scale_save_deadline) {
            gui_settings_save(&app.settings);
            ui_scale_save_pending = false;
        }

        // Drag-and-drop onto the channel panels: load playback files (RF per
        // channel zone by the drop point + audio-rate files as the monitor
        // source) and start playback. raylib delivers UTF-8 paths; the drop
        // point comes from the logical pointer wrapper (never a second
        // GetMousePosition call - UI scale integration contract).
        if (IsFileDropped()) {
            FilePathList dropped_files = LoadDroppedFiles();
            /* Diagnostic: exactly one line when the platform DELIVERS a drop.
             * A macOS/Linux run that sees no line means the windowing layer
             * never delivered the event (platform issue); a line plus a
             * status/reject message means the handler ran and refused the
             * file(s) (app issue). See dev/playback_drag_drop_audio.md. */
            fprintf(stderr, "[PLAYBACK] Drop delivered: %d file(s)\n",
                    (int)dropped_files.count);
            if (dropped_files.count > 0 && dropped_files.paths) {
                Vector2 drop_point = gui_ui_get_mouse_position();
                gui_playback_handle_dropped_files(&app,
                    (const char *const *)dropped_files.paths,
                    (size_t)dropped_files.count,
                    drop_point.x, drop_point.y);
            }
            UnloadDroppedFiles(dropped_files);
        }
#if defined(__APPLE__)
        /* macOS elevated (root) processes do not receive Finder drag-and-drop
         * (the WindowServer refuses cross-session drag delivery), and this app
         * always relaunches elevated for MS2130 hsdaoh/libusb capture. Tell
         * playback users once where the working entry points are instead of a
         * silent no-op drop. */
        {
            static bool s_macos_drop_hint_shown = false;
            if (!s_macos_drop_hint_shown && geteuid() == 0 &&
                app.selected_device >= 0 && app.selected_device < app.device_count &&
                app.devices[app.selected_device].type == DEVICE_TYPE_PLAYBACK) {
                s_macos_drop_hint_shown = true;
                gui_app_set_status(&app,
                    "macOS elevated: drag-and-drop is unavailable - use Browse or: misrc_gui <file>");
            }
        }
#endif
#if defined(__APPLE__) && (defined(__arm64__) || defined(__aarch64__))
        if (app.is_capturing) {
            double now = GetTime();
            if ((now - last_thread_promotion_time) >= thread_promotion_interval_s) {
                /* Catch late-spawned libusb/dispatch/Metal helper threads that
                 * appear after capture has already started. */
                macos_promote_all_task_threads();
                last_thread_promotion_time = now;
            }
        } else {
            last_thread_promotion_time = 0.0;
        }
#endif
        int current_layout_width = gui_ui_get_layout_width();
        int current_layout_height = gui_ui_get_layout_height();
        if (current_layout_width != last_layout_width || current_layout_height != last_layout_height) {
            Clay_SetLayoutDimensions((Clay_Dimensions){
                (float)current_layout_width, (float)current_layout_height
            });
            last_layout_width = current_layout_width;
            last_layout_height = current_layout_height;
        }

        // Check for pending popup result (for async confirmations like file overwrite)
        gui_record_check_popup(&app);

        // One-time startup warning: if FLAC threads is 0 (auto), warn once.
        // Uses a local bool so it fires only once per session, on the first
        // frame where no other popup is open.
        if (!flac_threads_zero_warned && !gui_popup_is_open()) {
            flac_threads_zero_warned = true;
            // Suppress when FLAC is turned off: the encoder thread setting is
            // meaningless (no FLAC writer runs), so a startup popup is noise.
            if (app.settings.use_flac && app.settings.flac_threads == 0) {
                gui_popup_info("FLAC threads set to auto (0)",
                    "FLAC encoder threads is 0 (auto).\n\n"
                    "Auto may not use all available CPU cores efficiently.\n"
                    "For best encode throughput on multi-core systems, set an\n"
                    "explicit thread count (e.g. 4, 6, or 8) in Settings.\n\n"
                    "You can raise it with the + button.");
            }
        }
        // One-time startup warning: if FLAC level is 1-3 (low compression),
        // warn once. Same pattern as the threads=0 check above.
        if (!flac_level_low_warned && !gui_popup_is_open()) {
            flac_level_low_warned = true;
            // Suppress when FLAC is turned off (same reason as the threads=0
            // warning above): the compression level has no effect with no
            // FLAC encoder running.
            if (app.settings.use_flac && app.settings.flac_level >= 1 && app.settings.flac_level <= 3) {
                gui_popup_info("FLAC level is low (1-3)",
                    "FLAC compression level is in the 1-3 range.\n\n"
                    "Levels 6-8 are recommended for archival RF capture\n"
                    "(best compression ratio, smallest files).\n\n"
                    "Only use level 1-3 if you have explicit resource\n"
                    "constraints (slow CPU, single-core, limited I/O).\n\n"
                    "You can raise it with the + button in Settings.");
            }
        }

        // Handle keyboard shortcuts
        // Popup gets priority for keyboard input
        if (gui_popup_is_open()) {
            if (IsKeyPressed(KEY_ESCAPE)) {
                gui_popup_dismiss();
            } else if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
                // Enter confirms (equivalent to clicking Yes/OK)
                // This is handled by simulating a click - we'll let the popup handle it
                // For now, ESC dismisses and the popup buttons handle confirmation
            }
        } else {
            if (IsKeyPressed(KEY_ESCAPE)) {
                if (app.settings_panel_open) {
                    app.settings_panel_open = false;
                } else {
                    gui_dropdown_close_all();
                }
            }

            if (IsKeyPressed(KEY_SPACE) && !app.settings_panel_open) {
                // Space = play/pause (DAW-style). In playback mode it toggles
                // pause; for live capture devices (no pause concept for a
                // live feed) it starts/stops the capture as before, still
                // refusing to disconnect mid-recording (tearing capture down
                // races the async record finalize and can corrupt the file).
                bool playback_dev =
                    app.selected_device >= 0 &&
                    app.selected_device < app.device_count &&
                    app.devices[app.selected_device].type == DEVICE_TYPE_PLAYBACK;
                if (gui_playback_is_starting(&app)) {
                    gui_app_stop_capture(&app);
                } else if (playback_dev && app.is_capturing && gui_playback_is_running(&app)) {
                    gui_playback_toggle_pause(&app);
                    gui_app_set_status(&app,
                        gui_playback_get_state(&app) == PLAYBACK_STATE_PAUSED
                            ? "Playback paused (space)"
                            : "Playback playing (space)");
                } else if (app.is_capturing) {
                    if (app.is_recording) {
                        gui_app_set_status(&app, "Stop recording before disconnecting (protects the capture file)");
                    } else {
                        gui_app_stop_capture(&app);
                    }
                } else {
                    gui_app_start_capture(&app);
                }
            }

            if (IsKeyPressed(KEY_R) && app.is_capturing && !app.settings_panel_open) {
                if (app.is_recording) {
                    gui_app_stop_recording(&app);
                } else {
                    gui_app_start_recording(&app);
                }
            }
        }

        // --auto-record unattended record cycle (phases above). Progress is
        // logged with [AUTO] prefixes so scripts can verify each step; a
        // failure sets auto_rec_failed and finishes the cycle (nonzero exit).
        if (auto_rec_phase != AUTO_REC_IDLE) {
            double auto_now = GetTime();
            if (auto_rec_phase == AUTO_REC_WAIT_CAPTURE) {
                if (app.is_capturing) {
                    auto_rec_phase = AUTO_REC_SETTLE;
                    auto_rec_phase_start = auto_now;
                    fprintf(stderr, "[AUTO] capture running; settling 2 s before recording\n");
                } else if (auto_now - auto_rec_phase_start > 30.0) {
                    fprintf(stderr, "[AUTO] ERROR: capture did not start within 30 s\n");
                    auto_rec_failed = true;
                    auto_rec_phase = AUTO_REC_FINALIZING;
                }
            } else if (auto_rec_phase == AUTO_REC_SETTLE) {
                if (!app.is_capturing) {
                    auto_rec_phase = AUTO_REC_WAIT_CAPTURE;
                    auto_rec_phase_start = auto_now;
                } else if (auto_now - auto_rec_phase_start >= 2.0) {
                    int rec_rc = gui_app_start_recording(&app);
                    if (rec_rc == RECORD_OK) {
                        auto_rec_phase = AUTO_REC_RECORDING;
                        auto_rec_deadline = auto_now + (double)auto_record_seconds;
                        fprintf(stderr, "[AUTO] recording started (%d s)\n", auto_record_seconds);
                    } else {
                        if (rec_rc == RECORD_PENDING) {
                            fprintf(stderr,
                                    "[AUTO] ERROR: recording blocked by the overwrite confirmation popup; "
                                    "set overwrite_files true (or timestamped auto-names) in the config\n");
                        } else {
                            fprintf(stderr, "[AUTO] ERROR: start recording failed (rc=%d)\n", rec_rc);
                        }
                        auto_rec_failed = true;
                        auto_rec_phase = AUTO_REC_FINALIZING;
                    }
                }
            } else if (auto_rec_phase == AUTO_REC_RECORDING) {
                if (!app.is_recording) {
                    fprintf(stderr, "[AUTO] recording stopped early; finishing\n");
#if defined(__APPLE__)
                    if (gui_cxadc_macos_active()) auto_rec_failed = true;
#endif
                    auto_rec_phase = AUTO_REC_FINALIZING;
                } else if (auto_now >= auto_rec_deadline) {
                    gui_app_stop_recording(&app);
                    fprintf(stderr, "[AUTO] recording stopped after %d s\n", auto_record_seconds);
                    auto_rec_phase = AUTO_REC_FINALIZING;
                }
            } else if (auto_rec_phase == AUTO_REC_FINALIZING) {
                if (!app.is_recording && !gui_record_is_finalizing()) {
                    if (app.is_capturing) {
                        gui_app_stop_capture(&app);
                        fprintf(stderr, "[AUTO] capture stopped\n");
                    }
#if defined(__APPLE__)
                    if (gui_cxadc_macos_failed()) auto_rec_failed = true;
#endif
                    fprintf(stderr, "[AUTO] record cycle complete%s; exiting\n",
                            auto_rec_failed ? " (with errors)" : "");
                    atomic_store(&do_exit, 1);
                    auto_rec_phase = AUTO_REC_IDLE;
                }
            }
        }

        // Apply Android gestures before Clay hit testing. Only a verified
        // single tap triggers controls; a pan/pinch must never click them.
#if defined(__ANDROID__)
        gui_android_viewport_update();
#endif
        // Update Clay mouse state
        Vector2 mouse_pos = gui_ui_get_mouse_position();
        Clay_SetPointerState((Clay_Vector2){ mouse_pos.x, mouse_pos.y },
#if defined(__ANDROID__)
                             gui_android_viewport_click_pressed(MOUSE_LEFT_BUTTON));
#else
                             IsMouseButtonDown(MOUSE_LEFT_BUTTON));
#endif
        Clay_UpdateScrollContainers(true, (Clay_Vector2){
            ui_zoom_result.passthrough_x * 20.0f,
            ui_zoom_result.passthrough_y * 20.0f
        }, dt);

        // Server/Client networking: execute queued control commands on the
        // main thread (server), and apply mirrored peer state (client). Both
        // are no-ops when net_mode == Local.
        gui_net_poll_commands(&app);
        gui_net_poll_mirror(&app);
        // Deferred device re-enumeration after an async net mode transition:
        // the transition worker (gui_net_request_apply_mode) set the flag on
        // completion; the device array is main-thread-owned, so the enumerate
        // runs here, not on the worker.
        if (atomic_exchange(&app.net_reenumerate_pending, false)) {
            gui_app_enumerate_devices(&app);
        }

        // stop-on-dropout requests are posted from capture callbacks and consumed here.
        if (app.is_capturing && atomic_exchange(&app.dropout_stop_requested, false)) {
            gui_dropout_reason_t reason =
                (gui_dropout_reason_t)atomic_exchange(&app.dropout_stop_reason, GUI_DROPOUT_NONE);
            gui_app_stop_capture(&app);
            app.reconnect_pending = false;
            app.reconnect_attempts = 0;
            gui_app_set_status(&app, gui_dropout_reason_status(reason));
            continue;
        }

        // Level autostop: stop RECORDING (not capture) when the RF signal level
        // stays below a configurable normalized level (0.1-0.8) for a
        // configurable duration (tape-end detection). The device stays connected
        // and streaming so the user can start a new recording without
        // reconnecting — unlike the digital-dropout path above, which tears down
        // capture because the stream itself is broken. Gated only by
        // level_autostop_enabled.
        //
        // The threshold is a normalized 0.X magnitude applied to the larger of
        // channel A's positive/negative peak. Peak atomics are NOT on a common
        // scale across backends (12-bit/2048 container for most, 512 for DdD,
        // 128 for 8-bit playback), so the comparison uses the device-aware peak
        // full-scale from gui_app_level_autostop_full_scale(). This is the
        // bit-depth offset correction: 0.4 means 0.4 of the active backend's
        // full scale regardless of 12/10/8-bit depth.
        if (app.is_capturing && app.is_recording && app.settings.level_autostop_enabled) {
            // Parse the configured level (normalized 0.1-0.8) and sustain duration (seconds).
            float level = (float)atof(app.settings.level_autostop_level_str);
            float sustain_s = (float)atof(app.settings.level_autostop_duration_str);
            if (level < 0.1f)      level = 0.1f;
            if (level > 0.8f)      level = 0.8f;
            if (sustain_s < 0.1f)  sustain_s = 0.1f;

            // Peak is stored as unsigned magnitude (0..full_scale). Use the larger of pos/neg on channel A.
            uint16_t peak_pos = (uint16_t)atomic_load(&app.peak_a_pos);
            uint16_t peak_neg = (uint16_t)atomic_load(&app.peak_a_neg);
            uint16_t peak = (peak_pos > peak_neg) ? peak_pos : peak_neg;

            // Threshold counts = level * active backend's peak full scale.
            // (e.g. 0.4 * 2048 = 819 for hsdaoh, 0.4 * 512 = 205 for DdD,
            //  0.4 * 128 = 51 for 8-bit playback.)
            float peak_full_scale_f = (float)gui_app_level_autostop_full_scale(&app);
            uint16_t threshold = (uint16_t)(level * peak_full_scale_f);
            if (threshold < 1) threshold = 1;

            if (!app.low_signal_armed) {
                // Arm once a real signal level is seen above the threshold.
                if (peak >= threshold) {
                    app.low_signal_armed = true;
                    app.low_signal_time = 0.0f;
                }
            } else {
                if (peak < threshold) {
                    app.low_signal_time += dt;
                    if (app.low_signal_time >= sustain_s) {
                        // Stop recording only — keep the capture/device
                        // connection alive (tape end != device disconnect).
                        // Reset low_signal state so a future recording re-arms
                        // only after a real signal is seen again.
                        gui_record_log_capture_event(&app, "INFO",
                            "Level autostop: sustained low signal (tape end); stopping recording",
                            GUI_ERROR_CLASS_NONE, 0);
                        gui_app_stop_recording(&app);
                        app.low_signal_time = 0.0f;
                        app.low_signal_armed = false;
                        gui_app_set_status(&app, gui_dropout_reason_status(GUI_DROPOUT_LOW_SIGNAL));
                        continue;
                    }
                } else {
                    // Signal recovered - reset timer but stay armed.
                    app.low_signal_time = 0.0f;
                }
            }
        }

        // Auto-reconnect logic
        if (app.auto_reconnect_enabled) {
            double now = GetTime();

            // Detect connection loss via callback timeout (no data for 2+ seconds).
            // Keep parser-state ownership scoped to capture lifecycle boundaries
            // in gui_capture.c (stop/start paths), not timeout polling logic here.
            // Grace period: suppress timeout for the first 5 seconds after capture
            // starts so the MS2130 has time to complete its initial HDMI/USB sync
            // and deliver the first data callback (first-connect on Windows can
            // take 3-4 seconds before any data arrives).
            // Client ingest mode is excluded: its data source is the network pump,
            // not a local hsdaoh device, so the hsdaoh-callback watchdog does not
            // apply (and must not tear down the ingest or re-enumerate devices).
            if (app.is_capturing && !gui_net_is_client(&app) && (now - app.capture_start_time) > 5.0
                && gui_capture_device_timeout(&app, 2000)) {
                // Device was disconnected unexpectedly - clean up properly
                fprintf(stderr, "[GUI] Device timeout detected, disconnecting...\n");
#if defined(__ANDROID__)
                /* Async stop: hsdaoh_stop_stream/close on the wrapped Android
                 * fd can hang joining libusb/libuvc threads. Blocking here on
                 * the render thread was the post-connect freeze: watchdog
                 * fired ~7s after connect (5s grace + 2s no-data) and stalled
                 * the UI inside stop_capture. Only fire once per episode
                 * (gui_app_capture_busy gate). */
                if (!gui_app_capture_busy()) {
                    gui_set_reconnect_target_from_selected(&app, &reconnect_target);
                    gui_app_stop_capture_async(&app);
                    gui_app_clear_display(&app);
                    app.reconnect_pending = true;
                    app.reconnect_attempt_time = now;
                    app.reconnect_attempts = 0;
                    gui_app_set_status(&app, "Connection lost (no data). Reconnecting...");
                }
#else
                gui_set_reconnect_target_from_selected(&app, &reconnect_target);
                gui_app_stop_capture(&app);
                gui_app_clear_display(&app);
                app.reconnect_pending = true;
                app.reconnect_attempt_time = now;
                app.reconnect_attempts = 0;
                gui_app_set_status(&app, "Connection lost. Reconnecting...");
#endif
            }

            // Attempt reconnection if pending
            if (app.reconnect_pending && !app.is_capturing &&
                !gui_playback_is_starting(&app)) {
#if defined(__ANDROID__)
                /* If a USB permission request is mid-flight, the per-frame
                 * poll in gui_handle_interactions completes the connect; just
                 * wait and don't burn a reconnect attempt. */
                extern int android_permission_pending(void);
                extern int android_usb_has_fd(void);
                extern int android_request_usb_permission_async(void);
                extern void android_usb_clear_fd(void);
                if (android_permission_pending() || gui_app_capture_busy()) {
                    /* wait for the async permission poll / in-flight start-stop
                     * worker to finish before attempting reconnect */
                } else
#endif
                {
                double retry_delay = (app.reconnect_attempts < 3) ? 1.0 : 3.0;  // 1s for first 3, then 3s
                if (now - app.reconnect_attempt_time >= retry_delay) {
                    app.reconnect_attempt_time = now;
                    app.reconnect_attempts++;

                    // Re-enumerate devices in case device was reconnected
                    gui_app_enumerate_devices(&app);

                    if (app.device_count > 0) {
                        if (reconnect_target.valid) {
                            int reconnect_dev = gui_find_reconnect_device(&app, &reconnect_target);
                            if (reconnect_dev < 0) {
                                if (reconnect_target.type == DEVICE_TYPE_HSDAOH) {
                                    char status_waiting[128];
                                    snprintf(status_waiting, sizeof(status_waiting),
                                             "Waiting for MS2130 hsdaoh device (attempt %d)", app.reconnect_attempts);
                                    gui_app_set_status(&app, status_waiting);
                                } else {
                                    char status_waiting[128];
                                    snprintf(status_waiting, sizeof(status_waiting),
                                             "Waiting for selected device (attempt %d)", app.reconnect_attempts);
                                    gui_app_set_status(&app, status_waiting);
                                }
                                continue;
                            }
                            app.selected_device = reconnect_dev;
                        }
                        char status_buf[128];
                        snprintf(status_buf, sizeof(status_buf), "Reconnecting (attempt %d)...", app.reconnect_attempts);
                        gui_app_set_status(&app, status_buf);
#if defined(__ANDROID__)
                        /* Software Test Signal must bypass USB permissions
                         * even during watchdog/auto reconnect. Real USB
                         * capture still waits for its Android-granted fd.
                         * Start on a worker to keep the render loop alive. */
                        if (app.selected_device >= 0 && app.selected_device < app.device_count &&
                            app.devices[app.selected_device].type == DEVICE_TYPE_PLAYBACK) {
                            (void)gui_app_start_capture(&app);
                        } else if ((app.selected_device >= 0 &&
                                    app.selected_device < app.device_count &&
                                    app.devices[app.selected_device].type == DEVICE_TYPE_SIMULATED) ||
                                   android_usb_has_fd()) {
                            gui_app_start_capture_async(&app);
                            gui_set_reconnect_target_from_selected(&app, &reconnect_target);
                            app.reconnect_pending = false;
                            app.reconnect_attempts = 0;
                            gui_app_set_status(&app, "Reconnecting...");
                        } else {
                            android_usb_clear_fd();
                            android_request_usb_permission_async();
                            gui_app_set_status(&app, "Requesting USB permission (reconnect)...");
                        }
#else
                        int reconnect_rc = gui_app_start_capture(&app);
                        if (reconnect_rc == 0 && !gui_playback_is_starting(&app)) {
                            gui_set_reconnect_target_from_selected(&app, &reconnect_target);
                            app.reconnect_pending = false;
                            app.reconnect_attempts = 0;
                            gui_app_set_status(&app, "Reconnected");
                        } else if (reconnect_rc == -3 || gui_status_is_permission_denied(&app)) {
                            app.reconnect_pending = false;
                        }
#endif
                    } else {
                        char status_buf_no_dev[128];
                        snprintf(status_buf_no_dev, sizeof(status_buf_no_dev), "No device found (attempt %d)", app.reconnect_attempts);
                        gui_app_set_status(&app, status_buf_no_dev);
                    }
                }
                }
            }
        }

        // Update VU meters
        gui_app_update_vu_meters(&app, dt);

        // Pump audio playback monitoring (system output)
        gui_audio_update_playback(&app);

        // Note: Display processing now handled by display thread via panel_process_all()
        // Each panel type (waveform, histogram, FFT) receives raw samples via vtable->process()

        // Build UI layout
        Clay_RenderCommandArray render_commands = app_build_render_commands(&app);

        // Handle Clay interactions
        gui_handle_interactions(&app);
        gui_ui_sync_android_keyboard_state();
        if (!was_capturing && app.is_capturing) {
            gui_set_reconnect_target_from_selected(&app, &reconnect_target);
        }

        // Handle panel scroll events (e.g., waveform/FFT zoom)
        float wheel = fabsf(ui_zoom_result.passthrough_x) >
                              fabsf(ui_zoom_result.passthrough_y)
                          ? ui_zoom_result.passthrough_x
                          : ui_zoom_result.passthrough_y;
        if (wheel != 0.0f) {
            panel_handle_all_scrolls(&app, wheel);
        }

        // Render
        BeginDrawing();
#if defined(__ANDROID__)
        const bool android_canvas_active = gui_android_viewport_begin_canvas();
#endif
        ClearBackground(COLOR_BG);

        // Render Clay UI (custom elements are handled via CLAY_RENDER_COMMAND_TYPE_CUSTOM)
        Clay_Raylib_Render(render_commands, fonts);

        // Draw FPS in debug mode
        #ifdef DEBUG
        DrawFPS(10, 10);
        #endif

#if defined(__APPLE__)
        gui_cxadc_macos_ui_draw(&app);
#endif
#if defined(__ANDROID__)
        gui_android_viewport_present_canvas(android_canvas_active);
#endif
        EndDrawing();
    }

    // Restore the original window proc (Windows modal move/size driver)
    gui_win32_modal_shutdown();

    // Cleanup
    if (app.is_recording) {
        gui_app_stop_recording(&app);
    }
    if (app.is_capturing) {
        gui_app_stop_capture(&app);
    }

    // Save settings before cleanup
    gui_settings_save(&app.settings);
    
    gui_app_cleanup(&app);
    free(clay_memory);

    // Unload fonts if we loaded TTFs (not the default font)
    gui_font_unload();
    if (fonts[0].texture.id != 0 && fonts[0].texture.id != GetFontDefault().texture.id) {
        UnloadFont(fonts[0]);
    }
    if (fonts[1].texture.id != 0 && fonts[1].texture.id != GetFontDefault().texture.id) {
        UnloadFont(fonts[1]);
    }

#if defined(__ANDROID__)
    gui_android_viewport_shutdown();
#endif
    CloseWindow();

    // Nonzero exit when the unattended --auto-record cycle failed (0 for
    // normal interactive exits and successful cycles).
    return auto_rec_failed ? 1 : 0;
}
