// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted for plain QMK/Vial (no Halcyon, no QP surface API)

#include "quantum.h"
#include "qp.h"
#include "hlc_tft_display.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
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
#define STATUS_TEXT_X          12
#define STATUS_RESERVED_WIDTH  140
#define STATUS_RESERVED_TOP    (LCD_HEIGHT - 95)
#define STATUS_TEXT_SHADOW_OFFSET 1
#define STATUS_TEXT_BG_PADDING   2
#define GRID_ORIGIN_X          HUD_RESERVED_WIDTH
#define GRID_ORIGIN_Y          0

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

static void draw_status_text(const char *text, painter_font_handle_t font, uint16_t y, uint8_t h, uint8_t s, uint8_t v) {
    if (!lcd || !font) {
        return;
    }

    uint16_t bg_left   = (STATUS_TEXT_X > STATUS_TEXT_BG_PADDING) ? (STATUS_TEXT_X - STATUS_TEXT_BG_PADDING) : 0;
    uint16_t bg_top    = (y > STATUS_TEXT_BG_PADDING) ? (y - STATUS_TEXT_BG_PADDING) : 0;
    size_t   len       = text ? strlen(text) : 0;
    uint16_t text_px   = len ? (uint16_t)(len * (font->line_height - 6)) : font->line_height;
    uint16_t bg_right  = STATUS_TEXT_X + text_px + STATUS_TEXT_BG_PADDING + STATUS_TEXT_SHADOW_OFFSET;
    uint16_t bg_bottom = y + font->line_height + STATUS_TEXT_BG_PADDING;

    if (bg_right >= LCD_WIDTH) {
        bg_right = LCD_WIDTH - 1;
    }
    if (bg_bottom >= LCD_HEIGHT) {
        bg_bottom = LCD_HEIGHT - 1;
    }

    qp_rect(lcd, bg_left, bg_top, bg_right, bg_bottom, 0, 0, 0, true);

    qp_drawtext_recolor(lcd,
                        STATUS_TEXT_X + STATUS_TEXT_SHADOW_OFFSET,
                        y + STATUS_TEXT_SHADOW_OFFSET,
                        font,
                        text,
                        0, 0, 0,
                        0, 0, 0);

    qp_drawtext_recolor(lcd,
                        STATUS_TEXT_X,
                        y,
                        font,
                        text,
                        h, s, v,
                        0, 0, 0);
}

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

static uint32_t last_input_time = 0;
static bool     display_awake   = true;

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

            // Skip drawing over the HUD area (layer glyph + status text)
            if ((left < HUD_RESERVED_WIDTH && top < HUD_RESERVED_HEIGHT) ||
                (left < STATUS_RESERVED_WIDTH && bottom > STATUS_RESERVED_TOP)) {
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

    if (force_full_redraw || now.raw != last_led_usb_state.raw || layer_changed) {
        uint16_t y;

        if (force_full_redraw || layer_changed) {
            clear_hud_background();
        }

        const char *caps_text = now.caps_lock ? "CAPS" : "caps";

        // Caps indicator
        y = LCD_HEIGHT - Retron27->line_height * 3 - 15;
        if (now.caps_lock) {
            draw_status_text(caps_text, Retron27_underline, y, 17, 191, 245);
        } else {
            draw_status_text(caps_text, Retron27, y, 17, 104, 77);
        }

        // Active layer label
        uint8_t active_layer = get_highest_layer(layer_state | default_layer_state);
        const uint8_t *active_color =
            (active_layer < (sizeof(layer_color_map) / sizeof(layer_color_map[0])))
                ? layer_color_map[active_layer]
                : layer_color_fallback;

        char active_buffer[24];
        snprintf(active_buffer, sizeof(active_buffer), "L:%s", hlc_tft_layer_short(active_layer));

        y = LCD_HEIGHT - Retron27->line_height * 2 - 10;
        draw_status_text(active_buffer, Retron27_underline, y, active_color[0], active_color[1], active_color[2]);

        // Default layer label
        uint8_t base_layer = get_highest_layer(default_layer_state ? default_layer_state : (1UL << 0));
        char base_buffer[24];
        snprintf(base_buffer, sizeof(base_buffer), "B:%s", hlc_tft_layer_short(base_layer));

        y = LCD_HEIGHT - Retron27->line_height - 5;
        draw_status_text(base_buffer, Retron27, y, 202, 104, 77);
    }

    // Layer number (recolored image). Load/close image handles here.
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

    // If display is asleep, wake it
    if (!display_awake) {
        display_do_wake();
    }
}
