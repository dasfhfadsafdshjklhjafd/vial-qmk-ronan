// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted for plain QMK/Vial (no Halcyon, no QP surface API)

#include "quantum.h"
#include "qp.h"
#include "hlc_tft_display.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "wpm.h"
#include "wait.h"     // for wait_ms()

#if defined(RP2040) || defined(ARDUINO_ARCH_RP2040) || defined(MCU_RP2040)
#    include "hardware/structs/rosc.h"
#    define HAVE_ROSC 1
#else
#    define HAVE_ROSC 0
#endif

// Fonts / images (ensure your rules.mk adds -Ikeyboards/astronaut/tft_display)
#include "graphics/fonts/Retron2000-27.qff.h"
#include "graphics/fonts/Retron2000-underline-27.qff.h"

#include "graphics/numbers/0.qgf.h"
#include "graphics/numbers/1.qgf.h"
#include "graphics/numbers/2.qgf.h"
#include "graphics/numbers/3.qgf.h"
#include "graphics/numbers/4.qgf.h"
#include "graphics/numbers/5.qgf.h"
#include "graphics/numbers/6.qgf.h"
#include "graphics/numbers/7.qgf.h"
#include "graphics/numbers/8.qgf.h"
#include "graphics/numbers/9.qgf.h"
#include "graphics/numbers/undef.qgf.h"

// Reserve space in the top-left corner for the HUD (layer glyph + status text)
#define HUD_RESERVED_WIDTH    100
#define HUD_RESERVED_HEIGHT   120
#define STATUS_RESERVED_WIDTH  140
#define STATUS_RESERVED_TOP    (LCD_HEIGHT - 95)
#define STATUS_TEXT_SHADOW_OFFSET 1
#define STATUS_TEXT_BG_PADDING   2
#define GRID_ORIGIN_X          HUD_RESERVED_WIDTH
#define GRID_ORIGIN_Y          0

#define LAYER_LABEL_X          12
#define LAYER_LABEL_Y          (HUD_RESERVED_HEIGHT - 18) // fallback; we compute runtime Y from font
#define CAPS_LABEL_X           12

#define WAVE_SAMPLE_COUNT      32
#define WAVE_SAMPLE_WIDTH       2
#define WAVE_WIDTH             (WAVE_SAMPLE_COUNT * WAVE_SAMPLE_WIDTH)
#define WAVE_HEIGHT            22
#define WAVE_LEFT              10
#define WAVE_TOP               (LCD_HEIGHT - WAVE_HEIGHT - 14)
#define WAVE_BASELINE_Y        (WAVE_TOP + WAVE_HEIGHT - 3)

#define WPM_RESERVED_LEFT      (LCD_WIDTH - 72)
#define WPM_RESERVED_TOP       (LCD_HEIGHT - 46)

// Fixed panel used to clear the entire WPM region on redraw (prevents stale digits)
#define WPM_PANEL_LEFT   (WPM_RESERVED_LEFT - STATUS_TEXT_BG_PADDING)
#define WPM_PANEL_TOP    (WPM_RESERVED_TOP  - STATUS_TEXT_BG_PADDING)
#define WPM_PANEL_RIGHT  (LCD_WIDTH - 1)
#define WPM_PANEL_BOTTOM (LCD_HEIGHT - 1)

static const uint8_t layer_color_map[][3] = {
    {HSV_LAYER_0},
    {HSV_LAYER_1},
    {HSV_LAYER_2},
    {HSV_LAYER_3},
    {HSV_LAYER_4},
    {HSV_LAYER_5},
    {HSV_LAYER_6},
    {HSV_LAYER_7},
};

static painter_font_handle_t Retron27;
static painter_font_handle_t Retron27_underline;

static int color_value = 0;

painter_device_t lcd = NULL;

static led_t          last_led_usb_state       = (led_t){0};
static layer_state_t  last_layer_state         = 0;
static layer_state_t  last_default_layer_state = 0;
static const uint8_t  layer_color_fallback[3]  = {HSV_LAYER_UNDEF};

static bool force_full_redraw = false;

