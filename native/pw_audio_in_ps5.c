/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_audio_in_ps5.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

const PwAudioInPs5Attempt pw_audio_in_ps5_attempts[PW_AUDIO_IN_PS5_ATTEMPTS] = {
    { PW_AUDIO_IN_PS5_TYPE_GENERAL, 48000 },
    { PW_AUDIO_IN_PS5_TYPE_VOICE_CHAT, 16000 },
    { PW_AUDIO_IN_PS5_TYPE_GENERAL, 16000 },
};

const char *const pw_audio_in_ps5_paths[PW_AUDIO_IN_PS5_PATHS] = {
    "libSceAudioIn.sprx", "/system/common/lib/libSceAudioIn.sprx",
};

int pw_audio_in_ps5_resolve(PwAudioInPs5Ops *ops, const PwAudioInPs5Loader *loader, char *how, size_t how_size)
{
    static const char *const names[] = { "sceAudioInOpen", "sceAudioInInput", "sceAudioInGetSilentState",
                                         "sceAudioInClose" };
    size_t used = 0;
    char scratch[8];

    if (!how || !how_size) how = scratch, how_size = sizeof(scratch);
    how[0] = 0;
    if (!ops || !loader || !loader->load || !loader->resolve) return PW_ERR_PRECONDITION;
    memset(ops, 0, sizeof(*ops));
    for (size_t i = 0; i < PW_AUDIO_IN_PS5_PATHS; i++) {
        int result = 0;
        int32_t handle = loader->load(pw_audio_in_ps5_paths[i], &result);
        void *found[4] = { 0 };
        size_t missing = 4;

        if (handle >= 0)
            for (missing = 0; missing < 4; missing++)
                if (loader->resolve(handle, names[missing], &found[missing]) != 0 || !found[missing]) break;
        if (handle >= 0 && missing == 4) {
            ops->open = (int (*)(int, int, int, uint32_t, uint32_t, uint32_t))(uintptr_t)found[0];
            ops->input = (int (*)(int, void *))(uintptr_t)found[1];
            ops->silent_state = (int (*)(int))(uintptr_t)found[2];
            ops->close = (int (*)(int))(uintptr_t)found[3];
            snprintf(how, how_size, "dlsym path=%s handle=%d", pw_audio_in_ps5_paths[i], (int)handle);
            return PW_OK;
        }
        if (used < how_size) {
            int n = handle < 0 ? snprintf(how + used, how_size - used, "%s%#x", i ? "," : "none load=", (unsigned)handle)
                               : snprintf(how + used, how_size - used, "%s%d:%s", i ? "," : "none load=", (int)handle,
                                          names[missing]);
            used += n > 0 ? (size_t)n : 0;
        }
    }
    return PW_ERR_NOT_FOUND;
}

#ifndef PW_AUDIO_IN_PS5_HOST_TEST
/* Link-only imports (native/stubs/libSceAudioIn.c); the system's module
 * provides them. The firmware leaves an import it does not bind at 0, and
 * a call to it jumps to 0 (docs/WINE_PS5_BUILD.md, "Imports a title does
 * not get"). Weak declarations make the compiler read the bound address
 * instead of assuming one, so an unbound import is seen and looked up. */
extern int sceAudioInOpen(int, int, int, uint32_t, uint32_t, uint32_t) __attribute__((weak));
extern int sceAudioInInput(int, void *) __attribute__((weak));
extern int sceAudioInGetSilentState(int) __attribute__((weak));
extern int sceAudioInClose(int) __attribute__((weak));
int32_t sceKernelLoadStartModule(const char *path, size_t argc, const void *argv, uint32_t flags,
                                 const void *option, int *result);
int sceKernelDlsym(int32_t handle, const char *symbol, void **address);

static int32_t kernel_load(const char *path, int *result)
{
    return sceKernelLoadStartModule(path, 0, NULL, 0, NULL, result);
}
#endif

int pw_audio_in_ps5_platform_ops(PwAudioInPs5Ops *ops, char *how, size_t how_size)
{
    if (how && how_size) how[0] = 0;
    if (!ops) return PW_ERR_PRECONDITION;
    memset(ops, 0, sizeof(*ops));
#ifdef PW_AUDIO_IN_PS5_HOST_TEST
    return PW_ERR_UNSUPPORTED;
#else
    if (sceAudioInOpen && sceAudioInInput && sceAudioInGetSilentState && sceAudioInClose) {
        ops->open = sceAudioInOpen;
        ops->input = sceAudioInInput;
        ops->silent_state = sceAudioInGetSilentState;
        ops->close = sceAudioInClose;
        if (how && how_size) snprintf(how, how_size, "direct");
        return PW_OK;
    }
    static const PwAudioInPs5Loader kernel = { kernel_load, sceKernelDlsym };
    return pw_audio_in_ps5_resolve(ops, &kernel, how, how_size);
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
    /* a short or empty read leaves silence, not the last grain */
    memset(frames, 0, PW_AUDIO_IN_PS5_GRAIN * sizeof(*frames));
    int status = in->handle >= 0 ? in->ops.input(in->handle, frames) : -1;
    /* what the console answers on success is not measured: the first is logged */
    if (status >= 0 && !in->reads) {
        snprintf(line, sizeof(line), "input first result=%#x", (unsigned)status);
        say(in, line);
    }
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
