/*
 * Mesh for Wayland: a live wallpaper on the background layer of any
 * compositor with wlr-layer-shell (Hyprland, Sway, river, niri, Wayfire,
 * labwc, KDE Plasma...). It draws the core with OpenGL ES, one surface per
 * monitor, and polls MeshMonitor with libcurl on a worker thread.
 *
 *   mesh-wallpaper                    on every monitor, until killed
 *   mesh-wallpaper -output DP-1       only on the named monitors (comma-separated)
 *   mesh-wallpaper -render 5 out.png [-size 1280x800]
 *                                     test: render off screen to a PNG, no compositor needed
 *
 * Settings come from ~/.config/screensaver-mesh/config (the same file and
 * keys as the XScreenSaver hack, see config.example), then from flags:
 * -server URL -token T -source S -preset NAME
 * -background/-dots/-lines/-packets #rrggbb[aa] -clock/-no-clock -24h/-12h
 * -label TEXT (custom text under the clock) or -label-user (your name)
 * -reduced-motion -fps N -idle-fps N -output NAMES -config FILE.
 * The config file is read again whenever it changes.
 *
 * It is built to stay out of the way while it runs:
 *   - frames are paced by the compositor's frame callbacks, so nothing is
 *     drawn for a monitor that is off or not being shown;
 *   - it draws at `fps` only while packets are moving and at `idle_fps`
 *     while the nodes merely drift (0 for either: every refresh);
 *   - when the compositor can't keep up, it draws less often until it can;
 *   - under Hyprland it stops drawing behind a full-screen window, and it
 *     draws in a way that lets Hyprland keep its blurred wallpaper cached;
 *   - with nothing on screen it stops polling MeshMonitor too.
 */
#define _GNU_SOURCE

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <cairo.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include "hyprland.h"
#include "live.h"
#include "mesh.h"
#include "meshmonitor.h"
#include "render_gl.h"

#ifndef MESH_VERSION
#define MESH_VERSION "dev"
#endif

#define NAMESPACE "mesh-wallpaper" /* what compositors call the surface, e.g. in Hyprland layer rules */
#define BUSY_TAIL 2.5   /* seconds at full frame rate after the last packet: rings fade, nodes settle */
#define STALL_AFTER 1.0 /* no frame callback for this long: the monitor isn't showing us */
#define STALL_PROBE 10.0

/* ---------- options ---------- */

typedef struct {
    mm_settings mm;
    int reduced_motion;
    double fps, idle_fps;
    char outputs[256]; /* comma-separated monitor names; empty for all */
    int pause_fullscreen, keep_on_top;
    double blur_fps; /* Hyprland: how often the blur behind windows catches up; 0 for every frame */
    int yield;       /* draw less often while the compositor is falling behind */
} host_settings;

static host_settings cfg;
static char config_path[1024];
static int saved_argc;
static char **saved_argv;
static double render_seconds = -1;
static const char *render_path;
static int render_w = 1280, render_h = 800;

static void usage(void) {
    fprintf(stderr,
            "usage: mesh-wallpaper [-output NAME[,NAME...]] [-config FILE] [-server URL] [-token T] [-source S]\n"
            "       [-preset default|meshtastic|green terminal|amber terminal|paper]\n"
            "       [-background|-dots|-lines|-packets #rrggbb[aa]] [-clock|-no-clock] [-24h|-12h]\n"
            "       [-label TEXT | -label-user] [-reduced-motion] [-fps N] [-idle-fps N]\n"
            "       [-render SECONDS OUT.png [-size WxH]] [-version]\n");
    exit(2);
}

static int truthy(const char *v) {
    return !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes") || !strcmp(v, "on");
}

/* One setting, from the config file or a flag. The core takes the keys every system shares. */
static void set_option(const char *key, const char *value) {
    if (!strcmp(key, "fps")) cfg.fps = atof(value);
    else if (!strcmp(key, "idle_fps")) cfg.idle_fps = atof(value);
    else if (!strcmp(key, "output")) snprintf(cfg.outputs, sizeof cfg.outputs, "%s", value);
    else if (!strcmp(key, "pause_fullscreen")) cfg.pause_fullscreen = truthy(value);
    else if (!strcmp(key, "keep_on_top")) cfg.keep_on_top = truthy(value);
    else if (!strcmp(key, "blur_fps")) cfg.blur_fps = atof(value);
    else if (!strcmp(key, "yield")) cfg.yield = truthy(value);
    else mm_settings_set(&cfg.mm, key, value);
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    return s;
}

/* key=value (or "key value") lines; # starts a comment. Same format as mm_settings_load_file. */
static void load_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[600];
    while (fgets(line, sizeof line, f)) {
        char *key = trim(line);
        if (!key[0] || key[0] == '#') continue;
        char *sep = strchr(key, '=');
        if (!sep) sep = strpbrk(key, " \t");
        if (!sep) continue;
        *sep = 0;
        set_option(trim(key), trim(sep + 1));
    }
    fclose(f);
}

/* Defaults, then the config file, then the flags. Runs again when the file changes. */
static void load_settings(void) {
    int argc = saved_argc;
    char **argv = saved_argv;
    memset(&cfg, 0, sizeof cfg);
    mm_settings_default(&cfg.mm);
    cfg.fps = 60;
    cfg.idle_fps = 30;
    cfg.pause_fullscreen = 1;
    cfg.keep_on_top = 1;
    cfg.blur_fps = 4;
    cfg.yield = 1;

    const char *config = NULL;
    for (int i = 1; i < argc - 1; i++)
        if (!strcmp(argv[i], "-config") || !strcmp(argv[i], "--config")) config = argv[i + 1];
    if (config) {
        snprintf(config_path, sizeof config_path, "%s", config);
    } else {
        const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
        if (xdg && *xdg) snprintf(config_path, sizeof config_path, "%s/screensaver-mesh/config", xdg);
        else snprintf(config_path, sizeof config_path, "%s/.config/screensaver-mesh/config", home ? home : ".");
    }
    load_file(config_path);

    static const char *const with_value[] = {"server", "token",   "source", "preset", "background",
                                             "dots",   "lines",   "packets", "output", "fps", NULL};
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] == '-') a++; /* --flag works too */
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;
        int known = 0;
        for (int k = 0; with_value[k] && !known; k++) {
            if (a[0] != '-' || strcmp(a + 1, with_value[k]) != 0 || !next) continue;
            set_option(with_value[k], next);
            known = 1;
            i++;
        }
        if (known) continue;
