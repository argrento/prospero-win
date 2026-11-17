/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_audio_in_ps5.h"
#include <stdio.h>
#include <string.h>

const PwAudioInPs5Attempt pw_audio_in_ps5_attempts[PW_AUDIO_IN_PS5_ATTEMPTS] = {
    { PW_AUDIO_IN_PS5_TYPE_GENERAL, 48000 },
    { PW_AUDIO_IN_PS5_TYPE_VOICE_CHAT, 16000 },
    { PW_AUDIO_IN_PS5_TYPE_GENERAL, 16000 },
};

#ifndef PW_AUDIO_IN_PS5_HOST_TEST
/* Link-only imports (native/stubs/libSceAudioIn.c); the system's module
 * provides them. */
extern int sceAudioInOpen(int, int, int, uint32_t, uint32_t, uint32_t);
extern int sceAudioInInput(int, void *);
extern int sceAudioInGetSilentState(int);
extern int sceAudioInClose(int);
static int platform_open(int u, int t, int i, uint32_t g, uint32_t r, uint32_t f)
{ return sceAudioInOpen(u, t, i, g, r, f); }
static int platform_input(int h, void *p) { return sceAudioInInput(h, p); }
static int platform_silent(int h) { return sceAudioInGetSilentState(h); }
static int platform_close(int h) { return sceAudioInClose(h); }
#endif

int pw_audio_in_ps5_platform_ops(PwAudioInPs5Ops *ops)
{
    if (!ops) return PW_ERR_PRECONDITION;
    memset(ops, 0, sizeof(*ops));
#ifdef PW_AUDIO_IN_PS5_HOST_TEST
    return PW_ERR_UNSUPPORTED;
#else
    ops->open = platform_open;
    ops->input = platform_input;
    ops->silent_state = platform_silent;
    ops->close = platform_close;
    return PW_OK;
#endif
}

int pw_audio_in_ps5_reopen_policy(const char *value)
{
    if (value && !strcmp(value, "plug")) return PW_AUDIO_IN_PS5_REOPEN_PLUG;
    if (value && !strcmp(value, "none")) return PW_AUDIO_IN_PS5_REOPEN_NONE;
    return PW_AUDIO_IN_PS5_REOPEN_SILENT;
}

static void say(const PwAudioInPs5 *in, const char *line)
{
    if (in->ops.log) in->ops.log(line);
}

int pw_audio_in_ps5_init(PwAudioInPs5 *in, const PwAudioInPs5Ops *ops, int user_id, int reopen)
{
    if (!in) return PW_ERR_PRECONDITION;
    memset(in, 0, sizeof(*in));
    in->handle = -1;
    in->silent = -1;
    if (!ops || !ops->open || !ops->input || !ops->silent_state || !ops->close || user_id < 0)
        return PW_ERR_PRECONDITION;
    in->ops = *ops;
    in->user_id = user_id;
    in->reopen = reopen;
    return PW_OK;
}

static int open_as(PwAudioInPs5 *in, int type, uint32_t rate, const char *why)
{
    char line[160];

    /* logged before the call, so a fault inside it can be told apart */
    snprintf(line, sizeof(line), "open %s user=%d type=%d rate=%u grain=%u", why, in->user_id, type,
             (unsigned)rate, (unsigned)PW_AUDIO_IN_PS5_GRAIN);
    say(in, line);
    int handle = in->ops.open(in->user_id, type, 0, PW_AUDIO_IN_PS5_GRAIN, rate, PW_AUDIO_IN_PS5_FORMAT_S16_MONO);
    snprintf(line, sizeof(line), "open %s result=%#x", why, (unsigned)handle);
    say(in, line);
    return handle;
}

int pw_audio_in_ps5_open(PwAudioInPs5 *in, uint32_t *rate)
{
    if (!in || !rate || !in->ops.open || in->opened) return PW_ERR_PRECONDITION;
    for (size_t i = 0; i < PW_AUDIO_IN_PS5_ATTEMPTS; i++) {
        const PwAudioInPs5Attempt *attempt = &pw_audio_in_ps5_attempts[i];
        int handle = open_as(in, attempt->type, attempt->rate, "first");

        if (handle < 0) continue;
        in->handle = handle;
        in->opened = 1;
        in->type = attempt->type;
        in->rate = attempt->rate;
        in->silent = -1;
        in->since_check = 0;
        in->epoch_set = 0;
        *rate = attempt->rate;
        return PW_OK;
    }
    return PW_ERR_STATE;
}

static void reopen(PwAudioInPs5 *in, const char *why)
{
    if (in->handle >= 0) (void)in->ops.close(in->handle);
    in->handle = open_as(in, in->type, in->rate, why);
    if (in->handle < 0) in->handle = -1;
    in->reopens++;
}

/* Every PW_AUDIO_IN_PS5_CHECK_GRAINS reads: the silent state, logged when
 * it changes; no device newly reported reopens under the silent policy. */
static void check(PwAudioInPs5 *in)
{
    char line[96];
    int state;

    if (++in->since_check < PW_AUDIO_IN_PS5_CHECK_GRAINS) return;
    in->since_check = 0;
    if (in->handle < 0) {
        /* a reopen that failed is tried again */
        reopen(in, "retry");
        return;
    }
    state = in->ops.silent_state(in->handle);
    if (state == in->silent) return;
    snprintf(line, sizeof(line), "silent=%#x reads=%u failures=%u", (unsigned)state, (unsigned)in->reads,
             (unsigned)in->failures);
    say(in, line);
    /* a device it had is gone; a state never read before (-1) is not a loss */
    int lost = state >= 0 && (state & PW_AUDIO_IN_PS5_SILENT_DEVICE_NONE) &&
               in->silent >= 0 && !(in->silent & PW_AUDIO_IN_PS5_SILENT_DEVICE_NONE);
    in->silent = state;
    if (lost && in->reopen != PW_AUDIO_IN_PS5_REOPEN_NONE) reopen(in, "silent");
}

int pw_audio_in_ps5_read(PwAudioInPs5 *in, int16_t *frames, uint32_t epoch)
{
    char line[96];

    if (!in || !frames || !in->opened) return PW_ERR_PRECONDITION;
    if (!in->epoch_set) in->epoch = epoch, in->epoch_set = 1;
    if (epoch != in->epoch) {
        in->epoch = epoch;
        if (in->reopen == PW_AUDIO_IN_PS5_REOPEN_PLUG) reopen(in, "plug");
    }
    int status = in->handle >= 0 ? in->ops.input(in->handle, frames) : -1;
    check(in);
    if (status < 0) {
        /* the first failure, then one line a thousand */
        if (!(in->failures++ % 1000)) {
            snprintf(line, sizeof(line), "input result=%#x failures=%u", (unsigned)status, (unsigned)in->failures);
            say(in, line);
        }
        return PW_ERR_STATE;
    }
    in->reads++;
    return PW_OK;
}

void pw_audio_in_ps5_close(PwAudioInPs5 *in)
{
    char line[96];

    if (!in || !in->opened) return;
    if (in->handle >= 0) (void)in->ops.close(in->handle);
    snprintf(line, sizeof(line), "close reads=%u failures=%u reopens=%u", (unsigned)in->reads,
             (unsigned)in->failures, (unsigned)in->reopens);
    say(in, line);
    in->handle = -1;
    in->opened = 0;
}
