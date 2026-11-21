/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The performance overlay's counts, lines and picture (pw_hud.h). */
#include "pw_hud.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* RGBA8 words as they sit in memory on a little-endian CPU: R in the low byte. */
#define HUD_BACKGROUND 0xFF000000u
#define HUD_TEXT 0xFFFFFFFFu
#define HUD_LABEL 0xFFFFC39Fu   /* light blue */
#define HUD_BAR 0xFF8EF0A0u     /* green */
#define HUD_SPIKE 0xFF7A7AFFu   /* red */
#define HUD_GRAPH_TOP_US 50000u /* the graph's full height: 20 frames a second */

/* 5x7 glyphs, one row a byte, the leftmost column in bit 4. */
typedef struct Glyph {
    char c;
    uint8_t rows[7];
} Glyph;

static const Glyph font[] = {
    { '0', { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E } }, { '1', { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { '2', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F } }, { '3', { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E } },
    { '4', { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } }, { '5', { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E } },
    { '6', { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E } }, { '7', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
    { '8', { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E } }, { '9', { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C } },
    { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } }, { 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
    { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } }, { 'D', { 0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C } },
    { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } }, { 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
    { 'G', { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F } }, { 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'I', { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } }, { 'J', { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C } },
    { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } }, { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
    { 'M', { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 } }, { 'N', { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 } },
    { 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } }, { 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
    { 'Q', { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D } }, { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
    { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } }, { 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
    { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } }, { 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 } },
    { 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A } }, { 'X', { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 } },
    { 'Y', { 0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04 } }, { 'Z', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F } },
    { 'x', { 0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11 } }, { '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C } },
    { '%', { 0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03 } }, { '>', { 0x00, 0x04, 0x02, 0x1F, 0x02, 0x04, 0x00 } },
    { '(', { 0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02 } }, { ')', { 0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08 } },
    { '-', { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 } }, { '/', { 0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00 } },
};

static const uint8_t *glyph_rows(char c)
{
    for (size_t i = 0; i < sizeof(font) / sizeof(font[0]); i++)
        if (font[i].c == c) return font[i].rows;
    return NULL;   /* a space, or a character the font lacks: blank */
}

PwHudLevel pw_hud_level_parse(const char *value)
{
    if (!value) return PW_HUD_OFF;
    if (!strcmp(value, "fps")) return PW_HUD_FPS;
    if (!strcmp(value, "frametimes")) return PW_HUD_FRAMETIMES;
    if (!strcmp(value, "full")) return PW_HUD_FULL;
    return PW_HUD_OFF;
}

static uint32_t rate10(uint64_t frames, uint64_t ns)
{
    return ns ? (uint32_t)((frames * 10000000000ull + ns / 2) / ns) : 0;
}

int pw_hud_frame(PwHudStats *stats, uint64_t now_ns, uint32_t shown)
{
    if (!stats->second_start_ns) {
        /* The first frame starts the count: rates are frames over the time after it. */
        stats->second_start_ns = stats->last_frame_ns = now_ns ? now_ns : 1;
        return 0;
    }
    uint64_t gap = now_ns > stats->last_frame_ns ? (now_ns - stats->last_frame_ns) / 1000u : 0;
    stats->graph_us[stats->graph_next] = gap > UINT32_MAX ? UINT32_MAX : (uint32_t)gap;
    stats->graph_next = (stats->graph_next + 1) % PW_HUD_GRAPH;
    if (stats->graph_count < PW_HUD_GRAPH) stats->graph_count++;
    stats->last_frame_ns = now_ns;
    stats->second_frames++;
    stats->second_shown += shown;
    uint64_t elapsed = now_ns > stats->second_start_ns ? now_ns - stats->second_start_ns : 0;
    if (elapsed < 1000000000u) return 0;
    stats->fps10 = rate10(stats->second_frames, elapsed);
    stats->shown10 = rate10(stats->second_shown, elapsed);
    uint64_t frame_us = elapsed / 1000u / stats->second_frames;
    stats->frame_us = frame_us > UINT32_MAX ? UINT32_MAX : (uint32_t)frame_us;
    stats->window_frames[stats->window_next] = stats->second_frames;
    stats->window_ns[stats->window_next] = elapsed;
    stats->window_next = (stats->window_next + 1) % PW_HUD_WINDOW_S;
    if (stats->window_count < PW_HUD_WINDOW_S) stats->window_count++;
    /* A second that took longer (a pause) covers more of the window: older
     * seconds that now fall outside it go. */
    uint64_t covered = elapsed;
    for (uint32_t i = 1; i < stats->window_count; i++) {
        uint32_t at = (stats->window_next + PW_HUD_WINDOW_S - 1 - i) % PW_HUD_WINDOW_S;
        if (covered >= (uint64_t)PW_HUD_WINDOW_S * 1000000000u) {
            stats->window_count = i;
            break;
        }
        covered += stats->window_ns[at];
    }
    stats->measured = 1;
    stats->second_start_ns = now_ns;
    stats->second_frames = stats->second_shown = 0;
    return 1;
}