#ifndef TFT_INACTIVITY_TIMEOUT_MS
#    define TFT_INACTIVITY_TIMEOUT_MS (10UL * 60UL * 1000UL)  // 10 minutes default
#endif

// Set to 1 to show the colored number glyph; 0 to disable it entirely
#ifndef SHOW_LAYER_GLYPH
#    define SHOW_LAYER_GLYPH 1
#endif

// Set to 1 to show the "L:..." text label; 0 to disable it entirely
#ifndef SHOW_LAYER_LABEL
#    define SHOW_LAYER_LABEL 0
#endif

static uint32_t last_input_time = 0;
static bool     display_awake   = true;
static uint8_t  wave_phase                 = 0;
static uint32_t wave_last_tick            = 0;
static uint8_t  last_wave_color_slot      = 255;
static bool     wave_initialized          = false;
static uint8_t  wave_samples[WAVE_SAMPLE_COUNT] = {0};

// WPM smoothing and draw cache
static uint16_t last_drawn_wpm              = 0xFFFF;
static uint16_t wpm_ema                     = 0;     // EMA state (alpha ≈ 0.25)

static const uint8_t wave_pattern[] = {0, 2, 6, 12, 18, 12, 6, 2, 0, 1, 4, 9, 14, 18, 14, 9, 4, 1, 0, 0};
#define WAVE_PATTERN_LENGTH (sizeof(wave_pattern) / sizeof(wave_pattern[0]))

__attribute__((weak)) const char *hlc_tft_layer_label(uint8_t layer) {
    switch (layer) {
        case 0: return "Layer 0";
        case 1: return "Layer 1";
        case 2: return "Layer 2";
        case 3: return "Layer 3";
        case 4: return "Layer 4";
        case 5: return "Layer 5";
        case 6: return "Layer 6";
        case 7: return "Layer 7";
        case 8: return "Layer 8";
        case 9: return "Layer 9";
        default: return "Layer ?";
    }
}

static inline void clear_hud_background(void) {
    if (lcd) {
        qp_rect(lcd, 0, 0, HUD_RESERVED_WIDTH - 1, HUD_RESERVED_HEIGHT - 1, 0, 0, 0, true);
    }
}

static void draw_status_text(uint16_t x, uint16_t y, const char *text, painter_font_handle_t font, uint8_t h, uint8_t s, uint8_t v) {
    if (!lcd || !font) {
        return;
    }

    uint16_t bg_left   = (x > STATUS_TEXT_BG_PADDING) ? (x - STATUS_TEXT_BG_PADDING) : 0;
    uint16_t bg_top    = (y > STATUS_TEXT_BG_PADDING) ? (y - STATUS_TEXT_BG_PADDING) : 0;
    size_t   len       = text ? strlen(text) : 0;
    uint16_t text_px   = len ? (uint16_t)(len * (font->line_height - 6)) : font->line_height;
    uint16_t bg_right  = x + text_px + STATUS_TEXT_BG_PADDING + STATUS_TEXT_SHADOW_OFFSET;
    uint16_t bg_bottom = y + font->line_height + STATUS_TEXT_BG_PADDING;

    if (bg_right >= LCD_WIDTH) {
        bg_right = LCD_WIDTH - 1;
    }
    if (bg_bottom >= LCD_HEIGHT) {
        bg_bottom = LCD_HEIGHT - 1;
    }

    qp_rect(lcd, bg_left, bg_top, bg_right, bg_bottom, 0, 0, 0, true);

    qp_drawtext_recolor(lcd,
                        x + STATUS_TEXT_SHADOW_OFFSET,
                        y + STATUS_TEXT_SHADOW_OFFSET,
                        font,
                        text,
                        0, 0, 0,
                        0, 0, 0);

    qp_drawtext_recolor(lcd,
                        x,
                        y,
                        font,
                        text,
                        h, s, v,
                        0, 0, 0);
}

