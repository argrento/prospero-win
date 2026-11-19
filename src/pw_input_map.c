/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_input_map.h"
#include <string.h>

/* scePadRead's bits for the inputs, in PwGameMapInput order; the touchpad
 * halves share the click's bit. */
enum { PAD_TOUCHPAD = 0x100000u, PAD_L2 = 0x100u, PAD_R2 = 0x200u };
static const uint32_t input_bits[PW_GAME_MAP_INPUTS] = {
    0x4000u, 0x2000u, 0x8000u, 0x1000u, 0x400u, 0x800u, PAD_L2, PAD_R2, 0x2u, 0x4u,
    0x10u, 0x40u, 0x80u, 0x20u, 0x8u, 0x1u, PAD_TOUCHPAD, PAD_TOUCHPAD,
};
/* The controller's buttons in the DualSense's form, by PwGameMapPad; 0 for
 * the triggers, Guide and the touchpad, which depend on more. */
static const uint32_t pad_bits[PW_GAME_PAD_COUNT] = {
    [PW_GAME_PAD_A] = 0x4000u, [PW_GAME_PAD_B] = 0x2000u, [PW_GAME_PAD_X] = 0x8000u, [PW_GAME_PAD_Y] = 0x1000u,
    [PW_GAME_PAD_LB] = 0x400u, [PW_GAME_PAD_RB] = 0x800u, [PW_GAME_PAD_LS] = 0x2u, [PW_GAME_PAD_RS] = 0x4u,
    [PW_GAME_PAD_DPAD_UP] = 0x10u, [PW_GAME_PAD_DPAD_DOWN] = 0x40u, [PW_GAME_PAD_DPAD_LEFT] = 0x80u,
    [PW_GAME_PAD_DPAD_RIGHT] = 0x20u, [PW_GAME_PAD_START] = 0x8u, [PW_GAME_PAD_BACK] = 0x1u,
};
enum { DIR_UP = 1, DIR_DOWN = 2, DIR_LEFT = 4, DIR_RIGHT = 8 };
/* the d-pad bits of a stick's directions, in DIR order */
static const uint32_t dir_pad_bits[4] = { 0x10u, 0x40u, 0x80u, 0x20u };

void pw_input_map_init(PwInputMapState *state)
{
    if (state) memset(state, 0, sizeof(*state));
}

static int64_t curved(int64_t t, int curve)
{
    if (curve == PW_GAME_CURVE_SQUARED) return t * t >> 16;
    if (curve == PW_GAME_CURVE_CUBED) return (t * t >> 16) * t >> 16;
    return t;
}

/* A size past the dead zone, scaled to 0..65536 and curved. */
static int64_t shaped(uint32_t size, uint32_t deadzone, uint32_t outer, int curve)
{
    int64_t t;

    if (size <= deadzone) return 0;
    t = (int64_t)(size - deadzone) * 65536 / (int64_t)(127 - outer - deadzone);
    if (t > 65536) t = 65536;
    return curved(t, curve);
}

int64_t pw_input_map_travel(int32_t axis, uint32_t deadzone, uint32_t outer, int curve, uint32_t speed,
                            uint32_t elapsed_us)
{
    uint32_t magnitude = (uint32_t)(axis < 0 ? -axis : axis);
    int64_t v;

    if (deadzone + outer >= 127) return 0;
    v = shaped(magnitude, deadzone, outer, curve);
    v = v * speed * elapsed_us / 1000000;   /* < 2^63 for speed <= 20000 */
    return axis < 0 ? -v : v;
}

static uint32_t root(uint32_t n)
{
    uint32_t r = 0;

    while ((r + 1) * (r + 1) <= n) r++;
    return r;
}

static int32_t clamp_axis(int64_t v)
{
    return v < -128 ? -128 : v > 127 ? 127 : (int32_t)v;
}

static uint32_t raw(uint32_t percent) { return percent * 127u / 100u; }

/* A stick to a controller stick: each axis as it is with no dead zone, outer
 * zone or curve; otherwise its distance from the centre shaped, in the same
 * direction. Then inverted. */
