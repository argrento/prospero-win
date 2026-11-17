/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_audio_route.h"
#include <string.h>

void pw_audio_route_init(PwAudioRoute *route, int policy)
{
    if (!route) return;
    memset(route, 0, sizeof(*route));
    route->policy = policy == PW_AUDIO_ROUTE_HEADSET || policy == PW_AUDIO_ROUTE_MAIN_USER ? policy :
                    PW_AUDIO_ROUTE_MAIN;
    route->port = PW_AUDIO_ROUTE_PORT_MAIN;
}

int pw_audio_route_policy(const char *value)
{
    if (value && !strcmp(value, "headset")) return PW_AUDIO_ROUTE_HEADSET;
    if (value && !strcmp(value, "main-user")) return PW_AUDIO_ROUTE_MAIN_USER;
    return PW_AUDIO_ROUTE_MAIN;
}

int pw_audio_route_due(PwAudioRoute *route)
{
    if (!route) return 0;
    if (route->grains++ % PW_AUDIO_ROUTE_CHECK_GRAINS) return 0;
    return 1;
}

static int same(const PwAudioRouteState *a, const PwAudioRouteState *b)
{
    return a->known == b->known && a->output == b->output && a->reroute_counter == b->reroute_counter;
}

static int headphone(const PwAudioRouteState *state)
{
    return state->known && (state->output & PW_AUDIO_ROUTE_OUTPUT_HEADPHONE);
}

int pw_audio_route_update(PwAudioRoute *route, const PwAudioRouteState *main,
                          const PwAudioRouteState *personal)
{
    static const PwAudioRouteState none;
    int result = 0, port;

    if (!route || !main) return 0;
    if (!personal) personal = &none;
    if (!same(&route->main, main) || !same(&route->personal, personal)) result |= PW_AUDIO_ROUTE_STATE_CHANGED;
    /* a personal port that changed may take grains again */
    if (!same(&route->personal, personal)) route->personal_failed = 0;
    route->main = *main;
    route->personal = *personal;

    port = PW_AUDIO_ROUTE_PORT_MAIN;
    /* The console already plays the main port on the headset: stay. */
    if (route->policy == PW_AUDIO_ROUTE_HEADSET && !headphone(main) && headphone(personal) &&
        !route->personal_failed)
        port = PW_AUDIO_ROUTE_PORT_PERSONAL;
    if (port != route->port) {
        route->port = port;
        route->switches++;
        result |= PW_AUDIO_ROUTE_PORT_CHANGED;
    }
    return result;
}

void pw_audio_route_failed(PwAudioRoute *route)
{
    if (!route || route->port != PW_AUDIO_ROUTE_PORT_PERSONAL) return;
    route->personal_failed = 1;
    route->port = PW_AUDIO_ROUTE_PORT_MAIN;
    route->failures++;
    route->switches++;
}

int pw_audio_route_port(const PwAudioRoute *route)
{
    return route ? route->port : PW_AUDIO_ROUTE_PORT_MAIN;
}
