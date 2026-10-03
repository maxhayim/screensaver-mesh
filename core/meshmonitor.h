/*
 * MeshMonitor REST API v1 helpers shared by the Windows and Linux savers:
 * settings and presets, URL building, and parsing the nodes and messages
 * responses. The platform does the HTTP and the threading.
 * (macOS has the same logic in Swift, on URLSession.)
 */
#ifndef MESH_MESHMONITOR_H
#define MESH_MESHMONITOR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MM_ID_LEN 32
#define MM_MAX_NODES 160
#define MM_MAX_MESSAGES 50
#define MM_NODES_EVERY 300 /* seconds */
#define MM_MESSAGES_EVERY 4
#define MM_OFFLINE_AFTER 8 /* failed polls in a row */

typedef struct {
    char server[256];
    char token[256];
    char source[128];
    char background[16], dots[16], lines[16], packets[16];
    int show_clock;
    int use_24_hour;
} mm_settings;

typedef struct {
    const char *name;
    const char *background, *dots, *lines, *packets;
} mm_preset;

extern const mm_preset mm_presets[];
extern const int mm_preset_count;

void mm_settings_default(mm_settings *s);
int mm_settings_live(const mm_settings *s);
/* Apply one "key=value" (or "key value") setting; unknown keys are ignored. */
void mm_settings_set(mm_settings *s, const char *key, const char *value);
/* Read a config file of key=value lines; # starts a comment. Returns 0 if unreadable. */
int mm_settings_load_file(mm_settings *s, const char *path);
/* Index of the preset matching the four colors, or -1 for custom. */
int mm_settings_preset(const mm_settings *s);
void mm_settings_apply_preset(mm_settings *s, int index);

/*
 * "{server}/api/v1/sources/{source}/{what}?..." where what is "nodes" or
 * "messages". A server without a scheme gets https://. Returns 0 if it doesn't fit.
 */
int mm_url(char *out, size_t size, const mm_settings *s, const char *what);

/* Node ids from a /nodes response, most recently heard first. Returns -1 on bad JSON. */
int mm_parse_nodes(const char *body, size_t length, char ids[][MM_ID_LEN], int max);

typedef struct {
    char from[MM_ID_LEN];
    char to[MM_ID_LEN];
} mm_message;

/* Remembers which messages were already seen. */
typedef struct mm_tracker mm_tracker;
mm_tracker *mm_tracker_create(void);
void mm_tracker_destroy(mm_tracker *t);

/*
 * New messages from a /messages response, oldest first. The first response
 * only primes the tracker: messages already there are not replayed.
 * Returns -1 on bad JSON.
 */
int mm_parse_messages(mm_tracker *t, const char *body, size_t length, mm_message *out, int max);

#ifdef __cplusplus
}
#endif

#endif
