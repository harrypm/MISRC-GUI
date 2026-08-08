/*
 * MISRC GUI - Graphical Capture Interface
 *
 * Real-time waveform display and capture control using raylib + Clay UI
 *
 * Copyright (C) 2023-2026 Harry Munday, AlessandroAU, Stefan O, Vrunk11, machcnz
 * Licensed under GNU GPL v3 or later
 */

// Clay UI library (header-only, implementation here)
#define CLAY_IMPLEMENTATION
#include "../ui/clay.h"

#include "raylib.h"
#include "../assets/inter_font_data.h"
#include "../assets/misrc_icon_png_data.h"
#include "../assets/space_mono_font_data.h"

#include "gui_app.h"
#include "../ui/gui_ui.h"
#include "../visualization/gui_text.h"
#include "../input/gui_capture.h"
#include "../processing/gui_extract.h"
#include "../visualization/gui_panel.h"
#include "../ui/gui_dropdown.h"
#include "../ui/gui_popup.h"
#include "../output/gui_record.h"
#include "../output/gui_audio.h"
#include "../../common/threading.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
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
static Font fonts[FONT_COUNT];

// Clay error handler
void clay_error_handler(Clay_ErrorData error) {
    fprintf(stderr, "Clay Error: %s\n", error.errorText.chars);
}
static void print_usage(const char *program_name) {
    fprintf(stdout,
            "MISRC GUI %s\n"
            "Usage:\n"
            "  %s [--help] [--version] [--smoke-test] [--debug-view]\n"
            "\n"
            "No arguments launch the GUI.\n"
            "Use --debug-view to enable verbose runtime logs.\n",
            MIRSC_TOOLS_VERSION,
            program_name ? program_name : "misrc_gui");
}
static int gui_layout_width(void) {
#if defined(__APPLE__)
    int width = GetScreenWidth();
#else
    int width = GetRenderWidth();
    if (width <= 0) {
        width = GetScreenWidth();
    }
#endif
    return (width > 0) ? width : 1;
}
static int gui_layout_height(void) {
#if defined(__APPLE__)
    int height = GetScreenHeight();
#else
    int height = GetRenderHeight();
    if (height <= 0) {
        height = GetScreenHeight();
    }
#endif
    return (height > 0) ? height : 1;
}
static bool gui_status_is_permission_denied(const gui_app_t *app) {
    if (!app) return false;
    return strstr(app->status_message, "Permission denied") != NULL;
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
            return "Capture stopped: sustained low/no signal (tape end)";
        case GUI_DROPOUT_NONE:
        default:
            return "Capture stopped: dropout detected";
    }
}
typedef struct {
    bool valid;
    device_type_t type;
    int index;
    char name[64];
    char serial[64];
} gui_reconnect_target_t;
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
    if (!app) return;
    if (app->selected_device < 0 || app->selected_device >= app->device_count) return;
    const device_info_t *dev = &app->devices[app->selected_device];
    target->valid = true;
    target->type = dev->type;
    target->index = dev->index;
    snprintf(target->name, sizeof(target->name), "%s", dev->name);
    snprintf(target->serial, sizeof(target->serial), "%s", dev->serial);
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
        } else {
            return i;
        }
    }
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
    if (geteuid() == 0) {
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

int main(int argc, char **argv) {
    bool debug_view = false;
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--help") == 0) || (strcmp(argv[i], "-h") == 0)) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--debug-view") == 0) {
            debug_view = true;
        }
        if (strcmp(argv[i], "--version") == 0) {
            fprintf(stdout, "%s\n", MIRSC_TOOLS_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "--smoke-test") == 0) {
            return 0;
        }
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

    // Load persistent settings (includes desktop path defaults)
    gui_settings_load(&app.settings);

    // Capture limit should not persist across relaunches.
    app.settings.capture_limit_seconds = 0;

    // Initialize raylib window
    unsigned int window_flags = FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT;
    SetConfigFlags(window_flags);
    // Keep defaults usable while fitting common laptop screens.
    const int default_window_width = 1425;
    const int default_window_height = 720;
    const int min_window_width = 1040;
    const int min_window_height = 650;
    char window_title[128];
    snprintf(window_title, sizeof(window_title), "MISRC Capture %s", MIRSC_TOOLS_VERSION);
    InitWindow(default_window_width, default_window_height, window_title);
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
    fonts[0] = LoadFontFromMemory(".ttf", inter_font_data, inter_font_data_size, 32, NULL, 256);
    if (fonts[0].texture.id == 0) {
        fprintf(stderr, "Error: Failed to load embedded Inter font data\n");
        CloseWindow();
        return 1;
    }
    SetTextureFilter(fonts[0].texture, TEXTURE_FILTER_BILINEAR);

    // Load embedded Space Mono font directly from memory (SIL Open Font License)
    // Font data is embedded as a C array for complete portability
    fonts[1] = LoadFontFromMemory(".ttf", space_mono_font_data, space_mono_font_data_size, 32, NULL, 256);
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
    Clay_Initialize(clay_arena, (Clay_Dimensions){ (float)gui_layout_width(), (float)gui_layout_height() },
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
            int cxadc_idx = gui_find_first_device_of_type(&app, DEVICE_TYPE_CXADC);
            if (cxadc_idx >= 0) {
                app.selected_device = cxadc_idx;
            }
        }
    }

    // Enable auto-reconnect by default
    app.auto_reconnect_enabled = true;

    // Do not auto-start capture on launch.
    // Keep mode controls toggleable until the user explicitly clicks Connect.
    if (app.device_count > 0) {
        int hs_idx = gui_find_first_device_of_type(&app, DEVICE_TYPE_HSDAOH);
        if (hs_idx >= 0) {
            app.selected_device = hs_idx;
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
    } else {
        gui_app_set_status(&app, "No devices found. Connect a device and retry.");
    }
    int last_layout_width = -1;
    int last_layout_height = -1;
    bool recording_fps_throttle = false;
#if defined(__APPLE__) && (defined(__arm64__) || defined(__aarch64__))
    double last_thread_promotion_time = 0.0;
    const double thread_promotion_interval_s = 0.25;
#endif

    // Main loop
    while (!WindowShouldClose() && !atomic_load(&do_exit)) {
        bool was_capturing = app.is_capturing;
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
        int current_layout_width = gui_layout_width();
        int current_layout_height = gui_layout_height();
        if (current_layout_width != last_layout_width || current_layout_height != last_layout_height) {
            Clay_SetLayoutDimensions((Clay_Dimensions){
                (float)current_layout_width, (float)current_layout_height
            });
            last_layout_width = current_layout_width;
            last_layout_height = current_layout_height;
        }

        // Check for pending popup result (for async confirmations like file overwrite)
        gui_record_check_popup(&app);

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
                if (app.is_capturing) {
                    gui_app_stop_capture(&app);
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

        // Update Clay mouse state
        Vector2 mouse_pos = GetMousePosition();
        Clay_SetPointerState((Clay_Vector2){ mouse_pos.x, mouse_pos.y },
                             IsMouseButtonDown(MOUSE_LEFT_BUTTON));
        Clay_UpdateScrollContainers(true, (Clay_Vector2){
            GetMouseWheelMoveV().x * 20.0f,
            GetMouseWheelMoveV().y * 20.0f
        }, dt);

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

        // Level autostop: stop capture when the RF signal level stays below a
        // configurable percentage for a configurable duration (tape-end detection).
        // This is independent from the digital dropout (frame error/missed frame)
        // logic above, which is unchanged. Gated only by level_autostop_enabled.
        if (app.is_capturing && app.is_recording && app.settings.level_autostop_enabled) {
            // Parse the configured level (percent) and sustain duration (seconds).
            float level_pct = (float)atof(app.settings.level_autostop_level_str);
            float sustain_s = (float)atof(app.settings.level_autostop_duration_str);
            if (level_pct < 1.0f)   level_pct = 1.0f;
            if (level_pct > 99.0f)  level_pct = 99.0f;
            if (sustain_s < 0.1f)   sustain_s = 0.1f;

            // Peak is stored as 0-2047 unsigned. Use the larger of pos/neg on channel A.
            uint16_t peak_pos = (uint16_t)atomic_load(&app.peak_a_pos);
            uint16_t peak_neg = (uint16_t)atomic_load(&app.peak_a_neg);
            uint16_t peak = (peak_pos > peak_neg) ? peak_pos : peak_neg;

            // Threshold counts = level% of 2048 full scale.
            uint16_t threshold = (uint16_t)((level_pct / 100.0f) * 2048.0f);
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
                        gui_app_stop_capture(&app);
                        app.reconnect_pending = false;
                        app.reconnect_attempts = 0;
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
            if (app.is_capturing && gui_capture_device_timeout(&app, 2000)) {
                // Device was disconnected unexpectedly - clean up properly
                fprintf(stderr, "[GUI] Device timeout detected, disconnecting...\n");
                gui_set_reconnect_target_from_selected(&app, &reconnect_target);
                gui_app_stop_capture(&app);
                gui_app_clear_display(&app);
                app.reconnect_pending = true;
                app.reconnect_attempt_time = now;
                app.reconnect_attempts = 0;
                gui_app_set_status(&app, "Connection lost. Reconnecting...");
            }

            // Attempt reconnection if pending
            if (app.reconnect_pending && !app.is_capturing) {
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
                        int reconnect_rc = 0;
                        char status_buf[128];
                        snprintf(status_buf, sizeof(status_buf), "Reconnecting (attempt %d)...", app.reconnect_attempts);
                        gui_app_set_status(&app, status_buf);
                        reconnect_rc = gui_app_start_capture(&app);
                        if (reconnect_rc == 0) {
                            gui_set_reconnect_target_from_selected(&app, &reconnect_target);
                            app.reconnect_pending = false;
                            app.reconnect_attempts = 0;
                            gui_app_set_status(&app, "Reconnected");
                        } else if (reconnect_rc == -3 || gui_status_is_permission_denied(&app)) {
                            app.reconnect_pending = false;
                        }
                    } else {
                        char status_buf_no_dev[128];
                        snprintf(status_buf_no_dev, sizeof(status_buf_no_dev), "No device found (attempt %d)", app.reconnect_attempts);
                        gui_app_set_status(&app, status_buf_no_dev);
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
        Clay_BeginLayout();
        gui_render_layout(&app);
        Clay_RenderCommandArray render_commands = Clay_EndLayout();

        // Handle Clay interactions
        gui_handle_interactions(&app);
        if (!was_capturing && app.is_capturing) {
            gui_set_reconnect_target_from_selected(&app, &reconnect_target);
        }

        // Handle panel scroll events (e.g., waveform/FFT zoom)
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            panel_handle_all_scrolls(&app, wheel);
        }

        // Render
        BeginDrawing();

        // Reset cursor to default at start of frame
        // (Panels will set crosshair cursor when hovering/dragging via their render functions)
        SetMouseCursor(MOUSE_CURSOR_DEFAULT);
        ClearBackground(COLOR_BG);

        // Render Clay UI (custom elements are handled via CLAY_RENDER_COMMAND_TYPE_CUSTOM)
        Clay_Raylib_Render(render_commands, fonts);

        // Draw FPS in debug mode
        #ifdef DEBUG
        DrawFPS(10, 10);
        #endif

        EndDrawing();
    }

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
    if (fonts[0].texture.id != 0 && fonts[0].texture.id != GetFontDefault().texture.id) {
        UnloadFont(fonts[0]);
    }
    if (fonts[1].texture.id != 0 && fonts[1].texture.id != GetFontDefault().texture.id) {
        UnloadFont(fonts[1]);
    }

    CloseWindow();

    return 0;
}
