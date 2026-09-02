#ifndef DEFINES_H
#define DEFINES_H

#include <stdint.h>
#include <math.h>

#define DEG_TO_RAD (3.14159265f / 180.0f)
#define STICK_DEADZONE 50

#define MINECRAFT_SURVIVAL_REACH 4.5f
#define GRAVITY -22.0f
#define JUMP_FORCE 8.2f

#define BASE_WALK_SPEED 4.317f
#define BASE_SPRINT_SPEED 5.612f
#define BASE_CROUCH_SPEED 1.31f

#define PLAYER_HALF_WIDTH 0.3f
#define STANDING_HEIGHT 1.8f
#define STANDING_EYE_LEVEL 1.62f
#define CROUCH_HEIGHT 1.65f
#define CROUCH_EYE_LEVEL 1.54f

#define CHUNK_SIZE_X 16
#define CHUNK_SIZE_Y 16
#define CHUNK_SIZE_Z 16

#define ATLAS_COLS 4
#define ATLAS_ROWS 1
#define TILE_SIZE 16

#define TEX_COL_DIRT       0
#define TEX_COL_GRASS_SIDE 1
#define TEX_COL_GRASS_TOP  2
#define TEX_COL_STONE      3

typedef enum {
    FACE_BOTTOM = 0,
    FACE_TOP    = 1,
    FACE_BACK   = 2,
    FACE_FRONT  = 3,
    FACE_LEFT   = 4,
    FACE_RIGHT  = 5
} FaceDir;

typedef struct {
    float x, y, z;
    float u, v;
} Vertex;

typedef struct {
    uint16_t id;
    const char *name;
    uint8_t is_solid;
    uint8_t face_textures[6];
} BlockState;

#endif
