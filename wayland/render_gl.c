#include "render_gl.h"

#include <GLES2/gl2.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Every line, dot, and ring is one quad. The fragment shader works out the
 * distance to the shape (a segment with a radius, or a ring), so edges are
 * antialiased without multisampling. Shape coordinates are relative to the
 * segment's start, which keeps them small enough for GPUs with 16-bit
 * fragment floats. The clock is one more quad in the same batch (a negative
 * ring width marks it as textured), so a whole frame is a single draw call
 * with no state changes.
 */
static const char *MESH_VS =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_local;\n"
    "attribute vec2 a_ba;\n"
    "attribute vec2 a_rw;\n"
    "attribute vec4 a_color;\n"
    "uniform vec2 u_res;\n"
    "varying vec2 v_local;\n"
    "varying vec2 v_ba;\n"
    "varying vec2 v_rw;\n"
    "varying vec4 v_color;\n"
    "void main() {\n"
    "    v_local = a_local; v_ba = a_ba; v_rw = a_rw; v_color = a_color;\n"
    "    vec2 p = a_pos / u_res * 2.0 - 1.0;\n"
    "    gl_Position = vec4(p.x, -p.y, 0.0, 1.0);\n"
    "}\n";

static const char *MESH_FS =
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "precision mediump float;\n"
    "#endif\n"
    "varying vec2 v_local;\n"
    "varying vec2 v_ba;\n"
    "varying vec2 v_rw;\n"
    "varying vec4 v_color;\n"
    "uniform sampler2D u_text;\n"
    "void main() {\n"
    "    if (v_rw.y < 0.0) {\n"
    "        gl_FragColor = v_color * texture2D(u_text, v_local).a;\n"
    "        return;\n"
    "    }\n"
    "    float l2 = dot(v_ba, v_ba);\n"
    "    float h = l2 > 0.0 ? clamp(dot(v_local, v_ba) / l2, 0.0, 1.0) : 0.0;\n"
    "    float d = length(v_local - v_ba * h);\n"
    "    float edge = v_rw.y > 0.0 ? abs(d - v_rw.x) - v_rw.y * 0.5 : d - v_rw.x;\n"
    "    gl_FragColor = v_color * clamp(0.5 - edge, 0.0, 1.0);\n"
    "}\n";

#define MAX_QUADS 4096
#define VERTEX_FLOATS 12 /* pos, local, ba, radius and ring width, color */

static GLuint mesh_program, index_buffer;
static GLint mesh_res;
static float *vertices;
static int quad_count;
static float frame_scale, frame_width, frame_height;

static GLuint compile(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof log, NULL, log);
        fprintf(stderr, "mesh-wallpaper: shader: %s\n", log);
        return 0;
    }
    return shader;
}

static GLuint link_program(const char *vs, const char *fs, const char *const *attributes) {
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    GLuint program = glCreateProgram();
    glAttachShader(program, v);
    glAttachShader(program, f);
    for (GLuint i = 0; attributes[i]; i++) glBindAttribLocation(program, i, attributes[i]);
    glLinkProgram(program);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    return ok ? program : 0;
}

int gl_setup(void) {
    static const char *const mesh_attributes[] = {"a_pos", "a_local", "a_ba", "a_rw", "a_color", NULL};
    mesh_program = link_program(MESH_VS, MESH_FS, mesh_attributes);
    vertices = malloc(sizeof(float) * MAX_QUADS * 4 * VERTEX_FLOATS);
    GLushort *indices = malloc(sizeof(GLushort) * MAX_QUADS * 6);
    if (!mesh_program || !vertices || !indices) {
        free(indices);
        return 0;
    }
    mesh_res = glGetUniformLocation(mesh_program, "u_res");

    for (int q = 0; q < MAX_QUADS; q++) {
        static const int corner[6] = {0, 1, 2, 0, 2, 3};
        for (int i = 0; i < 6; i++) indices[q * 6 + i] = (GLushort)(q * 4 + corner[i]);
    }
    glGenBuffers(1, &index_buffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, index_buffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(GLushort) * MAX_QUADS * 6, indices, GL_STATIC_DRAW);
    free(indices);

    /* All the state there is, set once: one program, one index buffer, the vertices streamed from memory. */
    glUseProgram(mesh_program);
    glUniform1i(glGetUniformLocation(mesh_program, "u_text"), 0);
    static const int sizes[5] = {2, 2, 2, 2, 4};
    const float *attribute = vertices;
    for (GLuint i = 0; i < 5; i++) {
        glEnableVertexAttribArray(i);
        glVertexAttribPointer(i, sizes[i], GL_FLOAT, GL_FALSE, sizeof(float) * VERTEX_FLOATS, attribute);
        attribute += sizes[i];
    }
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); /* colors are premultiplied, as Wayland wants them */
    return 1;
}

