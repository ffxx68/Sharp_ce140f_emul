/*
 * sd_spi.h - MMC/SDC (in SPI mode) low level driver for the CE-140F emulator
 *
 * PCB v1 wiring (Nucleo-L432KC):
 *    SCK  = PA5  (SPI1_SCK,  AF5)   NOTE: solder bridge SB16 must be open
 *    MISO = PA6  (SPI1_MISO, AF5)   NOTE: solder bridge SB18 must be open
 *    MOSI = PA7  (SPI1_MOSI, AF5)
 *    CS   = PB5  (plain GPIO output)
 *
 * This is the layer the CubeMX-generated FATFS/Target/user_diskio.c skeleton
 * was missing: without it every f_mount()/f_open() call fails and the emulator
 * can only answer commands that do not touch the card.
 */
#ifndef SD_SPI_H
#define SD_SPI_H

#include "main.h"
#include "ffconf.h"
#include "diskio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SPI clock = PCLK2 / prescaler, written straight into CR1_BR.
   PCLK2 is 32 MHz with the current clock tree. */
#define SD_SPI_PRESCALER_SLOW   (6u << SPI_CR1_BR_Pos)  /* /128 -> 250 kHz, card init */
#define SD_SPI_PRESCALER_FAST   (2u << SPI_CR1_BR_Pos)  /* /8   -> 4 MHz, normal use  */

#define SD_CS_Pin               GPIO_PIN_5
#define SD_CS_GPIO_Port         GPIOB

/* Configures SPI1 and the CS pin. Call once, before MX_FATFS_Init(). */
void SD_SPI_Init(void);

/* diskio back-end */
DSTATUS SD_disk_initialize(void);
DSTATUS SD_disk_status(void);
DRESULT SD_disk_read(BYTE *buff, DWORD sector, UINT count);
DRESULT SD_disk_write(const BYTE *buff, DWORD sector, UINT count);
DRESULT SD_disk_ioctl(BYTE cmd, void *buff);

#ifdef __cplusplus
}
#endif

#endif /* SD_SPI_H */
