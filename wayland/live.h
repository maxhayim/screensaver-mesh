/*
 * MeshMonitor polling for the Wayland host: one worker thread for every
 * output, with libcurl. It sends If-None-Match, so a poll with nothing new
 * is an empty 304, and it stops polling while no output is on screen.
 */
#ifndef MESH_LIVE_H
#define MESH_LIVE_H

#include "meshmonitor.h"

typedef struct live_event {
    struct live_event *next;
    enum { LIVE_NODES, LIVE_MESSAGE, LIVE_OFFLINE } type;
    double due;                 /* LIVE_MESSAGE: when to show it (CLOCK_MONOTONIC seconds) */
    int count;                  /* LIVE_NODES */
    char (*ids)[MM_ID_LEN];     /* LIVE_NODES, malloc'd */
    mm_message message;
} live_event;

/* Returns a file descriptor that becomes readable when events are waiting, or -1. */
int live_start(void);
/* Use these settings from now on. Starts the worker the first time they have a server and token. */
void live_configure(const mm_settings *s);
/* Pause (0) or resume (1) polling. Messages sent while paused are not replayed. */
void live_set_awake(int awake);
/* Take the waiting events, oldest first. The caller frees each event and its ids. */
live_event *live_take(void);

double live_now(void);

#endif