static void render_waveform(uint8_t layer, bool force) {
    if (!lcd) {
        return;
    }

    /*
     * More "alien" pattern (asymmetric peaks + softer return)
     * You can tune these numbers to taste.
     */
    static const uint8_t alien_pattern[] = {0, 2, 4, 7, 11, 16, 18, 6, 3, 1, 0, 0};
    const size_t alien_pattern_len = sizeof(alien_pattern) / sizeof(alien_pattern[0]);

    if (!wave_initialized) {
        for (uint8_t i = 0; i < WAVE_SAMPLE_COUNT; ++i) {
            uint8_t base = alien_pattern[i % alien_pattern_len];
            wave_samples[i] = (uint8_t)((base * (WAVE_HEIGHT - 4)) / 18);
        }
        wave_phase = 0;
        wave_last_tick = timer_read32();
        wave_initialized = true;
        last_wave_color_slot = 255;
        force = true;
    }

    uint32_t now = timer_read32();

    /* Use QMK WPM as pacing; fallback to get_current_wpm(). */
    uint16_t avg_wpm = get_current_wpm();
    if (avg_wpm > 255) avg_wpm = 255; // defensive clamp

    /* Map typing speed to an interval — faster typing => faster motion */
    uint16_t interval_ms;
    if (avg_wpm > 100) {
        interval_ms = 200;
    } else if (avg_wpm > 60) {
        interval_ms = 300;
    } else if (avg_wpm > 20) {
        interval_ms = 420;
    } else {
        interval_ms = 680;
    }

    bool changed = force;

    if (wave_last_tick == 0) {
        wave_last_tick = now;
    }

    if (timer_elapsed32(wave_last_tick) >= interval_ms) {
        /* shift samples left */
        memmove(&wave_samples[0], &wave_samples[1], WAVE_SAMPLE_COUNT - 1);

        /* pattern + phase */
        uint8_t pattern_value = alien_pattern[wave_phase % alien_pattern_len];
        wave_phase = (wave_phase + 1) % alien_pattern_len;

        uint8_t amplitude = (uint8_t)((pattern_value * (WAVE_HEIGHT - 4)) / 18);

        /* occasional big spike */
        if ((rand() & 31) == 0) {
            amplitude = (uint8_t)(WAVE_HEIGHT - 4);
        }

        /* random "notch" to make it jagged */
        if ((rand() & 15) == 0) {
            amplitude = (uint8_t)(amplitude / 2);
        }

        /* small random wobble */
        if ((rand() & 7) == 0) {
            int16_t tweak = amplitude + (int16_t)((rand() % 7) - 3);
            if (tweak < 0) tweak = 0;
            if (tweak > WAVE_HEIGHT - 4) tweak = WAVE_HEIGHT - 4;
            amplitude = (uint8_t)tweak;
        }

        wave_samples[WAVE_SAMPLE_COUNT - 1] = amplitude;
        wave_last_tick = now;
        changed = true;
    }

    if (layer != last_wave_color_slot) {
        changed = true;
    }

    if (!changed) return;

    /* color for this layer */
    size_t layer_count = sizeof(layer_color_map) / sizeof(layer_color_map[0]);
    const uint8_t *color = (layer < layer_count) ? layer_color_map[layer] : layer_color_fallback;

    /* clear waveform area */
    qp_rect(lcd,
            WAVE_LEFT,
            WAVE_TOP,
            WAVE_LEFT + WAVE_WIDTH - 1,
            WAVE_TOP + WAVE_HEIGHT - 1,
            0, 0, 0, true);

    /* baseline line (faint) */
    qp_rect(lcd,
            WAVE_LEFT,
            WAVE_BASELINE_Y,
            WAVE_LEFT + WAVE_WIDTH - 1,
            WAVE_BASELINE_Y,
            color[0],
            (uint8_t)(color[1] / 4),
            48,
            true);

    /*
     * Draw columns with a small trailing ghost for the previous two samples.
     * We draw from left to right so later ghost draws can be visually behind.
     */
    for (uint8_t i = 0; i < WAVE_SAMPLE_COUNT; ++i) {
        /* For each column, draw up to 3 layers: main (offset 0) and two ghosts (offset 1..2) */
        for (uint8_t ghost = 0; ghost < 3; ++ghost) {
            /* source index for this ghost layer (ghost=0 is current sample) */
            int src = (int)i - (int)ghost;
            if (src < 0) continue;

            uint8_t amplitude = wave_samples[src];
            if (amplitude == 0) continue;

            /* reduce amplitude for ghosts */
            if (ghost > 0) {
                if (amplitude > ghost * 2) amplitude -= ghost * 2;
                else amplitude = 0;
                if (amplitude == 0) continue;
            }

            if (amplitude > WAVE_HEIGHT - 3) amplitude = WAVE_HEIGHT - 3;
            int16_t top_y = WAVE_BASELINE_Y - amplitude;

            int16_t left  = WAVE_LEFT + src * WAVE_SAMPLE_WIDTH;
            int16_t right = left + WAVE_SAMPLE_WIDTH - 1;

            /* brightness/value based on amplitude and ghost level (stronger for main, dimmer for ghosts) */
            uint16_t base_v = 120 + amplitude * 5;        // base brightness for main (roughly 120..(120+5*max))
            if (base_v > 255) base_v = 255;
            uint8_t v_col = (uint8_t)((ghost == 0) ? base_v : (base_v / (1 + ghost * 2)));

            /* Saturation slightly reduced for ghosts to keep them subtle */
            uint8_t s_col = (uint8_t)((ghost == 0) ? color[1] : (color[1] / 2));

            /* Draw the column (filled) */
            qp_rect(lcd,
                    left,
                    top_y,
                    right,
                    WAVE_BASELINE_Y - 1,
                    color[0],
                    s_col,
                    v_col,
                    true);

            /* If this is the main layer and amplitude is tall, draw a 1-px "gap" at the peak to get a teeth effect */
            if (ghost == 0 && amplitude > 3) {
                int16_t peak_y = top_y;
                if (peak_y >= WAVE_TOP && peak_y <= WAVE_TOP + WAVE_HEIGHT) {
                    /* draw a 1-px horizontal line that's background-colored to produce a gap */
                    qp_rect(lcd,
                            left,
                            peak_y,
                            right,
                            peak_y,
                            0, 0, 0,
                            true);
                }
            }
        }
    }

    last_wave_color_slot = layer;
}


