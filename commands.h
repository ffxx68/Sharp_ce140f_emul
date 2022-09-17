#ifndef COMMANDS_H
#define COMMANDS_H

#include "mbed.h"

#define BUF_SIZE 256 // comms data depth
#define SD_HOME "/sd/"

extern volatile char     inDataBuf[];
extern volatile char     outDataBuf[];
extern volatile uint16_t inBufPosition;
extern volatile uint16_t outBufPosition;

void ProcessCommand ( void ) ;

#endif
