// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "quantum.h"
#include "qp.h"

// All values (including hue) are scaled to 0-255
#define HSV_BLACK 0, 0, 0

#define HSV_CAPS_OFF 17, 104, 77
#define HSV_CAPS_ON  17, 191, 245
#define HSV_SCROLL_OFF 202, 104, 77
#define HSV_SCROLL_ON  202, 191, 245
#define HSV_NUM_OFF  142, 104, 77
#define HSV_NUM_ON   142, 191, 245

#define HSV_LAYER_0 0, 0, 160
#define HSV_LAYER_1 23, 89, 255
#define HSV_LAYER_2 43, 71, 255
#define HSV_LAYER_3 0, 82, 255
#define HSV_LAYER_4 77, 64, 255
#define HSV_LAYER_5 176, 77, 255
#define HSV_LAYER_6 131, 99, 255
#define HSV_LAYER_7 154, 94, 255
#define HSV_LAYER_UNDEF 0, 255, 255

extern painter_device_t lcd;

void update_display(void);
void display_do_sleep(void);
void display_do_wake(void);
// Call this from your keymap when a key is pressed to update the display inactivity timer
// and wake the display if it's sleeping.
void hlc_tft_on_activity(void);

// Optional: keymaps can override to provide human-readable layer labels
const char *hlc_tft_layer_label(uint8_t layer);
const char *hlc_tft_layer_short(uint8_t layer);
