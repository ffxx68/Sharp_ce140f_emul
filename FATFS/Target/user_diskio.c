/* USER CODE BEGIN Header */
/**
 ******************************************************************************
  * @file    user_diskio.c
  * @brief   This file includes a diskio driver skeleton to be completed by the user.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
 /* USER CODE END Header */

#ifdef USE_OBSOLETE_USER_CODE_SECTION_0
/*
 * Warning: the user section 0 is no more in use (starting from CubeMx version 4.16.0)
 * To be suppressed in the future.
 * Kept to ensure backward compatibility with previous CubeMx versions when
 * migrating projects.
 * User code previously added there should be copied in the new user sections before
 * the section contents can be deleted.
 */
/* USER CODE BEGIN 0 */
/* USER CODE END 0 */
#endif

/* USER CODE BEGIN DECL */

/* Includes ------------------------------------------------------------------*/
#include <string.h>
#include <stdio.h>
#include "main.h"
#include "stm32l4xx_hal.h"
#include "ff_gen_drv.h"

extern void debug_log(const char *fmt, ...);

/* Private typedef -----------------------------------------------------------*/
/* Card type flags (CardType) */
#define CT_MMC		0x01	/* MMC ver 3 */
#define CT_SD1		0x02	/* SDC ver 1 */
#define CT_SD2		0x04	/* SDC ver 2 */
#define CT_BLOCK	0x08	/* Block addressing */

/* Private variables ---------------------------------------------------------*/
static volatile DSTATUS Stat = STA_NOINIT;
static uint8_t CardType = 0;

/* SPI helper functions */
static void SD_SPI_Init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_SPI1_CLK_ENABLE();
    __HAL_RCC_SPI1_FORCE_RESET();
    __HAL_RCC_SPI1_RELEASE_RESET();

    // PB5 -> CS (GPIO Output, Push-Pull, High Speed)
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_5, GPIO_PIN_SET);
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = GPIO_PIN_5;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    // PA5 -> SCK, PA7 -> MOSI (AF5 SPI1, Push-Pull, High Speed)
    GPIO_InitStruct.Pin = GPIO_PIN_5 | GPIO_PIN_7;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF5_SPI1;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    // PA6 -> MISO (AF5 SPI1, with Pull-Up)
    GPIO_InitStruct.Pin = GPIO_PIN_6;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF5_SPI1;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    // Configure SPI1
    SPI1->CR1 = 0;
    // CR2: Data size = 8 bit (DS = 0111), FRXTH = 1 (RXNE on 8 bit)
    SPI1->CR2 = (7 << SPI_CR2_DS_Pos) | SPI_CR2_FRXTH;
    // CR1: Master mode, Baudrate Prescaler = 256 (~312.5 kHz @ 80MHz), SSM = 1, SSI = 1
    SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | (7 << SPI_CR1_BR_Pos);
    // Enable SPI1
    SPI1->CR1 |= SPI_CR1_SPE;

    // Drain any leftover data in RX FIFO
    while (SPI1->SR & SPI_SR_RXNE) {
        (void)*(__IO uint8_t *)&SPI1->DR;
    }
}

static void SD_SPI_SetSpeed(uint8_t fast)
{
    SPI1->CR1 &= ~SPI_CR1_SPE;
    SPI1->CR1 &= ~SPI_CR1_BR_Msk;
    if (fast) {
        // Fast: APB2 / 8 = 10 MHz
        SPI1->CR1 |= (2 << SPI_CR1_BR_Pos);
    } else {
        // Slow: APB2 / 256 = 312.5 kHz
        SPI1->CR1 |= (7 << SPI_CR1_BR_Pos);
    }
    SPI1->CR1 |= SPI_CR1_SPE;
}

static inline uint8_t SD_SPI_ReadWriteByte(uint8_t data)
{
    if (SPI1->SR & SPI_SR_OVR) {
        (void)*(__IO uint8_t *)&SPI1->DR;
        (void)SPI1->SR;
    }
    while (SPI1->SR & SPI_SR_RXNE) {
        (void)*(__IO uint8_t *)&SPI1->DR;
    }

    uint32_t to = 20000;
    while (!(SPI1->SR & SPI_SR_TXE) && --to);

    *(__IO uint8_t *)&SPI1->DR = data;

    to = 20000;
    while (!(SPI1->SR & SPI_SR_RXNE) && --to);

    return *(__IO uint8_t *)&SPI1->DR;
}

static inline void SD_CS_LOW(void)
{
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_5, GPIO_PIN_RESET);
}

static inline void SD_CS_HIGH(void)
{
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_5, GPIO_PIN_SET);
}

