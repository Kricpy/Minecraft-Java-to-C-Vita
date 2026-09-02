#ifndef RAYCAST_H
#define RAYCAST_H

#include "defines.h"

int perform_raycast(float px, float py, float pz,
                    float dx, float dy, float dz,
                    int *hx, int *hy, int *hz, int *fd);

#endif
