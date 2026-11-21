/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_game_profile.h"
#include "../src/pw_wine_start.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define APP "[application]\nid = pinball\nname = Space Cadet Pinball\n" \
            "executable = C:\\Games\\Pinball\\PINBALL.EXE\nworking_directory = C:\\Games\\Pinball\n" \
            "prefix = default\nruntime = wine-wow64\narchitecture = pe32\ngraphics = gdi\n"

static int parse(const char *text, PwGameProfile *profile)
{
    return pw_game_profile_parse((const uint8_t *)text, strlen(text), profile);
}

static int preset(const char *text, PwGameInput *input)
{
    return pw_game_input_parse((const uint8_t *)text, strlen(text), input);
}

enum { CROSS, CIRCLE, SQUARE, TRIANGLE, L1, R1, L2, R2, L3, R3, UP, DOWN, LEFT, RIGHT, OPTIONS,
       CREATE, TOUCHPAD };

static void test_graphics_env(void)
{
    PwGameProfile p;
    PwGameEnv env[PW_GAME_GRAPHICS_ENV_MAX], cpu[PW_GAME_CPU_ENV_MAX], runtime[PW_GAME_RUNTIME_ENV_MAX];
    assert(parse(APP, &p) == PW_OK);
    assert(pw_game_graphics_env(NULL, env) == 0 && pw_game_graphics_env(&p, NULL) == 0);
    for (int graphics = PW_APP_GRAPHICS_AUTO; graphics <= PW_APP_GRAPHICS_ZINK; graphics++) {
        for (int fps = 0; fps < 2; fps++) {
            p.app.graphics = (PwAppGraphics)graphics; p.display.show_fps = fps;
            p.display.refresh = 120; p.display.opengl_thread = 1;
            p.runtime.thread_scheduling = 1; p.runtime.shared_input = 1; p.runtime.fast_clock = 1;
            p.runtime.cpu = PW_GAME_CPU_NATIVE; /* worst-case explicit override */
            size_t count = pw_game_graphics_env(&p, env);
            assert(count <= PW_GAME_GRAPHICS_ENV_MAX);
            /* Fixed 7; desktop, overrides, raw input, HID drivers, pad identity and the helper's sections 6;
             * graphics, display, runtime, clock and CPU. */
            assert(7 + 6 + count + PW_GAME_DISPLAY_ENV_MAX + pw_game_cpu_env(&p, cpu) +
                   pw_game_runtime_env(&p.runtime, runtime) + PW_GAME_CLOCK_ENV_MAX + PW_GAME_DEBUG_ENV_MAX +
                   PW_GAME_ENVIRONMENT_MAX <= PW_WINE_START_MAX_ENV);
            /* refresh and opengl_thread are not graphics variables */
            if (graphics == PW_APP_GRAPHICS_ZINK) {
                assert(count == (size_t)(2 + fps));
                assert(!strcmp(env[0].name, "GALLIUM_DRIVER") && !strcmp(env[0].value, "zink"));
                assert(!strcmp(env[1].name, "PW_VK_DEFER_DESCRIPTORS") && !strcmp(env[1].value, "1"));
                if (fps) assert(!strcmp(env[2].name, "GALLIUM_HUD"));
            } else {
                assert(count == (size_t)fps);
                if (fps) assert(!strcmp(env[0].name, "DXVK_HUD"));
            }
        }
    }
    /* The menu is a DXVK HUD item beside or instead of the frame rate; Mesa's
     * HUD has none. */
    assert(parse(APP "[display]\nmenu = true\n", &p) == PW_OK && p.display.menu == 1 && p.display.show_fps == 1);
    assert(pw_game_graphics_env(&p, env) == 1 && !strcmp(env[0].name, "DXVK_HUD") &&
           !strcmp(env[0].value, "fps,menu"));
    assert(parse(APP "[display]\nmenu = true\nshow_fps = false\n", &p) == PW_OK);
    assert(pw_game_graphics_env(&p, env) == 1 && !strcmp(env[0].value, "menu"));
    p.app.graphics = PW_APP_GRAPHICS_ZINK;
    assert(pw_game_graphics_env(&p, env) == 2 && strcmp(env[1].name, "GALLIUM_HUD"));
    assert(parse(APP "[display]\nmenu = false\n", &p) == PW_OK && p.display.menu == 0);
    assert(pw_game_graphics_env(&p, env) == 1 && !strcmp(env[0].value, "fps"));
    assert(parse(APP "[display]\nmenu = yes\n", &p) == PW_ERR_UNSUPPORTED);
    assert(parse(APP "[display]\nmenu = true\nmenu = false\n", &p) == PW_ERR_MALFORMED);
    /* A profile with the menu on cannot set the HUD variable itself. */
    assert(parse(APP "[display]\nmenu = true\nshow_fps = false\n[debug]\nenv = DXVK_HUD=full\n", &p) ==
           PW_ERR_UNSUPPORTED);
    assert(parse(APP "[display]\nshow_fps = false\n[debug]\nenv = DXVK_HUD=full\n", &p) == PW_OK);
    /* framegen: PW_FRAMEGEN for the Vulkan driver, any backend; show_fps
     * becomes the generator's counter (PW_FRAMEGEN_HUD), not the backend's. */
    assert(parse(APP "[display]\nshow_fps = false\n", &p) == PW_OK && p.display.framegen == 0);
    assert(pw_game_graphics_env(&p, env) == 0);
    assert(parse(APP "[display]\nframegen = off\nshow_fps = false\n", &p) == PW_OK && p.display.framegen == 0);
    assert(pw_game_graphics_env(&p, env) == 0);
    static const char *const factors[] = { "2", "3", "4" };
    for (int f = 2; f <= 4; f++) {
        char text[256];
        snprintf(text, sizeof(text), APP "[display]\nframegen = %d\n", f);
        assert(parse(text, &p) == PW_OK && p.display.framegen == (uint32_t)f);
        assert(pw_game_graphics_env(&p, env) == 2);
        assert(!strcmp(env[0].name, "PW_FRAMEGEN") && !strcmp(env[0].value, factors[f - 2]));
        assert(!strcmp(env[1].name, "PW_FRAMEGEN_HUD") && !strcmp(env[1].value, "1"));
    }
    assert(parse(APP "[display]\nframegen = 4\nshow_fps = false\n", &p) == PW_OK);
    assert(pw_game_graphics_env(&p, env) == 1 && !strcmp(env[0].value, "4"));
    /* fps_counter: the driver's counter, and framegen's comes first. */
    assert(parse(APP "[display]\nshow_fps = false\nfps_counter = true\n", &p) == PW_OK && p.display.fps_counter == 1);
    assert(pw_game_graphics_env(&p, env) == 1 && !strcmp(env[0].name, "PW_FRAMEGEN_COUNTER") &&
           !strcmp(env[0].value, "1"));
    assert(parse(APP "[display]\nshow_fps = false\nfps_counter = false\n", &p) == PW_OK && p.display.fps_counter == 0);
    assert(pw_game_graphics_env(&p, env) == 0);
    assert(parse(APP "[display]\nframegen = 2\nfps_counter = true\n", &p) == PW_OK);
    assert(pw_game_graphics_env(&p, env) == 2 && !strcmp(env[0].name, "PW_FRAMEGEN"));
    assert(parse(APP "[display]\nfps_counter = maybe\n", &p) == PW_ERR_UNSUPPORTED);
    assert(parse(APP "[display]\nfps_counter = true\nfps_counter = true\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[display]\nfps_counter = true\n[debug]\nenv = PW_FRAMEGEN_COUNTER=0\n", &p) == PW_ERR_UNSUPPORTED);
    /* The menu stays DXVK's, without its frame rate. */
    assert(parse(APP "[display]\nframegen = 2\nmenu = true\n", &p) == PW_OK);
    assert(pw_game_graphics_env(&p, env) == 3 && !strcmp(env[0].name, "DXVK_HUD") &&
           !strcmp(env[0].value, "menu") && !strcmp(env[1].name, "PW_FRAMEGEN") &&
           !strcmp(env[2].name, "PW_FRAMEGEN_HUD"));
    p.app.graphics = PW_APP_GRAPHICS_ZINK;
    assert(pw_game_graphics_env(&p, env) == PW_GAME_GRAPHICS_ENV_MAX &&
           !strcmp(env[0].name, "GALLIUM_DRIVER") && !strcmp(env[2].name, "PW_FRAMEGEN") &&
           !strcmp(env[2].value, "2") && !strcmp(env[3].name, "PW_FRAMEGEN_HUD"));
    /* A profile with framegen on cannot set PW_FRAMEGEN itself; without, it can. */
    assert(parse(APP "[display]\nframegen = 3\n[debug]\nenv = PW_FRAMEGEN=2\n", &p) == PW_ERR_UNSUPPORTED);
    assert(parse(APP "[display]\nframegen = 3\n[debug]\nenv = PW_FRAMEGEN_HUD=0\n", &p) == PW_ERR_UNSUPPORTED);
    assert(parse(APP "[debug]\nenv = PW_FRAMEGEN=2\n", &p) == PW_OK && p.display.framegen == 0);
    /* hud: the driver's overlay, for every backend; it replaces show_fps's, fps_counter's
     * and framegen's counters, and DXVK's HUD keeps only the menu. */
    {
        static const char *const levels[] = { "fps", "frametimes", "full" };
        for (int l = 0; l < 3; l++) {
            char text[256];
            snprintf(text, sizeof(text), APP "[display]\nshow_fps = false\nhud = %s\n", levels[l]);
            assert(parse(text, &p) == PW_OK && p.display.hud == (PwGameHud)(PW_GAME_HUD_FPS + l));
            assert(pw_game_graphics_env(&p, env) == 1 && !strcmp(env[0].name, "PW_HUD") &&
                   !strcmp(env[0].value, levels[l]));
        }
    }
    assert(parse(APP "[display]\nshow_fps = false\n", &p) == PW_OK && p.display.hud == PW_GAME_HUD_OFF);
    assert(parse(APP "[display]\nshow_fps = false\nhud = off\n", &p) == PW_OK && p.display.hud == PW_GAME_HUD_OFF);
    assert(pw_game_graphics_env(&p, env) == 0);
    assert(parse(APP "[display]\nhud = everything\n", &p) == PW_ERR_UNSUPPORTED);
    assert(parse(APP "[display]\nhud = fps\nhud = full\n", &p) == PW_ERR_MALFORMED);
    /* show_fps left on: the backend's counter gives way to the overlay. */
    assert(parse(APP "[display]\nhud = full\n", &p) == PW_OK && p.display.show_fps == 1);
    assert(pw_game_graphics_env(&p, env) == 1 && !strcmp(env[0].name, "PW_HUD"));
    p.app.graphics = PW_APP_GRAPHICS_ZINK;
    assert(pw_game_graphics_env(&p, env) == 3 && !strcmp(env[0].name, "GALLIUM_DRIVER") &&
           !strcmp(env[2].name, "PW_HUD"));
    assert(parse(APP "[display]\nshow_fps = false\nfps_counter = true\nhud = fps\n", &p) == PW_OK);
    assert(pw_game_graphics_env(&p, env) == 1 && !strcmp(env[0].name, "PW_HUD") && !strcmp(env[0].value, "fps"));
    /* With frame generation the overlay shows its pictures; the menu stays DXVK's. */
    assert(parse(APP "[display]\nframegen = 2\nmenu = true\nhud = frametimes\n", &p) == PW_OK);
    assert(pw_game_graphics_env(&p, env) == 3 && !strcmp(env[0].name, "DXVK_HUD") &&
           !strcmp(env[0].value, "menu") && !strcmp(env[1].name, "PW_FRAMEGEN") && !strcmp(env[2].name, "PW_HUD") &&
           !strcmp(env[2].value, "frametimes"));
    p.app.graphics = PW_APP_GRAPHICS_ZINK;
    assert(pw_game_graphics_env(&p, env) <= PW_GAME_GRAPHICS_ENV_MAX);
    /* The overlay's variable is the title's while hud is on; DXVK's HUD stays reachable through [debug] env. */
    assert(parse(APP "[display]\nhud = fps\n[debug]\nenv = PW_HUD=full\n", &p) == PW_ERR_UNSUPPORTED);
    assert(parse(APP "[display]\nhud = fps\n[debug]\nenv = PW_FRAMEGEN_COUNTER=1\n", &p) == PW_ERR_UNSUPPORTED);
    assert(parse(APP "[display]\nshow_fps = false\nhud = fps\n[debug]\nenv = DXVK_HUD=full\n", &p) == PW_OK);
    assert(parse(APP "[debug]\nenv = PW_HUD=full\n", &p) == PW_OK && p.display.hud == PW_GAME_HUD_OFF);
    /* A Zink game's [debug] env decides the deferral itself: the title adds none. */
    assert(parse(APP "[debug]\nenv = PW_VK_DEFER_DESCRIPTORS=0\n", &p) == PW_OK);
    p.app.graphics = PW_APP_GRAPHICS_ZINK; p.display.show_fps = 0;
    assert(pw_game_graphics_env(&p, env) == 1 && !strcmp(env[0].name, "GALLIUM_DRIVER"));
}

