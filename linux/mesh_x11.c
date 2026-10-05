/*
 * Mesh for Linux: an XScreenSaver hack drawing the core with cairo,
 * polling MeshMonitor with libcurl on a worker thread.
 *
 *   screensaver-mesh                  in its own window (any key closes it)
 *   screensaver-mesh -root            on the root window
 *   (XScreenSaver passes its window in $XSCREENSAVER_WINDOW)
 *   screensaver-mesh -render 5 out.png [-size 1280x800] [-preview]
 *                                     test: render off screen to a PNG, no X needed
 *
 * Settings come from ~/.config/screensaver-mesh/config (key=value lines),
 * then from flags: -server URL -token T -source S -preset NAME
 * -background/-dots/-lines/-packets #rrggbb -clock/-no-clock -24h/-12h
 * -label TEXT (custom text under the clock) or -label-user (your name)
 * -reduced-motion -config FILE.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cairo-xlib.h>
#include <cairo.h>
#include <curl/curl.h>

#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "meshmonitor.h"
#include "mesh.h"

/* ---------- options ---------- */

static mm_settings settings;
static int reduced_motion, compact;
static int use_root;
static Window target_window;
static double render_seconds = -1;
static const char *render_path;
static int render_w = 1280, render_h = 800;

static void usage(void) {
    fprintf(stderr,
            "usage: screensaver-mesh [-root | -window-id ID] [-config FILE] [-server URL] [-token T]\n"
            "       [-source S] [-preset default|meshtastic|green terminal|amber terminal|paper]\n"
            "       [-background|-dots|-lines|-packets #rrggbb] [-clock|-no-clock] [-24h|-12h]\n"
            "       [-label TEXT | -label-user]\n"
            "       [-reduced-motion] [-render SECONDS OUT.png [-size WxH] [-preview]]\n");
    exit(2);
}

static void parse_args(int argc, char **argv) {
    mm_settings_default(&settings);

    const char *config = NULL;
    for (int i = 1; i < argc - 1; i++)
        if (!strcmp(argv[i], "-config") || !strcmp(argv[i], "--config")) config = argv[i + 1];
    char path[1024];
    if (!config) {
        const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
        if (xdg && *xdg) snprintf(path, sizeof path, "%s/screensaver-mesh/config", xdg);
        else snprintf(path, sizeof path, "%s/.config/screensaver-mesh/config", home ? home : ".");
        config = path;
    }
    mm_settings_load_file(&settings, config);

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] == '-') a++; /* --flag works too */
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;
#define TAKES(name) (!strcmp(a, name) && next && (i++, 1))
        if (TAKES("-config")) continue;
        else if (TAKES("-server")) mm_settings_set(&settings, "server", next);
        else if (TAKES("-token")) mm_settings_set(&settings, "token", next);
        else if (TAKES("-source")) mm_settings_set(&settings, "source", next);
        else if (TAKES("-preset")) mm_settings_set(&settings, "preset", next);
        else if (TAKES("-background")) mm_settings_set(&settings, "background", next);
        else if (TAKES("-dots")) mm_settings_set(&settings, "dots", next);
        else if (TAKES("-lines")) mm_settings_set(&settings, "lines", next);
        else if (TAKES("-packets")) mm_settings_set(&settings, "packets", next);
        else if (TAKES("-label")) {
            /* XScreenSaver passes -label "" when the field is empty: keep the config file's choice. */
            if (*next) {
                mm_settings_set(&settings, "label", "custom");
                mm_settings_set(&settings, "label_text", next);
            }
        }
        else if (TAKES("-window-id")) target_window = (Window)strtoul(next, NULL, 0);
        else if (TAKES("-size")) {
            if (sscanf(next, "%dx%d", &render_w, &render_h) != 2 || render_w < 1 || render_h < 1) usage();
        } else if (!strcmp(a, "-render") && i + 2 < argc) {
            render_seconds = atof(argv[i + 1]);
            render_path = argv[i + 2];
            i += 2;
        }
        else if (!strcmp(a, "-clock")) settings.show_clock = 1;
        else if (!strcmp(a, "-no-clock")) settings.show_clock = 0;
        else if (!strcmp(a, "-24h")) settings.use_24_hour = 1;
        else if (!strcmp(a, "-12h")) settings.use_24_hour = 0;
        else if (!strcmp(a, "-label-user")) mm_settings_set(&settings, "label", "user");
        else if (!strcmp(a, "-reduced-motion")) reduced_motion = 1;
        else if (!strcmp(a, "-preview")) compact = 1;
        else if (!strcmp(a, "-root")) use_root = 1;
        else if (!strcmp(a, "-fps") || !strcmp(a, "-install") || !strcmp(a, "-visual")) {
            if (!strcmp(a, "-visual") && next) i++; /* standard hack flags we can ignore */
        } else usage();
