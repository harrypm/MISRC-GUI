/*
 * MISRC GUI - Reusable Phosphor Render Texture Module
 *
 * GPU-accelerated phosphor persistence effect using ping-pong render textures.
 */

#include "gui_phosphor_rt.h"
#include "rlgl.h"

#if defined(__ANDROID__)
#include "../core/gui_android_viewport.h"
/* Nested render textures are not a stack in raylib: EndTextureMode() always
 * switches to the default framebuffer. Restore the Android full-UI canvas
 * after finishing any inner phosphor/FFT texture pass. */
#define PHOSPHOR_END_TEXTURE_MODE() do { \
    EndTextureMode(); \
    gui_android_viewport_restore_canvas_after_texture(); \
} while (0)
#else
#define PHOSPHOR_END_TEXTURE_MODE() EndTextureMode()
#endif
#include <math.h>
#include <stdlib.h>

//-----------------------------------------------------------------------------
// Shader Code (embedded GLSL)
//-----------------------------------------------------------------------------

#if defined(GRAPHICS_API_OPENGL_ES2)
    #define GLSL_VERSION_STRING "#version 100\n"
    #define GLSL_PRECISION "precision mediump float;\n"
    #define GLSL_IN "attribute "
    #define GLSL_OUT "varying "
    #define GLSL_FRAG_IN "varying "
    #define GLSL_FRAG_OUT ""
    #define GLSL_FRAG_COLOR "gl_FragColor"
    #define GLSL_TEXTURE "texture2D"
#elif defined(GRAPHICS_API_OPENGL_ES3)
    #define GLSL_VERSION_STRING "#version 300 es\n"
    #define GLSL_PRECISION "precision mediump float;\n"
    #define GLSL_IN "in "
    #define GLSL_OUT "out "
    #define GLSL_FRAG_IN "in "
    #define GLSL_FRAG_OUT "out vec4 finalColor;\n"
    #define GLSL_FRAG_COLOR "finalColor"
    #define GLSL_TEXTURE "texture"
#elif defined(GRAPHICS_API_OPENGL_21)
    // Desktop OpenGL 2.1: GLSL 1.20 legacy attribute/varying syntax.
    #define GLSL_VERSION_STRING "#version 120\n"
    #define GLSL_PRECISION ""
    #define GLSL_IN "attribute "
    #define GLSL_OUT "varying "
    #define GLSL_FRAG_IN "varying "
    #define GLSL_FRAG_OUT ""
    #define GLSL_FRAG_COLOR "gl_FragColor"
    #define GLSL_TEXTURE "texture2D"
#else  // Desktop OpenGL 3.3+
    #define GLSL_VERSION_STRING "#version 330\n"
    #define GLSL_PRECISION ""
    #define GLSL_IN "in "
    #define GLSL_OUT "out "
    #define GLSL_FRAG_IN "in "
    #define GLSL_FRAG_OUT "out vec4 finalColor;\n"
    #define GLSL_FRAG_COLOR "finalColor"
    #define GLSL_TEXTURE "texture"
#endif

// Vertex shader - standard passthrough
static const char *phosphor_vs =
    GLSL_VERSION_STRING
    GLSL_PRECISION
    GLSL_IN "vec3 vertexPosition;\n"
    GLSL_IN "vec2 vertexTexCoord;\n"
    GLSL_OUT "vec2 fragTexCoord;\n"
    "uniform mat4 mvp;\n"
    "void main() {\n"
    "    fragTexCoord = vertexTexCoord;\n"
    "    gl_Position = mvp * vec4(vertexPosition, 1.0);\n"
    "}\n";