/* [display] refresh and frame_rate: two variables the title always sets. */
static void test_display_env(void)
{
    static const struct { const char *lines, *refresh, *frame_rate; } cases[] = {
        { "", "60", "0" },
        { "[display]\nrefresh = 60\n", "60", "0" },
        { "[display]\nrefresh = 120\n", "120", "0" },
        { "[display]\nframe_rate = 60\n", "60", "60" },
        { "[display]\nrefresh = 120\nframe_rate = 120\n", "120", "120" },
        { "[display]\nrefresh = 120\nframe_rate = 60\n", "120", "60" },
        { "[display]\nrefresh = 120\nframe_rate = 40\n", "120", "40" },
        { "[display]\nframe_rate = 30\n", "60", "30" },
        { "[display]\nframe_rate = 0\nopengl_thread = true\n", "60", "0" },
    };
    PwGameProfile p;
    PwGameEnv env[PW_GAME_DISPLAY_ENV_MAX];

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char text[1024];
        snprintf(text, sizeof(text), APP "%s", cases[i].lines);
        assert(parse(text, &p) == PW_OK);
        assert(pw_game_display_env(&p.display, env) == PW_GAME_DISPLAY_ENV_MAX);
        assert(!strcmp(env[0].name, "PS5_VIDEOOUT_REFRESH") && !strcmp(env[0].value, cases[i].refresh));
        assert(!strcmp(env[1].name, "PS5_VIDEOOUT_FRAME_RATE") && !strcmp(env[1].value, cases[i].frame_rate));
    }
    /* the title's default for a bare path= without a profile */
    {
        const PwGameDisplay display = { .refresh = 60 };
        assert(pw_game_display_env(&display, env) == 2 && !strcmp(env[0].value, "60") &&
               !strcmp(env[1].value, "0"));
    }
    assert(pw_game_display_env(NULL, env) == 0 && pw_game_display_env(&p.display, NULL) == 0);
    /* The names are the title's: neither [debug] env nor [environment] may set them. */
    assert(parse(APP "[debug]\nenv = PS5_VIDEOOUT_REFRESH=120\n", &p) != PW_OK);
    assert(parse(APP "[debug]\nenv = PS5_VIDEOOUT_FRAME_RATE=60\n", &p) != PW_OK);
    assert(parse(APP "[environment]\nPS5_VIDEOOUT_REFRESH = 120\n", &p) != PW_OK);
    assert(parse(APP "[environment]\nPS5_VIDEOOUT_FRAME_RATE = 60\n", &p) != PW_OK);
}

static void test_profile(void)
{
    PwGameProfile p;

    assert(parse(APP, &p) == PW_OK);
    assert(!strcmp(p.app.id, "pinball") && !strcmp(p.app.runtime, "wine-wow64"));
    assert(!p.display.width && p.display.scaling == PW_GAME_SCALING_FIT);
    assert(!p.input.set && p.input.bindings[CROSS].kind == PW_GAME_BIND_UNSET);

    assert(parse(APP "; comments and blank lines\n\n[display]\ndesktop = 1024x768\nscaling = Integer\n"
                 "[input]\npreset = pinball\nmode = keyboard\nmouse = right_stick\nmouse_speed = 900\nrumble = 40\n"
                 "cross = space\nL1 = z\nr1 = slash\nr2 = mouse_left\nl2 = mouse_right\n"
                 "r3 = mouse_middle\noptions = f3\nsquare = F12\ntriangle = vk:0x5B\n"
                 "circle = 7\ntouchpad = none\nup = up\n", &p) == PW_OK);
    assert(p.display.width == 1024 && p.display.height == 768);
    assert(p.display.scaling == PW_GAME_SCALING_INTEGER);
    assert(!strcmp(p.input.preset, "pinball") && p.input.mouse == PW_GAME_STICK_RIGHT);
    assert(p.input.mouse_speed == 900 && p.input.mode == PW_GAME_INPUT_KEYBOARD && p.input.rumble == 40);
    assert(p.input.bindings[CROSS].kind == PW_GAME_BIND_KEY && p.input.bindings[CROSS].code == 0x20);
    assert(p.input.bindings[CROSS].mask == 0x4000u && p.input.bindings[L1].code == 'Z');
    assert(p.input.bindings[R1].code == 0xbf && p.input.bindings[OPTIONS].code == 0x72);
    assert(p.input.bindings[SQUARE].code == 0x7b && p.input.bindings[TRIANGLE].code == 0x5b);
    assert(p.input.bindings[CIRCLE].code == '7' && p.input.bindings[UP].code == 0x26);
    assert(p.input.bindings[R2].kind == PW_GAME_BIND_MOUSE && p.input.bindings[R2].code == 0);
    assert(p.input.bindings[L2].code == 1 && p.input.bindings[R3].code == 2);
    assert(p.input.bindings[TOUCHPAD].kind == PW_GAME_BIND_NONE);
    assert(p.input.bindings[DOWN].kind == PW_GAME_BIND_UNSET);

    assert(parse(APP "[input]\nmode = xinput\n[display]\nscaling = stretch\n", &p) == PW_OK);
    assert(p.input.mode == PW_GAME_INPUT_XINPUT && p.display.scaling == PW_GAME_SCALING_STRETCH);
    /* playstation: the controller is a DualShock 4 on the game's HID bus */
    assert(parse(APP "[input]\nmode = playstation\nrumble = 30\n", &p) == PW_OK);
    assert(p.input.mode == PW_GAME_INPUT_PLAYSTATION && p.input.rumble == 30);
    assert(parse(APP "[input]\nmode = PlayStation\n", &p) == PW_OK && p.input.mode == PW_GAME_INPUT_PLAYSTATION);
    assert(p.display.view == PW_GAME_VIEW_WINDOW && p.display.show_fps == 1);
    assert(parse(APP "[display]\nview = desktop\n", &p) == PW_OK && p.display.view == PW_GAME_VIEW_DESKTOP);
    /* show_fps: the backend's frame-rate counter, on unless turned off */
    assert(parse(APP "[display]\nshow_fps = true\n", &p) == PW_OK && p.display.show_fps == 1);
    assert(parse(APP "[display]\nshow_fps = True\nview = desktop\n", &p) == PW_OK && p.display.show_fps == 1);
    assert(parse(APP "[display]\nshow_fps = false\n", &p) == PW_OK && p.display.show_fps == 0);
    /* refresh: 60 unless the profile asks for 120 */
    assert(parse(APP "[display]\nshow_fps = false\n", &p) == PW_OK && p.display.refresh == 60);
    assert(parse(APP "[display]\nrefresh = 120\n", &p) == PW_OK && p.display.refresh == 120);
    assert(parse(APP "[display]\nrefresh = 60\nshow_fps = true\n", &p) == PW_OK && p.display.refresh == 60);
    /* opengl_thread: off unless the profile asks for it */
    assert(parse(APP "[display]\nshow_fps = false\n", &p) == PW_OK && p.display.opengl_thread == 0);
    assert(parse(APP "[display]\nopengl_thread = true\n", &p) == PW_OK && p.display.opengl_thread == 1);
    assert(parse(APP "[display]\nopengl_thread = false\nrefresh = 120\n", &p) == PW_OK &&
           p.display.opengl_thread == 0 && p.display.refresh == 120);
    /* frame_rate: no cap unless the profile sets one */
    assert(parse(APP "[display]\nshow_fps = false\n", &p) == PW_OK && p.display.frame_rate == 0);
    assert(parse(APP "[display]\nframe_rate = 60\n", &p) == PW_OK && p.display.frame_rate == 60);
    assert(parse(APP "[display]\nrefresh = 120\nframe_rate = 40\n", &p) == PW_OK &&
           p.display.refresh == 120 && p.display.frame_rate == 40);
    assert(parse(APP "[display]\nframe_rate = 120\n", &p) == PW_OK && p.display.frame_rate == 120);
    assert(parse(APP "[display]\nframe_rate = 30\n", &p) == PW_OK && p.display.frame_rate == 30);
    assert(parse(APP "[display]\nframe_rate = 0\n", &p) == PW_OK && p.display.frame_rate == 0);
    /* another value is unsupported, a key twice malformed */
    assert(parse(APP "[display]\nframe_rate = 50\n", &p) == PW_ERR_UNSUPPORTED);
    assert(parse(APP "[display]\nrefresh = 90\n", &p) == PW_ERR_UNSUPPORTED);
    assert(parse(APP "[display]\nframe_rate = 60\nframe_rate = 60\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[display]\nrefresh = 120\nrefresh = 120\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[display]\nview = Window\n", &p) == PW_OK && p.display.view == PW_GAME_VIEW_WINDOW);
    assert(parse(APP "[display]\ndesktop = 320x200\n[input]\nmouse = left_stick\n", &p) == PW_OK);
    assert(p.display.width == 320 && p.input.mouse == PW_GAME_STICK_LEFT);
    assert(parse(APP "[display]\ndesktop = 3840x2160\n[input]\nmouse = none\ncross = enter\n", &p) == PW_OK);
    assert(p.input.mouse == PW_GAME_STICK_NONE && p.input.bindings[CROSS].code == 0x0d);

    /* [debug] winedebug: a game's own Wine channels, anywhere after [application]. */
    assert(!p.winedebug[0]);
    assert(parse(APP "[debug]\nwinedebug = +seh,warn+module,-all\n[display]\nview = desktop\n", &p) == PW_OK);
    assert(!strcmp(p.winedebug, "+seh,warn+module,-all") && p.display.view == PW_GAME_VIEW_DESKTOP);
    assert(parse(APP "[debug]\nwinedebug = trace+d3d.9,err=all\n", &p) == PW_OK);
}

/* [runtime] cpu: native by default for 32-bit games, the translator for
 * OpenGL ones, either chosen explicitly; native always brings batching. */
#define APP_ARCH(arch, gfx) "[application]\nid = g\nname = G\nexecutable = C:\\g.exe\n" \
    "working_directory = C:\\\nprefix = default\nruntime = wine-wow64\narchitecture = " arch \
    "\ngraphics = " gfx "\n"