#undef TAKES
    }
}

/* ---------- live polling ---------- */

typedef struct live_event {
    struct live_event *next;
    enum { EV_NODES, EV_MESSAGE, EV_OFFLINE } type;
    double due;
    int count;
    char (*ids)[MM_ID_LEN];
    mm_message message;
} live_event;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static live_event *head, *tail;

static double now_seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void push_event(live_event *e) {
    pthread_mutex_lock(&lock);
    if (tail) tail->next = e; else head = e;
    tail = e;
    pthread_mutex_unlock(&lock);
}

typedef struct {
    char *data;
    size_t len;
} buffer;

static size_t collect(char *ptr, size_t size, size_t n, void *userdata) {
    buffer *b = userdata;
    size_t add = size * n;
    if (b->len + add > 8 * 1024 * 1024) return 0;
    char *grown = realloc(b->data, b->len + add + 1);
    if (!grown) return 0;
    b->data = grown;
    memcpy(b->data + b->len, ptr, add);
    b->len += add;
    b->data[b->len] = 0;
    return add;
}

/* Returns a malloc'd body for a 2xx response, or NULL. */
static char *http_get(CURL *curl, const char *url, size_t *length) {
    buffer b = {NULL, 0};
    char auth[400];
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", settings.token);
    struct curl_slist *headers = curl_slist_append(NULL, auth);
    headers = curl_slist_append(headers, "Accept: application/json");
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "screensaver-mesh");
    long status = 0;
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    if (rc != CURLE_OK || status < 200 || status >= 300) {
        free(b.data);
        return NULL;
    }
    *length = b.len;
    return b.data ? b.data : calloc(1, 1);
}

static void *poll_thread(void *unused) {
    (void)unused;
    CURL *curl = curl_easy_init();
    mm_tracker *tracker = mm_tracker_create();
    double next_nodes = 0, next_messages = 0;
    int failures = 0;
    char url[1024];
    while (curl && tracker) {
        double now = now_seconds();
        for (int pass = 0; pass < 2; pass++) {
            int nodes = pass == 0;
            if (now < (nodes ? next_nodes : next_messages)) continue;
            if (nodes) next_nodes = now + MM_NODES_EVERY; else next_messages = now + MM_MESSAGES_EVERY;
            if (!mm_url(url, sizeof url, &settings, nodes ? "nodes" : "messages")) continue;
            size_t length = 0;
            char *body = http_get(curl, url, &length);
            int ok = 0;
            if (body && nodes) {
                live_event *e = calloc(1, sizeof *e);
                char(*ids)[MM_ID_LEN] = calloc(MM_MAX_NODES, MM_ID_LEN);
                int n = e && ids ? mm_parse_nodes(body, length, ids, MM_MAX_NODES) : -1;
                if (n >= 0) {
                    ok = 1;
                    e->type = EV_NODES;
                    e->count = n;
                    e->ids = ids;
                    push_event(e);
                } else {
                    free(e);
                    free(ids);
                }
            } else if (body) {
                mm_message fresh[MM_MAX_MESSAGES];
                int n = mm_parse_messages(tracker, body, length, fresh, MM_MAX_MESSAGES);
                if (n >= 0) ok = 1;
                for (int i = 0; i < n; i++) {
                    live_event *e = calloc(1, sizeof *e);
                    if (!e) break;
                    e->type = EV_MESSAGE;
                    e->due = now + i * 0.35;
                    e->message = fresh[i];
                    push_event(e);
                }
            }
            free(body);
            failures = ok ? 0 : failures + 1;
            if (failures == MM_OFFLINE_AFTER) {
                live_event *e = calloc(1, sizeof *e);
                if (e) { e->type = EV_OFFLINE; push_event(e); }
            }
        }
        usleep(1000 * 1000);
    }
    return NULL;
}

static void live_start(void) {
    if (!mm_settings_live(&settings)) return;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    pthread_t t;
    if (pthread_create(&t, NULL, poll_thread, NULL) == 0) pthread_detach(t);
}

