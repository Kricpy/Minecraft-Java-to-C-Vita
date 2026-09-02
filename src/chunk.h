#ifndef CHUNK_H
#define CHUNK_H

#include "defines.h"
#include <vitaGL.h>

extern uint8_t chunk_blocks[CHUNK_SIZE_X][CHUNK_SIZE_Y][CHUNK_SIZE_Z];
extern BlockState block_registry[4];
extern Vertex *mesh_vertices;
extern uint16_t *mesh_indices;
extern int vertex_count;
extern int index_count;

void init_mesh_buffers();
void free_mesh_buffers();
void generate_chunk();
void build_chunk_mesh();
int is_transparent(int x, int y, int z);

#endif
