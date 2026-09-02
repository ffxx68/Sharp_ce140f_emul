#ifndef COMMANDS_H
#define COMMANDS_H

#include "main.h"

#define OUT_BUF_SIZE 8192
#define IN_BUF_SIZE 1024
#define MAX_N_FILES 6

#define ERR_PRINTOUT(x) debug_log("ERR %s", x); print_to_pc(x)
#define ERR_SD_CARD_NOT_PRESENT "SD Card not present!\n"
#define SD_HOME "0:/"

extern volatile uint8_t     inDataBuf[];
extern volatile uint8_t     outDataBuf[];
extern volatile uint16_t    inBufPosition;
extern volatile uint16_t    outDataPutPosition;

void ProcessCommand(void);
void print_to_pc(const char* msg);

#endif