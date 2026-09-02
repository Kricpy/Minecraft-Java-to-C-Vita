#include "chunk.h"
#include <stdlib.h>

BlockState block_registry[4] = {
    {0, "minecraft:air",   0, {0, 0, 0, 0, 0, 0}},
    {1, "minecraft:dirt",  1, {TEX_COL_DIRT, TEX_COL_DIRT, TEX_COL_DIRT, TEX_COL_DIRT, TEX_COL_DIRT, TEX_COL_DIRT}},
    {2, "minecraft:grass", 1, {TEX_COL_GRASS_SIDE, TEX_COL_GRASS_SIDE, TEX_COL_GRASS_SIDE, TEX_COL_GRASS_SIDE, TEX_COL_GRASS_TOP, TEX_COL_DIRT}},
    {3, "minecraft:stone", 1, {TEX_COL_STONE, TEX_COL_STONE, TEX_COL_STONE, TEX_COL_STONE, TEX_COL_STONE, TEX_COL_STONE}}
};

uint8_t chunk_blocks[CHUNK_SIZE_X][CHUNK_SIZE_Y][CHUNK_SIZE_Z];

Vertex   *mesh_vertices = NULL;
uint16_t *mesh_indices  = NULL;
int vertex_count = 0;
int index_count  = 0;
int max_vertices = 0;
int max_indices  = 0;

void init_mesh_buffers() {
    max_vertices = 16384;
    max_indices  = 24576;
    mesh_vertices = (Vertex*)malloc(max_vertices * sizeof(Vertex));
    mesh_indices  = (uint16_t*)malloc(max_indices * sizeof(uint16_t));
}

void free_mesh_buffers() {
    free(mesh_vertices);
    free(mesh_indices);
}

static void grow_mesh_buffers() {
    max_vertices *= 2;
    max_indices  *= 2;
    mesh_vertices = (Vertex*)realloc(mesh_vertices, max_vertices * sizeof(Vertex));
    mesh_indices  = (uint16_t*)realloc(mesh_indices, max_indices * sizeof(uint16_t));
}

void generate_chunk() {
    for (int x = 0; x < CHUNK_SIZE_X; x++) {
        for (int z = 0; z < CHUNK_SIZE_Z; z++) {
            for (int y = 0; y < CHUNK_SIZE_Y; y++) {
                if (y == 0)      chunk_blocks[x][y][z] = 3;
                else if (y < 4)  chunk_blocks[x][y][z] = 1;
                else if (y == 4) chunk_blocks[x][y][z] = 2;
                else             chunk_blocks[x][y][z] = 0;
            }
        }
    }
}

int is_transparent(int x, int y, int z) {
    if (x < 0 || x >= CHUNK_SIZE_X ||
        y < 0 || y >= CHUNK_SIZE_Y ||
        z < 0 || z >= CHUNK_SIZE_Z) return 1;
    uint8_t id = chunk_blocks[x][y][z];
    return (id < 4) ? !block_registry[id].is_solid : 1;
}

static void get_block_uvs(uint8_t block_id, FaceDir face,
                           float *u0, float *v0, float *u1, float *v1) {
    int col = 0;
    if (block_id < 4) {
        int fi = (face == FACE_FRONT)  ? 0 :
                 (face == FACE_BACK)   ? 1 :
                 (face == FACE_LEFT)   ? 2 :
                 (face == FACE_RIGHT)  ? 3 :
                 (face == FACE_TOP)    ? 4 : 5;
        col = block_registry[block_id].face_textures[fi];
    }
    *u0 = (float)col       / (float)ATLAS_COLS;
    *v0 = 0.0f;
    *u1 = (float)(col + 1) / (float)ATLAS_COLS;
    *v1 = 1.0f;
}

