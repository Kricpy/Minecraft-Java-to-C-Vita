#ifndef RENDERER_H
#define RENDERER_H

#include "defines.h"
#include <vitaGL.h>

void renderer_init();
void set_perspective(float fovy, float aspect, float near, float far);
void draw_selection(float x, float y, float z);
void draw_crosshair();
void draw_chunk();
void draw_all_chunks(float px, float pz);

extern float g_modl[16];
extern GLuint g_atlas_tex;

#endif