#pragma once

#include "quantum.h"

#define LAYOUT( \
  L06, L05, L04, L03, L02, L01,                                     R01, R02, R03, R04, R05, R06, \
  L12, L11, L10, L09, L08, L07,                                     R07, R08, R09, R10, R11, R12, \
  L18, L17, L16, L15, L14, L13, L25, L24,                 R24, R25, R13, R14, R15, R16, R17, R18, \
                 L23, L22, L21, L20, L19,                 R19, R20, R21, R22, R23             \
  ) \
  { \
    { L01, L02, L03, L04, L05, L06}, \
    { L07, L08, L09, L10, L11, L12}, \
    { L13, L14, L15, L16, L17, L18}, \
    { L19, L20, L21, L22, L23, L24}, \
    { L25, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO,}, \
    { R01, R02, R03, R04, R05, R06}, \
    { R07, R08, R09, R10, R11, R12}, \
    { R13, R14, R15, R16, R17, R18}, \
    { R19, R20, R21, R22, R23, R24}, \
    { R25, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO,} \
  }