static void add_face(float x, float y, float z, FaceDir face, uint8_t id) {
    if (vertex_count + 4 >= max_vertices || index_count + 6 >= max_indices)
        grow_mesh_buffers();

    float u0, v0, u1, v1;
    get_block_uvs(id, face, &u0, &v0, &u1, &v1);
    uint16_t s = (uint16_t)vertex_count;

    switch (face) {
        case FACE_FRONT:
            mesh_vertices[vertex_count++] = (Vertex){x,     y,     z+1,  u0, v1};
            mesh_vertices[vertex_count++] = (Vertex){x+1,   y,     z+1,  u1, v1};
            mesh_vertices[vertex_count++] = (Vertex){x+1,   y+1,   z+1,  u1, v0};
            mesh_vertices[vertex_count++] = (Vertex){x,     y+1,   z+1,  u0, v0};
            break;
        case FACE_BACK:
            mesh_vertices[vertex_count++] = (Vertex){x+1,   y,     z,    u0, v1};
            mesh_vertices[vertex_count++] = (Vertex){x,     y,     z,    u1, v1};
            mesh_vertices[vertex_count++] = (Vertex){x,     y+1,   z,    u1, v0};
            mesh_vertices[vertex_count++] = (Vertex){x+1,   y+1,   z,    u0, v0};
            break;
        case FACE_LEFT:
            mesh_vertices[vertex_count++] = (Vertex){x,     y,     z,    u0, v1};
            mesh_vertices[vertex_count++] = (Vertex){x,     y,     z+1,  u1, v1};
            mesh_vertices[vertex_count++] = (Vertex){x,     y+1,   z+1,  u1, v0};
            mesh_vertices[vertex_count++] = (Vertex){x,     y+1,   z,    u0, v0};
            break;
        case FACE_RIGHT:
            mesh_vertices[vertex_count++] = (Vertex){x+1,   y,     z+1,  u0, v1};
            mesh_vertices[vertex_count++] = (Vertex){x+1,   y,     z,    u1, v1};
            mesh_vertices[vertex_count++] = (Vertex){x+1,   y+1,   z,    u1, v0};
            mesh_vertices[vertex_count++] = (Vertex){x+1,   y+1,   z+1,  u0, v0};
            break;
        case FACE_TOP:
            mesh_vertices[vertex_count++] = (Vertex){x,     y+1,   z+1,  u0, v1};
            mesh_vertices[vertex_count++] = (Vertex){x+1,   y+1,   z+1,  u1, v1};
            mesh_vertices[vertex_count++] = (Vertex){x+1,   y+1,   z,    u1, v0};
            mesh_vertices[vertex_count++] = (Vertex){x,     y+1,   z,    u0, v0};
            break;
        case FACE_BOTTOM:
            mesh_vertices[vertex_count++] = (Vertex){x,     y,     z,    u0, v1};
            mesh_vertices[vertex_count++] = (Vertex){x+1,   y,     z,    u1, v1};
            mesh_vertices[vertex_count++] = (Vertex){x+1,   y,     z+1,  u1, v0};
            mesh_vertices[vertex_count++] = (Vertex){x,     y,     z+1,  u0, v0};
            break;
    }

    mesh_indices[index_count++] = s;
    mesh_indices[index_count++] = s+1;
    mesh_indices[index_count++] = s+2;
    mesh_indices[index_count++] = s;
    mesh_indices[index_count++] = s+2;
    mesh_indices[index_count++] = s+3;
}

void build_chunk_mesh() {
    vertex_count = 0;
    index_count  = 0;
    for (int x = 0; x < CHUNK_SIZE_X; x++)
        for (int y = 0; y < CHUNK_SIZE_Y; y++)
            for (int z = 0; z < CHUNK_SIZE_Z; z++) {
                uint8_t id = chunk_blocks[x][y][z];
                if (id == 0) continue;
                float bx = (float)x, by = (float)y, bz = (float)z;
                if (is_transparent(x, y, z+1)) add_face(bx, by, bz, FACE_FRONT,  id);
                if (is_transparent(x, y, z-1)) add_face(bx, by, bz, FACE_BACK,   id);
                if (is_transparent(x-1, y, z)) add_face(bx, by, bz, FACE_LEFT,   id);
                if (is_transparent(x+1, y, z)) add_face(bx, by, bz, FACE_RIGHT,  id);
                if (is_transparent(x, y+1, z)) add_face(bx, by, bz, FACE_TOP,    id);
                if (is_transparent(x, y-1, z)) add_face(bx, by, bz, FACE_BOTTOM, id);
            }
}
