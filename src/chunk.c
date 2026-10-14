#include "chunk.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "network.h"

Chunk world_chunks[WORLD_SIZE_X][WORLD_SIZE_Z];

/* ------------------------------------------------------------------ */
/* Block registry - 1.12.2 block IDs (state = (id<<4)|meta)          */
/* face_textures order: {front, back, left, right, top, bottom}      */
/* ------------------------------------------------------------------ */
static const BlockState block_registry[] = {
    { 0,  "minecraft:air",         0, 1, { 0,0,0,0,0,0 } },
    { 1,  "minecraft:stone",       1, 0, { TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE } },
    { 2,  "minecraft:grass",       1, 0, { TEX_COL_GRASS_SIDE,TEX_COL_GRASS_SIDE,TEX_COL_GRASS_SIDE,TEX_COL_GRASS_SIDE,TEX_COL_GRASS_TOP,TEX_COL_DIRT } },
    { 3,  "minecraft:dirt",        1, 0, { TEX_COL_DIRT,TEX_COL_DIRT,TEX_COL_DIRT,TEX_COL_DIRT,TEX_COL_DIRT,TEX_COL_DIRT } },
    { 4,  "minecraft:cobblestone", 1, 0, { TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE } },
    { 5,  "minecraft:planks",      1, 0, { TEX_COL_WOOD,TEX_COL_WOOD,TEX_COL_WOOD,TEX_COL_WOOD,TEX_COL_WOOD,TEX_COL_WOOD } },
    { 6,  "minecraft:sapling",     0, 1, { TEX_COL_LEAF,TEX_COL_LEAF,TEX_COL_LEAF,TEX_COL_LEAF,TEX_COL_LEAF,TEX_COL_LEAF } },
    { 7,  "minecraft:bedrock",     1, 0, { TEX_COL_BEDROCK,TEX_COL_BEDROCK,TEX_COL_BEDROCK,TEX_COL_BEDROCK,TEX_COL_BEDROCK,TEX_COL_BEDROCK } },
    { 8,  "minecraft:flowing_water", 0, 1, { TEX_COL_WATER,TEX_COL_WATER,TEX_COL_WATER,TEX_COL_WATER,TEX_COL_WATER,TEX_COL_WATER } },
    { 9,  "minecraft:water",       0, 1, { TEX_COL_WATER,TEX_COL_WATER,TEX_COL_WATER,TEX_COL_WATER,TEX_COL_WATER,TEX_COL_WATER } },
    { 10, "minecraft:flowing_lava", 0, 1, { TEX_COL_LAVA,TEX_COL_LAVA,TEX_COL_LAVA,TEX_COL_LAVA,TEX_COL_LAVA,TEX_COL_LAVA } },
    { 11, "minecraft:lava",        0, 1, { TEX_COL_LAVA,TEX_COL_LAVA,TEX_COL_LAVA,TEX_COL_LAVA,TEX_COL_LAVA,TEX_COL_LAVA } },
    { 12, "minecraft:sand",        1, 0, { TEX_COL_SAND,TEX_COL_SAND,TEX_COL_SAND,TEX_COL_SAND,TEX_COL_SAND,TEX_COL_SAND } },
    { 13, "minecraft:gravel",      1, 0, { TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE } },
    { 14, "minecraft:gold_ore",    1, 0, { TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE } },
    { 15, "minecraft:iron_ore",    1, 0, { TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE } },
    { 16, "minecraft:coal_ore",    1, 0, { TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE } },
    { 17, "minecraft:log",         1, 0, { TEX_COL_WOOD_SIDE,TEX_COL_WOOD_SIDE,TEX_COL_WOOD_SIDE,TEX_COL_WOOD_SIDE,TEX_COL_WOOD_TOP,TEX_COL_WOOD_TOP } },
    { 18, "minecraft:leaves",      1, 1, { TEX_COL_LEAF,TEX_COL_LEAF,TEX_COL_LEAF,TEX_COL_LEAF,TEX_COL_LEAF,TEX_COL_LEAF } },
    { 19, "minecraft:sponge",      1, 0, { TEX_COL_SAND,TEX_COL_SAND,TEX_COL_SAND,TEX_COL_SAND,TEX_COL_SAND,TEX_COL_SAND } },
    { 20, "minecraft:glass",       0, 1, { TEX_COL_WATER,TEX_COL_WATER,TEX_COL_WATER,TEX_COL_WATER,TEX_COL_WATER,TEX_COL_WATER } },
    { 21, "minecraft:lapis_ore",   1, 0, { TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE } },
    { 22, "minecraft:lapis_block", 1, 0, { TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE } },
    { 24, "minecraft:sandstone",   1, 0, { TEX_COL_SAND,TEX_COL_SAND,TEX_COL_SAND,TEX_COL_SAND,TEX_COL_SAND,TEX_COL_SAND } },
    { 31, "minecraft:tall_grass",  0, 1, { TEX_COL_GRASS_SIDE,TEX_COL_GRASS_SIDE,TEX_COL_GRASS_SIDE,TEX_COL_GRASS_SIDE,TEX_COL_GRASS_SIDE,TEX_COL_GRASS_SIDE } },
    { 33, "minecraft:piston",      1, 0, { TEX_COL_WOOD,TEX_COL_WOOD,TEX_COL_WOOD,TEX_COL_WOOD,TEX_COL_WOOD_TOP,TEX_COL_WOOD_TOP } },
    { 35, "minecraft:wool",        1, 0, { TEX_COL_WOOD,TEX_COL_WOOD,TEX_COL_WOOD,TEX_COL_WOOD,TEX_COL_WOOD,TEX_COL_WOOD } },
    { 37, "minecraft:dandelion",   0, 1, { TEX_COL_GRASS_SIDE,TEX_COL_GRASS_SIDE,TEX_COL_GRASS_SIDE,TEX_COL_GRASS_SIDE,TEX_COL_GRASS_SIDE,TEX_COL_GRASS_SIDE } },
    { 38, "minecraft:poppy",       0, 1, { TEX_COL_GRASS_SIDE,TEX_COL_GRASS_SIDE,TEX_COL_GRASS_SIDE,TEX_COL_GRASS_SIDE,TEX_COL_GRASS_SIDE,TEX_COL_GRASS_SIDE } },
    { 39, "minecraft:brown_mushroom", 0, 1, { TEX_COL_DIRT,TEX_COL_DIRT,TEX_COL_DIRT,TEX_COL_DIRT,TEX_COL_DIRT,TEX_COL_DIRT } },
    { 40, "minecraft:red_mushroom", 0, 1, { TEX_COL_DIRT,TEX_COL_DIRT,TEX_COL_DIRT,TEX_COL_DIRT,TEX_COL_DIRT,TEX_COL_DIRT } },
    { 41, "minecraft:gold_block",  1, 0, { TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE } },
    { 42, "minecraft:iron_block",  1, 0, { TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE } },
    { 45, "minecraft:brick_block", 1, 0, { TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE } },
    { 47, "minecraft:bookshelf",   1, 0, { TEX_COL_WOOD,TEX_COL_WOOD,TEX_COL_WOOD,TEX_COL_WOOD,TEX_COL_WOOD,TEX_COL_WOOD } },
    { 48, "minecraft:mossy_cobblestone", 1, 0, { TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE } },
    { 49, "minecraft:obsidian",    1, 0, { TEX_COL_BEDROCK,TEX_COL_BEDROCK,TEX_COL_BEDROCK,TEX_COL_BEDROCK,TEX_COL_BEDROCK,TEX_COL_BEDROCK } },
    { 50, "minecraft:torch",       0, 1, { TEX_COL_WOOD,TEX_COL_WOOD,TEX_COL_WOOD,TEX_COL_WOOD,TEX_COL_WOOD,TEX_COL_WOOD } },
    { 52, "minecraft:mob_spawner", 1, 0, { TEX_COL_BEDROCK,TEX_COL_BEDROCK,TEX_COL_BEDROCK,TEX_COL_BEDROCK,TEX_COL_BEDROCK,TEX_COL_BEDROCK } },
    { 53, "minecraft:oak_stairs",  1, 0, { TEX_COL_WOOD,TEX_COL_WOOD,TEX_COL_WOOD,TEX_COL_WOOD,TEX_COL_WOOD,TEX_COL_WOOD } },
    { 54, "minecraft:chest",       1, 0, { TEX_COL_WOOD,TEX_COL_WOOD,TEX_COL_WOOD,TEX_COL_WOOD,TEX_COL_WOOD,TEX_COL_WOOD } },
    { 56, "minecraft:diamond_ore", 1, 0, { TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE } },
    { 57, "minecraft:diamond_block", 1, 0, { TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE } },
    { 58, "minecraft:crafting_table", 1, 0, { TEX_COL_WOOD,TEX_COL_WOOD,TEX_COL_WOOD,TEX_COL_WOOD,TEX_COL_WOOD_TOP,TEX_COL_WOOD_TOP } },
    { 79, "minecraft:ice",         0, 1, { TEX_COL_WATER,TEX_COL_WATER,TEX_COL_WATER,TEX_COL_WATER,TEX_COL_WATER,TEX_COL_WATER } },
    { 81, "minecraft:cactus",      1, 0, { TEX_COL_GRASS_SIDE,TEX_COL_GRASS_SIDE,TEX_COL_GRASS_SIDE,TEX_COL_GRASS_SIDE,TEX_COL_GRASS_SIDE,TEX_COL_GRASS_SIDE } },
    { 82, "minecraft:clay",        1, 0, { TEX_COL_DIRT,TEX_COL_DIRT,TEX_COL_DIRT,TEX_COL_DIRT,TEX_COL_DIRT,TEX_COL_DIRT } },
    { 83, "minecraft:sugar_cane",  0, 1, { TEX_COL_GRASS_SIDE,TEX_COL_GRASS_SIDE,TEX_COL_GRASS_SIDE,TEX_COL_GRASS_SIDE,TEX_COL_GRASS_SIDE,TEX_COL_GRASS_SIDE } },
    { 87, "minecraft:netherrack",  1, 0, { TEX_COL_BEDROCK,TEX_COL_BEDROCK,TEX_COL_BEDROCK,TEX_COL_BEDROCK,TEX_COL_BEDROCK,TEX_COL_BEDROCK } },
    { 88, "minecraft:soul_sand",   1, 0, { TEX_COL_SAND,TEX_COL_SAND,TEX_COL_SAND,TEX_COL_SAND,TEX_COL_SAND,TEX_COL_SAND } },
    { 89, "minecraft:glowstone",   1, 0, { TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE } },
    { 98, "minecraft:stone_bricks", 1, 0, { TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE } },
    { 101, "minecraft:iron_bars",  0, 1, { TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE } },
    { 102, "minecraft:glass_pane", 0, 1, { TEX_COL_WATER,TEX_COL_WATER,TEX_COL_WATER,TEX_COL_WATER,TEX_COL_WATER,TEX_COL_WATER } },
    { 112, "minecraft:nether_brick", 1, 0, { TEX_COL_BEDROCK,TEX_COL_BEDROCK,TEX_COL_BEDROCK,TEX_COL_BEDROCK,TEX_COL_BEDROCK,TEX_COL_BEDROCK } },
    { 121, "minecraft:end_stone",  1, 0, { TEX_COL_SAND,TEX_COL_SAND,TEX_COL_SAND,TEX_COL_SAND,TEX_COL_SAND,TEX_COL_SAND } },
    { 153, "minecraft:quartz_ore", 1, 0, { TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE,TEX_COL_STONE } },
    { 161, "minecraft:acacia_leaves", 1, 1, { TEX_COL_LEAF,TEX_COL_LEAF,TEX_COL_LEAF,TEX_COL_LEAF,TEX_COL_LEAF,TEX_COL_LEAF } },
    { 162, "minecraft:acacia_log", 1, 0, { TEX_COL_WOOD_SIDE,TEX_COL_WOOD_SIDE,TEX_COL_WOOD_SIDE,TEX_COL_WOOD_SIDE,TEX_COL_WOOD_TOP,TEX_COL_WOOD_TOP } },
    { 170, "minecraft:hay_block",  1, 0, { TEX_COL_GRASS_TOP,TEX_COL_GRASS_TOP,TEX_COL_GRASS_TOP,TEX_COL_GRASS_TOP,TEX_COL_GRASS_TOP,TEX_COL_GRASS_TOP } },
    { 172, "minecraft:hardened_clay", 1, 0, { TEX_COL_DIRT,TEX_COL_DIRT,TEX_COL_DIRT,TEX_COL_DIRT,TEX_COL_DIRT,TEX_COL_DIRT } },
    { 173, "minecraft:coal_block", 1, 0, { TEX_COL_BEDROCK,TEX_COL_BEDROCK,TEX_COL_BEDROCK,TEX_COL_BEDROCK,TEX_COL_BEDROCK,TEX_COL_BEDROCK } },
    { 174, "minecraft:packed_ice", 0, 1, { TEX_COL_WATER,TEX_COL_WATER,TEX_COL_WATER,TEX_COL_WATER,TEX_COL_WATER,TEX_COL_WATER } },
};
#define BLOCK_REGISTRY_COUNT (int)(sizeof(block_registry)/sizeof(block_registry[0]))

