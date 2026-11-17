#define _GNU_SOURCE

#include "live.h"

#ifdef MESH_CURL_COMPAT
#include "curl_compat.h"
#else
#include <curl/curl.h>
#endif

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#define OFFLINE_RETRY 30 /* seconds between tries while the server is unreachable */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake;
static mm_settings wanted;
static unsigned generation;
static int awake = 1, started;
static int notify_fd = -1;
static live_event *head, *tail;

double live_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void push_event(live_event *e) {
    pthread_mutex_lock(&lock);
    if (tail) tail->next = e; else head = e;
    tail = e;
    pthread_mutex_unlock(&lock);
    uint64_t one = 1;
    if (write(notify_fd, &one, sizeof one) < 0) { /* the counter is full: the reader is already due */ }
}

live_event *live_take(void) {
    uint64_t count;
    if (read(notify_fd, &count, sizeof count) < 0) { /* nothing waiting */ }
    pthread_mutex_lock(&lock);
    live_event *list = head;
    head = tail = NULL;
    pthread_mutex_unlock(&lock);
    return list;
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

#define ETAG_LEN 160

static size_t on_header(char *ptr, size_t size, size_t n, void *userdata) {
    size_t len = size * n;
    char *etag = userdata;
    if (len > 5 && strncasecmp(ptr, "etag:", 5) == 0) {
        const char *v = ptr + 5, *end = ptr + len;
        while (v < end && (*v == ' ' || *v == '\t')) v++;
        while (end > v && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ')) end--;
        size_t m = (size_t)(end - v);
        if (m < ETAG_LEN) {
            memcpy(etag, v, m);
            etag[m] = 0;
        }
    }
    return len;
}

/*
 * GET `url`. Returns the HTTP status (0 when the request failed). On a 200
 * the body is in *b and `etag` holds the response's ETag; a 304 means the
 * answer hasn't changed since the request that set `etag`.
 */
static long http_get(CURL *curl, const mm_settings *s, const char *url, char *etag, buffer *b) {
    char auth[400], match[ETAG_LEN + 32], fresh[ETAG_LEN] = "";
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", s->token);
    struct curl_slist *headers = curl_slist_append(NULL, auth);
    headers = curl_slist_append(headers, "Accept: application/json");
    if (etag[0]) {
        snprintf(match, sizeof match, "If-None-Match: %s", etag);
        headers = curl_slist_append(headers, match);
    }
    curl_easy_reset(curl); /* the connection stays open for the next poll */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, b);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, fresh);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); /* gzip, if the server offers it */
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "screensaver-mesh");
    long status = 0;
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    if (rc != CURLE_OK) return 0;
    if (status == 200) memcpy(etag, fresh, ETAG_LEN);
    return status;
}

static void push_nodes(const buffer *b, int *ok) {
    live_event *e = calloc(1, sizeof *e);
    char(*ids)[MM_ID_LEN] = calloc(MM_MAX_NODES, MM_ID_LEN);
    int n = e && ids ? mm_parse_nodes(b->data, b->len, ids, MM_MAX_NODES) : -1;
    if (n < 0) {
        free(e);
        free(ids);
        return;
    }
    *ok = 1;
    e->type = LIVE_NODES;
    e->count = n;
    e->ids = ids;
    push_event(e);
}

static void push_messages(mm_tracker *tracker, const buffer *b, double now, int *ok) {
    mm_message fresh[MM_MAX_MESSAGES];
    int n = mm_parse_messages(tracker, b->data, b->len, fresh, MM_MAX_MESSAGES);
    if (n >= 0) *ok = 1;
    for (int i = 0; i < n; i++) {
        live_event *e = calloc(1, sizeof *e);
        if (!e) break;
        e->type = LIVE_MESSAGE;
        e->due = now + i * 0.35;
        e->message = fresh[i];
        push_event(e);
    }
}

static void *poll_thread(void *unused) {
    (void)unused;
    CURL *curl = curl_easy_init();
    mm_tracker *tracker = NULL;
    mm_settings s;
    unsigned seen = 0;
    double next_nodes = 0, next_messages = 0;
    int failures = 0, offline = 0, slept = 0;
    char etag_nodes[ETAG_LEN] = "", etag_messages[ETAG_LEN] = "";
    char url[1024];

    while (curl) {
        pthread_mutex_lock(&lock);
        while (!awake || !mm_settings_live(&wanted)) {
            slept = 1;
            pthread_cond_wait(&wake, &lock);
        }
        int changed = seen != generation;
        seen = generation;
        s = wanted;
        pthread_mutex_unlock(&lock);

        if (changed) {
            etag_nodes[0] = 0;
            next_nodes = 0;
            failures = offline = 0;
        }
        if (changed || slept || !tracker) {
            /* Start over on messages: whatever was sent meanwhile is old news, not a burst to replay. */
            mm_tracker_destroy(tracker);
            tracker = mm_tracker_create();
            if (!tracker) break;
            etag_messages[0] = 0;
            next_messages = 0;
            slept = 0;
        }

        double now = live_now();
        for (int pass = 0; pass < 2; pass++) {
            int nodes = pass == 0;
            if (now < (nodes ? next_nodes : next_messages)) continue;
            if (!nodes && offline) continue; /* wait for the node list to come back first */
            if (nodes) next_nodes = now + (offline ? OFFLINE_RETRY : MM_NODES_EVERY);
            else next_messages = now + MM_MESSAGES_EVERY;
            if (!mm_url(url, sizeof url, &s, nodes ? "nodes" : "messages")) continue;

            buffer b = {NULL, 0};
            long status = http_get(curl, &s, url, nodes ? etag_nodes : etag_messages, &b);
            int ok = status == 304;
            if (status == 200 && b.data && nodes) push_nodes(&b, &ok);
            else if (status == 200 && b.data) push_messages(tracker, &b, now, &ok);
            free(b.data);

            if (ok) {
                failures = 0;
                if (nodes && offline) {
                    offline = 0;
                    slept = 1; /* prime the tracker again before showing messages */
                }
            } else if (++failures == MM_OFFLINE_AFTER) {
                offline = 1;
                etag_nodes[0] = 0; /* the next good answer must carry the node list */
                next_nodes = now + OFFLINE_RETRY;
                live_event *e = calloc(1, sizeof *e);
                if (e) {
                    e->type = LIVE_OFFLINE;
                    push_event(e);
                }
            }
        }

        double next = offline || next_nodes < next_messages ? next_nodes : next_messages;
        struct timespec until = {(time_t)next, (long)((next - (double)(time_t)next) * 1e9)};
        pthread_mutex_lock(&lock);
        if (awake && seen == generation && !slept) pthread_cond_timedwait(&wake, &lock, &until);
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

int live_start(void) {
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&wake, &attr);
    pthread_condattr_destroy(&attr);
    notify_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    return notify_fd;
}

void live_configure(const mm_settings *s) {
    pthread_mutex_lock(&lock);
    wanted = *s;
    generation++;
    if (!started && mm_settings_live(s)) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        pthread_t t;
        if (pthread_create(&t, NULL, poll_thread, NULL) == 0) {
            pthread_detach(t);
            started = 1;
        }
    }
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
}

void live_set_awake(int on) {
    pthread_mutex_lock(&lock);
    if (awake != on) {
        awake = on;
        pthread_cond_signal(&wake);
    }
    pthread_mutex_unlock(&lock);
}
