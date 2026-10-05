/*
 * Mesh for Windows: a .scr (an .exe renamed) drawing the core with GDI+,
 * polling MeshMonitor with WinHTTP on a worker thread.
 *
 *   Mesh.scr /s            run full screen on every monitor
 *   Mesh.scr /p <HWND>     draw in the Screen Saver Settings preview
 *   Mesh.scr /c[:HWND]     settings dialog (also with no arguments)
 *   Mesh.scr /x <seconds> <out.bmp> [width height] [preview]
 *                          test: render off screen and save a bitmap
 */
#define SECURITY_WIN32
#include <windows.h>
#include <security.h>
#include <commctrl.h>
#include <commdlg.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <winhttp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "meshmonitor.h"
#include "mesh.h"
#include "resource.h"

#define REG_KEY_W L"Software\\maxhayim\\screensaver-mesh"
#define FRAME_TIMER 1
#define WM_APP_STATUS (WM_APP + 1)

/* ---------- settings in the registry ---------- */

static void utf8_from_wide(char *out, int size, const wchar_t *in) {
    if (!WideCharToMultiByte(CP_UTF8, 0, in, -1, out, size, NULL, NULL)) out[0] = 0;
}

static void wide_from_utf8(wchar_t *out, int size, const char *in) {
    if (!MultiByteToWideChar(CP_UTF8, 0, in, -1, out, size)) out[0] = 0;
}

static const char *label_names[] = {"mesh", "user", "custom"};

/* Strings are stored as UTF-16 (REG_SZ) so custom text can be in any language. */
static void load_settings(mm_settings *s) {
    mm_settings_default(s);
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY_W, 0, KEY_READ, &key) != ERROR_SUCCESS) return;
    static const char *strings[] = {"server", "token", "source", "background", "dots", "lines", "packets",
                                    "label", "label_text"};
    for (size_t i = 0; i < sizeof strings / sizeof strings[0]; i++) {
        wchar_t name[32], value[300];
        char utf8[600];
        DWORD size = sizeof value - sizeof(wchar_t), type = 0;
        wide_from_utf8(name, 32, strings[i]);
        if (RegQueryValueExW(key, name, NULL, &type, (BYTE *)value, &size) == ERROR_SUCCESS && type == REG_SZ) {
            value[size / sizeof(wchar_t)] = 0;
            utf8_from_wide(utf8, sizeof utf8, value);
            mm_settings_set(s, strings[i], utf8);
        }
    }
    DWORD v, size = sizeof v, type;
    if (RegQueryValueExW(key, L"clock", NULL, &type, (BYTE *)&v, &size) == ERROR_SUCCESS && type == REG_DWORD)
        s->show_clock = v != 0;
    size = sizeof v;
    if (RegQueryValueExW(key, L"24hour", NULL, &type, (BYTE *)&v, &size) == ERROR_SUCCESS && type == REG_DWORD)
        s->use_24_hour = v != 0;
    RegCloseKey(key);
}

static void save_settings(const mm_settings *s) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY_W, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL) != ERROR_SUCCESS) return;
    struct { const char *name; const char *value; } strings[] = {
        {"server", s->server}, {"token", s->token}, {"source", s->source}, {"background", s->background},
        {"dots", s->dots}, {"lines", s->lines}, {"packets", s->packets},
        {"label", label_names[s->label >= 0 && s->label <= 2 ? s->label : 0]}, {"label_text", s->label_text},
    };
    for (size_t i = 0; i < sizeof strings / sizeof strings[0]; i++) {
        wchar_t name[32], value[300];
        wide_from_utf8(name, 32, strings[i].name);
        wide_from_utf8(value, 300, strings[i].value);
        RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)value, (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
    }
    DWORD clock = (DWORD)s->show_clock, h24 = (DWORD)s->use_24_hour;
    RegSetValueExW(key, L"clock", 0, REG_DWORD, (const BYTE *)&clock, sizeof clock);
    RegSetValueExW(key, L"24hour", 0, REG_DWORD, (const BYTE *)&h24, sizeof h24);
    RegCloseKey(key);
}

/* The name shown for "Your name": the display name, or the sign-in name. */
static void user_name(char *out, int size) {
    wchar_t name[256];
    ULONG n = 256;
    if (GetUserNameExW(NameDisplay, name, &n) && name[0]) {
        utf8_from_wide(out, size, name);
        return;
    }
    DWORD m = 256;
    if (GetUserNameW(name, &m)) utf8_from_wide(out, size, name);
    else out[0] = 0;
}