static const BlockState *find_block(uint16_t packed) {
    uint16_t bid = block_id_of(packed);
    for (int i = 0; i < BLOCK_REGISTRY_COUNT; i++)
        if (block_registry[i].id == bid) return &block_registry[i];
    return &block_registry[0];
}

/* ------------------------------------------------------------------ */
/* Chunk window index mapping                                         */
/* ------------------------------------------------------------------ */
static int chunk_window_index(int wx, int wz, int *ix, int *iz) {
    *ix = (wx >> 4) - g_net.origin_cx + WORLD_SIZE_X / 2;
    *iz = (wz >> 4) - g_net.origin_cz + WORLD_SIZE_Z / 2;
    return (*ix >= 0 && *ix < WORLD_SIZE_X && *iz >= 0 && *iz < WORLD_SIZE_Z);
}

/* ------------------------------------------------------------------ */
/* Neighbor transparency check for face culling                       */
/* ------------------------------------------------------------------ */
static int neighbor_transparent(int wx, int wy, int wz, int face_dir) {
    int nx = wx, ny = wy, nz = wz;
    switch (face_dir) {
        case FACE_FRONT: nz++; break;
        case FACE_BACK:  nz--; break;
        case FACE_LEFT:  nx--; break;
        case FACE_RIGHT: nx++; break;
        case FACE_TOP:   ny++; break;
        case FACE_BOTTOM: ny--; break;
    }
    if (ny < 0 || ny >= CHUNK_SIZE_Z) return 1;
    int ix, iz;
    if (!chunk_window_index(nx, nz, &ix, &iz)) return 1;
    Chunk *nc = &world_chunks[ix][iz];
    if (!nc->loaded) return 1; /* unloaded = assume transparent (render face) */
    uint16_t packed = nc->blocks[(ny << 8) | ((nz & 15) << 4) | (nx & 15)];
    if (block_id_of(packed) == 0) return 1;
    return find_block(packed)->is_translucent;
}