static void live_drain(mesh *m) {
    double now = now_seconds();
    pthread_mutex_lock(&lock);
    live_event **link = &head, *prev = NULL;
    while (*link) {
        live_event *e = *link;
        if (e->type == EV_MESSAGE && e->due > now) {
            prev = e;
            link = &e->next;
            continue;
        }
        *link = e->next;
        if (tail == e) tail = prev;
        if (e->type == EV_NODES && e->count > 0) {
            const char *ptrs[MM_MAX_NODES];
            for (int i = 0; i < e->count; i++) ptrs[i] = e->ids[i];
            mesh_set_live(m, 1);
            mesh_set_nodes(m, ptrs, e->count);
        } else if (e->type == EV_MESSAGE && mesh_is_live(m)) {
            mesh_message(m, e->message.from, e->message.to[0] ? e->message.to : NULL);
        } else if (e->type == EV_OFFLINE) {
            mesh_set_live(m, 0);
        }
        free(e->ids);
        free(e);
    }
    pthread_mutex_unlock(&lock);
}

/* ---------- drawing ---------- */

static mesh_color parse_or(const char *hex, const char *fallback) {
    mesh_color c;
    if (!mesh_parse_color(hex, &c)) mesh_parse_color(fallback, &c);
    return c;
}

static void draw_line(void *ctx, float x0, float y0, float x1, float y1, float width, mesh_color c) {
    cairo_t *cr = ctx;
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
    cairo_set_line_width(cr, width);
    cairo_move_to(cr, x0, y0);
    cairo_line_to(cr, x1, y1);
    cairo_stroke(cr);
}

static void draw_circle(void *ctx, float x, float y, float r, int filled, float width, mesh_color c) {
    cairo_t *cr = ctx;
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x, y, r, 0, 6.2831853);
    if (filled) {
        cairo_fill(cr);
    } else {
        cairo_set_line_width(cr, width);
        cairo_stroke(cr);
    }
}

/* The full name from the account (the first field of GECOS), else the login name. */
static const char *user_name(void) {
    static char name[256];
    if (name[0]) return name;
    struct passwd *pw = getpwuid(getuid());
    if (!pw) return NULL;
    if (pw->pw_gecos && *pw->pw_gecos) {
        snprintf(name, sizeof name, "%s", pw->pw_gecos);
        char *comma = strchr(name, ',');
        if (comma) *comma = 0;
    }
    if (!name[0] && pw->pw_name) snprintf(name, sizeof name, "%s", pw->pw_name);
    return name;
}

static void draw_clock(cairo_t *cr, const mesh *m, int width, int height) {
    char text[32];
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    strftime(text, sizeof text, settings.use_24_hour ? "%H:%M" : "%I:%M %p", &local);
    const char *shown = (!settings.use_24_hour && text[0] == '0') ? text + 1 : text;
    char label[400];
    mm_label(label, sizeof label, &settings, user_name(), mesh_is_live(m));

    mesh_color ink = parse_or(settings.dots, mm_presets[0].dots);
    double k = compact ? height / 900.0 : 1.0;
    if (k < 0.25) k = 0.25;
    double margin = 32 * k;
    (void)width;

    cairo_text_extents_t te;
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12 * k);
    cairo_text_extents(cr, label, &te);
    double label_y = height - margin;
    cairo_set_source_rgba(cr, ink.r, ink.g, ink.b, ink.a * 0.5);
    cairo_move_to(cr, margin, label_y);
    cairo_show_text(cr, label);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 48 * k);
    cairo_set_source_rgba(cr, ink.r, ink.g, ink.b, ink.a * 0.8);
    cairo_move_to(cr, margin - 2 * k, label_y - 12 * k - 8 * k);
    cairo_show_text(cr, shown);
}

static void render(cairo_t *cr, const mesh *m, int width, int height) {
    mesh_color bg = parse_or(settings.background, mm_presets[0].background);
    cairo_set_source_rgb(cr, bg.r, bg.g, bg.b);
    cairo_paint(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_GOOD);
    mesh_renderer r = {cr, draw_line, draw_circle};
    mesh_render(m, &r);
    if (settings.show_clock) draw_clock(cr, m, width, height);
}

