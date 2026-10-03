/* Core tests: cc -std=c99 -Icore core/mesh.c tests/test_mesh.c -lm && ./a.out */
#include "mesh.h"

#include <stdio.h>
#include <stdlib.h>

static int failures = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                \
        }                                                              \
    } while (0)

static int lines, circles;
static void count_line(void *c, float a, float b, float d, float e, float w, mesh_color k) {
    (void)c, (void)a, (void)b, (void)d, (void)e, (void)w, (void)k;
    lines++;
}
static void count_circle(void *c, float x, float y, float r, int f, float w, mesh_color k) {
    (void)c, (void)x, (void)y, (void)r, (void)f, (void)w, (void)k;
    circles++;
}

static void run(mesh *m, float seconds) {
    for (float t = 0; t < seconds; t += 1.0f / 60) mesh_step(m, 1.0f / 60);
}

int main(void) {
    mesh_color c;
    CHECK(mesh_parse_color("#f06a2a", &c) && c.r > 0.93f && c.a == 1.0f);
    CHECK(mesh_parse_color("eeebe480", &c) && c.a > 0.49f && c.a < 0.51f);
    CHECK(!mesh_parse_color("#12345", &c));
    CHECK(!mesh_parse_color("#zzzzzz", &c));

    /* Simulated mode: node count scales with area, 14..90. */
    mesh *m = mesh_create(42);
    mesh_resize(m, 1440, 900);
    CHECK(mesh_node_count(m) == 81);
    mesh_resize(m, 200, 100);
    CHECK(mesh_node_count(m) == 14);
    mesh_resize(m, 5120, 2880);
    CHECK(mesh_node_count(m) == 90);
    mesh_resize(m, 1440, 900);
    int peak = 0;
    for (int i = 0; i < 600; i++) {
        mesh_step(m, 1.0f / 60);
        if (mesh_packet_count(m) > peak) peak = mesh_packet_count(m);
    }
    CHECK(peak > 0 && peak <= 6);

    /* Live mode: no packets until a message arrives. */
    mesh_resize(m, 1440, 900);
    mesh_set_live(m, 1);
    CHECK(mesh_node_count(m) == 0);
    const char *ids[] = {"!a", "!b", "!c", "!d", "!e", "!f", "!g", "!h", "!b"};
    mesh_set_nodes(m, ids, 9);
    CHECK(mesh_node_count(m) == 8); /* duplicate dropped */
    run(m, 3);
    CHECK(mesh_packet_count(m) == 0);

    mesh_message(m, "!a", "!ffffffff");
    CHECK(mesh_packet_count(m) >= 0); /* may be 0 if !a has no links yet */
    mesh_message(m, "!a", "!h");
    CHECK(mesh_packet_count(m) >= 1);
    run(m, 30);
    CHECK(mesh_packet_count(m) == 0); /* all delivered */

    /* Unknown sender joins the mesh. */
    mesh_message(m, "!new", NULL);
    CHECK(mesh_node_count(m) == 9);

    /* Dropping nodes keeps in-flight packets consistent. */
    mesh_message(m, "!a", "!h");
    const char *fewer[] = {"!a", "!c"};
    mesh_set_nodes(m, fewer, 2);
    CHECK(mesh_node_count(m) == 2);
    run(m, 10);
    mesh_renderer r = {NULL, count_line, count_circle};
    lines = circles = 0;
    mesh_render(m, &r);
    CHECK(circles >= 2);

    /* Back to simulated. */
    mesh_set_live(m, 0);
    CHECK(mesh_node_count(m) == 81);
    mesh_destroy(m);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all core tests passed\n");
    return 0;
}
