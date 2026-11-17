#define _GNU_SOURCE

#include "hyprland.h"

#include "json.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static int connect_to(const char *runtime, const char *instance, const char *name) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    int n = snprintf(addr.sun_path, sizeof addr.sun_path, "%s/hypr/%s/%s", runtime, instance, name);
    if (n < 0 || (size_t)n >= sizeof addr.sun_path) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/*
 * The instance that owns our Wayland display. A service started by systemd
 * can inherit a stale HYPRLAND_INSTANCE_SIGNATURE (or none), so if that one
 * doesn't answer, look for the instance whose lock file names $WAYLAND_DISPLAY.
 */
static int hypr_socket(const char *name) {
    static char instance[256];
    const char *runtime = getenv("XDG_RUNTIME_DIR"), *env = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    const char *wayland = getenv("WAYLAND_DISPLAY");
    if (!runtime) return -1;
    if (!instance[0] && env) snprintf(instance, sizeof instance, "%s", env);
    int fd = instance[0] ? connect_to(runtime, instance, name) : -1;
    if (fd >= 0 || !wayland) return fd;

    char path[512];
    snprintf(path, sizeof path, "%s/hypr", runtime);
    DIR *dir = opendir(path);
    if (!dir) return -1;
    struct dirent *entry;
    while (fd < 0 && (entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;
        char pid[64], display[128] = "";
        snprintf(path, sizeof path, "%s/hypr/%s/hyprland.lock", runtime, entry->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(pid, sizeof pid, f) && fgets(display, sizeof display, f)) display[strcspn(display, "\r\n")] = 0;
        fclose(f);
        if (strcmp(display, wayland) != 0) continue;
        fd = connect_to(runtime, entry->d_name, name);
        if (fd >= 0) snprintf(instance, sizeof instance, "%s", entry->d_name);
    }
    closedir(dir);
    return fd;
}

int hypr_open_events(void) { return hypr_socket(".socket2.sock"); }

/* The events after which the wallpaper may have been covered or uncovered. */
static const char *const window_events[] = {
    "fullscreen",   "workspace",     "workspacev2",   "moveworkspace", "moveworkspacev2", "activespecial",
    "movewindow",   "movewindowv2",  "closewindow",   "monitoradded",  "monitorremoved",  "configreloaded",
    NULL,
};

static void classify(const char *line, const char *own_namespace, unsigned *what) {
    const char *sep = strstr(line, ">>");
    if (!sep) return;
    size_t len = (size_t)(sep - line);
    const char *data = sep + 2;
    if (len == 9 && !strncmp(line, "openlayer", 9)) {
        if (strcmp(data, own_namespace) != 0) *what |= HYPR_LAYERS;
        return;
    }
    for (int i = 0; window_events[i]; i++)
        if (strlen(window_events[i]) == len && !strncmp(line, window_events[i], len)) *what |= HYPR_WINDOWS;
}

int hypr_read_events(int fd, const char *own_namespace, unsigned *what) {
    static char buf[4096];
    static size_t have;
    for (;;) {
        ssize_t n = recv(fd, buf + have, sizeof buf - have, MSG_DONTWAIT);
        if (n == 0) return 0;
        if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
        have += (size_t)n;
        char *start = buf, *end = buf + have, *newline;
        while ((newline = memchr(start, '\n', (size_t)(end - start)))) {
            *newline = 0;
            classify(start, own_namespace, what);
            start = newline + 1;
        }
        have = (size_t)(end - start);
        memmove(buf, start, have);
        if (have == sizeof buf) have = 0; /* an overlong line: drop it */
    }
}

/* Send one command and return the malloc'd reply, or NULL. Never waits long: this runs on the drawing thread. */
static char *request(const char *command, size_t *length) {
    int fd = hypr_socket(".socket.sock");
    if (fd < 0) return NULL;
    struct timeval timeout = {0, 300 * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    char *reply = NULL;
    size_t len = 0, cap = 0;
    if (send(fd, command, strlen(command), MSG_NOSIGNAL) == (ssize_t)strlen(command)) {
        for (;;) {
            if (len + 4096 > cap) {
                cap = cap ? cap * 2 : 16384;
                char *grown = cap <= 8 * 1024 * 1024 ? realloc(reply, cap) : NULL;
                if (!grown) {
                    free(reply);
                    reply = NULL;
                    break;
                }
                reply = grown;
            }
            ssize_t n = recv(fd, reply + len, cap - len - 1, 0);
            if (n < 0) {
                free(reply);
                reply = NULL;
                break;
            }
            if (n == 0) break;
            len += (size_t)n;
        }
    }
    close(fd);
    if (reply) reply[len] = 0;
    *length = len;
    return reply;
}

static json_value *request_json(const char *command) {
    size_t length = 0;
    char *reply = request(command, &length);
    json_value *root = reply ? json_parse(reply, length) : NULL;
    free(reply);
    return root;
}

static int is_true(const json_value *v) { return v && v->type == JSON_BOOL && v->number != 0; }

static void add_name(char names[][HYPR_NAME_LEN], int max, int *count, const char *name) {
    if (*count < max && name) snprintf(names[(*count)++], HYPR_NAME_LEN, "%s", name);
}

int hypr_fullscreen_monitors(char names[][HYPR_NAME_LEN], int max) {
    json_value *monitors = request_json("j/monitors"), *workspaces = request_json("j/workspaces"), *clients = NULL;
    int count = -1;
    if (monitors && workspaces && monitors->type == JSON_ARRAY && workspaces->type == JSON_ARRAY) {
        count = 0;
        for (const json_value *m = monitors->child; m; m = m->next) {
            double shown = json_get_number(json_get(m, "activeWorkspace"), "id", -1e9);
            int has_fullscreen = 0;
            for (const json_value *w = workspaces->child; w; w = w->next)
                if (json_get_number(w, "id", -2e9) == shown && is_true(json_get(w, "hasfullscreen"))) has_fullscreen = 1;
            if (!has_fullscreen) continue;
            /* hasfullscreen is also set for a maximized window, which leaves the wallpaper in view around it. */
            if (!clients) clients = request_json("j/clients");
            if (!clients || clients->type != JSON_ARRAY) break;
            for (const json_value *c = clients->child; c; c = c->next) {
                if (json_get_number(json_get(c, "workspace"), "id", -3e9) != shown) continue;
                if ((int)json_get_number(c, "fullscreen", 0) & 2) {
                    add_name(names, max, &count, json_get_string(m, "name"));
                    break;
                }
            }
        }
    }
    json_free(monitors);
    json_free(workspaces);
    json_free(clients);
    return count;
}

int hypr_buried_monitors(const char *own_namespace, int pid, char names[][HYPR_NAME_LEN], int max) {
    json_value *layers = request_json("j/layers");
    if (!layers || layers->type != JSON_OBJECT) {
        json_free(layers);
        return -1;
    }
    int count = 0;
    for (const json_value *monitor = layers->child; monitor; monitor = monitor->next) {
        const json_value *background = json_get(json_get(monitor, "levels"), "0");
        if (!background || background->type != JSON_ARRAY) continue;
        /* Within a level, Hyprland lists (and draws) surfaces in the order they were mapped: the last is on top. */
        int ours = 0, above = 0;
        for (const json_value *layer = background->child; layer; layer = layer->next) {
            const char *ns = json_get_string(layer, "namespace");
            if (ns && !strcmp(ns, own_namespace) && (int)json_get_number(layer, "pid", -1) == pid) ours = 1, above = 0;
            else if (ours) above = 1;
        }
        if (ours && above) add_name(names, max, &count, monitor->key);
    }
    json_free(layers);
    return count;
}
