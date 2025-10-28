// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted for plain QMK/Vial (no Halcyon, no QP surface API)

#include "quantum.h"
#include "qp.h"
#include "hlc_tft_display.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "wpm.h"
#include "wait.h"     // for wait_ms()

#if defined(RP2040) || defined(ARDUINO_ARCH_RP2040) || defined(MCU_RP2040)
#    include "hardware/structs/rosc.h"
#    define HAVE_ROSC 1
#else
#    define HAVE_ROSC 0
#endif

#ifndef ARRAY_SIZE
#    define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

__attribute__((weak)) bool process_record_user(uint16_t keycode, keyrecord_t *record);

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

// Raise the scope a bit more off the bottom; tweak this number to taste
#define WAVE_BOTTOM_GAP        28
#define WAVE_TOP               (LCD_HEIGHT - WAVE_HEIGHT - WAVE_BOTTOM_GAP)

// Use the midline (oscilloscope-style) instead of bottom baseline
#define WAVE_BASELINE_Y        (WAVE_TOP + (WAVE_HEIGHT / 2))

#define WPM_RESERVED_LEFT      (LCD_WIDTH - 72)
#define WPM_RESERVED_TOP       (LCD_HEIGHT - 46)

// Fixed panel used to clear the entire WPM region on redraw (prevents stale digits)
#define WPM_PANEL_LEFT   (WPM_RESERVED_LEFT - STATUS_TEXT_BG_PADDING)
#define WPM_PANEL_TOP    (WPM_RESERVED_TOP  - STATUS_TEXT_BG_PADDING)
#define WPM_PANEL_RIGHT  (LCD_WIDTH - 1)
#define WPM_PANEL_BOTTOM (LCD_HEIGHT - 1)

#if SHOW_LAYER_LABEL
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

static const uint8_t  layer_color_fallback[3]  = {HSV_LAYER_UNDEF};
#endif



static painter_font_handle_t Retron27;
static painter_font_handle_t Retron27_underline;

static int color_value = 0;

painter_device_t lcd = NULL;

static led_t          last_led_usb_state       = (led_t){0};
static layer_state_t  last_layer_state         = 0;
static layer_state_t  last_default_layer_state = 0;

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

#ifndef SHOW_MORSE_CAPTION
#    define SHOW_MORSE_CAPTION 0
#endif

static uint32_t last_input_time = 0;
static bool     display_awake   = true;
static uint8_t  last_wave_color_slot      = 255;
static bool     wave_initialized          = false;
static uint8_t  last_wave_band            = 0xFF;
static uint8_t  wave_samples[WAVE_SAMPLE_COUNT] = {0};

#define MORSE_RECENT_HISTORY_LEN  18
#define MORSE_IDLE_TIMEOUT_MS   10000

static char     morse_recent_history[MORSE_RECENT_HISTORY_LEN];
static uint8_t  morse_recent_len = 0;
static char     morse_last_drawn_caption[MORSE_RECENT_HISTORY_LEN + 1] = {0};

static uint32_t morse_last_input_ms     = 0;
// ---------- Morse (plain pulses) ----------
#define MORSE_UNIT_MS 204            // morse time unit (dot=1, dash=3). Lower = faster.

// Base color for the Morse waveform tail (HSV 0..255) – muted Nostromo lime
#define MORSE_BASE_H  96
#define MORSE_BASE_S  88
#define MORSE_BASE_V 204

typedef struct {
    uint16_t threshold_wpm;  // minimum smoothed WPM for this band
    uint8_t  head_h;
    uint8_t  head_s;
    uint8_t  head_v;
} morse_color_band_t;

// Head colors (newest columns) shift warmer as WPM increases; keep saturation modest
static const morse_color_band_t morse_color_bands[] = {
    {  0,  86,  92, 210},   // calm typing -> gentle yellow-green accent
    {100,  58, 120, 218},   // picking up speed -> muted yellow
    {110,  36, 142, 220},   // fast -> softened amber
    {120,  18, 168, 214},   // very fast -> warm red-orange without neon
};

