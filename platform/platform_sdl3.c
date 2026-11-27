#include "platform.h"
#include <SDL3/SDL.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SDL_Window   *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture  *g_tex;
static int g_tex_w, g_tex_h;
static void update_title(void);

void platform_boost_thread(void)
{
    if (getenv("BEEMU_NO_BOOST")) return;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    // [[NSProcessInfo processInfo] beginActivityWithOptions:reason:] through the ObjC runtime
    // (this file is C): user initiated (system idle sleep still allowed) + latency critical.
    static id activity;
    if (activity) return;
    Class pi_cls = objc_getClass("NSProcessInfo"), str_cls = objc_getClass("NSString");
    if (!pi_cls || !str_cls) return;
    id pi = ((id (*)(id, SEL))objc_msgSend)((id)pi_cls, sel_registerName("processInfo"));
    id reason = ((id (*)(id, SEL, const char *))objc_msgSend)((id)str_cls, sel_registerName("stringWithUTF8String:"),
                                                              "emulated machine running in real time");
    uint64_t opts = (0x00FFFFFFull & ~(1ull << 20))      // NSActivityUserInitiatedAllowingIdleSystemSleep
                  | 0xFF00000000ull;                     // NSActivityLatencyCritical
    activity = ((id (*)(id, SEL, uint64_t, id))objc_msgSend)(pi, sel_registerName("beginActivityWithOptions:reason:"),
                                                             opts, reason);
    if (activity) ((id (*)(id, SEL))objc_msgSend)(activity, sel_registerName("retain"));
}

bool platform_init(const char *title, int width, int height)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    g_win = SDL_CreateWindow(title, width, height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!g_win) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
    g_ren = SDL_CreateRenderer(g_win, NULL);
    if (!g_ren) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetRenderVSync(g_ren, 0);     // main.c paces frames itself; a vsync wait would stall the guest
    update_title();
    return true;
}

static bool g_window_dirty = true;     // the window needs a present even if the frame didn't change
static uint64_t g_last_present;

static bool ensure_texture(int w, int h)
{
    if (g_tex && g_tex_w == w && g_tex_h == h) return true;
    if (g_tex) SDL_DestroyTexture(g_tex);
    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_XRGB8888,
                              SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!g_tex) return false;
    SDL_SetTextureScaleMode(g_tex, SDL_SCALEMODE_NEAREST);
    g_tex_w = w; g_tex_h = h;
    // Keep aspect ratio, letterbox as needed
    SDL_SetRenderLogicalPresentation(g_ren, w, h, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    return false;                     // new texture: the caller must upload everything
}

bool platform_present_rows(const uint32_t *xrgb, int width, int height, int pitch_bytes, int y0, int y1)
{
    if (!ensure_texture(width, height)) {
        if (!g_tex) return false;
        y0 = 0; y1 = height;
    }
    if (y0 < 0) y0 = 0;
    if (y1 > height) y1 = height;
    bool changed = y0 < y1;
    if (changed) {
        SDL_Rect r = { 0, y0, width, y1 - y0 };
        SDL_UpdateTexture(g_tex, &r, (const uint8_t *)xrgb + (size_t)y0 * (size_t)pitch_bytes, pitch_bytes);
    }
    uint64_t now = SDL_GetTicksNS();
    // Present on changes, when the window asked for it, and every half second regardless.
    if (!changed && !g_window_dirty && now - g_last_present < 500000000ull) return false;
    SDL_RenderClear(g_ren);
    SDL_RenderTexture(g_ren, g_tex, NULL, NULL);
    SDL_RenderPresent(g_ren);
    g_window_dirty = false;
    g_last_present = now;
    return true;
}

void platform_present_frame(const uint32_t *xrgb, int width, int height, int pitch_bytes)
{
    g_window_dirty = true;
    platform_present_rows(xrgb, width, height, pitch_bytes, 0, height);
}

// ---- input ----
// Keyboard: SDL scancodes are USB HID usages, which the core takes directly. On the Mac,
// Command maps to the BeOS Command key (PC Alt) and Option to the BeOS Option key
// (PC Windows key), so Cmd+C/V/W/Q work the way BeOS users expect.
// Mouse: click in the window to capture it (relative mode); Ctrl+Option releases it.
// Losing focus releases the mouse and lifts every key and button the guest thinks is held.

#define QLEN 64
static plat_event g_q[QLEN];
static int g_qhead, g_qcount;
static bool g_captured;
static bool g_held[SDL_SCANCODE_COUNT];
static uint32_t g_held_buttons;          // bit n = SDL button n
static float g_fx, g_fy;                 // fractional mouse motion carried over