#define TAKES(name) (!strcmp(a, name) && next && (i++, 1))
        if (TAKES("-config")) continue;
        else if (TAKES("-idle-fps")) set_option("idle_fps", next);
        else if (TAKES("-label")) {
            if (*next) {
                set_option("label", "custom");
                set_option("label_text", next);
            }
        } else if (TAKES("-size")) {
            if (sscanf(next, "%dx%d", &render_w, &render_h) != 2 || render_w < 1 || render_h < 1) usage();
        } else if (!strcmp(a, "-render") && i + 2 < argc) {
            render_seconds = atof(argv[i + 1]);
            render_path = argv[i + 2];
            i += 2;
        }
        else if (!strcmp(a, "-clock")) cfg.mm.show_clock = 1;
        else if (!strcmp(a, "-no-clock")) cfg.mm.show_clock = 0;
        else if (!strcmp(a, "-24h")) cfg.mm.use_24_hour = 1;
        else if (!strcmp(a, "-12h")) cfg.mm.use_24_hour = 0;
        else if (!strcmp(a, "-label-user")) set_option("label", "user");
        else if (!strcmp(a, "-reduced-motion")) cfg.reduced_motion = 1;
        else if (!strcmp(a, "-version")) {
            puts("mesh-wallpaper " MESH_VERSION);
            exit(0);
        } else usage();
#undef TAKES
    }

    /* fps 0 draws on every frame callback; idle_fps never exceeds fps. */
    if (cfg.fps < 0) cfg.fps = 0;
    if (cfg.idle_fps <= 0 || (cfg.fps > 0 && cfg.idle_fps > cfg.fps)) cfg.idle_fps = cfg.fps;
}

static mesh_color parse_or(const char *hex, const char *fallback) {
    mesh_color c;
    if (!mesh_parse_color(hex, &c)) mesh_parse_color(fallback, &c);
    return c;
}

static mesh_options current_options(void) {
    mesh_options o = mesh_default_options();
    const mm_preset *base = &mm_presets[0];
    o.background = parse_or(cfg.mm.background, base->background);
    o.node = parse_or(cfg.mm.dots, base->dots);
    o.link = parse_or(cfg.mm.lines, base->lines);
    o.packet = parse_or(cfg.mm.packets, base->packets);
    o.reduced_motion = cfg.reduced_motion;
    return o;
}

/* ---------- outputs ---------- */

typedef struct output {
    struct output *next;
    uint32_t id; /* the wl_output global's name */
    struct wl_output *wl;
    char name[HYPR_NAME_LEN];
    int integer_scale, ready;
    double period; /* seconds per refresh, 0 if unknown */

    struct wl_surface *surface; /* has the layer-shell role */
    struct zwlr_layer_surface_v1 *layer;
    /* What we draw on: `surface` itself, or (see use_canvas) a subsurface over a one-pixel backdrop. */
    struct wl_surface *canvas;
    struct wl_subsurface *subsurface;
    struct wl_buffer *backdrop;
    struct wp_viewport *backdrop_viewport;
    double blur_at; /* when the backdrop is next committed */
    struct wp_viewport *viewport;
    struct wp_fractional_scale_v1 *fractional;
    struct wl_egl_window *window;
    EGLSurface egl;
    struct wl_callback *frame;
    int width, height; /* points */
    int px_w, px_h;    /* pixels */
    int scale120;      /* preferred fractional scale * 120, 0 if not told */
    int configured;
    int covered; /* a full-screen window is over us (Hyprland) */
    int stalled; /* frame callbacks stopped coming */
    double frame_asked, next_frame, last_step, busy_until, raised_at;
    int raises, frames;
    double waited; /* for the debug report: time spent waiting for frame callbacks */
    int has_alpha; /* the buffer format has an alpha channel */

    /* Yielding: how far behind the pace we asked for the frames come, and how far we have backed off. */
    double asked, pace_from, pace_last, pace_asked, calm_since, sped_up_at, patience;
    int backoff; /* draw at 1/(backoff + 1) of the frame rate */

    mesh *m;
    int mesh_w, mesh_h;
    gl_text text;
    char text_shown[512];
    float text_x, text_y;
    long text_minute;
} output;

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wl_subcompositor *subcompositor;
static struct wl_shm *shm;
static struct wp_viewporter *viewporter;
static int hyprland; /* its IPC sockets are there */
static struct wp_fractional_scale_manager_v1 *fractional_manager;
static EGLDisplay egl_display = EGL_NO_DISPLAY;
static EGLConfig egl_config, egl_config_opaque; /* with and without an alpha channel */
static EGLContext egl_context = EGL_NO_CONTEXT;
static int gl_ready;
static output *outputs;

static int output_visible(const output *o) { return o->configured && !o->covered && !o->stalled; }

static int output_wanted(const output *o) {
    if (!cfg.outputs[0] || !strcmp(cfg.outputs, "*")) return 1;
    char list[sizeof cfg.outputs];
    snprintf(list, sizeof list, "%s", cfg.outputs);
    for (char *name = strtok(list, ", \t"); name; name = strtok(NULL, ", \t"))
        if (!strcmp(name, o->name)) return 1;
    return 0;
}

/* ---------- live ---------- */

static live_event *pending;
static char (*node_ids)[MM_ID_LEN];
static int node_count;

static void nodes_apply(output *o) {
    if (node_count <= 0 || o->mesh_w <= 0) return; /* the mesh places new nodes inside its size */
    const char *ids[MM_MAX_NODES];
    for (int i = 0; i < node_count; i++) ids[i] = node_ids[i];
    mesh_set_live(o->m, 1);
    mesh_set_nodes(o->m, ids, node_count);
}

static void go_simulated(void) {
    free(node_ids);
    node_ids = NULL;
    node_count = 0;
    for (output *o = outputs; o; o = o->next) mesh_set_live(o->m, 0);
}