static void stick_to_pad(const PwGameMapStick *stick, int32_t *dx, int32_t *dy)
{
    if (stick->deadzone || stick->outer || stick->curve != PW_GAME_CURVE_LINEAR) {
        uint32_t size = root((uint32_t)(*dx * *dx + *dy * *dy));
        int64_t length = shaped(size, raw(stick->deadzone), raw(stick->outer), stick->curve) * 127;

        if (!size || !length) {
            *dx = *dy = 0;
        } else {
            *dx = clamp_axis((int64_t)*dx * length / ((int64_t)size << 16));
            *dy = clamp_axis((int64_t)*dy * length / ((int64_t)size << 16));
        }
    }
    if (stick->invert_x) *dx = clamp_axis(-(int64_t)*dx);
    if (stick->invert_y) *dy = clamp_axis(-(int64_t)*dy);
}

/* The directions a stick holds past its dead zone, each axis on its own:
 * one comes on past the dead zone and goes off at 5 points below it. */
static uint8_t stick_dirs(const PwGameMapStick *stick, int32_t dx, int32_t dy, uint8_t was)
{
    const uint32_t on = raw(stick->deadzone),
                   off = raw(stick->deadzone > PW_INPUT_MAP_HYSTERESIS ? stick->deadzone - PW_INPUT_MAP_HYSTERESIS : 0);
    uint8_t dirs = 0;

    if (stick->invert_x) dx = -dx;
    if (stick->invert_y) dy = -dy;
    for (int d = 0; d < 4; d++) {
        const int32_t amount = d == 0 ? -dy : d == 1 ? dy : d == 2 ? -dx : dx;
        const uint32_t past = amount > 0 ? (uint32_t)amount : 0;
        if (was & (1u << d) ? past > off : past > on) dirs |= (uint8_t)(1u << d);
    }
    return dirs;
}

static size_t put(PwInputMapEvent *out, size_t used, size_t max, uint32_t type, uint32_t code, int32_t x,
                  uint32_t down)
{
    if (used >= max) return used;
    out[used] = (PwInputMapEvent){ type, code, x, 0, down };
    return used + 1;
}

/* A key or mouse-button target's press (modifiers first) or release (in the
 * opposite order). Wheel notches come with a press only. */
static size_t send(const PwGameMapTarget *target, int down, PwInputMapEvent *out, size_t used, size_t max)
{
    if (target->kind == PW_GAME_MAP_KEYS) {
        for (size_t k = 0; k < target->count; k++)
            used = put(out, used, max, PW_INPUT_MAP_KEY, target->keys[down ? k : target->count - 1 - k], 0,
                       (uint32_t)down);
    } else if (target->kind == PW_GAME_MAP_MOUSE) {
        if (target->code == PW_GAME_MOUSE_WHEEL_UP || target->code == PW_GAME_MOUSE_WHEEL_DOWN) {
            if (down)
                used = put(out, used, max, PW_INPUT_MAP_MOUSE_WHEEL, 0,
                           target->code == PW_GAME_MOUSE_WHEEL_UP ? 1 : -1, 0);
        } else {
            used = put(out, used, max, PW_INPUT_MAP_MOUSE_BUTTON, target->code, 0, (uint32_t)down);
        }
    }
    return used;
}

static int sends_events(const PwGameMapTarget *target)
{
    return target->kind == PW_GAME_MAP_KEYS || target->kind == PW_GAME_MAP_MOUSE;
}

static int holds(const PwGameMapTarget *target)
{
    return target->kind == PW_GAME_MAP_KEYS ||
           (target->kind == PW_GAME_MAP_MOUSE && target->code <= PW_GAME_MOUSE_FORWARD);
}

static int analog_trigger(const PwGameMapTarget *target, size_t input)
{
    return (input == PW_GAME_MAP_L2 || input == PW_GAME_MAP_R2) && target->kind == PW_GAME_MAP_PAD &&
           (target->code == PW_GAME_PAD_LT || target->code == PW_GAME_PAD_RT);
}

/* A trigger with a threshold works as a button that goes down at the
 * threshold and comes up 5 points below it. */
