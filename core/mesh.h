/*
 * Mesh — the drawing core shared by the macOS, Windows, and Linux savers.
 *
 * The core owns the simulation (nodes, links, packets) and draws through
 * three callbacks, so each platform only supplies "line", "circle", and
 * "fill" with its native 2D API. Coordinates are in points; the host
 * scales to pixels.
 */
#ifndef MESH_H
#define MESH_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { float r, g, b, a; } mesh_color;

typedef struct {
    void *ctx;
    void (*line)(void *ctx, float x0, float y0, float x1, float y1, float width, mesh_color c);
    void (*circle)(void *ctx, float x, float y, float radius, int filled, float width, mesh_color c);
} mesh_renderer;

typedef struct {
    mesh_color background;
    mesh_color node;
    mesh_color link;
    mesh_color packet;
    int reduced_motion; /* slower drift and packets */
    int compact;        /* small preview thumbnail */
} mesh_options;

typedef struct mesh mesh;

mesh *mesh_create(unsigned seed);
void mesh_destroy(mesh *m);

/* Defaults: #0b0b0a background, #eeebe4 ink, #f06a2a packets. */
mesh_options mesh_default_options(void);
void mesh_set_options(mesh *m, const mesh_options *opts);
void mesh_resize(mesh *m, float width, float height);

/*
 * Live mode: nodes come from mesh_set_nodes and packets only move when
 * mesh_message is called. Simulated mode (the default) needs no server:
 * random nodes, random packets.
 */
void mesh_set_live(mesh *m, int live);
int mesh_is_live(const mesh *m);

/* Replace the node list. Nodes that were already on screen keep their spot. */
void mesh_set_nodes(mesh *m, const char *const *ids, int count);

/*
 * A message was sent. `to` may be NULL or "!ffffffff" for a broadcast, which
 * ripples out through the links. Unknown senders are added to the mesh.
 */
void mesh_message(mesh *m, const char *from, const char *to);

void mesh_step(mesh *m, float dt);
void mesh_render(const mesh *m, const mesh_renderer *r);

int mesh_node_count(const mesh *m);
int mesh_packet_count(const mesh *m);

/* Parse "#rrggbb" or "#rrggbbaa". Returns 0 on bad input. */
int mesh_parse_color(const char *hex, mesh_color *out);

#ifdef __cplusplus
}
#endif

#endif