static void push(plat_event e)
{
    if (g_qcount == QLEN) return;
    g_q[(g_qhead + g_qcount++) % QLEN] = e;
}

static int map_key(int sc)
{
    switch (sc) {
    case SDL_SCANCODE_LGUI: return SDL_SCANCODE_LALT;    // Cmd    -> BeOS Command
    case SDL_SCANCODE_RGUI: return SDL_SCANCODE_RALT;
    case SDL_SCANCODE_LALT: return SDL_SCANCODE_LGUI;    // Option -> BeOS Option
    case SDL_SCANCODE_RALT: return SDL_SCANCODE_RGUI;
    default: return sc;
    }
}

static void update_title(void)
{
    SDL_SetWindowTitle(g_win, g_captured ? "beemu - press Ctrl+Option to release the mouse"
                                         : "beemu - click to capture the mouse");
}

static void set_capture(bool on)
{
    if (g_captured == on) return;
    g_captured = on;
    SDL_SetWindowRelativeMouseMode(g_win, on);
    g_fx = g_fy = 0;
    if (!on) {
        for (int b = 0; b < 32; b++)
            if (g_held_buttons & (1u << b))
                push((plat_event){ .type = PLAT_EVENT_MOUSE_BUTTON, .button = b, .down = false });
        g_held_buttons = 0;
    }
    update_title();
}

static void release_keys(void)
{
    for (int sc = 0; sc < SDL_SCANCODE_COUNT; sc++)
        if (g_held[sc]) {
            g_held[sc] = false;
            push((plat_event){ .type = PLAT_EVENT_KEY, .scancode = sc, .down = false });
        }
}

static void translate(const SDL_Event *e)
{
    if (e->type >= SDL_EVENT_WINDOW_FIRST && e->type <= SDL_EVENT_WINDOW_LAST) g_window_dirty = true;
    if (e->type >= SDL_EVENT_DISPLAY_FIRST && e->type <= SDL_EVENT_DISPLAY_LAST) g_window_dirty = true;
    switch (e->type) {
    case SDL_EVENT_QUIT:
        push((plat_event){ .type = PLAT_EVENT_QUIT });
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        set_capture(false);
        release_keys();
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        int sc = map_key((int)e->key.scancode);
        if (sc <= 0 || sc >= SDL_SCANCODE_COUNT) break;
        if (e->key.down && g_captured && (e->key.mod & SDL_KMOD_CTRL) && (e->key.mod & SDL_KMOD_ALT)
            && (e->key.scancode == SDL_SCANCODE_LALT || e->key.scancode == SDL_SCANCODE_RALT ||
                e->key.scancode == SDL_SCANCODE_LCTRL || e->key.scancode == SDL_SCANCODE_RCTRL))
            set_capture(false);
        if (!e->key.down && !g_held[sc]) break;          // key went down while we lacked focus
        g_held[sc] = e->key.down;
        push((plat_event){ .type = PLAT_EVENT_KEY, .scancode = sc, .down = e->key.down });
        break;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!g_captured) break;
        g_fx += e->motion.xrel;
        g_fy += e->motion.yrel;
        int dx = (int)g_fx, dy = (int)g_fy;
        g_fx -= (float)dx; g_fy -= (float)dy;
        if (dx || dy) push((plat_event){ .type = PLAT_EVENT_MOUSE_MOVE, .dx = dx, .dy = dy });
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        int b = e->button.button;
        if (!g_captured) {
            if (e->button.down && b == SDL_BUTTON_LEFT) set_capture(true);   // this click only captures
            break;
        }
        if (b < 0 || b >= 32) break;
        if (!e->button.down && !(g_held_buttons & (1u << b))) break;
        if (e->button.down) g_held_buttons |= 1u << b; else g_held_buttons &= ~(1u << b);
        push((plat_event){ .type = PLAT_EVENT_MOUSE_BUTTON, .button = b, .down = e->button.down });
        break;
    }
    default: break;
    }
}

bool platform_poll_event(plat_event *out)
{
    SDL_Event e;
    while (g_qcount == 0 && SDL_PollEvent(&e)) translate(&e);
    if (g_qcount == 0) return false;
    *out = g_q[g_qhead];
    g_qhead = (g_qhead + 1) % QLEN;
    g_qcount--;
    return true;
}

/* ---- sound ---- */

static SDL_AudioStream *g_audio;
static int g_audio_rate;
static uint32_t g_cushion_bytes;
static uint64_t g_underruns, g_dropped_bytes, g_padded_bytes;