/* ---------- HTTP ---------- */

/* GET with a bearer token. Returns a malloc'd body (caller frees) or NULL; *status gets the HTTP status. */
static char *http_get(const char *url_utf8, const char *token, DWORD *status, size_t *length) {
    *status = 0;
    *length = 0;
    wchar_t url[1024], auth[400];
    if (!MultiByteToWideChar(CP_UTF8, 0, url_utf8, -1, url, 1024)) return NULL;
    char auth_utf8[400];
    snprintf(auth_utf8, sizeof auth_utf8, "Authorization: Bearer %s\r\nAccept: application/json", token);
    if (!MultiByteToWideChar(CP_UTF8, 0, auth_utf8, -1, auth, 400)) return NULL;

    URL_COMPONENTS parts = {0};
    wchar_t host[256], path[1024];
    parts.dwStructSize = sizeof parts;
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 1024;
    parts.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url, 0, 0, &parts)) return NULL;
    wchar_t full_path[1200];
    _snwprintf(full_path, 1200, L"%ls%ls", path, parts.lpszExtraInfo ? parts.lpszExtraInfo : L"");
    full_path[1199] = 0;

    char *body = NULL;
    HINTERNET session = WinHttpOpen(L"screensaver-mesh", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) session = WinHttpOpen(L"screensaver-mesh", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                        WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = NULL, request = NULL;
    if (!session) goto done;
    WinHttpSetTimeouts(session, 10000, 10000, 10000, 10000);
    connect = WinHttpConnect(session, host, parts.nPort, 0);
    if (!connect) goto done;
    request = WinHttpOpenRequest(connect, L"GET", full_path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!request) goto done;
    if (!WinHttpSendRequest(request, auth, (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) goto done;
    if (!WinHttpReceiveResponse(request, NULL)) goto done;
    DWORD code = 0, code_size = sizeof code;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &code, &code_size, WINHTTP_NO_HEADER_INDEX);
    *status = code;

    size_t cap = 65536, len = 0;
    body = malloc(cap + 1);
    for (;;) {
        DWORD avail = 0, got = 0;
        if (!body || !WinHttpQueryDataAvailable(request, &avail) || !avail) break;
        if (len + avail > 8 * 1024 * 1024) break; /* far more than any real response */
        if (len + avail > cap) {
            while (len + avail > cap) cap *= 2;
            char *grown = realloc(body, cap + 1);
            if (!grown) break;
            body = grown;
        }
        if (!WinHttpReadData(request, body + len, avail, &got) || !got) break;
        len += got;
    }
    if (body) body[len] = 0;
    *length = len;

done:
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return body;
}

/* ---------- live polling ---------- */

typedef struct live_event {
    struct live_event *next;
    enum { EV_NODES, EV_MESSAGE, EV_OFFLINE } type;
    double due;          /* messages in a burst are spaced out */
    int count;
    char (*ids)[MM_ID_LEN];
    mm_message message;
} live_event;

static struct {
    mm_settings settings;
    HANDLE thread;
    volatile LONG stop;
    CRITICAL_SECTION lock;
    live_event *head, *tail;
} live;

static double now_seconds(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}

static void push_event(live_event *e) {
    EnterCriticalSection(&live.lock);
    if (live.tail) live.tail->next = e; else live.head = e;
    live.tail = e;
    LeaveCriticalSection(&live.lock);
}

static DWORD WINAPI poll_thread(LPVOID unused) {
    (void)unused;
    mm_tracker *tracker = mm_tracker_create();
    double next_nodes = 0, next_messages = 0;
    int failures = 0;
    char url[1024];
    while (!live.stop && tracker) {
        double now = now_seconds();
        for (int pass = 0; pass < 2; pass++) {
            int nodes = pass == 0;
            if (now < (nodes ? next_nodes : next_messages)) continue;
            if (nodes) next_nodes = now + MM_NODES_EVERY; else next_messages = now + MM_MESSAGES_EVERY;
            if (!mm_url(url, sizeof url, &live.settings, nodes ? "nodes" : "messages")) continue;
            DWORD status;
            size_t length;
            char *body = http_get(url, live.settings.token, &status, &length);
            int ok = 0;
            if (body && status >= 200 && status < 300) {
                if (nodes) {
                    live_event *e = calloc(1, sizeof *e);
                    char(*ids)[MM_ID_LEN] = calloc(MM_MAX_NODES, MM_ID_LEN);
                    int n = e && ids ? mm_parse_nodes(body, length, ids, MM_MAX_NODES) : -1;
                    if (n >= 0) {
                        ok = 1;
                        e->type = EV_NODES;
                        e->count = n;
                        e->ids = ids;
                        push_event(e);
                    } else {
                        free(e);
                        free(ids);
                    }
                } else {
                    mm_message fresh[MM_MAX_MESSAGES];
                    int n = mm_parse_messages(tracker, body, length, fresh, MM_MAX_MESSAGES);
                    if (n >= 0) ok = 1;
                    for (int i = 0; i < n; i++) {
                        live_event *e = calloc(1, sizeof *e);
                        if (!e) break;
                        e->type = EV_MESSAGE;
                        e->due = now + i * 0.35;
                        e->message = fresh[i];
                        push_event(e);
                    }
                }
            }
            free(body);
            failures = ok ? 0 : failures + 1;
            if (failures == MM_OFFLINE_AFTER) {
                live_event *e = calloc(1, sizeof *e);
                if (e) { e->type = EV_OFFLINE; push_event(e); }
            }
        }
        for (int i = 0; i < 10 && !live.stop; i++) Sleep(100);
    }
    mm_tracker_destroy(tracker);
    return 0;
}

static void live_start(const mm_settings *s) {
    InitializeCriticalSection(&live.lock);
    live.settings = *s;
    if (mm_settings_live(s)) live.thread = CreateThread(NULL, 0, poll_thread, NULL, 0, NULL);
}

/* Applies due events to the mesh. Runs on the UI thread every frame. */
static void live_drain(mesh *m) {
    double now = now_seconds();
    EnterCriticalSection(&live.lock);
    live_event **link = &live.head, *prev = NULL;
    while (*link) {
        live_event *e = *link;
        if (e->type == EV_MESSAGE && e->due > now) {
            prev = e;
            link = &e->next;
            continue;
        }
        *link = e->next;
        if (live.tail == e) live.tail = prev;
        if (e->type == EV_NODES && e->count > 0) {
            const char *ptrs[MM_MAX_NODES];
            for (int i = 0; i < e->count; i++) ptrs[i] = e->ids[i];
            mesh_set_live(m, 1);
            mesh_set_nodes(m, ptrs, e->count);
        } else if (e->type == EV_MESSAGE && mesh_is_live(m)) {
            mesh_message(m, e->message.from, e->message.to[0] ? e->message.to : NULL);
        } else if (e->type == EV_OFFLINE) {
            mesh_set_live(m, 0);
        }
        free(e->ids);
        free(e);
    }
    LeaveCriticalSection(&live.lock);
}

/* ---------- drawing ---------- */

typedef struct {
    GpGraphics *g;
    GpPen *pen;
    GpSolidFill *brush;
} gdip_ctx;

static ARGB argb(mesh_color c) {
    int a = (int)(c.a * 255.0f + 0.5f), r = (int)(c.r * 255.0f + 0.5f);
    int g = (int)(c.g * 255.0f + 0.5f), b = (int)(c.b * 255.0f + 0.5f);
    return ((ARGB)a << 24) | ((ARGB)r << 16) | ((ARGB)g << 8) | (ARGB)b;
}

static void draw_line(void *ctx, float x0, float y0, float x1, float y1, float width, mesh_color c) {
    gdip_ctx *d = ctx;
    GdipSetPenColor(d->pen, argb(c));
    GdipSetPenWidth(d->pen, width);
    GdipDrawLine(d->g, d->pen, x0, y0, x1, y1);
}

static void draw_circle(void *ctx, float x, float y, float r, int filled, float width, mesh_color c) {
    gdip_ctx *d = ctx;
    if (filled) {
        GdipSetSolidFillColor(d->brush, argb(c));
        GdipFillEllipse(d->g, (GpBrush *)d->brush, x - r, y - r, r * 2, r * 2);
    } else {
        GdipSetPenColor(d->pen, argb(c));
        GdipSetPenWidth(d->pen, width);
        GdipDrawEllipse(d->g, d->pen, x - r, y - r, r * 2, r * 2);
    }
}

typedef struct {
    mesh *m;
    mm_settings settings;
    int preview;
    float scale;         /* pixels per point */
    RECT clock_area;     /* the primary monitor, in canvas pixels */
    HDC dc;              /* back buffer */
    HBITMAP bitmap;
    void *bits;
    int width, height;   /* pixels */
    char user[256];      /* for the "Your name" label */
} saver;

static mesh_color parse_or(const char *hex, const char *fallback) {
    mesh_color c;
    if (!mesh_parse_color(hex, &c)) mesh_parse_color(fallback, &c);
    return c;
}

static void saver_resize(saver *s, int width, int height) {
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    if (s->bitmap && width == s->width && height == s->height) return;
    if (s->bitmap) DeleteObject(s->bitmap);
    if (!s->dc) s->dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height; /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    s->bitmap = CreateDIBSection(s->dc, &bi, DIB_RGB_COLORS, &s->bits, NULL, 0);
    SelectObject(s->dc, s->bitmap);
    s->width = width;
    s->height = height;
    mesh_resize(s->m, (float)width / s->scale, (float)height / s->scale);
}

static void saver_init(saver *s, int preview, float scale) {
    memset(s, 0, sizeof *s);
    load_settings(&s->settings);
    user_name(s->user, sizeof s->user);
    s->preview = preview;
    s->scale = scale > 0 ? scale : 1;
    s->m = mesh_create(GetTickCount() ^ (GetCurrentProcessId() << 16));
    mesh_options o = mesh_default_options();
    const mm_preset *base = &mm_presets[0];
    o.background = parse_or(s->settings.background, base->background);
    o.node = parse_or(s->settings.dots, base->dots);
    o.link = parse_or(s->settings.lines, base->lines);
    o.packet = parse_or(s->settings.packets, base->packets);
    BOOL animate = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0);
    o.reduced_motion = !animate;
    o.compact = preview;
    mesh_set_options(s->m, &o);
}

