#include "renderer.h"
#include "chunk.h"
#include "network.h"
#include "defines.h"
#include <math.h>

static float frustum_planes[6][4];
static float g_proj[16];
float g_modl[16];
extern GLuint g_atlas_tex;

void renderer_init() {
    glViewport(0, 0, 960, 544);
    glClearColor(0.4f, 0.7f, 1.0f, 1.0f);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_DEPTH_TEST);
}

void set_perspective(float fovy, float aspect, float near, float far) {
    float f = 1.0f / tanf(fovy * 0.5f * DEG_TO_RAD);
    g_proj[0] = f/aspect; g_proj[1] = 0; g_proj[2] = 0; g_proj[3] = 0;
    g_proj[4] = 0; g_proj[5] = f; g_proj[6] = 0; g_proj[7] = 0;
    g_proj[8] = 0; g_proj[9] = 0; g_proj[10] = (far+near)/(near-far); g_proj[11] = -1;
    g_proj[12] = 0; g_proj[13] = 0; g_proj[14] = (2.0f*far*near)/(near-far); g_proj[15] = 0;
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(g_proj);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

static void extract_frustum() {
    float clip[16];
    /* clip = g_proj (column-major) * g_modl (row-major), result row-major.
       g_proj_colmajor[k*4+i] = g_proj_rowmajor[i*4+k], so:
       clip[i][j] = sum_k g_proj[k*4+i] * g_modl[k*4+j] */
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            clip[i*4+j] = g_proj[0*4+i]*g_modl[0*4+j] + g_proj[1*4+i]*g_modl[1*4+j] +
                          g_proj[2*4+i]*g_modl[2*4+j] + g_proj[3*4+i]*g_modl[3*4+j];
        }
    }
    /* Gribb-Hartmann: extract planes from rows (row3 ± row_i) */
    /* Left */
    frustum_planes[0][0] = clip[12] + clip[0];
    frustum_planes[0][1] = clip[13] + clip[1];
    frustum_planes[0][2] = clip[14] + clip[2];
    frustum_planes[0][3] = clip[15] + clip[3];
    /* Right */
    frustum_planes[1][0] = clip[12] - clip[0];
    frustum_planes[1][1] = clip[13] - clip[1];
    frustum_planes[1][2] = clip[14] - clip[2];
    frustum_planes[1][3] = clip[15] - clip[3];
    /* Bottom */
    frustum_planes[2][0] = clip[12] + clip[4];
    frustum_planes[2][1] = clip[13] + clip[5];
    frustum_planes[2][2] = clip[14] + clip[6];
    frustum_planes[2][3] = clip[15] + clip[7];
    /* Top */
    frustum_planes[3][0] = clip[12] - clip[4];
    frustum_planes[3][1] = clip[13] - clip[5];
    frustum_planes[3][2] = clip[14] - clip[6];
    frustum_planes[3][3] = clip[15] - clip[7];
    /* Near */
    frustum_planes[4][0] = clip[12] + clip[8];
    frustum_planes[4][1] = clip[13] + clip[9];
    frustum_planes[4][2] = clip[14] + clip[10];
    frustum_planes[4][3] = clip[15] + clip[11];
    /* Far */
    frustum_planes[5][0] = clip[12] - clip[8];
    frustum_planes[5][1] = clip[13] - clip[9];
    frustum_planes[5][2] = clip[14] - clip[10];
    frustum_planes[5][3] = clip[15] - clip[11];
    for (int i = 0; i < 6; i++) {
        float len = sqrtf(frustum_planes[i][0]*frustum_planes[i][0] +
                          frustum_planes[i][1]*frustum_planes[i][1] +
                          frustum_planes[i][2]*frustum_planes[i][2]);
        if (len > 0) {
            frustum_planes[i][0] /= len; frustum_planes[i][1] /= len;
            frustum_planes[i][2] /= len; frustum_planes[i][3] /= len;
        }
    }
}