/* ------------------------------------------------------------------ */
/* Chunk mesh management                                               */
/* ------------------------------------------------------------------ */
static uint16_t static_palette[4096];

void chunk_init(Chunk *c) {
    memset(c, 0, sizeof(*c));
    for (int i = 0; i < CHUNK_SIZE_X*CHUNK_SIZE_Y*CHUNK_SIZE_Z; i++)
        c->blocks[i] = 0;
}

void chunk_free_mesh(Chunk *c) {
    free(c->vertices); c->vertices = NULL;
    free(c->indices);  c->indices  = NULL;
    c->vertex_count = c->index_count = 0;
    c->vert_cap = c->idx_cap = 0;
}

static void ensure_cap(Chunk *c, int need_v, int need_i) {
    if (need_v > c->vert_cap) {
        c->vert_cap = need_v * 2;
        if (!c->vertices) c->vertices = (Vertex*)malloc(c->vert_cap * sizeof(Vertex));
        else c->vertices = (Vertex*)realloc(c->vertices, c->vert_cap * sizeof(Vertex));
    }
    if (need_i > c->idx_cap) {
        c->idx_cap = need_i * 2;
        if (!c->indices) c->indices = (uint16_t*)malloc(c->idx_cap * sizeof(uint16_t));
        else c->indices = (uint16_t*)realloc(c->indices, c->idx_cap * sizeof(uint16_t));
    }
}

