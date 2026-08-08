/*
 * Clay renderer for raylib
 * Based on https://github.com/nicbarker/clay
 */

#include <clay.h>
#include "raylib.h"
#include "../visualization/gui_custom_elements.h"
#include "../visualization/gui_panel.h"
#include "../visualization/gui_vu_meter.h"
#include "gui_ui.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define CLAY_RECTANGLE_TO_RAYLIB_RECTANGLE(rectangle) (Rectangle) { .x = rectangle.x, .y = rectangle.y, .width = rectangle.width, .height = rectangle.height }
#define CLAY_COLOR_TO_RAYLIB_COLOR(color) (Color) { .r = (unsigned char)roundf(color.r), .g = (unsigned char)roundf(color.g), .b = (unsigned char)roundf(color.b), .a = (unsigned char)roundf(color.a) }


Clay_Dimensions Raylib_MeasureText(Clay_StringSlice text, Clay_TextElementConfig *config, void *userData) {
    // Measure string size for Font
    Clay_Dimensions textSize = { 0 };

    float maxTextWidth = 0.0f;
    float lineTextWidth = 0;
    int maxLineCharCount = 0;
    int lineCharCount = 0;

    float textHeight = config->fontSize;
    Font* fonts = (Font*)userData;
    Font fontToUse = fonts[config->fontId];
    // Font failed to load, likely the fonts are in the wrong place relative to the execution dir.
    // RayLib ships with a default font, so we can continue with that built in one.
    if (!fontToUse.glyphs) {
        fontToUse = GetFontDefault();
    }

    float scaleFactor = config->fontSize/(float)fontToUse.baseSize;

    for (int i = 0; i < text.length; ++i, lineCharCount++)
    {
        if (text.chars[i] == '\n') {
            maxTextWidth = fmax(maxTextWidth, lineTextWidth);
            maxLineCharCount = maxLineCharCount > lineCharCount ? maxLineCharCount : lineCharCount;
            lineTextWidth = 0;
            lineCharCount = 0;
            continue;
        }
        int index = text.chars[i] - 32;
        if (fontToUse.glyphs[index].advanceX != 0) lineTextWidth += fontToUse.glyphs[index].advanceX;
        else lineTextWidth += (fontToUse.recs[index].width + fontToUse.glyphs[index].offsetX);
    }

    maxTextWidth = fmax(maxTextWidth, lineTextWidth);
    maxLineCharCount = maxLineCharCount > lineCharCount ? maxLineCharCount : lineCharCount;

    textSize.width = maxTextWidth * scaleFactor + (lineCharCount * config->letterSpacing);
    textSize.height = textHeight;

    return textSize;
}

void Clay_Raylib_Initialize(int width, int height, const char *title, unsigned int flags) {
    SetConfigFlags(flags);
    InitWindow(width, height, title);
}

// A MALLOC'd buffer, that we keep modifying inorder to save from so many Malloc and Free Calls.
// Call Clay_Raylib_Close() to free
static char *temp_render_buffer = NULL;
static int temp_render_buffer_len = 0;

// Call after closing the window to clean up the render buffer
void Clay_Raylib_Close()
{
    if(temp_render_buffer) free(temp_render_buffer);
    temp_render_buffer_len = 0;

    CloseWindow();
}

