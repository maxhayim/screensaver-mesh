#include "meshmonitor.h"

#include "json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#endif

/* Same presets as macos/Settings.swift. */
const mm_preset mm_presets[] = {
    {"Default", "#0b0b0a", "#eeebe4", "#eeebe4", "#f06a2a"},
    {"Meshtastic", "#0a0f0b", "#e8f5ec", "#67ea94", "#67ea94"},
    {"Green terminal", "#000000", "#33ff66", "#33ff66", "#ccffcc"},
    {"Amber terminal", "#0a0700", "#ffb000", "#ffb000", "#fff1c4"},
    {"Paper", "#f4f1ea", "#1d1c1a", "#1d1c1a", "#e8591a"},
};
const int mm_preset_count = (int)(sizeof mm_presets / sizeof mm_presets[0]);

static void copy(char *dst, size_t size, const char *src) {
    if (!size) return;
    size_t n = src ? strlen(src) : 0;
    if (n >= size) n = size - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
}

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    size_t i = 0;
    while (s[i] && isspace((unsigned char)s[i])) i++;
    if (i) memmove(s, s + i, n - i + 1);
}

void mm_settings_apply_preset(mm_settings *s, int index) {
    if (index < 0 || index >= mm_preset_count) return;
    copy(s->background, sizeof s->background, mm_presets[index].background);
    copy(s->dots, sizeof s->dots, mm_presets[index].dots);
    copy(s->lines, sizeof s->lines, mm_presets[index].lines);
    copy(s->packets, sizeof s->packets, mm_presets[index].packets);
}

void mm_settings_default(mm_settings *s) {
    memset(s, 0, sizeof *s);
    copy(s->source, sizeof s->source, "default");
    mm_settings_apply_preset(s, 0);
    s->show_clock = 1;
}

int mm_settings_live(const mm_settings *s) { return s->server[0] && s->token[0]; }

static int truthy(const char *v) {
    return v && (strcmp(v, "1") == 0 || strcmp(v, "true") == 0 || strcmp(v, "yes") == 0 || strcmp(v, "on") == 0);
}

void mm_settings_set(mm_settings *s, const char *key, const char *value) {
    if (!key || !value) return;
    if (!strcmp(key, "server")) copy(s->server, sizeof s->server, value);
    else if (!strcmp(key, "token")) copy(s->token, sizeof s->token, value);
    else if (!strcmp(key, "source")) copy(s->source, sizeof s->source, *value ? value : "default");
    else if (!strcmp(key, "background")) copy(s->background, sizeof s->background, value);
    else if (!strcmp(key, "dots")) copy(s->dots, sizeof s->dots, value);
    else if (!strcmp(key, "lines")) copy(s->lines, sizeof s->lines, value);
    else if (!strcmp(key, "packets")) copy(s->packets, sizeof s->packets, value);
    else if (!strcmp(key, "clock")) s->show_clock = truthy(value);
    else if (!strcmp(key, "24hour")) s->use_24_hour = truthy(value);
    else if (!strcmp(key, "label")) {
        if (!strcmp(value, "user")) s->label = MM_LABEL_USER;
        else if (!strcmp(value, "custom")) s->label = MM_LABEL_CUSTOM;
        else s->label = MM_LABEL_MESH;
    } else if (!strcmp(key, "label_text")) copy(s->label_text, sizeof s->label_text, value);
    else if (!strcmp(key, "preset")) {
        for (int i = 0; i < mm_preset_count; i++) {
            const char *a = mm_presets[i].name, *b = value;
            while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) a++, b++;
            if (!*a && !*b) mm_settings_apply_preset(s, i);
        }
    }
}

void mm_label(char *out, size_t size, const mm_settings *s, const char *user_name, int live) {
    const char *base = "Mesh";
    if (s->label == MM_LABEL_USER && user_name && *user_name) base = user_name;
    else if (s->label == MM_LABEL_CUSTOM) base = s->label_text; /* empty is allowed: just the time */
    if (live) snprintf(out, size, *base ? "%s \xc2\xb7 live" : "live", base);
    else snprintf(out, size, "%s", base);
}

int mm_settings_load_file(mm_settings *s, const char *path) {
    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f) return 0;
    char line[600];
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        char *sep = strchr(line, '=');
        if (!sep) sep = strpbrk(line, " \t");
        if (!sep) continue;
        *sep = 0;
        char *value = sep + 1;
        trim(line);
        trim(value);
        mm_settings_set(s, line, value);
    }
    fclose(f);
    return 1;
}

int mm_settings_preset(const mm_settings *s) {
    for (int i = 0; i < mm_preset_count; i++) {
#ifdef _WIN32
#define same(a, b) (_stricmp(a, b) == 0)
#else
#define same(a, b) (strcasecmp(a, b) == 0)
#endif
        if (same(s->background, mm_presets[i].background) && same(s->dots, mm_presets[i].dots) &&
            same(s->lines, mm_presets[i].lines) && same(s->packets, mm_presets[i].packets))
            return i;
#undef same
    }
    return -1;
}