void pw_hud_cpu(PwHudStats *stats, uint64_t cpu_us, uint64_t now_ns)
{
    if (stats->cpu_wall_ns && now_ns > stats->cpu_wall_ns && cpu_us >= stats->cpu_us) {
        uint64_t wall_us = (now_ns - stats->cpu_wall_ns) / 1000u;
        if (wall_us) {
            stats->cpu10 = (uint32_t)(((cpu_us - stats->cpu_us) * 1000u + wall_us / 2) / wall_us);
            stats->cpu_measured = 1;
        }
    }
    stats->cpu_us = cpu_us;
    stats->cpu_wall_ns = now_ns;
}

int pw_hud_window(const PwHudStats *stats, uint32_t *min10, uint32_t *avg10, uint32_t *max10)
{
    uint64_t frames = 0, ns = 0;
    uint32_t low = UINT32_MAX, high = 0;

    if (!stats->window_count) return 0;
    for (uint32_t i = 0; i < stats->window_count; i++) {
        uint32_t rate = rate10(stats->window_frames[i], stats->window_ns[i]);
        if (rate < low) low = rate;
        if (rate > high) high = rate;
        frames += stats->window_frames[i];
        ns += stats->window_ns[i];
    }
    *min10 = low;
    *max10 = high;
    *avg10 = rate10(frames, ns);
    return 1;
}

/* bytes as "5.1 GB" or "512 MB" */
static void format_bytes(char *out, size_t size, uint64_t bytes)
{
    if (bytes >= (1ull << 30)) {
        uint64_t tenths = (bytes * 10u + (1ull << 29)) >> 30;
        snprintf(out, size, "%llu.%llu GB", (unsigned long long)(tenths / 10u), (unsigned long long)(tenths % 10u));
    } else {
        snprintf(out, size, "%llu MB", (unsigned long long)((bytes + (1u << 19)) >> 20));
    }
}

/* One line, cut at PW_HUD_LINE_CHARS: the overlay never shows more. */
__attribute__((format(printf, 2, 3))) static void line(char *out, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(out, PW_HUD_LINE_CHARS + 1, format, args);
    va_end(args);
}

size_t pw_hud_text(const PwHudStats *stats, PwHudLevel level, int framegen,
                   char lines[PW_HUD_LINES][PW_HUD_LINE_CHARS + 1])
{
    size_t n = 0;
    char rate[48];

    if (level == PW_HUD_OFF || !stats->measured) return 0;
    if (framegen)
        snprintf(rate, sizeof(rate), "%u.%u>%u.%u", stats->fps10 / 10u, stats->fps10 % 10u, stats->shown10 / 10u,
                 stats->shown10 % 10u);
    else
        snprintf(rate, sizeof(rate), "%u.%u", stats->fps10 / 10u, stats->fps10 % 10u);
    if (level == PW_HUD_FPS) {
        line(lines[n++], "FPS %s", rate);
        return n;
    }
    uint32_t ms100 = (stats->frame_us + 5u) / 10u;
    line(lines[n++], "FPS %s  %u.%u MS", rate, ms100 / 100u, ms100 % 100u / 10u);
    uint32_t min10, avg10, max10;
    if (pw_hud_window(stats, &min10, &avg10, &max10))
        line(lines[n++], "MIN %u.%u AVG %u.%u MAX %u.%u", min10 / 10u, min10 % 10u, avg10 / 10u,
                 avg10 % 10u, max10 / 10u, max10 % 10u);
    if (level < PW_HUD_FULL) return n;
    if (stats->render_w && stats->render_h) {
        if (stats->output_w && stats->output_h &&
            (stats->output_w != stats->render_w || stats->output_h != stats->render_h))
            line(lines[n++], "RES %ux%u>%ux%u", stats->render_w, stats->render_h, stats->output_w,
                     stats->output_h);
        else
            line(lines[n++], "RES %ux%u", stats->render_w, stats->render_h);
    }
    if (stats->cpu_measured) {
        uint32_t percent = (stats->cpu10 + 5u) / 10u;
        if (stats->cpus > 1)
            line(lines[n++], "CPU %u%% (%u%% OF %u CORES)", percent,
                     (stats->cpu10 / stats->cpus + 5u) / 10u, stats->cpus);
        else
            line(lines[n++], "CPU %u%%", percent);
    }
    if (stats->memory_known) {
        char used[48], peak[48];
        format_bytes(used, sizeof(used), stats->memory_bytes);
        format_bytes(peak, sizeof(peak), stats->memory_peak_bytes);
        line(lines[n++], "MEM %s  PEAK %s", used, peak);
    }
    return n;
}

