/*
 * MISRC GUI - Pop-out panels (see gui_popout.h).
 */
#include "gui_popout.h"

#include "../core/gui_app.h"
#include "../ui/gui_ui.h"
#include "gui_panel.h"
#include "panel_interface.h"
#include "gui_phosphor_rt.h"
#include "gui_text.h"

#include "rlgl.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(__ANDROID__) && !defined(MISRC_POPOUT_DISABLED)
#define POPOUT_ENABLED 1
#else
#define POPOUT_ENABLED 0
#endif

//-----------------------------------------------------------------------------
// Launch queue + settings text (also without pop-out support)
//-----------------------------------------------------------------------------

static bool s_launch_open[GUI_POPOUT_SLOTS];
static int s_fs_slot = -1;   // full-screen slot (builds without pop-out windows)
static bool s_launch_saved[GUI_POPOUT_SLOTS];   // restored at launch: use the saved geometry

bool gui_popout_apply_launch_arg(const char *arg) {
    static const char *usage = "--popout: expected <a|b>:<left|right>[,<a|b>:<left|right>...]";
    if (!arg || !arg[0]) { fprintf(stderr, "%s\n", usage); return false; }
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", arg);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        int ch = (tok[0] == 'a' || tok[0] == 'A') ? 0 : (tok[0] == 'b' || tok[0] == 'B') ? 1 : -1;
        int side = -1;
        if (ch >= 0 && tok[1] == ':') {
            if (strcmp(tok + 2, "left") == 0) side = 0;
            else if (strcmp(tok + 2, "right") == 0) side = 1;
        }
        if (side < 0) { fprintf(stderr, "%s (got '%s')\n", usage, arg); return false; }
        s_launch_open[ch * 2 + side] = true;
    }
    fprintf(stderr, "[AUTO] --popout: %s\n", arg);
    return true;
}


//-----------------------------------------------------------------------------
// In-app full-screen panels: the mode on Android / without GLFW, and on
// desktop with MISRC_PANEL_FULLSCREEN=1 (tests the same code path).
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// No pop-out windows (Android / no GLFW): the corner button makes the panel
// full screen inside the app instead (harrypm 2026-11-26: "make panel full
// screen instead of pop out for mobile").
//-----------------------------------------------------------------------------

static Rectangle s_fs_grid_btn[GUI_POPOUT_SLOTS];
static Rectangle s_fs_exit_btn;

// Touch-sized corner button.
#define FS_BTN_W 40.0f
#define FS_BTN_H 32.0f

static void fs_draw_corner_glyph(Rectangle r, bool enter, bool hover) {
    DrawRectangleRounded(r, 0.2f, 4, hover ? COLOR_BUTTON_HOVER : (Color){ 65, 65, 75, 190 });
    const Rectangle g = { r.x + 10, r.y + 8, r.width - 20, r.height - 16 };
    const Color c = COLOR_TEXT;
    const float k = 6.0f, t = 2.0f;
    if (enter) {   // corner brackets pointing out = full screen
        DrawLineEx((Vector2){ g.x, g.y }, (Vector2){ g.x + k, g.y }, t, c);
        DrawLineEx((Vector2){ g.x, g.y }, (Vector2){ g.x, g.y + k }, t, c);
        DrawLineEx((Vector2){ g.x + g.width, g.y }, (Vector2){ g.x + g.width - k, g.y }, t, c);
        DrawLineEx((Vector2){ g.x + g.width, g.y }, (Vector2){ g.x + g.width, g.y + k }, t, c);
        DrawLineEx((Vector2){ g.x, g.y + g.height }, (Vector2){ g.x + k, g.y + g.height }, t, c);
        DrawLineEx((Vector2){ g.x, g.y + g.height }, (Vector2){ g.x, g.y + g.height - k }, t, c);
        DrawLineEx((Vector2){ g.x + g.width, g.y + g.height }, (Vector2){ g.x + g.width - k, g.y + g.height }, t, c);
        DrawLineEx((Vector2){ g.x + g.width, g.y + g.height }, (Vector2){ g.x + g.width, g.y + g.height - k }, t, c);
    } else {       // a small framed box = back to the grid
        DrawRectangleLinesEx(g, t, c);
    }
}

static bool fs_is_open(int channel, int side) {
    return s_fs_slot >= 0 && s_fs_slot == channel * 2 + side;
}
static bool fs_open(struct gui_app *app, int channel, int side) {
    (void)app;
    const int s = channel * 2 + side;
    if (s < 0 || s >= GUI_POPOUT_SLOTS) return false;
    s_fs_slot = s;
    fprintf(stderr, "[POPOUT] slot %d full screen\n", s);
    return true;
}
static void fs_close(int channel, int side) {
    if (fs_is_open(channel, side)) {
        s_fs_slot = -1;
        fprintf(stderr, "[POPOUT] slot %d back to the grid\n", channel * 2 + side);
    }
}
static void fs_frame(struct gui_app *app) {
    if (!app) return;
    for (int s = 0; s < GUI_POPOUT_SLOTS; s++) {
        if (s_launch_open[s]) {   // --popout: first requested slot goes full screen
            s_launch_open[s] = false;
            if (s_fs_slot < 0) fs_open(app, s / 2, s % 2);
        }
    }
}
static void fs_restore(struct gui_app *app) { (void)app; }
static void fs_capture_settings(struct gui_app *app) { (void)app; }
static void fs_shutdown(void) { s_fs_slot = -1; }

