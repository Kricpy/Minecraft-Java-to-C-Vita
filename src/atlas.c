#include "atlas.h"
#include "defines.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define STB_IMAGE_IMPLEMENTATION
#include "../include/stb_image.h"

static void paste_texture(unsigned char *atlas_data, const char *path,
                           int col, int tint_grayscale) {
    int w, h, channels;
    unsigned char *data = stbi_load(path, &w, &h, &channels, 4);
    int start_x   = col * TILE_SIZE;
    int atlas_width = ATLAS_COLS * TILE_SIZE;

    if (!data) {
        for (int ty = 0; ty < TILE_SIZE; ty++)
            for (int tx = 0; tx < TILE_SIZE; tx++) {
                int di = ((ty * atlas_width) + (start_x + tx)) * 4;
                atlas_data[di+0] = (col == TEX_COL_STONE) ? 128 : 255;
                atlas_data[di+1] = (col == TEX_COL_GRASS_TOP) ? 255 : 0;
                atlas_data[di+2] = 0;
                atlas_data[di+3] = 255;
            }
        return;
    }

    for (int ty = 0; ty < TILE_SIZE; ty++)
        for (int tx = 0; tx < TILE_SIZE; tx++) {
            int si = ((ty * TILE_SIZE) + tx) * 4;
            int di = ((ty * atlas_width) + (start_x + tx)) * 4;
            unsigned char r = data[si+0], g = data[si+1],
                          b = data[si+2], a = data[si+3];
            if (tint_grayscale) {
                float intensity = (float)r / 255.0f;
                r = (unsigned char)(intensity * 92.0f);
                g = (unsigned char)(intensity * 180.0f);
                b = (unsigned char)(intensity * 74.0f);
                a = 255;
            }
            atlas_data[di+0] = r;
            atlas_data[di+1] = g;
            atlas_data[di+2] = b;
            atlas_data[di+3] = a;
        }
    stbi_image_free(data);
}

GLuint compile_dynamic_atlas() {
    int atlas_w = ATLAS_COLS * TILE_SIZE;
    int atlas_h = ATLAS_ROWS * TILE_SIZE;
    unsigned char *pixels = (unsigned char*)malloc(atlas_w * atlas_h * 4);
    memset(pixels, 0, atlas_w * atlas_h * 4);

    const char *base = "ux0:data/Minecraft/1.12.2/assets/textures/blocks/";
    char path[256];

    snprintf(path, sizeof(path), "%sdirt.png",       base);
    paste_texture(pixels, path, TEX_COL_DIRT, 0);
    snprintf(path, sizeof(path), "%sgrass_side.png",  base);
    paste_texture(pixels, path, TEX_COL_GRASS_SIDE, 0);
    snprintf(path, sizeof(path), "%sgrass_top.png",   base);
    paste_texture(pixels, path, TEX_COL_GRASS_TOP, 1);
    snprintf(path, sizeof(path), "%sstone.png",       base);
    paste_texture(pixels, path, TEX_COL_STONE, 0);

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
