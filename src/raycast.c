#include "raycast.h"
#include "chunk.h"
#include <math.h>

int perform_raycast(float px, float py, float pz,
                    float dx, float dy, float dz,
                    int *hx, int *hy, int *hz, int *fd) {
    float d = 0.0f;
    int lbx=-1, lby=-1, lbz=-1;
    while (d < MINECRAFT_SURVIVAL_REACH) {
        int bx=(int)(px+dx*d);
        int by=(int)(py+dy*d);
        int bz=(int)(pz+dz*d);
        if (bx>=0&&bx<CHUNK_SIZE_X&&by>=0&&by<CHUNK_SIZE_Y&&bz>=0&&bz<CHUNK_SIZE_Z) {
            if (block_registry[chunk_blocks[bx][by][bz]].is_solid) {
                *hx=bx; *hy=by; *hz=bz;
                if (lbx != -1) {
                    if      (lby>by) *fd=FACE_TOP;
                    else if (lby<by) *fd=FACE_BOTTOM;
                    else if (lbx>bx) *fd=FACE_RIGHT;
                    else if (lbx<bx) *fd=FACE_LEFT;
                    else if (lbz>bz) *fd=FACE_FRONT;
                    else             *fd=FACE_BACK;
                } else { *fd=FACE_TOP; }
                return 1;
            }
        }
        lbx=bx; lby=by; lbz=bz;
        d += 0.01f;
    }
    return 0;
}