static void add_face(Chunk *c, int x, int y, int z, FaceDir face, uint16_t packed) {
    const BlockState *bs = find_block(packed);
    uint8_t col = bs->face_textures[face == FACE_FRONT ? 0 :
                                    face == FACE_BACK  ? 1 :
                                    face == FACE_LEFT  ? 2 :
                                    face == FACE_RIGHT ? 3 :
                                    face == FACE_TOP   ? 4 : 5];
    float u0 = (float)col / (float)ATLAS_COLS;
    float u1 = (float)(col + 1) / (float)ATLAS_COLS;
    float v0 = 0.0f, v1 = 1.0f;
    uint16_t s = (uint16_t)c->vertex_count;
    if (c->index_count + 6 > 65530) return;
    ensure_cap(c, c->vertex_count + 4, c->index_count + 6);
    switch (face) {
        case FACE_FRONT:
            c->vertices[c->vertex_count++] = (Vertex){(float)x,   (float)y,   (float)z+1, u0, v1};
            c->vertices[c->vertex_count++] = (Vertex){(float)x+1, (float)y,   (float)z+1, u1, v1};
            c->vertices[c->vertex_count++] = (Vertex){(float)x+1, (float)y+1, (float)z+1, u1, v0};
            c->vertices[c->vertex_count++] = (Vertex){(float)x,   (float)y+1, (float)z+1, u0, v0};
            break;
        case FACE_BACK:
            c->vertices[c->vertex_count++] = (Vertex){(float)x+1, (float)y,   (float)z,   u0, v1};
            c->vertices[c->vertex_count++] = (Vertex){(float)x,   (float)y,   (float)z,   u1, v1};
            c->vertices[c->vertex_count++] = (Vertex){(float)x,   (float)y+1, (float)z,   u1, v0};
            c->vertices[c->vertex_count++] = (Vertex){(float)x+1, (float)y+1, (float)z,   u0, v0};
            break;
        case FACE_LEFT:
            c->vertices[c->vertex_count++] = (Vertex){(float)x,   (float)y,   (float)z,   u0, v1};
            c->vertices[c->vertex_count++] = (Vertex){(float)x,   (float)y,   (float)z+1, u1, v1};
            c->vertices[c->vertex_count++] = (Vertex){(float)x,   (float)y+1, (float)z+1, u1, v0};
            c->vertices[c->vertex_count++] = (Vertex){(float)x,   (float)y+1, (float)z,   u0, v0};
            break;
        case FACE_RIGHT:
            c->vertices[c->vertex_count++] = (Vertex){(float)x+1, (float)y,   (float)z+1, u0, v1};
            c->vertices[c->vertex_count++] = (Vertex){(float)x+1, (float)y,   (float)z,   u1, v1};
            c->vertices[c->vertex_count++] = (Vertex){(float)x+1, (float)y+1, (float)z,   u1, v0};
            c->vertices[c->vertex_count++] = (Vertex){(float)x+1, (float)y+1, (float)z+1, u0, v0};
            break;
        case FACE_TOP:
            c->vertices[c->vertex_count++] = (Vertex){(float)x,   (float)y+1, (float)z+1, u0, v1};
            c->vertices[c->vertex_count++] = (Vertex){(float)x+1, (float)y+1, (float)z+1, u1, v1};
            c->vertices[c->vertex_count++] = (Vertex){(float)x+1, (float)y+1, (float)z,   u1, v0};
            c->vertices[c->vertex_count++] = (Vertex){(float)x,   (float)y+1, (float)z,   u0, v0};
            break;
        case FACE_BOTTOM:
            c->vertices[c->vertex_count++] = (Vertex){(float)x,   (float)y,   (float)z,   u0, v1};
            c->vertices[c->vertex_count++] = (Vertex){(float)x+1, (float)y,   (float)z,   u1, v1};
            c->vertices[c->vertex_count++] = (Vertex){(float)x+1, (float)y,   (float)z+1, u1, v0};
            c->vertices[c->vertex_count++] = (Vertex){(float)x,   (float)y,   (float)z+1, u0, v0};
            break;
    }
    c->indices[c->index_count++] = s;
    c->indices[c->index_count++] = s+1;
    c->indices[c->index_count++] = s+2;
    c->indices[c->index_count++] = s;
    c->indices[c->index_count++] = s+2;
    c->indices[c->index_count++] = s+3;
}