static inline void SD_Deselect(void)
{
    SD_CS_HIGH();
    SD_SPI_ReadWriteByte(0xFF);
}

static uint8_t SD_WaitReady(void)
{
    uint32_t tickstart = HAL_GetTick();
    uint8_t res;
    do {
        res = SD_SPI_ReadWriteByte(0xFF);
        if (res == 0xFF) return 1;
    } while ((HAL_GetTick() - tickstart) < 500);
    return 0;
}

static uint8_t SD_Cmd(uint8_t cmd, uint32_t arg)
{
    uint8_t crc = 0x01;
    if (cmd == 0) crc = 0x95;
    else if (cmd == 8) crc = 0x87;

    SD_CS_LOW();

    SD_SPI_ReadWriteByte(0x40 | cmd);
    SD_SPI_ReadWriteByte((uint8_t)(arg >> 24));
    SD_SPI_ReadWriteByte((uint8_t)(arg >> 16));
    SD_SPI_ReadWriteByte((uint8_t)(arg >> 8));
    SD_SPI_ReadWriteByte((uint8_t)arg);
    SD_SPI_ReadWriteByte(crc);

    uint8_t res = 0xFF;
    for (int i = 0; i < 500; i++) {
        res = SD_SPI_ReadWriteByte(0xFF);
        if (!(res & 0x80)) break;
    }

    return res;
}

static uint8_t SD_Acmd(uint8_t acmd, uint32_t arg)
{
    uint8_t r = SD_Cmd(55, 0);
    SD_Deselect();
    if (r > 1) return r;
    r = SD_Cmd(acmd, arg);
    return r;
}

static uint8_t SD_RxDataBlock(BYTE *buff, UINT btr)
{
    uint8_t token;
    uint32_t tickstart = HAL_GetTick();

    do {
        token = SD_SPI_ReadWriteByte(0xFF);
    } while ((token == 0xFF) && (HAL_GetTick() - tickstart) < 200);

    if (token != 0xFE) {
        debug_log("SD_Rx token err 0x%02X\n", token);
        return 0;
    }

    for (UINT i = 0; i < btr; i++) {
        buff[i] = SD_SPI_ReadWriteByte(0xFF);
    }
    SD_SPI_ReadWriteByte(0xFF); // Discard CRC
    SD_SPI_ReadWriteByte(0xFF);

    return 1;
}

#if _USE_WRITE == 1
static uint8_t SD_TxDataBlock(const BYTE *buff, BYTE token)
{
    uint8_t resp;

    if (!SD_WaitReady()) return 0;

    SD_SPI_ReadWriteByte(token);
    if (token != 0xFD) {
        for (UINT i = 0; i < 512; i++) {
            SD_SPI_ReadWriteByte(buff[i]);
        }
        SD_SPI_ReadWriteByte(0xFF); // CRC dummy
        SD_SPI_ReadWriteByte(0xFF);
        resp = SD_SPI_ReadWriteByte(0xFF);
        if ((resp & 0x1F) != 0x05) {
            debug_log("SD_Tx resp err 0x%02X\n", resp);
            return 0;
        }
    }

    return 1;
}
#endif /* _USE_WRITE == 1 */

/* USER CODE END DECL */

/* Private function prototypes -----------------------------------------------*/
DSTATUS USER_initialize (BYTE pdrv);
DSTATUS USER_status (BYTE pdrv);
DRESULT USER_read (BYTE pdrv, BYTE *buff, DWORD sector, UINT count);
#if _USE_WRITE == 1
  DRESULT USER_write (BYTE pdrv, const BYTE *buff, DWORD sector, UINT count);
#endif /* _USE_WRITE == 1 */
#if _USE_IOCTL == 1
  DRESULT USER_ioctl (BYTE pdrv, BYTE cmd, void *buff);
#endif /* _USE_IOCTL == 1 */

Diskio_drvTypeDef  USER_Driver =
{
  USER_initialize,
  USER_status,
  USER_read,
#if  _USE_WRITE
  USER_write,
#endif  /* _USE_WRITE == 1 */
#if  _USE_IOCTL == 1
  USER_ioctl,
#endif /* _USE_IOCTL == 1 */
};

/* Private functions ---------------------------------------------------------*/

/**
  * @brief  Initializes a Drive
  * @param  pdrv: Physical drive number (0..)
  * @retval DSTATUS: Operation status
  */
