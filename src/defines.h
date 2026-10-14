#ifndef DEFINES_H
#define DEFINES_H

#include <vitaGL.h>

#define CHUNK_SIZE_X 16
#define CHUNK_SIZE_Y 16
#define CHUNK_SIZE_Z 256
#define WORLD_SIZE_X 16
#define WORLD_SIZE_Z 16
#define DEG_TO_RAD 0.0174532925f

#define STANDING_EYE_LEVEL 1.62f
#define CROUCH_EYE_LEVEL 1.27f
#define STANDING_HEIGHT 1.8f
#define CROUCH_HEIGHT 1.65f

#define TILE_SIZE 16
#define ATLAS_COLS 16
#define ATLAS_ROWS 16

#define PLAYER_HALF_WIDTH     0.3f
#define STICK_DEADZONE        16.0f
#define MINECRAFT_SURVIVAL_REACH 6.0f

#define TEX_COL_STONE 0
#define TEX_COL_GRASS_SIDE 1
#define TEX_COL_GRASS_TOP 2
#define TEX_COL_DIRT 3
#define TEX_COL_WOOD 4
#define TEX_COL_WOOD_SIDE 4
#define TEX_COL_WOOD_TOP 4
#define TEX_COL_LEAF 5
#define TEX_COL_BEDROCK 6
#define TEX_COL_SAND 7
#define TEX_COL_WATER 8
#define TEX_COL_LAVA 9

typedef enum {
    FACE_FRONT,
    FACE_BACK,
    FACE_LEFT,
    FACE_RIGHT,
    FACE_TOP,
    FACE_BOTTOM
} FaceDir;

typedef struct {
    float x, y, z;
    float u, v;
} Vertex;

typedef struct {
    float x, y, z;
} Vec3;

typedef struct {
    uint16_t id;
    const char *name;
    uint8_t is_solid;
    uint8_t is_translucent;
    uint8_t face_textures[6];
} BlockState;

#endif
