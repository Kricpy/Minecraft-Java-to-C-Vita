#ifndef CHUNK_H
#define CHUNK_H

#include "defines.h"
#include <vitaGL.h>

#define MAX_CHUNKS (WORLD_SIZE_X * WORLD_SIZE_Z)

typedef struct {
    uint16_t blocks[CHUNK_SIZE_X * CHUNK_SIZE_Y * CHUNK_SIZE_Z];
    uint8_t  biome[CHUNK_SIZE_X * CHUNK_SIZE_Z];
    uint16_t sections;
    uint8_t  loaded;
    int      x_off;
    int      z_off;
    Vertex  *vertices;
    uint16_t *indices;
    int      vertex_count;
    int      index_count;
    int      vert_cap;
    int      idx_cap;
    uint8_t  dirty;
    /* Incremental mesh building state */
    int      rebuild_phase;       // 0=idle, 1=counting faces, 2=building faces
    int      rebuild_x, rebuild_y, rebuild_z;
    int      rebuild_faces_added;
    int      rebuild_est_faces;
    int      rebuild_face_limit;
} Chunk;

extern Chunk world_chunks[WORLD_SIZE_X][WORLD_SIZE_Z];

static inline uint16_t pack_state(uint16_t block_id, uint8_t meta) {
    return (uint16_t)((block_id << 4) | (meta & 0xF));
}
static inline uint16_t block_id_of(uint16_t packed)  { return packed >> 4; }
static inline uint8_t  meta_of(uint16_t packed)     { return packed & 0xF; }

int  chunk_is_solid(int wx, int wy, int wz);
int  chunk_is_translucent(int wx, int wy, int wz);
int  chunk_is_air(int wx, int wy, int wz);
void chunk_set_block(int wx, int wy, int wz, uint16_t packed);
uint16_t chunk_get_block(int wx, int wy, int wz);

void chunk_init(Chunk *c);
void chunk_free_mesh(Chunk *c);
void chunk_rebuild_mesh(Chunk *c);
void chunk_mark_dirty(int cx, int cz);
void chunk_decode(int chunk_x, int chunk_z, int full_chunk,
                  int available_sections,
                  const uint8_t *data, size_t data_len);
void clear_all_chunks(void);

#endif