/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_input_map.h"
#include "../native/pw_wine_display.h"
#include "../src/pw_osk.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define APP "[application]\nid = pinball\nname = Space Cadet Pinball\n" \
            "executable = C:\\Games\\Pinball\\PINBALL.EXE\nworking_directory = C:\\Games\\Pinball\n" \
            "prefix = default\nruntime = wine-wow64\narchitecture = pe32\ngraphics = gdi\n"

enum { CROSS = 0x4000, CIRCLE = 0x2000, SQUARE = 0x8000, TRIANGLE = 0x1000, L1 = 0x400, R1 = 0x800, L2 = 0x100,
       R2 = 0x200, L3 = 0x2, R3 = 0x4, UP = 0x10, DOWN = 0x40, LEFT = 0x80, RIGHT = 0x20, OPTIONS = 0x8,
       CREATE = 0x1, TOUCHPAD = 0x100000 };
enum { TICK = 16667 };

/* A profile's [input] as the title has it: parsed, laid over the defaults,
 * and its mode settled. */
static PwGameInput mapped_input(const char *lines)
{
    char text[4096];
    PwGameProfile profile;
    PwGameInput input;

    snprintf(text, sizeof(text), APP "[input]\n%s", lines);
    if (pw_game_profile_parse((const uint8_t *)text, strlen(text), &profile) != PW_OK) {
        fprintf(stderr, "refused:\n%s\n", lines);
        assert(0);
    }
    pw_game_input_init(&input);
    pw_game_input_overlay(&input, &profile.input);
    pw_game_input_default_mode(&input);
    return input;
}

static PwInputMapSample sample(uint32_t held, uint32_t was)
{
    PwInputMapSample s;
    memset(&s, 0, sizeof(s));
    s.held = held;
    s.pressed = held & ~was;
    s.released = was & ~held;
    s.lx = s.ly = s.rx = s.ry = 128;
    s.active = 1;
    return s;
}

static size_t tick(PwInputMapState *state, const PwGameInput *input, const PwInputMapSample *s,
                   PwInputMapPad *pad, PwInputMapEvent *events)
{
    size_t count = pw_input_map_tick(state, &input->map, input->mode, s, TICK, pad, events, PW_INPUT_MAP_EVENT_MAX);
    assert(count <= PW_INPUT_MAP_EVENT_MAX);
    return count;
}

static int is_key(const PwInputMapEvent *e, uint32_t vk, uint32_t down)
{
    return e->type == PW_INPUT_MAP_KEY && e->code == vk && e->down == down;
}

/* A remap reaches the controller alone; keys and combinations come in their
 * order and go in the opposite one. */
static void test_buttons(void)
{
    PwGameInput input = mapped_input("mode = xinput\nmap = 1\ncross = pad:b\ncircle = pad:a\nsquare = key:ctrl+shift+s\n"
                                     "triangle = none\nl1 = pad:rt\nup = mouse:back\ndown = mouse:wheel_down\n");
    PwInputMapState state;
    PwInputMapPad pad;
    PwInputMapEvent e[PW_INPUT_MAP_EVENT_MAX];
    PwInputMapSample s;

    pw_input_map_init(&state);
    s = sample(CROSS | TRIANGLE | R1, 0);
    assert(tick(&state, &input, &s, &pad, e) == 0);
    assert(pad.buttons == (CIRCLE | R1) && pad.l2 == 0 && pad.r2 == 0 && pad.lx == 128 && pad.ry == 128);
    s = sample(CIRCLE | SQUARE | L1, CROSS | TRIANGLE | R1);
    assert(tick(&state, &input, &s, &pad, e) == 3);
    assert(is_key(&e[0], 0x11, 1) && is_key(&e[1], 0x10, 1) && is_key(&e[2], 'S', 1));
    assert(pad.buttons == CROSS && pad.r2 == 255);
    /* held: nothing new */
    s = sample(CIRCLE | SQUARE, CIRCLE | SQUARE | L1);
    assert(tick(&state, &input, &s, &pad, e) == 0 && pad.r2 == 0);
    s = sample(UP | DOWN, CIRCLE | SQUARE);
    assert(tick(&state, &input, &s, &pad, e) == 5);
    assert(is_key(&e[0], 'S', 0) && is_key(&e[1], 0x10, 0) && is_key(&e[2], 0x11, 0));
    assert(e[3].type == PW_INPUT_MAP_MOUSE_BUTTON && e[3].code == PW_GAME_MOUSE_BACK && e[3].down == 1);
    assert(e[4].type == PW_INPUT_MAP_MOUSE_WHEEL && e[4].x == -1);
    /* the wheel does not repeat while held */
    s = sample(UP | DOWN, UP | DOWN);
    assert(tick(&state, &input, &s, &pad, e) == 0);
    s = sample(0, UP | DOWN);
    assert(tick(&state, &input, &s, &pad, e) == 1 && e[0].type == PW_INPUT_MAP_MOUSE_BUTTON && e[0].down == 0);
    /* a press and its release in one batch: down, then up */
    s = sample(0, 0);
    s.pressed = SQUARE;
    s.released = SQUARE;
    assert(tick(&state, &input, &s, &pad, e) == 6 && is_key(&e[0], 0x11, 1) && is_key(&e[3], 'S', 0) &&
           is_key(&e[5], 0x11, 0));
    /* let go, then pressed again in one batch */
    s = sample(SQUARE, 0);
    assert(tick(&state, &input, &s, &pad, e) == 3);
    s = sample(SQUARE, SQUARE);
    s.pressed = s.released = SQUARE;
    assert(tick(&state, &input, &s, &pad, e) == 6 && is_key(&e[0], 'S', 0) && is_key(&e[5], 'S', 1));
}

