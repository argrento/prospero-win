/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_HUD_H
#define PW_HUD_H
/* The performance overlay libvulkan.prx draws in the top left of a game's
 * pictures (pw_framegen.cpp), without Vulkan: what it counts and the picture
 * of it. Levels, from [display] hud through PW_HUD:
 *
 *   fps         FPS 59.9                  (with frame generation: 29.9>59.8)
 *   frametimes  + the frame time, the lowest, average and highest frame rate
 *               of the last 10 seconds, and a graph of the last frames' times
 *   full        + the size the game draws at (and the output's, when it
 *               differs), the game's CPU time and its direct memory
 *
 * A stat nobody has given (the output size, memory) leaves its line out. The
 * picture is RGBA8, opaque: white text, uppercase letters light blue, on
 * black. */
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum PwHudLevel {
    PW_HUD_OFF = 0,
    PW_HUD_FPS = 1,
    PW_HUD_FRAMETIMES = 2,
    PW_HUD_FULL = 3,
} PwHudLevel;

enum {
    PW_HUD_WINDOW_S = 10,       /* the lowest, average and highest frame rate: seconds */
    PW_HUD_GRAPH = 120,         /* frame times in the graph */
    PW_HUD_LINES = 8,
    PW_HUD_LINE_CHARS = 40,
    PW_HUD_CELL_W = 6,          /* a 5x7 glyph and its spacing */
    PW_HUD_CELL_H = 9,
    PW_HUD_PAD = 2,
    PW_HUD_GRAPH_H = 18,
    PW_HUD_MAX_W = 256,         /* the picture never grows past this */
    PW_HUD_MAX_H = 128,
};

typedef struct PwHudStats {
    /* the second being counted */
    uint64_t second_start_ns;
    uint32_t second_frames, second_shown;
    /* the last whole second, in tenths of a frame a second */
    uint32_t fps10, shown10;
    uint32_t frame_us;          /* its average frame time */
    int measured;               /* a whole second has passed */
    /* the last PW_HUD_WINDOW_S whole seconds, a ring */
    uint32_t window_frames[PW_HUD_WINDOW_S];
    uint64_t window_ns[PW_HUD_WINDOW_S];
    uint32_t window_count, window_next;
    /* the last frames' times, a ring */
    uint64_t last_frame_ns;
    uint32_t graph_us[PW_HUD_GRAPH];
    uint32_t graph_count, graph_next;
    /* full: set by whoever knows them; 0 unknown */
    uint32_t render_w, render_h, output_w, output_h;
    uint32_t cpus;
    uint32_t cpu10;             /* CPU time per wall time, in tenths of a percent of one core */
    uint64_t cpu_us, cpu_wall_ns;
    int cpu_measured;
    uint64_t memory_bytes, memory_peak_bytes;
    int memory_known;
} PwHudStats;

/* "fps", "frametimes", "full": the level; anything else (NULL, "off", "1") off. */
PwHudLevel pw_hud_level_parse(const char *value);

/* One frame the game presented at now_ns, and the pictures it showed (1, or
 * more with frame generation). 1 when it completes a second. */
int pw_hud_frame(PwHudStats *stats, uint64_t now_ns, uint32_t shown);

/* The process's CPU time (user and system, microseconds) at now_ns; the
 * second call on gives the share since the one before. */
void pw_hud_cpu(PwHudStats *stats, uint64_t cpu_us, uint64_t now_ns);

/* The lowest, average and highest frame rate of the window, in tenths; 0
 * when no second is in it yet. */
int pw_hud_window(const PwHudStats *stats, uint32_t *min10, uint32_t *avg10, uint32_t *max10);

/* The overlay's lines for level (framegen: the generator shows pictures
 * between the frames). Returns how many; nothing before the first second. */
size_t pw_hud_text(const PwHudStats *stats, PwHudLevel level, int framegen,
                   char lines[PW_HUD_LINES][PW_HUD_LINE_CHARS + 1]);

/* The overlay's picture into pixels (RGBA8 words, stride_px words a row, at
 * least PW_HUD_MAX_W x PW_HUD_MAX_H): its size in *width and *height, 0x0
 * when there is nothing to show. */
void pw_hud_draw(const PwHudStats *stats, PwHudLevel level, int framegen, uint32_t *pixels, uint32_t stride_px,
                 uint32_t *width, uint32_t *height);

/* How many times the picture is scaled onto an image height pixels high:
 * 2 at 1080p, 4 at 2160p, never under 1. */
uint32_t pw_hud_scale(uint32_t height);

#ifdef __cplusplus
}
#endif
#endif
