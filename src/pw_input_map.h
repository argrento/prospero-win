/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_INPUT_MAP_H
#define PW_INPUT_MAP_H
/*
 * The input map (docs/INPUT_MAP.md): one tick of the DualSense, as the game
 * should see it, turned into what a profile's map sends. Out comes the
 * game's controller, in the DualSense's own form so the title's controller
 * code turns it into an XInput pad or a DualShock 4 as it does without the
 * map, and the keys, mouse buttons, pointer motion and wheel notches, in
 * the order the game gets them. A mapped input never reaches the
 * controller: the controller has only what the map sends to it.
 *
 * Nothing here reads the pad or posts input; the title does both. The map
 * is the first player's.
 */
#include "pw_game_profile.h"
#include <stddef.h>
#include <stdint.h>

/* Event types, numbered as Wine's input queue numbers them. */
enum { PW_INPUT_MAP_KEY = 1, PW_INPUT_MAP_MOUSE_MOVE, PW_INPUT_MAP_MOUSE_BUTTON, PW_INPUT_MAP_MOUSE_WHEEL };
/* The most events a tick or a release writes: every input's combination let
 * go, pressed and let go again, each stick's four keys, a move and a wheel. */
enum { PW_INPUT_MAP_EVENT_MAX = PW_GAME_MAP_INPUTS * 3 * PW_GAME_MAP_COMBO + 2 * 2 * 4 + 2 };
/* Percent below a threshold or a dead zone at which it lets go again. */
enum { PW_INPUT_MAP_HYSTERESIS = 5 };

typedef struct PwInputMapEvent {
    uint32_t type;      /* PW_INPUT_MAP_* */
    uint32_t code;      /* KEY: Windows virtual key; MOUSE_BUTTON: 0 left, 1 right, 2 middle, 3 X1, 4 X2 */
    int32_t x, y;       /* MOUSE_MOVE: pixels; MOUSE_WHEEL: x notches, up positive */
    uint32_t down;      /* KEY, MOUSE_BUTTON: 1 pressed, 0 let go */
} PwInputMapEvent;

/* The DualSense this tick, with scePadRead's button bits. */
typedef struct PwInputMapSample {
    uint32_t held, pressed, released;
    uint8_t lx, ly, rx, ry;     /* 0..255, 128 centred, y down */
    uint8_t l2, r2;             /* 0..255 */
    int touch_left;             /* the held touchpad click began on the left half */
    unsigned touch_fingers;     /* fingers that made it: two is the console's keyboard */
    int active;                 /* connected and not taken by the system */
} PwInputMapSample;

/* The game's controller, in the DualSense's form: Cross is A, Create Back,
 * and in xinput mode the touchpad bit on the right half (touch_left 0) is
 * Guide. */
typedef struct PwInputMapPad {
    uint32_t buttons;
    int touch_left;
    uint8_t lx, ly, rx, ry, l2, r2;
} PwInputMapPad;

typedef struct PwInputMapState {
    uint32_t down;              /* inputs whose keys or mouse button are down */
    uint32_t latched;           /* inputs kept from the game until let go */
    uint32_t trigger_on;        /* bit 0 l2, bit 1 r2: past the threshold */
    uint8_t dir_on[2], dir_latched[2];  /* per stick: 1 up, 2 down, 4 left, 8 right */
    int64_t pointer_x, pointer_y, wheel;    /* not yet sent, in 1/65536 */
} PwInputMapState;

void pw_input_map_init(PwInputMapState *state);
/* One tick of tick_us. pad gets the game's controller; out gets the events
 * (room for PW_INPUT_MAP_EVENT_MAX; fewer are written when max is smaller),
 * and the count is returned. A sample that is not active lets go of
 * everything and clears what the pointer and wheel have left over. */
size_t pw_input_map_tick(PwInputMapState *state, const PwGameMap *map, PwGameInputMode mode,
                         const PwInputMapSample *sample, uint32_t tick_us, PwInputMapPad *pad,
                         PwInputMapEvent *out, size_t max);
/* Let go of everything the map holds: keys and mouse buttons come up, and
 * what the pointer and wheel have left over is cleared. Every input still
 * held in sample (the real pad, may be NULL) stays away from the game until
 * it is let go. For a preset swap, a lost pad, and while the menu or the
 * console's keyboard has the pad. Returns the events written. */
size_t pw_input_map_release(PwInputMapState *state, const PwGameMap *map, PwGameInputMode mode,
                            const PwInputMapSample *sample, PwInputMapEvent *out, size_t max);
/* How far a stick axis (deflection -128..127) moves in elapsed_us, in
 * 1/65536 units, at up to speed units a second at full tilt: nothing at or
 * below deadzone, full from 127 - outer, scaled linearly between, then
 * curved (PwGameCurve). Both in raw units of 127. */
int64_t pw_input_map_travel(int32_t axis, uint32_t deadzone, uint32_t outer, int curve, uint32_t speed,
                            uint32_t elapsed_us);
#endif