/* Show what's due. `arrived` says the worker has new events to collect. */
static void live_apply(double now, int arrived) {
    live_event **link = &pending;
    if (arrived) {
        while (*link) link = &(*link)->next;
        *link = live_take();
    }
    link = &pending;
    while (*link) {
        live_event *e = *link;
        if (e->type == LIVE_MESSAGE && e->due > now) {
            link = &e->next;
            continue;
        }
        *link = e->next;
        if (!mm_settings_live(&cfg.mm)) {
            /* left over from before the server was removed from the settings */
        } else if (e->type == LIVE_NODES && e->count > 0) {
            free(node_ids);
            node_ids = e->ids;
            node_count = e->count;
            e->ids = NULL;
            for (output *o = outputs; o; o = o->next) nodes_apply(o);
        } else if (e->type == LIVE_MESSAGE) {
            for (output *o = outputs; o; o = o->next) {
                if (!output_visible(o) || !mesh_is_live(o->m)) continue;
                mesh_message(o->m, e->message.from, e->message.to[0] ? e->message.to : NULL);
                o->busy_until = now + BUSY_TAIL;
                if (o->next_frame > now) o->next_frame = now;
            }
        } else if (e->type == LIVE_OFFLINE) {
            go_simulated();
        }
        free(e->ids);
        free(e);
    }
}

/* ---------- drawing ---------- */

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

/*
 * The label's font and the time's, at one scale. They are made once and kept:
 * asking cairo for a face by name every minute leaves a copy of the font and
 * its glyphs in cairo's cache each time (it holds up to 256 of them), and
 * sends fontconfig off to check whether its configuration has changed.
 */
typedef struct {
    double scale;
    cairo_scaled_font_t *label, *time;
} clock_fonts;

static cairo_scaled_font_t *make_font(cairo_font_weight_t weight, double size) {
    cairo_font_face_t *face = cairo_toy_font_face_create("Sans", CAIRO_FONT_SLANT_NORMAL, weight);
    cairo_font_options_t *options = cairo_font_options_create();
    cairo_matrix_t matrix, identity;
    cairo_matrix_init_scale(&matrix, size, size);
    cairo_matrix_init_identity(&identity);
    cairo_scaled_font_t *font = cairo_scaled_font_create(face, &matrix, &identity, options);
    cairo_font_options_destroy(options);
    cairo_font_face_destroy(face);
    return font;
}

static const clock_fonts *fonts_at(double scale) {
    static clock_fonts sets[4]; /* one per scale in use; monitors rarely have more than two */
    static int next;
    for (int i = 0; i < 4; i++)
        if (sets[i].label && sets[i].scale == scale) return &sets[i];
    clock_fonts *set = &sets[next];
    next = (next + 1) % 4;
    if (set->label) cairo_scaled_font_destroy(set->label);
    if (set->time) cairo_scaled_font_destroy(set->time);
    set->scale = scale;
    set->label = make_font(CAIRO_FONT_WEIGHT_NORMAL, 12 * scale);
    set->time = make_font(CAIRO_FONT_WEIGHT_BOLD, 48 * scale);
    return set;
}

/*
 * The clock and its label, laid out like the other systems: label baseline
 * 32 points up from the bottom left, the time above it. Drawn with cairo
 * into an alpha texture, and only again when the minute or the label changes.
 */
static void clock_update(output *o, float scale) {
    time_t now = time(NULL);
    if (o->text.texture && now / 60 == o->text_minute && o->text_shown[0]) return;
    o->text_minute = now / 60;

    char text[32], label[400], shown[sizeof o->text_shown];
    struct tm local;
    localtime_r(&now, &local);
    strftime(text, sizeof text, cfg.mm.use_24_hour ? "%H:%M" : "%I:%M %p", &local);
    const char *time_text = (!cfg.mm.use_24_hour && text[0] == '0') ? text + 1 : text;
    mm_label(label, sizeof label, &cfg.mm, user_name(), mesh_is_live(o->m));
    snprintf(shown, sizeof shown, "%s\n%s\n%.4f", time_text, label, scale);
    if (o->text.texture && !strcmp(shown, o->text_shown)) return;
    memcpy(o->text_shown, shown, sizeof shown);

    double k = scale, margin = 32 * k;
    const clock_fonts *fonts = fonts_at(k);
    cairo_text_extents_t le, te;
    cairo_scaled_font_text_extents(fonts->label, label, &le);
    cairo_scaled_font_text_extents(fonts->time, time_text, &te);

    /* Everything relative to the label's origin; the time starts 2 points left and 20 up. */
    double tx = -2 * k, ty = -20 * k;
    double left = floor(fmin(le.x_bearing, tx + te.x_bearing)) - 2;
    double top = floor(fmin(le.y_bearing, ty + te.y_bearing)) - 2;
    double right = ceil(fmax(le.x_bearing + le.width, tx + te.x_bearing + te.width)) + 2;
    double bottom = ceil(fmax(le.y_bearing + le.height, ty + te.y_bearing + te.height)) + 2;
    int w = (int)(right - left), h = (int)(bottom - top);
    if (w < 1 || h < 1 || w > 8192 || h > 8192) return;

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_A8, w, h);
    cairo_t *cr = cairo_create(surface);
    cairo_set_scaled_font(cr, fonts->label);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.5);
    cairo_move_to(cr, -left, -top);
    cairo_show_text(cr, label);
    cairo_set_scaled_font(cr, fonts->time);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.8);
    cairo_move_to(cr, -left + tx, -top + ty);
    cairo_show_text(cr, time_text);
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    if (cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS)
        gl_text_upload(&o->text, cairo_image_surface_get_data(surface), w, h, cairo_image_surface_get_stride(surface));
    cairo_surface_destroy(surface);
    o->text_x = (float)(round(margin) + left);
    o->text_y = (float)(round(o->height * k - margin) + top);
}

static void draw(output *o) {
    float scale = (float)o->px_w / (float)o->width;
    gl_frame_begin(o->px_w, o->px_h, scale, parse_or(cfg.mm.background, mm_presets[0].background));
    gl_frame_mesh(o->m);
    if (cfg.mm.show_clock) {
        clock_update(o, scale);
        gl_text_draw(&o->text, o->text_x, o->text_y, parse_or(cfg.mm.dots, mm_presets[0].dots));
    }
    gl_frame_end();
}

static void advance(output *o, double now) {
    double dt = now - o->last_step;
    if (dt < 0 || dt > 0.25) dt = 1.0 / 60; /* just back on screen: carry on, don't catch up */
    o->last_step = now;
    while (dt > 1e-6) { /* the core takes steps of at most 50 ms */
        double step = dt > 0.05 ? 0.05 : dt;
        mesh_step(o->m, (float)step);
        dt -= step;
    }
    if (mesh_packet_count(o->m) > 0) o->busy_until = now + BUSY_TAIL;
}