static void draw_wpm_display(bool force) {
    if (!lcd || !Retron27) {
        return;
    }

    /* Use get_current_wpm() which is provided by QMK's WPM support */
    uint16_t actual = get_current_wpm();
    if (actual > 200) {
        actual = 200;
    }

    // initialize EMA properly on first run
    if (wpm_ema == 0) {
        wpm_ema = actual;
    }

    // EMA alpha ≈ 0.25: new = old + (actual - old) / 4
    int16_t diff = (int16_t)actual - (int16_t)wpm_ema;
    wpm_ema += diff / 4;

    uint16_t wpm = wpm_ema;

    if (!force && wpm == last_drawn_wpm) {
        return;
    }

    // FIX: clear a fixed panel so shrinking digits don't leave artifacts
    qp_rect(lcd, WPM_PANEL_LEFT, WPM_PANEL_TOP, WPM_PANEL_RIGHT, WPM_PANEL_BOTTOM, 0, 0, 0, true);

    char buffer[6];
    snprintf(buffer, sizeof(buffer), "%u", wpm);

    // Right-align inside fixed panel
    uint16_t char_w   = (Retron27->line_height > 6) ? (Retron27->line_height - 6) : Retron27->line_height;
    uint16_t text_w   = (uint16_t)(strlen(buffer) * char_w);
    uint16_t x        = WPM_PANEL_RIGHT - text_w - 4;  // 4px margin
    uint16_t y        = LCD_HEIGHT - Retron27->line_height - 5;

    qp_drawtext_recolor(lcd, x + STATUS_TEXT_SHADOW_OFFSET, y + STATUS_TEXT_SHADOW_OFFSET,
                        Retron27, buffer, 0, 0, 0, 0, 0, 0);
    qp_drawtext_recolor(lcd, x, y, Retron27, buffer, 142, 191, 245, 0, 0, 0);

    last_drawn_wpm = wpm;
}

__attribute__((weak)) const char *hlc_tft_layer_short(uint8_t layer) {
    switch (layer) {
        case 0: return "G";   // Graphite
        case 1: return "Br";  // Bracket+Num
        case 2: return "N";   // Nav
        case 3: return "M";   // Mouse
        case 4: return "4";
        case 5: return "T";   // To-Left
        case 6: return "Gm";  // Game
        case 7: return "P";   // GMAP
        case 8: return "L";   // Num Left
        case 9: return "9";
        default: return "?";
    }
}

