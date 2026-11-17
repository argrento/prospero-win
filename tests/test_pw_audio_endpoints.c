/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_audio_endpoints.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int render_on = 1, input_on = 1;
static int available(void) { return render_on; }
static int output(const int16_t *frames) { (void)frames; return 0; }
static int input_available(void) { return input_on; }
static int input_open(uint32_t *rate) { *rate = 48000; return 0; }
static int input_read(int16_t *frames) { (void)frames; return 0; }
static void input_close(void) {}

/* What ntdll exports: all of it, or a title shell from before capture. */
static const char *missing;
static void *lookup(const char *name)
{
    if (missing && !strcmp(name, missing)) return NULL;
    if (!strcmp(name, "pw_wine_audio_available")) return (void *)(uintptr_t)available;
    if (!strcmp(name, "pw_wine_audio_output")) return (void *)(uintptr_t)output;
    if (!strcmp(name, "pw_wine_audio_input_available")) return (void *)(uintptr_t)input_available;
    if (!strcmp(name, "pw_wine_audio_input_open")) return (void *)(uintptr_t)input_open;
    if (!strcmp(name, "pw_wine_audio_input_read")) return (void *)(uintptr_t)input_read;
    if (!strcmp(name, "pw_wine_audio_input_close")) return (void *)(uintptr_t)input_close;
    return NULL;
}
static void *older_ntdll(const char *name)
{
    return strstr(name, "_input_") ? NULL : lookup(name);
}

static size_t list(const PwAudioSinkCalls *calls, int flow, PwAudioEndpoint *endpoint)
{
    memset(endpoint, 0, sizeof(*endpoint));
    return pw_audio_endpoints(calls, flow, endpoint);
}

static void test_both_flows(void)
{
    PwAudioSinkCalls calls;
    PwAudioEndpoint e;
    pw_audio_sink_find(&calls, lookup);
    assert(calls.available && calls.output && calls.input_available && calls.input_open && calls.input_read &&
           calls.input_close);
    assert(list(&calls, PW_AUDIO_FLOW_RENDER, &e) == 1);
    assert(e.name[0] == 'P' && e.name[2] == '5' && e.name[3] == 0 && e.name_bytes == 4 * sizeof(uint16_t));
    assert(!strcmp(e.device, "ps5") && e.device_bytes == 4);
    assert(list(&calls, PW_AUDIO_FLOW_CAPTURE, &e) == 1);
    assert(e.name[0] == 'M' && e.name[9] == 'e' && e.name[10] == 0 && e.name_bytes == 11 * sizeof(uint16_t));
    assert(!strcmp(e.device, "ps5-mic") && e.device_bytes == 8);
    assert(list(&calls, 2, &e) == 0);            /* eAll is not a flow the driver lists */
    assert(pw_audio_endpoint_is("ps5", PW_AUDIO_FLOW_RENDER));
    assert(pw_audio_endpoint_is("ps5-mic", PW_AUDIO_FLOW_CAPTURE));
    assert(!pw_audio_endpoint_is("ps5", PW_AUDIO_FLOW_CAPTURE));
    assert(!pw_audio_endpoint_is("ps5-mic", PW_AUDIO_FLOW_RENDER));
    assert(!pw_audio_endpoint_is(NULL, PW_AUDIO_FLOW_RENDER) && !pw_audio_endpoint_is("ps5", 2));
}

/* A title that set no input (the microphone off, or no user): no capture
 * endpoint, the render one as before. Likewise with no output sink. */
static void test_title_choices(void)
{
    PwAudioSinkCalls calls;
    PwAudioEndpoint e;
    pw_audio_sink_find(&calls, lookup);
    input_on = 0;
    assert(list(&calls, PW_AUDIO_FLOW_CAPTURE, &e) == 0 && list(&calls, PW_AUDIO_FLOW_RENDER, &e) == 1);
    input_on = 1;
    render_on = 0;
    assert(list(&calls, PW_AUDIO_FLOW_RENDER, &e) == 0 && list(&calls, PW_AUDIO_FLOW_CAPTURE, &e) == 1);
    render_on = 1;
}

/* An ntdll without the input calls, or with only some: no capture endpoint. */
static void test_lookup(void)
{
    PwAudioSinkCalls calls;
    PwAudioEndpoint e;
    pw_audio_sink_find(&calls, older_ntdll);
    assert(calls.available && calls.output && !calls.input_available && !calls.input_read);
    assert(list(&calls, PW_AUDIO_FLOW_RENDER, &e) == 1 && list(&calls, PW_AUDIO_FLOW_CAPTURE, &e) == 0);
    static const char *const each[] = { "pw_wine_audio_input_available", "pw_wine_audio_input_open",
                                        "pw_wine_audio_input_read", "pw_wine_audio_input_close" };
    for (size_t i = 0; i < 4; i++) {
        missing = each[i];
        pw_audio_sink_find(&calls, lookup);
        assert(!calls.input_available && !calls.input_open && !calls.input_read && !calls.input_close);
        assert(calls.output && list(&calls, PW_AUDIO_FLOW_CAPTURE, &e) == 0);
    }
    missing = "pw_wine_audio_output";
    pw_audio_sink_find(&calls, lookup);
    assert(!calls.available && !calls.output && calls.input_read);
    assert(list(&calls, PW_AUDIO_FLOW_RENDER, &e) == 0 && list(&calls, PW_AUDIO_FLOW_CAPTURE, &e) == 1);
    missing = NULL;
    pw_audio_sink_find(&calls, NULL);
    assert(!calls.available && !calls.input_available);
    pw_audio_sink_find(NULL, lookup);
    assert(pw_audio_endpoints(NULL, PW_AUDIO_FLOW_RENDER, &e) == 0);
    pw_audio_sink_find(&calls, lookup);
    assert(pw_audio_endpoints(&calls, PW_AUDIO_FLOW_RENDER, NULL) == 0);
}

int main(void)
{
    test_both_flows();
    test_title_choices();
    test_lookup();
    puts("audio endpoints passed");
    return 0;
}