static int trigger_past(uint8_t value, uint8_t threshold, int was)
{
    return was ? value > (threshold - PW_INPUT_MAP_HYSTERESIS) * 255u / 100u : value >= threshold * 255u / 100u;
}

/* Which inputs the sample holds, presses and lets go, with the touchpad
 * click split by its half, a two-finger click (the console's keyboard)
 * left out, and thresholds applied. */
static void edges(PwInputMapState *state, const PwGameMap *map, const PwInputMapSample *sample, uint32_t *held,
                  uint32_t *pressed, uint32_t *released, int update)
{
    const int touch = sample->touch_fingers < 2;
    uint32_t trigger_on = state->trigger_on;

    *held = *pressed = *released = 0;
    for (size_t i = 0; i < PW_GAME_MAP_INPUTS; i++) {
        const uint32_t bit = input_bits[i], me = 1u << i;
        int h = (sample->held & bit) != 0, p = (sample->pressed & bit) != 0, r = (sample->released & bit) != 0;

        if (bit == PAD_TOUCHPAD) {
            const int left = i == PW_GAME_MAP_TOUCHPAD_LEFT, here = (sample->touch_left != 0) == left;
            h = h && touch && here;
            p = p && touch && here;
        }
        if ((i == PW_GAME_MAP_L2 || i == PW_GAME_MAP_R2) && map->threshold[i - PW_GAME_MAP_L2]) {
            const size_t t = i - PW_GAME_MAP_L2;
            const int was = (trigger_on >> t) & 1u;
            int now = sample->active && trigger_past(t ? sample->r2 : sample->l2, map->threshold[t], was);
            h = now;
            p = now && !was;
            r = !now && was;
            trigger_on = now ? trigger_on | (1u << t) : trigger_on & ~(1u << t);
        }
        if (!sample->active) h = p = 0;
        if (h) *held |= me;
        if (p) *pressed |= me;
        if (r) *released |= me;
    }
    if (update) state->trigger_on = trigger_on;
}

static void stick_axes(const PwInputMapSample *sample, size_t s, int32_t *dx, int32_t *dy)
{
    *dx = sample->active ? (int32_t)(s ? sample->rx : sample->lx) - 128 : 0;
    *dy = sample->active ? (int32_t)(s ? sample->ry : sample->ly) - 128 : 0;
}

size_t pw_input_map_release(PwInputMapState *state, const PwGameMap *map, PwGameInputMode mode,
                            const PwInputMapSample *sample, PwInputMapEvent *out, size_t max)
{
    size_t used = 0;

    if (!state || !map || !out) return 0;
    for (size_t i = 0; i < PW_GAME_MAP_INPUTS; i++) {
        PwGameMapTarget target = pw_game_map_target(map, i, mode);
        if (!(state->down & (1u << i))) continue;
        used = send(&target, 0, out, used, max);
    }
    for (size_t s = 0; s < PW_GAME_MAP_STICKS; s++) {
        const PwGameMapStick *stick = &map->stick[s];
        for (int d = 0; stick->mode == PW_GAME_STICK_MODE_KEYS && d < 4; d++)
            if ((state->dir_on[s] & (1u << d)) && stick->keys[d])
                used = put(out, used, max, PW_INPUT_MAP_KEY, stick->keys[d], 0, 0);
        state->dir_latched[s] |= state->dir_on[s];
        state->dir_on[s] = 0;
    }
    state->latched |= state->down;
    state->down = 0;
    state->pointer_x = state->pointer_y = state->wheel = 0;
    if (sample && sample->active) {
        uint32_t held, pressed, released;
        PwInputMapState probe = *state;

        /* what the real pad holds now: its triggers judged as if they were down */
        probe.trigger_on = 3u;
        edges(&probe, map, sample, &held, &pressed, &released, 0);
        state->latched |= held;
        state->trigger_on |= held >> PW_GAME_MAP_L2 & 3u;
        for (size_t s = 0; s < PW_GAME_MAP_STICKS; s++) {
            const PwGameMapStick *stick = &map->stick[s];
            int32_t dx, dy;
            if (stick->mode != PW_GAME_STICK_MODE_KEYS && stick->mode != PW_GAME_STICK_MODE_DPAD) continue;
            stick_axes(sample, s, &dx, &dy);
            state->dir_latched[s] |= stick_dirs(stick, dx, dy, 0xf);
        }
    }
    return used;
}

