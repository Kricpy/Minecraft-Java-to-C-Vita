#include "player.h"
#include "chunk.h"
#include <psp2/ctrl.h>
#include <math.h>

// Exact vanilla 1.12.2 values from Minecraft source
#define TICK_DT          0.05f      // 1/20 second per tick
#define GRAVITY_ACCEL    0.08f      // blocks per tick subtracted each tick
#define GRAVITY_DRAG     0.98f      // vertical velocity multiplied each tick
#define JUMP_VELOCITY    0.42f      // exact from getJumpUpwardsMotion()
#define GROUND_FRICTION  0.546f     // standard block friction (slipperiness 0.6 * 0.91)
#define WALK_ACCEL       0.098f     // base ground acceleration per tick
#define AIR_ACCEL_MULT   0.0204f  // vanilla air = 2% of ground (moveRelative 0.02F factor)
#define SPRINT_MULT      1.3f       // sprint is 30% faster
#define SNEAK_MULT       0.3f       // sneak is 30% of walk

#define DOUBLE_TAP_WINDOW 0.35f

void player_init(Player *p) {
    p->x = 8.0f; p->y = 6.0f; p->z = 8.0f;
    p->vx = p->vy = p->vz = 0.0f;
    p->render_x = p->x; p->render_y = p->y; p->render_z = p->z;
    p->yaw = 0.0f; p->pitch = 0.0f;
    p->on_ground = 0;
    p->is_sprinting = 0;
    p->is_sneaking = 0;
    p->jump_queued = 0;
    p->jump_pressed_last = 0;
    p->tick_accum = 0.0f;
    p->fwd_tap_count = 0;
    p->fwd_tap_timer = 0.0f;
    p->fwd_held_last = 0;
}

int check_collision(float px, float py, float pz, float height) {
    // Use floor for block coords — much more reliable than roundf
    int minX = (int)floorf(px - PLAYER_HALF_WIDTH);
    int maxX = (int)floorf(px + PLAYER_HALF_WIDTH);
    int minY = (int)floorf(py);
    int maxY = (int)floorf(py + height - 0.001f);
    int minZ = (int)floorf(pz - PLAYER_HALF_WIDTH);
    int maxZ = (int)floorf(pz + PLAYER_HALF_WIDTH);

    for (int x = minX; x <= maxX; x++)
    for (int y = minY; y <= maxY; y++)
    for (int z = minZ; z <= maxZ; z++) {
        if (x<0||x>=CHUNK_SIZE_X||y<0||y>=CHUNK_SIZE_Y||z<0||z>=CHUNK_SIZE_Z)
            continue;
        uint8_t id = chunk_blocks[x][y][z];
        if (id == 0 || id >= 4 || !block_registry[id].is_solid) continue;
        // Block occupies [x, x+1] x [y, y+1] x [z, z+1]
        float bx0=(float)x, bx1=(float)x+1.0f;
        float by0=(float)y, by1=(float)y+1.0f;
        float bz0=(float)z, bz1=(float)z+1.0f;
        float px0=px-PLAYER_HALF_WIDTH, px1=px+PLAYER_HALF_WIDTH;
        float py0=py,                   py1=py+height;
        float pz0=pz-PLAYER_HALF_WIDTH, pz1=pz+PLAYER_HALF_WIDTH;
        if (px0<bx1&&px1>bx0&&py0<by1&&py1>by0&&pz0<bz1&&pz1>bz0)
            return 1;
    }
    return 0;
}

int is_supported_by_floor(float px, float py, float pz) {
    float p_minX=px-PLAYER_HALF_WIDTH, p_maxX=px+PLAYER_HALF_WIDTH;
    float p_minZ=pz-PLAYER_HALF_WIDTH, p_maxZ=pz+PLAYER_HALF_WIDTH;
    float checkY = py - 0.05f;
    int cy = (int)floorf(checkY);
    if (cy<0||cy>=CHUNK_SIZE_Y) return 0;
    for (int x=(int)floorf(p_minX); x<=(int)floorf(p_maxX); x++)
    for (int z=(int)floorf(p_minZ); z<=(int)floorf(p_maxZ); z++) {
        if (x<0||x>=CHUNK_SIZE_X||z<0||z>=CHUNK_SIZE_Z) continue;
        uint8_t id = chunk_blocks[x][cy][z];
        if (id==0||id>=4||!block_registry[id].is_solid) continue;
        float bx0=(float)x, bx1=(float)x+1.0f;
        float by1=(float)cy+1.0f;
        float bz0=(float)z, bz1=(float)z+1.0f;
        if (p_minX<bx1&&p_maxX>bx0&&
            p_minZ<bz1&&p_maxZ>bz0&&
            checkY<by1) return 1;
    }
    return 0;
}

