#include QMK_KEYBOARD_H
#include "keymap_german.h"

enum layers { BASE, SYM, NUM, FUN, NAV, OTHER };

#define A_LT LGUI_T(DE_A)
#define S_LT LALT_T(DE_S)
#define D_LT LCTL_T(DE_D)
#define F_LT LSFT_T(DE_F)
#define J_LT LSFT_T(DE_J)
#define K_LT LCTL_T(DE_K)
#define L_LT LALT_T(DE_L)
#define HASH_LT LGUI_T(DE_HASH)

#define OS_GUI OSM(MOD_LGUI)
#define OS_ALT OSM(MOD_LALT)
#define OS_CTL OSM(MOD_LCTL)
#define OS_SFT OSM(MOD_LSFT)

#define INS_MODE LSA(KC_INS)
#define IJ_RUN C(S(KC_F11))

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    [BASE] = LAYOUT(
        KC_ESC,  DE_Q,    DE_W,    DE_E,    DE_R,    DE_T,                                                        DE_Z,    DE_U,    DE_I,    DE_O,    DE_P,     KC_DEL,
        KC_TAB,  A_LT,    S_LT,    D_LT,    F_LT,    DE_G, KC_MUTE,                                      KC_CALC, DE_H,    J_LT,    K_LT,    L_LT,    HASH_LT,  DE_PLUS,
        DE_LABK, DE_Y,    DE_X,    DE_C,    DE_V,    DE_B, KC_MPRV, KC_MPLY, KC_MNXT, TG(NUM), TO(BASE), TG(NAV), DE_N,    DE_M,    DE_COMM, DE_DOT,  DE_MINS,  KC_ENT,
                                   MO(NUM), MO(FUN), KC_BSPC,                                                     KC_SPC,  MO(NAV), MO(SYM)
    ),
    [SYM] = LAYOUT(
        _______, DE_AT,   DE_LPRN, DE_RPRN, DE_UDIA, DE_SS,                                                         XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX, _______,
        _______, DE_BSLS, DE_LCBR, DE_RCBR, DE_ADIA, DE_ODIA, _______,                                     _______, XXXXXXX, KC_LSFT, KC_LCTL, KC_LALT, KC_LGUI, XXXXXXX,
        XXXXXXX, DE_EURO, DE_LBRC, DE_RBRC, DE_CIRC, DE_GRV,  _______, _______, _______, _______, _______, _______, XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX, _______,
                                   _______, _______, _______,                                                       _______, _______, _______

    ),
    [NUM] = LAYOUT(
        _______, XXXXXXX, XXXXXXX,    C(KC_PGUP), C(KC_PGDN), XXXXXXX,                                                       DE_7,    DE_8,    DE_9,    DE_TILD, XXXXXXX, _______,
        _______, KC_LGUI, KC_LALT,    KC_LCTL,    KC_LSFT,    XXXXXXX, _______,                                     _______, DE_4,    DE_5,    DE_6,    DE_PIPE, XXXXXXX, XXXXXXX,
        XXXXXXX, XXXXXXX, C(S(DE_X)), C(S(DE_C)), C(S(DE_V)), XXXXXXX, _______, _______, _______, _______, _______, _______, DE_1,    DE_2,    DE_3,    DE_QUES, XXXXXXX, _______,
                                      _______,    _______, _______,                                                 _______, _______, DE_0
    ),
    [FUN] = LAYOUT(
        _______, OS_GUI,  OS_ALT,  OS_CTL,  OS_SFT,  XXXXXXX,                                                       KC_F7,   KC_F8,   KC_F9,   KC_F12,  XXXXXXX, _______,
        _______, KC_LGUI, KC_LALT, KC_LCTL, KC_LSFT, XXXXXXX, _______,                                     _______, KC_F4,   KC_F5,   KC_F6,   KC_F11,  XXXXXXX, XXXXXXX,
        XXXXXXX, XXXXXXX, C(DE_X), C(DE_C), C(DE_V), XXXXXXX, _______, _______, _______, _______, _______, _______, KC_F1,   KC_F2,   KC_F3,   KC_F10,  XXXXXXX, _______,
                                   _______, _______, _______,                                                      _______,  _______, _______
    ),
    [NAV] = LAYOUT(
        CW_TOGG, XXXXXXX, KC_PGUP, KC_HOME, KC_UP,   KC_END,                                                         XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX, _______,
        KC_CAPS, XXXXXXX, KC_PGDN, KC_LEFT, KC_DOWN, KC_RIGHT, _______,                                     _______, XXXXXXX, KC_LSFT, KC_LCTL, KC_LALT, KC_LGUI, XXXXXXX,
        KC_INS,  XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX,  _______, _______, _______, _______, _______, _______, XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX, _______,
                                   _______, _______,  _______,                                     _______, _______, _______
    )
};

bool caps_word_press_user(uint16_t keycode) {
    switch (keycode) {
        case KC_A ... KC_Z:
        case KC_MINS:
        case DE_MINS:
        case DE_UDIA:
        case DE_ADIA:
        case DE_ODIA:
            add_weak_mods(MOD_BIT(KC_LSFT));
            return true;

        case KC_1 ... KC_0:
        case KC_BSPC:
        case KC_DEL:
        case KC_UNDS:
            return true;

        default:
            return false;
    }
}