/* --- Sleep / Wake helpers --- */
void display_do_sleep(void) {
    if (!display_awake) return;
    if (lcd) qp_power(lcd, false);   // controller off (backlight still on if BLK=3V3)
    display_awake = false;
}

void display_do_wake(void) {
    if (display_awake) return;
    if (lcd) qp_power(lcd, true);
    // force a full redraw (clear then mark)
    if (lcd) {
        qp_rect(lcd, 0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, 0, 0, 0, true);
    }
    force_full_redraw = true;
    update_display();
    if (lcd) qp_flush(lcd);
    display_awake = true;
}

/* --- helpers --- */

// return true if two rects intersect
static inline bool rects_intersect(uint16_t l1, uint16_t t1, uint16_t r1, uint16_t b1,
                                   uint16_t l2, uint16_t t2, uint16_t r2, uint16_t b2) {
    return !(r1 < l2 || l1 > r2 || b1 < t2 || t1 > b2);
}

/* --- Game of Life (optional animation); static to avoid external linkage --- */
#define GRID_WIDTH 27
#define GRID_HEIGHT 48
#define CELL_SIZE 4
#define OUTLINE_SIZE 1
#define INITIAL_ALIVE_PROBABILITY 0.20f

static bool grid[GRID_HEIGHT][GRID_WIDTH];
static bool new_grid[GRID_HEIGHT][GRID_WIDTH];
static bool changed_grid[GRID_HEIGHT][GRID_WIDTH];

static uint32_t get_random_32bit(void) {
#if HAVE_ROSC
    uint32_t random_value = 0;
    for (int i = 0; i < 32; i++) {
        wait_ms(1);
        random_value = (random_value << 1) | (rosc_hw->randombit & 1);
    }
    return random_value;
#else
    uint32_t t = timer_read32();
    return (t << 16) ^ (uint32_t)rand();
#endif
}

static void init_grid(void) {
    for (int y = 0; y < GRID_HEIGHT; y++) {
        for (int x = 0; x < GRID_WIDTH; x++) {
            grid[y][x] = (rand() < (int)(INITIAL_ALIVE_PROBABILITY * RAND_MAX));
            changed_grid[y][x] = true;
        }
    }
}

static inline void draw_cell_rect(uint16_t left, uint16_t top, uint16_t right, uint16_t bottom, uint8_t h, uint8_t s, uint8_t v) {
    qp_rect(lcd, left, top, right, bottom, 0, 0, 0, true); // outline/background
    qp_rect(lcd, left + OUTLINE_SIZE, top + OUTLINE_SIZE, right - OUTLINE_SIZE, bottom - OUTLINE_SIZE, h, s, v, true);
}

