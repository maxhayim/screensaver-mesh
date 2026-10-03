/*
 * A small JSON reader, enough for MeshMonitor's API responses.
 * Parses into a tree owned by one allocation; free it with json_free.
 */
#ifndef MESH_JSON_H
#define MESH_JSON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT } json_type;

typedef struct json_value json_value;
struct json_value {
    json_type type;
    double number;      /* JSON_NUMBER, JSON_BOOL (0 or 1) */
    char *string;       /* JSON_STRING; for members of an object, also see key */
    char *key;          /* set when this value is an object member */
    json_value *child;  /* first element or member */
    json_value *next;   /* next sibling */
};

/* Returns NULL on malformed input. */
json_value *json_parse(const char *text, size_t length);
void json_free(json_value *root);

const json_value *json_get(const json_value *object, const char *key);
const char *json_get_string(const json_value *object, const char *key);
/* Returns `fallback` when the member is missing or not a number. */
double json_get_number(const json_value *object, const char *key, double fallback);

#ifdef __cplusplus
}
#endif

#endif