#define MORSE_THICKNESS 3            // vertical thickness (pixels) of the Morse pulses

// phrases to loop (edit to taste)
static const char *morse_phrases[] = { "SOS", "NOSTROMO", "HELP"};

static uint8_t  morse_timeline[1024];  // 0=OFF, 1=ON
static size_t   morse_len = 0;
static size_t   morse_pos = 0;
static size_t   morse_phrase_idx = 0;
static uint32_t morse_last_tick = 0;

static inline uint8_t lerp_u8(uint8_t a, uint8_t b, uint16_t t_num, uint16_t t_den) {
    // linear interpolate (a..b) with t in [0..1] represented as t_num/t_den
    int16_t da = (int16_t)b - (int16_t)a;
    return (uint8_t)(a + (int32_t)da * t_num / (int32_t)t_den);
}

static inline uint8_t lerp_hue8(uint8_t h1, uint8_t h2, uint16_t t_num, uint16_t t_den) {
    // shortest-arc hue interpolation in 0..255
    int16_t dh = (int16_t)h2 - (int16_t)h1;
    if (dh > 127)  dh -= 256;
    if (dh < -128) dh += 256;
    int16_t h = (int16_t)h1 + (int32_t)dh * t_num / (int32_t)t_den;
    if (h < 0) h += 256;
    return (uint8_t)(h & 0xFF);
}

static const char *morse_code_for_char(char c) {
    switch ((c >= 'a' && c <= 'z') ? (c - 32) : c) {
        case 'A': return ".-";   case 'B': return "-..."; case 'C': return "-.-.";
        case 'D': return "-..";  case 'E': return ".";    case 'F': return "..-.";
        case 'G': return "--.";  case 'H': return "...."; case 'I': return "..";
        case 'J': return ".---"; case 'K': return "-.-";  case 'L': return ".-..";
        case 'M': return "--";   case 'N': return "-.";   case 'O': return "---";
        case 'P': return ".--."; case 'Q': return "--.-"; case 'R': return ".-.";
        case 'S': return "...";  case 'T': return "-";    case 'U': return "..-";
        case 'V': return "...-"; case 'W': return ".--";  case 'X': return "-..-";
        case 'Y': return "-.--"; case 'Z': return "--..";
        case '0': return "-----"; case '1': return ".----"; case '2': return "..---";
        case '3': return "...--"; case '4': return "....-"; case '5': return ".....";
        case '6': return "-...."; case '7': return "--..."; case '8': return "---..";
        case '9': return "----.";
        case '.': return ".-.-.-";  case ',': return "--..--";
        case '?': return "..--..";  case '!': return "-.-.--";
        case '/': return "-..-.";   case '(': return "-.--.";
        case ')': return "-.--.-";  case '&': return ".-...";
        case ':': return "---...";  case ';': return "-.-.-.";
        case '=': return "-...-";   case '+': return ".-.-.";
        case '-': return "-....-";  case '_': return "..--.-";
        case '"': return ".-..-.";  case '$': return "...-..-";
        case '\'': return ".----."; case '@': return ".--.-.";
        default:  return "";
    }
}