static void draw_grid(void) {
    for (int y = 0; y < GRID_HEIGHT; y++) {
        for (int x = 0; x < GRID_WIDTH; x++) {
            if (!changed_grid[y][x]) continue;
            uint16_t left   = GRID_ORIGIN_X + x * (CELL_SIZE + OUTLINE_SIZE);
            uint16_t top    = GRID_ORIGIN_Y + y * (CELL_SIZE + OUTLINE_SIZE);
            uint16_t right  = left + CELL_SIZE + OUTLINE_SIZE;
            uint16_t bottom = top  + CELL_SIZE + OUTLINE_SIZE;

            // Skip drawing over HUD, status, WPM panel, or waveform
            if (rects_intersect(left, top, right, bottom, 0, 0, HUD_RESERVED_WIDTH - 1, HUD_RESERVED_HEIGHT - 1) ||
                rects_intersect(left, top, right, bottom, 0, STATUS_RESERVED_TOP, STATUS_RESERVED_WIDTH - 1, LCD_HEIGHT - 1) ||
                rects_intersect(left, top, right, bottom, WPM_PANEL_LEFT, WPM_PANEL_TOP, WPM_PANEL_RIGHT, WPM_PANEL_BOTTOM) ||
                rects_intersect(left, top, right, bottom, WAVE_LEFT, WAVE_TOP, WAVE_LEFT + WAVE_WIDTH - 1, WAVE_TOP + WAVE_HEIGHT - 1)) {
                changed_grid[y][x] = false;
                continue;
            }

            if (grid[y][x]) {
                switch (color_value) {
                case 0: draw_cell_rect(left, top, right, bottom, 0,   0, 160); break;
                case 1: draw_cell_rect(left, top, right, bottom, 23, 89, 255); break;
                case 2: draw_cell_rect(left, top, right, bottom, 43, 71, 255); break;
                case 3: draw_cell_rect(left, top, right, bottom, 0,  82, 255); break;
                case 4: draw_cell_rect(left, top, right, bottom, 77, 64, 255); break;
                case 5: draw_cell_rect(left, top, right, bottom, 176,77, 255); break;
                case 6: draw_cell_rect(left, top, right, bottom, 131,99, 255); break;
                case 7: draw_cell_rect(left, top, right, bottom, 154,94, 255); break;
                default: draw_cell_rect(left, top, right, bottom, 0,255,255); break;
                }
            } else {
                qp_rect(lcd, left, top, right, bottom, 0, 0, 0, true);
            }
            changed_grid[y][x] = false;
        }
    }
}

static void update_grid(void) {
    for (int y = 0; y < GRID_HEIGHT; y++) {
        for (int x = 0; x < GRID_WIDTH; x++) {
            int alive_neighbors = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    int ny = y + dy;
                    int nx = x + dx;
                    if (ny >= 0 && ny < GRID_HEIGHT && nx >= 0 && nx < GRID_WIDTH) {
                        alive_neighbors += grid[ny][nx] ? 1 : 0;
                    }
                }
            }
            if (grid[y][x]) new_grid[y][x] = (alive_neighbors == 2 || alive_neighbors == 3);
            else new_grid[y][x] = (alive_neighbors == 3);
            changed_grid[y][x] = (grid[y][x] != new_grid[y][x]);
        }
    }
    for (int y = 0; y < GRID_HEIGHT; y++) {
        for (int x = 0; x < GRID_WIDTH; x++) grid[y][x] = new_grid[y][x];
    }
}

static void add_cell_cluster(void) {
    int cluster_size = 3;
    int x = rand() % (GRID_WIDTH - cluster_size);
    int y = rand() % (GRID_HEIGHT - cluster_size);
    for (int dy = 0; dy < cluster_size; dy++) {
        for (int dx = 0; dx < cluster_size; dx++) {
            bool is_alive = (rand() & 1);
            grid[y + dy][x + dx] = is_alive;
            changed_grid[y + dy][x + dx] = true;
        }
    }
}

/* === Display / status === */

