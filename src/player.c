#include "player.h"
#include "chunk.h"
#include "network.h"
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

static inline void sanitize_player(Player *p) {
    if (!isfinite(p->x)) p->x = 8.0f;
    if (!isfinite(p->y)) p->y = 64.0f;
    if (!isfinite(p->z)) p->z = 8.0f;
    if (!isfinite(p->vx)) p->vx = 0.0f;
    if (!isfinite(p->vy)) p->vy = 0.0f;
    if (!isfinite(p->vz)) p->vz = 0.0f;
    if (!isfinite(p->render_x)) p->render_x = p->x;
    if (!isfinite(p->render_y)) p->render_y = p->y;
    if (!isfinite(p->render_z)) p->render_z = p->z;
    if (!isfinite(p->yaw)) p->yaw = 0.0f;
    if (!isfinite(p->pitch)) p->pitch = 0.0f;
    
    // Clamp to reasonable bounds
    const float MAX_POS = 30000000.0f; // Minecraft world border
    const float MAX_VEL = 100.0f;
    p->x = fmaxf(fminf(p->x, MAX_POS), -MAX_POS);
    p->y = fmaxf(fminf(p->y, MAX_POS), -MAX_POS);
    p->z = fmaxf(fminf(p->z, MAX_POS), -MAX_POS);
    p->vx = fmaxf(fminf(p->vx, MAX_VEL), -MAX_VEL);
    p->vy = fmaxf(fminf(p->vy, MAX_VEL), -MAX_VEL);
    p->vz = fmaxf(fminf(p->vz, MAX_VEL), -MAX_VEL);
    
    // Pitch clamp
    if (p->pitch > 89.0f) p->pitch = 89.0f;
    if (p->pitch < -89.0f) p->pitch = -89.0f;
}

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
    p->health = 20;
    p->max_health = 20;
    p->dead = 0;
    p->respawn_timer = 0;
}

int check_collision(float px, float py, float pz, float height) {
    int minX = (int)floorf(px - PLAYER_HALF_WIDTH);
    int maxX = (int)floorf(px + PLAYER_HALF_WIDTH);
    int minY = (int)floorf(py);
    int maxY = (int)floorf(py + height - 0.001f);
    int minZ = (int)floorf(pz - PLAYER_HALF_WIDTH);
    int maxZ = (int)floorf(pz + PLAYER_HALF_WIDTH);

    for (int x = minX; x <= maxX; x++)
    for (int y = minY; y <= maxY; y++)
    for (int z = minZ; z <= maxZ; z++) {
        if (!chunk_is_solid(x, y, z)) continue;
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
    float checkY = py - 0.05f;
    int cy = (int)floorf(checkY);
    if (cy < 0 || cy >= CHUNK_SIZE_Z) return 0;
    for (int x=(int)floorf(px-PLAYER_HALF_WIDTH); x<=(int)floorf(px+PLAYER_HALF_WIDTH); x++)
    for (int z=(int)floorf(pz-PLAYER_HALF_WIDTH); z<=(int)floorf(pz+PLAYER_HALF_WIDTH); z++) {
        if (chunk_is_solid(x, cy, z)) return 1;
    }
    return 0;
}

// One discrete physics tick — exactly mirrors Minecraft's EntityLivingBase
// Returns previous position via out params for interpolation
static void physics_tick(Player *p, float kf, float ks, int sneak,
                          float *prev_x, float *prev_y, float *prev_z) {
    sanitize_player(p);
    
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
                p->y = (float)(int)floorf(high_y + 0.001f);
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
    /* Exact 1.12.2 moveRelative: motionX = strafe*cos - forward*sin,
       motionZ = forward*cos + strafe*sin.  strafe>0 = LEFT. */
    float wish_x = -dsy * kf + dcy * ks;
    float wish_z =  dcy * kf + dsy * ks;
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
    sanitize_player(p);
    
    SceCtrlData ctrl;
    sceCtrlPeekBufferPositive(0, &ctrl, 1);

    // Camera — runs every frame for smooth look
    // Minecraft: yaw increases = turn right, pitch increases = look down
    float rx = (float)ctrl.rx - 128.0f;
    float ry = (float)ctrl.ry - 128.0f;
    if (fabsf(rx) >= STICK_DEADZONE) p->yaw   += (rx/128.0f) * 120.0f * dt;
    if (fabsf(ry) >= STICK_DEADZONE) p->pitch += (ry/128.0f) * 90.0f * dt;
    if (p->pitch >  89.0f) p->pitch =  89.0f;
    if (p->pitch < -89.0f) p->pitch = -89.0f;

    // Read stick — map to WASD-style movement.
    // ly = forward/backward (W/S), lx = left/right (A/D).
    // Use a large deadzone + analog scaling so small stick
    // deflections don't produce full-step diagonal jitter.
    float lx = (float)ctrl.lx - 128.0f;
    float ly = (float)ctrl.ly - 128.0f;
    float kf = 0.0f, ks = 0.0f;
    if (ly < -STICK_DEADZONE) kf =  (-ly - STICK_DEADZONE) / (127.0f - STICK_DEADZONE);
    else if (ly >  STICK_DEADZONE) kf = -(ly - STICK_DEADZONE) / (127.0f - STICK_DEADZONE);
    if (lx < -STICK_DEADZONE) ks =  (-lx - STICK_DEADZONE) / (127.0f - STICK_DEADZONE);
    else if (lx >  STICK_DEADZONE) ks = -(lx - STICK_DEADZONE) / (127.0f - STICK_DEADZONE);
    if (kf >  1.0f) kf =  1.0f;
    if (kf < -1.0f) kf = -1.0f;
    if (ks >  1.0f) ks =  1.0f;
    if (ks < -1.0f) ks = -1.0f;

    int sneak    = (ctrl.buttons & SCE_CTRL_SQUARE) ? 1 : 0;
    int fwd_held = (kf > 0.5f) ? 1 : 0;

    // Jump — only trigger on press edge, not while held.
    // Guard against lag-induced spam: with high latency the same key
    // press can be sampled across multiple physics ticks, each setting
    // jump_queued. When the player lands, all queued jumps fire in
    // sequence and the server sees a bounce ("jumping in air"). Only
    // accept a new jump when not already queued AND not airborne.
    int jump_pressed = (ctrl.buttons & SCE_CTRL_CROSS) ? 1 : 0;
    if (jump_pressed && !p->jump_pressed_last && !p->jump_queued && p->on_ground) {
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

    // Death/respawn handling
    if (p->health <= 0 && !p->dead) {
        p->dead = 1;
        p->respawn_timer = 0;
        log_debug("player: died, health=%d", p->health);
    }
    if (p->dead) {
        p->respawn_timer++;
        /* Don't auto-respawn if respawn screen is being shown */
        extern int show_respawn;
        if (p->respawn_timer >= 40 && !show_respawn) { // 2 seconds at 20 ticks/sec
            extern void net_send_respawn(void);
            net_send_respawn();
            p->dead = 0;
            p->health = p->max_health;
            p->respawn_timer = 0;
            log_debug("player: auto-respawn sent");
        }
    }
}