static void build_morse_timeline(const char *phrase, uint8_t trailing_gap_units) {
    size_t pos = 0;
    if (!phrase) {
        morse_len = 0;
        morse_pos = 0;
        return;
    }
    const size_t max_len = sizeof(morse_timeline);

    for (const char *p = phrase; *p && pos < max_len; ++p) {
        char c = *p;
        if (c == ' ') {
            for (int k = 0; k < 7 && pos < max_len; ++k) morse_timeline[pos++] = 0; // canonical word gap (7 units)
            continue;
        }
        const char *code = morse_code_for_char(c);
        if (!code || !*code) {
            for (int k = 0; k < 3 && pos < max_len; ++k) morse_timeline[pos++] = 0; // unknown char gap
            continue;
        }
        for (const char *e = code; *e && pos < max_len; ++e) {
            int on_units = (*e == '.') ? 1 : 3;
            for (int u = 0; u < on_units && pos < max_len; ++u) morse_timeline[pos++] = 1; // ON
            if (e[1] && pos < max_len) morse_timeline[pos++] = 0; // intra-element gap
        }
        char next = *(p + 1);
        if (next != '\0' && next != ' ') {
            for (int k = 0; k < 3 && pos < max_len; ++k) morse_timeline[pos++] = 0; // letter gap
        }
    }
    for (int k = 0; k < trailing_gap_units && pos < max_len; ++k) morse_timeline[pos++] = 0; // trailing gap

    morse_len = pos;
    morse_pos = 0;
    morse_last_tick = timer_read32();
}


static void __attribute__((unused)) morse_recent_to_string(char *out, size_t out_len) {
    size_t copy_len = (morse_recent_len < (out_len - 1)) ? morse_recent_len : (out_len - 1);
    if (copy_len) {
        memcpy(out, &morse_recent_history[morse_recent_len - copy_len], copy_len);
    }
    out[copy_len] = '\0';
}



static void ensure_morse_timeline(void) {
    if (morse_len && morse_pos < morse_len) {
        return;
    }

    morse_len = 0;
    morse_pos = 0;

    // Always play the configured phrases in a loop (no live-typing feed).
    build_morse_timeline(morse_phrases[morse_phrase_idx], 7);
    morse_phrase_idx = (morse_phrase_idx + 1) % (ARRAY_SIZE(morse_phrases));
}


static bool morse_translate_keycode(uint16_t keycode, bool shifted, char *morse_char, char *display_char, bool *is_backspace) {
    *is_backspace = false;
    *morse_char   = 0;
    *display_char = 0;

    if (keycode >= KC_A && keycode <= KC_Z) {
        char base = (char)('a' + (keycode - KC_A));
        *display_char = shifted ? (char)toupper((unsigned char)base) : base;
        *morse_char   = (char)toupper((unsigned char)base);
        return true;
    }

    switch (keycode) {
        case KC_1: *display_char = shifted ? '!' : '1'; *morse_char = shifted ? '!' : '1'; return true;
        case KC_2: *display_char = shifted ? '@' : '2'; *morse_char = shifted ? '@' : '2'; return true;
        case KC_3: *display_char = shifted ? '#' : '3'; *morse_char = shifted ? '#' : '3'; return true;
        case KC_4: *display_char = shifted ? '$' : '4'; *morse_char = shifted ? '$' : '4'; return true;
        case KC_5: *display_char = shifted ? '%' : '5'; *morse_char = shifted ? '%' : '5'; return true;
        case KC_6: *display_char = shifted ? '^' : '6'; *morse_char = shifted ? '^' : '6'; return true;
        case KC_7: *display_char = shifted ? '&' : '7'; *morse_char = shifted ? '&' : '7'; return true;
        case KC_8: *display_char = shifted ? '*' : '8'; *morse_char = shifted ? '*' : '8'; return true;
        case KC_9: *display_char = shifted ? '(' : '9'; *morse_char = shifted ? '(' : '9'; return true;
        case KC_0: *display_char = shifted ? ')' : '0'; *morse_char = shifted ? ')' : '0'; return true;

        case KC_SPACE: *display_char = ' '; *morse_char = ' '; return true;
        case KC_TAB:   *display_char = ' '; *morse_char = ' '; return true;
        case KC_ENTER: *display_char = ' '; *morse_char = ' '; return true;

        case KC_MINUS:    *display_char = shifted ? '_' : '-'; *morse_char = shifted ? '_' : '-'; return true;
        case KC_EQUAL:    *display_char = shifted ? '+' : '='; *morse_char = shifted ? '+' : '='; return true;
        case KC_LBRC: *display_char = shifted ? '{' : '['; *morse_char = shifted ? '{' : '['; return true;
        case KC_RBRC: *display_char = shifted ? '}' : ']'; *morse_char = shifted ? '}' : ']'; return true;
        case KC_BSLS:    *display_char = shifted ? '|' : '\\'; *morse_char = shifted ? '|' : '\\'; return true;
        case KC_COLON:   *display_char = shifted ? ':' : ';'; *morse_char = shifted ? ':' : ';'; return true;
        case KC_QUOTE:    *display_char = shifted ? '"' : '\''; *morse_char = shifted ? '"' : '\''; return true;
        case KC_GRAVE:    *display_char = shifted ? '~' : '`'; *morse_char = shifted ? '~' : '`'; return true;
        case KC_COMMA:    *display_char = shifted ? '<' : ','; *morse_char = shifted ? '<' : ','; return true;
        case KC_DOT:      *display_char = shifted ? '>' : '.'; *morse_char = shifted ? '>' : '.'; return true;
        case KC_SLASH:    *display_char = shifted ? '?' : '/'; *morse_char = shifted ? '?' : '/'; return true;

        case KC_BSPC:
            *is_backspace = true;
            return true;

        default:
            break;
    }

    return false;
}


