#ifndef COMMANDS_H
#define COMMANDS_H

#include "main.h"

/* Placed in SRAM2 (see the .sram2 section in the linker script): SRAM1 is only
 * 48 KB and outDataBuf alone takes 38 KB of it. Nothing here may rely on being
 * zero at reset - the startup code does not clear this section. */
#define SRAM2_DATA __attribute__((section(".sram2")))

/* Max file size during LOAD: the whole file is staged here before being sent.
 * Must stay at 40000 - the Mbed value. The CubeIDE port dropped it to 38000,
 * which brings back issue #6: a file over ~38 KB is silently truncated (we
 * report "file complete", the Sharp is still waiting for the rest) and the
 * machine hangs until BREAK. */
#define OUT_BUF_SIZE 40000
#define IN_BUF_SIZE 2000
#define MAX_N_FILES 6

#define ERR_PRINTOUT(x) debug_log("ERR %s", x); print_to_pc(x)
#define ERR_SD_CARD_NOT_PRESENT "SD Card not present!\n"

/* Error answer for the commands whose good reply is a single byte.
 *
 * Measured on the machine, answering a KILL of a file that is not there:
 *   0x10 -> I/O ERROR      0x80 -> "Error"      0xFF -> taken as success
 * The 0xFF this firmware used to send was simply ignored, so a KILL that
 * failed looked exactly like one that worked. */
#define ERR_REPLY 0x10

/* The name the emulator answers with a directory listing. Rewritten on every
 * OPEN ... FOR INPUT and hidden from FILES; not a real CE-140F feature. */
#define DIRLIST_NAME "DIRLIST.TXT"
#define SD_HOME "0:/"

extern volatile uint8_t     inDataBuf[];
extern volatile uint8_t     outDataBuf[];
extern volatile uint16_t    inBufPosition;
extern volatile uint16_t    outDataPutPosition;

void debug_putc(char c);
void debug_puts(const char *s);
void commands_init(void);
void ProcessCommand(void);
void print_to_pc(const char* msg);
bool sd_ready(void);
void sd_invalidate(void);
void loadWatchdogArm(void);
void loadWatchdogDisarm(void);
void loadWatchdogService(void);

#endif