static void draw_clock(saver *s, GpGraphics *g) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t time[32];
    if (s->settings.use_24_hour) _snwprintf(time, 32, L"%02d:%02d", t.wHour, t.wMinute);
    else _snwprintf(time, 32, L"%d:%02d %ls", t.wHour % 12 ? t.wHour % 12 : 12, t.wMinute, t.wHour < 12 ? L"AM" : L"PM");
    time[31] = 0;
    char label_utf8[400];
    wchar_t label[400];
    mm_label(label_utf8, sizeof label_utf8, &s->settings, s->user, mesh_is_live(s->m));
    wide_from_utf8(label, 400, label_utf8);

    mesh_color ink = parse_or(s->settings.dots, mm_presets[0].dots);
    float k = s->preview ? (float)s->height / s->scale / 900.0f : 1.0f;
    if (k < 0.25f) k = 0.25f;
    float margin = 32 * k;

    GpFontFamily *family = NULL;
    if (GdipCreateFontFamilyFromName(L"Segoe UI", NULL, &family) != Ok) return;
    GpFont *big = NULL, *small = NULL;
    GdipCreateFont(family, 48 * k, FontStyleBold, UnitPixel, &big);
    GdipCreateFont(family, 12 * k, FontStyleRegular, UnitPixel, &small);
    GpSolidFill *brush = NULL;
    GdipSetTextRenderingHint(g, TextRenderingHintAntiAlias);

    float left = (float)s->clock_area.left / s->scale + margin;
    float bottom = (float)s->clock_area.bottom / s->scale - margin;
    RectF layout = {0, 0, 2000, 400}, box_label = {0}, box_time = {0};
    GdipMeasureString(g, label, -1, small, &layout, NULL, &box_label, NULL, NULL);
    GdipMeasureString(g, time, -1, big, &layout, NULL, &box_time, NULL, NULL);

    mesh_color c = ink;
    c.a *= 0.5f;
    GdipCreateSolidFill(argb(c), &brush);
    RectF at_label = {left, bottom - box_label.Height, 2000, box_label.Height};
    GdipDrawString(g, label, -1, small, &at_label, NULL, (GpBrush *)brush);
    c = ink;
    c.a *= 0.8f;
    GdipSetSolidFillColor(brush, argb(c));
    RectF at_time = {left - 4 * k, at_label.Y - box_time.Height + 8 * k, 2000, box_time.Height};
    GdipDrawString(g, time, -1, big, &at_time, NULL, (GpBrush *)brush);

    GdipDeleteBrush((GpBrush *)brush);
    GdipDeleteFont(big);
    GdipDeleteFont(small);
    GdipDeleteFontFamily(family);
}

