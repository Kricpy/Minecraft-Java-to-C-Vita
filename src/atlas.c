#include "atlas.h"
#include "defines.h"
#include "renderer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "network.h"

#define STB_IMAGE_IMPLEMENTATION
#include "../include/stb_image.h"

GLuint g_atlas_tex = 0;

/* Vita accepts both "ux0:data/..." and "ux0/data/..." path styles.
   Try the colon form first, then the slash form, so textures load
   regardless of which convention the user's install uses. */
static unsigned char *load_tex(const char *base, const char *name,
                                int *w, int *h, int *ch) {
    char path[300];
    snprintf(path, sizeof(path), "%s%s", base, name);
    unsigned char *d = stbi_load(path, w, h, ch, 4);
    if (d) { log_debug("atlas: loaded %s (%dx%d)", path, *w, *h); return d; }
    /* Try slash form as fallback */
    char path2[300];
    const char *slash = strchr(base, ':');
    if (slash) {
        size_t pre = (size_t)(slash - base);
        snprintf(path2, sizeof(path2), "%.*s/%s", (int)pre, base, name);
        d = stbi_load(path2, w, h, ch, 4);
        if (d) { log_debug("atlas: loaded %s (%dx%d)", path2, *w, *h); return d; }
    }
    log_debug("atlas: no PNG for %s", name);
    return NULL;
}

static void paste_texture(unsigned char *atlas_data, const char *name,
                           int col, int tint_grayscale, const char *base) {
    int w, h, ch;
    unsigned char *data = load_tex(base, name, &w, &h, &ch);
    int start_x   = col * TILE_SIZE;
    int atlas_width = ATLAS_COLS * TILE_SIZE;

    for (int ty = 0; ty < TILE_SIZE; ty++)
        for (int tx = 0; tx < TILE_SIZE; tx++) {
            int di = ((ty * atlas_width) + (start_x + tx)) * 4;
            unsigned char r, g, b;
            if (data) {
                int si = ((ty * TILE_SIZE) + tx) * 4;
                r = data[si+0]; g = data[si+1]; b = data[si+2];
                if (tint_grayscale) {
                    float intensity = (float)r / 255.0f;
                    r = (unsigned char)(intensity * 92.0f);
                    g = (unsigned char)(intensity * 180.0f);
                    b = (unsigned char)(intensity * 74.0f);
                }
            } else {
                /* Fallback solid colors so blocks are never invisible */
                switch (col) {
                    case TEX_COL_STONE:       r=128; g=128; b=128; break;
                    case TEX_COL_GRASS_SIDE:  r=124; g=180; b=74;  break;
                    case TEX_COL_GRASS_TOP:   r=150; g=210; b=80;  break;
                    case TEX_COL_DIRT:        r=134; g=96;  b=67;  break;
                    case TEX_COL_WOOD:        r=160; g=120; b=70;  break;
                    case TEX_COL_LEAF:        r=34;  g=139; b=34;  break;
                    case TEX_COL_BEDROCK:     r=85;  g=85;  b=85;  break;
                    case TEX_COL_SAND:        r=218; g=196; b=130; break;
                    case TEX_COL_WATER:       r=60;  g=100; b=180; break;
                    case TEX_COL_LAVA:        r=180; g=60;  b=20;  break;
                    default:                  r=200; g=200; b=200; break;
                }
            }
            atlas_data[di+0] = r;
            atlas_data[di+1] = g;
            atlas_data[di+2] = b;
            atlas_data[di+3] = 255;
        }
    if (data) stbi_image_free(data);
}

GLuint compile_dynamic_atlas() {
    int atlas_w = ATLAS_COLS * TILE_SIZE;
    int atlas_h = ATLAS_ROWS * TILE_SIZE;
    unsigned char *pixels = (unsigned char*)malloc(atlas_w * atlas_h * 4);
    memset(pixels, 0, atlas_w * atlas_h * 4);

    const char *base = "ux0:/Minecraft/1.12.2/assets/minecraft/textures/blocks/";

    paste_texture(pixels, "dirt.png",       TEX_COL_DIRT,       0, base);
    paste_texture(pixels, "grass_side.png", TEX_COL_GRASS_SIDE, 0, base);
    paste_texture(pixels, "grass_top.png",  TEX_COL_GRASS_TOP,  1, base);
    paste_texture(pixels, "stone.png",      TEX_COL_STONE,      0, base);

    log_debug("atlas: compiled %dx%d texture", atlas_w, atlas_h);

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, atlas_w, atlas_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    free(pixels);
    return tex;
}