size_t pw_input_map_tick(PwInputMapState *state, const PwGameMap *map, PwGameInputMode mode,
                         const PwInputMapSample *sample, uint32_t tick_us, PwInputMapPad *pad,
                         PwInputMapEvent *out, size_t max)
{
    PwGameMapTarget targets[PW_GAME_MAP_INPUTS];
    uint32_t held, pressed, released;
    int32_t sticks[2][2] = { { 0, 0 }, { 0, 0 } };
    uint8_t dirs[PW_GAME_MAP_STICKS];
    size_t used = 0;

    if (!state || !map || !sample || !pad || !out) return 0;
    memset(pad, 0, sizeof(*pad));
    edges(state, map, sample, &held, &pressed, &released, 1);
    /* An input kept from the game comes back with its next press, or is
     * free once it is let go. */
    for (size_t i = 0; i < PW_GAME_MAP_INPUTS; i++) {
        const uint32_t me = 1u << i;
        targets[i] = pw_game_map_target(map, i, mode);
        if (!(state->latched & me)) continue;
        if (pressed & me || !(held & me)) state->latched &= ~me;
        if (state->latched & me) held &= ~me;
    }
    /* Sticks: their directions first, so their keys go with the buttons'. */
    for (size_t s = 0; s < PW_GAME_MAP_STICKS; s++) {
        const PwGameMapStick *stick = &map->stick[s];
        int32_t dx, dy;

        stick_axes(sample, s, &dx, &dy);
        dirs[s] = 0;
        if (stick->mode == PW_GAME_STICK_MODE_KEYS || stick->mode == PW_GAME_STICK_MODE_DPAD) {
            uint8_t now = stick_dirs(stick, dx, dy, (uint8_t)(state->dir_on[s] | state->dir_latched[s]));
            state->dir_latched[s] &= now;
            dirs[s] = now & (uint8_t)~state->dir_latched[s];
        } else if (stick->mode == PW_GAME_STICK_MODE_PAD_LEFT || stick->mode == PW_GAME_STICK_MODE_PAD_RIGHT) {
            const int to = stick->mode == PW_GAME_STICK_MODE_PAD_RIGHT;
            stick_to_pad(stick, &dx, &dy);
            sticks[to][0] += dx;
            sticks[to][1] += dy;
        } else if (stick->mode == PW_GAME_STICK_MODE_MOUSE) {
            const uint32_t deadzone = raw(stick->deadzone), outer = raw(stick->outer);
            state->pointer_x += pw_input_map_travel(stick->invert_x ? -dx : dx, deadzone, outer, stick->curve,
                                                    stick->speed, tick_us);
            state->pointer_y += pw_input_map_travel(stick->invert_y ? -dy : dy, deadzone, outer, stick->curve,
                                                    stick->speed, tick_us);
        } else if (stick->mode == PW_GAME_STICK_MODE_WHEEL) {
            /* up turns the wheel up: the stick's y grows downwards */
            state->wheel -= pw_input_map_travel(stick->invert_y ? -dy : dy, raw(stick->deadzone), raw(stick->outer),
                                                stick->curve, stick->speed, tick_us);
        }
    }
    /* What comes up, buttons then stick keys; then what goes down. */
    for (size_t i = 0; i < PW_GAME_MAP_INPUTS; i++) {
        const uint32_t me = 1u << i;
        if ((state->down & me) && ((released & me) || !(held & me))) {
            used = send(&targets[i], 0, out, used, max);
            state->down &= ~me;
        }
    }
    for (size_t s = 0; s < PW_GAME_MAP_STICKS; s++)
        for (int d = 0; map->stick[s].mode == PW_GAME_STICK_MODE_KEYS && d < 4; d++)
            if ((state->dir_on[s] & ~dirs[s] & (1u << d)) && map->stick[s].keys[d])
                used = put(out, used, max, PW_INPUT_MAP_KEY, map->stick[s].keys[d], 0, 0);
    for (size_t i = 0; i < PW_GAME_MAP_INPUTS; i++) {
        const uint32_t me = 1u << i;
        if (!(pressed & me) || (state->down & me) || !sends_events(&targets[i]) || (state->latched & me)) continue;
        used = send(&targets[i], 1, out, used, max);
        if (holds(&targets[i])) state->down |= me;
    }
    for (size_t s = 0; s < PW_GAME_MAP_STICKS; s++) {
        for (int d = 0; map->stick[s].mode == PW_GAME_STICK_MODE_KEYS && d < 4; d++)
            if ((dirs[s] & ~state->dir_on[s] & (1u << d)) && map->stick[s].keys[d])
                used = put(out, used, max, PW_INPUT_MAP_KEY, map->stick[s].keys[d], 0, 1);
        state->dir_on[s] = dirs[s];
    }
    /* A press and its release in the same batch: it comes up again now. */
    for (size_t i = 0; i < PW_GAME_MAP_INPUTS; i++) {
        const uint32_t me = 1u << i;
        if ((state->down & me) && !(held & me)) {
            used = send(&targets[i], 0, out, used, max);
            state->down &= ~me;
        }
    }
    /* The pointer and the wheel: whole pixels and notches, towards zero, so
     * both directions keep their fraction. */
    if (!sample->active) state->pointer_x = state->pointer_y = state->wheel = 0;
    {
        const int64_t dx = state->pointer_x / 65536, dy = state->pointer_y / 65536, notches = state->wheel / 65536;
        if (dx || dy) {
            state->pointer_x -= dx * 65536;
            state->pointer_y -= dy * 65536;
            if (used < max) out[used++] = (PwInputMapEvent){ PW_INPUT_MAP_MOUSE_MOVE, 0, (int32_t)dx, (int32_t)dy, 0 };
        }
        if (notches) {
            state->wheel -= notches * 65536;
            used = put(out, used, max, PW_INPUT_MAP_MOUSE_WHEEL, 0, (int32_t)notches, 0);
        }
    }
    /* The controller: what the map sends to it. */
    for (size_t i = 0; i < PW_GAME_MAP_INPUTS; i++) {
        const PwGameMapTarget *target = &targets[i];
        if (target->kind != PW_GAME_MAP_PAD) continue;
        if (analog_trigger(target, i)) {
            uint8_t pull = sample->active ? (i == PW_GAME_MAP_L2 ? sample->l2 : sample->r2) : 0;
            uint8_t *to = target->code == PW_GAME_PAD_LT ? &pad->l2 : &pad->r2;
            if (pull > *to) *to = pull;
            continue;
        }
        if (!(held & (1u << i))) continue;
        switch (target->code) {
        case PW_GAME_PAD_LT: pad->l2 = 255; break;
        case PW_GAME_PAD_RT: pad->r2 = 255; break;
        case PW_GAME_PAD_GUIDE:
            /* the right half of the touchpad is Guide in xinput mode; a
             * DualShock 4's PS button stays the console's */
            if (mode != PW_GAME_INPUT_PLAYSTATION) pad->buttons |= PAD_TOUCHPAD;
            break;
        case PW_GAME_PAD_TOUCHPAD:
            if (mode == PW_GAME_INPUT_PLAYSTATION) pad->buttons |= PAD_TOUCHPAD;
            break;
        default: pad->buttons |= pad_bits[target->code]; break;
        }
    }
    pad->touch_left = mode == PW_GAME_INPUT_PLAYSTATION ? sample->touch_left : 0;
    for (size_t s = 0; s < PW_GAME_MAP_STICKS; s++)
        if (map->stick[s].mode == PW_GAME_STICK_MODE_DPAD)
            for (int d = 0; d < 4; d++)
                if (dirs[s] & (1u << d)) pad->buttons |= dir_pad_bits[d];
    pad->lx = (uint8_t)(128 + clamp_axis(sticks[0][0]));
    pad->ly = (uint8_t)(128 + clamp_axis(sticks[0][1]));
    pad->rx = (uint8_t)(128 + clamp_axis(sticks[1][0]));
    pad->ry = (uint8_t)(128 + clamp_axis(sticks[1][1]));
    return used;
}