void update_display(void) {
    static bool first_run_fonts = false;

    if (!first_run_fonts) {
        Retron27 = qp_load_font_mem(font_Retron2000_27);
        Retron27_underline = qp_load_font_mem(font_Retron2000_underline_27);
        first_run_fonts = true;
        force_full_redraw = true;
    }

    led_t now = host_keyboard_led_state();
    bool  layer_changed = (last_layer_state != layer_state) || (last_default_layer_state != default_layer_state);
    bool  force_now = force_full_redraw;

    if (force_full_redraw || now.raw != last_led_usb_state.raw || layer_changed) {
        if (force_full_redraw || layer_changed) {
            clear_hud_background();
        }

        // Draw the layer glyph FIRST so the text renders on top of it if needed
        #if SHOW_LAYER_GLYPH
        if (force_full_redraw || layer_changed) {
            painter_image_handle_t img;
            switch (get_highest_layer(layer_state | default_layer_state)) {
            case 0: img = qp_load_image_mem(gfx_0); qp_drawimage_recolor(lcd, 5, 5, img, HSV_LAYER_0, HSV_BLACK); qp_close_image(img); break;
            case 1: img = qp_load_image_mem(gfx_1); qp_drawimage_recolor(lcd, 5, 5, img, HSV_LAYER_1, HSV_BLACK); qp_close_image(img); break;
            case 2: img = qp_load_image_mem(gfx_2); qp_drawimage_recolor(lcd, 5, 5, img, HSV_LAYER_2, HSV_BLACK); qp_close_image(img); break;
            case 3: img = qp_load_image_mem(gfx_3); qp_drawimage_recolor(lcd, 5, 5, img, HSV_LAYER_3, HSV_BLACK); qp_close_image(img); break;
            case 4: img = qp_load_image_mem(gfx_4); qp_drawimage_recolor(lcd, 5, 5, img, HSV_LAYER_4, HSV_BLACK); qp_close_image(img); break;
            case 5: img = qp_load_image_mem(gfx_5); qp_drawimage_recolor(lcd, 5, 5, img, HSV_LAYER_5, HSV_BLACK); qp_close_image(img); break;
            case 6: img = qp_load_image_mem(gfx_6); qp_drawimage_recolor(lcd, 5, 5, img, HSV_LAYER_6, HSV_BLACK); qp_close_image(img); break;
            case 7: img = qp_load_image_mem(gfx_7); qp_drawimage_recolor(lcd, 5, 5, img, HSV_LAYER_7, HSV_BLACK); qp_close_image(img); break;
            default: img = qp_load_image_mem(gfx_undef); qp_drawimage_recolor(lcd, 5, 5, img, HSV_LAYER_UNDEF, HSV_BLACK); qp_close_image(img); break;
            }
            last_layer_state = layer_state;
        }
        #endif

        // Draw CAPS/caps status near the bottom-right of HUD area
        const char *caps_text = now.caps_lock ? "CAPS" : "caps";
        uint16_t caps_y = LCD_HEIGHT - (Retron27->line_height * 3) - 15;
        if (now.caps_lock) {
            draw_status_text(CAPS_LABEL_X, caps_y, caps_text, Retron27_underline, 17, 191, 245);
        } else {
            draw_status_text(CAPS_LABEL_X, caps_y, caps_text, Retron27, 17, 104, 77);
        }

        // Compute label Y to be ABOVE the CAPS area (so it won't overlap the CAPS or the glyph)
        uint16_t layer_label_y = LAYER_LABEL_Y;
        if (Retron27) {
            // place the label just above CAPS text (6px gap)
            if (caps_y > (uint16_t)(Retron27->line_height + 6)) {
                layer_label_y = caps_y - Retron27->line_height - 6;
            } else {
                layer_label_y = LAYER_LABEL_Y; // fallback
            }
        }

        uint8_t active_layer = get_highest_layer(layer_state | default_layer_state);
        char active_buffer[24];
        snprintf(active_buffer, sizeof(active_buffer), "L:%s", hlc_tft_layer_short(active_layer));

        #if SHOW_LAYER_LABEL
        // determine color for text
        const uint8_t *active_color =
            (active_layer < (sizeof(layer_color_map) / sizeof(layer_color_map[0])))
                ? layer_color_map[active_layer]
                : layer_color_fallback;

        // Right-align the label inside the HUD box so it doesn't hit the glyph on the left
        painter_font_handle_t label_font = Retron27_underline ? Retron27_underline : Retron27;
        size_t   len     = strlen(active_buffer);
        uint16_t char_w  = (label_font->line_height > 6) ? (label_font->line_height - 6) : label_font->line_height;
        uint16_t text_px = (uint16_t)(len * char_w);
        uint16_t margin  = 6;
        uint16_t label_x = (text_px + margin < HUD_RESERVED_WIDTH) ? (HUD_RESERVED_WIDTH - text_px - margin) : LAYER_LABEL_X;

        draw_status_text(label_x, layer_label_y, active_buffer, label_font,
                         active_color[0], active_color[1], active_color[2]);
        #else
        // Label disabled: clear the label area to avoid leftovers
        if (force_full_redraw || layer_changed) {
            painter_font_handle_t label_font = Retron27_underline ? Retron27_underline : Retron27;
            size_t   len     = strlen(active_buffer);
            uint16_t char_w  = (label_font->line_height > 6) ? (label_font->line_height - 6) : label_font->line_height;
            uint16_t text_px = (uint16_t)(len * char_w);
            uint16_t bg_left   = (LAYER_LABEL_X > STATUS_TEXT_BG_PADDING) ? (LAYER_LABEL_X - STATUS_TEXT_BG_PADDING) : 0;
            uint16_t bg_top    = (layer_label_y > STATUS_TEXT_BG_PADDING) ? (layer_label_y - STATUS_TEXT_BG_PADDING) : 0;
            uint16_t bg_right  = LAYER_LABEL_X + text_px + STATUS_TEXT_BG_PADDING + STATUS_TEXT_SHADOW_OFFSET;
            uint16_t bg_bottom = layer_label_y + label_font->line_height + STATUS_TEXT_BG_PADDING;
            if (bg_right >= LCD_WIDTH) bg_right = LCD_WIDTH - 1;
            if (bg_bottom >= LCD_HEIGHT) bg_bottom = LCD_HEIGHT - 1;
            qp_rect(lcd, bg_left, bg_top, bg_right, bg_bottom, 0, 0, 0, true);
        }
        #endif
    }

    uint8_t active_layer_for_wave = get_highest_layer(layer_state | default_layer_state);
    render_waveform(active_layer_for_wave, force_now || layer_changed);
    draw_wpm_display(force_now);

    force_full_redraw = false;

    last_led_usb_state       = now;
    last_layer_state         = layer_state;
    last_default_layer_state = default_layer_state;
}