DSTATUS USER_initialize (
	BYTE pdrv           /* Physical drive nmuber to identify the drive */
)
{
  /* USER CODE BEGIN INIT */
    uint8_t r, ocr[4];

    debug_log("USER_initialize(pdrv=%d)\n", pdrv);
    if (pdrv != 0) return STA_NOINIT;

    SD_SPI_Init();
    SD_SPI_SetSpeed(0); // Slow clock for init <= 400kHz

    // Send at least 80 clocks with CS HIGH
    SD_CS_HIGH();
    for (int i = 0; i < 20; i++) {
        SD_SPI_ReadWriteByte(0xFF);
    }

    // Send CMD0 to enter SPI mode
    r = 0xFF;
    for (int i = 0; i < 100; i++) {
        r = SD_Cmd(0, 0);
        SD_Deselect();
        if (r == 0x01) break;
        HAL_Delay(10);
    }
    debug_log("CMD0 resp: 0x%02X\n", r);

    if (r != 0x01) {
        Stat = STA_NOINIT;
        debug_log("SD Init FAIL (CMD0)!\n");
        return Stat;
    }

    // Check SD version with CMD8 (arg: 0x1AA)
    r = SD_Cmd(8, 0x1AA);
    debug_log("CMD8 resp: 0x%02X\n", r);

    uint8_t ty = 0;
    if (r == 0x01) {
        // SDv2+
        for (int i = 0; i < 4; i++) {
            ocr[i] = SD_SPI_ReadWriteByte(0xFF);
        }
        SD_Deselect();
        debug_log("CMD8 data: %02X %02X %02X %02X\n", ocr[0], ocr[1], ocr[2], ocr[3]);

        if (ocr[2] == 0x01 && ocr[3] == 0xAA) {
            uint32_t start = HAL_GetTick();
            uint8_t r41 = 0xFF;
            while ((HAL_GetTick() - start) < 1000) {
                r41 = SD_Acmd(41, 1UL << 30); // ACMD41 with HCS
                SD_Deselect();
                if (r41 == 0) break;
                HAL_Delay(10);
            }
            debug_log("ACMD41 resp: 0x%02X\n", r41);

            if (r41 == 0) {
                r = SD_Cmd(58, 0); // READ_OCR
                if (r == 0) {
                    for (int i = 0; i < 4; i++) {
                        ocr[i] = SD_SPI_ReadWriteByte(0xFF);
                    }
                    debug_log("CMD58 OCR: %02X %02X %02X %02X\n", ocr[0], ocr[1], ocr[2], ocr[3]);
                    ty = (ocr[0] & 0x40) ? (CT_SD2 | CT_BLOCK) : CT_SD2;
                }
                SD_Deselect();
            }
        }
    } else {
        // SDv1 or MMC
        SD_Deselect();
        uint8_t cmd = (SD_Acmd(41, 0) <= 1) ? 41 : 1;
        SD_Deselect();
        uint32_t start = HAL_GetTick();
        while ((HAL_GetTick() - start) < 1000) {
            if (cmd == 41) r = SD_Acmd(41, 0);
            else r = SD_Cmd(1, 0);
            SD_Deselect();
            if (r == 0) break;
            HAL_Delay(10);
        }
        if (r == 0) {
            ty = (cmd == 41) ? CT_SD1 : CT_MMC;
            r = SD_Cmd(16, 512); // SET_BLOCKLEN
            SD_Deselect();
            if (r != 0) ty = 0;
        }
    }

    CardType = ty;
    debug_log("SD CardType: 0x%02X\n", ty);

    if (ty) {
        Stat &= ~STA_NOINIT;
        SD_SPI_SetSpeed(1); // Fast clock (10 MHz)
        debug_log("SD Init OK!\n");
    } else {
        Stat = STA_NOINIT;
        debug_log("SD Init FAIL!\n");
    }

    return Stat;
  /* USER CODE END INIT */
}

/**
  * @brief  Gets Disk Status
  * @param  pdrv: Physical drive number (0..)
  * @retval DSTATUS: Operation status
  */
DSTATUS USER_status (
	BYTE pdrv       /* Physical drive number to identify the drive */
)
{
  /* USER CODE BEGIN STATUS */
    if (pdrv != 0) return STA_NOINIT;
    return Stat;
  /* USER CODE END STATUS */
}

/**
  * @brief  Reads Sector(s)
  * @param  pdrv: Physical drive number (0..)
  * @param  *buff: Data buffer to store read data
  * @param  sector: Sector address (LBA)
  * @param  count: Number of sectors to read (1..128)
  * @retval DRESULT: Operation result
  */