static void saver_render(saver *s) {
    GpGraphics *g = NULL;
    if (GdipCreateFromHDC(s->dc, &g) != Ok) return;
    GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
    GdipSetPixelOffsetMode(g, PixelOffsetModeHalf);
    GdipGraphicsClear(g, argb(parse_or(s->settings.background, mm_presets[0].background)) | 0xff000000u);
    GdipScaleWorldTransform(g, s->scale, s->scale, MatrixOrderPrepend);

    gdip_ctx ctx = {g, NULL, NULL};
    GdipCreatePen1(0xffffffff, 1, UnitPixel, &ctx.pen);
    GdipSetPenLineCap197819(ctx.pen, LineCapRound, LineCapRound, DashCapRound);
    GdipCreateSolidFill(0xffffffff, &ctx.brush);
    mesh_renderer r = {&ctx, draw_line, draw_circle};
    mesh_render(s->m, &r);
    GdipDeletePen(ctx.pen);
    GdipDeleteBrush((GpBrush *)ctx.brush);

    if (s->settings.show_clock) draw_clock(s, g);
    GdipDeleteGraphics(g);
}

/* ---------- windows ---------- */

static saver app;
static int full_screen;
static POINT first_mouse;
static int have_mouse;
static double started, last_frame;

static void quit_saver(HWND hwnd) {
    if (full_screen) DestroyWindow(hwnd);
}