/* === QMK hooks === */

void keyboard_post_init_user(void) {
    // BLK tied to 3V3; do not call backlight_enable() unless you enable QMK backlight and wire BLK appropriately.

    lcd = qp_st7789_make_spi_device(LCD_WIDTH, LCD_HEIGHT, LCD_CS_PIN, LCD_DC_PIN, LCD_RST_PIN, LCD_SPI_DIVISOR, LCD_SPI_MODE);

    if (lcd) {
        qp_init(lcd, LCD_ROTATION);
        qp_set_viewport_offsets(lcd, LCD_OFFSET_X, LCD_OFFSET_Y);
        qp_rect(lcd, 0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, 0, 0, 0, true);
        qp_power(lcd, true);
        qp_flush(lcd);
    }

    // optional GoL init
    srand((int)get_random_32bit());
    init_grid();
    color_value = rand() % 8;

    last_input_time = timer_read32();
    wave_last_tick = last_input_time;
    wave_phase = 0;
    last_wave_color_slot = 255;
    size_t pattern_len = WAVE_PATTERN_LENGTH;
    for (uint8_t i = 0; i < WAVE_SAMPLE_COUNT; ++i) {
        uint8_t base = wave_pattern[i % pattern_len];
        wave_samples[i] = (uint8_t)((base * (WAVE_HEIGHT - 4)) / 18);
    }
    wave_initialized = true;
    last_drawn_wpm = 0xFFFF;
    wpm_ema = 0; // will initialize on first draw
    display_awake = true;
    force_full_redraw = true;

    update_display();
    if (lcd) qp_flush(lcd);
}

void suspend_power_down_user(void) { if (lcd) qp_power(lcd, false); }
void suspend_wakeup_init_user(void)  { if (lcd) qp_power(lcd, true);  }

void housekeeping_task_user(void) {
    static uint32_t last_draw = 0;

    if (timer_elapsed32(last_input_time) >= TFT_INACTIVITY_TIMEOUT_MS) {
        display_do_sleep();
        return;
    }

    update_display();

    if (timer_elapsed32(last_draw) >= 100) {
        draw_grid();
        update_grid();
        if ((rand() & 255) < 8) {
            add_cell_cluster();
            color_value = rand() % 8;
        }
        last_draw = timer_read32();
    }

    if (lcd && display_awake) {
        qp_flush(lcd);
    }
}

// Called by keymap when user activity occurs (key press)
void hlc_tft_on_activity(void) {
    // Update the inactivity timer
    last_input_time = timer_read32();
    wave_last_tick = last_input_time;
    last_wave_color_slot = 255;

    // Force a redraw of WPM and HUD area
    last_drawn_wpm = 0xFFFF;
    force_full_redraw = false; // keep false; update_display will decide based on layer/led changes

    // If display is asleep, wake it
    if (!display_awake) {
        display_do_wake();
    }
}
