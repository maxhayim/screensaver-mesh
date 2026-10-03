/*
 * Mesh — drawing core: a simulated mesh, plus a live mode driven by
 * MeshMonitor messages.
 */
#include "mesh.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NODES 160
#define MAX_PACKETS 240
#define MAX_RINGS 96
#define ID_LEN 32
#define BROADCAST_ID "!ffffffff"
#define BROADCAST_HOPS 3
#define WRAP_MARGIN 20.0f

typedef struct {
    char id[ID_LEN];
    float x, y;
    float vx, vy; /* steady drift */
    float kx, ky; /* kick from a nearby message, decays back to zero */
    float r;
} node;

typedef struct {
    int a, b;
    float t;     /* 0..1 along a -> b */
    float delay; /* edge-lengths to wait before starting */
    int hops;
    int wander;  /* simulated packets pick the next hop as they go */
    int ring;    /* flash a ring on arrival */
} packet;

typedef struct {
    int n;
    float age, life, radius_from, radius_to, alpha;
} ring;

struct mesh {
    mesh_options opts;
    float width, height;
    int live;
    unsigned rng;
    float clock, last_spawn;
    node nodes[MAX_NODES];
    int node_count;
    packet packets[MAX_PACKETS];
    int packet_count;
    ring rings[MAX_RINGS];
    int ring_count;
};

/* ---------- helpers ---------- */