static void frame_done(void *data, struct wl_callback *callback, uint32_t time_ms) {
    (void)time_ms;
    output *o = data;
    wl_callback_destroy(callback);
    if (o->frame == callback) o->frame = NULL;
    o->stalled = 0;
    o->waited += live_now() - o->frame_asked;
}

static const struct wl_callback_listener frame_listener = {frame_done};

/*
 * Seconds until the next frame: `fps` while packets move, `idle_fps` otherwise, on a whole number of
 * refreshes; 0 means as soon as the compositor asks. Backing off stretches it.
 */
static double frame_interval(const output *o, double now) {
    double fps = now < o->busy_until ? cfg.fps : cfg.idle_fps;
    double want = fps > 0 ? 1.0 / fps : 0;
    if (o->period > 0) {
        double refreshes = floor(want / o->period + 0.5);
        if (fps > 0) want = (refreshes < 1 ? 1 : refreshes) * o->period;
        if (o->backoff) want = (want > 0 ? want : o->period) * (o->backoff + 1);
    }
    return want;
}

/*
 * A frame is only drawn once the compositor has asked for it, so when the
 * compositor can't keep up, frames come slower than the pace we set. That
 * means the GPU is short: something else needs it, or our frames cost the
 * compositor more than it has. Either way, asking for fewer is what helps,
 * so when a second's worth of frames took a third longer than asked, halve
 * the rate (then a third, then a quarter). Try the faster rate again after
 * it has held for a while; each attempt that fails doubles the wait before
 * the next, so a machine that can't do it isn't pushed every few seconds.
 */
static void yield_check(output *o, double now) {
    if (!cfg.yield || o->period <= 0) {
        o->backoff = 0;
        return;
    }
    if (o->patience <= 0) o->patience = 2;
    if (now - o->pace_last > 0.25) { /* just back on screen: start measuring afresh */
        o->pace_from = now;
        o->pace_asked = 0;
    } else {
        o->pace_asked += o->asked;
    }
    o->pace_last = now;
    if (now - o->pace_from < 1 || o->pace_asked <= 0) return;
    double behind = (now - o->pace_from) / o->pace_asked; /* 1 when on pace */
    o->pace_from = now;
    o->pace_asked = 0;

    if (behind > 1.33) {
        o->calm_since = 0;
        if (o->backoff >= 3) return;
        if (now - o->sped_up_at < 4) o->patience = fmin(o->patience * 2, 60);
        o->backoff++;
    } else {
        if (o->calm_since <= 0) o->calm_since = now;
        if (o->backoff > 0 && now - o->calm_since >= o->patience) {
            o->backoff--;
            o->sped_up_at = now;
            o->calm_since = 0;
        } else if (o->patience > 2 && now - o->sped_up_at > 4) {
            o->patience = 2; /* the last speed-up held */
        }
    }
}

static void output_render(output *o, double now) {
    if (!eglMakeCurrent(egl_display, o->egl, o->egl, egl_context)) return;
    advance(o, now);
    draw(o);
    o->frame = wl_surface_frame(o->canvas);
    wl_callback_add_listener(o->frame, &frame_listener, o);
    o->frame_asked = now;
    eglSwapBuffers(egl_display, o->egl);
    if (o->subsurface && now >= o->blur_at) {
        /* Committing the layer surface is what tells Hyprland the wallpaper changed: blur it again. */
        wl_surface_commit(o->surface);
        o->blur_at = now + 1.0 / cfg.blur_fps;
    }
    /* Keep to the schedule rather than to when we happened to wake up, so the pace doesn't drift. */
    yield_check(o, now);
    double interval = frame_interval(o, now);
    o->asked = interval > o->period ? interval : o->period;
    o->next_frame = (now - o->next_frame < interval ? o->next_frame : now) + interval;
    o->frames++;
}

/* ---------- surfaces ---------- */

/*
 * Hyprland keeps a blurred copy of the wallpaper for the translucent windows
 * above it, and makes a new one, redrawing the whole monitor, every time a
 * background layer surface commits. For an animated wallpaper that is a
 * full-screen blur per frame, and on an integrated GPU it halves the frame
 * rate. So under Hyprland the mesh is drawn on a subsurface: its commits
 * only damage what they show, and the layer surface below it, a single
 * stretched pixel of the background color, is committed `blur_fps` times a
 * second to let the blur catch up. At the speed the nodes drift, a blur
 * that is a quarter of a second old looks the same.
 */
static int wants_alpha(void) { return parse_or(cfg.mm.background, mm_presets[0].background).a < 1.0f; }

static int use_canvas(void) { return hyprland && cfg.blur_fps > 0 && subcompositor && shm && viewporter; }

static struct wl_buffer *pixel_buffer(mesh_color c) {
    uint32_t pixel = (uint32_t)lroundf(c.a * 255) << 24 | (uint32_t)lroundf(c.r * c.a * 255) << 16 |
                     (uint32_t)lroundf(c.g * c.a * 255) << 8 | (uint32_t)lroundf(c.b * c.a * 255);
    int fd = memfd_create("mesh-wallpaper", MFD_CLOEXEC);
    if (fd < 0) return NULL;
    struct wl_buffer *buffer = NULL;
    if (write(fd, &pixel, sizeof pixel) == sizeof pixel) {
        struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, sizeof pixel);
        buffer = wl_shm_pool_create_buffer(pool, 0, 1, 1, sizeof pixel, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
    }
    close(fd);
    return buffer;
}

static void set_opaque(output *o, struct wl_surface *surface) {
    struct wl_region *region = NULL;
    if (parse_or(cfg.mm.background, mm_presets[0].background).a >= 1.0f) {
        region = wl_compositor_create_region(compositor);
        wl_region_add(region, 0, 0, o->width, o->height);
    }
    wl_surface_set_opaque_region(surface, region);
    if (region) wl_region_destroy(region);
}

/* The canvas gets its state with its next frame; the backdrop under it is committed here. */
static void output_set_opaque(output *o) {
    set_opaque(o, o->canvas);
    if (!o->subsurface) return;
    if (!o->backdrop) o->backdrop = pixel_buffer(parse_or(cfg.mm.background, mm_presets[0].background));
    set_opaque(o, o->surface);
    wl_surface_attach(o->surface, o->backdrop, 0, 0);
    wl_surface_damage_buffer(o->surface, 0, 0, 1, 1);
    wp_viewport_set_destination(o->backdrop_viewport, o->width, o->height);
    wl_surface_commit(o->surface);
}

