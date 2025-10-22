#pragma once

#include_next "mcuconf.h"

// Use the RP2040's second SPI controller so GP10/GP11 can be used for SCK/MOSI.
#undef RP_SPI_USE_SPI0
#define RP_SPI_USE_SPI0 FALSE

#undef RP_SPI_USE_SPI1
#define RP_SPI_USE_SPI1 TRUE