static void test_cpu(void)
{
    PwGameProfile p;
    PwGameEnv env[PW_GAME_CPU_ENV_MAX];

    assert(parse(APP, &p) == PW_OK && p.runtime.cpu == PW_GAME_CPU_DEFAULT);
    assert(pw_game_cpu_native(&p) == 1);                       /* pe32 GDI */
    assert(pw_game_cpu_env(&p, env) == 2);
    assert(!strcmp(env[0].name, "WINE_PS5_WOW64_CPU") && !strcmp(env[0].value, "wow64native.dll"));
    assert(!strcmp(env[1].name, "PW_VK_BATCH") && !strcmp(env[1].value, "1"));

    assert(parse(APP_ARCH("pe32", "dxvk"), &p) == PW_OK && pw_game_cpu_native(&p) == 1);
    assert(parse(APP_ARCH("pe32", "auto"), &p) == PW_OK && pw_game_cpu_native(&p) == 1);
    /* OpenGL draws through Zink: native with the batching, like zink */
    assert(parse(APP_ARCH("pe32", "opengl"), &p) == PW_OK && pw_game_cpu_native(&p) == 1);
    assert(p.app.graphics == PW_APP_GRAPHICS_ZINK);
    assert(pw_game_cpu_env(&p, env) == 2 && !strcmp(env[1].name, "PW_VK_BATCH"));
    assert(parse(APP_ARCH("pe64", "dxvk"), &p) == PW_OK && pw_game_cpu_native(&p) == 0);

    assert(parse(APP_ARCH("pe32", "zink"), &p) == PW_OK && pw_game_cpu_native(&p) == 1);
    assert(pw_game_cpu_env(&p, env) == 2 && !strcmp(env[1].name, "PW_VK_BATCH"));
    assert(parse(APP_ARCH("pe64", "zink"), &p) == PW_OK && pw_game_cpu_env(&p, env) == 0);
    assert(parse(APP_ARCH("pe32", "zink") "[runtime]\ncpu=translator\n", &p) == PW_OK &&
           pw_game_cpu_env(&p, env) == 0);

    /* explicit choices override the default both ways */
    assert(parse(APP_ARCH("pe32", "opengl") "[runtime]\ncpu = translator\n", &p) == PW_OK);
    assert(p.runtime.cpu == PW_GAME_CPU_TRANSLATOR && pw_game_cpu_native(&p) == 0 &&
           pw_game_cpu_env(&p, env) == 0);
    assert(parse(APP_ARCH("pe32", "gdi") "[runtime]\ncpu = native\n", &p) == PW_OK);
    assert(p.runtime.cpu == PW_GAME_CPU_NATIVE && pw_game_cpu_native(&p) == 1 &&
           pw_game_cpu_env(&p, env) == 2);
    assert(parse(APP_ARCH("pe32", "dxvk") "[runtime]\nCPU = Translator\n", &p) == PW_OK);
    assert(p.runtime.cpu == PW_GAME_CPU_TRANSLATOR && pw_game_cpu_native(&p) == 0 &&
           pw_game_cpu_env(&p, env) == 0);
    /* a 64-bit game never uses the WoW64 CPU, even when asked */
    assert(parse(APP_ARCH("pe64", "dxvk") "[runtime]\ncpu = native\n", &p) == PW_OK);
    assert(pw_game_cpu_native(&p) == 0 && pw_game_cpu_env(&p, env) == 0);
    /* with thread scheduling, in either order */
    assert(parse(APP "[runtime]\ncpu = native\nthread_scheduling = 1\n", &p) == PW_OK);
    assert(p.runtime.thread_scheduling == 1 && p.runtime.cpu == PW_GAME_CPU_NATIVE);
    assert(pw_game_cpu_native(NULL) == 0 && pw_game_cpu_env(NULL, env) == 0 &&
           pw_game_cpu_env(&p, NULL) == 0);
}

/* [runtime]: opt-in settings that reach Wine's environment, off unless set. */
static void test_runtime(void)
{
    PwGameEnv env[PW_GAME_RUNTIME_ENV_MAX];
    PwGameProfile p;

    assert(parse(APP, &p) == PW_OK);
    assert(!p.runtime.thread_scheduling && pw_game_runtime_env(&p.runtime, env) == 0);
    assert(parse(APP "[runtime]\nthread_scheduling = true\n", &p) == PW_OK);
    assert(p.runtime.thread_scheduling == 1);
    assert(pw_game_runtime_env(&p.runtime, env) == 1);
    assert(!strcmp(env[0].name, "WINE_PS5_SCHED") && !strcmp(env[0].value, "1"));
    /* Among the other sections, in any case. */
    assert(parse(APP "[display]\nview = desktop\n[runtime]\nThread_Scheduling = 1\n"
                 "[debug]\nwinedebug = +seh\n", &p) == PW_OK);
    assert(p.runtime.thread_scheduling == 1);
    assert(p.display.view == PW_GAME_VIEW_DESKTOP && !strcmp(p.winedebug, "+seh"));
    /* Turned off explicitly: nothing reaches the environment. */
    assert(parse(APP "[runtime]\nthread_scheduling = FALSE\n", &p) == PW_OK);
    assert(!p.runtime.thread_scheduling && pw_game_runtime_env(&p.runtime, env) == 0);
    assert(pw_game_runtime_env(NULL, env) == 0 && pw_game_runtime_env(&p.runtime, NULL) == 0);

    /* shared_input reaches Wine; fast_clock is only a flag, the title
     * measures the TSC and sets the clock variables itself. */
    assert(parse(APP, &p) == PW_OK && !p.runtime.shared_input && !p.runtime.fast_clock);
    assert(parse(APP "[runtime]\nshared_input = true\n", &p) == PW_OK);
    assert(p.runtime.shared_input == 1 && !p.runtime.fast_clock);
    assert(pw_game_runtime_env(&p.runtime, env) == 1);
    assert(!strcmp(env[0].name, "PW_INPUT_SHARED_FAST") && !strcmp(env[0].value, "1"));
    assert(parse(APP "[runtime]\nfast_clock = 1\n", &p) == PW_OK);
    assert(p.runtime.fast_clock == 1 && !p.runtime.shared_input && pw_game_runtime_env(&p.runtime, env) == 0);
    assert(parse(APP "[runtime]\nfast_clock = false\nshared_input = 0\n", &p) == PW_OK);
    assert(!p.runtime.fast_clock && !p.runtime.shared_input);
    assert(parse(APP "[runtime]\nthread_scheduling = 1\nshared_input = 1\nfast_clock = true\ncpu = native\n",
                 &p) == PW_OK);
    assert(p.runtime.fast_clock && p.runtime.cpu == PW_GAME_CPU_NATIVE);
    assert(pw_game_runtime_env(&p.runtime, env) == 2);
    assert(!strcmp(env[0].name, "WINE_PS5_SCHED") && !strcmp(env[1].name, "PW_INPUT_SHARED_FAST"));
}

/* [runtime] system_locale and user_locale: Windows locale names, set as the
 * Unix locale Wine reads for each (LC_CTYPE, LC_MESSAGES); unset by default. */
static void test_runtime_locales(void)
{
    PwGameEnv env[PW_GAME_RUNTIME_ENV_MAX];
    PwGameProfile p;

    assert(parse(APP, &p) == PW_OK);
    assert(!p.runtime.system_locale && !p.runtime.user_locale && pw_game_runtime_env(&p.runtime, env) == 0);
    /* The system locale alone: Japanese code page, Wine's own user locale. */
    assert(parse(APP "[runtime]\nsystem_locale = ja-JP\n", &p) == PW_OK);
    assert(!strcmp(p.runtime.system_locale, "ja_JP.UTF-8") && !p.runtime.user_locale);
    assert(pw_game_runtime_env(&p.runtime, env) == 1);
    assert(!strcmp(env[0].name, "LC_CTYPE") && !strcmp(env[0].value, "ja_JP.UTF-8"));
    /* Both, in any case, after the other switches. */
    assert(parse(APP "[runtime]\nversion = 2\nUser_Locale = en-gb\nSYSTEM_LOCALE = ZH-TW\n", &p) == PW_OK);
    assert(!strcmp(p.runtime.system_locale, "zh_TW.UTF-8") && !strcmp(p.runtime.user_locale, "en_GB.UTF-8"));
    assert(parse(APP "[runtime]\nthread_scheduling = 1\nshared_input = 1\nsystem_locale = ko-KR\n"
                 "user_locale = de-DE\n", &p) == PW_OK);
    assert(pw_game_runtime_env(&p.runtime, env) == PW_GAME_RUNTIME_ENV_MAX);
    assert(!strcmp(env[2].name, "LC_CTYPE") && !strcmp(env[2].value, "ko_KR.UTF-8"));
    assert(!strcmp(env[3].name, "LC_MESSAGES") && !strcmp(env[3].value, "de_DE.UTF-8"));
    /* Every listed name maps to its Unix locale. */
    {
        static const char *const names[] = { "en-US", "en-GB", "de-DE", "fr-FR", "es-ES", "it-IT", "pt-BR",
                                             "pl-PL", "ru-RU", "ja-JP", "ko-KR", "zh-CN", "zh-TW" };
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            char text[1024], unix_name[16];
            snprintf(text, sizeof(text), APP "[runtime]\nsystem_locale = %s\n", names[i]);
            assert(parse(text, &p) == PW_OK);
            snprintf(unix_name, sizeof(unix_name), "%c%c_%c%c.UTF-8", names[i][0], names[i][1], names[i][3],
                     names[i][4]);
            assert(!strcmp(p.runtime.system_locale, unix_name));
        }
    }
    /* Names Wine might take but the profile does not list, Unix spellings,
     * empty values and repeats are refused. */
    assert(parse(APP "[runtime]\nsystem_locale = ja_JP.UTF-8\n", &p) == PW_ERR_UNSUPPORTED);
    assert(parse(APP "[runtime]\nsystem_locale = ja\n", &p) == PW_ERR_UNSUPPORTED);
    assert(parse(APP "[runtime]\nuser_locale = nl-NL\n", &p) == PW_ERR_UNSUPPORTED);
    assert(parse(APP "[runtime]\nuser_locale = ja-JP;x\n", &p) == PW_ERR_UNSUPPORTED);
    assert(parse(APP "[runtime]\nsystem_locale =\n", &p) != PW_OK);
    assert(parse(APP "[runtime]\nsystem_locale = ja-JP\nsystem_locale = ja-JP\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[runtime]\nlocale = ja-JP\n", &p) == PW_ERR_UNSUPPORTED);
}

/* [runtime] version: a profile without it is version 1 and keeps wave64
 * pixel shaders unless it asks for pswave32; version 2 takes the driver's
 * defaults. A version this title does not know is refused. */
