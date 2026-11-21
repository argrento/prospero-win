/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_hud.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define MS 1000000ull
#define S 1000000000ull

static uint32_t pixels[PW_HUD_MAX_W * PW_HUD_MAX_H];

/* frames evenly over seconds, from start: the time of the last one */
static uint64_t run(PwHudStats *stats, uint64_t start, uint32_t frames_a_second, uint32_t seconds, uint32_t shown)
{
    uint64_t t = start;
    for (uint64_t i = 1; i <= (uint64_t)frames_a_second * seconds; i++) {
        t = start + i * S / frames_a_second;
        (void)pw_hud_frame(stats, t, shown);
    }
    return t;
}

static int text_has(const PwHudStats *stats, PwHudLevel level, int framegen, const char *want)
{
    char lines[PW_HUD_LINES][PW_HUD_LINE_CHARS + 1];
    size_t n = pw_hud_text(stats, level, framegen, lines);
    for (size_t i = 0; i < n; i++)
        if (!strcmp(lines[i], want)) return 1;
    for (size_t i = 0; i < n; i++) fprintf(stderr, "  line %zu: '%s'\n", i, lines[i]);
    fprintf(stderr, "  wanted '%s'\n", want);
    return 0;
}

int main(void)
{
    assert(pw_hud_level_parse("fps") == PW_HUD_FPS);
    assert(pw_hud_level_parse("frametimes") == PW_HUD_FRAMETIMES);
    assert(pw_hud_level_parse("full") == PW_HUD_FULL);
    assert(pw_hud_level_parse("off") == PW_HUD_OFF && pw_hud_level_parse("1") == PW_HUD_OFF &&
           pw_hud_level_parse(NULL) == PW_HUD_OFF && pw_hud_level_parse("FPS") == PW_HUD_OFF);

    /* Nothing before a whole second has passed. */
    PwHudStats stats;
    memset(&stats, 0, sizeof(stats));
    char lines[PW_HUD_LINES][PW_HUD_LINE_CHARS + 1];
    assert(pw_hud_text(&stats, PW_HUD_FULL, 0, lines) == 0);
    uint32_t w = 1, h = 1;
    pw_hud_draw(&stats, PW_HUD_FULL, 0, pixels, PW_HUD_MAX_W, &w, &h);
    assert(w == 0 && h == 0);

    /* The first frame starts the count: 60 frames after it in one second is 60.0. */
    uint64_t t = 5 * S;
    assert(!pw_hud_frame(&stats, t, 1));
    for (int i = 1; i < 60; i++) assert(!pw_hud_frame(&stats, t + (uint64_t)i * S / 60, 1));
    assert(pw_hud_frame(&stats, t + S, 1));
    assert(stats.fps10 == 600 && stats.frame_us == 16666 && stats.measured);
    assert(text_has(&stats, PW_HUD_FPS, 0, "FPS 60.0"));
    assert(pw_hud_text(&stats, PW_HUD_FPS, 0, lines) == 1);
    assert(pw_hud_text(&stats, PW_HUD_OFF, 0, lines) == 0);
    t += S;

    /* The window: 9 more seconds at 30 fill it: min 30, max 60, the average of all its frames. */
    t = run(&stats, t, 30, 9, 1);
    uint32_t lo, avg, hi;
    assert(pw_hud_window(&stats, &lo, &avg, &hi));
    assert(lo == 300 && hi == 600 && avg == 330 && stats.window_count == PW_HUD_WINDOW_S);
    /* A second at 45 pushes the oldest, the 60, out. */
    t = run(&stats, t, 45, 1, 1);
    assert(pw_hud_window(&stats, &lo, &avg, &hi) && lo == 300 && hi == 450 && avg == 315);
    t = run(&stats, t, 30, 1, 1);
    assert(text_has(&stats, PW_HUD_FRAMETIMES, 0, "FPS 30.0  33.3 MS"));
    assert(text_has(&stats, PW_HUD_FRAMETIMES, 0, "MIN 30.0 AVG 31.5 MAX 45.0"));
    /* frametimes has no full lines */
    assert(pw_hud_text(&stats, PW_HUD_FRAMETIMES, 0, lines) == 2);

    /* Frame generation: the game's frames, then the pictures shown. */
    t = run(&stats, t, 30, 1, 2);
    assert(text_has(&stats, PW_HUD_FPS, 1, "FPS 30.0>60.0"));

    /* full: a line for each stat someone gave. */
    assert(pw_hud_text(&stats, PW_HUD_FULL, 0, lines) == 2);
    stats.render_w = 1600; stats.render_h = 900;
    assert(text_has(&stats, PW_HUD_FULL, 0, "RES 1600x900"));
    stats.output_w = 3840; stats.output_h = 2160;
    assert(text_has(&stats, PW_HUD_FULL, 0, "RES 1600x900>3840x2160"));
    stats.output_w = 1600; stats.output_h = 900;
    assert(text_has(&stats, PW_HUD_FULL, 0, "RES 1600x900"));
    pw_hud_cpu(&stats, 1000000, t);
    assert(!stats.cpu_measured);
    pw_hud_cpu(&stats, 1000000 + 2120000, t + S);   /* 2.12 s of CPU in one second */
    assert(stats.cpu_measured && stats.cpu10 == 2120);
    assert(text_has(&stats, PW_HUD_FULL, 0, "CPU 212%"));
    stats.cpus = 13;
    assert(text_has(&stats, PW_HUD_FULL, 0, "CPU 212% (16% OF 13 CORES)"));
    stats.memory_known = 1;
    stats.memory_bytes = 5476083302ull;        /* 5.1 GiB */
    stats.memory_peak_bytes = 512ull << 20;
    assert(text_has(&stats, PW_HUD_FULL, 0, "MEM 5.1 GB  PEAK 512 MB"));
    assert(pw_hud_text(&stats, PW_HUD_FULL, 0, lines) == 5);
    for (size_t i = 0; i < 5; i++) assert(strlen(lines[i]) <= PW_HUD_LINE_CHARS);

    /* The picture: black panel, glyph pixels light blue (letters) and white (digits). */
    memset(pixels, 0x55, sizeof(pixels));
    pw_hud_draw(&stats, PW_HUD_FPS, 0, pixels, PW_HUD_MAX_W, &w, &h);
    assert(w == strlen("FPS 30.0") * PW_HUD_CELL_W + 2 * PW_HUD_PAD && h == PW_HUD_CELL_H + 2 * PW_HUD_PAD);
    assert(pixels[0] == 0xFF000000u && pixels[(h - 1) * PW_HUD_MAX_W + w - 1] == 0xFF000000u);
    /* F's top row is five pixels from the pad, one row down */
    for (uint32_t x = 0; x < 5; x++) assert(pixels[(PW_HUD_PAD + 1) * PW_HUD_MAX_W + PW_HUD_PAD + x] == 0xFFFFC39Fu);
    /* 3's top row (0x1F) in the fifth cell is white */
    assert(pixels[(PW_HUD_PAD + 1) * PW_HUD_MAX_W + PW_HUD_PAD + 4 * PW_HUD_CELL_W] == 0xFFFFFFFFu);
    /* nothing past the panel is touched */
    assert(pixels[w] == 0x55555555u && pixels[h * PW_HUD_MAX_W] == 0x55555555u);

    /* With frame times, a graph under the text: steady frames green, a long one red. */
    t = run(&stats, t, 60, 1, 1);
    t += 100 * MS;
    (void)pw_hud_frame(&stats, t, 1);
    pw_hud_draw(&stats, PW_HUD_FRAMETIMES, 0, pixels, PW_HUD_MAX_W, &w, &h);
    uint32_t base = PW_HUD_PAD + 2 * PW_HUD_CELL_H + PW_HUD_GRAPH_H;   /* the graph's bottom row */
    assert(h == 2 * PW_HUD_CELL_H + 2 * PW_HUD_PAD + PW_HUD_GRAPH_H + 1);
    assert(pixels[base * PW_HUD_MAX_W + PW_HUD_PAD + PW_HUD_GRAPH - 2] == 0xFF8EF0A0u);
    assert(pixels[base * PW_HUD_MAX_W + PW_HUD_PAD + PW_HUD_GRAPH - 1] == 0xFF7A7AFFu);
    /* the long frame (past the graph's top) fills it; a 16.7 ms one is short */
    assert(pixels[(base - PW_HUD_GRAPH_H + 1) * PW_HUD_MAX_W + PW_HUD_PAD + PW_HUD_GRAPH - 1] == 0xFF7A7AFFu);
    assert(pixels[(base - PW_HUD_GRAPH_H + 1) * PW_HUD_MAX_W + PW_HUD_PAD + PW_HUD_GRAPH - 2] == 0xFF000000u);

    /* At full the graph stays under the two frame-rate lines, and the rest follows it. */
    pw_hud_draw(&stats, PW_HUD_FULL, 0, pixels, PW_HUD_MAX_W, &w, &h);
    assert(h == 5 * PW_HUD_CELL_H + 2 * PW_HUD_PAD + PW_HUD_GRAPH_H + 1);
    assert(pixels[base * PW_HUD_MAX_W + PW_HUD_PAD + PW_HUD_GRAPH - 1] == 0xFF7A7AFFu);
    /* R of RES (light blue) one row into the third line, below the graph */
    assert(pixels[(PW_HUD_PAD + 2 * PW_HUD_CELL_H + PW_HUD_GRAPH_H + 1 + 1) * PW_HUD_MAX_W + PW_HUD_PAD] == 0xFFFFC39Fu);

    /* The widest picture still fits. */
    stats.render_w = stats.output_w = 3840; stats.render_h = 2160; stats.output_h = 2160;
    stats.render_w = 1920; stats.render_h = 1080;
    stats.cpu10 = 999999; stats.cpus = 16;
    stats.memory_bytes = stats.memory_peak_bytes = 1ull << 40;
    pw_hud_draw(&stats, PW_HUD_FULL, 1, pixels, PW_HUD_MAX_W, &w, &h);
    assert(w > 0 && w <= PW_HUD_MAX_W && h > 0 && h <= PW_HUD_MAX_H);

    /* A pause longer than the window: the seconds before it fall out of it. */
    memset(&stats, 0, sizeof(stats));
    t = 100 * S;
    (void)pw_hud_frame(&stats, t, 1);
    t = run(&stats, t, 60, 10, 1);
    assert(pw_hud_window(&stats, &lo, &avg, &hi) && lo == 600 && hi == 600 && stats.window_count == 10);
    t += 30 * S;
    assert(pw_hud_frame(&stats, t, 1));
    assert(stats.window_count == 1 && pw_hud_window(&stats, &lo, &avg, &hi) && hi == 0 && avg == 0);
    /* A 3 s pause keeps the 7 seconds before it that still fall in the window. */
    t = run(&stats, t, 60, 10, 1);
    t += 3 * S;
    assert(pw_hud_frame(&stats, t, 1));
    assert(stats.window_count == 8 && pw_hud_window(&stats, &lo, &avg, &hi) && hi == 600 && lo == 3);
    /* Two hours without a frame: the frame time saturates instead of wrapping. */
    t += 7200 * S;
    assert(pw_hud_frame(&stats, t, 1) && stats.frame_us == UINT32_MAX);
    pw_hud_draw(&stats, PW_HUD_FRAMETIMES, 0, pixels, PW_HUD_MAX_W, &w, &h);
    assert(w > 0 && h > 0);

    assert(pw_hud_scale(720) == 1 && pw_hud_scale(1080) == 2 && pw_hud_scale(1440) == 3 && pw_hud_scale(2160) == 4 &&
           pw_hud_scale(240) == 1 && pw_hud_scale(0) == 1);
    puts("test_pw_hud: ok");
    return 0;
}
