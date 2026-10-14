#ifndef PLAYER_H
#define PLAYER_H

#include "defines.h"

typedef struct {
    // Physics position (used for collision only)
    float x, y, z;
    float vx, vy, vz;
    // Interpolated render position (smooth)
    float render_x, render_y, render_z;
    // Camera
    float yaw, pitch;
    // State
    int on_ground;
    int is_sprinting;
    int is_sneaking;
    int jump_queued;
    int jump_pressed_last; // edge detection for jump
    // Physics tick accumulator
    float tick_accum;
    // Double-tap sprint detection
    int   fwd_tap_count;
    float fwd_tap_timer;
    int   fwd_held_last;
    // Health/death
    int health;
    int max_health;
    int dead;
    int respawn_timer;
} Player;

void player_init(Player *p);
void player_update(Player *p, float dt);
int  check_collision(float px, float py, float pz, float height);
int  is_supported_by_floor(float px, float py, float pz);

#endif
