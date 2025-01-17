// Copyright 2024 Ashish Shrestha (@axhixh)
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#define RP2040_BOOTLOADER_DOUBLE_TAP_RESET
#define RP2040_BOOTLOADER_DOUBLE_TAP_RESET_TIMEOUT 1000U

#define SERIAL_USART_TX_PIN GP0
// flash this to left side (unconnected)

#define MATRIX_ROW_PINS { GP29, GP28, GP27, GP14, GP15}  //somehow fried gp26
#define MATRIX_COL_PINS { GP3, GP4, GP5, GP6, GP7, GP8}
#define MATRIX_ROW_PINS_RIGHT { GP29, GP28, GP27, GP26, GP15 }  
#define SPLIT_HAND_PIN GP26
#define	SPLIT_HAND_PIN_LOW_IS_LEFT

// flash this to left side (unconnected)

#define DYNAMIC_KEYMAP_LAYER_COUNT 10


// // flash this to the right side

// #define MATRIX_ROW_PINS { GP29, GP28, GP27, GP26, GP15}  //somehow fried gp26
// #define MATRIX_COL_PINS { GP3, GP4, GP5, GP6, GP7, GP8}

// // flash this to the right side