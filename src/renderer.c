#include "renderer.h"
#include "chunk.h"
#include <math.h>

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
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(m);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
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

void draw_chunk() {
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(3, GL_FLOAT, sizeof(Vertex), &mesh_vertices[0].x);
    glTexCoordPointer(2, GL_FLOAT, sizeof(Vertex), &mesh_vertices[0].u);
    glDrawElements(GL_TRIANGLES, index_count, GL_UNSIGNED_SHORT, mesh_indices);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
}
