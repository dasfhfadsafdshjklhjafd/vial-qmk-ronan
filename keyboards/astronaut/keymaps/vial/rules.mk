VIA_ENABLE = yes
VIAL_ENABLE = yes
# VIAL_INSECURE = yes
CONSOLE_ENABLE = yes
DEFERRED_EXEC_ENABLE = yes
KEY_OVERRIDE_ENABLE = yes
WPM_ENABLE = yes

# ensure the painter driver headers are on the include path
CFLAGS += -Idrivers/painter -Idrivers/painter/generic
CFLAGS += -Ikeyboards/astronaut/tft_display
OPT_DEFS += -DSHOW_LAYER_LABEL=0
OPT_DEFS += -DSHOW_LAYER_GLYPH=1


# In keyboards/astronaut/keymaps/vial/rules.mk
QUANTUM_PAINTER_ENABLE = yes
QUANTUM_PAINTER_FONTS_ENABLE = yes
QUANTUM_PAINTER_GLYPHS_ENABLE = yes
QUANTUM_PAINTER_DRIVERS += st7789_spi

SRC += \
  tft_display/hlc_tft_display.c \
  tft_display/graphics/fonts/Retron2000-27.qff.c \
  tft_display/graphics/fonts/Retron2000-underline-27.qff.c \
  tft_display/graphics/numbers/0.qgf.c \
  tft_display/graphics/numbers/1.qgf.c \
  tft_display/graphics/numbers/2.qgf.c \
  tft_display/graphics/numbers/3.qgf.c \
  tft_display/graphics/numbers/4.qgf.c \
  tft_display/graphics/numbers/5.qgf.c \
  tft_display/graphics/numbers/6.qgf.c \
  tft_display/graphics/numbers/7.qgf.c \
  tft_display/graphics/numbers/8.qgf.c \
  tft_display/graphics/numbers/9.qgf.c \
  tft_display/graphics/numbers/undef.qgf.c