/* The surface has a size (or a new scale): size the buffer to the monitor's real pixels. */
static void output_resize(output *o) {
    if (o->width <= 0 || o->height <= 0) return;
    if (o->viewport) {
        double scale = o->scale120 ? o->scale120 / 120.0 : o->integer_scale;
        o->px_w = (int)lround(o->width * scale);
        o->px_h = (int)lround(o->height * scale);
        wp_viewport_set_destination(o->viewport, o->width, o->height);
    } else {
        o->px_w = o->width * o->integer_scale;
        o->px_h = o->height * o->integer_scale;
        wl_surface_set_buffer_scale(o->canvas, o->integer_scale);
    }
    if (!o->window) {
        /* Without an alpha channel the compositor can draw us without blending: one less read of the screen per frame. */
        o->has_alpha = wants_alpha();
        o->window = wl_egl_window_create(o->canvas, o->px_w, o->px_h);
        o->egl = o->window ? eglCreateWindowSurface(egl_display, o->has_alpha ? egl_config : egl_config_opaque,
                                                    (EGLNativeWindowType)o->window, NULL)
                           : EGL_NO_SURFACE;
        if (o->egl == EGL_NO_SURFACE || !eglMakeCurrent(egl_display, o->egl, o->egl, egl_context)) {
            fprintf(stderr, "mesh-wallpaper: can't create a GL surface for %s\n", o->name);
            return;
        }
        eglSwapInterval(egl_display, 0); /* frame callbacks pace us; never block in eglSwapBuffers */
        if (!gl_ready && !(gl_ready = gl_setup())) {
            fprintf(stderr, "mesh-wallpaper: OpenGL ES 2 setup failed\n");
            exit(1);
        }
    } else {
        wl_egl_window_resize(o->window, o->px_w, o->px_h, 0, 0);
    }
    output_set_opaque(o);
    if (o->width != o->mesh_w || o->height != o->mesh_h) {
        int first = o->mesh_w == 0;
        o->mesh_w = o->width;
        o->mesh_h = o->height;
        mesh_resize(o->m, (float)o->width, (float)o->height);
        if (first) nodes_apply(o);
    }
    o->text_shown[0] = 0;
    o->configured = 1;
    o->next_frame = 0;
}

