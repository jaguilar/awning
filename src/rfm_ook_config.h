#ifndef AWNING_RFM_OOK_CONFIG_H
#define AWNING_RFM_OOK_CONFIG_H

#ifdef RFM_OOK_CONFIG_INCLUDE
#include RFM_OOK_CONFIG_INCLUDE
#else

#define RFM_OOK_SPI spi0
#define RFM_OOK_SPI_MOSI 3
#define RFM_OOK_SPI_MISO 4
#define RFM_OOK_SPI_SCK 2
#define RFM_OOK_SPI_CS 5
#define RFM_OOK_RESET 9

#endif

#endif