// Fragment shader - composites accumulated intensity into heatmap color with bloom
static const char *phosphor_composite_fs =
    GLSL_VERSION_STRING
    GLSL_PRECISION
    GLSL_FRAG_IN "vec2 fragTexCoord;\n"
    GLSL_FRAG_OUT
    "uniform sampler2D texture0;\n"
    "uniform vec2 texelSize;\n"
    "uniform float bloomIntensity;\n"
    "\n"
    "vec4 intensityToHeatmap(float intensity) {\n"
    "    if (intensity < 0.02) return vec4(0.0, 0.0, 0.0, 0.0);\n"
    "    if (intensity > 1.0) intensity = 1.0;\n"
    "\n"
    "    vec3 color;\n"
    "    if (intensity < 0.25) {\n"
    "        float t = intensity / 0.25;\n"
    "        color = vec3(0.0, 0.078 * t, 0.392 + 0.608 * t);\n"
    "    } else if (intensity < 0.5) {\n"
    "        float t = (intensity - 0.25) / 0.25;\n"
    "        color = vec3(0.0, 0.078 + 0.922 * t, 1.0 - 0.784 * t);\n"
    "    } else if (intensity < 0.75) {\n"
    "        float t = (intensity - 0.5) / 0.25;\n"
    "        color = vec3(t, 1.0, 0.216 - 0.216 * t);\n"
    "    } else {\n"
    "        float t = (intensity - 0.75) / 0.25;\n"
    "        color = vec3(1.0, 1.0 - 0.706 * t, 0.0);\n"
    "    }\n"
    "\n"
    "    float alpha = 0.3 + 0.7 * intensity * intensity;\n"
    "    return vec4(color, alpha);\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    // Sample current pixel\n"
    "    float center = " GLSL_TEXTURE "(texture0, fragTexCoord).r;\n"
    "\n"
    "    // CRT-like bloom: stronger vertical bleed, slight horizontal bleed\n"
    "    // Vertical samples (primary bloom direction for CRT phosphor)\n"
    "    float up1    = " GLSL_TEXTURE "(texture0, fragTexCoord + vec2(0.0, -texelSize.y)).r;\n"
    "    float down1  = " GLSL_TEXTURE "(texture0, fragTexCoord + vec2(0.0,  texelSize.y)).r;\n"
    "    float up2    = " GLSL_TEXTURE "(texture0, fragTexCoord + vec2(0.0, -2.0*texelSize.y)).r;\n"
    "    float down2  = " GLSL_TEXTURE "(texture0, fragTexCoord + vec2(0.0,  2.0*texelSize.y)).r;\n"
    "    float up3    = " GLSL_TEXTURE "(texture0, fragTexCoord + vec2(0.0, -3.0*texelSize.y)).r;\n"
    "    float down3  = " GLSL_TEXTURE "(texture0, fragTexCoord + vec2(0.0,  3.0*texelSize.y)).r;\n"
    "    float up4    = " GLSL_TEXTURE "(texture0, fragTexCoord + vec2(0.0, -4.0*texelSize.y)).r;\n"
    "    float down4  = " GLSL_TEXTURE "(texture0, fragTexCoord + vec2(0.0,  4.0*texelSize.y)).r;\n"
    "\n"
    "    // Horizontal samples (subtle side bleed)\n"
    "    float left1  = " GLSL_TEXTURE "(texture0, fragTexCoord + vec2(-texelSize.x, 0.0)).r;\n"
    "    float right1 = " GLSL_TEXTURE "(texture0, fragTexCoord + vec2( texelSize.x, 0.0)).r;\n"
    "    float left2  = " GLSL_TEXTURE "(texture0, fragTexCoord + vec2(-2.0*texelSize.x, 0.0)).r;\n"
    "    float right2 = " GLSL_TEXTURE "(texture0, fragTexCoord + vec2( 2.0*texelSize.x, 0.0)).r;\n"
    "\n"
    "    // Vertical bloom (CRT phosphor glow - stronger)\n"
    "    float vBloom = 0.6 * (up1 + down1) + 0.35 * (up2 + down2) + 0.15 * (up3 + down3) + 0.05 * (up4 + down4);\n"
    "\n"
    "    // Horizontal bloom (subtle side glow)\n"
    "    float hBloom = 0.25 * (left1 + right1) + 0.1 * (left2 + right2);\n"
    "\n"
    "    // Combine: center + scaled bloom\n"
    "    float intensity = center + bloomIntensity * (vBloom + hBloom);\n"
    "    intensity = min(intensity, 1.0);\n"
    "\n"
    "    " GLSL_FRAG_COLOR " = intensityToHeatmap(intensity);\n"
    "}\n";

