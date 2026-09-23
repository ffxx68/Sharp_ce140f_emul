/*
 * sd_spi.c - MMC/SDC (in SPI mode) low level driver for the CE-140F emulator.
 *
 * Replaces what the Mbed build got from the SDFileSystem library, which the
 * CubeIDE port never brought over (FATFS/Target/user_diskio.c was left as the
 * empty CubeMX skeleton, so every f_mount() failed).
 *
 * Same wiring as the Mbed version for the PCB v1 board:
 *      SDFileSystem sd(PA_7, PA_6, PA_5, PB_5, "sd"); // mosi, miso, sclk, cs
 */

#include "sd_spi.h"
#include <string.h>

/* MMC/SD command set (SPI mode) */
#define CMD0    (0)         /* GO_IDLE_STATE */
#define CMD1    (1)         /* SEND_OP_COND (MMC) */
#define ACMD41  (0x80 + 41) /* SEND_OP_COND (SDC) */
#define CMD8    (8)         /* SEND_IF_COND */
#define CMD9    (9)         /* SEND_CSD */
#define CMD10   (10)        /* SEND_CID */
#define CMD12   (12)        /* STOP_TRANSMISSION */
#define ACMD13  (0x80 + 13) /* SD_STATUS (SDC) */
#define CMD16   (16)        /* SET_BLOCKLEN */
#define CMD17   (17)        /* READ_SINGLE_BLOCK */
#define CMD18   (18)        /* READ_MULTIPLE_BLOCK */
#define CMD23   (23)        /* SET_BLOCK_COUNT (MMC) */
#define ACMD23  (0x80 + 23) /* SET_WR_BLK_ERASE_COUNT (SDC) */
#define CMD24   (24)        /* WRITE_BLOCK */
#define CMD25   (25)        /* WRITE_MULTIPLE_BLOCK */
#define CMD32   (32)        /* ERASE_ER_BLK_START */
#define CMD33   (33)        /* ERASE_ER_BLK_END */
#define CMD38   (38)        /* ERASE */
#define CMD55   (55)        /* APP_CMD */
#define CMD58   (58)        /* READ_OCR */

/* Card type flags (CardType) */
#define CT_MMC      0x01
#define CT_SD1      0x02
#define CT_SD2      0x04
#define CT_SDC      (CT_SD1 | CT_SD2)
#define CT_BLOCK    0x08    /* block addressing (SDHC/SDXC) */

static volatile DSTATUS Stat = STA_NOINIT;
static BYTE CardType;

/*--------------------------------------------------------------------------
   Low level SPI helpers
---------------------------------------------------------------------------*/

static void spi_set_prescaler(uint32_t prescaler)
{
    SPI1->CR1 &= ~SPI_CR1_SPE;
    SPI1->CR1 = (SPI1->CR1 & ~SPI_CR1_BR_Msk) | (prescaler & SPI_CR1_BR_Msk);
    SPI1->CR1 |= SPI_CR1_SPE;
}

/* One byte out / one byte in. Direct register access: the HAL call overhead
   would dominate at 8 MHz, and a 512-byte sector needs 512 of these. */
static BYTE xchg_spi(BYTE dat)
{
    while (!(SPI1->SR & SPI_SR_TXE)) { }
    *(__IO uint8_t *)&SPI1->DR = dat;       /* 8-bit access: one frame only */
    while (!(SPI1->SR & SPI_SR_RXNE)) { }
    return *(__IO uint8_t *)&SPI1->DR;
}

static void rcvr_spi_multi(BYTE *buff, UINT btr)
{
    while (btr--) {
        *buff++ = xchg_spi(0xFF);
    }
}

static void xmit_spi_multi(const BYTE *buff, UINT btx)
{
    while (btx--) {
        xchg_spi(*buff++);
    }
}

#define CS_LOW()   HAL_GPIO_WritePin(SD_CS_GPIO_Port, SD_CS_Pin, GPIO_PIN_RESET)
#define CS_HIGH()  HAL_GPIO_WritePin(SD_CS_GPIO_Port, SD_CS_Pin, GPIO_PIN_SET)

/* Wait until the card releases the DO line (not busy). Returns 1 on success. */
static int wait_ready(UINT wt_ms)
{
    uint32_t start = HAL_GetTick();
    BYTE d;

    do {
        d = xchg_spi(0xFF);
    } while (d != 0xFF && (HAL_GetTick() - start) < wt_ms);

    return (d == 0xFF) ? 1 : 0;
}

static void deselect(void)
{
    CS_HIGH();
    xchg_spi(0xFF);     /* provide the 8 clocks the card needs to release DO */
}