bool process_record_kb(uint16_t keycode, keyrecord_t *record) {
    if (record->event.pressed) {
        uint16_t base_code = keycode;
        bool     tapped    = true;

#ifdef QK_MOD_TAP
        if (IS_QK_MOD_TAP(keycode) || IS_QK_LAYER_TAP(keycode)) {
            if (!(record->tap.count && !record->tap.interrupted)) {
                tapped = false;
            }
            base_code = (uint16_t)(keycode & 0xFF);
        }
#endif

#ifdef QK_TAP_DANCE
        if (IS_QK_TAP_DANCE(keycode)) {
            tapped = false;
        }
#endif
#ifdef QK_ONE_SHOT_MOD
        if (IS_QK_ONE_SHOT_MOD(keycode)) {
            tapped = false;
        }
#endif

        if (tapped) {
            uint8_t mods = get_mods() | get_oneshot_mods() | get_weak_mods();
            bool    shifted = (mods & MOD_MASK_SHIFT) != 0;

            bool backspace = false;
            char morse_char = 0;
            char display_char = 0;
            if (morse_translate_keycode(base_code, shifted, &morse_char, &display_char, &backspace)) {
                // keep the last-input timestamp so the display still wakes / idle logic works
                morse_last_input_ms = timer_read32();

                // --- DISABLE live-echo of typed keys:
                // Don't push typed keys into any live queue and don't update recent caption.
                // This preserves playing the predefined morse_phrases loop only.
            }
        }
    }

    return process_record_user(keycode, record);
}

// WPM smoothing and draw cache
static uint16_t last_drawn_wpm              = 0xFFFF;
static uint16_t wpm_ema                     = 0;     // EMA state (alpha ≈ 0.25)

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

    // init storage first time
    if (!wave_initialized) {
        for (uint8_t i = 0; i < WAVE_SAMPLE_COUNT; ++i) wave_samples[i] = 0;
        wave_initialized = true;
        last_wave_color_slot = 255;
        last_wave_band = 0xFF;
        force = true;
    }
    ensure_morse_timeline();

    uint32_t now = timer_read32();
    bool     changed = force;

    uint16_t wpm_for_color = wpm_ema ? wpm_ema : get_current_wpm();
    if (wpm_for_color > 200) {
        wpm_for_color = 200;
    }

    uint8_t color_band = 0;
    for (uint8_t i = 0; i < (uint8_t)ARRAY_SIZE(morse_color_bands); ++i) {
        if (wpm_for_color >= morse_color_bands[i].threshold_wpm) {
            color_band = i;
        } else {
            break;
        }
    }

    if (color_band != last_wave_band) {
        changed = true;
    }

    if (morse_last_tick == 0) morse_last_tick = now;

    // advance one unit (dot=1, dash=3) at MORSE_UNIT_MS cadence
    if (timer_elapsed32(morse_last_tick) >= MORSE_UNIT_MS) {
        // scroll left
        memmove(&wave_samples[0], &wave_samples[1], WAVE_SAMPLE_COUNT - 1);

        // next sample: 0/1
        uint8_t next_on = 0;
        if (morse_len && morse_pos < morse_len) {
            next_on = morse_timeline[morse_pos];
        }
        wave_samples[WAVE_SAMPLE_COUNT - 1] = next_on;

        // advance timeline (wrap to next phrase)
        morse_pos++;
        ensure_morse_timeline();

        morse_last_tick = now;
        changed = true;
    }

    // always redraw if layer changed (in case you later tie color to layer)
    if (layer != last_wave_color_slot) changed = true;

