/*
 * The core for JavaScript (web/index.js). Built to WebAssembly by web/build.sh.
 *
 * Drawing goes out through two imports from the module "host"; nothing else is
 * imported. Strings and response bodies pass through memory from alloc/dealloc.
 */
#include <stdlib.h>
#include <string.h>

#include "mesh.h"
#include "meshmonitor.h"

#define IMPORT(name) __attribute__((import_module("host"), import_name(name)))
#define EXPORT(name) __attribute__((export_name(name)))

IMPORT("line") void js_line(float x0, float y0, float x1, float y1, float width, float r, float g, float b, float a);
IMPORT("circle") void js_circle(float x, float y, float radius, int filled, float width, float r, float g, float b, float a);

static void line(void *c, float x0, float y0, float x1, float y1, float w, mesh_color k) {
    (void)c;
    js_line(x0, y0, x1, y1, w, k.r, k.g, k.b, k.a);
}

static void circle(void *c, float x, float y, float rad, int filled, float w, mesh_color k) {
    (void)c;
    js_circle(x, y, rad, filled, w, k.r, k.g, k.b, k.a);
}

static const mesh_renderer RENDERER = {0, line, circle};

/* ---------- memory ---------- */

EXPORT("alloc") void *alloc(unsigned size) { return malloc(size ? size : 1); }
EXPORT("dealloc") void dealloc(void *p) { free(p); }

/* ---------- the mesh ---------- */

EXPORT("create") mesh *create(unsigned seed) { return mesh_create(seed); }
EXPORT("destroy") void destroy(mesh *m) { mesh_destroy(m); }
EXPORT("resize") void resize(mesh *m, float w, float h) { mesh_resize(m, w, h); }
EXPORT("step") void step(mesh *m, float dt) { mesh_step(m, dt); }
EXPORT("render") void render(mesh *m) { mesh_render(m, &RENDERER); }
EXPORT("set_live") void set_live(mesh *m, int live) { mesh_set_live(m, live); }
EXPORT("is_live") int is_live(mesh *m) { return mesh_is_live(m); }
EXPORT("node_count") int node_count(mesh *m) { return mesh_node_count(m); }
EXPORT("packet_count") int packet_count(mesh *m) { return mesh_packet_count(m); }

/* Colors as 0..1 floats: background, dots, lines, packets. */
EXPORT("set_options")
void set_options(mesh *m, float br, float bg, float bb, float dr, float dg, float db, float lr, float lg, float lb,
                 float pr, float pg, float pb, int reduced_motion, int compact) {
    mesh_options o = mesh_default_options();
    o.background = (mesh_color){br, bg, bb, 1};
    o.node = (mesh_color){dr, dg, db, 1};
    o.link = (mesh_color){lr, lg, lb, 1};
    o.packet = (mesh_color){pr, pg, pb, 1};
    o.reduced_motion = reduced_motion;
    o.compact = compact;
    mesh_set_options(m, &o);
}

/* A message from `from` to `to` (NUL-terminated; `to` empty or "!ffffffff" for a broadcast). */
EXPORT("message") void message(mesh *m, const char *from, const char *to) {
    mesh_message(m, from, to && *to ? to : NULL);
}

/* ---------- MeshMonitor responses, parsed exactly as the native savers do ---------- */

static char node_ids[MM_MAX_NODES][MM_ID_LEN];

/* A /nodes response body: hands its nodes to the mesh. Returns the count, or -1 for bad JSON. */
EXPORT("nodes_json") int nodes_json(mesh *m, const char *body, unsigned length) {
    int n = mm_parse_nodes(body, length, node_ids, MM_MAX_NODES);
    if (n <= 0) return n;
    const char *ids[MM_MAX_NODES];
    for (int i = 0; i < n; i++) ids[i] = node_ids[i];
    mesh_set_nodes(m, ids, n);
    return n;
}

static mm_message fresh[MM_MAX_MESSAGES];

EXPORT("tracker_create") mm_tracker *tracker_create(void) { return mm_tracker_create(); }
EXPORT("tracker_destroy") void tracker_destroy(mm_tracker *t) { mm_tracker_destroy(t); }

/* A /messages response body: the new messages, oldest first, read with message_from/message_to.
   The first response only primes the tracker. Returns the count, or -1 for bad JSON. */
EXPORT("messages_json") int messages_json(mm_tracker *t, const char *body, unsigned length) {
    return mm_parse_messages(t, body, length, fresh, MM_MAX_MESSAGES);
}
EXPORT("message_from") const char *message_from(int i) { return i >= 0 && i < MM_MAX_MESSAGES ? fresh[i].from : ""; }
EXPORT("message_to") const char *message_to(int i) { return i >= 0 && i < MM_MAX_MESSAGES ? fresh[i].to : ""; }

/* ---------- the label under the clock ---------- */

static char label_out[400];

/* kind: 0 Mesh, 1 your name, 2 custom text. Returns "Mesh", the name, or the text, plus " · live". */
EXPORT("label") const char *label(int kind, const char *text, const char *user, int live) {
    mm_settings s;
    mm_settings_default(&s);
    s.label = kind == 1 ? MM_LABEL_USER : kind == 2 ? MM_LABEL_CUSTOM : MM_LABEL_MESH;
    if (text) {
        size_t n = strlen(text);
        if (n >= sizeof s.label_text) n = sizeof s.label_text - 1;
        memcpy(s.label_text, text, n);
        s.label_text[n] = 0;
    }
    mm_label(label_out, sizeof label_out, &s, user, live);
    return label_out;
}