// One discrete physics tick — exactly mirrors Minecraft's EntityLivingBase
// Returns previous position via out params for interpolation
static void physics_tick(Player *p, float kf, float ks, int sneak,
                          float *prev_x, float *prev_y, float *prev_z) {
    float height = sneak ? CROUCH_HEIGHT : STANDING_HEIGHT;
    *prev_x = p->x;
    *prev_y = p->y;
    *prev_z = p->z;

    // === VANILLA 1.12.2 MOVEMENT MODEL ===

    // Step 1: Gravity (vanilla always applies, skipped on ground to prevent bounce)
    if (!p->on_ground) {
        p->vy -= GRAVITY_ACCEL;   // 0.08 per tick
        p->vy *= GRAVITY_DRAG;    // 0.98 per tick
        if (fabsf(p->vy) < 0.005f && p->vy < 0.0f) p->vy = 0.0f;
    } else {
        p->vy = 0.0f;
    }

    // Step 2: Y movement + collision
    // Vanilla: Entity moves Y, resolves collision, snaps to surface, sets onGround
    int just_landed = 0;
    {
        float old_y = p->y;
        p->y += p->vy;

        if (check_collision(p->x, p->y, p->z, height)) {
            if (p->vy < 0.0f) {
                // Landing - binary search for collision surface (handles any fall speed)
                float high_y = old_y;  // clear position
                float low_y = p->y;    // collision position
                for (int i = 0; i < 8; i++) {
                    float mid = (high_y + low_y) * 0.5f;
                    if (check_collision(p->x, mid, p->z, height))
                        low_y = mid;
                    else
                        high_y = mid;
                }
                // Snap to block surface (top of block feet touched)
                p->y = (float)((int)floorf(high_y - 0.001f)) + 1.0f;
                p->vy = 0.0f;
                p->on_ground = 1;
                just_landed = 1;
            } else {
                // Ceiling
                p->y = old_y;
                p->vy = 0.0f;
            }
        } else if (p->vy != 0.0f) {
            p->on_ground = 0;
        }
        // vy == 0 and no collision: standing on ground boundary, keep on_ground
    }

    // Step 3: Jump impulse
    // Vanilla: key handler checks onGround AFTER move() sets it, sets motionY = 0.42,
    // then the same tick's move() applies it. We re-apply Y movement after jump fires.
    int on_ground_before_jump = p->on_ground;  // Save for friction/air factor after jump clears it
    if (p->jump_queued && p->on_ground) {
        p->vy = JUMP_VELOCITY;  // 0.42
        p->jump_queued = 0;
        p->on_ground = 0;  // Jump takes you airborne

        // Re-apply Y movement with jump velocity THIS tick (vanilla does in same move() call)
        float old_y2 = p->y;
        p->y += p->vy;
        if (check_collision(p->x, p->y, p->z, height)) {
            // Jump hits ceiling (too close to block above)
            p->y = old_y2;
            p->vy = 0.0f;
        }
    }

    // Step 4: Ground detection for friction + air factor
    // on_ground_before_jump: player was on ground when jump fired this tick
    // just_landed: landed this tick
    // p->on_ground: currently on ground
    // Any of these = treat as "on ground" for friction/air factor
    int is_on_ground = p->on_ground || on_ground_before_jump || just_landed;

    // Step 5: Friction (vanilla: ground only, no air friction)
    if (is_on_ground) {
        p->vx *= GROUND_FRICTION;  // 0.546
        p->vz *= GROUND_FRICTION;
    }

    // Step 6: Directional speed (vanilla adds speed directly, no acceleration)
    // EntityPlayer.speed = 0.1F. Sprint = 1.3x. Sneak = 0.3x.
    float speed = 0.1f;
    if (p->is_sprinting && !sneak) speed = 0.13f;
    if (sneak)                      speed = 0.03f;

    float rad_yaw = p->yaw * DEG_TO_RAD;
    float dcy = cosf(rad_yaw), dsy = sinf(rad_yaw);
    float wish_x = -dsy * kf + dcy * ks;
    float wish_z = -dcy * kf - dsy * ks;
    float wish_len = sqrtf(wish_x * wish_x + wish_z * wish_z);
    if (wish_len > 0.0f) {
        wish_x /= wish_len;
        wish_z /= wish_len;
    }
    // Air factor: vanilla applies 0.02x when not on ground
    if (!is_on_ground) {
        wish_x *= 0.02f;
        wish_z *= 0.02f;
    }
    p->vx += wish_x * speed;
    p->vz += wish_z * speed;

    // Step 7: X axis movement + collision
    float old_x = p->x;
    p->x += p->vx;
    if (sneak && is_on_ground && !is_supported_by_floor(p->x, p->y, p->z)) {
        p->x = old_x; p->vx = 0.0f;
    }
    if (check_collision(p->x, p->y, p->z, height)) {
        p->x = old_x; p->vx = 0.0f;
        p->is_sprinting = 0;
    }

    // Step 8: Z axis movement + collision
    float old_z = p->z;
    p->z += p->vz;
    if (sneak && is_on_ground && !is_supported_by_floor(p->x, p->y, p->z)) {
        p->z = old_z; p->vz = 0.0f;
    }
    if (check_collision(p->x, p->y, p->z, height)) {
        p->z = old_z; p->vz = 0.0f;
        p->is_sprinting = 0;
    }

    // Step 9: Edge detection
    if (p->on_ground && !is_supported_by_floor(p->x, p->y, p->z)) {
        p->on_ground = 0;
    }
}