static int select_card(void)
{
    CS_LOW();
    xchg_spi(0xFF);     /* dummy clock to force DO enabled */

    if (wait_ready(500)) return 1;
    deselect();
    return 0;
}

/* Receive a data packet (token + payload + CRC) */
static int rcvr_datablock(BYTE *buff, UINT btr)
{
    BYTE token;
    uint32_t start = HAL_GetTick();

    do {
        token = xchg_spi(0xFF);
    } while (token == 0xFF && (HAL_GetTick() - start) < 200);

    if (token != 0xFE) return 0;    /* invalid data token */

    rcvr_spi_multi(buff, btr);
    xchg_spi(0xFF);                 /* discard CRC */
    xchg_spi(0xFF);
    return 1;
}

/* Send a data packet. token 0xFE = single/first block, 0xFD = stop tran */
static int xmit_datablock(const BYTE *buff, BYTE token)
{
    BYTE resp;

    if (!wait_ready(500)) return 0;

    xchg_spi(token);
    if (token != 0xFD) {
        xmit_spi_multi(buff, 512);
        xchg_spi(0xFF);             /* dummy CRC */
        xchg_spi(0xFF);

        resp = xchg_spi(0xFF);
        if ((resp & 0x1F) != 0x05) return 0;   /* not accepted */
    }
    return 1;
}

/* Send a command packet, return the R1 response (bit7 set = timeout) */
static BYTE send_cmd(BYTE cmd, DWORD arg)
{
    BYTE n, res;

    if (cmd & 0x80) {   /* ACMDxx is CMD55 followed by CMDxx */
        cmd &= 0x7F;
        res = send_cmd(CMD55, 0);
        if (res > 1) return res;
    }

    /* Select the card and wait for it to become ready, except for CMD12 */
    if (cmd != CMD12) {
        deselect();
        if (!select_card()) return 0xFF;
    }

    xchg_spi(0x40 | cmd);
    xchg_spi((BYTE)(arg >> 24));
    xchg_spi((BYTE)(arg >> 16));
    xchg_spi((BYTE)(arg >> 8));
    xchg_spi((BYTE)arg);

    n = 0x01;                       /* dummy CRC + stop bit */
    if (cmd == CMD0) n = 0x95;      /* valid CRC for CMD0(0) */
    if (cmd == CMD8) n = 0x87;      /* valid CRC for CMD8(0x1AA) */
    xchg_spi(n);

    if (cmd == CMD12) xchg_spi(0xFF);   /* discard one byte after CMD12 */

    n = 10;
    do {
        res = xchg_spi(0xFF);
    } while ((res & 0x80) && --n);

    return res;
}

/*--------------------------------------------------------------------------
   Public interface
---------------------------------------------------------------------------*/

void SD_SPI_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_SPI1_CLK_ENABLE();

    /* CS - plain output, idle high */
    HAL_GPIO_WritePin(SD_CS_GPIO_Port, SD_CS_Pin, GPIO_PIN_SET);
    GPIO_InitStruct.Pin = SD_CS_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(SD_CS_GPIO_Port, &GPIO_InitStruct);

    /* SCK / MISO / MOSI - SPI1 alternate function.
       NOTE: PA5 and PA6 only work as digital pins with SB16/SB18 open. */
    GPIO_InitStruct.Pin = GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;     /* keeps MISO defined with no card */
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF5_SPI1;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* SPI1 straight through the registers: the HAL SPI module is not part of
       this project (see stm32l4xx_hal_conf.h) and all we need is mode 0,
       8-bit frames, software NSS. */
    SPI1->CR1 = 0;
    SPI1->CR2 = SPI_CR2_FRXTH                                   /* RXNE on 8 bits */
              | SPI_CR2_DS_0 | SPI_CR2_DS_1 | SPI_CR2_DS_2;     /* DS = 0111 -> 8 bit */
    SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSI | SPI_CR1_SSM
              | (SD_SPI_PRESCALER_SLOW & SPI_CR1_BR_Msk);       /* CPOL=0, CPHA=0 */
    SPI1->CR1 |= SPI_CR1_SPE;
}

DSTATUS SD_disk_status(void)
{
    return Stat;
}