void chunk_rebuild_mesh(Chunk *c) {
    // One-pass mesh builder. VitaGL's fixed-function pipeline can't handle
    // 400k+ draw calls per frame, so cap faces per chunk. Memory is the hard
    // limit: Vita has ~96MB total and 256 chunks must share it, so use a
    // FIXED per-chunk buffer that is never grown. 2000 faces = 8000 verts
    // (128KB) + 12000 indices (24KB) = ~152KB/chunk; 256 chunks = 39MB.
    const int MAX_FACES = 2000;
    const int VCAP = 8192;
    const int ICAP = 12288;
    if (!c->dirty || !c->loaded) return;

    // Free old buffers and allocate the fixed cap. Never realloc larger.
    free(c->vertices); c->vertices = NULL; c->vert_cap = 0;
    free(c->indices);  c->indices  = NULL; c->idx_cap  = 0;
    c->vertices = (Vertex*)malloc(VCAP * sizeof(Vertex));
    c->indices  = (uint16_t*)malloc(ICAP * sizeof(uint16_t));
    c->vert_cap = VCAP;
    c->idx_cap  = ICAP;

    c->vertex_count = 0;
    c->index_count  = 0;
    int faces = 0;

    for (int x = 0; x < CHUNK_SIZE_X && faces < MAX_FACES; x++) {
        for (int y = 0; y < CHUNK_SIZE_Z && faces < MAX_FACES; y++) {
            for (int z = 0; z < CHUNK_SIZE_X && faces < MAX_FACES; z++) {
                uint16_t packed = c->blocks[(y<<8)|(z<<4)|x];
                if (block_id_of(packed) == 0) continue;
                const BlockState *bs = find_block(packed);
                if (!bs->is_solid && !bs->is_translucent) continue;
                if (neighbor_transparent(c->x_off+x, y, c->z_off+z, FACE_FRONT)) { add_face(c,x,y,z,FACE_FRONT,packed); faces++; }
                if (faces >= MAX_FACES) break;
                if (neighbor_transparent(c->x_off+x, y, c->z_off+z, FACE_BACK)) { add_face(c,x,y,z,FACE_BACK,packed); faces++; }
                if (faces >= MAX_FACES) break;
                if (neighbor_transparent(c->x_off+x, y, c->z_off+z, FACE_LEFT)) { add_face(c,x,y,z,FACE_LEFT,packed); faces++; }
                if (faces >= MAX_FACES) break;
                if (neighbor_transparent(c->x_off+x, y, c->z_off+z, FACE_RIGHT)) { add_face(c,x,y,z,FACE_RIGHT,packed); faces++; }
                if (faces >= MAX_FACES) break;
                if (neighbor_transparent(c->x_off+x, y, c->z_off+z, FACE_TOP)) { add_face(c,x,y,z,FACE_TOP,packed); faces++; }
                if (faces >= MAX_FACES) break;
                if (neighbor_transparent(c->x_off+x, y, c->z_off+z, FACE_BOTTOM)) { add_face(c,x,y,z,FACE_BOTTOM,packed); faces++; }
            }
        }
    }

    c->dirty = 0;
    c->rebuild_phase = 0;
    c->rebuild_est_faces = 0;
    c->rebuild_face_limit = 0;
    c->rebuild_faces_added = 0;
    c->rebuild_x = c->rebuild_y = c->rebuild_z = 0;
}