static void output_drop_surface(output *o) {
    if (o->frame) wl_callback_destroy(o->frame);
    if (o->egl != EGL_NO_SURFACE && o->egl) {
        eglMakeCurrent(egl_display, o->egl, o->egl, egl_context);
        gl_text_free(&o->text);
        eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(egl_display, o->egl);
    }
    if (o->window) wl_egl_window_destroy(o->window);
    if (o->viewport) wp_viewport_destroy(o->viewport);
    if (o->fractional) wp_fractional_scale_v1_destroy(o->fractional);
    if (o->subsurface) {
        wl_subsurface_destroy(o->subsurface);
        wl_surface_destroy(o->canvas);
        wp_viewport_destroy(o->backdrop_viewport);
        if (o->backdrop) wl_buffer_destroy(o->backdrop);
    }
    if (o->layer) zwlr_layer_surface_v1_destroy(o->layer);
    if (o->surface) wl_surface_destroy(o->surface);
    o->canvas = NULL;
    o->subsurface = NULL;
    o->backdrop = NULL;
    o->backdrop_viewport = NULL;
    o->frame = NULL;
    o->egl = EGL_NO_SURFACE;
    o->window = NULL;
    o->viewport = NULL;
    o->fractional = NULL;
    o->layer = NULL;
    o->surface = NULL;
    memset(&o->text, 0, sizeof o->text);
    o->configured = o->stalled = 0;
    o->width = o->height = o->scale120 = 0;
}

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer, uint32_t serial, uint32_t width,
                            uint32_t height) {
    output *o = data;
    zwlr_layer_surface_v1_ack_configure(layer, serial);
    if (!width || !height) return;
    if (o->configured && (int)width == o->width && (int)height == o->height) return;
    o->width = (int)width;
    o->height = (int)height;
    output_resize(o);
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer) {
    (void)layer;
    output_drop_surface(data);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {layer_configure, layer_closed};

static void fractional_scale(void *data, struct wp_fractional_scale_v1 *fractional, uint32_t scale) {
    (void)fractional;
    output *o = data;
    if ((int)scale == o->scale120) return;
    o->scale120 = (int)scale;
    if (o->configured) output_resize(o);
}

static const struct wp_fractional_scale_v1_listener fractional_listener = {fractional_scale};

static void output_create_surface(output *o) {
    o->surface = wl_compositor_create_surface(compositor);
    o->layer = zwlr_layer_shell_v1_get_layer_surface(layer_shell, o->surface, o->wl,
                                                     ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, NAMESPACE);
    zwlr_layer_surface_v1_set_size(o->layer, 0, 0);
    zwlr_layer_surface_v1_set_anchor(o->layer,
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(o->layer, -1); /* the whole monitor, under any bars */
    zwlr_layer_surface_v1_set_keyboard_interactivity(o->layer, 0);
    zwlr_layer_surface_v1_add_listener(o->layer, &layer_listener, o);

    o->canvas = o->surface;
    if (use_canvas()) {
        o->canvas = wl_compositor_create_surface(compositor);
        o->subsurface = wl_subcompositor_get_subsurface(subcompositor, o->canvas, o->surface);
        wl_subsurface_set_desync(o->subsurface); /* its frames don't wait for the layer surface */
        o->backdrop_viewport = wp_viewporter_get_viewport(viewporter, o->surface);
        o->blur_at = 0;
    }

    /* No input region: clicks and scrolls on the desktop go to the compositor, not to us. */
    struct wl_region *empty = wl_compositor_create_region(compositor);
    wl_surface_set_input_region(o->surface, empty);
    if (o->subsurface) wl_surface_set_input_region(o->canvas, empty);
    wl_region_destroy(empty);

    if (viewporter) o->viewport = wp_viewporter_get_viewport(viewporter, o->canvas);
    if (viewporter && fractional_manager) {
        o->fractional = wp_fractional_scale_manager_v1_get_fractional_scale(fractional_manager, o->canvas);
        wp_fractional_scale_v1_add_listener(o->fractional, &fractional_listener, o);
    }
    wl_surface_commit(o->surface); /* the first configure answers this */
}

/* Give every wanted monitor a surface, and take it away from the others. */
static void outputs_sync(void) {
    if (!compositor || !layer_shell || egl_context == EGL_NO_CONTEXT) return;
    for (output *o = outputs; o; o = o->next) {
        if (!o->ready) continue;
        int wanted = output_wanted(o);
        if (wanted && !o->surface) output_create_surface(o);
        else if (!wanted && o->surface) output_drop_surface(o);
    }
}

static void output_geometry(void *data, struct wl_output *wl, int32_t x, int32_t y, int32_t pw, int32_t ph,
                            int32_t subpixel, const char *make, const char *model, int32_t transform) {
    (void)data, (void)wl, (void)x, (void)y, (void)pw, (void)ph, (void)subpixel, (void)make, (void)model;
    (void)transform;
}

static void output_mode(void *data, struct wl_output *wl, uint32_t flags, int32_t width, int32_t height,
                        int32_t refresh) {
    (void)wl, (void)width, (void)height;
    output *o = data;
    if ((flags & WL_OUTPUT_MODE_CURRENT) && refresh > 0) o->period = 1000.0 / refresh; /* refresh is in mHz */
}

static void output_done(void *data, struct wl_output *wl) {
    (void)wl;
    output *o = data;
    o->ready = 1;
    outputs_sync();
}

static void output_scale(void *data, struct wl_output *wl, int32_t factor) {
    (void)wl;
    output *o = data;
    if (factor < 1 || factor == o->integer_scale) return;
    o->integer_scale = factor;
    if (o->configured && !o->scale120) output_resize(o);
}

static void output_name(void *data, struct wl_output *wl, const char *name) {
    (void)wl;
    output *o = data;
    snprintf(o->name, sizeof o->name, "%s", name);
}

static void output_description(void *data, struct wl_output *wl, const char *description) {
    (void)data, (void)wl, (void)description;
}

static const struct wl_output_listener output_listener = {output_geometry, output_mode, output_done,
                                                          output_scale,    output_name, output_description};

static mesh *make_mesh(unsigned salt) {
    mesh *m = mesh_create((unsigned)time(NULL) ^ ((unsigned)getpid() << 16) ^ (salt * 2654435761u));
    mesh_options options = current_options();
    if (m) mesh_set_options(m, &options);
    return m;
}

static void registry_global(void *data, struct wl_registry *registry, uint32_t id, const char *interface,
                            uint32_t version) {
    (void)data;
    if (!strcmp(interface, wl_compositor_interface.name)) {
        compositor = wl_registry_bind(registry, id, &wl_compositor_interface, version < 4 ? version : 4);
    } else if (!strcmp(interface, wl_subcompositor_interface.name)) {
        subcompositor = wl_registry_bind(registry, id, &wl_subcompositor_interface, 1);
    } else if (!strcmp(interface, wl_shm_interface.name)) {
        shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
    } else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
        layer_shell = wl_registry_bind(registry, id, &zwlr_layer_shell_v1_interface, 1);
    } else if (!strcmp(interface, wp_viewporter_interface.name)) {
        viewporter = wl_registry_bind(registry, id, &wp_viewporter_interface, 1);
    } else if (!strcmp(interface, wp_fractional_scale_manager_v1_interface.name)) {
        fractional_manager = wl_registry_bind(registry, id, &wp_fractional_scale_manager_v1_interface, 1);
    } else if (!strcmp(interface, wl_output_interface.name)) {
        output *o = calloc(1, sizeof *o);
        if (!o || !(o->m = make_mesh(id))) {
            free(o);
            return;
        }
        o->id = id;
        o->integer_scale = 1;
        o->egl = EGL_NO_SURFACE;
        o->wl = wl_registry_bind(registry, id, &wl_output_interface, version < 4 ? version : 4);
        wl_output_add_listener(o->wl, &output_listener, o);
        o->next = outputs;
        outputs = o;
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t id) {
    (void)data, (void)registry;
    for (output **link = &outputs; *link; link = &(*link)->next) {
        output *o = *link;
        if (o->id != id) continue;
        *link = o->next;
        output_drop_surface(o);
        wl_output_destroy(o->wl);
        mesh_destroy(o->m);
        free(o);
        return;
    }
}

static const struct wl_registry_listener registry_listener = {registry_global, registry_global_remove};

/* An RGBA8888 or RGBX8888 window config with no depth, stencil, or multisample buffers to pay for. */
static EGLConfig pick_config(int alpha) {
    const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                 EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                                 EGL_ALPHA_SIZE, alpha ? 8 : 0, EGL_NONE};
    EGLConfig configs[128], fallback = NULL;
    EGLint count = 0;
    if (!eglChooseConfig(egl_display, attributes, configs, 128, &count)) return NULL;
    for (EGLint i = 0; i < count; i++) {
        EGLint red = 0, bits = 0, depth = 0, stencil = 0, samples = 0;
        eglGetConfigAttrib(egl_display, configs[i], EGL_RED_SIZE, &red);
        eglGetConfigAttrib(egl_display, configs[i], EGL_ALPHA_SIZE, &bits);
        eglGetConfigAttrib(egl_display, configs[i], EGL_DEPTH_SIZE, &depth);
        eglGetConfigAttrib(egl_display, configs[i], EGL_STENCIL_SIZE, &stencil);
        eglGetConfigAttrib(egl_display, configs[i], EGL_SAMPLES, &samples);
        if (red != 8 || bits != (alpha ? 8 : 0)) continue;
        if (!depth && !stencil && !samples) return configs[i];
        if (!fallback) fallback = configs[i];
    }
    return fallback;
}

static int egl_start(void) {
    static const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    egl_display = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, display, NULL);
    if (egl_display == EGL_NO_DISPLAY || !eglInitialize(egl_display, NULL, NULL)) return 0;
    if (!eglBindAPI(EGL_OPENGL_ES_API)) return 0;
    if (!(egl_config = pick_config(1))) return 0;
    /* One context draws to both kinds of surface only if it isn't tied to a config. */
    const char *extensions = eglQueryString(egl_display, EGL_EXTENSIONS);
    int any_config = extensions && strstr(extensions, "EGL_KHR_no_config_context");
    egl_config_opaque = any_config ? pick_config(0) : NULL;
    if (!egl_config_opaque) egl_config_opaque = egl_config, any_config = 0;
    egl_context = eglCreateContext(egl_display, any_config ? EGL_NO_CONFIG_KHR : egl_config, EGL_NO_CONTEXT,
                                   context_attributes);
    return egl_context != EGL_NO_CONTEXT;
}

/* ---------- Hyprland ---------- */

