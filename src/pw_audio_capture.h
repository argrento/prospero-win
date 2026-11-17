/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_AUDIO_CAPTURE_H
#define PW_AUDIO_CAPTURE_H
#include "pw_audio_mix.h"

/* The capture side of Wine's PS5 audio driver (wine/wineps5): the console's
 * microphone gives 16-bit mono grains at its own rate; each capture stream
 * keeps a ring of frames in the format its client chose (mmdevapi's local
 * buffer) and gets every grain converted into it.
 *
 * Conversion resamples linearly from the microphone's rate to the client's
 * (one frame of latency, like the mixer's), copies the mono sample to every
 * channel and encodes it as 8-bit unsigned, 16/24/32-bit signed or 32-bit
 * float PCM. The mixer's resampler (pw_audio_mix.h) only goes towards the
 * console's 48 kHz stereo, so capture has its own.
 *
 * A full ring drops its oldest frame for each new one (the client is late:
 * it gets the newest sound). While the client reads (between GetBuffer and
 * ReleaseBuffer, `locked`), the oldest frames are the ones it reads, so a
 * new frame that finds the ring full is lost instead.
 *
 * Portable: no allocation and no locking. The driver owns the ring and
 * serialises access to a stream. */

typedef struct PwAudioCapture {
    PwAudioMixFormat format;    /* the client's */
    uint8_t *frames;            /* ring of capacity frames, block_align bytes each */
    uint32_t capacity;
    uint32_t offset,held;       /* the oldest unread frame, and how many there are */
    uint32_t locked;            /* frames from offset the client is reading */
    uint64_t written,dropped;   /* frames put in the ring, and frames lost to a full one */
    uint32_t source_rate;       /* the microphone's, 0 until the first write */
    uint64_t step,phase;        /* source frames per client frame, and position: 32.32 */
    float previous,current;
} PwAudioCapture;

/* An empty ring frames[capacity] for format (pw_audio_mix_format_check). */
int pw_audio_capture_init(PwAudioCapture *capture,const PwAudioMixFormat *format,
                          uint8_t *frames,uint32_t capacity);
/* Forget the held frames and the resampler's history (a stopped stream's Reset). */
void pw_audio_capture_reset(PwAudioCapture *capture);
/* Convert count mono 16-bit frames at rate (NULL: that many frames of
 * silence) into the ring. Returns the client frames made, dropped ones
 * included; 0 for a rate outside PW_AUDIO_MIX_MIN_RATE..MAX_RATE. */
uint32_t pw_audio_capture_write(PwAudioCapture *capture,const int16_t *samples,uint32_t count,
                                uint32_t rate);
/* The client is done with frames frames from offset (at most held). */
void pw_audio_capture_consume(PwAudioCapture *capture,uint32_t frames);

#endif