static void test_version(void)
{
    PwGameEnv env[PW_GAME_VERSION_ENV_MAX];
    PwGameProfile p;

    /* [launcher]: off without it; its lines are the first program's input. */
    assert(parse(APP, &p) == PW_OK && !p.has_launcher);
    assert(parse(APP "[input]\npreset = gamepad\n[launcher]\nlabel = Mouse\ngame_label = Arrow keys\n"
                 "mode = keyboard\nmouse = left_stick\ncross = mouse_left\n", &p) == PW_OK);
    assert(p.has_launcher && !strcmp(p.launcher_label, "Mouse") && !strcmp(p.game_label, "Arrow keys"));
    assert(p.launcher_input.mouse == PW_GAME_STICK_LEFT && p.launcher_input.mode == PW_GAME_INPUT_KEYBOARD);
    assert(p.input.mouse == PW_GAME_STICK_NONE && !strcmp(p.input.preset, "gamepad"));
    assert(parse(APP "[launcher]\n", &p) == PW_OK && p.has_launcher && !p.launcher_label[0] && !p.launcher_chain_off);
    assert(parse(APP "[launcher]\npreset = mouse\n", &p) == PW_ERR_MALFORMED);
    /* chain = off: only the touchpad switch; off is the one value, once. */
    assert(parse(APP "[launcher]\nchain = off\nlabel = Mouse\nmode = keyboard\n", &p) == PW_OK);
    assert(p.has_launcher && p.launcher_chain_off && p.launcher_input.mode == PW_GAME_INPUT_KEYBOARD);
    assert(parse(APP "[launcher]\nchain = on\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[launcher]\nchain = off\nchain = off\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[input]\nchain = off\n", &p) != PW_OK);
    /* [helper]: a second program beside the game; off without the section. */
    assert(parse(APP, &p) == PW_OK && !p.has_helper && !p.helper_program[0]);
    assert(parse(APP "[helper]\nprogram = C:\\Tools\\service.exe\nready = tools/service.ready\nwait = 45\n", &p) == PW_OK);
    assert(p.has_helper && !strcmp(p.helper_program, "C:\\Tools\\service.exe") &&
           !strcmp(p.helper_ready, "tools/service.ready") && p.helper_wait == 45);
    assert(parse(APP "[helper]\nprogram = Z:\\data\\x y\\a.exe\n", &p) == PW_OK && !p.helper_ready[0] && !p.helper_wait);
    assert(parse(APP "[helper]\n", &p) == PW_ERR_MALFORMED);                          /* no program */
    assert(parse(APP "[helper]\nprogram = service.exe\n", &p) == PW_ERR_MALFORMED);  /* no drive */
    assert(parse(APP "[helper]\nprogram = C:\\a\"b.exe\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[helper]\nprogram = C:\\a.exe\nprogram = C:\\b.exe\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[helper]\nprogram = C:\\a.exe\nready = ../x\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[helper]\nprogram = C:\\a.exe\nready = /x\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[helper]\nprogram = C:\\a.exe\nready = a//b\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[helper]\nprogram = C:\\a.exe\nready = a\\b\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[helper]\nprogram = C:\\a.exe\nwait = 0\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[helper]\nprogram = C:\\a.exe\nwait = 121\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[helper]\nprogram = C:\\a.exe\nother = 1\n", &p) == PW_ERR_MALFORMED);
    /* [steam] install_dir: the folder in steamapps\common; none without the section. */
    assert(parse(APP, &p) == PW_OK && !p.steam_install_dir[0]);
    assert(parse(APP "[steam]\ninstall_dir = The Talos Principle\n", &p) == PW_OK &&
           !strcmp(p.steam_install_dir, "The Talos Principle"));
    assert(parse(APP "[steam]\ninstall_dir =   Half-Life 2  \n[helper]\nprogram = C:\\a.exe\n", &p) == PW_OK &&
           !strcmp(p.steam_install_dir, "Half-Life 2") && p.has_helper);
    assert(parse(APP "[steam]\n", &p) == PW_ERR_MALFORMED);                                   /* no folder */
    assert(parse(APP "[steam]\ninstall_dir =\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[steam]\ninstall_dir = a\ninstall_dir = b\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[steam]\ninstall_dir = ..\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[steam]\ninstall_dir = a\\b\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[steam]\ninstall_dir = a/b\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[steam]\ninstall_dir = C:\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[steam]\ninstall_dir = a\nother = 1\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[steam]\ninstall_dir = a\n[steam]\ninstall_dir = b\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[launcher]\nplayer2 = mouse\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[launcher]\nlabel = a\nlabel = b\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[launcher]\nlabel = a\"b\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[launcher]\nunknown = 1\n", &p) != PW_OK);
    assert(parse(APP "[launcher]\n[launcher]\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP, &p) == PW_OK && pw_game_profile_version(&p) == 1);
    assert(pw_game_version_env(&p, env) == 1);
    assert(!strcmp(env[0].name, "RADV_PS5_PS_WAVE64") && !strcmp(env[0].value, "1"));
    assert(parse(APP "[runtime]\nversion = 1\n", &p) == PW_OK && pw_game_profile_version(&p) == 1);
    assert(pw_game_version_env(&p, env) == 1);
    assert(parse(APP "[runtime]\nversion = 2\n", &p) == PW_OK && pw_game_profile_version(&p) == 2);
    assert(pw_game_version_env(&p, env) == 0);
    assert(parse(APP "[runtime]\nthread_scheduling = 1\nVersion = 2\n", &p) == PW_OK);
    assert(pw_game_profile_version(&p) == 2 && p.runtime.thread_scheduling == 1);
    /* Once, and only a version this title knows. */
    assert(parse(APP "[runtime]\nversion = 2\nversion = 2\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[runtime]\nversion = 3\n", &p) == PW_ERR_UNSUPPORTED);
    assert(parse(APP "[runtime]\nversion = 0\n", &p) == PW_ERR_UNSUPPORTED);
    assert(parse(APP "[runtime]\nversion = \n", &p) != PW_OK);
    /* Version 1 that asks for wave32, in [environment] or a [debug] env
     * line, among other options: nothing added. */
    assert(parse(APP "[environment]\nRADV_PERFTEST = pswave32\n", &p) == PW_OK);
    assert(pw_game_version_env(&p, env) == 0);
    assert(parse(APP "[environment]\nRADV_PERFTEST = sam,pswave32\n", &p) == PW_OK);
    assert(pw_game_version_env(&p, env) == 0);
    assert(parse(APP "[debug]\nenv = RADV_PERFTEST=sam,pswave32\n", &p) == PW_OK);
    assert(pw_game_version_env(&p, env) == 0);
    /* Other options, or a word that only starts with pswave32, keep wave64. */
    assert(parse(APP "[environment]\nRADV_PERFTEST = sam,nggc\n", &p) == PW_OK);
    assert(pw_game_version_env(&p, env) == 1);
    assert(parse(APP "[environment]\nRADV_PERFTEST = pswave32x\n", &p) == PW_OK);
    assert(pw_game_version_env(&p, env) == 1);
    /* The profile's own RADV_PS5_PS_WAVE64 is never set twice. */
    assert(parse(APP "[environment]\nRADV_PS5_PS_WAVE64 = 1\n", &p) == PW_OK);
    assert(pw_game_version_env(&p, env) == 0);
    /* Version 2 with Wave64 chosen: the profile's own variable does it. */
    assert(parse(APP "[runtime]\nversion = 2\n[environment]\nRADV_PS5_PS_WAVE64 = 1\n", &p) == PW_OK);
    assert(pw_game_version_env(&p, env) == 0 && p.environment_count == 1);
    assert(pw_game_profile_version(NULL) == 1 && pw_game_version_env(NULL, env) == 0 &&
           pw_game_version_env(&p, NULL) == 0);
}

/* [audio]: volume 0-100 in at most three digits, microphone on|off, each
 * once; without the section full volume and the microphone on. */
static void test_audio(void)
{
    PwGameProfile p;

    assert(parse(APP, &p) == PW_OK && p.audio.volume == 100 && p.audio.microphone == 1);
    assert(parse(APP "[audio]\nvolume = 50\n", &p) == PW_OK && p.audio.volume == 50 && p.audio.microphone == 1);
    assert(parse(APP "[audio]\nvolume = 0\nmicrophone = off\n", &p) == PW_OK);
    assert(p.audio.volume == 0 && p.audio.microphone == 0);
    assert(parse(APP "[audio]\nmicrophone = on\n", &p) == PW_OK && p.audio.microphone == 1 && p.audio.volume == 100);
    assert(parse(APP "[audio]\nvolume = 100\n", &p) == PW_OK && p.audio.volume == 100);
    /* three digits, as the launcher writes them; leading zeros are digits */
    assert(parse(APP "[audio]\nvolume = 007\n", &p) == PW_OK && p.audio.volume == 7);
    assert(parse(APP "[display]\nview = desktop\n[audio]\nMicrophone = OFF\n[runtime]\nversion = 2\n", &p) == PW_OK);
    assert(p.audio.microphone == 0);
    assert(parse(APP "[audio]\nvolume = 101\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[audio]\nvolume = 0100\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[audio]\nvolume = -1\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[audio]\nvolume = 50%\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[audio]\nvolume =\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[audio]\nvolume = 50\nvolume = 50\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[audio]\nmicrophone = off\nmicrophone = off\n", &p) == PW_ERR_MALFORMED);
    assert(parse(APP "[audio]\nmicrophone = true\n", &p) == PW_ERR_UNSUPPORTED);
    assert(parse(APP "[audio]\nmicrophone = 0\n", &p) == PW_ERR_UNSUPPORTED);
    assert(parse(APP "[audio]\nmute = true\n", &p) == PW_ERR_UNSUPPORTED);
    assert(parse(APP "[audio]\n[audio]\n", &p) == PW_ERR_MALFORMED);
    /* not in an input preset */
    PwGameInput input;
    pw_game_input_init(&input);
    assert(pw_game_input_parse((const uint8_t *)"[input]\nmode = xinput\n[audio]\nvolume = 5\n", 41, &input) ==
           PW_ERR_MALFORMED);
}

static void test_refusals(void)
{
    static const char *const bad[] = {
        "[display]\ndesktop = 800x600\n" APP,                  /* application not first */
        APP "[display]\n[display]\n",                          /* duplicate section */
        APP "[input]\n[input]\n",
        APP "[sound]\n",                                       /* unknown section */
        APP "[display]\nshow_fps = yes\n",                     /* only true or false */
        APP "[display]\nshow_fps =\n",
        APP "[display]\nshow_fps = true\nshow_fps = false\n",   /* twice */
        APP "[display]\nrefresh = 90\n",                      /* 60 or 120 only */
        APP "[display]\nrefresh =\n",
        APP "[display]\nrefresh = 120\nrefresh = 60\n",         /* twice */
        APP "[display]\nframe_rate = 50\n",                   /* 0, 120, 60, 40 or 30 only */
        APP "[display]\nframe_rate = 90\n",
        APP "[display]\nframe_rate = 060\n",
        APP "[display]\nframe_rate = -60\n",
        APP "[display]\nframe_rate =\n",
        APP "[display]\nframe_rate = 60\nframe_rate = 30\n",     /* twice */
        APP "[display]\nopengl_thread = yes\n",                /* true or false only */
        APP "[display]\nopengl_thread = true\nopengl_thread = false\n", /* twice */
        APP "[display]\nframegen = 1\n",                     /* off or 2..4 only */
        APP "[display]\nframegen = 5\n",
        APP "[display]\nframegen = on\n",
        APP "[display]\nframegen =\n",
        APP "[display]\nframegen = 2\nframegen = off\n",     /* twice */
        APP "[debug]\nwinedebug = +seh\n[debug]\n",            /* duplicate section */
        APP "[debug]\nwinedebug =\n",                         /* empty */
        APP "[debug]\nwinedebug = +seh;rm\n",                  /* not a channel list */
        APP "[debug]\nwinedebug = +seh all\n",
        APP "[debug]\nwinedebug = $HOME\n",
        APP "[debug]\nwinedebug = +seh\nwinedebug = +relay\n",  /* twice */
        APP "[debug]\nrelay = on\n",                           /* unknown key */
        APP "[debug]\nwinedebug = +aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n",   /* too long */
        APP "[runtime]\n[runtime]\n",                          /* duplicate section */
        APP "[runtime]\nthread_scheduling = yes\n",             /* true/false or 1/0 only */
        APP "[runtime]\nthread_scheduling = 2\n",
        APP "[runtime]\nthread_scheduling =\n",
        APP "[runtime]\nthread_scheduling = on\n",
        APP "[runtime]\nthread_scheduling = 0\nthread_scheduling = 1\n",
        APP "[runtime]\nshared_input = yes\n",                  /* true/false or 1/0 only */
        APP "[runtime]\nshared_input =\n",
        APP "[runtime]\nshared_input = 1\nshared_input = 0\n",   /* once */
        APP "[runtime]\nfast_clock = on\n",
        APP "[runtime]\nfast_clock = 2\n",
        APP "[runtime]\nfast_clock = true\nfast_clock = true\n",
        APP "[display]\nfast_clock = 1\n",                      /* another section's key */
        APP "[runtime]\ntrust_code_pages = 1\n",                /* unknown key */
        APP "[runtime]\ncpu = dbt\n",                           /* native or translator only */
        APP "[runtime]\ncpu =\n",
        APP "[runtime]\ncpu = native\ncpu = translator\n",     /* once */
        APP "[runtime]\nwinedebug = +seh\n",                    /* another section's key */
        APP "[display]\nthread_scheduling = 1\n",
        "[runtime]\nthread_scheduling = 1\n" APP,               /* application not first */
        APP "[display\n",
        APP "[]\n",
        APP "[display]\ncolour = 32\n",                        /* unknown key */
        APP "[display]\ndesktop = 800x600\ndesktop = 640x480\n",
        APP "[display]\ndesktop = 800\n",
        APP "[display]\ndesktop = 100x100\n",
        APP "[display]\ndesktop = 4000x2000\n",
        APP "[display]\ndesktop = 800x3000\n",
        APP "[display]\ndesktop = 8a0x600\n",
        APP "[display]\ndesktop = 800x\n",
        APP "[display]\nscaling = zoom\n",
        APP "[display]\nscaling = fit\nscaling = fit\n",
        APP "[display]\nview = screen\n",
        APP "[display]\nview = window\nview = window\n",
        APP "[input]\nmode = gamepad\n",
        APP "[input]\nmode = keyboard\nmode = keyboard\n",
        APP "[input]\nmode = playstation\nmode = xinput\n",
        APP "[input]\nmode = dualshock4\n",
        APP "[input]\nmouse = both_sticks\n",
        APP "[input]\nmouse = none\nmouse = none\n",
        APP "[input]\nmouse_speed = 0\n",
        APP "[input]\nmouse_speed = 20001\n",
        APP "[input]\nmouse_speed = fast\n",
        APP "[input]\nmouse_speed = 10\nmouse_speed = 10\n",
        APP "[input]\nmouse_speed = 1234567890\n",
        APP "[input]\nrumble = 101\n",
        APP "[input]\nrumble = -1\n",
        APP "[input]\nrumble = strong\n",
        APP "[input]\nrumble = 0\nrumble = 0\n",
        APP "[input]\ncross = space\ncross = enter\n",
        APP "[input]\ncross = hyperspace\n",
        APP "[input]\ncross = f0\n",
        APP "[input]\ncross = f25\n",
        APP "[input]\ncross = fx\n",
        APP "[input]\ncross = !\n",
        APP "[input]\ncross = vk:0x00\n",
        APP "[input]\ncross = vk:0xZZ\n",
        APP "[input]\nstart = space\n",
        APP "[input]\npreset = Pinball\n",
        APP "[input]\npreset = a/b\n",
        APP "[input]\npreset =\n",
        APP "[input]\npreset = a\npreset = b\n",
        APP "[input]\nplayer2 = P2\n",
        APP "[input]\nplayer2 =\n",
        APP "[input]\nplayer2 = a\nplayer2 = b\n",
        APP "[input]\npreset = 0123456789012345678901234567890123456789012345678901234567890123456789\n",
        APP "[input]\ncross space\n",
        APP "[input]\n = space\n",
        "[application]\nid = x\n",                           /* pw_app_profile refuses */
        "id = pinball\n",
    };
    PwGameProfile p;

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (parse(bad[i], &p) == PW_OK) { fprintf(stderr, "accepted bad profile %zu\n", i); assert(0); }
    }
    assert(pw_game_profile_parse(NULL, 1, &p) == PW_ERR_PRECONDITION);
    assert(pw_game_profile_parse((const uint8_t *)APP, 0, &p) == PW_ERR_PRECONDITION);
    assert(pw_game_profile_parse((const uint8_t *)APP, strlen(APP), NULL) == PW_ERR_PRECONDITION);
    assert(pw_game_profile_parse((const uint8_t *)APP, PW_APP_PROFILE_MAX_BYTES + 1, &p) == PW_ERR_LIMIT);
}

static void test_presets(void)
{
    PwGameInput input, overrides;
    PwGameProfile p;

    pw_game_input_init(&input);
    assert(input.mouse_speed == PW_GAME_MOUSE_SPEED_DEFAULT && input.mode == PW_GAME_INPUT_KEYBOARD);
    assert(input.rumble == PW_GAME_RUMBLE_MAX);
    assert(input.bindings[L1].mask == 0x400u && input.bindings[L1].kind == PW_GAME_BIND_UNSET);
    assert(preset("# shared pinball keys\n[input]\nl1 = z\nr1 = slash\ncross = space\n"
                  "mouse = right_stick\n", &input) == PW_OK);
    assert(input.bindings[L1].code == 'Z' && input.mouse == PW_GAME_STICK_RIGHT);
    assert(input.mouse_speed == PW_GAME_MOUSE_SPEED_DEFAULT);

    /* The profile's own lines override the preset, the rest stays. */
    assert(parse(APP "[input]\npreset = pinball\ncross = enter\nmouse_speed = 400\n", &p) == PW_OK);
    pw_game_input_overlay(&input, &p.input);
    assert(input.bindings[CROSS].code == 0x0d && input.bindings[L1].code == 'Z');
    assert(input.mouse_speed == 400 && input.mouse == PW_GAME_STICK_RIGHT);
    overrides = p.input;
    overrides.set = 0;
    pw_game_input_overlay(&input, &overrides);
    assert(input.mouse_speed == 400);

    /* rumble = 0 turns it off; a preset's value stays unless the profile sets one. */
    assert(preset("[input]\nrumble = 60\n", &input) == PW_OK && input.rumble == 60);
    assert(parse(APP "[input]\npreset = pinball\n", &p) == PW_OK);
    pw_game_input_overlay(&input, &p.input);
    assert(input.rumble == 60);
    assert(parse(APP "[input]\npreset = pinball\nrumble = 0\n", &p) == PW_OK);
    pw_game_input_overlay(&input, &p.input);
    assert(input.rumble == 0);

    /* A second pad's preset is named by the profile alone. */
    assert(parse(APP "[input]\npreset = bomb\nplayer2 = bomb-p2\n", &p) == PW_OK);
    assert(!strcmp(p.input.preset, "bomb") && !strcmp(p.input.player2, "bomb-p2"));
    assert(parse(APP "[input]\npreset = bomb\n", &p) == PW_OK && !p.input.player2[0]);

    /* A preset holds only [input] and cannot chain another preset. */
    assert(preset("[input]\npreset = other\n", &input) == PW_ERR_MALFORMED);
    assert(preset("[input]\nplayer2 = other\n", &input) == PW_ERR_MALFORMED);
    assert(preset("[display]\ndesktop = 800x600\n", &input) == PW_ERR_MALFORMED);
    assert(preset(APP, &input) == PW_ERR_MALFORMED);
    assert(preset("; nothing\n", &input) == PW_ERR_MALFORMED);
    assert(preset("l1 = z\n", &input) == PW_ERR_MALFORMED);
    assert(preset("[input]\nl1 = warp\n", &input) == PW_ERR_UNSUPPORTED);
    assert(input.bindings[L1].code == 'Z');             /* refused files change nothing */
    assert(pw_game_input_parse(NULL, 1, &input) == PW_ERR_PRECONDITION);
    assert(pw_game_input_parse((const uint8_t *)"[input]\n", 8, NULL) == PW_ERR_PRECONDITION);
    assert(pw_game_input_parse((const uint8_t *)"[input]\n", PW_APP_PROFILE_MAX_BYTES + 1, &input) ==
           PW_ERR_LIMIT);
    pw_game_input_init(NULL);
    pw_game_input_overlay(NULL, &input);
    pw_game_input_overlay(&input, NULL);

    assert(pw_game_button_mask(CREATE) == 0x1u && pw_game_button_mask(TOUCHPAD) == 0x100000u);
    assert(pw_game_button_mask(PW_GAME_BUTTON_COUNT) == 0);
}

/* Nothing bound and no mode set: xinput, so the DualSense reaches games
 * that read a gamepad while the keyboard and mouse still work. */
static void test_default_mode(void)
{
    PwGameInput input;
    PwGameProfile p;

    pw_game_input_init(&input);
    pw_game_input_default_mode(&input);
    assert(input.mode == PW_GAME_INPUT_XINPUT);

    assert(parse(APP "[display]\ndesktop = 800x600\n", &p) == PW_OK);
    pw_game_input_init(&input);
    pw_game_input_overlay(&input, &p.input);
    pw_game_input_default_mode(&input);
    assert(input.mode == PW_GAME_INPUT_XINPUT);

    /* A binding, a pointer stick or an explicit mode keeps keyboard mode. */
    assert(parse(APP "[input]\ncross = space\n", &p) == PW_OK);
    pw_game_input_init(&input);
    pw_game_input_overlay(&input, &p.input);
    pw_game_input_default_mode(&input);
    assert(input.mode == PW_GAME_INPUT_KEYBOARD);
    assert(parse(APP "[input]\nmouse = left_stick\n", &p) == PW_OK);
    pw_game_input_init(&input);
    pw_game_input_overlay(&input, &p.input);
    pw_game_input_default_mode(&input);
    assert(input.mode == PW_GAME_INPUT_KEYBOARD);
    assert(parse(APP "[input]\nmode = keyboard\n", &p) == PW_OK);
    pw_game_input_init(&input);
    pw_game_input_overlay(&input, &p.input);
    pw_game_input_default_mode(&input);
    assert(input.mode == PW_GAME_INPUT_KEYBOARD);
    assert(parse(APP "[input]\nmode = playstation\n", &p) == PW_OK);
    pw_game_input_init(&input);
    pw_game_input_overlay(&input, &p.input);
    pw_game_input_default_mode(&input);
    assert(input.mode == PW_GAME_INPUT_PLAYSTATION);

    /* A preset's bindings count too. */
    pw_game_input_init(&input);
    assert(preset("[input]\nl1 = z\n", &input) == PW_OK);
    pw_game_input_default_mode(&input);
    assert(input.mode == PW_GAME_INPUT_KEYBOARD);
    pw_game_input_default_mode(NULL);
}

/* The forms the published profiles use (the prospero-win-profiles
 * repository): a preset gives Pinball its keys, Minesweeper shares the
 * mouse preset, the 7-Zip benchmark passes an arguments line, and the
 * gamepad preset binds nothing. */
static void test_published_forms(void)
{
    PwGameProfile p;
    PwGameInput input;

    assert(parse(APP "\n[display]\ndesktop = 800x600\nscaling = fit\n\n[input]\npreset = pinball\n",
                 &p) == PW_OK);
    assert(!strcmp(p.app.id, "pinball") && !strcmp(p.input.preset, "pinball"));
    assert(p.display.width == 800 && p.display.height == 600 && p.display.scaling == PW_GAME_SCALING_FIT);
    pw_game_input_init(&input);
    assert(preset("; comment\n[input]\nmode = keyboard\nl1 = z\nr1 = slash\ncross = space\n"
                  "left = x\nright = period\nup = up\noptions = f3\nsquare = f2\n"
                  "mouse = right_stick\nmouse_speed = 900\nr2 = mouse_left\nl2 = mouse_right\n",
                  &input) == PW_OK);
    pw_game_input_overlay(&input, &p.input);
    assert(input.bindings[L1].code == 'Z' && input.bindings[R1].code == 0xbf);
    assert(input.bindings[CROSS].code == 0x20 && input.mouse == PW_GAME_STICK_RIGHT);
    assert(input.bindings[R2].kind == PW_GAME_BIND_MOUSE && input.bindings[R2].code == 0);

    assert(parse("[application]\nid = minesweeper\nname = Minesweeper\n"
                 "executable = C:\\windows\\system32\\winemine.exe\n"
                 "working_directory = C:\\windows\\system32\nprefix = default\n"
                 "runtime = wine-wow64\narchitecture = pe64\ngraphics = gdi\n\n[input]\npreset = mouse\n",
                 &p) == PW_OK);
    assert(!strcmp(p.input.preset, "mouse") && p.app.architecture == PW_APP_ARCH_PE64);
    pw_game_input_init(&input);
    assert(preset("[input]\nmode = keyboard\nmouse = left_stick\nmouse_speed = 900\n"
                  "cross = mouse_left\ncircle = mouse_right\nsquare = mouse_middle\n"
                  "r2 = mouse_left\nl2 = mouse_right\n", &input) == PW_OK);
    assert(input.mouse == PW_GAME_STICK_LEFT && input.bindings[CROSS].kind == PW_GAME_BIND_MOUSE);
    assert(input.bindings[CIRCLE].code == 1 && input.bindings[SQUARE].code == 2);

    assert(parse("[application]\nid = sevenzip-bench\nname = 7-Zip benchmark\n"
                 "executable = C:\\Tools\\7za.exe\nworking_directory = C:\\Tools\n"
                 "arguments = b -mmt1 -md22\nprefix = default\nruntime = wine-wow64\n"
                 "architecture = pe32\ngraphics = gdi\n", &p) == PW_OK);
    assert(!strcmp(p.app.id, "sevenzip-bench") && !strcmp(p.app.arguments, "b -mmt1 -md22"));
    assert(p.app.architecture == PW_APP_ARCH_PE32 && !p.input.preset[0]);

    pw_game_input_init(&input);
    assert(preset("[input]\nmode = xinput\nmouse = none\n", &input) == PW_OK);
    assert(input.mode == PW_GAME_INPUT_XINPUT && input.mouse == PW_GAME_STICK_NONE);
    for (size_t i = 0; i < PW_GAME_BUTTON_COUNT; i++) assert(input.bindings[i].kind == PW_GAME_BIND_UNSET);
}

/* [debug] env: extra diagnostics variables, only of the families graphics
 * and the native CPU read, never one the title sets itself. */
static void test_debug_env(void)
{
    PwGameProfile p;
    PwGameEnv env[PW_GAME_DEBUG_ENV_MAX];

    assert(parse(APP, &p) == PW_OK && p.debug_env_count == 0 && pw_game_debug_env(&p, env) == 0);
    assert(parse(APP "[debug]\nwinedebug = +fps\nenv = PW_NATIVE_PROFILE=1\n"
                 "env = PW_VK_BATCH_STATS=1\nenv = DXVK_LOG_PATH=C:/logs\nenv = MESA_LOADER_DRIVER_OVERRIDE=zink\n",
                 &p) == PW_OK);
    assert(!strcmp(p.winedebug, "+fps") && pw_game_debug_env(&p, env) == 4);
    assert(!strcmp(env[0].name, "PW_NATIVE_PROFILE") && !strcmp(env[0].value, "1"));
    assert(!strcmp(env[1].name, "PW_VK_BATCH_STATS") && !strcmp(env[1].value, "1"));
    assert(!strcmp(env[2].name, "DXVK_LOG_PATH") && !strcmp(env[2].value, "C:/logs"));
    assert(!strcmp(env[3].name, "MESA_LOADER_DRIVER_OVERRIDE") && !strcmp(env[3].value, "zink"));
    assert(parse(APP "[debug]\nenv = RADV_DEBUG=nocache,hang\nenv = VK_LOADER_DEBUG=all\n"
                 "env = GALLIUM_PRINT_OPTIONS=1\nenv = PW_VK_BATCH_MASK=0x7f\n", &p) == PW_OK);
    assert(p.debug_env_count == 4 && !strcmp(p.debug_env[0].value, "nocache,hang"));
    /* Zink's own options, for the OpenGL games it draws. */
    assert(parse(APP "[debug]\nenv = ZINK_DESCRIPTORS=lazy\n", &p) == PW_OK);
    assert(p.debug_env_count == 1 && !strcmp(p.debug_env[0].name, "ZINK_DESCRIPTORS") &&
           !strcmp(p.debug_env[0].value, "lazy"));
    /* vkd3d-proton's own options, the feature level a Direct3D 12 game
     * insists on among them. */
    assert(parse(APP "[debug]\nenv = VKD3D_FEATURE_LEVEL=12_0\nenv = VKD3D_CONFIG=nodxr\n", &p) == PW_OK);
    assert(p.debug_env_count == 2 && !strcmp(p.debug_env[0].name, "VKD3D_FEATURE_LEVEL") &&
           !strcmp(p.debug_env[0].value, "12_0") && !strcmp(p.debug_env[1].value, "nodxr"));
    assert(parse(APP "[debug]\nenv = VKD3DX=1\n", &p) == PW_ERR_UNSUPPORTED);
    /* A value may itself hold '=': only the first one splits. */
    assert(parse(APP "[debug]\nenv = DXVK_CONFIG=d3d9.maxFrameRate=60\n", &p) == PW_OK);
    assert(!strcmp(p.debug_env[0].name, "DXVK_CONFIG") && !strcmp(p.debug_env[0].value, "d3d9.maxFrameRate=60"));
    /* Five lines fit at once: a config file path beside the profiler and timing diagnostics. */
    assert(parse(APP "[debug]\nenv = DXVK_LOG_LEVEL=info\n"
                 "env = DXVK_LOG_PATH=Z:/logs\nenv = DXVK_CONFIG_FILE=C:/Games/GTASA/dxvk.conf\n"
                 "env = PW_VK_BATCH_STATS=1\nenv = PW_NATIVE_PROFILE=1\n", &p) == PW_OK);
    assert(pw_game_debug_env(&p, env) == 5);
    assert(!strcmp(env[0].name, "DXVK_LOG_LEVEL") && !strcmp(env[0].value, "info"));
    assert(!strcmp(env[1].name, "DXVK_LOG_PATH") && !strcmp(env[1].value, "Z:/logs"));
    assert(!strcmp(env[2].name, "DXVK_CONFIG_FILE") && !strcmp(env[2].value, "C:/Games/GTASA/dxvk.conf"));
    assert(!strcmp(env[3].name, "PW_VK_BATCH_STATS") && !strcmp(env[3].value, "1"));
    assert(!strcmp(env[4].name, "PW_NATIVE_PROFILE") && !strcmp(env[4].value, "1"));
    /* The HUD variables only when show_fps does not set them already. */
    assert(parse(APP "[debug]\nenv = DXVK_HUD=fps,gpuload\n", &p) == PW_ERR_UNSUPPORTED);
    assert(parse(APP "[display]\nshow_fps = false\n[debug]\nenv = DXVK_HUD=fps,gpuload\n", &p) == PW_OK);
    assert(!strcmp(p.debug_env[0].value, "fps,gpuload"));
    assert(parse(APP "[debug]\nenv = GALLIUM_HUD=fps\n[display]\nshow_fps = true\n", &p) == PW_ERR_UNSUPPORTED);
    /* Refused: names the title sets, other families, bad names and values,
     * duplicates, a sixth line, empty or oversized parts. */
    const char *refused[] = {
        "env = PW_VK_BATCH=0\n", "env = PW_INPUT_SHARED_FAST=1\n", "env = PW_QPC_TSC_HZ=1\n",
        "env = PW_QPC_TSC_VALIDATED=1\n", "env = GALLIUM_DRIVER=llvmpipe\n", "env = WINEDEBUG=+all\n",
        "env = WINE_PS5_WOW64_CPU=wowprospero.dll\n", "env = LD_PRELOAD=/x\n", "env = SDL_X=1\n",
        "env = pw_native_profile=1\n", "env = PW_X Y=1\n", "env = PW_X=a b\n", "env = PW_X=$HOME\n",
        "env = PW_X=\n", "env = =1\n", "env = PW_X\n", "env = PW_X=1\nenv = PW_X=2\n",
        "env = PW_A=1\nenv = PW_B=1\nenv = PW_C=1\nenv = PW_D=1\nenv = PW_E=1\nenv = PW_F=1\n",
        "env = PW_" "0123456789012345678901234567890123456789012345=1\n",
        "env = PW_X=" "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456\n",
    };
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        char text[2048];
        snprintf(text, sizeof(text), APP "[debug]\n%s", refused[i]);
        assert(parse(text, &p) != PW_OK);
    }
    /* The title's environment has room for every profile variable at once. */
    assert(PW_GAME_DEBUG_ENV_MAX == 5);
    assert(7 + 6 + PW_GAME_GRAPHICS_ENV_MAX + PW_GAME_DISPLAY_ENV_MAX + PW_GAME_RUNTIME_ENV_MAX +
           PW_GAME_VERSION_ENV_MAX + PW_GAME_CPU_ENV_MAX + PW_GAME_CLOCK_ENV_MAX + PW_GAME_DEBUG_ENV_MAX + PW_GAME_ENVIRONMENT_MAX <=
           PW_WINE_START_MAX_ENV);
}