static LRESULT CALLBACK saver_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, FRAME_TIMER, 15, NULL);
        started = last_frame = now_seconds();
        return 0;
    case WM_SIZE:
        saver_resize(&app, LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_TIMER: {
        double now = now_seconds();
        live_drain(app.m);
        mesh_step(app.m, (float)(now - last_frame));
        last_frame = now;
        saver_render(&app);
        HDC dc = GetDC(hwnd);
        BitBlt(dc, 0, 0, app.width, app.height, app.dc, 0, 0, SRCCOPY);
        ReleaseDC(hwnd, dc);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (app.dc) BitBlt(dc, 0, 0, app.width, app.height, app.dc, 0, 0, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR:
        if (full_screen) { SetCursor(NULL); return TRUE; }
        break;
    case WM_MOUSEMOVE:
        if (full_screen) {
            POINT p = {(short)LOWORD(lp), (short)HIWORD(lp)};
            if (now_seconds() - started < 0.4) return 0; /* ignore the jitter right after starting */
            if (!have_mouse) { first_mouse = p; have_mouse = 1; return 0; }
            if (abs(p.x - first_mouse.x) + abs(p.y - first_mouse.y) > 8) quit_saver(hwnd);
        }
        return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
    case WM_MOUSEWHEEL:
        quit_saver(hwnd);
        return 0;
    case WM_ACTIVATEAPP:
        if (!wp) quit_saver(hwnd);
        return 0;
    case WM_SYSCOMMAND:
        if (full_screen && (wp == SC_SCREENSAVE || wp == SC_CLOSE)) return 0;
        break;
    case WM_DESTROY:
        KillTimer(hwnd, FRAME_TIMER);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static BOOL CALLBACK find_primary(HMONITOR mon, HDC dc, LPRECT r, LPARAM data) {
    (void)dc;
    MONITORINFO mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(mon, &mi);
    if (mi.dwFlags & MONITORINFOF_PRIMARY) *(RECT *)data = *r;
    return TRUE;
}

static float system_scale(void) {
    HDC dc = GetDC(NULL);
    int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);
    return dpi > 0 ? (float)dpi / 96.0f : 1.0f;
}

static int run_saver(HINSTANCE inst, HWND parent) {
    full_screen = parent == NULL;
    saver_init(&app, !full_screen, full_screen ? system_scale() : 1.0f);
    live_start(&app.settings);

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = saver_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"MeshSaver";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    HWND hwnd;
    if (full_screen) {
        int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int w = GetSystemMetrics(SM_CXVIRTUALSCREEN), h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        RECT primary = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        EnumDisplayMonitors(NULL, NULL, find_primary, (LPARAM)&primary);
        OffsetRect(&primary, -x, -y);
        app.clock_area = primary;
        hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"Mesh", WS_POPUP | WS_VISIBLE, x, y, w, h, NULL, NULL,
                               inst, NULL);
    } else {
        RECT r;
        GetClientRect(parent, &r);
        app.clock_area = r;
        hwnd = CreateWindowExW(0, wc.lpszClassName, L"Mesh", WS_CHILD | WS_VISIBLE, 0, 0, r.right, r.bottom, parent,
                               NULL, inst, NULL);
    }
    if (!hwnd) return 1;
    if (full_screen) SetForegroundWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    live.stop = 1;
    /* Don't wait for a slow request; the process is about to end anyway. */
    return 0;
}

/* Test mode: render off screen for a while and save a 32-bit BMP. */
static int run_test(double seconds, const wchar_t *out, int width, int height, int preview) {
    saver_init(&app, preview, 1.0f);
    app.clock_area.right = width;
    app.clock_area.bottom = height;
    saver_resize(&app, width, height);
    live_start(&app.settings);
    double start = now_seconds(), last = start;
    while (now_seconds() - start < seconds) {
        Sleep(16);
        double now = now_seconds();
        live_drain(app.m);
        mesh_step(app.m, (float)(now - last));
        last = now;
    }
    saver_render(&app);
    live.stop = 1;

    FILE *f = _wfopen(out, L"wb");
    if (!f) return 1;
    BITMAPFILEHEADER fh = {0};
    BITMAPINFOHEADER ih = {0};
    DWORD image = (DWORD)width * (DWORD)height * 4;
    fh.bfType = 0x4d42;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + image;
    ih.biSize = sizeof ih;
    ih.biWidth = width;
    ih.biHeight = -height;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&ih, sizeof ih, 1, f);
    fwrite(app.bits, image, 1, f);
    fclose(f);

    /* A GUI program has no console, so the summary goes next to the bitmap. */
    wchar_t summary_path[MAX_PATH];
    _snwprintf(summary_path, MAX_PATH, L"%ls.txt", out);
    summary_path[MAX_PATH - 1] = 0;
    FILE *summary = _wfopen(summary_path, L"w");
    if (summary) {
        fprintf(summary, "nodes %d, live %d\n", mesh_node_count(app.m), mesh_is_live(app.m));
        fclose(summary);
    }
    return 0;
}