#if SHOW_MORSE_CAPTION
    // Caption draw: only clear/draw when there is an actual change (and only if there's text)
    bool caption_changed = false;
    char caption_buf[MORSE_RECENT_HISTORY_LEN + 1] = {0};
    bool want_caption = false;
    bool had_caption  = false;
    if (Retron27) {
        morse_recent_to_string(caption_buf, sizeof(caption_buf));
        want_caption = (caption_buf[0] != '\0');
        had_caption  = (morse_last_drawn_caption[0] != '\0');
        if (want_caption && had_caption) {
            caption_changed = (strncmp(caption_buf, morse_last_drawn_caption, sizeof(caption_buf)) != 0);
        } else {
            caption_changed = (want_caption != had_caption);
        }
    }

    if (!changed && !caption_changed) {
        return;
    }

    if (caption_changed && Retron27) {
        uint16_t line_height = Retron27->line_height;
        uint16_t char_w      = (line_height > 6) ? (line_height - 6) : line_height;
        uint16_t caption_y   = (WAVE_TOP > (line_height + 6)) ? (uint16_t)(WAVE_TOP - line_height - 6)
                                                              : (WAVE_TOP > line_height ? (uint16_t)(WAVE_TOP - line_height) : 0);
        uint16_t clear_left  = WAVE_LEFT;
        uint16_t clear_top   = (caption_y > STATUS_TEXT_BG_PADDING) ? (caption_y - STATUS_TEXT_BG_PADDING) : 0;
        uint16_t clear_right = clear_left + (uint16_t)(MORSE_RECENT_HISTORY_LEN * char_w) + (STATUS_TEXT_BG_PADDING * 2) + STATUS_TEXT_SHADOW_OFFSET;
        if (clear_right >= LCD_WIDTH) {
            clear_right = LCD_WIDTH - 1;
        }
        uint16_t clear_bottom = caption_y + line_height + STATUS_TEXT_BG_PADDING;
        if (clear_bottom >= LCD_HEIGHT) {
            clear_bottom = LCD_HEIGHT - 1;
        }
        if (clear_bottom >= WAVE_TOP) {
            clear_bottom = (WAVE_TOP > 0) ? (uint16_t)(WAVE_TOP - 1) : 0;
        }

        // Only clear if we used to have caption or we are about to draw one
        if (had_caption || want_caption) {
            qp_rect(lcd, clear_left, clear_top, clear_right, clear_bottom, 0, 0, 0, true);
        }
        if (want_caption) {
            draw_status_text(clear_left, caption_y, caption_buf, Retron27, MORSE_BASE_H, MORSE_BASE_S, MORSE_BASE_V);
        }
        strncpy(morse_last_drawn_caption, caption_buf, sizeof(morse_last_drawn_caption));
        morse_last_drawn_caption[sizeof(morse_last_drawn_caption) - 1] = '\0';
    }
#else
    // Caption disabled: if nothing else changed, bail early.
    if (!changed) {
        return;
    }
