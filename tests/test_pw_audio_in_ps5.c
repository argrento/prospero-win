/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../native/pw_audio_in_ps5.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* A fake libSceAudioIn: which opens it takes, what reads and states give. */
static int opens, inputs, closes, states, next_handle = 10;
static int refuse_rate, refuse_all, input_fails, silent_value;
static int last_type, last_user;
static uint32_t last_rate, last_grain, last_format;
static int open_handles;
static char lines[32][160];
static int line_count;

static int op_open(int u, int t, int i, uint32_t g, uint32_t r, uint32_t f)
{
    opens++;
    last_user = u, last_type = t, last_rate = r, last_grain = g, last_format = f;
    assert(i == 0);
    if (refuse_all || (refuse_rate && r == (uint32_t)refuse_rate)) return (int)0x80260005u;
    open_handles++;
    return next_handle++;
}
static int op_input(int h, void *p)
{
    int16_t *s = p;
    assert(h >= 10);
    inputs++;
    if (input_fails) return (int)0x80260009u;
    for (int i = 0; i < PW_AUDIO_IN_PS5_GRAIN; i++) s[i] = (int16_t)(h * 100 + i);
    return PW_AUDIO_IN_PS5_GRAIN;
}
static int op_silent(int h) { (void)h; states++; return silent_value; }
static int op_close(int h) { assert(h >= 10); closes++; open_handles--; return 0; }
static void op_log(const char *line)
{
    if (line_count < 32) snprintf(lines[line_count++], sizeof(lines[0]), "%s", line);
}
static int logged(const char *text)
{
    for (int i = 0; i < line_count; i++)
        if (strstr(lines[i], text)) return 1;
    return 0;
}

static const PwAudioInPs5Ops ops = { op_open, op_input, op_silent, op_close, op_log };

static void reset(void)
{
    opens = inputs = closes = states = open_handles = 0;
    refuse_rate = refuse_all = input_fails = silent_value = 0;
    next_handle = 10;
    line_count = 0;
}

static void test_init(void)
{
    PwAudioInPs5 in;
    PwAudioInPs5Ops partial = ops;
    uint32_t rate;
    int16_t grain[PW_AUDIO_IN_PS5_GRAIN];
    partial.silent_state = NULL;
    assert(pw_audio_in_ps5_init(NULL, &ops, 1, 0) == PW_ERR_PRECONDITION);
    assert(pw_audio_in_ps5_init(&in, NULL, 1, 0) == PW_ERR_PRECONDITION && in.handle == -1);
    assert(pw_audio_in_ps5_init(&in, &partial, 1, 0) == PW_ERR_PRECONDITION);
    assert(pw_audio_in_ps5_init(&in, &ops, -1, 0) == PW_ERR_PRECONDITION);
    /* a microphone that failed to init opens nothing */
    assert(pw_audio_in_ps5_open(&in, &rate) == PW_ERR_PRECONDITION && opens == 0);
    assert(pw_audio_in_ps5_read(&in, grain, 0) == PW_ERR_PRECONDITION);
    pw_audio_in_ps5_close(&in);
    pw_audio_in_ps5_close(NULL);
    assert(pw_audio_in_ps5_reopen_policy(NULL) == PW_AUDIO_IN_PS5_REOPEN_SILENT);
    assert(pw_audio_in_ps5_reopen_policy("silent") == PW_AUDIO_IN_PS5_REOPEN_SILENT);
    assert(pw_audio_in_ps5_reopen_policy("plug") == PW_AUDIO_IN_PS5_REOPEN_PLUG);
    assert(pw_audio_in_ps5_reopen_policy("none") == PW_AUDIO_IN_PS5_REOPEN_NONE);
    PwAudioInPs5Ops platform;
    assert(pw_audio_in_ps5_platform_ops(NULL) == PW_ERR_PRECONDITION);
    assert(pw_audio_in_ps5_platform_ops(&platform) == PW_ERR_UNSUPPORTED && !platform.open);
}

