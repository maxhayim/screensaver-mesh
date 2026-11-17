/*
 * Optional Hyprland integration over its IPC sockets. Layer-shell can't
 * tell a wallpaper that a full-screen window is covering it, or that
 * another wallpaper was mapped on top of it; Hyprland's event socket can.
 * Under any other compositor hypr_open_events returns -1 and none of this runs.
 */
#ifndef MESH_HYPRLAND_H
#define MESH_HYPRLAND_H

#define HYPR_NAME_LEN 64
#define HYPR_WINDOWS 1u /* full-screen state or the visible workspaces may have changed */
#define HYPR_LAYERS 2u  /* another client mapped a layer surface */

/* The event socket's file descriptor, or -1 when not running under Hyprland. */
int hypr_open_events(void);
/* Read what's waiting and OR HYPR_* flags into *what. Returns 0 when the socket has closed. */
int hypr_read_events(int fd, const char *own_namespace, unsigned *what);

/* Names of the monitors whose visible workspace has a full-screen window. Returns how many, or -1. */
int hypr_fullscreen_monitors(char names[][HYPR_NAME_LEN], int max);
/* Names of the monitors where another background layer surface is stacked above ours. */
int hypr_buried_monitors(const char *own_namespace, int pid, char names[][HYPR_NAME_LEN], int max);

#endif