bool platform_audio_open(int rate)
{
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        fprintf(stderr, "beemu: no audio: %s\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec spec = { SDL_AUDIO_S16LE, 2, rate };
    g_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!g_audio) {
        fprintf(stderr, "beemu: no audio device: %s\n", SDL_GetError());
        return false;
    }
    g_audio_rate = rate;
    const char *c = getenv("BEEMU_AUDIO_CUSHION_MS");                 // default 60 ms; 0 = none
    uint32_t ms = c ? (uint32_t)strtoul(c, NULL, 10) : 60;
    g_cushion_bytes = (uint32_t)((uint64_t)rate * ms / 1000) * 4;
    SDL_ResumeAudioStreamDevice(g_audio);
    return true;
}

// The emulated card produces sound in real time but in bursts (between time slices, not while a
// frame is drawn), so the device would run dry between bursts and click. Keep a cushion: after
// the queue empties, put g_cushion of silence in front of the next data; beyond 0.25 s, drop.
void platform_audio_write(const int16_t *frames, int n)
{
    if (!g_audio || n <= 0) return;
    int queued = SDL_GetAudioStreamQueued(g_audio);
    if (queued > g_audio_rate * 4 / 4) { g_dropped_bytes += (uint64_t)n * 4; return; }
    if (queued == 0 && g_cushion_bytes) {
        static uint8_t zero[16384];
        g_underruns++;
        for (uint32_t left = g_cushion_bytes; left; ) {
            uint32_t k = left < sizeof zero ? left : (uint32_t)sizeof zero;
            SDL_PutAudioStreamData(g_audio, zero, (int)k);
            left -= k;
        }
        g_padded_bytes += g_cushion_bytes;
    } else if (queued == 0) {
        g_underruns++;
    }
    SDL_PutAudioStreamData(g_audio, frames, n * 4);
}

void platform_shutdown(void)
{
    if (g_audio) {
        if (getenv("BEEMU_STATS"))
            fprintf(stderr, "beemu: audio: %llu underruns (device ran dry), %.2f s of silence padded, %.2f s dropped\n",
                    (unsigned long long)g_underruns, (double)g_padded_bytes / 4 / g_audio_rate,
                    (double)g_dropped_bytes / 4 / g_audio_rate);
        SDL_DestroyAudioStream(g_audio);
        g_audio = NULL;
    }
    if (g_tex) SDL_DestroyTexture(g_tex);
    if (g_ren) SDL_DestroyRenderer(g_ren);
    if (g_win) SDL_DestroyWindow(g_win);
    SDL_Quit();
}

/* ---- dialogs ---- */

typedef struct { volatile bool done; char *path; } pick_t;

static void SDLCALL pick_done(void *userdata, const char *const *files, int filter)
{
    (void)filter;
    pick_t *p = userdata;
    if (files && files[0]) p->path = SDL_strdup(files[0]);
    p->done = true;
}

bool platform_ui_init(void)
{
    return SDL_WasInit(SDL_INIT_VIDEO) || SDL_Init(SDL_INIT_VIDEO);
}

char *platform_pick_file(const char *title, const char *default_path)
{
    if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_Init(SDL_INIT_VIDEO)) return NULL;
    pick_t p = { false, NULL };
    SDL_PropertiesID props = SDL_CreateProperties();
    if (title) SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, title);
    if (default_path) SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, default_path);
    if (g_win) SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, g_win);
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFILE, pick_done, &p, props);
    SDL_DestroyProperties(props);
    while (!p.done) {                 // the dialog runs on the main thread's event loop
        SDL_Event e;
        SDL_WaitEventTimeout(&e, 50);
    }
    if (!p.path) return NULL;
    char *out = strdup(p.path);       // caller frees with free()
    SDL_free(p.path);
    return out;
}

int platform_choose(const char *title, const char *message, const char *const *buttons, int n, int def)
{
    if (n <= 0 || n > 8) return -1;
    if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_Init(SDL_INIT_VIDEO)) return -1;
    SDL_MessageBoxButtonData b[8];
    for (int i = 0; i < n; i++)
        b[i] = (SDL_MessageBoxButtonData){
            .flags = i == def ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0, .buttonID = i, .text = buttons[i] };
    SDL_MessageBoxData d = { .flags = SDL_MESSAGEBOX_INFORMATION, .window = g_win, .title = title,
                             .message = message, .numbuttons = n, .buttons = b };
    int hit = -1;
    if (!SDL_ShowMessageBox(&d, &hit)) return -1;
    return hit;
}

void platform_error(const char *title, const char *message)
{
    if (!SDL_WasInit(SDL_INIT_VIDEO)) SDL_Init(SDL_INIT_VIDEO);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, message, g_win);
}