DSTATUS SD_disk_initialize(void)
{
    BYTE n, cmd, ty, ocr[4];
    uint32_t start;

    Stat = STA_NOINIT;
    CardType = 0;

    spi_set_prescaler(SD_SPI_PRESCALER_SLOW);

    CS_HIGH();
    for (n = 10; n; n--) xchg_spi(0xFF);    /* 80 dummy clocks */

    ty = 0;
    if (send_cmd(CMD0, 0) == 1) {           /* enter idle state */
        start = HAL_GetTick();
        if (send_cmd(CMD8, 0x1AA) == 1) {   /* SDv2? */
            for (n = 0; n < 4; n++) ocr[n] = xchg_spi(0xFF);
            if (ocr[2] == 0x01 && ocr[3] == 0xAA) {     /* 2.7-3.6V range */
                while ((HAL_GetTick() - start) < 1000 && send_cmd(ACMD41, 1UL << 30)) { }
                if ((HAL_GetTick() - start) < 1000 && send_cmd(CMD58, 0) == 0) {
                    for (n = 0; n < 4; n++) ocr[n] = xchg_spi(0xFF);
                    ty = (ocr[0] & 0x40) ? CT_SD2 | CT_BLOCK : CT_SD2;
                }
            }
        } else {                            /* SDv1 or MMCv3 */
            if (send_cmd(ACMD41, 0) <= 1) {
                ty = CT_SD1; cmd = ACMD41;
            } else {
                ty = CT_MMC; cmd = CMD1;
            }
            while ((HAL_GetTick() - start) < 1000 && send_cmd(cmd, 0)) { }
            if ((HAL_GetTick() - start) >= 1000 || send_cmd(CMD16, 512) != 0) {
                ty = 0;
            }
        }
    }
    CardType = ty;
    deselect();

    if (ty) {
        Stat &= ~STA_NOINIT;
        spi_set_prescaler(SD_SPI_PRESCALER_FAST);
    }
    return Stat;
}

DRESULT SD_disk_read(BYTE *buff, DWORD sector, UINT count)
{
    if (!count) return RES_PARERR;
    if (Stat & STA_NOINIT) return RES_NOTRDY;

    if (!(CardType & CT_BLOCK)) sector *= 512;  /* byte addressing */

    if (count == 1) {
        if ((send_cmd(CMD17, sector) == 0) && rcvr_datablock(buff, 512)) {
            count = 0;
        }
    } else {
        if (send_cmd(CMD18, sector) == 0) {
            do {
                if (!rcvr_datablock(buff, 512)) break;
                buff += 512;
            } while (--count);
            send_cmd(CMD12, 0);             /* stop transmission */
        }
    }
    deselect();

    return count ? RES_ERROR : RES_OK;
}

DRESULT SD_disk_write(const BYTE *buff, DWORD sector, UINT count)
{
    if (!count) return RES_PARERR;
    if (Stat & STA_NOINIT) return RES_NOTRDY;
    if (Stat & STA_PROTECT) return RES_WRPRT;

    if (!(CardType & CT_BLOCK)) sector *= 512;

    if (count == 1) {
        if ((send_cmd(CMD24, sector) == 0) && xmit_datablock(buff, 0xFE)) {
            count = 0;
        }
    } else {
        if (CardType & CT_SDC) send_cmd(ACMD23, count);
        if (send_cmd(CMD25, sector) == 0) {
            do {
                if (!xmit_datablock(buff, 0xFC)) break;
                buff += 512;
            } while (--count);
            if (!xmit_datablock(0, 0xFD)) count = 1;    /* stop token */
        }
    }
    deselect();

    return count ? RES_ERROR : RES_OK;
}

DRESULT SD_disk_ioctl(BYTE cmd, void *buff)
{
    DRESULT res = RES_ERROR;
    BYTE n, csd[16];
    DWORD csize;

    if (Stat & STA_NOINIT) return RES_NOTRDY;

    switch (cmd) {
    case CTRL_SYNC:
        if (select_card()) res = RES_OK;
        break;

    case GET_SECTOR_COUNT:
        if ((send_cmd(CMD9, 0) == 0) && rcvr_datablock(csd, 16)) {
            if ((csd[0] >> 6) == 1) {           /* SDv2 (CSD v2.0) */
                csize = csd[9] + ((WORD)csd[8] << 8) + ((DWORD)(csd[7] & 63) << 16) + 1;
                *(DWORD *)buff = csize << 10;
            } else {                            /* SDv1 or MMC */
                n = (csd[5] & 15) + ((csd[10] & 128) >> 7) + ((csd[9] & 3) << 1) + 2;
                csize = (csd[8] >> 6) + ((WORD)csd[7] << 2) + ((WORD)(csd[6] & 3) << 10) + 1;
                *(DWORD *)buff = csize << (n - 9);
            }
            res = RES_OK;
        }
        break;

    case GET_SECTOR_SIZE:
        *(WORD *)buff = 512;
        res = RES_OK;
        break;

    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 128;   /* erase block size in sectors, generic value */
        res = RES_OK;
        break;

    default:
        res = RES_PARERR;
        break;
    }
    deselect();

    return res;
}
