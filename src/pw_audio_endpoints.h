/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_AUDIO_ENDPOINTS_H
#define PW_AUDIO_ENDPOINTS_H
#include "../include/prospero_win.h"
#include <stddef.h>
#include <stdint.h>

/* The devices Wine's PS5 audio driver (wine/wineps5) lists, and the title
 * calls it finds them through.
 *
 * The title's sink lives in ntdll (wine/ps5/pw_wine_sink.h); the driver
 * looks its calls up by name. "PS5", the render endpoint, needs the output
 * pair; "Microphone", the capture endpoint, needs the four input calls and
 * a title that set an input: a title without one (an older title, or a
 * game whose profile turns the microphone off) lists no capture device, as
 * before capture existed. Each flow has one endpoint, its default.
 *
 * Portable: the driver passes its dlsym. */

/* Wine's EDataFlow. */
enum { PW_AUDIO_FLOW_RENDER = 0, PW_AUDIO_FLOW_CAPTURE = 1 };

typedef struct PwAudioSinkCalls {
    int (*available)(void);
    int (*output)(const int16_t *frames);
    int (*input_available)(void);
    int (*input_open)(uint32_t *rate);
    int (*input_read)(int16_t *frames);
    void (*input_close)(void);
} PwAudioSinkCalls;

typedef struct PwAudioEndpoint {
    const uint16_t *name;       /* UTF-16, terminated; name_bytes with the terminator */
    size_t name_bytes;
    const char *device;         /* the driver's id for it, terminated */
    size_t device_bytes;
} PwAudioEndpoint;

/* Looks every call up: the output pair only together, the input calls only
 * all four. A NULL lookup finds nothing. */
void pw_audio_sink_find(PwAudioSinkCalls *calls, void *(*lookup)(const char *name));
/* The endpoints of flow the title's sink offers now, into endpoint (room for
 * one): how many. */
size_t pw_audio_endpoints(const PwAudioSinkCalls *calls, int flow, PwAudioEndpoint *endpoint);
/* Whether device is the id of flow's endpoint. */
int pw_audio_endpoint_is(const char *device, int flow);

#endif