static void fs_draw_button(int channel, int side, Rectangle b) {
    const int s = channel * 2 + side;
    if (s < 0 || s >= GUI_POPOUT_SLOTS) return;
    if (b.width < 2.0f * FS_BTN_W || b.height < 2.0f * FS_BTN_H) { s_fs_grid_btn[s] = (Rectangle){ 0 }; return; }
    s_fs_grid_btn[s] = (Rectangle){ b.x + b.width - FS_BTN_W - 6.0f, b.y + b.height - FS_BTN_H - 6.0f, FS_BTN_W, FS_BTN_H };
    fs_draw_corner_glyph(s_fs_grid_btn[s], true, CheckCollisionPointRec(gui_ui_get_mouse_position(), s_fs_grid_btn[s]));
}
static bool fs_handle_grid_click(struct gui_app *app, int channel, int side, Vector2 click, Rectangle b) {
    (void)b;
    const int s = channel * 2 + side;
    if (s < 0 || s >= GUI_POPOUT_SLOTS || s_fs_slot >= 0) return false;
    if (s_fs_grid_btn[s].width > 0 && CheckCollisionPointRec(click, s_fs_grid_btn[s])) {
        fs_open(app, channel, side);
        return true;
    }
    return false;
}
static void fs_draw_placeholder(int channel, int side, Rectangle b, const char *view_name) {
    // Hidden under the full-screen panel; kept plain.
    (void)channel; (void)side; (void)view_name;
    DrawRectangleRec(b, COLOR_PANEL_BG);
}

#if POPOUT_ENABLED

//-----------------------------------------------------------------------------
// GLFW 3 (exported by raylib's bundled GLFW, or a system glfw3; the API is
// stable across 3.x - declared here so no GLFW headers are needed).
//-----------------------------------------------------------------------------

typedef struct GLFWwindow GLFWwindow;
typedef struct GLFWmonitor GLFWmonitor;
typedef struct { int width, height, redBits, greenBits, blueBits, refreshRate; } GLFWvidmode;
typedef void (*GLFWscrollfun)(GLFWwindow *, double, double);
GLFWwindow *glfwGetCurrentContext(void);
void glfwDefaultWindowHints(void);
void glfwWindowHint(int hint, int value);
GLFWwindow *glfwCreateWindow(int width, int height, const char *title, GLFWmonitor *monitor, GLFWwindow *share);
void glfwDestroyWindow(GLFWwindow *window);
void glfwMakeContextCurrent(GLFWwindow *window);
void glfwSwapInterval(int interval);
void glfwSwapBuffers(GLFWwindow *window);
void glfwGetFramebufferSize(GLFWwindow *window, int *width, int *height);
void glfwGetWindowSize(GLFWwindow *window, int *width, int *height);
void glfwGetWindowPos(GLFWwindow *window, int *x, int *y);
void glfwSetWindowPos(GLFWwindow *window, int x, int y);
void glfwShowWindow(GLFWwindow *window);
void glfwGetCursorPos(GLFWwindow *window, double *x, double *y);
int glfwGetMouseButton(GLFWwindow *window, int button);
int glfwGetKey(GLFWwindow *window, int key);
int glfwWindowShouldClose(GLFWwindow *window);
GLFWscrollfun glfwSetScrollCallback(GLFWwindow *window, GLFWscrollfun callback);
void glfwSetWindowUserPointer(GLFWwindow *window, void *pointer);
void *glfwGetWindowUserPointer(GLFWwindow *window);
GLFWmonitor **glfwGetMonitors(int *count);
void glfwGetMonitorPos(GLFWmonitor *monitor, int *x, int *y);
const GLFWvidmode *glfwGetVideoMode(GLFWmonitor *monitor);
void glfwSetWindowMonitor(GLFWwindow *window, GLFWmonitor *monitor, int x, int y, int w, int h, int refresh);
GLFWmonitor *glfwGetWindowMonitor(GLFWwindow *window);
int glfwGetWindowAttrib(GLFWwindow *window, int attrib);

#define P_GLFW_PRESS 1
#define P_GLFW_MOUSE_BUTTON_LEFT 0
#define P_GLFW_KEY_ESCAPE 256
#define P_GLFW_KEY_F11 300
#define P_GLFW_FOCUSED 0x00020001
#define P_GLFW_RESIZABLE 0x00020003
#define P_GLFW_VISIBLE 0x00020004
#define P_GLFW_CONTEXT_VERSION_MAJOR 0x00022002
#define P_GLFW_CONTEXT_VERSION_MINOR 0x00022003
#define P_GLFW_OPENGL_FORWARD_COMPAT 0x00022006
#define P_GLFW_OPENGL_PROFILE 0x00022008
#define P_GLFW_OPENGL_CORE_PROFILE 0x00032001
#define P_GLFW_DONT_CARE (-1)