/* Controller targets that depend on the mode, and the touchpad's halves. */
static void test_modes(void)
{
    PwInputMapState state;
    PwInputMapPad pad;
    PwInputMapEvent e[PW_INPUT_MAP_EVENT_MAX];
    PwInputMapSample s;
    PwGameInput x = mapped_input("mode = xinput\nmap = 1\ncross = pad:guide\ncircle = pad:touchpad\n");
    PwGameInput ps = mapped_input("mode = playstation\nmap = 1\ncross = pad:guide\ncircle = pad:touchpad\n");
    PwGameInput keys = mapped_input("mode = keyboard\nmap = 1\ntouchpad_left = key:m\ntouchpad_right = key:i\n");

    pw_input_map_init(&state);
    s = sample(CROSS | CIRCLE, 0);
    tick(&state, &x, &s, &pad, e);
    assert(pad.buttons == TOUCHPAD && pad.touch_left == 0);   /* Guide; the touchpad is no XInput button */
    pw_input_map_init(&state);
    tick(&state, &ps, &s, &pad, e);
    assert(pad.buttons == TOUCHPAD);                          /* the click; the PS button stays the console's */
    s = sample(CROSS, 0);
    pw_input_map_init(&state);
    tick(&state, &ps, &s, &pad, e);
    assert(pad.buttons == 0);
    /* no line: the halves are Back and Guide in xinput mode, the click on a DualShock 4 */
    PwGameInput bare = mapped_input("mode = xinput\nmap = 1\n");
    pw_input_map_init(&state);
    s = sample(TOUCHPAD, 0);
    s.touch_left = 1;
    s.touch_fingers = 1;
    tick(&state, &bare, &s, &pad, e);
    assert(pad.buttons == CREATE);
    s.touch_left = 0;
    pw_input_map_init(&state);
    tick(&state, &bare, &s, &pad, e);
    assert(pad.buttons == TOUCHPAD && pad.touch_left == 0);
    /* the half the click began on sends its key */
    pw_input_map_init(&state);
    s = sample(TOUCHPAD, 0);
    s.touch_left = 1;
    s.touch_fingers = 1;
    assert(tick(&state, &keys, &s, &pad, e) == 1 && is_key(&e[0], 'M', 1));
    s = sample(0, TOUCHPAD);
    assert(tick(&state, &keys, &s, &pad, e) == 1 && is_key(&e[0], 'M', 0));
    s = sample(TOUCHPAD, 0);
    s.touch_fingers = 1;
    assert(tick(&state, &keys, &s, &pad, e) == 1 && is_key(&e[0], 'I', 1));
    s = sample(0, TOUCHPAD);
    assert(tick(&state, &keys, &s, &pad, e) == 1 && is_key(&e[0], 'I', 0));
    /* a two-finger click is the console's keyboard's, never the map's */
    s = sample(TOUCHPAD, 0);
    s.touch_fingers = 2;
    assert(tick(&state, &keys, &s, &pad, e) == 0);
    s = sample(0, TOUCHPAD);
    assert(tick(&state, &keys, &s, &pad, e) == 0);
}

/* A trigger as a button: down at its threshold, up 5 points below it; with
 * no threshold, the DualSense's own click. */
static void test_triggers(void)
{
    PwGameInput input = mapped_input("mode = xinput\nmap = 1\nr2 = mouse:left\nr2_threshold = 30\nl2 = pad:rt\n");
    PwGameInput click = mapped_input("mode = xinput\nmap = 1\nr2 = key:space\n");
    PwInputMapState state;
    PwInputMapPad pad;
    PwInputMapEvent e[PW_INPUT_MAP_EVENT_MAX];
    PwInputMapSample s;

    pw_input_map_init(&state);
    s = sample(0, 0);
    s.r2 = 75;  /* 30 * 255 / 100 = 76 */
    s.l2 = 90;
    assert(tick(&state, &input, &s, &pad, e) == 0 && pad.r2 == 90 && pad.l2 == 0);
    s.r2 = 76;
    assert(tick(&state, &input, &s, &pad, e) == 1 && e[0].type == PW_INPUT_MAP_MOUSE_BUTTON && e[0].down == 1);
    assert(pad.r2 == 90);   /* the trigger as a button never reaches the controller */
    s.r2 = 64;  /* 25 * 255 / 100 = 63 */
    assert(tick(&state, &input, &s, &pad, e) == 0);
    s.r2 = 63;
    assert(tick(&state, &input, &s, &pad, e) == 1 && e[0].down == 0);
    s.r2 = 70;
    assert(tick(&state, &input, &s, &pad, e) == 0);
    /* the analog bits do not matter without a threshold: the click does */
    pw_input_map_init(&state);
    s = sample(0, 0);
    s.r2 = 255;
    assert(tick(&state, &click, &s, &pad, e) == 0 && pad.r2 == 0);
    s = sample(R2, 0);
    s.r2 = 20;
    assert(tick(&state, &click, &s, &pad, e) == 1 && is_key(&e[0], 0x20, 1) && pad.r2 == 0);
}

/* Sticks: keys and d-pad with their hysteresis, mouse, wheel, and a
 * controller stick shaped or left as it is. */