/* 48 kHz when the system takes it, else 16 kHz voice chat; every answer logged. */
static void test_open(void)
{
    PwAudioInPs5 in;
    uint32_t rate = 0;
    reset();
    assert(pw_audio_in_ps5_init(&in, &ops, 0x1234, PW_AUDIO_IN_PS5_REOPEN_SILENT) == PW_OK);
    assert(pw_audio_in_ps5_open(&in, NULL) == PW_ERR_PRECONDITION);
    assert(pw_audio_in_ps5_open(&in, &rate) == PW_OK && rate == 48000 && opens == 1);
    assert(last_user == 0x1234 && last_type == PW_AUDIO_IN_PS5_TYPE_GENERAL && last_grain == 256 &&
           last_format == PW_AUDIO_IN_PS5_FORMAT_S16_MONO);
    assert(logged("open first user=4660 type=1 rate=48000") && logged("result=0xa"));
    assert(pw_audio_in_ps5_open(&in, &rate) == PW_ERR_PRECONDITION && opens == 1);   /* open already */
    pw_audio_in_ps5_close(&in);
    assert(closes == 1 && in.handle == -1 && logged("close reads=0"));

    reset();
    refuse_rate = 48000;
    assert(pw_audio_in_ps5_init(&in, &ops, 7, 0) == PW_OK);
    assert(pw_audio_in_ps5_open(&in, &rate) == PW_OK && rate == 16000 && opens == 2);
    assert(last_type == PW_AUDIO_IN_PS5_TYPE_VOICE_CHAT && logged("result=0x80260005"));
    pw_audio_in_ps5_close(&in);

    reset();
    refuse_all = 1;
    assert(pw_audio_in_ps5_init(&in, &ops, 7, 0) == PW_OK);
    assert(pw_audio_in_ps5_open(&in, &rate) == PW_ERR_STATE && opens == PW_AUDIO_IN_PS5_ATTEMPTS);
    assert(!in.opened);
    pw_audio_in_ps5_close(&in);
    assert(closes == 0);
}

static void test_read_and_silent_state(void)
{
    PwAudioInPs5 in;
    uint32_t rate;
    int16_t grain[PW_AUDIO_IN_PS5_GRAIN];
    reset();
    assert(pw_audio_in_ps5_init(&in, &ops, 7, PW_AUDIO_IN_PS5_REOPEN_NONE) == PW_OK);
    assert(pw_audio_in_ps5_open(&in, &rate) == PW_OK);
    assert(pw_audio_in_ps5_read(&in, NULL, 0) == PW_ERR_PRECONDITION);
    assert(pw_audio_in_ps5_read(&in, grain, 0) == PW_OK && grain[0] == 1000 && grain[255] == 1255);
    /* the state is read every PW_AUDIO_IN_PS5_CHECK_GRAINS grains, logged on a change */
    for (int i = 1; i < PW_AUDIO_IN_PS5_CHECK_GRAINS; i++) assert(pw_audio_in_ps5_read(&in, grain, 0) == PW_OK);
    assert(states == 1 && in.silent == 0 && logged("silent=0 reads=47"));
    silent_value = PW_AUDIO_IN_PS5_SILENT_USER_SETTING;
    for (int i = 0; i < PW_AUDIO_IN_PS5_CHECK_GRAINS; i++) assert(pw_audio_in_ps5_read(&in, grain, 0) == PW_OK);
    assert(states == 2 && logged("silent=0x4"));
    /* no device, but the policy is none: no reopen */
    silent_value = PW_AUDIO_IN_PS5_SILENT_DEVICE_NONE;
    for (int i = 0; i < PW_AUDIO_IN_PS5_CHECK_GRAINS; i++) assert(pw_audio_in_ps5_read(&in, grain, 0) == PW_OK);
    assert(opens == 1 && in.reopens == 0);
    /* a failed read is an error for the caller, logged once */
    input_fails = 1;
    assert(pw_audio_in_ps5_read(&in, grain, 0) == PW_ERR_STATE);
    assert(pw_audio_in_ps5_read(&in, grain, 0) == PW_ERR_STATE);
    assert(in.failures == 2 && logged("input result=0x80260009 failures=1") && !logged("failures=2"));
    pw_audio_in_ps5_close(&in);
    assert(open_handles == 0);
}

