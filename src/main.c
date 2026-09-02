#include <psp2/kernel/processmgr.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/sysmem.h>
#include <vitaGL.h>
#include <math.h>

#include "defines.h"
#include "chunk.h"
#include "atlas.h"
#include "player.h"
#include "raycast.h"
#include "renderer.h"

int main() {
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    vglInit(0x800000);
    renderer_init();

    GLuint tex = compile_dynamic_atlas();
    init_mesh_buffers();
    generate_chunk();
    build_chunk_mesh();

    Player player;
    player_init(&player);

    int place_ticks = 0, break_ticks = 0;
    float tick_timer = 0.0f;
    SceUInt64 last_time = sceKernelGetProcessTimeWide();

    while (1) {
        SceCtrlData ctrl;
        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        if (ctrl.buttons & SCE_CTRL_START) break;

        SceUInt64 curr_time = sceKernelGetProcessTimeWide();
        float dt = (float)(curr_time - last_time) / 1000000.0f;
        last_time = curr_time;
        if (dt > 0.1f) dt = 0.1f;

        tick_timer += dt;
        int triggered_tick = 0;
        if (tick_timer >= 0.05f) {
            triggered_tick = 1;
            tick_timer -= 0.05f;
        }
        if (triggered_tick) {
            if (place_ticks > 0) place_ticks--;
            if (break_ticks > 0) break_ticks--;
        }

        player_update(&player, dt);

        float current_eye = player.is_sneaking ? CROUCH_EYE_LEVEL : STANDING_EYE_LEVEL;
        float current_h   = player.is_sneaking ? CROUCH_HEIGHT : STANDING_HEIGHT;
        float rad_yaw     = player.yaw * DEG_TO_RAD;
        float rad_pitch   = player.pitch * DEG_TO_RAD;
        float dir_x = -sinf(rad_yaw) * cosf(rad_pitch);
        float dir_y =  sinf(rad_pitch);
        float dir_z = -cosf(rad_yaw) * cosf(rad_pitch);

        // Use interpolated render position for camera and raycast
        float eyex = player.render_x, eyey = player.render_y + current_eye, eyez = player.render_z;

        int hx=-1, hy=-1, hz=-1, face_hit=-1;
        int hit = perform_raycast(eyex, eyey, eyez,
                                  dir_x, dir_y, dir_z,
                                  &hx, &hy, &hz, &face_hit);

        if ((ctrl.buttons & SCE_CTRL_RTRIGGER) && hit && break_ticks == 0) {
            chunk_blocks[hx][hy][hz] = 0;
            build_chunk_mesh();
            break_ticks = 5;
        }
        if (!(ctrl.buttons & SCE_CTRL_RTRIGGER)) break_ticks = 0;

        if ((ctrl.buttons & SCE_CTRL_LTRIGGER) && hit && place_ticks == 0) {
            int cx=hx, cy=hy, cz=hz;
            if      (face_hit==FACE_BOTTOM) cy--;
            else if (face_hit==FACE_TOP)    cy++;
            else if (face_hit==FACE_BACK)   cz--;
            else if (face_hit==FACE_FRONT)  cz++;
            else if (face_hit==FACE_LEFT)   cx--;
            else if (face_hit==FACE_RIGHT)  cx++;

            if (cx>=0&&cx<CHUNK_SIZE_X&&cy>=0&&cy<CHUNK_SIZE_Y&&cz>=0&&cz<CHUNK_SIZE_Z) {
                // Use physics position for placement overlap check (collision)
                float bx0=(float)cx,   bx1=(float)cx+1.0f;
                float by0=(float)cy,   by1=(float)cy+1.0f;
                float bz0=(float)cz,   bz1=(float)cz+1.0f;
                float px0=player.x-PLAYER_HALF_WIDTH, px1=player.x+PLAYER_HALF_WIDTH;
                float py0=player.y, py1=player.y+current_h;
                float pz0=player.z-PLAYER_HALF_WIDTH, pz1=player.z+PLAYER_HALF_WIDTH;
                int overlap = (px0<bx1&&px1>bx0&&py0<by1&&py1>by0&&pz0<bz1&&pz1>bz0);
                if (!overlap && chunk_blocks[cx][cy][cz]==0) {
                    chunk_blocks[cx][cy][cz] = 3;
                    build_chunk_mesh();
                    place_ticks = 4;
                }
            }
        }
        if (!(ctrl.buttons & SCE_CTRL_LTRIGGER)) place_ticks = 0;

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        float fov = player.is_sprinting ? 77.0f : 70.0f;
        set_perspective(fov, 960.0f/544.0f, 0.05f, 100.0f);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glRotatef(-player.pitch, 1,0,0);
        glRotatef(-player.yaw,   0,1,0);
        glTranslatef(-player.render_x, -(player.render_y+current_eye), -player.render_z);

        glBindTexture(GL_TEXTURE_2D, tex);
        draw_chunk();

        if (hit) draw_selection((float)hx, (float)hy, (float)hz);

        draw_crosshair();

        vglSwapBuffers(GL_TRUE);
    }

    free_mesh_buffers();
    sceKernelExitProcess(0);
    return 0;
}