static void refresh_covered(void) {
    char names[16][HYPR_NAME_LEN];
    int count = cfg.pause_fullscreen ? hypr_fullscreen_monitors(names, 16) : 0;
    for (output *o = outputs; o; o = o->next) {
        int covered = 0;
        for (int i = 0; i < count; i++)
            if (!strcmp(names[i], o->name)) covered = 1;
        if (covered == o->covered) continue;
        o->covered = covered;
        if (!covered) o->next_frame = 0;
    }
}

/*
 * Layer-shell stacks surfaces of one layer in the order they were mapped, so
 * a wallpaper daemon or shell that (re)starts after us lands on top. Mapping
 * a new surface puts us back; the mesh itself carries over.
 */
static void raise_buried(double now) {
    char names[16][HYPR_NAME_LEN];
    int count = hypr_buried_monitors(NAMESPACE, (int)getpid(), names, 16);
    for (int i = 0; i < count; i++) {
        for (output *o = outputs; o; o = o->next) {
            if (strcmp(names[i], o->name) != 0 || !o->surface) continue;
            if (now - o->raised_at > 60) o->raises = 0;
            if (o->raises >= 5) continue; /* something else insists on the top spot: let it have it */
            o->raises++;
            o->raised_at = now;
            output_drop_surface(o);
            output_create_surface(o);
        }
    }
}

/* ---------- settings reload ---------- */