static float rnd(mesh *m) {
    /* xorshift32 */
    unsigned x = m->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    m->rng = x ? x : 0x9e3779b9u;
    return (float)(x & 0xffffff) / (float)0x1000000;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static float slow_factor(const mesh *m) { return m->opts.reduced_motion ? 0.35f : 1.0f; }

static float base_reach(const mesh *m) { return m->opts.compact ? 46.0f : 150.0f; }

/* Real meshes can be sparse; widen the reach so a few nodes still link up. */
static float reach(const mesh *m) {
    float base = base_reach(m);
    if (!m->live || m->node_count < 2) return base;
    float spacing = sqrtf(m->width * m->height / (float)m->node_count);
    return clampf(spacing * 1.2f, base, base * 2.2f);
}

static int linked(const mesh *m, int i, int j) {
    float dx = m->nodes[j].x - m->nodes[i].x;
    float dy = m->nodes[j].y - m->nodes[i].y;
    float rr = reach(m);
    return dx * dx + dy * dy < rr * rr;
}

static void place_node(mesh *m, node *n) {
    float speed = (m->opts.compact ? 9.0f : 16.0f) * slow_factor(m);
    float angle = rnd(m) * 6.2831853f;
    float v = speed * (0.4f + rnd(m) * 0.6f);
    n->x = rnd(m) * m->width;
    n->y = rnd(m) * m->height;
    n->vx = cosf(angle) * v;
    n->vy = sinf(angle) * v;
    n->kx = n->ky = 0;
    n->r = m->opts.compact ? 1.4f : 2.0f + rnd(m) * 1.2f;
}

static void populate_simulated(mesh *m) {
    float area = m->width * m->height;
    int count = (int)lroundf(area / (m->opts.compact ? 2600.0f : 16000.0f));
    if (count < 14) count = 14;
    if (count > 90) count = 90;
    m->node_count = count;
    for (int i = 0; i < count; i++) {
        m->nodes[i].id[0] = 0;
        place_node(m, &m->nodes[i]);
    }
    m->packet_count = 0;
    m->ring_count = 0;
}

static int find_node(const mesh *m, const char *id) {
    if (!id || !*id) return -1;
    for (int i = 0; i < m->node_count; i++)
        if (strncmp(m->nodes[i].id, id, ID_LEN - 1) == 0) return i;
    return -1;
}

static int add_node(mesh *m, const char *id) {
    if (m->node_count >= MAX_NODES) return -1;
    node *n = &m->nodes[m->node_count];
    memset(n, 0, sizeof *n);
    strncpy(n->id, id, ID_LEN - 1);
    place_node(m, n);
    return m->node_count++;
}

static void add_packet(mesh *m, int a, int b, float delay, int wander, int ring_on_arrival) {
    if (m->packet_count >= MAX_PACKETS || a < 0 || b < 0 || a == b) return;
    packet *p = &m->packets[m->packet_count++];
    p->a = a;
    p->b = b;
    p->t = 0;
    p->delay = delay;
    p->hops = 0;
    p->wander = wander;
    p->ring = ring_on_arrival;
}

static void add_ring(mesh *m, int n, float life, float from, float to, float alpha) {
    if (m->ring_count >= MAX_RINGS) return;
    ring *r = &m->rings[m->ring_count++];
    r->n = n;
    r->age = 0;
    r->life = life;
    r->radius_from = from;
    r->radius_to = to;
    r->alpha = alpha;
}

/* ---------- public API ---------- */

mesh_options mesh_default_options(void) {
    mesh_options o;
    mesh_parse_color("#0b0b0a", &o.background);
    mesh_parse_color("#eeebe4", &o.node);
    mesh_parse_color("#eeebe4", &o.link);
    mesh_parse_color("#f06a2a", &o.packet);
    o.reduced_motion = 0;
    o.compact = 0;
    return o;
}

mesh *mesh_create(unsigned seed) {
    mesh *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->rng = seed ? seed : 0x2545f491u;
    m->opts = mesh_default_options();
    return m;
}

void mesh_destroy(mesh *m) { free(m); }

void mesh_set_options(mesh *m, const mesh_options *opts) {
    int reshape = opts->compact != m->opts.compact || opts->reduced_motion != m->opts.reduced_motion;
    m->opts = *opts;
    if (reshape && !m->live && m->width > 0) populate_simulated(m);
}

void mesh_resize(mesh *m, float width, float height) {
    if (width <= 0 || height <= 0) return;
    if (m->live && m->width > 0) {
        float sx = width / m->width, sy = height / m->height;
        for (int i = 0; i < m->node_count; i++) {
            m->nodes[i].x *= sx;
            m->nodes[i].y *= sy;
        }
        m->width = width;
        m->height = height;
        return;
    }
    m->width = width;
    m->height = height;
    if (!m->live) populate_simulated(m);
}

void mesh_set_live(mesh *m, int live) {
    live = live ? 1 : 0;
    if (live == m->live) return;
    m->live = live;
    m->packet_count = 0;
    m->ring_count = 0;
    if (live) m->node_count = 0;
    else if (m->width > 0) populate_simulated(m);
}

int mesh_is_live(const mesh *m) { return m->live; }

void mesh_set_nodes(mesh *m, const char *const *ids, int count) {
    if (!m->live) return;
    static node old[MAX_NODES];
    int old_count = m->node_count;
    int remap[MAX_NODES];
    memcpy(old, m->nodes, sizeof(node) * (size_t)old_count);
    for (int i = 0; i < old_count; i++) remap[i] = -1;

    int n = 0;
    for (int i = 0; i < count && n < MAX_NODES; i++) {
        const char *id = ids[i];
        if (!id || !*id) continue;
        int dup = 0;
        for (int k = 0; k < n; k++)
            if (strncmp(m->nodes[k].id, id, ID_LEN - 1) == 0) { dup = 1; break; }
        if (dup) continue;

        int prev = -1;
        for (int k = 0; k < old_count; k++)
            if (strncmp(old[k].id, id, ID_LEN - 1) == 0) { prev = k; break; }
        if (prev >= 0) {
            m->nodes[n] = old[prev];
            remap[prev] = n;
        } else {
            memset(&m->nodes[n], 0, sizeof(node));
            strncpy(m->nodes[n].id, id, ID_LEN - 1);
            place_node(m, &m->nodes[n]);
        }
        n++;
    }
    m->node_count = n;

    int pc = 0;
    for (int i = 0; i < m->packet_count; i++) {
        packet p = m->packets[i];
        if (p.a >= old_count || p.b >= old_count) continue;
        p.a = remap[p.a];
        p.b = remap[p.b];
        if (p.a >= 0 && p.b >= 0) m->packets[pc++] = p;
    }
    m->packet_count = pc;

    int rc = 0;
    for (int i = 0; i < m->ring_count; i++) {
        ring r = m->rings[i];
        if (r.n >= old_count) continue;
        r.n = remap[r.n];
        if (r.n >= 0) m->rings[rc++] = r;
    }
    m->ring_count = rc;
}

/* Push nearby nodes away from the sender; they drift back to their own pace. */
static void stir(mesh *m, int src) {
    float rr = reach(m) * 1.6f;
    float strength = (m->opts.compact ? 18.0f : 60.0f) * slow_factor(m);
    for (int i = 0; i < m->node_count; i++) {
        if (i == src) continue;
        float dx = m->nodes[i].x - m->nodes[src].x;
        float dy = m->nodes[i].y - m->nodes[src].y;
        float d = sqrtf(dx * dx + dy * dy);
        if (d >= rr || d < 0.001f) continue;
        float push = (1.0f - d / rr) * strength;
        m->nodes[i].kx += dx / d * push;
        m->nodes[i].ky += dy / d * push;
    }
    add_ring(m, src, 1.2f, m->nodes[src].r, reach(m) * 0.35f, 0.7f);
}

void mesh_message(mesh *m, const char *from, const char *to) {
    if (!m->live || !from) return;
    int a = find_node(m, from);
    if (a < 0) a = add_node(m, from);
    if (a < 0) return;

    int broadcast = !to || !*to || strcmp(to, BROADCAST_ID) == 0;
    int b = broadcast ? -1 : find_node(m, to);
    if (!broadcast && b < 0) b = add_node(m, to);
    if (b == a) broadcast = 1;

    stir(m, a);

    /* Breadth-first search over the current links. */
    int parent[MAX_NODES], depth[MAX_NODES], queue[MAX_NODES];
    for (int i = 0; i < m->node_count; i++) parent[i] = -1, depth[i] = -1;
    int head = 0, tail = 0;
    queue[tail++] = a;
    depth[a] = 0;
    while (head < tail) {
        int u = queue[head++];
        if (!broadcast && u == b) break;
        if (broadcast && depth[u] >= BROADCAST_HOPS) continue;
        for (int v = 0; v < m->node_count; v++) {
            if (depth[v] >= 0 || !linked(m, u, v)) continue;
            depth[v] = depth[u] + 1;
            parent[v] = u;
            queue[tail++] = v;
        }
    }

    if (broadcast) {
        /* Every edge of the search tree carries a packet, one hop at a time. */
        for (int i = 1; i < tail; i++) {
            int v = queue[i];
            int leaf = 1;
            for (int k = 1; k < tail; k++)
                if (parent[queue[k]] == v) { leaf = 0; break; }
            add_packet(m, parent[v], v, (float)(depth[v] - 1), 0, leaf);
        }
        return;
    }

    if (depth[b] < 0) {
        /* Not reachable through visible links: one long hop. */
        add_packet(m, a, b, 0, 0, 1);
        return;
    }
    int path[MAX_NODES], len = 0;
    for (int v = b; v >= 0 && len < MAX_NODES; v = parent[v]) path[len++] = v;
    for (int i = len - 1, hop = 0; i > 0; i--, hop++)
        add_packet(m, path[i], path[i - 1], (float)hop, 0, i == 1);
}

void mesh_step(mesh *m, float dt) {
    if (dt < 0) dt = 0;
    if (dt > 0.05f) dt = 0.05f;
    m->clock += dt;
    float decay = expf(-1.6f * dt);

    for (int i = 0; i < m->node_count; i++) {
        node *n = &m->nodes[i];
        n->x += (n->vx + n->kx) * dt;
        n->y += (n->vy + n->ky) * dt;
        n->kx *= decay;
        n->ky *= decay;
        if (n->x < -WRAP_MARGIN) n->x = m->width + WRAP_MARGIN;
        if (n->x > m->width + WRAP_MARGIN) n->x = -WRAP_MARGIN;
        if (n->y < -WRAP_MARGIN) n->y = m->height + WRAP_MARGIN;
        if (n->y > m->height + WRAP_MARGIN) n->y = -WRAP_MARGIN;
    }

    /* Simulated mode: send a new packet every so often from a random node. */
    float every = m->opts.reduced_motion ? 2.4f : 0.9f;
    if (!m->live && m->node_count > 1 && m->clock - m->last_spawn > every && m->packet_count < 6) {
        m->last_spawn = m->clock;
        int from = (int)(rnd(m) * (float)m->node_count) % m->node_count;
        int options[MAX_NODES], count = 0;
        for (int j = 0; j < m->node_count; j++)
            if (j != from && linked(m, from, j)) options[count++] = j;
        if (count) add_packet(m, from, options[(int)(rnd(m) * (float)count) % count], 0, 1, 1);
    }

    float rate = m->opts.reduced_motion ? 0.6f : 1.6f;
    int kept = 0;
    for (int i = 0; i < m->packet_count; i++) {
        packet p = m->packets[i];
        if (p.delay > 0) {
            p.delay -= dt * rate;
            m->packets[kept++] = p;
            continue;
        }
        p.t += dt * rate;
        if (p.t >= 1) {
            if (p.wander) {
                p.hops += 1;
                int options[MAX_NODES], count = 0;
                for (int j = 0; j < m->node_count; j++)
                    if (j != p.b && j != p.a && linked(m, p.b, j)) options[count++] = j;
                if (count && p.hops <= 7) {
                    p.a = p.b;
                    p.b = options[(int)(rnd(m) * (float)count) % count];
                    p.t = 0;
                    m->packets[kept++] = p;
                    continue;
                }
            }
            if (p.ring) add_ring(m, p.b, 0.5f, m->opts.compact ? 5.0f : 12.0f, m->opts.compact ? 5.0f : 12.0f, 0.6f);
            continue;
        }
        m->packets[kept++] = p;
    }
    m->packet_count = kept;

    int rk = 0;
    for (int i = 0; i < m->ring_count; i++) {
        ring r = m->rings[i];
        r.age += dt;
        if (r.age < r.life) m->rings[rk++] = r;
    }
    m->ring_count = rk;
}

static mesh_color with_alpha(mesh_color c, float a) {
    c.a *= a;
    return c;
}

void mesh_render(const mesh *m, const mesh_renderer *r) {
    const mesh_options *o = &m->opts;
    float rr = reach(m);
    float link_width = o->compact ? 0.6f : 1.0f;

    for (int i = 0; i < m->node_count; i++) {
        for (int j = i + 1; j < m->node_count; j++) {
            float dx = m->nodes[j].x - m->nodes[i].x;
            float dy = m->nodes[j].y - m->nodes[i].y;
            float d2 = dx * dx + dy * dy;
            if (d2 > rr * rr) continue;
            float alpha = (1.0f - sqrtf(d2) / rr) * 0.35f;
            r->line(r->ctx, m->nodes[i].x, m->nodes[i].y, m->nodes[j].x, m->nodes[j].y, link_width,
                    with_alpha(o->link, alpha));
        }
    }

    mesh_color node_color = with_alpha(o->node, 0.75f);
    for (int i = 0; i < m->node_count; i++)
        r->circle(r->ctx, m->nodes[i].x, m->nodes[i].y, m->nodes[i].r, 1, 0, node_color);

    for (int i = 0; i < m->ring_count; i++) {
        const ring *g = &m->rings[i];
        float k = g->age / g->life;
        float radius = g->radius_from + (g->radius_to - g->radius_from) * k;
        r->circle(r->ctx, m->nodes[g->n].x, m->nodes[g->n].y, radius, 0, o->compact ? 0.8f : 1.2f,
                  with_alpha(o->packet, g->alpha * (1.0f - k)));
    }

    float trail = o->compact ? 1.0f : 1.6f;
    float head = o->compact ? 2.0f : 3.4f;
    for (int i = 0; i < m->packet_count; i++) {
        const packet *p = &m->packets[i];
        if (p->delay > 0) continue;
        const node *a = &m->nodes[p->a];
        const node *b = &m->nodes[p->b];
        float x = a->x + (b->x - a->x) * p->t;
        float y = a->y + (b->y - a->y) * p->t;
        r->line(r->ctx, a->x, a->y, x, y, trail, with_alpha(o->packet, 0.55f));
        r->circle(r->ctx, x, y, head, 1, 0, o->packet);
    }
}

int mesh_node_count(const mesh *m) { return m->node_count; }
int mesh_packet_count(const mesh *m) { return m->packet_count; }

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int mesh_parse_color(const char *hex, mesh_color *out) {
    if (!hex || !out) return 0;
    if (*hex == '#') hex++;
    size_t len = strlen(hex);
    if (len != 6 && len != 8) return 0;
    int v[8];
    for (size_t i = 0; i < len; i++)
        if ((v[i] = hexval(hex[i])) < 0) return 0;
    out->r = (float)(v[0] * 16 + v[1]) / 255.0f;
    out->g = (float)(v[2] * 16 + v[3]) / 255.0f;
    out->b = (float)(v[4] * 16 + v[5]) / 255.0f;
    out->a = len == 8 ? (float)(v[6] * 16 + v[7]) / 255.0f : 1.0f;
    return 1;
}