// Fragment shader - opacity mode (uses channel color with intensity as alpha)
static const char *phosphor_opacity_fs =
    GLSL_VERSION_STRING
    GLSL_PRECISION
    GLSL_FRAG_IN "vec2 fragTexCoord;\n"
    GLSL_FRAG_OUT
    "uniform sampler2D texture0;\n"
    "uniform vec2 texelSize;\n"
    "uniform vec3 channelColor;\n"
    "uniform float bloomIntensity;\n"
    "\n"
    "void main() {\n"
    "    // Sample current pixel\n"
    "    float center = " GLSL_TEXTURE "(texture0, fragTexCoord).r;\n"
    "\n"
    "    // CRT-like bloom: stronger vertical bleed, slight horizontal bleed\n"
    "    // Vertical samples (primary bloom direction for CRT phosphor)\n"
    "    float up1    = " GLSL_TEXTURE "(texture0, fragTexCoord + vec2(0.0, -texelSize.y)).r;\n"
    "    float down1  = " GLSL_TEXTURE "(texture0, fragTexCoord + vec2(0.0,  texelSize.y)).r;\n"
    "    float up2    = " GLSL_TEXTURE "(texture0, fragTexCoord + vec2(0.0, -2.0*texelSize.y)).r;\n"
    "    float down2  = " GLSL_TEXTURE "(texture0, fragTexCoord + vec2(0.0,  2.0*texelSize.y)).r;\n"
    "    float up3    = " GLSL_TEXTURE "(texture0, fragTexCoord + vec2(0.0, -3.0*texelSize.y)).r;\n"
    "    float down3  = " GLSL_TEXTURE "(texture0, fragTexCoord + vec2(0.0,  3.0*texelSize.y)).r;\n"
    "    float up4    = " GLSL_TEXTURE "(texture0, fragTexCoord + vec2(0.0, -4.0*texelSize.y)).r;\n"
    "    float down4  = " GLSL_TEXTURE "(texture0, fragTexCoord + vec2(0.0,  4.0*texelSize.y)).r;\n"
    "\n"
    "    // Horizontal samples (subtle side bleed)\n"
    "    float left1  = " GLSL_TEXTURE "(texture0, fragTexCoord + vec2(-texelSize.x, 0.0)).r;\n"
    "    float right1 = " GLSL_TEXTURE "(texture0, fragTexCoord + vec2( texelSize.x, 0.0)).r;\n"
    "    float left2  = " GLSL_TEXTURE "(texture0, fragTexCoord + vec2(-2.0*texelSize.x, 0.0)).r;\n"
    "    float right2 = " GLSL_TEXTURE "(texture0, fragTexCoord + vec2( 2.0*texelSize.x, 0.0)).r;\n"
    "\n"
    "    // Vertical bloom (CRT phosphor glow - stronger)\n"
    "    float vBloom = 0.6 * (up1 + down1) + 0.35 * (up2 + down2) + 0.15 * (up3 + down3) + 0.05 * (up4 + down4);\n"
    "\n"
    "    // Horizontal bloom (subtle side glow)\n"
    "    float hBloom = 0.25 * (left1 + right1) + 0.1 * (left2 + right2);\n"
    "\n"
    "    // Combine: center + scaled bloom\n"
    "    float intensity = center + bloomIntensity * (vBloom + hBloom);\n"
    "    intensity = min(intensity, 1.0);\n"
    "\n"
    "    // Use channel color with intensity as alpha\n"
    "    if (intensity < 0.02) {\n"
    "        " GLSL_FRAG_COLOR " = vec4(0.0, 0.0, 0.0, 0.0);\n"
    "    } else {\n"
    "        float alpha = 0.3 + 0.7 * intensity;\n"
    "        " GLSL_FRAG_COLOR " = vec4(channelColor, alpha);\n"
    "    }\n"
    "}\n";