static mesh *make_mesh(void) {
    mesh *m = mesh_create((unsigned)time(NULL) ^ ((unsigned)getpid() << 16));
    mesh_options o = mesh_default_options();
    const mm_preset *base = &mm_presets[0];
    o.background = parse_or(settings.background, base->background);
    o.node = parse_or(settings.dots, base->dots);
    o.link = parse_or(settings.lines, base->lines);
    o.packet = parse_or(settings.packets, base->packets);
    o.reduced_motion = reduced_motion;
    o.compact = compact;
    mesh_set_options(m, &o);
    return m;
}

static void sleep_seconds(double s) {
    if (s <= 0) return;
    struct timespec t = {(time_t)s, (long)((s - (double)(time_t)s) * 1e9)};
    nanosleep(&t, NULL);
}

/* ---------- entry ---------- */

static int run_render(void) {
    mesh *m = make_mesh();
    mesh_resize(m, (float)render_w, (float)render_h);
    live_start();
    double start = now_seconds(), last = start;
    while (now_seconds() - start < render_seconds) {
        sleep_seconds(1.0 / 60);
        double now = now_seconds();
        live_drain(m);
        mesh_step(m, (float)(now - last));
        last = now;
    }
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, render_w, render_h);
    cairo_t *cr = cairo_create(surface);
    render(cr, m, render_w, render_h);
    cairo_destroy(cr);
    cairo_status_t st = cairo_surface_write_to_png(surface, render_path);
    cairo_surface_destroy(surface);
    printf("nodes %d, live %d\n", mesh_node_count(m), mesh_is_live(m));
    return st == CAIRO_STATUS_SUCCESS ? 0 : 1;
}

int main(int argc, char **argv) {
    parse_args(argc, argv);
    if (render_path) return run_render();

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "screensaver-mesh: can't open display\n");
        return 1;
    }
    const char *env = getenv("XSCREENSAVER_WINDOW");
    if (!target_window && env && *env) target_window = (Window)strtoul(env, NULL, 0);
    if (!target_window && use_root) target_window = DefaultRootWindow(dpy);

    int own = 0;
    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    Window win = target_window;
    if (!win) {
        own = 1;
        int screen = DefaultScreen(dpy);
        win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0, 1280, 800, 0, BlackPixel(dpy, screen),
                                  BlackPixel(dpy, screen));
        XStoreName(dpy, win, "Mesh");
        XSetWMProtocols(dpy, win, &wm_delete, 1);
        XSelectInput(dpy, win, StructureNotifyMask | KeyPressMask | ExposureMask);
        XMapWindow(dpy, win);
    } else {
        XSelectInput(dpy, win, StructureNotifyMask | ExposureMask);
    }

    XWindowAttributes wa;
    XGetWindowAttributes(dpy, win, &wa);
    int width = wa.width, height = wa.height;
    if (!compact && width < 400 && height < 300) compact = 1; /* settings preview */

    mesh *m = make_mesh();
    mesh_resize(m, (float)width, (float)height);
    live_start();

    GC gc = XCreateGC(dpy, win, 0, NULL);
    Pixmap back = XCreatePixmap(dpy, win, (unsigned)width, (unsigned)height, (unsigned)wa.depth);
    cairo_surface_t *surface = cairo_xlib_surface_create(dpy, back, wa.visual, width, height);

    double last = now_seconds();
    for (;;) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == ConfigureNotify && (ev.xconfigure.width != width || ev.xconfigure.height != height)) {
                width = ev.xconfigure.width;
                height = ev.xconfigure.height;
                cairo_surface_destroy(surface);
                XFreePixmap(dpy, back);
                back = XCreatePixmap(dpy, win, (unsigned)width, (unsigned)height, (unsigned)wa.depth);
                surface = cairo_xlib_surface_create(dpy, back, wa.visual, width, height);
                mesh_resize(m, (float)width, (float)height);
            } else if (own && (ev.type == KeyPress ||
                               (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == wm_delete))) {
                return 0;
            }
        }
        double now = now_seconds();
        live_drain(m);
        mesh_step(m, (float)(now - last));
        last = now;

        cairo_t *cr = cairo_create(surface);
        render(cr, m, width, height);
        cairo_destroy(cr);
        cairo_surface_flush(surface);
        XCopyArea(dpy, back, win, gc, 0, 0, (unsigned)width, (unsigned)height, 0, 0);
        XFlush(dpy);

        sleep_seconds(1.0 / 60 - (now_seconds() - now));
    }
}