void chunk_mark_dirty(int cx, int cz) {
    if (cx>=0&&cx<WORLD_SIZE_X&&cz>=0&&cz<WORLD_SIZE_Z)
        world_chunks[cx][cz].dirty = 1;
}

/* ------------------------------------------------------------------ */
/* Block queries                                                       */
/* ------------------------------------------------------------------ */
uint16_t chunk_get_block(int wx, int wy, int wz) {
    if (wy < 0 || wy >= CHUNK_SIZE_Z) return 0;
    int ix, iz;
    if (!chunk_window_index(wx, wz, &ix, &iz)) return 0;
    return world_chunks[ix][iz].blocks[(wy << 8) | ((wz & 15) << 4) | (wx & 15)];
}

void chunk_set_block(int wx, int wy, int wz, uint16_t packed) {
    if (wy < 0 || wy >= CHUNK_SIZE_Z) return;
    int ix, iz;
    if (!chunk_window_index(wx, wz, &ix, &iz)) return;
    world_chunks[ix][iz].blocks[(wy << 8) | ((wz & 15) << 4) | (wx & 15)] = packed;
    world_chunks[ix][iz].dirty = 1;
}

int chunk_is_solid(int wx, int wy, int wz) {
    if (wy < 0 || wy >= CHUNK_SIZE_Z) return 0;
    int ix, iz;
    if (!chunk_window_index(wx, wz, &ix, &iz)) return 1;
    Chunk *c = &world_chunks[ix][iz];
    if (!c->loaded) return 1;
    uint16_t packed = c->blocks[(wy << 8) | ((wz & 15) << 4) | (wx & 15)];
    if (block_id_of(packed) == 0) return 0;
    return find_block(packed)->is_solid;
}

