/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_AUDIO_PS5_H
#define PW_AUDIO_PS5_H
/*
 * The console's main audio port, for a caller that mixes and clocks
 * itself: Wine's audio driver (wine/wineps5) hands the title one grain of
 * 16-bit stereo at a time, and each output call returns when the port has
 * taken it.
 */
#include "../include/prospero_win.h"
#include "../src/pw_audio_route.h"
#include <stdint.h>

enum { PW_AUDIO_PS5_RATE=48000, PW_AUDIO_PS5_GRAIN=256, PW_AUDIO_PS5_CHANNELS=2 };
/* Users and port types (PS4 values from shadPS4, not verified on PS5). */
enum { PW_AUDIO_PS5_USER_SYSTEM=0xff, PW_AUDIO_PS5_PORT_MAIN=0, PW_AUDIO_PS5_PORT_VOICE=2,
       PW_AUDIO_PS5_PORT_PERSONAL=3, PW_AUDIO_PS5_PORT_PADSPK=4 };

/* sceAudioOutGetPortState's answer as shadPS4 lays it out for the PS4. The
 * call takes no size, so the buffer has room past it in case the console's
 * is larger; the title logs the raw bytes until the layout is measured. */
typedef struct PwAudioPs5PortState {
    uint16_t output;            /* PW_AUDIO_ROUTE_OUTPUT_* */
    uint8_t channel,reserved8;
    int16_t volume;
    uint16_t reroute_counter;
    uint64_t flag;
    uint64_t reserved64[2];
    uint8_t slack[32];
} PwAudioPs5PortState;

typedef struct PwAudioPs5Ops {
    int (*init)(void);
    int (*open)(int user_id,int type,int index,uint32_t grain,
                uint32_t rate,uint32_t format);
    int (*volume)(int handle,int flags,const int32_t *volumes);
    int (*output)(int handle,const void *samples);
    int (*close)(int handle);
    int (*port_state)(int handle,PwAudioPs5PortState *state);  /* optional */
} PwAudioPs5Ops;

int pw_audio_ps5_platform_ops(PwAudioPs5Ops *);
/* The main port for a caller that mixes and clocks itself (Wine's audio
 * sink): the library initialised, PW_AUDIO_PS5_GRAIN frames of 16-bit
 * stereo at PW_AUDIO_PS5_RATE per output call, unity volume. The caller
 * then calls ops->output(*handle, grain) and ops->close(*handle).
 * PW_ERR_STATE when the library refuses. */
int pw_audio_ps5_open_port(const PwAudioPs5Ops *ops,int *handle);
/* The same for user_id's port of type (the main port of the foreground
 * user, or its personal port); result (optional) gets the console's answer
 * to the open (the handle, or its error) for the log. */
int pw_audio_ps5_open_port_as(const PwAudioPs5Ops *ops,int user_id,int type,int *handle,int *result);
/* The port's state for the router: known 0 when ops has no port_state or
 * the call failed (its result in *status). raw (optional) gets the bytes. */
PwAudioRouteState pw_audio_ps5_port_state(const PwAudioPs5Ops *ops,int handle,int *status,
                                          PwAudioPs5PortState *raw);

#endif