// Simple decay shader - linear multiplicative decay per frame
static const char *phosphor_decay_fs =
    GLSL_VERSION_STRING
    GLSL_PRECISION
    GLSL_FRAG_IN "vec2 fragTexCoord;\n"
    GLSL_FRAG_OUT
    "uniform sampler2D texture0;\n"
    "uniform float decayRate;\n"
    "\n"
    "void main() {\n"
    "    float intensity = " GLSL_TEXTURE "(texture0, fragTexCoord).r;\n"
    "    intensity = intensity * decayRate;\n"
    "    if (intensity < 0.02) intensity = 0.0;\n"
    "    " GLSL_FRAG_COLOR " = vec4(intensity, 0.0, 0.0, 1.0);\n"
    "}\n";

//-----------------------------------------------------------------------------
// Shader State (shared across all instances)
//-----------------------------------------------------------------------------

static Shader s_composite_shader = {0};
static Shader s_opacity_shader = {0};
static Shader s_decay_shader = {0};
static bool s_shaders_loaded = false;

static int s_composite_texelSize_loc = -1;
static int s_composite_bloomIntensity_loc = -1;
static int s_opacity_texelSize_loc = -1;
static int s_opacity_channelColor_loc = -1;
static int s_opacity_bloomIntensity_loc = -1;
static int s_decay_decayRate_loc = -1;

//-----------------------------------------------------------------------------
// Shader Management
//-----------------------------------------------------------------------------

bool phosphor_rt_init_shaders(void) {
    if (s_shaders_loaded) return true;

    // Load composite shader (heatmap mode with bloom)
    s_composite_shader = LoadShaderFromMemory(phosphor_vs, phosphor_composite_fs);
    if (s_composite_shader.id == 0) {
        TraceLog(LOG_WARNING, "PHOSPHOR_RT: Failed to load composite shader");
        return false;
    }
    s_composite_texelSize_loc = GetShaderLocation(s_composite_shader, "texelSize");
    s_composite_bloomIntensity_loc = GetShaderLocation(s_composite_shader, "bloomIntensity");

    // Load opacity shader (channel color mode)
    s_opacity_shader = LoadShaderFromMemory(phosphor_vs, phosphor_opacity_fs);
    if (s_opacity_shader.id == 0) {
        TraceLog(LOG_WARNING, "PHOSPHOR_RT: Failed to load opacity shader");
        UnloadShader(s_composite_shader);
        return false;
    }
    s_opacity_texelSize_loc = GetShaderLocation(s_opacity_shader, "texelSize");
    s_opacity_channelColor_loc = GetShaderLocation(s_opacity_shader, "channelColor");
    s_opacity_bloomIntensity_loc = GetShaderLocation(s_opacity_shader, "bloomIntensity");

    // Load decay shader (persistence buffer update)
    s_decay_shader = LoadShaderFromMemory(phosphor_vs, phosphor_decay_fs);
    if (s_decay_shader.id == 0) {
        TraceLog(LOG_WARNING, "PHOSPHOR_RT: Failed to load decay shader");
        UnloadShader(s_composite_shader);
        UnloadShader(s_opacity_shader);
        return false;
    }
    s_decay_decayRate_loc = GetShaderLocation(s_decay_shader, "decayRate");

    s_shaders_loaded = true;
    TraceLog(LOG_INFO, "PHOSPHOR_RT: GPU shaders loaded successfully");
    return true;
}