DRESULT USER_read (
	BYTE pdrv,      /* Physical drive nmuber to identify the drive */
	BYTE *buff,     /* Data buffer to store read data */
	DWORD sector,   /* Sector address in LBA */
	UINT count      /* Number of sectors to read */
)
{
  /* USER CODE BEGIN READ */
    if (pdrv != 0 || !count) return RES_PARERR;
    if (Stat & STA_NOINIT) return RES_NOTRDY;

    if (!(CardType & CT_BLOCK)) sector *= 512;

    if (count == 1) {
        if (SD_Cmd(17, sector) == 0) {
            if (SD_RxDataBlock(buff, 512)) count = 0;
        }
        SD_Deselect();
    } else {
        if (SD_Cmd(18, sector) == 0) {
            do {
                if (!SD_RxDataBlock(buff, 512)) break;
                buff += 512;
            } while (--count);
            SD_Cmd(12, 0); // STOP_TRANSMISSION
        }
        SD_Deselect();
    }

    return count ? RES_ERROR : RES_OK;
  /* USER CODE END READ */
}

/**
  * @brief  Writes Sector(s)
  * @param  pdrv: Physical drive number (0..)
  * @param  *buff: Data to be written
  * @param  sector: Sector address (LBA)
  * @param  count: Number of sectors to write (1..128)
  * @retval DRESULT: Operation result
  */
#if _USE_WRITE == 1
DRESULT USER_write (
	BYTE pdrv,          /* Physical drive nmuber to identify the drive */
	const BYTE *buff,   /* Data to be written */
	DWORD sector,       /* Sector address in LBA */
	UINT count          /* Number of sectors to write */
)
{
  /* USER CODE BEGIN WRITE */
    if (pdrv != 0 || !count) return RES_PARERR;
    if (Stat & STA_NOINIT) return RES_NOTRDY;
    if (Stat & STA_PROTECT) return RES_WRPRT;

    if (!(CardType & CT_BLOCK)) sector *= 512;

    if (count == 1) {
        if (SD_Cmd(24, sector) == 0) {
            if (SD_TxDataBlock(buff, 0xFE)) count = 0;
        }
        SD_Deselect();
    } else {
        if (CardType & (CT_SD1 | CT_SD2)) {
            SD_Acmd(23, count);
            SD_Deselect();
        }
        if (SD_Cmd(25, sector) == 0) {
            do {
                if (!SD_TxDataBlock(buff, 0xFC)) break;
                buff += 512;
            } while (--count);
            if (!SD_TxDataBlock(0, 0xFD)) count = 1;
        }
        SD_Deselect();
    }

    return count ? RES_ERROR : RES_OK;
  /* USER CODE END WRITE */
}
#endif /* _USE_WRITE == 1 */

/**
  * @brief  I/O control operation
  * @param  pdrv: Physical drive number (0..)
  * @param  cmd: Control code
  * @param  *buff: Buffer to send/receive control data
  * @retval DRESULT: Operation result
  */
#if _USE_IOCTL == 1
DRESULT USER_ioctl (
	BYTE pdrv,      /* Physical drive nmuber (0..) */
	BYTE cmd,       /* Control code */
	void *buff      /* Buffer to send/receive control data */
)
{
  /* USER CODE BEGIN IOCTL */
    DRESULT res = RES_ERROR;
    BYTE n, csd[16];
    DWORD csize;

    if (pdrv != 0) return RES_PARERR;
    if (Stat & STA_NOINIT) return RES_NOTRDY;

    switch (cmd) {
    case CTRL_SYNC:
        SD_CS_LOW();
        if (SD_WaitReady()) res = RES_OK;
        SD_Deselect();
        break;

    case GET_SECTOR_COUNT:
        if (SD_Cmd(9, 0) == 0 && SD_RxDataBlock(csd, 16)) {
            if ((csd[0] >> 6) == 1) { // SDC ver 2.00 (SDHC/SDXC)
                csize = csd[9] + ((DWORD)csd[8] << 8) + ((DWORD)(csd[7] & 63) << 16) + 1;
                *(DWORD*)buff = csize << 10;
            } else { // SDC ver 1.XX or MMC
                n = (csd[5] & 15) + ((csd[10] & 128) >> 7) + ((csd[9] & 3) << 1) + 2;
                csize = (csd[8] >> 6) + ((WORD)csd[7] << 2) + ((WORD)(csd[6] & 3) << 10) + 1;
                *(DWORD*)buff = (DWORD)csize << (n - 9);
            }
            res = RES_OK;
        }
        SD_Deselect();
        break;

    case GET_SECTOR_SIZE:
        *(WORD*)buff = 512;
        res = RES_OK;
        break;

    case GET_BLOCK_SIZE:
        *(DWORD*)buff = 1;
        res = RES_OK;
        break;

    case CTRL_TRIM:
        res = RES_OK;
        break;

    default:
        res = RES_PARERR;
        break;
    }

    return res;
  /* USER CODE END IOCTL */
}
#endif /* _USE_IOCTL == 1 */