static int watch_config(void) {
    char dir[sizeof config_path];
    snprintf(dir, sizeof dir, "%s", config_path);
    char *slash = strrchr(dir, '/');
    if (!slash) return -1;
    *slash = 0;
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) return -1;
    /* Watch the directory: editors replace the file rather than write into it. */
    if (inotify_add_watch(fd, dir, IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int config_touched(int fd) {
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    const char *base = strrchr(config_path, '/');
    base = base ? base + 1 : config_path;
    int touched = 0;
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        for (char *p = buf; p < buf + n;) {
            struct inotify_event *event = (struct inotify_event *)p;
            if (event->len && !strcmp(event->name, base)) touched = 1;
            p += sizeof *event + event->len;
        }
    }
    return touched;
}

static void reload(void) {
    mm_settings before = cfg.mm;
    load_settings();
    mesh_options options = current_options();
    for (output *o = outputs; o; o = o->next) {
        mesh_set_options(o->m, &options);
        o->text_shown[0] = 0;
        o->next_frame = 0;
        if (o->surface && ((o->subsurface != NULL) != use_canvas() || (o->window && o->has_alpha != wants_alpha()))) {
            output_drop_surface(o); /* a different kind of surface or buffer: outputs_sync maps it */
        } else if (o->surface && o->configured) {
            if (o->backdrop) wl_buffer_destroy(o->backdrop);
            o->backdrop = NULL;
            output_set_opaque(o);
        }
    }
    outputs_sync();
    if (strcmp(before.server, cfg.mm.server) || strcmp(before.token, cfg.mm.token) ||
        strcmp(before.source, cfg.mm.source)) {
        if (!mm_settings_live(&cfg.mm)) go_simulated();
        live_configure(&cfg.mm);
    }
}

/*
 * MESH_WALLPAPER_DEBUG=1 prints what each monitor is doing every five seconds.
 * "callback" is how long the compositor took to ask for the next frame: if
 * that is longer than a refresh, the compositor is what limits the frame rate.
 */
static void report(double seconds) {
    for (output *o = outputs; o; o = o->next) {
        char yielding[32] = "";
        if (o->backoff) snprintf(yielding, sizeof yielding, ", yielding 1/%d", o->backoff + 1);
        fprintf(stderr, "mesh-wallpaper: %s %dx%d %.1f fps (callback %.1f ms)%s%s%s%s, %d nodes, %d packets, %s\n",
                o->name, o->px_w, o->px_h, o->frames / seconds, o->frames ? o->waited / o->frames * 1000 : 0,
                yielding, o->surface ? "" : ", no surface", o->covered ? ", covered" : "",
                o->stalled ? ", not shown" : "",
                mesh_node_count(o->m), mesh_packet_count(o->m), mesh_is_live(o->m) ? "live" : "simulated");
        o->frames = 0;
        o->waited = 0;
    }
}

/* ---------- entry ---------- */

/* Off screen, for tests and screenshots: step the mesh for a while, then draw one frame into a PNG. */
static int run_render(void) {
    static const EGLint config_attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                                               EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                               EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    static const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    static const EGLint pbuffer_attributes[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    EGLint count = 0;
    egl_display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (egl_display == EGL_NO_DISPLAY || !eglInitialize(egl_display, NULL, NULL)) {
        egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (egl_display == EGL_NO_DISPLAY || !eglInitialize(egl_display, NULL, NULL)) {
            fprintf(stderr, "mesh-wallpaper: no EGL display to render with\n");
            return 1;
        }
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    if (!eglChooseConfig(egl_display, config_attributes, &egl_config, 1, &count) || count < 1) return 1;
    egl_context = eglCreateContext(egl_display, egl_config, EGL_NO_CONTEXT, context_attributes);
    EGLSurface pbuffer = eglCreatePbufferSurface(egl_display, egl_config, pbuffer_attributes);
    if (egl_context == EGL_NO_CONTEXT || !eglMakeCurrent(egl_display, pbuffer, pbuffer, egl_context) || !gl_setup()) {
        fprintf(stderr, "mesh-wallpaper: OpenGL ES 2 setup failed\n");
        return 1;
    }

    GLuint texture, framebuffer;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, render_w, render_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return 1;

    output o;
    memset(&o, 0, sizeof o);
    o.m = make_mesh(1);
    if (!o.m) return 1;
    o.width = o.px_w = o.mesh_w = render_w;
    o.height = o.px_h = o.mesh_h = render_h;
    o.configured = 1;
    mesh_resize(o.m, (float)render_w, (float)render_h);
    outputs = &o;

    double start = live_now(), last = start;
    while (live_now() - start < render_seconds) {
        struct timespec nap = {0, 1000000000L / 60};
        nanosleep(&nap, NULL);
        double now = live_now();
        live_apply(now, 1);
        mesh_step(o.m, (float)(now - last));
        last = now;
    }
    draw(&o);
    glFinish();

    unsigned char *rgba = malloc((size_t)render_w * (size_t)render_h * 4);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, render_w, render_h);
    if (!rgba || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) return 1;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, render_w, render_h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    unsigned char *pixels = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);
    for (int y = 0; y < render_h; y++) { /* GL rows go bottom up */
        const unsigned char *from = rgba + (size_t)(render_h - 1 - y) * (size_t)render_w * 4;
        uint32_t *to = (uint32_t *)(void *)(pixels + (size_t)y * (size_t)stride);
        for (int x = 0; x < render_w; x++, from += 4)
            to[x] = (uint32_t)from[3] << 24 | (uint32_t)from[0] << 16 | (uint32_t)from[1] << 8 | from[2];
    }
    cairo_surface_mark_dirty(surface);
    cairo_status_t status = cairo_surface_write_to_png(surface, render_path);
    cairo_surface_destroy(surface);
    free(rgba);
    printf("nodes %d, live %d\n", mesh_node_count(o.m), mesh_is_live(o.m));
    return status == CAIRO_STATUS_SUCCESS ? 0 : 1;
}

int main(int argc, char **argv) {
    saved_argc = argc;
    saved_argv = argv;
    load_settings();
    int live_fd = live_start();
    live_configure(&cfg.mm);
    if (render_path) return run_render();

    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "mesh-wallpaper: can't connect to a Wayland compositor\n");
        return 1;
    }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!compositor || !layer_shell) {
        fprintf(stderr, "mesh-wallpaper: this compositor has no wlr-layer-shell, which a wallpaper needs "
                        "(GNOME doesn't; Hyprland, Sway, river, niri, Wayfire, labwc, and KDE do)\n");
        return 1;
    }
    if (!egl_start()) {
        fprintf(stderr, "mesh-wallpaper: can't start OpenGL ES 2 on this display\n");
        return 1;
    }
    int hypr_fd = hypr_open_events();
    hyprland = hypr_fd >= 0;
    wl_display_roundtrip(display); /* the monitors describe themselves; outputs_sync runs as each finishes */
    outputs_sync();

    enum { FD_WAYLAND, FD_LIVE, FD_HYPR, FD_CONFIG, FD_COUNT };
    struct pollfd fds[FD_COUNT] = {{wl_display_get_fd(display), POLLIN, 0},
                                   {live_fd, POLLIN, 0},
                                   {hypr_fd, POLLIN, 0},
                                   {watch_config(), POLLIN, 0}};
    /* Times (CLOCK_MONOTONIC seconds) when a deferred check is due; negative when none is. */
    double covered_due = fds[FD_HYPR].fd >= 0 ? 0 : -1, buried_due = -1, reload_due = -1;
    double report_due = getenv("MESH_WALLPAPER_DEBUG") ? live_now() + 5 : -1;

    for (;;) {
        double now = live_now(), wake = -1;
#define WAKE_AT(t) do { double at_ = (t); if (wake < 0 || at_ < wake) wake = at_; } while (0)
        live_apply(now, fds[FD_LIVE].revents != 0);
        if (reload_due >= 0 && now >= reload_due) {
            reload_due = -1;
            reload();
            if (fds[FD_HYPR].fd >= 0) covered_due = now;
        }
        if (covered_due >= 0 && now >= covered_due) {
            covered_due = -1;
            refresh_covered();
        }
        if (buried_due >= 0 && now >= buried_due) {
            buried_due = -1;
            if (cfg.keep_on_top) raise_buried(now);
        }
        if (report_due >= 0 && now >= report_due) {
            report(5 + now - report_due);
            report_due = now + 5;
        }

        int showing = 0;
        for (output *o = outputs; o; o = o->next) {
            if (!o->configured || o->covered) continue;
            if (o->frame && o->stalled && now - o->frame_asked >= STALL_PROBE) {
                /* In case a callback was lost: one frame now and then finds out if we're back on screen. */
                wl_callback_destroy(o->frame);
                o->frame = NULL;
            }
            if (!o->frame) {
                if (now >= o->next_frame) output_render(o, now);
                else WAKE_AT(o->next_frame);
            }
            if (o->frame) {
                if (!o->stalled && now - o->frame_asked >= STALL_AFTER) o->stalled = 1;
                WAKE_AT(o->frame_asked + (o->stalled ? STALL_PROBE : STALL_AFTER));
            }
            if (output_visible(o)) showing = 1;
        }
        live_set_awake(showing);
        for (live_event *e = pending; e; e = e->next) WAKE_AT(e->due);
        if (reload_due >= 0) WAKE_AT(reload_due);
        if (covered_due >= 0) WAKE_AT(covered_due);
        if (buried_due >= 0) WAKE_AT(buried_due);
        if (report_due >= 0) WAKE_AT(report_due);
#undef WAKE_AT

        while (wl_display_prepare_read(display) != 0) wl_display_dispatch_pending(display);
        if (wl_display_flush(display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(display);
            break;
        }
        struct timespec timeout, *wait = NULL;
        if (wake >= 0) {
            double left = wake - live_now();
            if (left < 0) left = 0;
            timeout.tv_sec = (time_t)left;
            timeout.tv_nsec = (long)((left - (double)timeout.tv_sec) * 1e9);
            wait = &timeout;
        }
        int ready = ppoll(fds, FD_COUNT, wait, NULL);
        if (ready < 0) {
            wl_display_cancel_read(display);
            if (errno == EINTR) continue;
            break;
        }
        if (fds[FD_WAYLAND].revents & POLLIN) {
            if (wl_display_read_events(display) < 0) break;
        } else {
            wl_display_cancel_read(display);
        }
        if (wl_display_dispatch_pending(display) < 0) break;
        if (fds[FD_WAYLAND].revents & (POLLERR | POLLHUP)) break;

        now = live_now();
        if (fds[FD_HYPR].fd >= 0 && fds[FD_HYPR].revents) {
            unsigned what = 0;
            if (!hypr_read_events(fds[FD_HYPR].fd, NAMESPACE, &what)) {
                close(fds[FD_HYPR].fd);
                fds[FD_HYPR].fd = -1;
            }
            /* Events come in bursts: look once, shortly after the first. */
            if ((what & HYPR_WINDOWS) && covered_due < 0) covered_due = now + 0.05;
            if ((what & HYPR_LAYERS) && buried_due < 0) buried_due = now + 0.5;
        }
        if (fds[FD_CONFIG].fd >= 0 && fds[FD_CONFIG].revents && config_touched(fds[FD_CONFIG].fd) && reload_due < 0)
            reload_due = now + 0.15;
    }
    fprintf(stderr, "mesh-wallpaper: lost the Wayland connection\n");
    return 1;
}