static void test_sticks(void)
{
    PwInputMapState state;
    PwInputMapPad pad;
    PwInputMapEvent e[PW_INPUT_MAP_EVENT_MAX];
    PwInputMapSample s;
    PwGameInput keys = mapped_input("mode = keyboard\nmap = 1\nleft_stick = keys\nleft_stick_keys = w,s,a,none\n"
                                    "right_stick = pad:dpad\nright_stick_invert_y = true\n");

    /* dead zone 40: on past 50, off at 44 or less */
    pw_input_map_init(&state);
    s = sample(0, 0);
    s.ly = 128 - 50;
    assert(tick(&state, &keys, &s, &pad, e) == 0);
    s.ly = 128 - 51;
    assert(tick(&state, &keys, &s, &pad, e) == 1 && is_key(&e[0], 'W', 1));
    s.ly = 128 - 45;
    assert(tick(&state, &keys, &s, &pad, e) == 0);
    s.ly = 128 - 44;
    assert(tick(&state, &keys, &s, &pad, e) == 1 && is_key(&e[0], 'W', 0));
    /* a diagonal holds two; none sends nothing */
    s.ly = 255;
    s.lx = 0;
    assert(tick(&state, &keys, &s, &pad, e) == 2 && is_key(&e[0], 'S', 1) && is_key(&e[1], 'A', 1));
    s.lx = 255;
    assert(tick(&state, &keys, &s, &pad, e) == 1 && is_key(&e[0], 'A', 0));
    /* the d-pad, inverted up and down */
    s = sample(0, 0);
    s.ry = 0;
    s.rx = 255;
    tick(&state, &keys, &s, &pad, e);
    assert(pad.buttons == (DOWN | RIGHT) && pad.rx == 128 && pad.lx == 128);

    /* mouse: the pointer stick's motion, dead zone 16 = 20 raw, squared, 1200 */
    PwGameInput mouse = mapped_input("mode = keyboard\nmap = 1\nleft_stick = mouse\nright_stick = wheel\n"
                                     "right_stick_speed = 10\n");
    PwWinePointer pointer = { 0, 0 };
    PwWineInput move;
    pw_input_map_init(&state);
    for (int i = 0; i < 300; i++) {
        uint8_t x = (uint8_t)(i * 37), y = (uint8_t)(255 - i * 11);
        int moved = pw_wine_pointer_step(&pointer, x, y, 1200, TICK, &move);
        size_t n;
        s = sample(0, 0);
        s.lx = x;
        s.ly = y;
        n = tick(&state, &mouse, &s, &pad, e);
        assert(n == (size_t)moved);
        if (moved) assert(e[0].type == PW_INPUT_MAP_MOUSE_MOVE && e[0].x == move.x && e[0].y == move.y);
    }
    /* wheel: 10 notches a second at full tilt, up for up */
    pw_input_map_init(&state);
    int notches = 0;
    for (int i = 0; i < 60; i++) {
        s = sample(0, 0);
        s.ry = 0;
        size_t n = pw_input_map_tick(&state, &mouse.map, mouse.mode, &s, 1000000 / 60, &pad, e, PW_INPUT_MAP_EVENT_MAX);
        for (size_t k = 0; k < n; k++) {
            assert(e[k].type == PW_INPUT_MAP_MOUSE_WHEEL && e[k].x > 0);
            notches += e[k].x;
        }
    }
    assert(notches == 9 || notches == 10);

    /* controller sticks: as they are, swapped, added, shaped */
    PwGameInput swap = mapped_input("mode = xinput\nmap = 1\nleft_stick = pad:right_stick\nright_stick = pad:right_stick\n");
    pw_input_map_init(&state);
    s = sample(0, 0);
    s.lx = 200; s.ly = 10; s.rx = 255; s.ry = 128;
    tick(&state, &swap, &s, &pad, e);
    assert(pad.lx == 128 && pad.ly == 128 && pad.rx == 255 && pad.ry == 10);
    PwGameInput shaped = mapped_input("mode = xinput\nmap = 1\nleft_stick_deadzone = 20\nleft_stick_invert_x = true\n");
    pw_input_map_init(&state);
    s = sample(0, 0);
    s.lx = 128 + 20; s.ly = 128 + 10;   /* within 25 raw */
    tick(&state, &shaped, &s, &pad, e);
    assert(pad.lx == 128 && pad.ly == 128);
    s.lx = 255; s.ly = 128;
    tick(&state, &shaped, &s, &pad, e);
    assert(pad.lx == 1 && pad.ly == 128);
    s.lx = 128 - 64; s.ly = 128;        /* (64 - 25) / (127 - 25) of the way */
    tick(&state, &shaped, &s, &pad, e);
    assert(pad.lx == 128 + 48 && pad.ly == 128);
    s.lx = 0; s.ly = 0;                 /* a corner keeps its direction, at full reach */
    tick(&state, &shaped, &s, &pad, e);
    assert(pad.lx == 128 + 89 && pad.ly == 128 - 89);
    /* The distance is not rounded down before the dead zone: at 90 % (114
     * raw), (114, 15) is sqrt(13221) = 114.98 from the centre, 0.98 past it
     * of 13, so 7.5 % of the way: 9.57 long, (9.49, 1.25) in its direction. */
    PwGameInput wide = mapped_input("mode = xinput\nmap = 1\nleft_stick_deadzone = 90\n");
    pw_input_map_init(&state);
    s = sample(0, 0);
    s.lx = 128 + 114; s.ly = 128 + 15;
    tick(&state, &wide, &s, &pad, e);
    assert(pad.lx == 128 + 9 && pad.ly == 128 + 1);
    s.lx = 128 + 114; s.ly = 128;       /* on the dead zone itself: nothing */
    tick(&state, &wide, &s, &pad, e);
    assert(pad.lx == 128 && pad.ly == 128);
}

/* Letting go: a swap, the menu or the keyboard; what is still held stays
 * away until it is let go. A lost pad lets go of everything. */
