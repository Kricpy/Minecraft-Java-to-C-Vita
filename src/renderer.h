#ifndef RENDERER_H
#define RENDERER_H

#include "defines.h"
#include <vitaGL.h>

void renderer_init();
void set_perspective(float fovy, float aspect, float near, float far);
void draw_selection(float x, float y, float z);
void draw_crosshair();
void draw_chunk();

#endif