//-----------------------------------------------------------------------------
// State
//-----------------------------------------------------------------------------

typedef struct {
    bool open;
    GLFWwindow *win;
    RenderTexture2D rt;
    int rt_w, rt_h;
    unsigned int vao, vbo;      // vao: this window's context (VAOs are not shared)
    unsigned buttons_down;      // bit 0 left, 1 right, 2 middle (as raylib)
    bool f11_down, esc_down;
    double wheel;               // scroll callback accumulation
    bool fullscreen;
    int wx, wy, ww, wh;         // windowed geometry (kept while fullscreen)
    bool have_geom;
    Rectangle fs_btn;           // fullscreen button (logical, panel space)
} popout_t;

static popout_t s_po[GUI_POPOUT_SLOTS];
static GLFWwindow *s_main;
static Shader s_blit;
static bool s_blit_ok;
static int s_blit_tex_loc = -1;
static Rectangle s_grid_btn[GUI_POPOUT_SLOTS];

static bool popout_fs_mode(void) {
    static int mode = -1;
    if (mode < 0) mode = getenv("MISRC_PANEL_FULLSCREEN") != NULL;
    return mode != 0;
}

bool gui_popout_available(void) { return !popout_fs_mode(); }

bool gui_popout_is_open(int channel, int side) {
    if (popout_fs_mode()) return fs_is_open(channel, side);
    const int s = channel * 2 + side;
    return s >= 0 && s < GUI_POPOUT_SLOTS && s_po[s].open;
}

static void popout_scroll_cb(GLFWwindow *win, double dx, double dy) {
    popout_t *p = glfwGetWindowUserPointer(win);
    if (p) p->wheel += fabs(dx) > fabs(dy) ? dx : dy;
}

static bool popout_blit_init(void) {
    if (s_blit_ok) return true;
    const bool gl33 = rlGetVersion() >= RL_OPENGL_33;
    const char *vs33 =
        "#version 330\n"
        "in vec3 vertexPosition; in vec2 vertexTexCoord; out vec2 uv;\n"
        "void main(){ uv = vertexTexCoord; gl_Position = vec4(vertexPosition.xy, 0.0, 1.0); }\n";
    const char *fs33 =
        "#version 330\n"
        "uniform sampler2D texture0; in vec2 uv; out vec4 finalColor;\n"
        "void main(){ finalColor = vec4(texture(texture0, uv).rgb, 1.0); }\n";
    const char *vs12 =
        "#version 120\n"
        "attribute vec3 vertexPosition; attribute vec2 vertexTexCoord; varying vec2 uv;\n"
        "void main(){ uv = vertexTexCoord; gl_Position = vec4(vertexPosition.xy, 0.0, 1.0); }\n";
    const char *fs12 =
        "#version 120\n"
        "uniform sampler2D texture0; varying vec2 uv;\n"
        "void main(){ gl_FragColor = vec4(texture2D(texture0, uv).rgb, 1.0); }\n";
    s_blit = LoadShaderFromMemory(gl33 ? vs33 : vs12, gl33 ? fs33 : fs12);
    s_blit_ok = IsShaderValid(s_blit);
    if (s_blit_ok) s_blit_tex_loc = GetShaderLocation(s_blit, "texture0");
    else fprintf(stderr, "[POPOUT] blit shader failed; pop-outs disabled\n");
    return s_blit_ok;
}

static const char *popout_slot_name(int slot) {
    static const char *names[GUI_POPOUT_SLOTS] = { "A left", "A right", "B left", "B right" };
    return names[slot];
}

static void popout_destroy(popout_t *p) {
    if (!p->win) { p->open = false; return; }
    if (!p->fullscreen) {
        glfwGetWindowPos(p->win, &p->wx, &p->wy);
        glfwGetWindowSize(p->win, &p->ww, &p->wh);
        p->have_geom = true;
    }
    glfwMakeContextCurrent(p->win);
    if (p->vao) rlUnloadVertexArray(p->vao);
    glfwMakeContextCurrent(s_main);
    if (p->vbo) rlUnloadVertexBuffer(p->vbo);
    p->vao = p->vbo = 0;
    if (p->rt.id) UnloadRenderTexture(p->rt);
    p->rt = (RenderTexture2D){ 0 };
    p->rt_w = p->rt_h = 0;
    glfwDestroyWindow(p->win);
    p->win = NULL;
    p->open = false;
    p->wheel = 0.0;
}

static GLFWmonitor *popout_monitor_for(popout_t *p) {
    int count = 0;
    GLFWmonitor **mons = glfwGetMonitors(&count);
    if (!mons || count <= 0) return NULL;
    int x, y, w, h;
    glfwGetWindowPos(p->win, &x, &y);
    glfwGetWindowSize(p->win, &w, &h);
    const int cx = x + w / 2, cy = y + h / 2;
    for (int i = 0; i < count; i++) {
        int mx, my;
        glfwGetMonitorPos(mons[i], &mx, &my);
        const GLFWvidmode *m = glfwGetVideoMode(mons[i]);
        if (m && cx >= mx && cx < mx + m->width && cy >= my && cy < my + m->height) return mons[i];
    }
    return mons[0];
}