/* [environment]: more variables for the game, by Playden's rules
 * (IsProsperoEnvironmentEntry), in file order. */
/* WINE_NTOSKRNL_HOSTED per mode, architecture and PW_NSIPROXY_DISABLE. */
static void test_ntoskrnl_hosted(void)
{
    static const char app64[] = "[application]\nid = sts\nname = Slay\nexecutable = C:\\Games\\Sts.exe\n"
                                "working_directory = C:\\Games\nprefix = default\nruntime = wine-wow64\n"
                                "architecture = pe64\ngraphics = gdi\n";
    char text[512];
    PwGameProfile p;

    /* 32-bit: the HID drivers in the pad modes, nothing in the others */
    assert(parse(APP, &p) == PW_OK && !pw_game_nsiproxy_disabled(&p));
    assert(!strcmp(pw_game_ntoskrnl_hosted(&p, 1), "1") && !pw_game_ntoskrnl_hosted(&p, 0));
    assert(parse(APP "[environment]\nPW_NSIPROXY_DISABLE = 1\n", &p) == PW_OK && pw_game_nsiproxy_disabled(&p));
    assert(!strcmp(pw_game_ntoskrnl_hosted(&p, 1), "winebus") && !pw_game_ntoskrnl_hosted(&p, 0));
    /* 64-bit: nsiproxy after the HID drivers, or alone; the switch keeps the HID drivers */
    assert(parse(app64, &p) == PW_OK);
    assert(!strcmp(pw_game_ntoskrnl_hosted(&p, 1), "1") && !strcmp(pw_game_ntoskrnl_hosted(&p, 0), "nsiproxy"));
    snprintf(text, sizeof(text), "%s[environment]\nPW_NSIPROXY_DISABLE = 1\n", app64);
    assert(parse(text, &p) == PW_OK);
    assert(!strcmp(pw_game_ntoskrnl_hosted(&p, 1), "winebus") && !pw_game_ntoskrnl_hosted(&p, 0));
    snprintf(text, sizeof(text), "%s[environment]\nPW_NSIPROXY_DISABLE = 0\n", app64);
    assert(parse(text, &p) == PW_OK);
    assert(!strcmp(pw_game_ntoskrnl_hosted(&p, 1), "1") && !strcmp(pw_game_ntoskrnl_hosted(&p, 0), "nsiproxy"));
    /* no profile: the pad modes still get their drivers */
    assert(!pw_game_nsiproxy_disabled(NULL) && !strcmp(pw_game_ntoskrnl_hosted(NULL, 1), "1") &&
           !pw_game_ntoskrnl_hosted(NULL, 0));
}

