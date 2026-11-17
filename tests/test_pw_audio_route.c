/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_audio_route.h"
#include <assert.h>
#include <stdio.h>

static PwAudioRouteState state(uint16_t output, uint16_t reroute)
{
    return (PwAudioRouteState){1, output, reroute};
}

static void test_policy_and_due(void)
{
    PwAudioRoute route;
    assert(pw_audio_route_policy(NULL) == PW_AUDIO_ROUTE_MAIN);
    assert(pw_audio_route_policy("") == PW_AUDIO_ROUTE_MAIN);
    assert(pw_audio_route_policy("main") == PW_AUDIO_ROUTE_MAIN);
    assert(pw_audio_route_policy("Headset") == PW_AUDIO_ROUTE_MAIN);
    assert(pw_audio_route_policy("headset") == PW_AUDIO_ROUTE_HEADSET);
    assert(pw_audio_route_policy("main-user") == PW_AUDIO_ROUTE_MAIN_USER);
    pw_audio_route_init(&route, 7);
    assert(route.policy == PW_AUDIO_ROUTE_MAIN && pw_audio_route_port(&route) == PW_AUDIO_ROUTE_PORT_MAIN);
    /* the first grain, then every PW_AUDIO_ROUTE_CHECK_GRAINS */
    assert(pw_audio_route_due(&route));
    for (int i = 1; i < PW_AUDIO_ROUTE_CHECK_GRAINS; i++) assert(!pw_audio_route_due(&route));
    assert(pw_audio_route_due(&route));
    assert(!pw_audio_route_due(NULL) && pw_audio_route_port(NULL) == PW_AUDIO_ROUTE_PORT_MAIN);
    pw_audio_route_init(NULL, 0);
    pw_audio_route_failed(NULL);
    assert(pw_audio_route_update(NULL, NULL, NULL) == 0);
}

/* The default policy logs every change and never leaves the main port. */
static void test_main_policy(void)
{
    PwAudioRoute route;
    PwAudioRouteState tv = state(PW_AUDIO_ROUTE_OUTPUT_PRIMARY, 0), head = state(PW_AUDIO_ROUTE_OUTPUT_HEADPHONE, 1);
    /* main-user only changes the user the main port is opened as */
    pw_audio_route_init(&route, PW_AUDIO_ROUTE_MAIN_USER);
    assert(route.policy == PW_AUDIO_ROUTE_MAIN_USER);
    assert(pw_audio_route_update(&route, &tv, &head) == PW_AUDIO_ROUTE_STATE_CHANGED);
    assert(pw_audio_route_port(&route) == PW_AUDIO_ROUTE_PORT_MAIN);
    pw_audio_route_init(&route, PW_AUDIO_ROUTE_MAIN);
    assert(pw_audio_route_update(&route, &tv, NULL) == PW_AUDIO_ROUTE_STATE_CHANGED);
    assert(pw_audio_route_update(&route, &tv, NULL) == 0);
    assert(pw_audio_route_update(&route, &tv, &head) == PW_AUDIO_ROUTE_STATE_CHANGED);
    assert(pw_audio_route_port(&route) == PW_AUDIO_ROUTE_PORT_MAIN);
    /* only the reroute counter moves: still a change worth a log line */
    PwAudioRouteState rerouted = state(PW_AUDIO_ROUTE_OUTPUT_PRIMARY, 1);
    assert(pw_audio_route_update(&route, &rerouted, &head) == PW_AUDIO_ROUTE_STATE_CHANGED);
    PwAudioRouteState unknown = {0, 0, 0};
    assert(pw_audio_route_update(&route, &unknown, &head) == PW_AUDIO_ROUTE_STATE_CHANGED);
    assert(route.switches == 0 && pw_audio_route_port(&route) == PW_AUDIO_ROUTE_PORT_MAIN);
}