static void flush(void) {
    if (!quad_count) return;
    glDrawElements(GL_TRIANGLES, quad_count * 6, GL_UNSIGNED_SHORT, NULL);
    quad_count = 0;
}

/* corners: four x, y pairs going around the quad. a and b are the segment's ends, in pixels. */
static void quad(const float *corners, float ax, float ay, float bx, float by, float radius, float ring,
                 mesh_color c) {
    if (c.a <= 0) return;
    if (quad_count == MAX_QUADS) flush();
    float *v = vertices + quad_count * 4 * VERTEX_FLOATS;
    for (int i = 0; i < 4; i++, v += VERTEX_FLOATS) {
        v[0] = corners[i * 2];
        v[1] = corners[i * 2 + 1];
        v[2] = v[0] - ax;
        v[3] = v[1] - ay;
        v[4] = bx - ax;
        v[5] = by - ay;
        v[6] = radius;
        v[7] = ring;
        v[8] = c.r * c.a;
        v[9] = c.g * c.a;
        v[10] = c.b * c.a;
        v[11] = c.a;
    }
    quad_count++;
}

static void draw_line(void *ctx, float x0, float y0, float x1, float y1, float width, mesh_color c) {
    (void)ctx;
    float s = frame_scale;
    x0 *= s, y0 *= s, x1 *= s, y1 *= s;
    float radius = width * s * 0.5f, pad = radius + 1.0f;
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    if (len < 0.0001f) dx = 1, dy = 0; else dx /= len, dy /= len;
    float px = dx * pad, py = dy * pad, nx = -dy * pad, ny = dx * pad;
    /* A quad along the line, not its bounding box: far fewer pixels to shade. */
    float corners[8] = {x0 - px - nx, y0 - py - ny, x0 - px + nx, y0 - py + ny,
                        x1 + px + nx, y1 + py + ny, x1 + px - nx, y1 + py - ny};
    quad(corners, x0, y0, x1, y1, radius, 0, c);
}

static void draw_circle(void *ctx, float x, float y, float radius, int filled, float width, mesh_color c) {
    (void)ctx;
    float s = frame_scale;
    x *= s, y *= s, radius *= s;
    float ring = filled ? 0 : width * s;
    float pad = radius + ring * 0.5f + 1.0f;
    float corners[8] = {x - pad, y - pad, x - pad, y + pad, x + pad, y + pad, x + pad, y - pad};
    quad(corners, x, y, x, y, radius, ring, c);
}

void gl_frame_begin(int width, int height, float scale, mesh_color background) {
    frame_scale = scale;
    frame_width = (float)width;
    frame_height = (float)height;
    quad_count = 0;
    glViewport(0, 0, width, height);
    glUniform2f(mesh_res, frame_width, frame_height);
    glClearColor(background.r * background.a, background.g * background.a, background.b * background.a,
                 background.a);
    glClear(GL_COLOR_BUFFER_BIT);
}

void gl_frame_mesh(const mesh *m) {
    mesh_renderer r = {NULL, draw_line, draw_circle};
    mesh_render(m, &r);
}

void gl_frame_end(void) { flush(); }

void gl_text_upload(gl_text *t, const unsigned char *alpha, int width, int height, int stride) {
    unsigned char *tight = malloc((size_t)width * (size_t)height);
    if (!tight) return;
    for (int y = 0; y < height; y++) memcpy(tight + (size_t)y * (size_t)width, alpha + (size_t)y * (size_t)stride, (size_t)width);
    if (!t->texture) glGenTextures(1, &t->texture);
    glBindTexture(GL_TEXTURE_2D, t->texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, width, height, 0, GL_ALPHA, GL_UNSIGNED_BYTE, tight);
    free(tight);
    t->width = width;
    t->height = height;
}

void gl_text_draw(const gl_text *t, float x, float y, mesh_color ink) {
    if (!t->texture || ink.a <= 0) return;
    if (quad_count == MAX_QUADS) flush();
    float w = (float)t->width, h = (float)t->height;
    const float corners[8] = {x, y, x, y + h, x + w, y + h, x + w, y};
    static const float uv[8] = {0, 0, 0, 1, 1, 1, 1, 0};
    float *v = vertices + quad_count * 4 * VERTEX_FLOATS;
    for (int i = 0; i < 4; i++, v += VERTEX_FLOATS) {
        v[0] = corners[i * 2];
        v[1] = corners[i * 2 + 1];
        v[2] = uv[i * 2];
        v[3] = uv[i * 2 + 1];
        v[4] = v[5] = v[6] = 0;
        v[7] = -1; /* textured */
        v[8] = ink.r * ink.a;
        v[9] = ink.g * ink.a;
        v[10] = ink.b * ink.a;
        v[11] = ink.a;
    }
    quad_count++;
    glBindTexture(GL_TEXTURE_2D, t->texture);
}

void gl_text_free(gl_text *t) {
    if (t->texture) glDeleteTextures(1, &t->texture);
    memset(t, 0, sizeof *t);
}
