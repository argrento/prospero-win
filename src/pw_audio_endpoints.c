/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_audio_endpoints.h"
#include <string.h>

static const uint16_t render_name[] = { 'P', 'S', '5', 0 };
static const char render_device[] = "ps5";
static const uint16_t capture_name[] = { 'M', 'i', 'c', 'r', 'o', 'p', 'h', 'o', 'n', 'e', 0 };
static const char capture_device[] = "ps5-mic";

typedef void *(*Lookup)(const char *name);

void pw_audio_sink_find(PwAudioSinkCalls *calls, Lookup lookup)
{
    if (!calls) return;
    memset(calls, 0, sizeof(*calls));
    if (!lookup) return;
    calls->available = (int (*)(void))(uintptr_t)lookup("pw_wine_audio_available");
    calls->output = (int (*)(const int16_t *))(uintptr_t)lookup("pw_wine_audio_output");
    if (!calls->available || !calls->output) calls->available = NULL, calls->output = NULL;
    calls->input_available = (int (*)(void))(uintptr_t)lookup("pw_wine_audio_input_available");
    calls->input_open = (int (*)(uint32_t *))(uintptr_t)lookup("pw_wine_audio_input_open");
    calls->input_read = (int (*)(int16_t *))(uintptr_t)lookup("pw_wine_audio_input_read");
    calls->input_close = (void (*)(void))(uintptr_t)lookup("pw_wine_audio_input_close");
    if (!calls->input_available || !calls->input_open || !calls->input_read || !calls->input_close) {
        calls->input_available = NULL;
        calls->input_open = NULL;
        calls->input_read = NULL;
        calls->input_close = NULL;
    }
}

size_t pw_audio_endpoints(const PwAudioSinkCalls *calls, int flow, PwAudioEndpoint *endpoint)
{
    if (!calls || !endpoint) return 0;
    if (flow == PW_AUDIO_FLOW_RENDER && calls->available && calls->available() == 1) {
        *endpoint = (PwAudioEndpoint){ render_name, sizeof(render_name), render_device, sizeof(render_device) };
        return 1;
    }
    if (flow == PW_AUDIO_FLOW_CAPTURE && calls->input_available && calls->input_available() == 1) {
        *endpoint = (PwAudioEndpoint){ capture_name, sizeof(capture_name), capture_device, sizeof(capture_device) };
        return 1;
    }
    return 0;
}

int pw_audio_endpoint_is(const char *device, int flow)
{
    if (!device) return 0;
    if (flow == PW_AUDIO_FLOW_RENDER) return !strcmp(device, render_device);
    if (flow == PW_AUDIO_FLOW_CAPTURE) return !strcmp(device, capture_device);
    return 0;
}
