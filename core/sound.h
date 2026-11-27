// Host side of emulated sound. Sound devices produce 16-bit signed stereo at SOUND_RATE as
// host time passes and hand it to a sound_out_fn; the frontend plays it (SDL audio) or
// writes it to a WAV file. The core never includes an audio library (like net.h for networking).
#ifndef SOUND_H
#define SOUND_H

#include <stdint.h>

#define SOUND_RATE 44100

// frames: interleaved left/right samples, n frames.
typedef void (*sound_out_fn)(void *opaque, const int16_t *frames, int n);

#endif