static void test_release(void)
{
    PwGameInput input = mapped_input("mode = xinput\nmap = 1\ncross = key:ctrl+a\ncircle = pad:a\nr2 = key:e\n"
                                     "r2_threshold = 50\nleft_stick = keys\nleft_stick_keys = w,s,a,d\nright_stick = mouse\n");
    PwInputMapState state;
    PwInputMapPad pad;
    PwInputMapEvent e[PW_INPUT_MAP_EVENT_MAX];
    PwInputMapSample s;

    pw_input_map_init(&state);
    s = sample(CROSS | CIRCLE, 0);
    s.r2 = 200;
    s.ly = 0;
    s.rx = 200;
    assert(tick(&state, &input, &s, &pad, e) == 4 + 1);   /* ctrl, a, e, w and a move */
    assert(pad.buttons == CROSS);
    s = sample(CROSS | CIRCLE, CROSS | CIRCLE);
    s.r2 = 200;
    s.ly = 0;
    assert(pw_input_map_release(&state, &input.map, input.mode, &s, e, PW_INPUT_MAP_EVENT_MAX) == 4);
    assert(is_key(&e[0], 'A', 0) && is_key(&e[1], 0x11, 0) && is_key(&e[2], 'E', 0) && is_key(&e[3], 'W', 0));
    assert(state.pointer_x == 0 && state.pointer_y == 0);
    /* still held after the release: nothing reaches the game */
    assert(tick(&state, &input, &s, &pad, e) == 0 && pad.buttons == 0);
    s.r2 = 140;   /* above 45 % */
    s.ly = 128 - 50;
    assert(tick(&state, &input, &s, &pad, e) == 0);
    /* let go of each; then they work again */
    s = sample(0, CROSS | CIRCLE);
    s.r2 = 100;
    s.ly = 128 - 44;
    assert(tick(&state, &input, &s, &pad, e) == 0);
    s = sample(CROSS | CIRCLE, 0);
    s.r2 = 200;
    s.ly = 0;
    assert(tick(&state, &input, &s, &pad, e) == 4 && pad.buttons == CROSS);
    /* a lost pad: all comes up */
    s = sample(0, CROSS | CIRCLE);
    s.active = 0;
    s.r2 = 0;
    assert(tick(&state, &input, &s, &pad, e) == 4 && is_key(&e[0], 'A', 0) && is_key(&e[1], 0x11, 0) &&
           is_key(&e[2], 'E', 0) && is_key(&e[3], 'W', 0) && pad.buttons == 0);
    /* a fresh press while latched counts */
    pw_input_map_init(&state);
    s = sample(CROSS, 0);
    tick(&state, &input, &s, &pad, e);
    s = sample(CROSS, CROSS);
    assert(pw_input_map_release(&state, &input.map, input.mode, &s, e, PW_INPUT_MAP_EVENT_MAX) == 2);
    s.pressed = s.released = CROSS;
    assert(tick(&state, &input, &s, &pad, e) == 2 && is_key(&e[0], 0x11, 1));
    /* without a pad nothing is kept */
    pw_input_map_init(&state);
    assert(pw_input_map_release(&state, &input.map, input.mode, NULL, e, PW_INPUT_MAP_EVENT_MAX) == 0 && !state.latched);

    /* Sticks and triggers that reach the game as they move are kept too:
     * the controller reads idle, and the pointer and the wheel stay still,
     * until each is back at rest. */
    PwGameInput analog = mapped_input("mode = xinput\nmap = 1\nr2 = pad:rt\nl2 = pad:rt\n");
    PwGameInput pointer = mapped_input("mode = keyboard\nmap = 1\nleft_stick = mouse\nright_stick = wheel\n");
    pw_input_map_init(&state);
    s = sample(R2, 0);
    s.lx = 220;
    s.r2 = 240;
    tick(&state, &analog, &s, &pad, e);
    assert(pad.lx == 220 && pad.r2 == 240);
    assert(pw_input_map_release(&state, &analog.map, analog.mode, &s, e, PW_INPUT_MAP_EVENT_MAX) == 0);
    s.pressed = 0;
    tick(&state, &analog, &s, &pad, e);
    assert(pad.lx == 128 && pad.r2 == 0);
    s.lx = 128 + 21;   /* still past 16 % (20 raw) */
    s.r2 = 13;         /* still past 5 % (12 raw) */
    s.l2 = 30;         /* L2 was at rest: it is the game's */
    tick(&state, &analog, &s, &pad, e);
    assert(pad.lx == 128 && pad.r2 == 30);
    s.lx = 128 + 20;
    s.r2 = 12;
    s.l2 = 0;
    tick(&state, &analog, &s, &pad, e);
    assert(pad.lx == 128 + 20 && pad.r2 == 12);
    s.lx = 220;
    s.r2 = 240;
    tick(&state, &analog, &s, &pad, e);
    assert(pad.lx == 220 && pad.r2 == 240);
    pw_input_map_init(&state);
    s = sample(0, 0);
    s.lx = 255;
    s.ry = 0;
    assert(pw_input_map_release(&state, &pointer.map, pointer.mode, &s, e, PW_INPUT_MAP_EVENT_MAX) == 0);
    for (int i = 0; i < 30; i++) assert(tick(&state, &pointer, &s, &pad, e) == 0);
    s.lx = s.ry = 128;
    assert(tick(&state, &pointer, &s, &pad, e) == 0);
    s.lx = 255;
    s.ry = 0;
    size_t moved = 0;
    for (int i = 0; i < 30; i++) moved += tick(&state, &pointer, &s, &pad, e);
    assert(moved > 30);
}

/* Two combinations share a modifier: it stays down until the last lets go,
 * and a stick's key shares its key with a button's. */
static void test_shared_keys(void)
{
    PwGameInput input = mapped_input("mode = keyboard\nmap = 1\ncross = key:ctrl+s\ncircle = key:ctrl+d\n"
                                     "square = key:w\nleft_stick = keys\nleft_stick_keys = w,s,a,d\n");
    PwInputMapState state;
    PwInputMapPad pad;
    PwInputMapEvent e[PW_INPUT_MAP_EVENT_MAX];
    PwInputMapSample s;

    pw_input_map_init(&state);
    s = sample(CROSS, 0);
    assert(tick(&state, &input, &s, &pad, e) == 2 && is_key(&e[0], 0x11, 1) && is_key(&e[1], 'S', 1));
    s = sample(CROSS | CIRCLE, CROSS);
    assert(tick(&state, &input, &s, &pad, e) == 1 && is_key(&e[0], 'D', 1));
    s = sample(CIRCLE, CROSS | CIRCLE);
    assert(tick(&state, &input, &s, &pad, e) == 1 && is_key(&e[0], 'S', 0));
    s = sample(0, CIRCLE);
    assert(tick(&state, &input, &s, &pad, e) == 2 && is_key(&e[0], 'D', 0) && is_key(&e[1], 0x11, 0));
    /* square and the stick's up both hold W */
    s = sample(SQUARE, 0);
    assert(tick(&state, &input, &s, &pad, e) == 1 && is_key(&e[0], 'W', 1));
    s = sample(SQUARE, SQUARE);
    s.ly = 0;
    assert(tick(&state, &input, &s, &pad, e) == 0);
    s = sample(0, SQUARE);
    s.ly = 0;
    assert(tick(&state, &input, &s, &pad, e) == 0);
    s = sample(0, 0);
    assert(tick(&state, &input, &s, &pad, e) == 1 && is_key(&e[0], 'W', 0));
    /* a release lets go of a shared key once */
    s = sample(CROSS | CIRCLE, 0);
    assert(tick(&state, &input, &s, &pad, e) == 3);
    assert(pw_input_map_release(&state, &input.map, input.mode, NULL, e, PW_INPUT_MAP_EVENT_MAX) == 3);
    assert(is_key(&e[0], 'S', 0) && is_key(&e[1], 'D', 0) && is_key(&e[2], 0x11, 0));
}