/* ---------- settings dialog ---------- */

static mm_settings dialog_settings;
static HWND dialog;

static COLORREF colorref(const char *hex) {
    mesh_color c = parse_or(hex, "#ffffff");
    return RGB((int)(c.r * 255 + 0.5f), (int)(c.g * 255 + 0.5f), (int)(c.b * 255 + 0.5f));
}

static char *color_field(int id) {
    switch (id) {
    case IDC_BACKGROUND: return dialog_settings.background;
    case IDC_DOTS: return dialog_settings.dots;
    case IDC_LINES: return dialog_settings.lines;
    default: return dialog_settings.packets;
    }
}

static void sync_preset(HWND dlg) {
    int i = mm_settings_preset(&dialog_settings);
    SendDlgItemMessageW(dlg, IDC_PRESET, CB_SETCURSEL, i >= 0 ? (WPARAM)i : (WPARAM)mm_preset_count, 0);
    for (int id = IDC_BACKGROUND; id <= IDC_PACKETS; id++) InvalidateRect(GetDlgItem(dlg, id), NULL, TRUE);
}

static void read_fields(HWND dlg) {
    char text[300];
    GetDlgItemTextA(dlg, IDC_SERVER, text, sizeof text);
    mm_settings_set(&dialog_settings, "server", text);
    GetDlgItemTextA(dlg, IDC_TOKEN, text, sizeof text);
    mm_settings_set(&dialog_settings, "token", text);
    GetDlgItemTextA(dlg, IDC_SOURCE, text, sizeof text);
    mm_settings_set(&dialog_settings, "source", text);
    int label = (int)SendDlgItemMessageW(dlg, IDC_LABEL, CB_GETCURSEL, 0, 0);
    dialog_settings.label = label >= 0 && label <= 2 ? label : MM_LABEL_MESH;
    wchar_t wide[61];
    GetDlgItemTextW(dlg, IDC_LABEL_TEXT, wide, 61);
    utf8_from_wide(dialog_settings.label_text, sizeof dialog_settings.label_text, wide);
    dialog_settings.show_clock = IsDlgButtonChecked(dlg, IDC_CLOCK) == BST_CHECKED;
    dialog_settings.use_24_hour = IsDlgButtonChecked(dlg, IDC_24HOUR) == BST_CHECKED;
}

