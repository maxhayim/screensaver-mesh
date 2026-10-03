#include "json.h"

#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 64

typedef struct {
    const char *p, *end;
    json_value *nodes; /* arena of values */
    size_t node_count, node_cap;
    char *strings;     /* arena of decoded strings */
    size_t string_len, string_cap;
} parser;

/* Values and strings live in two arenas sized up front from the input, so no
   pointer ever moves while parsing. */
static json_value *new_value(parser *ps, json_type type) {
    if (ps->node_count >= ps->node_cap) return NULL;
    json_value *v = &ps->nodes[ps->node_count++];
    memset(v, 0, sizeof *v);
    v->type = type;
    return v;
}

static void skip_ws(parser *ps) {
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) ps->p++;
}

static int hex4(const char *s, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

static void put_utf8(parser *ps, unsigned cp) {
    char *o = ps->strings + ps->string_len;
    if (cp < 0x80) { o[0] = (char)cp; ps->string_len += 1; }
    else if (cp < 0x800) { o[0] = (char)(0xc0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3f)); ps->string_len += 2; }
    else if (cp < 0x10000) {
        o[0] = (char)(0xe0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        o[2] = (char)(0x80 | (cp & 0x3f)); ps->string_len += 3;
    } else {
        o[0] = (char)(0xf0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
        o[2] = (char)(0x80 | ((cp >> 6) & 0x3f)); o[3] = (char)(0x80 | (cp & 0x3f)); ps->string_len += 4;
    }
}

/* Decodes a string starting after the opening quote. */
static char *parse_string(parser *ps) {
    char *start = ps->strings + ps->string_len;
    while (ps->p < ps->end && *ps->p != '"') {
        char c = *ps->p++;
        if ((unsigned char)c < 0x20) return NULL;
        if (c != '\\') {
            ps->strings[ps->string_len++] = c;
            continue;
        }
        if (ps->p >= ps->end) return NULL;
        char e = *ps->p++;
        switch (e) {
        case '"': case '\\': case '/': ps->strings[ps->string_len++] = e; break;
        case 'b': ps->strings[ps->string_len++] = '\b'; break;
        case 'f': ps->strings[ps->string_len++] = '\f'; break;
        case 'n': ps->strings[ps->string_len++] = '\n'; break;
        case 'r': ps->strings[ps->string_len++] = '\r'; break;
        case 't': ps->strings[ps->string_len++] = '\t'; break;
        case 'u': {
            unsigned cp;
            if (ps->end - ps->p < 4 || !hex4(ps->p, &cp)) return NULL;
            ps->p += 4;
            if (cp >= 0xd800 && cp < 0xdc00 && ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                unsigned lo;
                if (hex4(ps->p + 2, &lo) && lo >= 0xdc00 && lo < 0xe000) {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    ps->p += 6;
                }
            }
            put_utf8(ps, cp);
            break;
        }
        default: return NULL;
        }
    }
    if (ps->p >= ps->end) return NULL;
    ps->p++; /* closing quote */
    ps->strings[ps->string_len++] = 0;
    return start;
}

static json_value *parse_value(parser *ps, int depth);

static json_value *parse_container(parser *ps, int depth, int object) {
    json_value *v = new_value(ps, object ? JSON_OBJECT : JSON_ARRAY);
    if (!v) return NULL;
    char close = object ? '}' : ']';
    json_value *last = NULL;
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == close) { ps->p++; return v; }
    for (;;) {
        char *key = NULL;
        skip_ws(ps);
        if (object) {
            if (ps->p >= ps->end || *ps->p != '"') return NULL;
            ps->p++;
            if (!(key = parse_string(ps))) return NULL;
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') return NULL;
            ps->p++;
        }
        json_value *item = parse_value(ps, depth + 1);
        if (!item) return NULL;
        item->key = key;
        if (last) last->next = item; else v->child = item;
        last = item;
        skip_ws(ps);
        if (ps->p >= ps->end) return NULL;
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == close) { ps->p++; return v; }
        return NULL;
    }
}

static int match(parser *ps, const char *word) {
    size_t n = strlen(word);
    if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, word, n) != 0) return 0;
    ps->p += n;
    return 1;
}

static json_value *parse_value(parser *ps, int depth) {
    if (depth > MAX_DEPTH) return NULL;
    skip_ws(ps);
    if (ps->p >= ps->end) return NULL;
    char c = *ps->p;
    if (c == '{' || c == '[') { ps->p++; return parse_container(ps, depth, c == '{'); }
    if (c == '"') {
        ps->p++;
        json_value *v = new_value(ps, JSON_STRING);
        if (!v || !(v->string = parse_string(ps))) return NULL;
        return v;
    }
    if (match(ps, "true")) { json_value *v = new_value(ps, JSON_BOOL); if (v) v->number = 1; return v; }
    if (match(ps, "false")) return new_value(ps, JSON_BOOL);
    if (match(ps, "null")) return new_value(ps, JSON_NULL);
    if (c == '-' || (c >= '0' && c <= '9')) {
        char buf[64];
        size_t n = 0;
        while (ps->p < ps->end && n < sizeof buf - 1 && strchr("+-0123456789.eE", *ps->p)) buf[n++] = *ps->p++;
        buf[n] = 0;
        char *stop;
        double d = strtod(buf, &stop);
        if (stop == buf) return NULL;
        json_value *v = new_value(ps, JSON_NUMBER);
        if (v) v->number = d;
        return v;
    }
    return NULL;
}

json_value *json_parse(const char *text, size_t length) {
    if (!text) return NULL;
    /* Every value needs at least one input byte, and decoded strings never grow
       (a \uXXXX escape is 6 bytes in, at most 4 out), so input length bounds both. */
    size_t node_cap = length + 1;
    size_t string_cap = length + 1;
    size_t bytes = sizeof(json_value) * node_cap + string_cap;
    char *block = malloc(bytes);
    if (!block) return NULL;
    parser ps = {text, text + length, (json_value *)block, 0, node_cap,
                 block + sizeof(json_value) * node_cap, 0, string_cap};
    json_value *root = parse_value(&ps, 0);
    skip_ws(&ps);
    if (!root || ps.p != ps.end || root != (json_value *)block) {
        free(block);
        return NULL;
    }
    return root;
}

void json_free(json_value *root) { free(root); }

const json_value *json_get(const json_value *object, const char *key) {
    if (!object || object->type != JSON_OBJECT) return NULL;
    for (const json_value *m = object->child; m; m = m->next)
        if (m->key && strcmp(m->key, key) == 0) return m;
    return NULL;
}

const char *json_get_string(const json_value *object, const char *key) {
    const json_value *v = json_get(object, key);
    return v && v->type == JSON_STRING ? v->string : NULL;
}

double json_get_number(const json_value *object, const char *key, double fallback) {
    const json_value *v = json_get(object, key);
    return v && v->type == JSON_NUMBER ? v->number : fallback;
}