/* One read can hold more than one pull of a trigger: a threshold sees each. */
static void test_trigger_reads(void)
{
    PwGameInput input = mapped_input("mode = keyboard\nmap = 1\nr2 = key:e\nr2_threshold = 30\n");
    PwInputMapState state;
    PwInputMapPad pad;
    PwInputMapEvent e[PW_INPUT_MAP_EVENT_MAX];
    PwInputMapSample s;
    uint8_t l2[3] = { 0, 0, 0 }, r2[3];

    /* 30 %: down at 76, up at 63 */
    pw_input_map_init(&state);
    s = sample(0, 0);
    s.pulls[0] = l2;
    s.pulls[1] = r2;
    s.pull_count = 2;
    r2[0] = 80; r2[1] = s.r2 = 70;  /* crossed, and stays past the release point */
    assert(tick(&state, &input, &s, &pad, e) == 1 && is_key(&e[0], 'E', 1));
    r2[0] = 50; r2[1] = s.r2 = 70;  /* came back under 63 within the read */
    assert(tick(&state, &input, &s, &pad, e) == 1 && is_key(&e[0], 'E', 0));
    r2[0] = 80; r2[1] = s.r2 = 70;
    assert(tick(&state, &input, &s, &pad, e) == 1 && is_key(&e[0], 'E', 1));
    r2[0] = 50; r2[1] = s.r2 = 80;  /* let go and pulled again */
    assert(tick(&state, &input, &s, &pad, e) == 2 && is_key(&e[0], 'E', 0) && is_key(&e[1], 'E', 1));
    s.pull_count = 3;
    r2[0] = 20; r2[1] = 90; r2[2] = s.r2 = 10;  /* a tap within one read: down and up */
    assert(tick(&state, &input, &s, &pad, e) == 3 && is_key(&e[0], 'E', 0) && is_key(&e[1], 'E', 1) &&
           is_key(&e[2], 'E', 0));

    /* through the title's pad: the read's pulls come with it */
    PwPadPs5 real, mapped;
    PwWineInput out[PW_INPUT_MAP_EVENT_MAX];
    pw_input_map_init(&state);
    memset(&real, 0, sizeof(real));
    real.core.connected = 1;
    real.left_stick.x = real.left_stick.y = real.right_stick.x = real.right_stick.y = 0x80;
    real.pulls[1][0] = 80;
    real.pulls[1][1] = real.r2 = 70;
    real.pull_count = 2;
    assert(pw_wine_input_map(&state, &input, &real, TICK, 0, &mapped, out, PW_INPUT_MAP_EVENT_MAX) == 1 &&
           out[0].type == PW_WINE_INPUT_KEY && out[0].code == 'E' && out[0].down == 1);
    assert(mapped.pull_count == 0);
    /* pulls that do not end with the newest are not the read's: only r2 counts */
    pw_input_map_init(&state);
    real.pulls[1][1] = 99;
    assert(pw_wine_input_map(&state, &input, &real, TICK, 0, &mapped, out, PW_INPUT_MAP_EVENT_MAX) == 0);
}

/* A button let go and pressed again between two ticks, while the console's
 * keyboard is closed: its view keeps the press, and the map sends both. */
static void test_repress(void)
{
    PwGameInput input = mapped_input("mode = keyboard\nmap = 1\ncross = key:space\n");
    PwInputMapState state;
    PwInputMapPad pad;
    PwInputMapEvent e[PW_INPUT_MAP_EVENT_MAX];
    PwInputMapSample s;
    PwOsk osk;
    PwOskView view;

    memset(&osk, 0, sizeof(osk));
    pw_input_map_init(&state);
    assert(!pw_osk_tick(&osk, CROSS, CROSS, 0, 0, &view));
    s = sample(view.held, 0);
    s.pressed = view.pressed;
    s.released = view.released;
    assert(tick(&state, &input, &s, &pad, e) == 1 && is_key(&e[0], 0x20, 1));
    assert(!pw_osk_tick(&osk, CROSS, CROSS, CROSS, 0, &view));
    s = sample(view.held, 0);
    s.pressed = view.pressed;
    s.released = view.released;
    assert(tick(&state, &input, &s, &pad, e) == 2 && is_key(&e[0], 0x20, 0) && is_key(&e[1], 0x20, 1));
}

/* A DualShock 4 has no Guide: a [launcher] preset in playstation mode makes
 * the game's controller one, and an xinput map's Guide then sends nothing,
 * not the touchpad's click. */
static void test_dualshock_guide(void)
{
    PwGameInput input = mapped_input("mode = xinput\nmap = 1\ncross = pad:guide\ncircle = pad:back\n");
    PwInputMapState state;
    PwPadPs5 real, mapped;
    PwWineInput out[PW_INPUT_MAP_EVENT_MAX];
    PwWinePad game;
    PwWinePadExtra extra;

    memset(&real, 0, sizeof(real));
    real.core.connected = 1;
    real.core.previous_buttons = real.core.pressed_edges = CROSS | CIRCLE;
    real.left_stick.x = real.left_stick.y = real.right_stick.x = real.right_stick.y = 0x80;
    pw_input_map_init(&state);
    pw_wine_input_map(&state, &input, &real, TICK, 1, &mapped, out, PW_INPUT_MAP_EVENT_MAX);
    assert(pw_wine_game_pad_playstation(&mapped, &game, &extra));
    assert(!(extra.buttons & PW_WINE_PAD_TOUCHPAD) && (extra.buttons & PW_WINE_PAD_SHARE));
    /* the XInput controller: Guide as ever */
    pw_input_map_init(&state);
    pw_wine_input_map(&state, &input, &real, TICK, 0, &mapped, out, PW_INPUT_MAP_EVENT_MAX);
    assert(pw_wine_game_pad(&mapped, &game) && (game.buttons & 0x0400u) && (game.buttons & 0x0020u));
}

