/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_AUDIO_IN_PS5_H
#define PW_AUDIO_IN_PS5_H
/*
 * The console's microphone for Wine's capture endpoint (wine/wineps5): the
 * foreground user's, which the system takes from a headset when one with a
 * microphone is plugged in, else from the controller. Opened only when a
 * game starts capturing, read one grain of 16-bit mono at a time (the read
 * blocks), closed when the game stops.
 *
 * libSceAudioIn's types, formats and silent-state bits come from shadPS4,
 * which documents the PS4; none is verified on PS5. Open tries the
 * attempts below in order and logs every answer, so a console log shows
 * what the system took. The silent state is read every
 * PW_AUDIO_IN_PS5_CHECK_GRAINS grains and logged when it changes.
 *
 * Whether an open handle follows a headset plugged in or pulled out is a
 * console question. Two reopen policies cover the answer:
 *   PW_AUDIO_IN_PS5_REOPEN_SILENT  (default) reopen when the silent state
 *       goes from a device to no device: a handle that follows the plug
 *       never does while a controller is connected, so this costs nothing
 *       then. It reopens once per loss, not again while none comes back.
 *   PW_AUDIO_IN_PS5_REOPEN_PLUG    also reopen when the caller's plug epoch
 *       changes (the output's port state changed: a headset came or went).
 *   PW_AUDIO_IN_PS5_REOPEN_NONE    never.
 * A reopen keeps the type and rate that opened, so the reader's rate does
 * not change; while it fails, reads fail and the reader plays silence.
 */
#include "../include/prospero_win.h"
#include <stddef.h>
#include <stdint.h>

enum {
    PW_AUDIO_IN_PS5_GRAIN = 256,
    PW_AUDIO_IN_PS5_TYPE_VOICE_CHAT = 0, PW_AUDIO_IN_PS5_TYPE_GENERAL = 1,
    PW_AUDIO_IN_PS5_TYPE_VOICE_RECOGNITION = 5,
    PW_AUDIO_IN_PS5_FORMAT_S16_MONO = 0, PW_AUDIO_IN_PS5_FORMAT_S16_STEREO = 2,
    PW_AUDIO_IN_PS5_SILENT_DEVICE_NONE = 0x1, PW_AUDIO_IN_PS5_SILENT_PRIORITY_LOW = 0x2,
    PW_AUDIO_IN_PS5_SILENT_USER_SETTING = 0x4, PW_AUDIO_IN_PS5_SILENT_UNABLE_FORMAT = 0x8,
    PW_AUDIO_IN_PS5_CHECK_GRAINS = 48,
    PW_AUDIO_IN_PS5_REOPEN_NONE = 0, PW_AUDIO_IN_PS5_REOPEN_SILENT = 1, PW_AUDIO_IN_PS5_REOPEN_PLUG = 2,
    PW_AUDIO_IN_PS5_ATTEMPTS = 3,
};

/* What open tries, in order: 48 kHz if the system takes it, else 16 kHz. */
typedef struct PwAudioInPs5Attempt { int type; uint32_t rate; } PwAudioInPs5Attempt;
extern const PwAudioInPs5Attempt pw_audio_in_ps5_attempts[PW_AUDIO_IN_PS5_ATTEMPTS];

typedef struct PwAudioInPs5Ops {
    int (*open)(int user_id, int type, int index, uint32_t grain, uint32_t rate, uint32_t format);
    int (*input)(int handle, void *samples);    /* blocks for a grain: frames read, or an error */
    int (*silent_state)(int handle);            /* the bits, or an error */
    int (*close)(int handle);
    void (*log)(const char *line);
} PwAudioInPs5Ops;

typedef struct PwAudioInPs5 {
    PwAudioInPs5Ops ops;
    int user_id, reopen;
    int handle;                 /* -1 while closed */
    int opened;                 /* 1 between open and close, even while a reopen fails */
    int type;
    uint32_t rate;
    int silent;                 /* the last silent state read, or -1 */
    uint32_t since_check, epoch;
    int epoch_set;              /* the first read after an open takes the caller's epoch */
    uint32_t reads, failures, reopens;
} PwAudioInPs5;

/* How a title reaches libSceAudioIn when its imports were left unbound:
 * load a module by name and look a symbol up in it. */
typedef struct PwAudioInPs5Loader {
    int32_t (*load)(const char *path, int *result);   /* a module handle, or a negative error */
    int (*resolve)(int32_t handle, const char *symbol, void **address);   /* 0 when found */
} PwAudioInPs5Loader;
/* The paths load is tried with, in order. */
enum { PW_AUDIO_IN_PS5_PATHS = 2 };
extern const char *const pw_audio_in_ps5_paths[PW_AUDIO_IN_PS5_PATHS];

/* The four calls from the first path that loads and has them all: PW_OK, or
 * PW_ERR_NOT_FOUND. how (may be NULL) says what each path answered, for the
 * title's log: "dlsym path=<p> handle=<h>", or "none load=<status>,..." with
 * the first missing symbol of a path that loaded. */
int pw_audio_in_ps5_resolve(PwAudioInPs5Ops *ops, const PwAudioInPs5Loader *loader, char *how, size_t how_size);
/* The console's calls: the title's imports when the firmware bound them
 * ("direct"), else pw_audio_in_ps5_resolve through the kernel. A console
 * firmware was measured leaving weak imports at 0 although the title lists
 * libSceAudioIn as needed, so the lookup is the usual way. PW_ERR_NOT_FOUND
 * when neither reached the calls, PW_ERR_UNSUPPORTED on the host. how as
 * for pw_audio_in_ps5_resolve. */
int pw_audio_in_ps5_platform_ops(PwAudioInPs5Ops *ops, char *how, size_t how_size);
/* The policy an environment value names: "plug", "none", else silent. */
int pw_audio_in_ps5_reopen_policy(const char *value);
/* A closed microphone of user_id. */
int pw_audio_in_ps5_init(PwAudioInPs5 *in, const PwAudioInPs5Ops *ops, int user_id, int reopen);
/* Opens it: PW_OK and the rate of its grains, PW_ERR_STATE when no attempt
 * opened, PW_ERR_PRECONDITION when it is open already. */
int pw_audio_in_ps5_open(PwAudioInPs5 *in, uint32_t *rate);
/* One grain of PW_AUDIO_IN_PS5_GRAIN mono frames: PW_OK, or PW_ERR_STATE
 * when the system gave none (the caller plays silence). frames is cleared
 * first, so frames the system did not fill are silence. epoch: a number the
 * caller changes when a headset may have come or gone. */
int pw_audio_in_ps5_read(PwAudioInPs5 *in, int16_t *frames, uint32_t epoch);
void pw_audio_in_ps5_close(PwAudioInPs5 *in);

#endif
