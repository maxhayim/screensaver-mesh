/* JSON and MeshMonitor parsing tests:
   cc -std=c99 -Icore core/json.c core/meshmonitor.c tests/test_meshmonitor.c && ./a.out */
#include "json.h"
#include "meshmonitor.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);       \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

static int parse_ok(const char *s) {
    json_value *v = json_parse(s, strlen(s));
    json_free(v);
    return v != NULL;
}

int main(void) {
    /* JSON */
    CHECK(parse_ok("{}"));
    CHECK(parse_ok("[]"));
    CHECK(parse_ok(" {\"a\": [1, -2.5e3, true, false, null, \"x\\n\\u00e9\\ud83d\\ude00\"]} "));
    CHECK(!parse_ok("{"));
    CHECK(!parse_ok("{\"a\" 1}"));
    CHECK(!parse_ok("[1,]"));
    CHECK(!parse_ok("[1] x"));
    CHECK(!parse_ok("\"unterminated"));
    CHECK(!parse_ok(""));
    const char *doc = "{\"s\":\"h\\u00e9\",\"n\":42,\"o\":{\"k\":\"v\"}}";
    json_value *v = json_parse(doc, strlen(doc));
    CHECK(v && strcmp(json_get_string(v, "s"), "h\xc3\xa9") == 0);
    CHECK(json_get_number(v, "n", 0) == 42);
    CHECK(json_get_number(v, "missing", 7) == 7);
    CHECK(strcmp(json_get_string(json_get(v, "o"), "k"), "v") == 0);
    json_free(v);

    /* Settings */
    mm_settings s;
    mm_settings_default(&s);
    CHECK(!mm_settings_live(&s) && s.show_clock && !strcmp(s.source, "default"));
    CHECK(mm_settings_preset(&s) == 0);
    mm_settings_set(&s, "preset", "meshtastic");
    CHECK(mm_settings_preset(&s) == 1);
    mm_settings_set(&s, "dots", "#123456");
    CHECK(mm_settings_preset(&s) == -1);
    mm_settings_set(&s, "24hour", "true");
    mm_settings_set(&s, "clock", "0");
    CHECK(s.use_24_hour && !s.show_clock);

    /* The label under the clock */
    char label[200];
    mm_settings l;
    mm_settings_default(&l);
    mm_label(label, sizeof label, &l, "Max", 0);
    CHECK(!strcmp(label, "Mesh"));
    mm_label(label, sizeof label, &l, "Max", 1);
    CHECK(!strcmp(label, "Mesh \xc2\xb7 live"));
    mm_settings_set(&l, "label", "user");
    mm_label(label, sizeof label, &l, "Max Hayim", 1);
    CHECK(!strcmp(label, "Max Hayim \xc2\xb7 live"));
    mm_label(label, sizeof label, &l, NULL, 0);
    CHECK(!strcmp(label, "Mesh"));
    mm_settings_set(&l, "label", "custom");
    mm_settings_set(&l, "label_text", "KO4XYZ base");
    mm_label(label, sizeof label, &l, "Max", 0);
    CHECK(!strcmp(label, "KO4XYZ base"));
    mm_settings_set(&l, "label_text", "");
    mm_label(label, sizeof label, &l, "Max", 0);
    CHECK(!strcmp(label, ""));

    /* URLs */
    char url[512];
    mm_settings_set(&s, "server", "mesh.example.com:8080/");
    mm_settings_set(&s, "token", "mm_v1_x");
    CHECK(mm_settings_live(&s));
    CHECK(mm_url(url, sizeof url, &s, "nodes") &&
          !strcmp(url, "https://mesh.example.com:8080/api/v1/sources/default/nodes?active=true&sinceDays=7"));
    mm_settings_set(&s, "server", "http://10.0.0.5:8080");
    mm_settings_set(&s, "source", "doral base");
    CHECK(mm_url(url, sizeof url, &s, "messages") &&
          !strcmp(url, "http://10.0.0.5:8080/api/v1/sources/doral%20base/messages?limit=50"));

    /* Nodes, most recently heard first */
    const char *nodes = "{\"success\":true,\"count\":3,\"data\":["
                        "{\"nodeId\":\"!old\",\"lastHeard\":100},"
                        "{\"nodeId\":\"!new\",\"lastHeard\":300},"
                        "{\"shortName\":\"no id\"},"
                        "{\"nodeId\":\"!mid\",\"lastHeard\":200}]}";
    char ids[MM_MAX_NODES][MM_ID_LEN];
    CHECK(mm_parse_nodes(nodes, strlen(nodes), ids, MM_MAX_NODES) == 3);
    CHECK(!strcmp(ids[0], "!new") && !strcmp(ids[1], "!mid") && !strcmp(ids[2], "!old"));
    CHECK(mm_parse_nodes("<html>", 6, ids, MM_MAX_NODES) == -1);

    /* Messages: the first poll primes, later ones report only new, oldest first */
    mm_tracker *t = mm_tracker_create();
    mm_message out[MM_MAX_MESSAGES];
    const char *first = "{\"data\":[{\"id\":\"1\",\"fromNodeId\":\"!a\",\"toNodeId\":\"!b\",\"createdAt\":10}]}";
    CHECK(mm_parse_messages(t, first, strlen(first), out, MM_MAX_MESSAGES) == 0);
    const char *second = "{\"data\":["
                         "{\"id\":3,\"fromNodeId\":\"!c\",\"toNodeId\":\"!ffffffff\",\"createdAt\":30},"
                         "{\"id\":\"2\",\"fromNodeId\":\"!b\",\"toNodeId\":\"!a\",\"createdAt\":20},"
                         "{\"id\":\"1\",\"fromNodeId\":\"!a\",\"toNodeId\":\"!b\",\"createdAt\":10}]}";
    CHECK(mm_parse_messages(t, second, strlen(second), out, MM_MAX_MESSAGES) == 2);
    CHECK(!strcmp(out[0].from, "!b") && !strcmp(out[1].from, "!c") && !strcmp(out[1].to, "!ffffffff"));
    CHECK(mm_parse_messages(t, second, strlen(second), out, MM_MAX_MESSAGES) == 0);
    CHECK(mm_parse_messages(t, "nope", 4, out, MM_MAX_MESSAGES) == -1);
    mm_tracker_destroy(t);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all meshmonitor tests passed\n");
    return 0;
}