static void test_environment(void)
{
    PwGameProfile p;
    PwGameEnv env[PW_GAME_ENVIRONMENT_MAX];

    assert(parse(APP, &p) == PW_OK && p.environment_count == 0 && pw_game_environment_env(&p, env) == 0);
    assert(pw_game_environment_env(NULL, env) == 0 && pw_game_environment_env(&p, NULL) == 0);
    /* Only this media switch is admitted from the PW_ family. */
    assert(parse(APP "[environment]\nPW_MEDIA_DISABLE = 1\n", &p) == PW_OK);
    assert(pw_game_environment_env(&p, env) == 1);
    assert(!strcmp(env[0].name, "PW_MEDIA_DISABLE") && !strcmp(env[0].value, "1"));
    assert(parse(APP "[environment]\nPW_MEDIA_DISABLE = 0\n", &p) == PW_OK);
    assert(pw_game_environment_env(&p, env) == 1 && !strcmp(env[0].value, "0"));
    /* and the nsiproxy switch, also exactly 0 or 1 */
    assert(parse(APP "[environment]\nPW_NSIPROXY_DISABLE = 1\n", &p) == PW_OK);
    assert(pw_game_environment_env(&p, env) == 1);
    assert(!strcmp(env[0].name, "PW_NSIPROXY_DISABLE") && !strcmp(env[0].value, "1"));
    assert(parse(APP "[environment]\nPW_NSIPROXY_DISABLE = 0\n", &p) == PW_OK && !pw_game_nsiproxy_disabled(&p));
    /* Playden's address space row writes this one, as exactly 0 or 1 */
    assert(parse(APP "[environment]\nWINE_LARGE_ADDRESS_AWARE = 1\n", &p) == PW_OK);
    assert(pw_game_environment_env(&p, env) == 1);
    assert(!strcmp(env[0].name, "WINE_LARGE_ADDRESS_AWARE") && !strcmp(env[0].value, "1"));
    /* and the open-file budgets of patch 0993, as up to four digits */
    assert(parse(APP "[environment]\nWINE_PS5_PARK_FILES = 96\nWINE_PS5_CACHED_FILES = 0\n", &p) == PW_OK);
    assert(pw_game_environment_env(&p, env) == 2);
    assert(!strcmp(env[0].name, "WINE_PS5_PARK_FILES") && !strcmp(env[0].value, "96"));
    assert(!strcmp(env[1].name, "WINE_PS5_CACHED_FILES") && !strcmp(env[1].value, "0"));
    /* Playden's CPU cores row writes this one */
    assert(parse(APP "[environment]\nWINE_CPU_TOPOLOGY = 4:0,1,2,3\nDXVK_FRAME_RATE = 60\n"
                 "SDL_HINT_X = a b=c\nRADV_PERFTEST = gpl\nMESA_SHADER_CACHE_DIR = Z:/cache\n", &p) == PW_OK);
    assert(pw_game_environment_env(&p, env) == 5);
    assert(!strcmp(env[0].name, "WINE_CPU_TOPOLOGY") && !strcmp(env[0].value, "4:0,1,2,3"));
    assert(!strcmp(env[1].name, "DXVK_FRAME_RATE") && !strcmp(env[1].value, "60"));
    assert(!strcmp(env[2].name, "SDL_HINT_X") && !strcmp(env[2].value, "a b=c"));
    assert(!strcmp(env[4].name, "MESA_SHADER_CACHE_DIR"));
    /* anywhere after [application], beside the other sections */
    assert(parse(APP "[environment]\nDXVK_FRAME_RATE = 30\n[display]\nshow_fps = false\n"
                 "[debug]\nenv = PW_NATIVE_PROFILE=1\n", &p) == PW_OK);
    assert(p.environment_count == 1 && p.debug_env_count == 1 && !p.display.show_fps);
    /* eight fit */
    assert(parse(APP "[environment]\nDXVK_A = 1\nDXVK_B = 1\nDXVK_C = 1\nDXVK_D = 1\nDXVK_E = 1\n"
                 "DXVK_F = 1\nDXVK_G = 1\nDXVK_H = 1\n", &p) == PW_OK);
    assert(pw_game_environment_env(&p, env) == PW_GAME_ENVIRONMENT_MAX);
    {
        static const char *refused[] = {
            "WINEPREFIX = C:\\x\n",                /* steers Wine or the loader */
            "WINEDLLOVERRIDES = d3d9=n\n", "WINEDEBUG = +all\n", "WINE_PS5_SCHED = 1\n",
            "LD_PRELOAD = x\n", "LD_LIBRARY_PATH = /x\n", "PATH = /x\n", "HOME = /tmp\n",
            "PW_MEDIA_DISABLE = 2\n", "PW_MEDIA_DISABLE = -1\n", "PW_MEDIA_DISABLE = +1\n",
            "PW_MEDIA_DISABLE = true\n", "PW_MEDIA_DISABLE = false\n", "PW_MEDIA_DISABLE = 00\n",
            "PW_MEDIA_DISABLE = 01\n", "PW_MEDIA_DISABLE = 1 0\n", "PW_MEDIA_DISABLE =\n",
            "PW_MEDIA_DISABLE_X = 1\n", "PW_NATIVE_PROFILE = 1\n", "PW_WINE64_MODE = game\n",
            "PW_MEDIA_DISABLE = 1\nPW_MEDIA_DISABLE = 0\n",
            "PW_NSIPROXY_DISABLE = 2\n", "PW_NSIPROXY_DISABLE = true\n", "PW_NSIPROXY_DISABLE = 01\n",
            "PW_NSIPROXY_DISABLE =\n", "PW_NSIPROXY = 1\n", "PW_NSIPROXY_DISABLE = 1\nPW_NSIPROXY_DISABLE = 1\n",
            "WINE_LARGE_ADDRESS_AWARE = 2\n", "WINE_LARGE_ADDRESS_AWARE = yes\n", "WINE_LARGE_ADDRESS_AWARE = 11\n",
            "WINE_PS5_PARK_FILES = -1\n", "WINE_PS5_PARK_FILES = 12345\n", "WINE_PS5_CACHED_FILES = 4x\n",
            "WINE_PS5_PARK_DIRECTORIES = 0\n", "WINE_LARGE_ADDRESS = 1\n",
            "PW_VK_BATCH = 0\n", "GALLIUM_DRIVER = llvmpipe\n", /* [debug] env's families */
            "DXVK_HUD = full\n", "SDL_JOYSTICK_RAWINPUT = 1\n", /* the title sets them */
            "DXVK_ = 1\n", "SDL_ = 1\n",                     /* a family alone */
            "dxvk_frame_rate = 60\n", "DXVK-X = 1\n", "1DXVK = 1\n", "DXVK X = 1\n",
            "DXVK_FRAME_RATE =\n", "= 1\n", "DXVK_FRAME_RATE\n",
            "DXVK_CONFIG = \"x\"\n", "DXVK_CONFIG = 'x'\n", "DXVK_CONFIG = a\tb\n",
            "DXVK_FRAME_RATE = 60\nDXVK_FRAME_RATE = 30\n",
            "DXVK_A = 1\nDXVK_B = 1\nDXVK_C = 1\nDXVK_D = 1\nDXVK_E = 1\nDXVK_F = 1\nDXVK_G = 1\nDXVK_H = 1\n"
            "DXVK_I = 1\n",
            "DXVK_A = 1\n[environment]\n",
            "DXVK_" "01234567890123456789012345678901234567890123456789012345678 = 1\n",
            "DXVK_X = " "0123456789012345678901234567890123456789012345678901234567890123456789"
            "012345678901234567890123456789012345678901234567890123456789\n",
        };
        for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
            char text[2048];
            snprintf(text, sizeof(text), APP "[environment]\n%s", refused[i]);
            assert(parse(text, &p) != PW_OK);
        }
    }
    /* the longest name and value fit */
    assert(parse(APP "[environment]\nDXVK_" "0123456789012345678901234567890123456789012345678901234567 = "
                 "0123456789012345678901234567890123456789012345678901234567890123456789"
                 "012345678901234567890123456789012345678901234567890123456\n", &p) == PW_OK);
    assert(strlen(p.environment[0].name) == 63 && strlen(p.environment[0].value) == 127);
    /* one variable, one section */
    assert(parse(APP "[debug]\nenv = DXVK_LOG_LEVEL=info\n[environment]\nDXVK_LOG_LEVEL = none\n", &p) ==
           PW_ERR_MALFORMED);
    /* a preset file holds [input] only */
    {
        PwGameInput input;
        pw_game_input_init(&input);
        assert(preset("[input]\ncross = space\n[environment]\nDXVK_A = 1\n", &input) != PW_OK);
    }
}

