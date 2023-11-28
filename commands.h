#ifndef COMMANDS_H
#define COMMANDS_H
#include "mbed.h"

// #define ASYNCHOUT 1 // sending output data asynchronously - TO DEBUG!! 

// communication data depth (max file size during LOAD)
#if defined TARGET_NUCLEO_L432KC
#define OUT_BUF_SIZE 20000
#define IN_BUF_SIZE 1000
#endif
#if defined TARGET_NUCLEO_L053R8
#define OUT_BUF_SIZE 256
#define IN_BUF_SIZE 256
#endif

#define ERR_PRINTOUT(x) debug_log("ERR %s",x); pc.printf(x)
#define ERR_SD_CARD_NOT_PRESENT "SD Card not present!\n"
#define SD_HOME "/sd/"
#define MAX_N_FILES 6 

extern volatile uint8_t     inDataBuf[];
extern volatile uint8_t     outDataBuf[];
extern volatile uint16_t    inBufPosition;
extern volatile uint16_t    outDataPutPosition;

void ProcessCommand ( void ) ;

#endif