void player_update(Player *p, float dt) {
    SceCtrlData ctrl;
    sceCtrlPeekBufferPositive(0, &ctrl, 1);

    // Camera — runs every frame for smooth look
    float rx = (float)ctrl.rx - 128.0f;
    float ry = (float)ctrl.ry - 128.0f;
    if (fabsf(rx) >= STICK_DEADZONE) p->yaw   -= (rx/128.0f) * 120.0f * dt;
    if (fabsf(ry) >= STICK_DEADZONE) p->pitch  -= (ry/128.0f) * 90.0f * dt;
    if (p->pitch >  89.0f) p->pitch =  89.0f;
    if (p->pitch < -89.0f) p->pitch = -89.0f;

    // Read stick
    float lx = (float)ctrl.lx - 128.0f;
    float ly = (float)ctrl.ly - 128.0f;
    float kf = 0.0f, ks = 0.0f;
    if      (ly < -STICK_DEADZONE) kf =  1.0f;
    else if (ly >  STICK_DEADZONE) kf = -1.0f;
    if      (lx < -STICK_DEADZONE) ks = -1.0f;
    else if (lx >  STICK_DEADZONE) ks =  1.0f;

    int sneak    = (ctrl.buttons & SCE_CTRL_SQUARE) ? 1 : 0;
    int fwd_held = (kf > 0.5f) ? 1 : 0;

    // Jump — only trigger on press edge, not while held
    // This prevents double jumps when the button is held across multiple ticks
    int jump_pressed = (ctrl.buttons & SCE_CTRL_CROSS) ? 1 : 0;
    if (jump_pressed && !p->jump_pressed_last) {
        p->jump_queued = 1;
    }
    p->jump_pressed_last = jump_pressed;

    // Double-tap forward to sprint
    p->fwd_tap_timer -= dt;
    if (p->fwd_tap_timer < 0.0f) {
        p->fwd_tap_timer = 0.0f;
        p->fwd_tap_count = 0;
    }
    if (fwd_held && !p->fwd_held_last) {
        p->fwd_tap_count++;
        p->fwd_tap_timer = DOUBLE_TAP_WINDOW;
        if (p->fwd_tap_count >= 2 && !sneak) {
            p->is_sprinting = 1;
            p->fwd_tap_count = 0;
        }
    }
    p->fwd_held_last = fwd_held;

    // Stop sprinting
    if (!fwd_held || sneak) p->is_sprinting = 0;

    p->is_sneaking = sneak;

    // Fixed tick accumulator — run as many ticks as have elapsed
    p->tick_accum += dt;
    while (p->tick_accum >= TICK_DT) {
        float prev_x, prev_y, prev_z;
        physics_tick(p, kf, ks, sneak, &prev_x, &prev_y, &prev_z);
        // Update render position to latest prev after each tick
        // so interpolation always uses the most recent interval
        p->render_x = prev_x;
        p->render_y = prev_y;
        p->render_z = prev_z;
        p->tick_accum -= TICK_DT;
    }
    // Interpolate render position between ticks for smooth movement
    float alpha = p->tick_accum / TICK_DT;
    p->render_x += (p->x - p->render_x) * alpha;
    p->render_y += (p->y - p->render_y) * alpha;
    p->render_z += (p->z - p->render_z) * alpha;
}