static void test_headset_policy(void)
{
    PwAudioRoute route;
    PwAudioRouteState tv = state(PW_AUDIO_ROUTE_OUTPUT_PRIMARY, 0);
    PwAudioRouteState none = state(0, 0), head = state(PW_AUDIO_ROUTE_OUTPUT_HEADPHONE, 1);
    PwAudioRouteState main_head = state(PW_AUDIO_ROUTE_OUTPUT_HEADPHONE, 1);
    pw_audio_route_init(&route, PW_AUDIO_ROUTE_HEADSET);
    assert(pw_audio_route_update(&route, &tv, &none) == PW_AUDIO_ROUTE_STATE_CHANGED);
    assert(pw_audio_route_port(&route) == PW_AUDIO_ROUTE_PORT_MAIN);
    /* a headset on the personal port: the mix moves there */
    assert(pw_audio_route_update(&route, &tv, &head) == (PW_AUDIO_ROUTE_STATE_CHANGED | PW_AUDIO_ROUTE_PORT_CHANGED));
    assert(pw_audio_route_port(&route) == PW_AUDIO_ROUTE_PORT_PERSONAL && route.switches == 1);
    assert(pw_audio_route_update(&route, &tv, &head) == 0);
    /* unplugged: back */
    PwAudioRouteState gone = state(0, 2);
    assert(pw_audio_route_update(&route, &tv, &gone) == (PW_AUDIO_ROUTE_STATE_CHANGED | PW_AUDIO_ROUTE_PORT_CHANGED));
    assert(pw_audio_route_port(&route) == PW_AUDIO_ROUTE_PORT_MAIN && route.switches == 2);
    /* the console moves the main port itself: nothing to do */
    assert(pw_audio_route_update(&route, &main_head, &head) == PW_AUDIO_ROUTE_STATE_CHANGED);
    assert(pw_audio_route_port(&route) == PW_AUDIO_ROUTE_PORT_MAIN);
    /* no personal port, or an unreadable one: main */
    assert(pw_audio_route_update(&route, &tv, NULL) == PW_AUDIO_ROUTE_STATE_CHANGED);
    assert(pw_audio_route_port(&route) == PW_AUDIO_ROUTE_PORT_MAIN);
    PwAudioRouteState unread = {0, PW_AUDIO_ROUTE_OUTPUT_HEADPHONE, 0};
    assert(pw_audio_route_update(&route, &tv, &unread) == PW_AUDIO_ROUTE_STATE_CHANGED);
    assert(pw_audio_route_port(&route) == PW_AUDIO_ROUTE_PORT_MAIN);
}

/* A personal port that refuses grains is left alone until its state changes. */
static void test_failure(void)
{
    PwAudioRoute route;
    PwAudioRouteState tv = state(PW_AUDIO_ROUTE_OUTPUT_PRIMARY, 0), head = state(PW_AUDIO_ROUTE_OUTPUT_HEADPHONE, 1);
    pw_audio_route_init(&route, PW_AUDIO_ROUTE_HEADSET);
    pw_audio_route_failed(&route);          /* on the main port: nothing */
    assert(route.failures == 0);
    assert(pw_audio_route_update(&route, &tv, &head) & PW_AUDIO_ROUTE_PORT_CHANGED);
    pw_audio_route_failed(&route);
    assert(pw_audio_route_port(&route) == PW_AUDIO_ROUTE_PORT_MAIN && route.failures == 1 && route.switches == 2);
    assert(pw_audio_route_update(&route, &tv, &head) == 0);
    assert(pw_audio_route_port(&route) == PW_AUDIO_ROUTE_PORT_MAIN);
    PwAudioRouteState replugged = state(PW_AUDIO_ROUTE_OUTPUT_HEADPHONE, 3);
    assert(pw_audio_route_update(&route, &tv, &replugged) == (PW_AUDIO_ROUTE_STATE_CHANGED | PW_AUDIO_ROUTE_PORT_CHANGED));
    assert(pw_audio_route_port(&route) == PW_AUDIO_ROUTE_PORT_PERSONAL);
}

int main(void)
{
    test_policy_and_due();
    test_main_policy();
    test_headset_policy();
    test_failure();
    puts("audio route passed");
    return 0;
}