void Clay_Raylib_Render(Clay_RenderCommandArray renderCommands, Font* fonts)
{
    for (int j = 0; j < renderCommands.length; j++)
    {
        Clay_RenderCommand *renderCommand = Clay_RenderCommandArray_Get(&renderCommands, j);
        Clay_BoundingBox boundingBox = {roundf(renderCommand->boundingBox.x), roundf(renderCommand->boundingBox.y), roundf(renderCommand->boundingBox.width), roundf(renderCommand->boundingBox.height)};
        switch (renderCommand->commandType)
        {
            case CLAY_RENDER_COMMAND_TYPE_TEXT: {
                Clay_TextRenderData *textData = &renderCommand->renderData.text;
                Font fontToUse = fonts[textData->fontId];

                int strlen = textData->stringContents.length + 1;

                if(strlen > temp_render_buffer_len) {
                    // Grow the temp buffer if we need a larger string
                    if(temp_render_buffer) free(temp_render_buffer);
                    temp_render_buffer = (char *) malloc(strlen);
                    temp_render_buffer_len = strlen;
                }

                // Raylib uses standard C strings so isn't compatible with cheap slices, we need to clone the string to append null terminator
                memcpy(temp_render_buffer, textData->stringContents.chars, textData->stringContents.length);
                temp_render_buffer[textData->stringContents.length] = '\0';
                DrawTextEx(fontToUse, temp_render_buffer, (Vector2){boundingBox.x, boundingBox.y}, (float)textData->fontSize, (float)textData->letterSpacing, CLAY_COLOR_TO_RAYLIB_COLOR(textData->textColor));

                break;
            }
            case CLAY_RENDER_COMMAND_TYPE_IMAGE: {
                Texture2D imageTexture = *(Texture2D *)renderCommand->renderData.image.imageData;
                Clay_Color tintColor = renderCommand->renderData.image.backgroundColor;
                if (tintColor.r == 0 && tintColor.g == 0 && tintColor.b == 0 && tintColor.a == 0) {
                    tintColor = (Clay_Color) { 255, 255, 255, 255 };
                }
                DrawTexturePro(
                    imageTexture,
                    (Rectangle) { 0, 0, imageTexture.width, imageTexture.height },
                    (Rectangle){boundingBox.x, boundingBox.y, boundingBox.width, boundingBox.height},
                    (Vector2) {},
                    0,
                    CLAY_COLOR_TO_RAYLIB_COLOR(tintColor));
                break;
            }
            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START: {
                BeginScissorMode((int)roundf(boundingBox.x), (int)roundf(boundingBox.y), (int)roundf(boundingBox.width), (int)roundf(boundingBox.height));
                break;
            }
            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END: {
                EndScissorMode();
                break;
            }
            case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
                Clay_RectangleRenderData *config = &renderCommand->renderData.rectangle;
                if (config->cornerRadius.topLeft > 0) {
                    float radius = (config->cornerRadius.topLeft * 2) / (float)((boundingBox.width > boundingBox.height) ? boundingBox.height : boundingBox.width);
                    DrawRectangleRounded((Rectangle) { boundingBox.x, boundingBox.y, boundingBox.width, boundingBox.height }, radius, 8, CLAY_COLOR_TO_RAYLIB_COLOR(config->backgroundColor));
                } else {
                    DrawRectangle(boundingBox.x, boundingBox.y, boundingBox.width, boundingBox.height, CLAY_COLOR_TO_RAYLIB_COLOR(config->backgroundColor));
                }
                break;
            }
            case CLAY_RENDER_COMMAND_TYPE_BORDER: {
                Clay_BorderRenderData *config = &renderCommand->renderData.border;
                // Left border
                if (config->width.left > 0) {
                    DrawRectangle((int)roundf(boundingBox.x), (int)roundf(boundingBox.y + config->cornerRadius.topLeft), (int)config->width.left, (int)roundf(boundingBox.height - config->cornerRadius.topLeft - config->cornerRadius.bottomLeft), CLAY_COLOR_TO_RAYLIB_COLOR(config->color));
                }
                // Right border
                if (config->width.right > 0) {
                    DrawRectangle((int)roundf(boundingBox.x + boundingBox.width - config->width.right), (int)roundf(boundingBox.y + config->cornerRadius.topRight), (int)config->width.right, (int)roundf(boundingBox.height - config->cornerRadius.topRight - config->cornerRadius.bottomRight), CLAY_COLOR_TO_RAYLIB_COLOR(config->color));
                }
                // Top border
                if (config->width.top > 0) {
                    DrawRectangle((int)roundf(boundingBox.x + config->cornerRadius.topLeft), (int)roundf(boundingBox.y), (int)roundf(boundingBox.width - config->cornerRadius.topLeft - config->cornerRadius.topRight), (int)config->width.top, CLAY_COLOR_TO_RAYLIB_COLOR(config->color));
                }
                // Bottom border
                if (config->width.bottom > 0) {
                    DrawRectangle((int)roundf(boundingBox.x + config->cornerRadius.bottomLeft), (int)roundf(boundingBox.y + boundingBox.height - config->width.bottom), (int)roundf(boundingBox.width - config->cornerRadius.bottomLeft - config->cornerRadius.bottomRight), (int)config->width.bottom, CLAY_COLOR_TO_RAYLIB_COLOR(config->color));
                }
                if (config->cornerRadius.topLeft > 0) {
                    DrawRing((Vector2) { roundf(boundingBox.x + config->cornerRadius.topLeft), roundf(boundingBox.y + config->cornerRadius.topLeft) }, roundf(config->cornerRadius.topLeft - config->width.top), config->cornerRadius.topLeft, 180, 270, 10, CLAY_COLOR_TO_RAYLIB_COLOR(config->color));
                }
                if (config->cornerRadius.topRight > 0) {
                    DrawRing((Vector2) { roundf(boundingBox.x + boundingBox.width - config->cornerRadius.topRight), roundf(boundingBox.y + config->cornerRadius.topRight) }, roundf(config->cornerRadius.topRight - config->width.top), config->cornerRadius.topRight, 270, 360, 10, CLAY_COLOR_TO_RAYLIB_COLOR(config->color));
                }
                if (config->cornerRadius.bottomLeft > 0) {
                    DrawRing((Vector2) { roundf(boundingBox.x + config->cornerRadius.bottomLeft), roundf(boundingBox.y + boundingBox.height - config->cornerRadius.bottomLeft) }, roundf(config->cornerRadius.bottomLeft - config->width.bottom), config->cornerRadius.bottomLeft, 90, 180, 10, CLAY_COLOR_TO_RAYLIB_COLOR(config->color));
                }
                if (config->cornerRadius.bottomRight > 0) {
                    DrawRing((Vector2) { roundf(boundingBox.x + boundingBox.width - config->cornerRadius.bottomRight), roundf(boundingBox.y + boundingBox.height - config->cornerRadius.bottomRight) }, roundf(config->cornerRadius.bottomRight - config->width.bottom), config->cornerRadius.bottomRight, 0.1, 90, 10, CLAY_COLOR_TO_RAYLIB_COLOR(config->color));
                }
                break;
            }
            case CLAY_RENDER_COMMAND_TYPE_CUSTOM: {
                Clay_CustomRenderData *config = &renderCommand->renderData.custom;
                CustomLayoutElement *customElement = (CustomLayoutElement *)config->customData;
                if (!customElement) continue;
                switch (customElement->type) {
                    case CUSTOM_LAYOUT_ELEMENT_TYPE_CHANNEL_PANEL: {
                        CustomLayoutElement_ChannelPanel *panel = &customElement->customData.channel_panel;
                        if (panel->app) {
                            Color color = (panel->channel == 0) ? COLOR_CHANNEL_A : COLOR_CHANNEL_B;
                            render_channel_panels(panel->app, panel->channel,
                                                  boundingBox.x, boundingBox.y,
                                                  boundingBox.width, boundingBox.height, color);
                        }
                        break;
                    }
                    case CUSTOM_LAYOUT_ELEMENT_TYPE_VU_METER: {
                        CustomLayoutElement_VUMeter *vu = &customElement->customData.vu_meter;
                        render_vu_meter(boundingBox.x, boundingBox.y,
                                        boundingBox.width, boundingBox.height,
                                        vu->meter, vu->label, vu->is_clipping_pos,
                                        vu->is_clipping_neg, vu->channel_color);
                        break;
                    }
                    case CUSTOM_LAYOUT_ELEMENT_TYPE_SETTINGS_ICON: {
                        // Simple gear icon drawn with primitives (no font dependency)
                        float cx = boundingBox.x + boundingBox.width * 0.5f;
                        float cy = boundingBox.y + boundingBox.height * 0.5f;
                        float r_outer = (boundingBox.width < boundingBox.height ? boundingBox.width : boundingBox.height) * 0.45f;
                        float r_inner = r_outer * 0.55f;
                        Color col = COLOR_TEXT;

                        DrawCircleLines((int)roundf(cx), (int)roundf(cy), r_outer, col);
                        DrawCircleLines((int)roundf(cx), (int)roundf(cy), r_inner, col);

                        // 8 teeth/spokes
                        for (int i = 0; i < 8; i++) {
                            float a = (float)i * (PI / 4.0f);
                            float x0 = cx + cosf(a) * r_inner;
                            float y0 = cy + sinf(a) * r_inner;
                            float x1 = cx + cosf(a) * r_outer;
                            float y1 = cy + sinf(a) * r_outer;
                            DrawLineEx((Vector2){x0, y0}, (Vector2){x1, y1}, 2.0f, col);
                        }
                        break;
                    }
                    case CUSTOM_LAYOUT_ELEMENT_TYPE_CLOCK_ICON: {
                        // Simple clock icon drawn with primitives (no font dependency)
                        float cx = boundingBox.x + boundingBox.width * 0.5f;
                        float cy = boundingBox.y + boundingBox.height * 0.5f;
                        float radius = (boundingBox.width < boundingBox.height ? boundingBox.width : boundingBox.height) * 0.45f;
                        Color col = COLOR_TEXT;

                        DrawCircleLines((int)roundf(cx), (int)roundf(cy), radius, col);
                        DrawCircle((int)roundf(cx), (int)roundf(cy), 1.8f, col);

                        // Hour hand (roughly 10 o'clock)
                        DrawLineEx((Vector2){cx, cy},
                                   (Vector2){cx - radius * 0.40f, cy - radius * 0.20f},
                                   2.2f, col);

                        // Minute hand (roughly 2 o'clock)
                        DrawLineEx((Vector2){cx, cy},
                                   (Vector2){cx + radius * 0.48f, cy - radius * 0.48f},
                                   2.0f, col);
                        break;
                    }
                    default: break;
                }
                break;
            }
            default: {
                printf("Error: unhandled render command.");
                exit(1);
            }
        }
    }
}