/* ---- The old presets and their templates give the same input ----------- */

static uint32_t rng = 12345;
static uint32_t next(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

/* A DualSense from pw_pad_ps5: the buttons' edges follow the held ones, and a
 * lost or intercepted pad reads released and centred. */
static void pad_step(PwPadPs5 *pad, int t)
{
    const uint32_t all = CROSS | CIRCLE | SQUARE | TRIANGLE | L1 | R1 | L2 | R2 | L3 | R3 | UP | DOWN | LEFT | RIGHT |
                         OPTIONS | CREATE | TOUCHPAD;
    const uint32_t was = pad->core.previous_buttons;
    const unsigned phase = (unsigned)(t % 400);
    uint32_t held = was;
    int lost = phase >= 300 && phase < 310, intercepted = phase >= 350 && phase < 356;

    for (uint32_t bit = 1; bit; bit <<= 1)
        if ((all & bit) && next() % 9 == 0) held ^= bit;
    pad->intercepted = (unsigned)intercepted;
    pad->core.connected = !lost && !intercepted;
    if (!pad->core.connected) held = 0;
    pad->core.previous_buttons = held;
    pad->core.pressed_edges = held & ~was;
    pad->core.released_edges = was & ~held;
    if (pad->core.connected) {
        static const uint8_t spots[] = { 0, 1, 20, 64, 100, 107, 108, 109, 127, 128, 129, 147, 148, 149, 200, 254, 255 };
        uint8_t *axes[4] = { &pad->left_stick.x, &pad->left_stick.y, &pad->right_stick.x, &pad->right_stick.y };
        for (int a = 0; a < 4; a++)
            if (next() % 3 == 0) *axes[a] = next() % 2 ? spots[next() % sizeof(spots)] : (uint8_t)next();
        if (next() % 3 == 0) pad->l2 = (uint8_t)next();
        if (next() % 3 == 0) pad->r2 = (uint8_t)next();
        pad->finger_down = next() % 2;
        pad->finger_count = pad->finger_down;
        pad->finger_x = (uint16_t)(next() % 1920);
        pad->finger_y = (uint16_t)(next() % 1080);
    } else {
        pad->left_stick.x = pad->left_stick.y = pad->right_stick.x = pad->right_stick.y = 0x80;
        pad->l2 = pad->r2 = 0;
        pad->finger_down = pad->finger_count = 0;
    }
    if (pad->core.pressed_edges & TOUCHPAD) {
        pad->touch_left = next() % 2;
        pad->touch_held = 1;
        pad->touch_fingers = 1;
        pad->touch_from = PW_PAD_PS5_TOUCH_FROM_CLICK;
        pad->touch_x = pad->finger_x;
        pad->touch_y = pad->finger_y;
    } else if (!(held & TOUCHPAD)) {
        pad->touch_held = 0;
        pad->touch_fingers = 0;
        pad->touch_from = PW_PAD_PS5_TOUCH_FROM_NONE;
    }
}

/* Controller 0 as the title feeds it: idle under a preset that is no
 * controller. */
static int controller(const PwPadPs5 *pad, PwGameInputMode mode, PwWinePad *state, PwWinePadExtra *extra)
{
    int connected = mode == PW_GAME_INPUT_PLAYSTATION ? pw_wine_game_pad_playstation(pad, state, extra) :
                    (memset(extra, 0, sizeof(*extra)), pw_wine_game_pad(pad, state));
    if (connected && mode == PW_GAME_INPUT_KEYBOARD) {
        memset(state, 0, sizeof(*state));
        state->connected = 1;
        memset(extra, 0, sizeof(*extra));
    }
    return connected;
}

static void equivalent(const char *name, const char *preset, const char *template_lines, int ticks)
{
    PwGameInput old, map = mapped_input(template_lines);
    PwInputMapState state;
    PwWinePointer pointer = { 0, 0 };
    PwPadPs5 pad;
    size_t events = 0, moves = 0;

    pw_game_input_init(&old);
    assert(pw_game_input_parse((const uint8_t *)preset, strlen(preset), &old) == PW_OK);
    pw_game_input_default_mode(&old);
    assert(!old.map.on && map.map.on && old.mode == map.mode);
    pw_input_map_init(&state);
    memset(&pad, 0, sizeof(pad));
    pad.left_stick.x = pad.left_stick.y = pad.right_stick.x = pad.right_stick.y = 0x80;
    for (int t = 0; t < ticks; t++) {
        PwWineInput a[PW_INPUT_MAP_EVENT_MAX], b[PW_INPUT_MAP_EVENT_MAX];
        PwPadPs5 unbound, mapped;
        PwWinePad pa, pb;
        PwWinePadExtra xa, xb;
        size_t na, nb;
        int ca, cb;

        pad_step(&pad, t);
        /* today's path, as the frame loop runs it */
        na = pw_wine_game_inputs(&old, pad.core.pressed_edges, pad.core.released_edges, a, 2 * PW_GAME_BUTTON_COUNT);
        if (old.mouse != PW_GAME_STICK_NONE) {
            const PwPadPs5Stick *stick = old.mouse == PW_GAME_STICK_LEFT ? &pad.left_stick : &pad.right_stick;
            na += (size_t)pw_wine_pointer_step(&pointer, stick->x, stick->y, old.mouse_speed, TICK, &a[na]);
        }
        /* The one intended difference: a lost pad clears the pointer's
         * fraction of a pixel under the map. */
        if (!pad.core.connected || pad.intercepted) pointer = (PwWinePointer){ 0, 0 };
        pw_wine_pad_unbound(&pad, &old, &unbound);
        ca = controller(&unbound, old.mode, &pa, &xa);
        /* the map */
        nb = pw_wine_input_map(&state, &map, &pad, TICK, map.mode == PW_GAME_INPUT_PLAYSTATION, &mapped, b,
                               PW_INPUT_MAP_EVENT_MAX);
        cb = controller(&mapped, map.mode, &pb, &xb);
        if (na != nb || memcmp(a, b, na * sizeof(a[0])) || ca != cb || memcmp(&pa, &pb, sizeof(pa)) ||
            memcmp(&xa, &xb, sizeof(xa))) {
            fprintf(stderr, "%s: tick %d differs: events %zu/%zu buttons %#x/%#x held %#x pressed %#x released %#x\n",
                    name, t, na, nb, pa.buttons, pb.buttons, pad.core.previous_buttons, pad.core.pressed_edges,
                    pad.core.released_edges);
            for (size_t i = 0; i < na || i < nb; i++)
                fprintf(stderr, "  %zu: %u/%u %#x/%#x %d,%d/%d,%d %u/%u\n", i, i < na ? a[i].type : 0,
                        i < nb ? b[i].type : 0, i < na ? a[i].code : 0, i < nb ? b[i].code : 0,
                        i < na ? a[i].x : 0, i < na ? a[i].y : 0, i < nb ? b[i].x : 0, i < nb ? b[i].y : 0,
                        i < na ? a[i].down : 0, i < nb ? b[i].down : 0);
            assert(0);
        }
        events += na;
        for (size_t i = 0; i < na; i++) moves += a[i].type == PW_WINE_INPUT_MOUSE_MOVE;
    }
    printf("  %s: %d ticks, %zu events (%zu moves) the same\n", name, ticks, events, moves);
}

#define GAMEPAD "cross = pad:a\ncircle = pad:b\nsquare = pad:x\ntriangle = pad:y\nl1 = pad:lb\nr1 = pad:rb\n" \
    "l2 = pad:lt\nr2 = pad:rt\nl3 = pad:ls\nr3 = pad:rs\nup = pad:dpad_up\ndown = pad:dpad_down\n" \
    "left = pad:dpad_left\nright = pad:dpad_right\noptions = pad:start\ncreate = pad:back\n" \
    "touchpad_left = pad:back\ntouchpad_right = pad:guide\nleft_stick = pad:left_stick\nright_stick = pad:right_stick\n"
#define MOUSE "cross = mouse:left\ncircle = mouse:right\nsquare = mouse:middle\ntriangle = none\nl1 = none\n" \
    "r1 = none\nl2 = mouse:right\nr2 = mouse:left\nl3 = none\nr3 = none\nup = none\ndown = none\nleft = none\n" \
    "right = none\noptions = key:enter\ncreate = none\ntouchpad_left = none\ntouchpad_right = none\n" \
    "left_stick = mouse\nleft_stick_speed = 900\nright_stick = none\n"
#define ARROWS "cross = key:z\ncircle = key:x\nsquare = key:shift\ntriangle = key:c\nl1 = none\nr1 = none\n" \
    "l2 = none\nr2 = none\nl3 = none\nr3 = none\nup = key:up\ndown = key:down\nleft = key:left\n" \
    "right = key:right\noptions = key:enter\ncreate = none\ntouchpad_left = none\ntouchpad_right = none\n" \
    "left_stick = none\nright_stick = none\n"
#define WASD "cross = key:space\ncircle = key:ctrl\nsquare = key:r\ntriangle = key:e\nl1 = key:q\nr1 = key:f\n" \
    "l2 = mouse:right\nr2 = mouse:left\nl3 = key:shift\nr3 = key:v\nup = mouse:wheel_up\ndown = mouse:wheel_down\n" \
    "left = key:1\nright = key:2\noptions = key:escape\ncreate = key:tab\ntouchpad_left = key:m\n" \
    "touchpad_right = key:i\nleft_stick = keys\nleft_stick_keys = w,s,a,d\nright_stick = mouse\n"

static void test_presets(void)
{
    /* the presets' [input] lines, as the input files hold them */
    equivalent("gamepad", "[input]\nmode = xinput\nmouse = none\n", "mode = xinput\nmap = 1\n" GAMEPAD, 40000);
    equivalent("mouse", "[input]\nmode = keyboard\nmouse = left_stick\nmouse_speed = 900\ncross = mouse_left\n"
               "circle = mouse_right\nsquare = mouse_middle\nr2 = mouse_left\nl2 = mouse_right\noptions = enter\n",
               "mode = keyboard\nmap = 1\n" MOUSE, 40000);
    equivalent("arrows", "[input]\nmode = keyboard\nmouse = none\nup = up\ndown = down\nleft = left\nright = right\n"
               "cross = z\ncircle = x\ntriangle = c\nsquare = shift\noptions = enter\n",
               "mode = keyboard\nmap = 1\n" ARROWS, 40000);
    /* no line at all is the controller as it is, in both controller modes */
    equivalent("xinput, empty map", "[input]\nmode = xinput\n", "mode = xinput\nmap = 1\n", 20000);
    equivalent("playstation, empty map", "[input]\nmode = playstation\n", "mode = playstation\nmap = 1\n", 20000);
    equivalent("playstation, gamepad lines", "[input]\nmode = playstation\n",
               "mode = playstation\nmap = 1\ncross = pad:a\ncircle = pad:b\ncreate = pad:back\n"
               "touchpad_left = pad:touchpad\ntouchpad_right = pad:touchpad\n", 20000);
    /* a profile binding a few buttons in xinput mode: the bound ones leave the controller */
    equivalent("xinput with keys", "[input]\nmode = xinput\ncross = space\nr2 = mouse_left\ntouchpad = m\n",
               "mode = xinput\nmap = 1\ncross = key:space\nr2 = mouse:left\ntouchpad_left = key:m\n"
               "touchpad_right = key:m\n", 20000);
    assert(strlen(GAMEPAD) < 4096 && strlen(MOUSE) < 4096 && strlen(ARROWS) < 4096 && strlen(WASD) < 4096);
}

/* The pointer stick as it was before the map: its own copy, so a change to
 * the shared curve shows here. */
static int64_t old_travel(uint8_t stick, uint32_t speed, uint32_t elapsed_us)
{
    int32_t axis = (int32_t)stick - 0x80, magnitude = axis < 0 ? -axis : axis;
    int64_t t, v;

    if (magnitude <= 20) return 0;
    t = (int64_t)(magnitude - 20) * 65536 / (127 - 20);
    if (t > 65536) t = 65536;
    v = t * t >> 16;
    v = v * speed * elapsed_us / 1000000;
    return axis < 0 ? -v : v;
}

static void test_travel(void)
{
    static const uint32_t speeds[] = { 1, 900, 1200, 20000 }, times[] = { 1, 16000, 16667, 1000000 };
    for (int v = 0; v < 256; v++)
        for (size_t i = 0; i < 4; i++)
            for (size_t k = 0; k < 4; k++)
                assert(pw_input_map_travel(v - 128, 20, 0, PW_GAME_CURVE_SQUARED, speeds[i], times[k]) ==
                       old_travel((uint8_t)v, speeds[i], times[k]));
}

/* The WASD and mouse template, input by input: what each sends, and a
 * controller that reads idle. */
static void test_wasd(void)
{
    static const struct { uint32_t button; int left; uint32_t type, code; int x; } sends[] = {
        { CROSS, 0, PW_INPUT_MAP_KEY, 0x20, 0 }, { CIRCLE, 0, PW_INPUT_MAP_KEY, 0x11, 0 },
        { SQUARE, 0, PW_INPUT_MAP_KEY, 'R', 0 }, { TRIANGLE, 0, PW_INPUT_MAP_KEY, 'E', 0 },
        { L1, 0, PW_INPUT_MAP_KEY, 'Q', 0 }, { R1, 0, PW_INPUT_MAP_KEY, 'F', 0 },
        { L2, 0, PW_INPUT_MAP_MOUSE_BUTTON, 1, 0 }, { R2, 0, PW_INPUT_MAP_MOUSE_BUTTON, 0, 0 },
        { L3, 0, PW_INPUT_MAP_KEY, 0x10, 0 }, { R3, 0, PW_INPUT_MAP_KEY, 'V', 0 },
        { UP, 0, PW_INPUT_MAP_MOUSE_WHEEL, 0, 1 }, { DOWN, 0, PW_INPUT_MAP_MOUSE_WHEEL, 0, -1 },
        { LEFT, 0, PW_INPUT_MAP_KEY, '1', 0 }, { RIGHT, 0, PW_INPUT_MAP_KEY, '2', 0 },
        { OPTIONS, 0, PW_INPUT_MAP_KEY, 0x1b, 0 }, { CREATE, 0, PW_INPUT_MAP_KEY, 0x09, 0 },
        { TOUCHPAD, 1, PW_INPUT_MAP_KEY, 'M', 0 }, { TOUCHPAD, 0, PW_INPUT_MAP_KEY, 'I', 0 },
    };
    static const struct { uint8_t lx, ly; uint32_t vk; } stick[] = {
        { 128, 0, 'W' }, { 128, 255, 'S' }, { 0, 128, 'A' }, { 255, 128, 'D' },
    };
    PwGameInput wasd = mapped_input("mode = keyboard\nmap = 1\n" WASD);
    PwInputMapState state;
    PwInputMapPad pad;
    PwInputMapEvent e[PW_INPUT_MAP_EVENT_MAX];
    PwInputMapSample s;

    assert(wasd.mode == PW_GAME_INPUT_KEYBOARD);
    pw_input_map_init(&state);
    for (size_t i = 0; i < sizeof(sends) / sizeof(sends[0]); i++) {
        const int wheel = sends[i].type == PW_INPUT_MAP_MOUSE_WHEEL;
        s = sample(sends[i].button, 0);
        s.touch_left = sends[i].left;
        s.touch_fingers = 1;
        s.l2 = s.r2 = 255;
        assert(tick(&state, &wasd, &s, &pad, e) == 1);
        assert(e[0].type == sends[i].type && e[0].code == sends[i].code && e[0].x == sends[i].x &&
               e[0].down == (wheel ? 0u : 1u));
        assert(pad.buttons == 0);
        s = sample(0, sends[i].button);
        assert(tick(&state, &wasd, &s, &pad, e) == (wheel ? 0u : 1u));
        if (!wheel) assert(e[0].type == sends[i].type && e[0].code == sends[i].code && e[0].down == 0);
    }
    for (size_t i = 0; i < 4; i++) {
        s = sample(0, 0);
        s.lx = stick[i].lx;
        s.ly = stick[i].ly;
        assert(tick(&state, &wasd, &s, &pad, e) == 1 && is_key(&e[0], stick[i].vk, 1));
        s = sample(0, 0);
        assert(tick(&state, &wasd, &s, &pad, e) == 1 && is_key(&e[0], stick[i].vk, 0));
    }
    /* the right stick moves the pointer, right and down */
    size_t moves = 0;
    for (int i = 0; i < 10; i++) {
        s = sample(0, 0);
        s.rx = s.ry = 255;
        size_t n = tick(&state, &wasd, &s, &pad, e);
        for (size_t k = 0; k < n; k++) {
            assert(e[k].type == PW_INPUT_MAP_MOUSE_MOVE && e[k].x > 0 && e[k].y > 0);
            moves++;
        }
        assert(pad.lx == 128 && pad.rx == 128 && pad.l2 == 0 && pad.r2 == 0);
    }
    assert(moves == 10);
}

int main(void)
{
    test_buttons();
    test_modes();
    test_triggers();
    test_sticks();
    test_release();
    test_shared_keys();
    test_trigger_reads();
    test_repress();
    test_dualshock_guide();
    test_presets();
    test_travel();
    test_wasd();
    printf("input map passed: remaps, keys and combinations, mouse buttons and the wheel, touchpad halves, "
           "triggers with thresholds, stick modes, letting go, and the old presets matched by their templates\n");
    return 0;
}
