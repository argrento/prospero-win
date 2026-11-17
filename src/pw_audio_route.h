/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_AUDIO_ROUTE_H
#define PW_AUDIO_ROUTE_H
/*
 * Which console audio port the title plays Wine's mix on, and when it moves.
 *
 * The title plays every grain on the main port. The console's "Output to
 * Headphones" setting decides where that port is heard: with "All Audio" the
 * system routes it to a headset (controller jack or USB) and reports the
 * headset in the port's state; with "Chat Audio" it stays on the TV. Whether
 * a main port opened as the system user follows the headset the way a native
 * game's does has not been measured on PS5, so the title watches the port
 * state, logs every change, and can move the mix itself:
 *
 *   PW_AUDIO_ROUTE_MAIN     the main port only (the default). The state is
 *                           read and logged; nothing moves.
 *   PW_AUDIO_ROUTE_MAIN_USER  the main port opened as the foreground user
 *                           instead of the system user (a native game's
 *                           way); nothing moves.
 *   PW_AUDIO_ROUTE_HEADSET  the main port, or the foreground user's personal
 *                           port while that port reports a headset. The
 *                           personal port reaches the headset under either
 *                           "Output to Headphones" setting, so this policy
 *                           does not follow "Chat Audio"; it is for a probe
 *                           and for a console where the main port is shown
 *                           not to follow.
 *
 * The router never moves while the main port itself reports the headset:
 * the console already plays the game there. A personal port that fails to
 * take a grain sends the mix back to the main port until its state changes.
 *
 * Portable: no console calls. The title reads the states every
 * PW_AUDIO_ROUTE_CHECK_GRAINS grains and plays on pw_audio_route_port().
 */
#include "../include/prospero_win.h"
#include <stdint.h>

/* Port state output bits (PS4 values from shadPS4, not verified on PS5). */
enum {
    PW_AUDIO_ROUTE_OUTPUT_PRIMARY = 0x01,
    PW_AUDIO_ROUTE_OUTPUT_HEADPHONE = 0x40,
    PW_AUDIO_ROUTE_OUTPUT_EXTERNAL = 0x80,
};

enum { PW_AUDIO_ROUTE_MAIN = 0, PW_AUDIO_ROUTE_HEADSET = 1, PW_AUDIO_ROUTE_MAIN_USER = 2 };
enum { PW_AUDIO_ROUTE_PORT_MAIN = 0, PW_AUDIO_ROUTE_PORT_PERSONAL = 1 };
/* About half a second of 256-frame grains at 48 kHz. */
enum { PW_AUDIO_ROUTE_CHECK_GRAINS = 96 };
/* pw_audio_route_update's result bits. */
enum { PW_AUDIO_ROUTE_STATE_CHANGED = 1, PW_AUDIO_ROUTE_PORT_CHANGED = 2 };

/* What a port reported; known is 0 when the state could not be read. */
typedef struct PwAudioRouteState {
    int known;
    uint16_t output;
    uint16_t reroute_counter;
} PwAudioRouteState;

typedef struct PwAudioRoute {
    int policy;
    int port;                       /* PW_AUDIO_ROUTE_PORT_* */
    uint32_t grains;                /* since the last check */
    PwAudioRouteState main, personal;
    int personal_failed;            /* until the personal port's state changes */
    uint32_t switches, failures;
} PwAudioRoute;

/* A router on the main port. A policy it does not know is
 * PW_AUDIO_ROUTE_MAIN. */
void pw_audio_route_init(PwAudioRoute *route, int policy);
/* The policy an environment value names: "headset", "main-user", else
 * main. */
int pw_audio_route_policy(const char *value);
/* Counts one grain: 1 when the states are due for a check (the first
 * grain, then every PW_AUDIO_ROUTE_CHECK_GRAINS). */
int pw_audio_route_due(PwAudioRoute *route);
/* Takes the newest states (personal NULL when no personal port is open) and
 * picks the port. PW_AUDIO_ROUTE_* result bits. */
int pw_audio_route_update(PwAudioRoute *route, const PwAudioRouteState *main,
                          const PwAudioRouteState *personal);
/* The personal port refused a grain: back to the main port. */
void pw_audio_route_failed(PwAudioRoute *route);
int pw_audio_route_port(const PwAudioRoute *route);

#endif