/* The default policy reopens once the handle reports its device gone. */
static void test_reopen_silent(void)
{
    PwAudioInPs5 in;
    uint32_t rate;
    int16_t grain[PW_AUDIO_IN_PS5_GRAIN];
    reset();
    refuse_rate = 48000;
    assert(pw_audio_in_ps5_init(&in, &ops, 7, PW_AUDIO_IN_PS5_REOPEN_SILENT) == PW_OK);
    assert(pw_audio_in_ps5_open(&in, &rate) == PW_OK && rate == 16000 && in.handle == 10);
    /* a first state of no device is not a loss: nothing was there */
    silent_value = PW_AUDIO_IN_PS5_SILENT_DEVICE_NONE;
    for (int i = 0; i < PW_AUDIO_IN_PS5_CHECK_GRAINS; i++) assert(pw_audio_in_ps5_read(&in, grain, 0) == PW_OK);
    assert(in.reopens == 0);
    silent_value = 0;
    for (int i = 0; i < PW_AUDIO_IN_PS5_CHECK_GRAINS; i++) assert(pw_audio_in_ps5_read(&in, grain, 0) == PW_OK);
    silent_value = PW_AUDIO_IN_PS5_SILENT_DEVICE_NONE;
    for (int i = 0; i < PW_AUDIO_IN_PS5_CHECK_GRAINS; i++) assert(pw_audio_in_ps5_read(&in, grain, 0) == PW_OK);
    /* the same type and rate as before: the reader's rate does not change */
    assert(in.reopens == 1 && in.handle == 11 && closes == 1 && last_rate == 16000 &&
           last_type == PW_AUDIO_IN_PS5_TYPE_VOICE_CHAT && logged("open silent"));
    /* still no device: no second reopen until it comes back and goes again */
    for (int i = 0; i < 2 * PW_AUDIO_IN_PS5_CHECK_GRAINS; i++) pw_audio_in_ps5_read(&in, grain, 0);
    assert(in.reopens == 1);
    silent_value = 0;
    for (int i = 0; i < PW_AUDIO_IN_PS5_CHECK_GRAINS; i++) pw_audio_in_ps5_read(&in, grain, 0);
    silent_value = PW_AUDIO_IN_PS5_SILENT_DEVICE_NONE;
    for (int i = 0; i < PW_AUDIO_IN_PS5_CHECK_GRAINS; i++) pw_audio_in_ps5_read(&in, grain, 0);
    assert(in.reopens == 2 && in.handle == 12);
    /* a reopen that fails: reads fail (silence) until a later check opens it */
    silent_value = 0;
    for (int i = 0; i < PW_AUDIO_IN_PS5_CHECK_GRAINS; i++) pw_audio_in_ps5_read(&in, grain, 0);
    silent_value = PW_AUDIO_IN_PS5_SILENT_DEVICE_NONE;
    refuse_all = 1;
    for (int i = 0; i < PW_AUDIO_IN_PS5_CHECK_GRAINS; i++) pw_audio_in_ps5_read(&in, grain, 0);
    assert(in.handle == -1 && in.opened);
    int before = inputs;
    assert(pw_audio_in_ps5_read(&in, grain, 0) == PW_ERR_STATE && inputs == before);
    refuse_all = 0;
    for (int i = 0; i < PW_AUDIO_IN_PS5_CHECK_GRAINS; i++) pw_audio_in_ps5_read(&in, grain, 0);
    assert(in.handle >= 10 && logged("open retry"));
    assert(pw_audio_in_ps5_read(&in, grain, 0) == PW_OK);
    pw_audio_in_ps5_close(&in);
    assert(open_handles == 0);
}

/* The plug policy also reopens when the caller's epoch moves. */
static void test_reopen_plug(void)
{
    PwAudioInPs5 in;
    uint32_t rate;
    int16_t grain[PW_AUDIO_IN_PS5_GRAIN];
    reset();
    assert(pw_audio_in_ps5_init(&in, &ops, 7, PW_AUDIO_IN_PS5_REOPEN_PLUG) == PW_OK);
    assert(pw_audio_in_ps5_open(&in, &rate) == PW_OK);
    /* the first read takes the epoch as it is */
    assert(pw_audio_in_ps5_read(&in, grain, 5) == PW_OK && in.reopens == 0);
    assert(pw_audio_in_ps5_read(&in, grain, 5) == PW_OK && in.reopens == 0);
    assert(pw_audio_in_ps5_read(&in, grain, 6) == PW_OK && in.reopens == 1 && in.handle == 11);
    assert(grain[0] == 1100 && logged("open plug"));
    pw_audio_in_ps5_close(&in);
    /* the silent policy ignores the epoch */
    reset();
    assert(pw_audio_in_ps5_init(&in, &ops, 7, PW_AUDIO_IN_PS5_REOPEN_SILENT) == PW_OK);
    assert(pw_audio_in_ps5_open(&in, &rate) == PW_OK);
    assert(pw_audio_in_ps5_read(&in, grain, 1) == PW_OK && pw_audio_in_ps5_read(&in, grain, 2) == PW_OK);
    assert(in.reopens == 0);
    pw_audio_in_ps5_close(&in);
    /* and a reopen after close and open starts from the new epoch */
    assert(pw_audio_in_ps5_open(&in, &rate) == PW_OK);
    assert(pw_audio_in_ps5_read(&in, grain, 9) == PW_OK && in.reopens == 0);
    pw_audio_in_ps5_close(&in);
    assert(open_handles == 0);
}

int main(void)
{
    test_init();
    test_open();
    test_read_and_silent_state();
    test_reopen_silent();
    test_reopen_plug();
    puts("audio in passed");
    return 0;
}