int chunk_is_air(int wx, int wy, int wz) {
    if (wy < 0 || wy >= CHUNK_SIZE_Z) return 1;
    int ix, iz;
    if (!chunk_window_index(wx, wz, &ix, &iz)) return 1;
    Chunk *c = &world_chunks[ix][iz];
    if (!c->loaded) return 1;
    return block_id_of(chunk_get_block(wx, wy, wz)) == 0;
}

int chunk_is_translucent(int x, int y, int z) {
    if (y < 0 || y >= CHUNK_SIZE_Z) return 1;
    int ix, iz;
    if (!chunk_window_index(x, z, &ix, &iz)) return 1;
    Chunk *c = &world_chunks[ix][iz];
    if (!c->loaded) return 1;
    uint16_t packed = c->blocks[(y << 8) | ((z & 15) << 4) | (x & 15)];
    if (block_id_of(packed) == 0) return 1;
    return find_block(packed)->is_translucent;
}

/* ------------------------------------------------------------------ */
/* 1.12.2 chunk section decoding (paletted container, protocol 340)    */
/* ------------------------------------------------------------------ */
static uint64_t rd_u64be(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

void chunk_decode(int chunk_x, int chunk_z, int full_chunk,
                  int available_sections,
                  const uint8_t *data, size_t data_len) {
    int ix = chunk_x - g_net.origin_cx + WORLD_SIZE_X / 2;
    int iz = chunk_z - g_net.origin_cz + WORLD_SIZE_Z / 2;
    if (ix < 0 || ix >= WORLD_SIZE_X || iz < 0 || iz >= WORLD_SIZE_Z) return;

    Chunk *c = &world_chunks[ix][iz];
    c->x_off = chunk_x * 16;
    c->z_off = chunk_z * 16;

    log_debug("chunk_decode: chunk=(%d,%d) full=%d sections=0x%X data_len=%u", chunk_x, chunk_z, full_chunk, available_sections, (unsigned)data_len);

    int total_blocks = 0;
    int non_air_blocks = 0;

    size_t p = 0;
    for (int sy = 0; sy < 16; sy++) {
        if (!((available_sections >> sy) & 1)) continue;
        if (p >= data_len) break;

        int bits = data[p++];
        if (bits < 1 || bits > 32) {
            log_debug("chunk_decode: invalid bits=%d at section %d", bits, sy);
            break;
        }

        uint16_t palette[4096];
        int palette_count = 0;
        if (bits <= 8) {
            RCUR r = { data, data_len, p };
            int32_t cnt = rc_varint(&r);
            p = r.p;
            if (cnt < 0 || cnt > 4096) {
                log_debug("chunk_decode: invalid palette count %d at section %d", cnt, sy);
                return;
            }
            for (int i = 0; i < cnt; i++) {
                RCUR r2 = { data, data_len, p };
                int32_t v = rc_varint(&r2);
                p = r2.p;
                palette[i] = (uint16_t)(v & 0xFFFF);
            }
            palette_count = cnt;
        }

        RCUR r = { data, data_len, p };
        int32_t nlongs = rc_varint(&r);
        p = r.p;
        if (nlongs < 0 || p + (size_t)nlongs * 8 > data_len) {
            log_debug("chunk_decode: invalid nlongs=%d at section %d", nlongs, sy);
            return;
        }

        const uint8_t *longs = data + p;
        uint64_t mask = (bits >= 64) ? ~(uint64_t)0 : ((uint64_t)1 << bits) - 1;

        int section_blocks = 0;
        for (int i = 0; i < 4096; i++) {
            int bitpos = i * bits;
            int j = bitpos >> 6;
            int l = bitpos & 63;
            int k = ((i + 1) * bits - 1) >> 6;
            uint64_t val;
            if (j == k) {
                val = (rd_u64be(longs + j * 8) >> l) & mask;
            } else {
                val = ((rd_u64be(longs + j * 8) >> l) |
                       (rd_u64be(longs + k * 8) << (64 - l))) & mask;
            }
            uint16_t packed;
            if (bits > 8) {
                packed = (uint16_t)(val & 0xFFFF);
            } else {
                packed = ((int)val < palette_count) ? palette[(int)val] : 0;
            }
            int x = i & 15;
            int z = (i >> 4) & 15;
            int y = i >> 8;
            c->blocks[((sy * 16 + y) << 8) | (z << 4) | x] = packed;
            total_blocks++;
            if (block_id_of(packed) != 0) {
                non_air_blocks++;
            }
        }
        p += (size_t)nlongs * 8;

        if (full_chunk) {
            p += 2048; /* block light */
            p += 2048; /* sky light */
        }
        if (p > data_len) break;
    }

    log_debug("chunk_decode: done chunk=(%d,%d) total_blocks=%d non_air=%d", chunk_x, chunk_z, total_blocks, non_air_blocks);

    if (full_chunk) p += 256; /* biomes */

    c->loaded = 1;
    c->dirty = 1;
}