#include "renderer.h"
#include "chunk.h"
#include "network.h"
#include "defines.h"
#include <math.h>

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
    float m[16] = {
        f/aspect, 0, 0,  0,
        0,        f, 0,  0,
        0, 0, (far+near)/(near-far), -1,
        0, 0, (2.0f*far*near)/(near-far), 0
    };
    glPushMatrix();
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(m);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glPopMatrix();
}

void draw_all_chunks(float px, float pz) {
    /* Distance-only culling. The frustum extractor was culling visible
       chunks (white strips, blocks missing when looking down) because
       the matrix math didn't match the GL camera. A 64-block radius
       covers the 8-chunk view distance and can't go wrong. */
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
    if (++draw_log_count % 120 == 0)
        log_debug("render: drew %d chunks (atlas=%u)", drawn, (unsigned)g_atlas_tex);
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
    glPushMatrix();
    glOrtho(0, 960, 544, 0, -1, 1);
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
    glPopMatrix();
}