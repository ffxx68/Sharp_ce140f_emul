#ifndef COMMANDS_H
#define COMMANDS_H

#include "mbed.h"

#define BUF_SIZE 256 // comms data depth

extern volatile char     inDataBuf[];
extern volatile char     outDataBuf[];
extern volatile uint16_t inBufPosition;
extern volatile uint16_t outBufPosition;
extern volatile uint8_t  checksum;

void ProcessCommand ( void ) ;

#endif