static void popout_set_fullscreen(popout_t *p, bool on) {
    if (!p->win || p->fullscreen == on) return;
    if (on) {
        glfwGetWindowPos(p->win, &p->wx, &p->wy);
        glfwGetWindowSize(p->win, &p->ww, &p->wh);
        p->have_geom = true;
        GLFWmonitor *mon = popout_monitor_for(p);
        const GLFWvidmode *m = mon ? glfwGetVideoMode(mon) : NULL;
        if (!mon || !m) return;
        glfwSetWindowMonitor(p->win, mon, 0, 0, m->width, m->height, m->refreshRate);
    } else {
        glfwSetWindowMonitor(p->win, NULL, p->wx, p->wy, p->ww > 0 ? p->ww : 960,
                             p->wh > 0 ? p->wh : 540, P_GLFW_DONT_CARE);
    }
    p->fullscreen = on;
}

static bool popout_open_impl(gui_app_t *app, int channel, int side, bool use_saved);

bool gui_popout_open(gui_app_t *app, int channel, int side) {
    if (popout_fs_mode()) return fs_open(app, channel, side);
    return popout_open_impl(app, channel, side, false);
}

// A new pop-out lands on the main window, exactly over the panel it came
// from (same screen, position and size; harrypm 2026-11-26 "make pop out
// land on main window position"). Restored pop-outs use their saved spot.
static bool popout_open_impl(gui_app_t *app, int channel, int side, bool use_saved) {
    const int s = channel * 2 + side;
    if (s < 0 || s >= GUI_POPOUT_SLOTS) return false;
    popout_t *p = &s_po[s];
    if (p->open) return true;
    if (!s_main) s_main = glfwGetCurrentContext();
    if (!s_main || !popout_blit_init()) return false;
    glfwDefaultWindowHints();
    glfwWindowHint(P_GLFW_RESIZABLE, 1);
    glfwWindowHint(P_GLFW_VISIBLE, 1);
    if (rlGetVersion() >= RL_OPENGL_33) {
        glfwWindowHint(P_GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(P_GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(P_GLFW_OPENGL_PROFILE, P_GLFW_OPENGL_CORE_PROFILE);
#if defined(__APPLE__)
        glfwWindowHint(P_GLFW_OPENGL_FORWARD_COMPAT, 1);
#endif
    }
    char title[64];
    snprintf(title, sizeof(title), "MISRC - %s panel", popout_slot_name(s));
    int x = 0, y = 0, w = 960, h = 540;
    const bool saved = use_saved && p->have_geom && p->ww > 0 && p->wh > 0;
    if (saved) {
        x = p->wx; y = p->wy; w = p->ww; h = p->wh;
    } else {
        int mx = 0, my = 0;
        glfwGetWindowPos(s_main, &mx, &my);   // main window client-area origin
        const float scale = gui_ui_get_scale_factor();
        const channel_panel_config_t *cfg = channel ? &app->panel_config_b : &app->panel_config_a;
        const Rectangle b = side ? cfg->right_bounds : cfg->left_bounds;
        if (b.width > 0 && b.height > 0) {
            x = mx + (int)lroundf(b.x * scale);
            y = my + (int)lroundf(b.y * scale);
            w = (int)lroundf(b.width * scale);
            h = (int)lroundf(b.height * scale);
        } else {
            x = mx + 40; y = my + 40;
        }
        if (w < 320) w = 320;
        if (h < 200) h = 200;
    }
    glfwWindowHint(P_GLFW_VISIBLE, 0);   // place first, then show (no flash at the default spot)
    p->win = glfwCreateWindow(w, h, title, NULL, s_main);
    glfwMakeContextCurrent(s_main);
    if (!p->win) {
        fprintf(stderr, "[POPOUT] could not create the %s window\n", popout_slot_name(s));
        return false;
    }
    glfwSetWindowPos(p->win, x, y);
    glfwShowWindow(p->win);
    glfwSetWindowUserPointer(p->win, p);
    glfwSetScrollCallback(p->win, popout_scroll_cb);
    glfwMakeContextCurrent(p->win);
    glfwSwapInterval(0);   // the main window paces the frame
    p->vao = rlLoadVertexArray();
    glfwMakeContextCurrent(s_main);
    // Full-window quad: x, y, z, u, v (two triangles). RenderTexture rows
    // are stored with GL's bottom-left origin, so NDC -> uv is direct.
    static const float quad[] = {
        -1, -1, 0, 0, 0,   1, -1, 0, 1, 0,   1, 1, 0, 1, 1,
        -1, -1, 0, 0, 0,   1,  1, 0, 1, 1,  -1, 1, 0, 0, 1,
    };
    p->vbo = rlLoadVertexBuffer(quad, (int)sizeof(quad), false);
    p->open = true;
    p->fullscreen = false;
    p->buttons_down = 0;
    p->f11_down = p->esc_down = false;
    int main_x = 0, main_y = 0;
    glfwGetWindowPos(s_main, &main_x, &main_y);
    fprintf(stderr, "[POPOUT] %s panel popped out at %d,%d %dx%d%s (main window client %d,%d)\n",
            popout_slot_name(s), x, y, w, h, saved ? " (saved)" : " (over its panel)", main_x, main_y);
    return true;
}

void gui_popout_close(int channel, int side) {
    if (popout_fs_mode()) { fs_close(channel, side); return; }
    const int s = channel * 2 + side;
    if (s < 0 || s >= GUI_POPOUT_SLOTS || !s_po[s].open) return;
    popout_destroy(&s_po[s]);
    fprintf(stderr, "[POPOUT] %s panel returned\n", popout_slot_name(s));
}

void gui_popout_shutdown(void) {
    for (int s = 0; s < GUI_POPOUT_SLOTS; s++) if (s_po[s].open) popout_destroy(&s_po[s]);
    if (s_blit_ok) UnloadShader(s_blit);
    s_blit_ok = false;
}

static void popout_slot_panel(gui_app_t *app, int s, panel_view_type_t *view, void **state) {
    const channel_panel_config_t *cfg = (s / 2) ? &app->panel_config_b : &app->panel_config_a;
    if (s % 2 == 0) { *view = cfg->left_view; *state = cfg->left_state; }
    else { *view = cfg->right_view; *state = cfg->split ? cfg->right_state : NULL; }
}

static void popout_draw_fs_button(popout_t *p, Rectangle panel, Vector2 mouse) {
    const float w = 24.0f, h = 18.0f;
    p->fs_btn = (Rectangle){ panel.x + panel.width - w - 6.0f, panel.y + panel.height - h - 6.0f, w, h };
    const bool hover = CheckCollisionPointRec(mouse, p->fs_btn);
    DrawRectangleRounded(p->fs_btn, 0.2f, 4, hover ? COLOR_BUTTON_HOVER : (Color){ 65, 65, 75, 170 });
    // Corner brackets = fullscreen; inward = leave fullscreen.
    const Rectangle r = { p->fs_btn.x + 6, p->fs_btn.y + 4, w - 12, h - 8 };
    const Color c = COLOR_TEXT;
    const float k = 4.0f;
    if (!p->fullscreen) {
        DrawLineEx((Vector2){ r.x, r.y }, (Vector2){ r.x + k, r.y }, 1.5f, c);
        DrawLineEx((Vector2){ r.x, r.y }, (Vector2){ r.x, r.y + k }, 1.5f, c);
        DrawLineEx((Vector2){ r.x + r.width, r.y + r.height }, (Vector2){ r.x + r.width - k, r.y + r.height }, 1.5f, c);
        DrawLineEx((Vector2){ r.x + r.width, r.y + r.height }, (Vector2){ r.x + r.width, r.y + r.height - k }, 1.5f, c);
    } else {
        DrawRectangleLinesEx(r, 1.5f, c);
    }
}

void gui_popout_frame(gui_app_t *app) {
    if (popout_fs_mode()) { fs_frame(app); return; }
    if (!app) return;
    // Launch flags / restored slots wait for the first frame (window exists).
    for (int s = 0; s < GUI_POPOUT_SLOTS; s++) {
        if (s_launch_open[s]) {
            // A --popout slot lands over its panel: wait (up to ~2 s) until
            // the grid has laid that panel out once.
            static int s_wait_frames[GUI_POPOUT_SLOTS];
            const channel_panel_config_t *cfg = (s / 2) ? &app->panel_config_b : &app->panel_config_a;
            const Rectangle b = (s % 2) ? cfg->right_bounds : cfg->left_bounds;
            if (!s_launch_saved[s] && (b.width <= 0 || b.height <= 0) && s_wait_frames[s]++ < 120) continue;
            s_launch_open[s] = false;
            popout_open_impl(app, s / 2, s % 2, s_launch_saved[s]);
            s_launch_saved[s] = false;
        }
    }
    bool any = false;
    for (int s = 0; s < GUI_POPOUT_SLOTS; s++) any = any || s_po[s].open;
    if (!any) return;
    const float scale = gui_ui_get_scale_factor();

    for (int s = 0; s < GUI_POPOUT_SLOTS; s++) {
        popout_t *p = &s_po[s];
        if (!p->open) continue;
        panel_view_type_t view;
        void *state;
        popout_slot_panel(app, s, &view, &state);
        // The window was closed, or the slot no longer has a panel (split off).
        if (glfwWindowShouldClose(p->win) || (s % 2 == 1 && !state)) {
            gui_popout_close(s / 2, s % 2);
            continue;
        }
        int fbw = 0, fbh = 0, ww = 0, wh = 0;
        glfwGetFramebufferSize(p->win, &fbw, &fbh);
        glfwGetWindowSize(p->win, &ww, &wh);
        if (fbw <= 0 || fbh <= 0 || ww <= 0 || wh <= 0) continue;   // minimised
        if (!p->rt.id || p->rt_w != fbw || p->rt_h != fbh) {
            if (p->rt.id) UnloadRenderTexture(p->rt);
            p->rt = LoadRenderTexture(fbw, fbh);
            p->rt_w = fbw;
            p->rt_h = fbh;
            if (!p->rt.id) continue;
        }
        // Mouse in the panel's logical space (window -> framebuffer -> UI scale).
        double cx = 0.0, cy = 0.0;
        glfwGetCursorPos(p->win, &cx, &cy);
        const Vector2 mouse = { (float)(cx * fbw / ww / scale), (float)(cy * fbh / wh / scale) };
        const Rectangle panel = { 0.0f, 0.0f, fbw / scale, fbh / scale };

        // Keys: F11 toggles fullscreen, Esc leaves it.
        const bool f11 = glfwGetKey(p->win, P_GLFW_KEY_F11) == P_GLFW_PRESS;
        const bool esc = glfwGetKey(p->win, P_GLFW_KEY_ESCAPE) == P_GLFW_PRESS;
        if (f11 && !p->f11_down) popout_set_fullscreen(p, !p->fullscreen);
        if (esc && !p->esc_down && p->fullscreen) popout_set_fullscreen(p, false);
        p->f11_down = f11;
        p->esc_down = esc;

        // Input first (as the main loop does), then render.
        gui_ui_set_mouse_override(true, mouse);
        unsigned down_mask = 0;
        for (int b = 0; b < 3; b++)
            if (glfwGetMouseButton(p->win, b) == P_GLFW_PRESS) down_mask |= 1u << b;
        const unsigned pressed_mask = down_mask & ~p->buttons_down;
        p->buttons_down = down_mask;
        // Held for the input step AND the render (panels update drags there).
        gui_ui_set_mouse_override_buttons(down_mask, pressed_mask);
        const bool pressed = (pressed_mask & 1u) != 0;
        const panel_vtable_t *vt = panel_get_vtable(view);
        if (pressed) {
            if (p->fs_btn.width > 0 && CheckCollisionPointRec(mouse, p->fs_btn)) {
                popout_set_fullscreen(p, !p->fullscreen);
            } else if (vt && vt->handle_click && CheckCollisionPointRec(mouse, panel)) {
                vt->handle_click(state, app, s / 2, mouse, panel);
            }
        }
        if (p->wheel != 0.0 && vt && vt->handle_scroll)
            vt->handle_scroll(state, (float)p->wheel, panel);
        p->wheel = 0.0;

        BeginTextureMode(p->rt);
        phosphor_rt_set_outer_target(&p->rt);   // inner phosphor passes return here
        ClearBackground(COLOR_BG);
        rlPushMatrix();
        rlScalef(scale, scale, 1.0f);
        panel_render_popout(app, s / 2, s % 2, panel);
        popout_draw_fs_button(p, panel, mouse);
        rlPopMatrix();
        phosphor_rt_set_outer_target(NULL);
        EndTextureMode();
        gui_ui_set_mouse_override_buttons(0, 0);
        gui_ui_set_mouse_override(false, mouse);
    }
    rlDrawRenderBatchActive();

    // Present: each pop-out draws its texture in its own context.
    for (int s = 0; s < GUI_POPOUT_SLOTS; s++) {
        popout_t *p = &s_po[s];
        if (!p->open || !p->rt.id) continue;
        glfwMakeContextCurrent(p->win);
        rlViewport(0, 0, p->rt_w, p->rt_h);
        rlDisableDepthTest();
        rlDisableBackfaceCulling();
        rlDisableColorBlend();
        rlClearColor(0, 0, 0, 255);
        rlClearScreenBuffers();
        rlEnableShader(s_blit.id);
        int unit = 0;
        rlSetUniform(s_blit_tex_loc, &unit, RL_SHADER_UNIFORM_INT, 1);
        rlActiveTextureSlot(0);
        rlEnableTexture(p->rt.texture.id);
        if (p->vao) rlEnableVertexArray(p->vao);
        rlEnableVertexBuffer(p->vbo);
        rlSetVertexAttribute(0, 3, RL_FLOAT, false, 5 * (int)sizeof(float), 0);
        rlEnableVertexAttribute(0);
        rlSetVertexAttribute(1, 2, RL_FLOAT, false, 5 * (int)sizeof(float), 3 * (int)sizeof(float));
        rlEnableVertexAttribute(1);
        rlDrawVertexArray(0, 6);
        rlDisableVertexBuffer();
        if (p->vao) rlDisableVertexArray();
        rlDisableTexture();
        rlDisableShader();
        glfwSwapBuffers(p->win);
    }
    glfwMakeContextCurrent(s_main);
    rlEnableColorBlend();
}

//-----------------------------------------------------------------------------
// Settings: "s:o,x,y,w,h,f;..." per slot
//-----------------------------------------------------------------------------

void gui_popout_capture_settings(gui_app_t *app) {
    if (!app) return;
    char out[sizeof(app->settings.popout_windows)];
    size_t len = 0;
    out[0] = '\0';
    for (int s = 0; s < GUI_POPOUT_SLOTS; s++) {
        popout_t *p = &s_po[s];
        if (p->open && p->win && !p->fullscreen) {
            glfwGetWindowPos(p->win, &p->wx, &p->wy);
            glfwGetWindowSize(p->win, &p->ww, &p->wh);
            p->have_geom = true;
        }
        if (!p->have_geom && !p->open) continue;
        int n = snprintf(out + len, sizeof(out) - len, "%s%d:%d,%d,%d,%d,%d,%d", len ? ";" : "", s,
                         p->open ? 1 : 0, p->wx, p->wy, p->ww, p->wh, p->fullscreen ? 1 : 0);
        if (n < 0 || (size_t)n >= sizeof(out) - len) break;
        len += (size_t)n;
    }
    snprintf(app->settings.popout_windows, sizeof(app->settings.popout_windows), "%s", out);
}

void gui_popout_restore(gui_app_t *app) {
    if (!app || !app->settings.popout_windows[0]) return;
    char buf[sizeof(app->settings.popout_windows)];
    snprintf(buf, sizeof(buf), "%s", app->settings.popout_windows);
    for (char *tok = strtok(buf, ";"); tok; tok = strtok(NULL, ";")) {
        int s, o, x, y, w, h, f;
        if (sscanf(tok, "%d:%d,%d,%d,%d,%d,%d", &s, &o, &x, &y, &w, &h, &f) != 7 ||
            s < 0 || s >= GUI_POPOUT_SLOTS) continue;
        popout_t *p = &s_po[s];
        p->wx = x; p->wy = y; p->ww = w; p->wh = h;
        p->have_geom = w > 0 && h > 0;
        if (o) { s_launch_open[s] = true; s_launch_saved[s] = true; }   // opened on the first frame
        (void)f;   // fullscreen is not restored automatically (screens change)
    }
}

//-----------------------------------------------------------------------------
// Main grid: corner button + placeholder
//-----------------------------------------------------------------------------

void gui_popout_draw_button(int channel, int side, Rectangle b) {
    if (popout_fs_mode()) { fs_draw_button(channel, side, b); return; }
    const int s = channel * 2 + side;
    if (s < 0 || s >= GUI_POPOUT_SLOTS || b.width < 80.0f || b.height < 60.0f) {
        if (s >= 0 && s < GUI_POPOUT_SLOTS) s_grid_btn[s] = (Rectangle){ 0 };
        return;
    }
    const float w = 24.0f, h = 18.0f;
    Rectangle r = { b.x + b.width - w - 6.0f, b.y + b.height - h - 6.0f, w, h };
    s_grid_btn[s] = r;
    const bool hover = CheckCollisionPointRec(gui_ui_get_mouse_position(), r);
    DrawRectangleRounded(r, 0.2f, 4, hover ? COLOR_BUTTON_HOVER : (Color){ 65, 65, 75, 150 });
    // Pop-out glyph: a window with an arrow leaving its top-right corner.
    const Color c = hover ? COLOR_TEXT : (Color){ 200, 200, 210, 200 };
    DrawRectangleLinesEx((Rectangle){ r.x + 5, r.y + 6, 10, 8 }, 1.2f, c);
    DrawLineEx((Vector2){ r.x + 11, r.y + 9 }, (Vector2){ r.x + 19, r.y + 3 }, 1.4f, c);
    DrawLineEx((Vector2){ r.x + 15, r.y + 3 }, (Vector2){ r.x + 19, r.y + 3 }, 1.4f, c);
    DrawLineEx((Vector2){ r.x + 19, r.y + 3 }, (Vector2){ r.x + 19, r.y + 7 }, 1.4f, c);
}

void gui_popout_draw_placeholder(int channel, int side, Rectangle b, const char *view_name) {
    if (popout_fs_mode()) { fs_draw_placeholder(channel, side, b, view_name); return; }
    (void)channel;
    (void)side;
    DrawRectangleRec(b, COLOR_PANEL_BG);
    DrawRectangleLinesEx(b, 1.0f, COLOR_GRID);
    char line[96];
    snprintf(line, sizeof(line), "%s popped out", view_name ? view_name : "Panel");
    const int fs = 18, fs2 = 14;
    const int w1 = gui_text_measure(line, fs);
    const char *hint = "Click here (or close its window) to return it";
    const int w2 = gui_text_measure(hint, fs2);
    gui_text_draw(line, b.x + (b.width - w1) / 2.0f, b.y + b.height / 2.0f - fs - 2, fs, COLOR_TEXT);
    gui_text_draw(hint, b.x + (b.width - w2) / 2.0f, b.y + b.height / 2.0f + 4, fs2, COLOR_TEXT_DIM);
}

bool gui_popout_handle_grid_click(gui_app_t *app, int channel, int side, Vector2 click, Rectangle b) {
    if (popout_fs_mode()) return fs_handle_grid_click(app, channel, side, click, b);
    const int s = channel * 2 + side;
    if (s < 0 || s >= GUI_POPOUT_SLOTS) return false;
    if (s_po[s].open) {
        if (CheckCollisionPointRec(click, b)) {
            gui_popout_close(channel, side);
            return true;
        }
        return false;
    }
    if (s_grid_btn[s].width > 0 && CheckCollisionPointRec(click, s_grid_btn[s])) {
        gui_popout_open(app, channel, side);
        return true;
    }
    return false;
}

#else  // !POPOUT_ENABLED
bool gui_popout_available(void) { return false; }
bool gui_popout_is_open(int channel, int side) { return fs_is_open(channel, side); }
bool gui_popout_open(struct gui_app *app, int channel, int side) { return fs_open(app, channel, side); }
void gui_popout_close(int channel, int side) { fs_close(channel, side); }
void gui_popout_frame(struct gui_app *app) { fs_frame(app); }
void gui_popout_restore(struct gui_app *app) { fs_restore(app); }
void gui_popout_capture_settings(struct gui_app *app) { fs_capture_settings(app); }
void gui_popout_shutdown(void) { fs_shutdown(); }
void gui_popout_draw_button(int channel, int side, Rectangle b) { fs_draw_button(channel, side, b); }
bool gui_popout_handle_grid_click(struct gui_app *app, int channel, int side, Vector2 click, Rectangle b) {
    return fs_handle_grid_click(app, channel, side, click, b);
}
void gui_popout_draw_placeholder(int channel, int side, Rectangle b, const char *view_name) {
    fs_draw_placeholder(channel, side, b, view_name);
}
#endif

//-----------------------------------------------------------------------------
// Full-screen panel (all builds; only entered where pop-outs are unavailable)
//-----------------------------------------------------------------------------

bool gui_popout_fullscreen_active(void) { return s_fs_slot >= 0; }

static Rectangle fs_bounds(void) {
    return (Rectangle){ 0.0f, 0.0f, (float)gui_ui_get_layout_width(), (float)gui_ui_get_layout_height() };
}

void gui_popout_draw_fullscreen(struct gui_app *app) {
    if (!app || s_fs_slot < 0) return;
    const channel_panel_config_t *cfg = (s_fs_slot / 2) ? &app->panel_config_b : &app->panel_config_a;
    if (s_fs_slot % 2 == 1 && (!cfg->split || !cfg->right_state)) {   // the slot went away
        s_fs_slot = -1;
        return;
    }
    const Rectangle b = fs_bounds();
    const float scale = gui_ui_get_scale_factor();
    rlDrawRenderBatchActive();
    rlPushMatrix();
    rlScalef(scale, scale, 1.0f);
    DrawRectangleRec(b, COLOR_BG);
    panel_render_popout(app, s_fs_slot / 2, s_fs_slot % 2, b);
    {
    s_fs_exit_btn = (Rectangle){ b.x + b.width - FS_BTN_W - 8.0f, b.y + b.height - FS_BTN_H - 8.0f, FS_BTN_W, FS_BTN_H };
    fs_draw_corner_glyph(s_fs_exit_btn, false, CheckCollisionPointRec(gui_ui_get_mouse_position(), s_fs_exit_btn));
    }
    rlPopMatrix();
}

bool gui_popout_fullscreen_input(struct gui_app *app, bool pressed) {
    if (!app || s_fs_slot < 0) return false;
    const int ch = s_fs_slot / 2, side = s_fs_slot % 2;
    if (IsKeyPressed(KEY_BACK) || IsKeyPressed(KEY_ESCAPE)) {
        gui_popout_close(ch, side);
        return true;
    }
    if (!pressed) return true;   // taps are this panel's alone while full screen
    const Vector2 click = gui_ui_get_mouse_position();
    if (s_fs_exit_btn.width > 0 && CheckCollisionPointRec(click, s_fs_exit_btn)) {
        gui_popout_close(ch, side);
        return true;
    }
    const channel_panel_config_t *cfg = ch ? &app->panel_config_b : &app->panel_config_a;
    const panel_view_type_t view = side ? cfg->right_view : cfg->left_view;
    void *state = side ? cfg->right_state : cfg->left_state;
    const panel_vtable_t *vt = panel_get_vtable(view);
    if (vt && vt->handle_click) vt->handle_click(state, app, ch, click, fs_bounds());
    return true;
}

bool gui_popout_fullscreen_scroll(struct gui_app *app, float delta) {
    if (!app || s_fs_slot < 0) return false;
    const int ch = s_fs_slot / 2, side = s_fs_slot % 2;
    const channel_panel_config_t *cfg = ch ? &app->panel_config_b : &app->panel_config_a;
    const panel_view_type_t view = side ? cfg->right_view : cfg->left_view;
    void *state = side ? cfg->right_state : cfg->left_state;
    const panel_vtable_t *vt = panel_get_vtable(view);
    if (vt && vt->handle_scroll) vt->handle_scroll(state, delta, fs_bounds());
    return true;
}