#endif

    // clear the waveform region
    qp_rect(lcd,
            WAVE_LEFT,
            WAVE_TOP,
            WAVE_LEFT + WAVE_WIDTH - 1,
            WAVE_TOP + WAVE_HEIGHT - 1,
            0, 0, 0, true);

    // draw thin horizontal pulses for ON columns with gradient color per column
    const uint16_t denom = (WAVE_SAMPLE_COUNT > 1) ? (WAVE_SAMPLE_COUNT - 1) : 1;
    const int16_t cy = (int16_t)(WAVE_TOP + (WAVE_HEIGHT / 2));
    int16_t top_y    = cy - (int16_t)((MORSE_THICKNESS - 1) / 2);
    int16_t bottom_y = top_y + MORSE_THICKNESS - 1;

    if (top_y < (int16_t)WAVE_TOP) top_y = (int16_t)WAVE_TOP;
    if (bottom_y > (int16_t)(WAVE_TOP + WAVE_HEIGHT - 1)) bottom_y = (int16_t)(WAVE_TOP + WAVE_HEIGHT - 1);

    uint8_t tail_h = MORSE_BASE_H;
    uint8_t tail_s = MORSE_BASE_S;
    uint8_t tail_v = MORSE_BASE_V;

    const morse_color_band_t *band = &morse_color_bands[color_band];
    uint8_t head_h = band->head_h;
    uint8_t head_s = band->head_s;
    uint8_t head_v = band->head_v;

    for (uint8_t i = 0; i < WAVE_SAMPLE_COUNT; ++i) {
        if (!wave_samples[i]) continue;

        // gradient from muted lime (older) toward warmer head tone based on current WPM
        uint16_t t = i;
        uint8_t h = lerp_hue8(tail_h, head_h, t, denom);
        uint8_t s = lerp_u8   (tail_s, head_s, t, denom);
        uint8_t v = lerp_u8   (tail_v, head_v, t, denom);

        int16_t left  = (int16_t)(WAVE_LEFT + i * WAVE_SAMPLE_WIDTH);
        int16_t right = (int16_t)(left + WAVE_SAMPLE_WIDTH - 1);

        qp_rect(lcd, left, top_y, right, bottom_y, h, s, v, true);

        // feather the top/bottom edges for a softer, larger look
        uint8_t glow_s = (uint8_t)(((uint16_t)s * 3) / 4);
        uint8_t glow_v = (uint8_t)(((uint16_t)v * 3) / 4);

        if (top_y > (int16_t)WAVE_TOP) {
            qp_rect(lcd, left, top_y - 1, right, top_y - 1, h, glow_s, glow_v, true);
        }
        if (bottom_y < (int16_t)(WAVE_TOP + WAVE_HEIGHT - 1)) {
            qp_rect(lcd, left, bottom_y + 1, right, bottom_y + 1, h, glow_s, glow_v, true);
        }
    }

    last_wave_color_slot = layer;
    last_wave_band       = color_band;
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
    last_wave_color_slot = 255;
    last_wave_band = 0xFF;
    for (uint8_t i = 0; i < WAVE_SAMPLE_COUNT; ++i) {
        wave_samples[i] = 0;
    }
    wave_initialized = true;
    morse_len = 0;           // force timeline build on first render
    morse_pos = 0;
    morse_last_tick = timer_read32();
    morse_recent_len = 0;
    morse_last_drawn_caption[0] = '\0';
    morse_last_input_ms = timer_read32();

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
    last_wave_color_slot = 255;
    last_wave_band = 0xFF;
    morse_last_input_ms = timer_read32();

    // Force a redraw of WPM and HUD area
    last_drawn_wpm = 0xFFFF;
    force_full_redraw = false; // keep false; update_display will decide based on layer/led changes

    // If display is asleep, wake it
    if (!display_awake) {
        display_do_wake();
    }
}