static void encode_segment(char *out, size_t size, const char *in) {
    size_t n = 0;
    for (; *in && n + 4 < size; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[n++] = (char)c;
        else n += (size_t)snprintf(out + n, size - n, "%%%02X", c);
    }
    out[n] = 0;
}

int mm_url(char *out, size_t size, const mm_settings *s, const char *what) {
    char server[300], source[400];
    copy(server, sizeof server, s->server);
    trim(server);
    size_t n = strlen(server);
    while (n && server[n - 1] == '/') server[--n] = 0;
    if (!n) return 0;
    encode_segment(source, sizeof source, s->source[0] ? s->source : "default");
    const char *scheme = strstr(server, "://") ? "" : "https://";
    const char *query = !strcmp(what, "nodes") ? "?active=true&sinceDays=7" : "?limit=50";
    int written = snprintf(out, size, "%s%s/api/v1/sources/%s/%s%s", scheme, server, source, what, query);
    return written > 0 && (size_t)written < size;
}

/* ---------- responses ---------- */

static const json_value *data_array(const json_value *root) {
    const json_value *data = json_get(root, "data");
    if (data && data->type == JSON_ARRAY) return data;
    return root && root->type == JSON_ARRAY ? root : NULL;
}

typedef struct {
    const char *id;
    double heard;
} heard_node;

static int by_heard_desc(const void *a, const void *b) {
    double x = ((const heard_node *)a)->heard, y = ((const heard_node *)b)->heard;
    return x < y ? 1 : (x > y ? -1 : 0);
}

int mm_parse_nodes(const char *body, size_t length, char ids[][MM_ID_LEN], int max) {
    json_value *root = json_parse(body, length);
    const json_value *data = data_array(root);
    if (!data) {
        json_free(root);
        return -1;
    }
    size_t total = 0;
    for (const json_value *n = data->child; n; n = n->next) total++;
    heard_node *list = calloc(total ? total : 1, sizeof *list);
    size_t count = 0;
    for (const json_value *n = data->child; n && list; n = n->next) {
        const char *id = json_get_string(n, "nodeId");
        if (!id || !*id) continue;
        list[count].id = id;
        list[count].heard = json_get_number(n, "lastHeard", 0);
        count++;
    }
    if (list) qsort(list, count, sizeof *list, by_heard_desc);
    int out = 0;
    for (size_t i = 0; i < count && out < max; i++) copy(ids[out++], MM_ID_LEN, list[i].id);
    free(list);
    json_free(root);
    return out;
}

#define SEEN_CAP 2048

struct mm_tracker {
    char keys[SEEN_CAP][64];
    int next; /* ring buffer: the oldest keys fall out */
    int count;
    int primed;
};

mm_tracker *mm_tracker_create(void) { return calloc(1, sizeof(mm_tracker)); }
void mm_tracker_destroy(mm_tracker *t) { free(t); }

static int seen(mm_tracker *t, const char *key) {
    for (int i = 0; i < t->count; i++)
        if (!strcmp(t->keys[i], key)) return 1;
    return 0;
}

static void remember(mm_tracker *t, const char *key) {
    copy(t->keys[t->next], sizeof t->keys[0], key);
    t->next = (t->next + 1) % SEEN_CAP;
    if (t->count < SEEN_CAP) t->count++;
}

typedef struct {
    double time;
    mm_message m;
} timed_message;

static int by_time_asc(const void *a, const void *b) {
    double x = ((const timed_message *)a)->time, y = ((const timed_message *)b)->time;
    return x < y ? -1 : (x > y ? 1 : 0);
}

int mm_parse_messages(mm_tracker *t, const char *body, size_t length, mm_message *out, int max) {
    json_value *root = json_parse(body, length);
    const json_value *data = data_array(root);
    if (!data) {
        json_free(root);
        return -1;
    }
    timed_message fresh[MM_MAX_MESSAGES];
    int count = 0;
    for (const json_value *m = data->child; m; m = m->next) {
        const char *from = json_get_string(m, "fromNodeId");
        if (!from) continue;
        char key[64];
        const json_value *id = json_get(m, "id");
        if (id && id->type == JSON_STRING) copy(key, sizeof key, id->string);
        else if (id && id->type == JSON_NUMBER) snprintf(key, sizeof key, "%.0f", id->number);
        else snprintf(key, sizeof key, "%s|%.0f", from, json_get_number(m, "timestamp", 0));
        if (seen(t, key)) continue;
        remember(t, key);
        if (count >= MM_MAX_MESSAGES) continue;
        timed_message *f = &fresh[count++];
        f->time = json_get_number(m, "createdAt", json_get_number(m, "timestamp", 0));
        copy(f->m.from, MM_ID_LEN, from);
        copy(f->m.to, MM_ID_LEN, json_get_string(m, "toNodeId"));
    }
    json_free(root);
    if (!t->primed) {
        t->primed = 1;
        return 0;
    }
    qsort(fresh, (size_t)count, sizeof fresh[0], by_time_asc);
    int n = 0;
    for (int i = 0; i < count && n < max; i++) out[n++] = fresh[i].m;
    return n;
}