void phosphor_rt_cleanup_shaders(void) {
    if (s_shaders_loaded) {
        UnloadShader(s_composite_shader);
        UnloadShader(s_opacity_shader);
        UnloadShader(s_decay_shader);
        s_shaders_loaded = false;
        s_composite_shader.id = 0;
        s_opacity_shader.id = 0;
        s_decay_shader.id = 0;
    }
}

//-----------------------------------------------------------------------------
// Render Texture Lifecycle
//-----------------------------------------------------------------------------

bool phosphor_rt_init(phosphor_rt_t *prt, int logical_width,
                      int logical_height, float render_scale_x,
                      float render_scale_y) {
    if (!prt) return false;

    // Clamp dimensions and derive display-pixel-sized render textures. Keeping
    // this buffer at physical density avoids blurring at >100% and needless
    // over-allocation at <100% while the rest of the UI remains logical.
    if (logical_width < 1) logical_width = 1;
    if (logical_height < 1) logical_height = 1;
    if (!isfinite(render_scale_x) || render_scale_x <= 0.0f) render_scale_x = 1.0f;
    if (!isfinite(render_scale_y) || render_scale_y <= 0.0f) render_scale_y = 1.0f;
    int width = (int)lroundf((float)logical_width * render_scale_x);
    int height = (int)lroundf((float)logical_height * render_scale_y);
    if (width < 1) width = 1;
    if (height < 1) height = 1;

    // Check if resize needed
    if (prt->valid && prt->width == width && prt->height == height &&
        prt->logical_width == logical_width &&
        prt->logical_height == logical_height) {
        return true;  // Already correct size
    }

    // Free existing render textures
    if (prt->valid) {
        UnloadRenderTexture(prt->rt[0]);
        UnloadRenderTexture(prt->rt[1]);
        prt->valid = false;
    }

    // Initialize shaders if needed
    if (!phosphor_rt_init_shaders()) {
        return false;
    }

    // Create ping-pong render textures
    prt->rt[0] = LoadRenderTexture(width, height);
    prt->rt[1] = LoadRenderTexture(width, height);

    if (prt->rt[0].id == 0 || prt->rt[1].id == 0) {
        TraceLog(LOG_WARNING, "PHOSPHOR_RT: Failed to create render textures");
        if (prt->rt[0].id) UnloadRenderTexture(prt->rt[0]);
        if (prt->rt[1].id) UnloadRenderTexture(prt->rt[1]);
        return false;
    }

    // Set texture filtering for smooth appearance
    SetTextureFilter(prt->rt[0].texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(prt->rt[1].texture, TEXTURE_FILTER_BILINEAR);

    // EndTextureMode() resets raylib's model-view matrix. Preserve an outer UI
    // camera when initialization happens during a Clay custom render command.
    Matrix outer_modelview = rlGetMatrixModelview();

    // Clear render textures
    BeginTextureMode(prt->rt[0]); ClearBackground(BLACK); PHOSPHOR_END_TEXTURE_MODE();
    BeginTextureMode(prt->rt[1]); ClearBackground(BLACK); PHOSPHOR_END_TEXTURE_MODE();
    rlSetMatrixModelview(outer_modelview);

    prt->width = width;
    prt->height = height;
    prt->logical_width = logical_width;
    prt->logical_height = logical_height;
    prt->rt_index = 0;
    prt->valid = true;

    // Set default config if not already set
    if (prt->config.decay_rate == 0.0f) {
        prt->config.decay_rate = PHOSPHOR_DEFAULT_DECAY_RATE;
    }
    if (prt->config.hit_increment == 0.0f) {
        prt->config.hit_increment = PHOSPHOR_DEFAULT_HIT_INCREMENT;
    }
    if (prt->config.bloom_intensity == 0.0f) {
        prt->config.bloom_intensity = PHOSPHOR_DEFAULT_BLOOM;
    }

    TraceLog(LOG_INFO,
             "PHOSPHOR_RT: Initialized %dx%d render textures for %dx%d logical pixels",
             width, height, logical_width, logical_height);
    return true;
}

void phosphor_rt_clear(phosphor_rt_t *prt) {
    if (!prt || !prt->valid) return;

    Matrix outer_modelview = rlGetMatrixModelview();
    BeginTextureMode(prt->rt[0]); ClearBackground(BLACK); PHOSPHOR_END_TEXTURE_MODE();
    BeginTextureMode(prt->rt[1]); ClearBackground(BLACK); PHOSPHOR_END_TEXTURE_MODE();
    rlSetMatrixModelview(outer_modelview);
}

void phosphor_rt_cleanup(phosphor_rt_t *prt) {
    if (!prt) return;

    if (prt->valid) {
        UnloadRenderTexture(prt->rt[0]);
        UnloadRenderTexture(prt->rt[1]);
        prt->valid = false;
    }
    prt->width = 0;
    prt->height = 0;
    prt->logical_width = 0;
    prt->logical_height = 0;
    prt->rt_index = 0;
    prt->outer_modelview_saved = false;
}

void phosphor_rt_set_config(phosphor_rt_t *prt, const phosphor_rt_config_t *config) {
    if (!prt || !config) return;
    prt->config = *config;
}

void phosphor_rt_set_decay_rate(phosphor_rt_t *prt, float decay_rate) {
    if (!prt) return;
    prt->config.decay_rate = decay_rate;
}

void phosphor_rt_set_hit_increment(phosphor_rt_t *prt, float hit_increment) {
    if (!prt) return;
    prt->config.hit_increment = hit_increment;
}

void phosphor_rt_set_bloom_intensity(phosphor_rt_t *prt, float bloom_intensity) {
    if (!prt) return;
    prt->config.bloom_intensity = bloom_intensity;
}

void phosphor_rt_set_channel_color(phosphor_rt_t *prt, const float *channel_color) {
    if (!prt || !channel_color) return;
    prt->config.channel_color[0] = channel_color[0];
    prt->config.channel_color[1] = channel_color[1];
    prt->config.channel_color[2] = channel_color[2];
}

//-----------------------------------------------------------------------------
// Rendering Pipeline
//-----------------------------------------------------------------------------

void phosphor_rt_begin_frame(phosphor_rt_t *prt) {
    if (!prt || !prt->valid || !s_shaders_loaded) return;

    int current = prt->rt_index;
    int next = 1 - current;

    // Apply decay to previous frame, write to next buffer
    prt->outer_modelview = rlGetMatrixModelview();
    prt->outer_modelview_saved = true;
    BeginTextureMode(prt->rt[next]);
    ClearBackground(BLACK);

    float decay_rate = prt->config.decay_rate;
    SetShaderValue(s_decay_shader, s_decay_decayRate_loc, &decay_rate, SHADER_UNIFORM_FLOAT);
    BeginShaderMode(s_decay_shader);
    // Render textures are flipped in Y
    DrawTextureRec(prt->rt[current].texture,
                   (Rectangle){0, 0, (float)prt->width, -(float)prt->height},
                   (Vector2){0, 0}, WHITE);
    EndShaderMode();

    // Now ready for drawing new primitives with additive blending
    BeginBlendMode(BLEND_ADDITIVE);
    rlScalef((float)prt->width / (float)prt->logical_width,
             (float)prt->height / (float)prt->logical_height,
             1.0f);
}

void phosphor_rt_end_frame(phosphor_rt_t *prt) {
    if (!prt || !prt->valid) return;

    EndBlendMode();
    PHOSPHOR_END_TEXTURE_MODE();
    if (prt->outer_modelview_saved) {
        rlSetMatrixModelview(prt->outer_modelview);
        prt->outer_modelview_saved = false;
    }

    // Swap buffers
    prt->rt_index = 1 - prt->rt_index;
}

void phosphor_rt_render(phosphor_rt_t *prt, float x, float y, bool use_alpha_blend) {
    if (!prt || !prt->valid || !s_shaders_loaded) return;

    float texelSize[2] = {1.0f / prt->width, 1.0f / prt->height};
    float bloom = prt->config.bloom_intensity;
    SetShaderValue(s_composite_shader, s_composite_texelSize_loc, texelSize, SHADER_UNIFORM_VEC2);
    SetShaderValue(s_composite_shader, s_composite_bloomIntensity_loc, &bloom, SHADER_UNIFORM_FLOAT);

    if (use_alpha_blend) {
        BeginBlendMode(BLEND_ALPHA);
    }

    BeginShaderMode(s_composite_shader);
    DrawTexturePro(prt->rt[prt->rt_index].texture,
                   (Rectangle){0, 0, (float)prt->width, -(float)prt->height},
                   (Rectangle){x, y,
                               (float)prt->logical_width,
                               (float)prt->logical_height},
                   (Vector2){0, 0}, 0.0f, WHITE);
    EndShaderMode();

    if (use_alpha_blend) {
        EndBlendMode();
    }
}

void phosphor_rt_render_opacity(phosphor_rt_t *prt, float x, float y) {
    if (!prt || !prt->valid || !s_shaders_loaded) return;

    float texelSize[2] = {1.0f / prt->width, 1.0f / prt->height};
    float bloom = prt->config.bloom_intensity;
    SetShaderValue(s_opacity_shader, s_opacity_texelSize_loc, texelSize, SHADER_UNIFORM_VEC2);
    SetShaderValue(s_opacity_shader, s_opacity_channelColor_loc, prt->config.channel_color, SHADER_UNIFORM_VEC3);
    SetShaderValue(s_opacity_shader, s_opacity_bloomIntensity_loc, &bloom, SHADER_UNIFORM_FLOAT);

    BeginShaderMode(s_opacity_shader);
    DrawTexturePro(prt->rt[prt->rt_index].texture,
                   (Rectangle){0, 0, (float)prt->width, -(float)prt->height},
                   (Rectangle){x, y,
                               (float)prt->logical_width,
                               (float)prt->logical_height},
                   (Vector2){0, 0}, 0.0f, WHITE);
    EndShaderMode();
}

Color phosphor_rt_get_draw_color(phosphor_rt_t *prt) {
    float intensity = prt ? prt->config.hit_increment : PHOSPHOR_DEFAULT_HIT_INCREMENT;
    unsigned char val = (unsigned char)(intensity * 255.0f);
    return (Color){val, 0, 0, 255};
}

//-----------------------------------------------------------------------------
// Channel colors (RGB 0-1 range for shader)
//-----------------------------------------------------------------------------

const float PHOSPHOR_CHANNEL_COLOR_A[3] = { 80.0f/255.0f, 220.0f/255.0f, 100.0f/255.0f };
const float PHOSPHOR_CHANNEL_COLOR_B[3] = { 220.0f/255.0f, 200.0f/255.0f, 80.0f/255.0f };

//-----------------------------------------------------------------------------
// Waveform Drawing Helpers
//-----------------------------------------------------------------------------

void phosphor_rt_draw_waveform(phosphor_rt_t *prt,
                               const waveform_sample_t *samples, size_t sample_count,
                               float amplitude_scale) {
    if (!prt || !prt->valid || !samples || sample_count < 2) return;

    int buf_height = prt->logical_height;

    // Scale factor: half height = full amplitude
    float scale = amplitude_scale * 0.5f;
    float center_y = buf_height * 0.5f;

    // Get draw color for hit intensity
    Color waveColor = phosphor_rt_get_draw_color(prt);

    // Draw waveform as connected line segments
    for (size_t i = 0; i < sample_count - 1; i++) {
        float y0 = center_y - samples[i].value * scale * buf_height;
        float y1 = center_y - samples[i + 1].value * scale * buf_height;

        float x0 = (float)i;
        float x1 = (float)(i + 1);

        DrawLineEx((Vector2){x0, y0}, (Vector2){x1, y1}, 1.0f, waveColor);
    }
}