/* The input map (docs/INPUT_MAP.md): its lines, its defaults and what it refuses. */
static void test_input_map(void)
{
    PwGameProfile p;
    PwGameMapTarget t;

    /* the grammar's own example */
    assert(parse(APP "[input]\nmode = keyboard\nmap = 1\ncross = key:space\ncircle = key:ctrl\nsquare = key:r\n"
                 "triangle = key:e\nl2 = mouse:right\nr2 = mouse:left\nr2_threshold = 30\nup = mouse:wheel_up\n"
                 "down = mouse:wheel_down\nleft_stick = keys\nleft_stick_keys = w,s,a,d\nright_stick = mouse\n"
                 "right_stick_speed = 1500\n", &p) == PW_OK);
    assert(p.input.map.on && p.input.mode == PW_GAME_INPUT_KEYBOARD);
    t = p.input.map.inputs[PW_GAME_MAP_CROSS];
    assert(t.kind == PW_GAME_MAP_KEYS && t.count == 1 && t.keys[0] == 0x20);
    assert(p.input.map.inputs[PW_GAME_MAP_CIRCLE].keys[0] == 0x11);
    assert(p.input.map.inputs[PW_GAME_MAP_SQUARE].keys[0] == 'R');
    t = p.input.map.inputs[PW_GAME_MAP_R2];
    assert(t.kind == PW_GAME_MAP_MOUSE && t.code == PW_GAME_MOUSE_LEFT && p.input.map.threshold[1] == 30 &&
           p.input.map.threshold[0] == 0);
    assert(p.input.map.inputs[PW_GAME_MAP_UP].code == PW_GAME_MOUSE_WHEEL_UP);
    assert(p.input.map.inputs[PW_GAME_MAP_L1].kind == PW_GAME_MAP_DEFAULT);
    {
        const PwGameMapStick *l = &p.input.map.stick[0], *r = &p.input.map.stick[1];
        assert(l->mode == PW_GAME_STICK_MODE_KEYS && l->deadzone == 40 && !memcmp(l->keys, "WSAD", 4));
        assert(r->mode == PW_GAME_STICK_MODE_MOUSE && r->speed == 1500 && r->deadzone == 16 && r->outer == 0 &&
               r->curve == PW_GAME_CURVE_SQUARED && !r->invert_x && !r->invert_y);
    }
    /* no line: the input's own controller input, the touchpad by mode */
    assert(parse(APP "[input]\nmode = xinput\nmap = 1\n", &p) == PW_OK && p.input.map.on);
    t = pw_game_map_target(&p.input.map, PW_GAME_MAP_CROSS, PW_GAME_INPUT_XINPUT);
    assert(t.kind == PW_GAME_MAP_PAD && t.code == PW_GAME_PAD_A);
    assert(pw_game_map_target(&p.input.map, PW_GAME_MAP_CREATE, PW_GAME_INPUT_XINPUT).code == PW_GAME_PAD_BACK);
    assert(pw_game_map_target(&p.input.map, PW_GAME_MAP_TOUCHPAD_LEFT, PW_GAME_INPUT_XINPUT).code == PW_GAME_PAD_BACK);
    assert(pw_game_map_target(&p.input.map, PW_GAME_MAP_TOUCHPAD_RIGHT, PW_GAME_INPUT_XINPUT).code == PW_GAME_PAD_GUIDE);
    assert(pw_game_map_target(&p.input.map, PW_GAME_MAP_TOUCHPAD_LEFT, PW_GAME_INPUT_PLAYSTATION).code ==
           PW_GAME_PAD_TOUCHPAD);
    assert(pw_game_map_target(&p.input.map, PW_GAME_MAP_R2, PW_GAME_INPUT_KEYBOARD).code == PW_GAME_PAD_RT);
    assert(p.input.map.stick[0].mode == PW_GAME_STICK_MODE_PAD_LEFT && p.input.map.stick[1].mode ==
           PW_GAME_STICK_MODE_PAD_RIGHT && p.input.map.stick[0].deadzone == 0 &&
           p.input.map.stick[0].curve == PW_GAME_CURVE_LINEAR);
    /* a line that states the default means the same */
    assert(parse(APP "[input]\nmap = 1\ncross = pad:a\nleft_stick = pad:left_stick\nleft_stick_deadzone = 0\n"
                 "left_stick_curve = linear\n", &p) == PW_OK);
    /* without a mode line the map plays as the xinput controller, as no binding does today */
    assert(p.input.mode == PW_GAME_INPUT_KEYBOARD);
    {
        PwGameInput input;
        pw_game_input_init(&input);
        pw_game_input_overlay(&input, &p.input);
        pw_game_input_default_mode(&input);
        assert(input.map.on && input.mode == PW_GAME_INPUT_XINPUT);
    }
    /* every target form */
    assert(parse(APP "[input]\nmap = 1\ncross = key:ctrl+shift+s\ncircle = key:lctrl+rctrl+lalt+f12\n"
                 "square = key:vk:0xe2\ntriangle = mouse:back\nl1 = mouse:forward\nr1 = mouse:middle\n"
                 "l3 = none\nr3 = pad:guide\nup = pad:touchpad\ndown = pad:lt\nleft = key:semicolon\n"
                 "right = key:num0+decimal+multiply+divide\noptions = key:apps\ncreate = key:printscreen\n"
                 "touchpad_left = key:rwin\ntouchpad_right = key:f24\n", &p) == PW_OK);
    t = p.input.map.inputs[PW_GAME_MAP_CROSS];
    assert(t.count == 3 && t.keys[0] == 0x11 && t.keys[1] == 0x10 && t.keys[2] == 'S');
    t = p.input.map.inputs[PW_GAME_MAP_CIRCLE];
    assert(t.count == 4 && t.keys[0] == 0xa2 && t.keys[1] == 0xa3 && t.keys[2] == 0xa4 && t.keys[3] == 0x7b);
    assert(p.input.map.inputs[PW_GAME_MAP_SQUARE].keys[0] == 0xe2);
    assert(p.input.map.inputs[PW_GAME_MAP_TRIANGLE].code == PW_GAME_MOUSE_BACK);
    assert(p.input.map.inputs[PW_GAME_MAP_L1].code == PW_GAME_MOUSE_FORWARD);
    assert(p.input.map.inputs[PW_GAME_MAP_L3].kind == PW_GAME_MAP_NONE);
    assert(p.input.map.inputs[PW_GAME_MAP_DOWN].kind == PW_GAME_MAP_PAD &&
           p.input.map.inputs[PW_GAME_MAP_DOWN].code == PW_GAME_PAD_LT);
    assert(p.input.map.inputs[PW_GAME_MAP_LEFT].keys[0] == 0xba);
    t = p.input.map.inputs[PW_GAME_MAP_RIGHT];
    assert(t.count == 4 && t.keys[0] == 0x60 && t.keys[1] == 0x6e && t.keys[2] == 0x6a && t.keys[3] == 0x6f);
    assert(p.input.map.inputs[PW_GAME_MAP_TOUCHPAD_RIGHT].keys[0] == 0x87);
    /* stick modes and their tuning */
    assert(parse(APP "[input]\nmap = 1\nleft_stick = pad:right_stick\nleft_stick_deadzone = 20\n"
                 "left_stick_outer = 10\nleft_stick_curve = cubed\nleft_stick_invert_x = true\n"
                 "left_stick_invert_y = false\nright_stick = wheel\nright_stick_speed = 100\n"
                 "right_stick_invert_y = true\n", &p) == PW_OK);
    assert(p.input.map.stick[0].mode == PW_GAME_STICK_MODE_PAD_RIGHT && p.input.map.stick[0].deadzone == 20 &&
           p.input.map.stick[0].outer == 10 && p.input.map.stick[0].curve == PW_GAME_CURVE_CUBED &&
           p.input.map.stick[0].invert_x == 1);
    assert(p.input.map.stick[1].mode == PW_GAME_STICK_MODE_WHEEL && p.input.map.stick[1].speed == 100 &&
           p.input.map.stick[1].invert_y && p.input.map.stick[1].deadzone == 16);
    assert(parse(APP "[input]\nmap = 1\nright_stick_deadzone = 25\nright_stick = pad:dpad\n", &p) == PW_OK);
    assert(p.input.map.stick[1].mode == PW_GAME_STICK_MODE_DPAD && p.input.map.stick[1].deadzone == 25);
    assert(parse(APP "[input]\nmap = 1\nleft_stick = keys\nleft_stick_keys = up,none,none,shift\n", &p) == PW_OK);
    assert(!memcmp(p.input.map.stick[0].keys, "\x26\0\0\x10", 4));
    assert(parse(APP "[input]\nmap = 1\nr2 = pad:a\nr2_threshold = 5\nl2 = key:x\nl2_threshold = 100\n", &p) ==
           PW_OK && p.input.map.threshold[0] == 100 && p.input.map.threshold[1] == 5);
    assert(parse(APP "[input]\nmap = 1\nl2 = pad:rt\nr2 = pad:lt\n", &p) == PW_OK);
    /* mode, rumble and player2 keep their meaning */
    assert(parse(APP "[input]\nmode = playstation\nrumble = 40\nmap = 1\nplayer2 = p2\ncross = pad:b\n", &p) == PW_OK &&
           p.input.mode == PW_GAME_INPUT_PLAYSTATION && p.input.rumble == 40 && !strcmp(p.input.player2, "p2"));
    assert(parse(APP "[input]\nmap = 1\nmode = keyboard\n", &p) == PW_OK);
    /* without map = 1 they are read as before, in any case */
    assert(parse(APP "[input]\nMODE = KEYBOARD\nRumble = 050\nplayer2 = p2\n", &p) == PW_OK &&
           p.input.mode == PW_GAME_INPUT_KEYBOARD && p.input.rumble == 50 && !p.input.map.on);

    static const char *const refused[] = {
        /* where map goes */
        "[input]\nmap = 2\n", "[input]\nmap = true\n", "[input]\nmap = 1\nmap = 1\n", "[input]\nMap = 1\n",
        "[input]\ncross = space\nmap = 1\n", "[input]\ncross = none\nmap = 1\n",
        "[input]\npreset = gamepad\nmap = 1\n", "[input]\nmouse = left_stick\nmap = 1\n",
        "[input]\nmouse_speed = 900\nmap = 1\n", "[input]\nleft_stick = keys\nmap = 1\n",
        "[input]\nmap = 1\npreset = gamepad\n", "[input]\nmap = 1\nmouse = none\n",
        "[input]\nmap = 1\nmouse_speed = 900\n",
        /* [launcher] takes a map as strictly as [input] */
        "[launcher]\ncross = space\nmap = 1\n", "[launcher]\nmouse = left_stick\nmap = 1\n", "[launcher]\nmap = 1\nmap = 1\n",
        "[launcher]\nmap = 1\nmouse = left_stick\n", "[launcher]\nmap = 1\npreset = mouse\n", "[launcher]\nmap = 1\ncross = z\n",
        "[launcher]\nmap = 1\nmode = KEYBOARD\n", "[launcher]\nmap = 1\nleft_stick_speed = 900\n",
        /* the lines it keeps are as strict as its own, before it as well as after */
        "[input]\nmap = 1\nMODE = KEYBOARD\n", "[input]\nmap = 1\nmode = KEYBOARD\n", "[input]\nmap = 1\nMode = keyboard\n",
        "[input]\nmap = 1\nrumble = 050\n", "[input]\nmap = 1\nRUMBLE = 50\n", "[input]\nmap = 1\nPlayer2 = p2\n",
        "[input]\nMODE = KEYBOARD\nmap = 1\n", "[input]\nmode = Keyboard\nmap = 1\n", "[input]\nrumble = 050\nmap = 1\n",
        "[input]\nPLAYER2 = p2\nmap = 1\n",
        /* old-style values and the old touchpad */
        "[input]\nmap = 1\ncross = z\n", "[input]\nmap = 1\nr2 = mouse_left\n", "[input]\nmap = 1\ncross = vk:0x41\n",
        "[input]\nmap = 1\ntouchpad = pad:back\n", "[input]\nmap = 1\nps = none\n",
        /* case, spaces, repeats */
        "[input]\nmap = 1\ncross = KEY:space\n", "[input]\nmap = 1\nCross = key:space\n",
        "[input]\nmap = 1\ncross = key:Space\n", "[input]\nmap = 1\ncross = key:ctrl + s\n",
        "[input]\nmap = 1\ncross = key:a\ncross = key:b\n", "[input]\nmap = 1\nleft_stick_keys = w, s, a, d\nleft_stick = keys\n",
        "[input]\nmap = 1\nleft_stick = mouse\nleft_stick = mouse\n",
        "[input]\nmap = 1\nleft_stick = mouse\nleft_stick_speed = 900\nleft_stick_speed = 900\n",
        /* targets */
        "[input]\nmap = 1\ncross = pad:\n", "[input]\nmap = 1\ncross = pad:left_stick\n", "[input]\nmap = 1\ncross = pad:z\n",
        "[input]\nmap = 1\ncross = mouse:x1\n", "[input]\nmap = 1\ncross = key:\n", "[input]\nmap = 1\ncross = key:+a\n",
        "[input]\nmap = 1\ncross = key:a+\n", "[input]\nmap = 1\ncross = key:a+a\n",
        "[input]\nmap = 1\ncross = key:shift+ctrl+s\n", "[input]\nmap = 1\ncross = key:s+ctrl\n",
        "[input]\nmap = 1\ncross = key:ctrl+shift+alt+lwin+s\n", "[input]\nmap = 1\ncross = key:vk:0x20\n",
        "[input]\nmap = 1\ncross = key:vk:0x41\n", "[input]\nmap = 1\ncross = key:vk:0x00\n",
        "[input]\nmap = 1\ncross = key:vk:0xE2\n", "[input]\nmap = 1\ncross = key:vk:0x11\n",
        "[input]\nmap = 1\ncross = key:f0\n", "[input]\nmap = 1\ncross = key:f25\n", "[input]\nmap = 1\ncross = key:f01\n",
        "[input]\nmap = 1\ncross = key:none\n", "[input]\nmap = 1\ncross = key:;\n", "[input]\nmap = 1\ncross = key:numpad0\n",
        /* thresholds */
        "[input]\nmap = 1\nr2_threshold = 30\n", "[input]\nmap = 1\nr2 = pad:rt\nr2_threshold = 30\n",
        "[input]\nmap = 1\nr2 = pad:lt\nr2_threshold = 30\n", "[input]\nmap = 1\nr2 = none\nr2_threshold = 30\n",
        "[input]\nmap = 1\nr2 = key:a\nr2_threshold = 4\n", "[input]\nmap = 1\nr2 = key:a\nr2_threshold = 101\n",
        "[input]\nmap = 1\nr2 = key:a\nr2_threshold = 030\n", "[input]\nmap = 1\ncross_threshold = 30\n",
        /* sticks */
        "[input]\nmap = 1\nleft_stick = pad:left\n", "[input]\nmap = 1\nleft_stick = mouse_left\n",
        "[input]\nmap = 1\nleft_stick = keys\n", "[input]\nmap = 1\nleft_stick_keys = w,s,a,d\n",
        "[input]\nmap = 1\nleft_stick = mouse\nleft_stick_keys = w,s,a,d\n",
        "[input]\nmap = 1\nleft_stick = keys\nleft_stick_keys = w,s,a\n",
        "[input]\nmap = 1\nleft_stick = keys\nleft_stick_keys = w,s,a,d,e\n",
        "[input]\nmap = 1\nleft_stick = keys\nleft_stick_keys = w,w,a,d\n",
        "[input]\nmap = 1\nleft_stick = keys\nleft_stick_keys = w,s,a,ctrl+d\n",
        "[input]\nmap = 1\nleft_stick = keys\nleft_stick_keys = key:w,s,a,d\n",
        "[input]\nmap = 1\nleft_stick = keys\nleft_stick_keys = w,s,a,d\nleft_stick_outer = 5\n",
        "[input]\nmap = 1\nleft_stick = keys\nleft_stick_keys = w,s,a,d\nleft_stick_deadzone = 5\n",
        "[input]\nmap = 1\nleft_stick = pad:dpad\nleft_stick_curve = linear\n",
        "[input]\nmap = 1\nleft_stick_speed = 900\n", "[input]\nmap = 1\nleft_stick = pad:dpad\nleft_stick_speed = 9\n",
        "[input]\nmap = 1\nleft_stick = none\nleft_stick_deadzone = 10\n",
        "[input]\nmap = 1\nleft_stick = none\nleft_stick_invert_y = false\n",
        "[input]\nmap = 1\nleft_stick = wheel\nleft_stick_invert_x = true\n",
        "[input]\nmap = 1\nleft_stick = wheel\nleft_stick_speed = 101\n",
        "[input]\nmap = 1\nleft_stick = mouse\nleft_stick_speed = 0\n",
        "[input]\nmap = 1\nleft_stick = mouse\nleft_stick_speed = 20001\n",
        "[input]\nmap = 1\nleft_stick_deadzone = 91\n", "[input]\nmap = 1\nleft_stick_outer = 51\n",
        "[input]\nmap = 1\nleft_stick_deadzone = 50\nleft_stick_outer = 41\n",
        "[input]\nmap = 1\nleft_stick_curve = quadratic\n", "[input]\nmap = 1\nleft_stick_invert_x = 1\n",
        "[input]\nmap = 1\nleft_stick_deadzone = 08\n", "[input]\nmap = 1\nleft_stick_dead = 8\n",
        "[input]\nmap = 1\nstick_deadzone = 8\n", "[input]\nmap = 1\nleft_stick_ = 8\n",
    };
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        char text[1024];
        snprintf(text, sizeof(text), "%s%s", APP, refused[i]);
        if (parse(text, &p) == PW_OK) {
            fprintf(stderr, "accepted: %s\n", refused[i]);
            assert(0);
        }
    }
    assert(parse(APP "[input]\nmap = 1\nleft_stick_deadzone = 50\nleft_stick_outer = 40\n", &p) == PW_OK);
    /* the map is the profile's alone: never in a preset file, and the tuning lines need it */
    {
        PwGameInput input;
        pw_game_input_init(&input);
        assert(preset("[input]\nmap = 1\n", &input) != PW_OK && !input.map.on);
        assert(preset("[input]\ncross = key:space\n", &input) != PW_OK);
    }
    assert(parse(APP "[input]\nleft_stick = keys\n", &p) != PW_OK);
    assert(parse(APP "[input]\ntouchpad_left = z\n", &p) != PW_OK);
    /* [launcher] takes its own map: the touchpad hold swaps a layout for another one, and either side's mode */
    assert(parse(APP "[input]\nmode = xinput\nmap = 1\ncross = pad:b\n[launcher]\nchain = off\nlabel = Menus\n"
                 "game_label = Custom\nmode = keyboard\nmap = 1\ncross = mouse:left\nleft_stick = mouse\n"
                 "left_stick_speed = 900\n", &p) == PW_OK &&
           p.input.map.on && p.launcher_input.map.on && p.launcher_chain_off &&
           p.launcher_input.mode == PW_GAME_INPUT_KEYBOARD && !strcmp(p.launcher_label, "Menus"));
    /* and beside the old lines in [input] too */
    assert(parse(APP "[input]\nmode = xinput\ncross = space\n[launcher]\nchain = off\nmap = 1\ncross = key:enter\n", &p) ==
               PW_OK && !p.input.map.on && p.launcher_input.map.on);
    /* [launcher] keeps the old lines beside a mapped [input] */
    assert(parse(APP "[input]\nmode = xinput\nmap = 1\ncross = pad:b\n[launcher]\nmode = keyboard\nmouse = left_stick\n"
                 "cross = mouse_left\n", &p) == PW_OK && p.input.map.on && !p.launcher_input.map.on &&
           p.launcher_input.bindings[CROSS].kind == PW_GAME_BIND_MOUSE);
}

int main(void)
{
    test_input_map();
    test_graphics_env();
    test_published_forms();
    test_profile();
    test_display_env();
    test_runtime();
    test_runtime_locales();
    test_version();
    test_cpu();
    test_debug_env();
    test_environment();
    test_ntoskrnl_hosted();
    test_refusals();
    test_audio();
    test_presets();
    test_default_mode();
    printf("game profile passed: application plus display and input, every binding kind, "
           "the video output's refresh and frame rate, runtime settings and their environment, the CPU backend choice, debug env and [environment] variables, refusals, shared presets overridden by the profile, the input map\n");
    return 0;
}