static void fill(uint32_t *pixels, uint32_t stride_px, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                 uint32_t colour)
{
    for (uint32_t row = y; row < y + h; row++)
        for (uint32_t col = x; col < x + w; col++) pixels[row * stride_px + col] = colour;
}

void pw_hud_draw(const PwHudStats *stats, PwHudLevel level, int framegen, uint32_t *pixels, uint32_t stride_px,
                 uint32_t *width, uint32_t *height)
{
    char lines[PW_HUD_LINES][PW_HUD_LINE_CHARS + 1];
    size_t count = pw_hud_text(stats, level, framegen, lines);
    uint32_t chars = 0;
    int graph = level >= PW_HUD_FRAMETIMES && stats->graph_count;

    *width = *height = 0;
    if (!count) return;
    for (size_t i = 0; i < count; i++)
        if (strlen(lines[i]) > chars) chars = (uint32_t)strlen(lines[i]);
    uint32_t inner_w = chars * PW_HUD_CELL_W;
    if (graph && inner_w < PW_HUD_GRAPH) inner_w = PW_HUD_GRAPH;
    uint32_t w = inner_w + 2 * PW_HUD_PAD;
    uint32_t h = (uint32_t)count * PW_HUD_CELL_H + 2 * PW_HUD_PAD + (graph ? PW_HUD_GRAPH_H + 1 : 0);
    if (w > PW_HUD_MAX_W) w = PW_HUD_MAX_W;
    if (h > PW_HUD_MAX_H) h = PW_HUD_MAX_H;
    fill(pixels, stride_px, 0, 0, w, h, HUD_BACKGROUND);
    /* The graph goes under the frame-rate lines (pw_hud_text: the rate, then
     * the window's when it has a second), the same at frametimes and full. */
    size_t above = graph ? 1u + (stats->window_count > 0) : count;
    if (above > count) above = count;
    uint32_t graph_h = graph ? PW_HUD_GRAPH_H + 1 : 0;
    for (size_t i = 0; i < count; i++) {
        uint32_t y0 = PW_HUD_PAD + (uint32_t)i * PW_HUD_CELL_H + 1 + (i >= above ? graph_h : 0);
        for (size_t k = 0; lines[i][k]; k++) {
            uint32_t x0 = PW_HUD_PAD + (uint32_t)k * PW_HUD_CELL_W;
            const uint8_t *rows = glyph_rows(lines[i][k]);
            if (!rows || x0 + 5 > w || y0 + 7 > h) continue;
            uint32_t colour = lines[i][k] >= 'A' && lines[i][k] <= 'Z' ? HUD_LABEL : HUD_TEXT;
            for (uint32_t gy = 0; gy < 7; gy++)
                for (uint32_t gx = 0; gx < 5; gx++)
                    if (rows[gy] >> (4 - gx) & 1) pixels[(y0 + gy) * stride_px + x0 + gx] = colour;
        }
    }
    if (graph) {
        /* Oldest on the left, one pixel a frame; a frame half again as long as the
         * last second's average is red. */
        uint32_t base = PW_HUD_PAD + (uint32_t)above * PW_HUD_CELL_H + 1 + PW_HUD_GRAPH_H;
        uint32_t first = (stats->graph_next + PW_HUD_GRAPH - stats->graph_count) % PW_HUD_GRAPH;
        uint64_t spike = (uint64_t)stats->frame_us + stats->frame_us / 2;
        if (base > h) base = h;
        for (uint32_t i = 0; i < stats->graph_count && PW_HUD_PAD + i < w; i++) {
            uint32_t us = stats->graph_us[(first + i) % PW_HUD_GRAPH];
            uint32_t bar = (uint32_t)(((uint64_t)(us < HUD_GRAPH_TOP_US ? us : HUD_GRAPH_TOP_US) * PW_HUD_GRAPH_H +
                                       HUD_GRAPH_TOP_US - 1) / HUD_GRAPH_TOP_US);
            if (bar > base) bar = base;
            fill(pixels, stride_px, PW_HUD_PAD + i, base - bar, 1, bar,
                 stats->measured && us > spike ? HUD_SPIKE : HUD_BAR);
        }
    }
    *width = w;
    *height = h;
}

uint32_t pw_hud_scale(uint32_t height)
{
    uint32_t scale = (height + 270u) / 540u;
    return scale ? scale : 1u;
}
