/*
 * OpenGL ES 2 drawing for the Wayland host: the core's lines and circles,
 * and a small alpha texture for the clock, batched into one draw call.
 * Every call needs a current GL context.
 */
#ifndef MESH_RENDER_GL_H
#define MESH_RENDER_GL_H

#include "mesh.h"

/* Compile the shaders. Returns 0 on failure. */
int gl_setup(void);

/* Clear to `background` and set up a frame of width x height pixels, `scale` pixels per point. */
void gl_frame_begin(int width, int height, float scale, mesh_color background);
void gl_frame_mesh(const mesh *m);
/* Draw what the frame has collected: the mesh and the text, in one call. */
void gl_frame_end(void);

typedef struct {
    unsigned texture;
    int width, height; /* pixels */
} gl_text;

/* Replace the texture with `alpha`: one byte per pixel, rows `stride` bytes apart. */
void gl_text_upload(gl_text *t, const unsigned char *alpha, int width, int height, int stride);
/* Add to the frame with its top left corner at x, y (pixels), tinted with `ink`. One text per frame. */
void gl_text_draw(const gl_text *t, float x, float y, mesh_color ink);
void gl_text_free(gl_text *t);

#endif
