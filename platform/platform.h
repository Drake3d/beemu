// Thin platform interface. The emulator core never includes SDL.
// A native Metal/CoreAudio backend can replace platform_sdl3.c later.
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PLAT_EVENT_NONE = 0,
    PLAT_EVENT_QUIT,
    PLAT_EVENT_KEY,        // scancode = USB HID usage (Mac Cmd/Option already mapped to Alt/GUI)
    PLAT_EVENT_MOUSE_MOVE, // dx, dy relative; only while the mouse is captured
    PLAT_EVENT_MOUSE_BUTTON, // button: 1 left, 2 middle, 3 right
} plat_event_type;

typedef struct {
    plat_event_type type;
    int  scancode;
    bool down;
    int  dx, dy;
    int  button;
} plat_event;

// Ask the host to treat the calling (emulation) thread as interactive and latency critical:
// top scheduling priority, no App Nap, no timer coalescing. Works without platform_init.
// The guest's sound and timing depend on it waking on time. BEEMU_NO_BOOST=1 skips it.
void platform_boost_thread(void);
bool platform_init(const char *title, int width, int height);
void platform_present_frame(const uint32_t *xrgb, int width, int height, int pitch_bytes);
// Like platform_present_frame, but only rows [y0, y1) changed since the last call (empty when
// y0 >= y1): only they are uploaded, and nothing is presented when nothing changed, unless the
// window needs redrawing. Returns true if a frame was presented.
bool platform_present_rows(const uint32_t *xrgb, int width, int height, int pitch_bytes, int y0, int y1);
bool platform_poll_event(plat_event *out);   // returns false when queue is empty
void platform_shutdown(void);

// Sets up the host's windowing (application object, menu) without opening a window, so native
// dialogs and the machine launcher can run before platform_init. Returns false if it can't.
bool platform_ui_init(void);

// Native "open file" dialog (works before platform_init). Returns a malloc'd path,
// or NULL if the user cancelled. default_path: a file or folder to start in (may be NULL).
char *platform_pick_file(const char *title, const char *default_path);

// Native alert with buttons (works before platform_init). Returns the index of the button
// pressed; def is the one Return picks. -1 if the dialog could not be shown or was closed.
int platform_choose(const char *title, const char *message, const char *const *buttons, int n, int def);

// Sound output: 16-bit signed interleaved stereo at `rate`. open returns false if there is no
// audio device. write may be called from the emulation thread at any pace; data beyond about
// a quarter second of queued sound is dropped so latency can't grow.
bool platform_audio_open(int rate);
void platform_audio_write(const int16_t *frames, int n);

// Native error alert, for when there is no terminal to print to (the .app).
void platform_error(const char *title, const char *message);

#endif