static int chunk_in_frustum(float wx, float wz, float px, float pz) {
    float dx = wx + 8.0f - px;
    float dz = wz + 8.0f - pz;
    float dist2 = dx*dx + dz*dz;
    if (dist2 > 4096.0f) return 0; /* 64 blocks = 4 chunks max */

    float minx = wx, maxx = wx + 16.0f;
    float minz = wz, maxz = wz + 16.0f;
    for (int p = 0; p < 6; p++) {
        int out = 0;
        if (frustum_planes[p][0] * minx + frustum_planes[p][1] * 0 + frustum_planes[p][2] * minz + frustum_planes[p][3] > 0) out++;
        if (frustum_planes[p][0] * maxx + frustum_planes[p][1] * 0 + frustum_planes[p][2] * minz + frustum_planes[p][3] > 0) out++;
        if (frustum_planes[p][0] * minx + frustum_planes[p][1] * 256 + frustum_planes[p][2] * minz + frustum_planes[p][3] > 0) out++;
        if (frustum_planes[p][0] * maxx + frustum_planes[p][1] * 256 + frustum_planes[p][2] * minz + frustum_planes[p][3] > 0) out++;
        if (frustum_planes[p][0] * minx + frustum_planes[p][1] * 0 + frustum_planes[p][2] * maxz + frustum_planes[p][3] > 0) out++;
        if (frustum_planes[p][0] * maxx + frustum_planes[p][1] * 0 + frustum_planes[p][2] * maxz + frustum_planes[p][3] > 0) out++;
        if (frustum_planes[p][0] * minx + frustum_planes[p][1] * 256 + frustum_planes[p][2] * maxz + frustum_planes[p][3] > 0) out++;
        if (frustum_planes[p][0] * maxx + frustum_planes[p][1] * 256 + frustum_planes[p][2] * maxz + frustum_planes[p][3] > 0) out++;
        if (out == 8) return 0;
    }
    return 1;
}

void draw_selection(float x, float y, float z) {
    float epsilon = 0.001f;
    float mx=x-epsilon,  xx=x+1+epsilon;
    float my=y-epsilon,  xy=y+1+epsilon;
    float mz=z-epsilon,  xz=z+1+epsilon;
    glDisable(GL_TEXTURE_2D);
    glColor4f(0,0,0,0.4f);
    glLineWidth(2.5f);
    glBegin(GL_LINES);
    glVertex3f(mx,my,mz); glVertex3f(xx,my,mz);
    glVertex3f(xx,my,mz); glVertex3f(xx,my,xz);
    glVertex3f(xx,my,xz); glVertex3f(mx,my,xz);
    glVertex3f(mx,my,xz); glVertex3f(mx,my,mz);
    glVertex3f(mx,xy,mz); glVertex3f(xx,xy,mz);
    glVertex3f(xx,xy,mz); glVertex3f(xx,xy,xz);
    glVertex3f(xx,xy,xz); glVertex3f(mx,xy,xz);
    glVertex3f(mx,xy,xz); glVertex3f(mx,xy,mz);
    glVertex3f(mx,my,mz); glVertex3f(mx,xy,mz);
    glVertex3f(xx,my,mz); glVertex3f(xx,xy,mz);
    glVertex3f(xx,my,xz); glVertex3f(xx,xy,xz);
    glVertex3f(mx,my,xz); glVertex3f(mx,xy,xz);
    glEnd();
    glEnable(GL_TEXTURE_2D);
}

void draw_crosshair() {
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 960, 544, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_TEXTURE_2D);
    glColor4f(1,1,1,1);
    glLineWidth(2.0f);
    glBegin(GL_LINES);
    glVertex2f(474,272); glVertex2f(486,272);
    glVertex2f(480,266); glVertex2f(480,278);
    glEnd();
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
}

void draw_all_chunks(float px, float pz) {
    /* Simple distance-based culling. The full frustum extractor was
       culling visible chunks (white patches, blocks missing when
       looking down) because the matrix math didn't match the GL
       camera. A 64-block radius is enough for the 8-chunk view
       distance and can't go wrong. */
    int drawn = 0;
    for (int cx = 0; cx < WORLD_SIZE_X; cx++) {
        for (int cz = 0; cz < WORLD_SIZE_Z; cz++) {
            Chunk *c = &world_chunks[cx][cz];
            if (!c->loaded || c->vertex_count == 0) continue;
            if (!c->vertices || !c->indices) continue;
            if (c->index_count <= 0) continue;
            float dx = (float)c->x_off + 8.0f - px;
            float dz = (float)c->z_off + 8.0f - pz;
            if (dx*dx + dz*dz > 4096.0f) continue; /* >64 blocks away */
            drawn++;
            glPushMatrix();
            glTranslatef((float)c->x_off, 0.0f, (float)c->z_off);
            glBindTexture(GL_TEXTURE_2D, g_atlas_tex);
            glEnableClientState(GL_VERTEX_ARRAY);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glVertexPointer(3, GL_FLOAT, sizeof(Vertex), &c->vertices[0].x);
            glTexCoordPointer(2, GL_FLOAT, sizeof(Vertex), &c->vertices[0].u);
            glDrawElements(GL_TRIANGLES, c->index_count, GL_UNSIGNED_SHORT, c->indices);
            glDisableClientState(GL_VERTEX_ARRAY);
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            glPopMatrix();
        }
    }
    static int draw_log_count = 0;
    if (++draw_log_count % 120 == 0) log_debug("render: drew %d chunks (atlas=%u)", drawn, (unsigned)g_atlas_tex);
}