static DWORD WINAPI test_thread(LPVOID param) {
    mm_settings *s = param;
    char url[1024], *message = malloc(200);
    if (!message) return 0;
    DWORD status = 0;
    size_t length = 0;
    char *body = mm_url(url, sizeof url, s, "nodes") ? http_get(url, s->token, &status, &length) : NULL;
    if (!body && !status) snprintf(message, 200, "Couldn't reach the server");
    else if (status == 401) snprintf(message, 200, "Server answered 401 (check the API token)");
    else if (status == 404) snprintf(message, 200, "Server answered 404 (check the source)");
    else if (status < 200 || status >= 300) snprintf(message, 200, "Server answered %lu", (unsigned long)status);
    else {
        char(*ids)[MM_ID_LEN] = calloc(MM_MAX_NODES, MM_ID_LEN);
        int n = ids ? mm_parse_nodes(body, length, ids, MM_MAX_NODES) : -1;
        free(ids);
        if (n < 0) snprintf(message, 200, "Not a MeshMonitor API response");
        else snprintf(message, 200, "Connected: %d active nodes", n);
    }
    free(body);
    free(s);
    if (!PostMessageW(dialog, WM_APP_STATUS, 0, (LPARAM)message)) free(message);
    return 0;
}

static INT_PTR CALLBACK settings_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        dialog = dlg;
        load_settings(&dialog_settings);
        SetDlgItemTextA(dlg, IDC_SERVER, dialog_settings.server);
        SetDlgItemTextA(dlg, IDC_TOKEN, dialog_settings.token);
        SetDlgItemTextA(dlg, IDC_SOURCE, strcmp(dialog_settings.source, "default") ? dialog_settings.source : "");
        SendDlgItemMessageW(dlg, IDC_SERVER, EM_SETCUEBANNER, TRUE, (LPARAM)L"https://meshmonitor.example.com");
        SendDlgItemMessageW(dlg, IDC_TOKEN, EM_SETCUEBANNER, TRUE, (LPARAM)L"mm_v1_...");
        SendDlgItemMessageW(dlg, IDC_SOURCE, EM_SETCUEBANNER, TRUE, (LPARAM)L"default");
        for (int i = 0; i < mm_preset_count; i++)
            SendDlgItemMessageA(dlg, IDC_PRESET, CB_ADDSTRING, 0, (LPARAM)mm_presets[i].name);
        SendDlgItemMessageA(dlg, IDC_PRESET, CB_ADDSTRING, 0, (LPARAM) "Custom");
        static const wchar_t *label_titles[] = {L"Mesh", L"Your name", L"Custom text"};
        for (int i = 0; i < 3; i++) SendDlgItemMessageW(dlg, IDC_LABEL, CB_ADDSTRING, 0, (LPARAM)label_titles[i]);
        SendDlgItemMessageW(dlg, IDC_LABEL, CB_SETCURSEL, (WPARAM)dialog_settings.label, 0);
        SendDlgItemMessageW(dlg, IDC_LABEL_TEXT, EM_LIMITTEXT, 60, 0);
        SendDlgItemMessageW(dlg, IDC_LABEL_TEXT, EM_SETCUEBANNER, TRUE, (LPARAM)L"Text under the clock");
        wchar_t label_text[130];
        wide_from_utf8(label_text, 130, dialog_settings.label_text);
        SetDlgItemTextW(dlg, IDC_LABEL_TEXT, label_text);
        EnableWindow(GetDlgItem(dlg, IDC_LABEL_TEXT), dialog_settings.label == MM_LABEL_CUSTOM);
        CheckDlgButton(dlg, IDC_CLOCK, dialog_settings.show_clock ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_24HOUR, dialog_settings.use_24_hour ? BST_CHECKED : BST_UNCHECKED);
        sync_preset(dlg);
        return TRUE;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lp;
        if (di->CtlID < IDC_BACKGROUND || di->CtlID > IDC_PACKETS) break;
        DrawFrameControl(di->hDC, &di->rcItem, DFC_BUTTON,
                         DFCS_BUTTONPUSH | ((di->itemState & ODS_SELECTED) ? DFCS_PUSHED : 0));
        RECT swatch = di->rcItem;
        InflateRect(&swatch, -4, -4);
        HBRUSH b = CreateSolidBrush(colorref(color_field((int)di->CtlID)));
        FillRect(di->hDC, &swatch, b);
        DeleteObject(b);
        FrameRect(di->hDC, &swatch, (HBRUSH)GetStockObject(GRAY_BRUSH));
        return TRUE;
    }
    case WM_APP_STATUS: {
        char *message = (char *)lp;
        SetDlgItemTextA(dlg, IDC_STATUS, message);
        free(message);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_PRESET:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int i = (int)SendDlgItemMessageW(dlg, IDC_PRESET, CB_GETCURSEL, 0, 0);
                if (i >= 0 && i < mm_preset_count) mm_settings_apply_preset(&dialog_settings, i);
                sync_preset(dlg);
            }
            return TRUE;
        case IDC_LABEL:
            if (HIWORD(wp) == CBN_SELCHANGE)
                EnableWindow(GetDlgItem(dlg, IDC_LABEL_TEXT),
                             SendDlgItemMessageW(dlg, IDC_LABEL, CB_GETCURSEL, 0, 0) == MM_LABEL_CUSTOM);
            return TRUE;
        case IDC_BACKGROUND: case IDC_DOTS: case IDC_LINES: case IDC_PACKETS: {
            static COLORREF custom[16];
            CHOOSECOLORW cc = {0};
            cc.lStructSize = sizeof cc;
            cc.hwndOwner = dlg;
            cc.lpCustColors = custom;
            cc.rgbResult = colorref(color_field(LOWORD(wp)));
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;
            if (ChooseColorW(&cc))
                snprintf(color_field(LOWORD(wp)), 16, "#%02x%02x%02x", GetRValue(cc.rgbResult),
                         GetGValue(cc.rgbResult), GetBValue(cc.rgbResult));
            sync_preset(dlg);
            return TRUE;
        }
        case IDC_TEST: {
            read_fields(dlg);
            if (!dialog_settings.server[0]) {
                SetDlgItemTextA(dlg, IDC_STATUS, "Enter a server address");
                return TRUE;
            }
            if (!dialog_settings.token[0]) {
                SetDlgItemTextA(dlg, IDC_STATUS, "Enter an API token");
                return TRUE;
            }
            SetDlgItemTextA(dlg, IDC_STATUS, "Connecting...");
            mm_settings *copy = malloc(sizeof *copy);
            if (copy) {
                *copy = dialog_settings;
                HANDLE t = CreateThread(NULL, 0, test_thread, copy, 0, NULL);
                if (t) CloseHandle(t); else free(copy);
            }
            return TRUE;
        }
        case IDOK:
            read_fields(dlg);
            save_settings(&dialog_settings);
            EndDialog(dlg, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* ---------- entry ---------- */

static HWND parse_hwnd(const wchar_t *s) {
    return s ? (HWND)(ULONG_PTR)_wcstoui64(s, NULL, 10) : NULL;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    (void)prev, (void)cmd, (void)show;
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    GdiplusStartupInput gin = {1, NULL, FALSE, FALSE};
    ULONG_PTR gdip;
    GdiplusStartup(&gdip, &gin, NULL);

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t mode = L'c';
    const wchar_t *arg = NULL;
    if (argc > 1 && (argv[1][0] == L'/' || argv[1][0] == L'-')) {
        mode = (wchar_t)towlower(argv[1][1]);
        if (argv[1][2] == L':') arg = argv[1] + 3;
        else if (argc > 2) arg = argv[2];
    }

    int result = 0;
    if (mode == L's') {
        result = run_saver(inst, NULL);
    } else if (mode == L'p' && parse_hwnd(arg)) {
        result = run_saver(inst, parse_hwnd(arg));
    } else if (mode == L'x' && argc >= 4) {
        int w = argc > 5 ? _wtoi(argv[4]) : 1280, h = argc > 5 ? _wtoi(argv[5]) : 800;
        int preview = argc > 6 && !wcscmp(argv[6], L"preview");
        result = run_test(_wtof(argv[2]), argv[3], w > 0 ? w : 1280, h > 0 ? h : 800, preview);
    } else {
        HWND parent = mode == L'c' ? parse_hwnd(arg) : NULL;
        DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_SETTINGS), parent ? parent : GetForegroundWindow(), settings_proc, 0);
    }
    LocalFree(argv);
    GdiplusShutdown(gdip);
    ExitProcess((UINT)result);
}
