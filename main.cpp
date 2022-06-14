////////////////////////////////////////////////////////
// Sharp CE-140F diskette emulator
//
// v1 1/6/2022 - adaptation to MBed NUCLEO 
// of the version received by contact@pockemul.com
//
////////////////////////////////////////////////////////
#include "mbed.h"

#define BUF_SIZE 128 // data depth
#define DEBUG_SIZE 2048 // print debug buffer
#define NIBBLE_DELAY_1 100 // us
#define NIBBLE_ACK_DELAY 100 // us
#define BIT_DELAY_1 200 // us
#define BIT_DELAY_2 2000 // us
#define ACK_DELAY 20000 // us
#define ACK_TIMEOUT 1 // s
#define DATA_WAIT 9000 // us
#define IN_DATAREADY_TIMEOUT 1 // s - how long ??

// input ports
DigitalIn   in_BUSY     (D4);    
InterruptIn irq_BUSY    (D4);    
DigitalIn   in_D_OUT    (D5);    
InterruptIn irq_D_OUT   (D5);
DigitalIn   in_X_OUT    (D7);     
InterruptIn irq_X_OUT   (D7);
DigitalIn   in_D_IN     (D8);  
DigitalIn   in_SEL_2    (D10);     
DigitalIn   in_SEL_1    (D11);

// output ports  
DigitalOut        out_ACK     (D9);     
DigitalOut        out_X_OUT   (D12);     
DigitalOut        out_D_IN    (D13);     
DigitalOut        out_SEL_2   (D14);     
DigitalOut        out_SEL_1   (D15);     

// info led
DigitalOut        infoLed   (LED1);

// timers
Timer             mainTimer;
Timeout           ackOffTimeout;
Timeout           inDataReadyTimeout;
Ticker            outDataTicker;


// PC comms
RawSerial         pc(USBTX, USBRX);
InterruptIn       btn(USER_BUTTON);

volatile uint8_t  deviceCode;
volatile uint8_t  bitCount;
volatile char     debugBuf[DEBUG_SIZE];
volatile char     debugLine[80];
volatile bool     highNibbleIn = false;
volatile uint8_t  dataInByte;
volatile char     inDataBuf[BUF_SIZE];
volatile char     outDataBuf[BUF_SIZE];
volatile uint16_t inBufPosition;
volatile uint16_t outBufPosition;
volatile uint8_t  checksum = 0;

void debug_log(const char *fmt, ...)
{
    va_list va;
    va_start (va, fmt);
    sprintf((char*)debugLine,"%d ",mainTimer.read_us());
    strcat((char*)debugBuf,(char*)debugLine);
    vsprintf ((char*)debugLine, fmt, va);
    strcat((char*)debugBuf,(char*)debugLine);
    strcat((char*)debugBuf,"\n\r");
    va_end (va);
}

// a "timeout" on ACK line 
void  ackOff ( void ) {
    
    out_ACK = 0; 
    infoLed = 0;
     
}

char CheckSum(char b) {
    checksum = (checksum + b) & 0xff;
    return b;
}

void outDataAppend(char b) {
    outDataBuf[(outBufPosition++)%BUF_SIZE ] = b;
}

void sendString(char* s) {
    for (int i=0;i<strlen(s);i++){
        outDataAppend(CheckSum(s[i]));
    }
}

void inDataReady ( void ) {
    
    pc.putc('c');
    // Command processing here ...
    sprintf ( (char*)debugLine, "\n\r%d Processing...\n\r", mainTimer.read_us()) ; strcat ((char*)debugBuf, (char*)debugLine) ;
    if ( inBufPosition > 0 ) {
        sprintf ( (char*)debugLine, "%d inBufPosition %d...\n\r", mainTimer.read_us(), inBufPosition) ; strcat ((char*)debugBuf, (char*)debugLine) ;

        // Verify checksum
        checksum=0;
        for (int i=0;i<inBufPosition-1;i++) {
            checksum = (inDataBuf[i]+checksum) & 0xff;
        }   
        sprintf ( (char*)debugLine, "%d checksum 0x%02X vs 0x%02X\n\r" ,  mainTimer.read_us(), checksum, inDataBuf[inBufPosition-1]); strcat ((char*)debugBuf, (char*)debugLine) ;
        if ( checksum == inDataBuf[inBufPosition-1]) {
            // processing command
            pc.printf(" 0x%02X\n\r", inDataBuf[0]);
            sprintf ( (char*)debugLine, "%d command 0x%02X\n\r" , mainTimer.read_us(), inDataBuf[0]); strcat ((char*)debugBuf, (char*)debugLine) ;
            checksum=0;
            outBufPosition = 0;
            
            if ( inDataBuf[0] == 0x05 ) {
                // case 0x07: process_FILES_LIST(1);break;
                outDataAppend(0x00);
                sendString("X:TEST    .BAS ");        
                outDataAppend(checksum);
                //outDataAppend(0x00); // DEBUG ONLY
                debug_log ( "dataout %u [%02X]" , outBufPosition, checksum); 
                //debug_log ( "<%s>", outDataBuf+1);// DEBUG ONLY
            }
        } else
        {
            pc.putc('x');
            debug_log ( "checksum error"); 
        }
    }
}

void nibbleReady ( void ) {
    // test lines
    char inNibble = ( in_SEL_1 + (in_SEL_2<<1) + (in_D_OUT<<2) + (in_D_IN<<3) );
    //debug_log ( "(%d) %01X ", highNibbleIn, inNibble ) ; 
    if ( out_ACK == 0 ) {
        wait_us ( NIBBLE_DELAY_1 );
        out_ACK = 1;
        infoLed = 1;
        ackOffTimeout.attach( &ackOff, ACK_TIMEOUT ); // max time high ack
        if ( highNibbleIn ) {
            highNibbleIn = false;
            inDataBuf[inBufPosition] = (inNibble << 4) + inDataBuf[inBufPosition];
            checksum = (inDataBuf[inBufPosition] + checksum) & 0xff;
            sprintf ( (char*)debugLine, " %u:%02X [%02X]", inBufPosition, inDataBuf[inBufPosition], checksum ) ; strcat ((char*)debugBuf, (char*)debugLine) ;
            inBufPosition++; // = (inBufPosition+1)%BUF_SIZE; // circular for safety; may cut off data!
            // Data processing starts after last byte (timeout reset at each byte received) 
            inDataReadyTimeout.attach( &inDataReady, IN_DATAREADY_TIMEOUT );
        } else {
            highNibbleIn = true;
            inDataBuf[inBufPosition] = inNibble;
            //debug_log ( " %01X", inDataBuf[inBufPosition] ) ; 
        }
    } else {
        pc.putc('x');
        debug_log ( "nibbleReady out_ACK!=0\n\r" ) ;
    }
}

void nibbleAck ( void ) {
    // test lines
    //debug_log ( "ack (%01X)\n\r", ( in_SEL_1 + (in_SEL_2<<1) + (in_D_OUT<<2) + (in_D_IN<<3) )) ; 
    if ( out_ACK == 1 ) {
        wait_us ( NIBBLE_ACK_DELAY );
        out_ACK = 0;
        infoLed = 0;
    } else {
        pc.putc('x');
        debug_log ( "nibbleAck out_ACK!=1\n\r" ); 
    }
}

// Serial bit receive
void bitReady ( void ) {
    uint32_t nTimeout;
    
    if ( out_ACK == 1 ) {
        bool bit;
        wait_us ( BIT_DELAY_1 );
        bit = in_D_OUT; // get bit value
        //pc.putc(0x30+bit);pc.putc(' ');
        //sprintf ( (char*)debugLine, " bit %d: %d\n\r",  bitCount, bit ); 
        out_ACK = 0; // bit has been received
        infoLed = 0;
        deviceCode>>=1;
        if (bit) deviceCode|=0x80;
        if ((bitCount=(++bitCount)&7)==0) {
            // 8 bits received
            irq_BUSY.rise(NULL); // detach this IRQ
            pc.printf("d 0x%02X\n\r",deviceCode);
            debug_log ( "Device ID 0x%02X", deviceCode ); 
            if ( deviceCode == 0x41 ) {
                // Sharp-PC is looking for a CE140F (device code 0x41) - Here we are!
                sprintf ( (char*)debugLine, "%d CE140F\n\r", mainTimer.read_us()); strcat ((char*)debugBuf, (char*)debugLine) ;
                inBufPosition = 0;
                highNibbleIn = false;
                checksum = 0;
                // set data handshake triggers on the BUSY input
                irq_BUSY.fall(&nibbleAck);
                irq_BUSY.rise(&nibbleReady);
                // check for both BUSY and X_OUT to go down, before starting data receive
                nTimeout = 100000; // timeout: 1s
                while ( ( in_X_OUT || in_BUSY ) && (nTimeout--) ) {
                    wait_us (10);
                }
                if (nTimeout) {
                    // ... start data handling ...
                    out_ACK = 1;
                    infoLed = 1;
                    wait_us ( DATA_WAIT );
                    out_ACK = 0;
                    infoLed = 0;
                } else {
                    pc.putc('x');
                    debug_log ("Timeout!\n\r") ;
                }

            } 
        } else {
            wait_us ( BIT_DELAY_2 );
            out_ACK = 1; // ready for a new bit
            infoLed = 1;
            ackOffTimeout.attach( &ackOff, ACK_TIMEOUT ); // max time high ack
        }
    }
}

void startDeviceCodeSeq ( void ) {
    
    wait_us (BIT_DELAY_1);
    if ( in_D_OUT == 1 ) {
        // Device Code transfer starts with both X_OUT and DOUT high
        // (X_OUT high with DOUT low is for cassette write)
        
        infoLed = 1;
        out_ACK = 1;
        ackOffTimeout.attach( &ackOff, ACK_TIMEOUT ); // max time high ack
        
        bitCount = 0;
        deviceCode = 0;
        sprintf ( (char*)debugBuf, "Device\n\r" ); 

        wait_us (ACK_DELAY) ;   //?? or, wait AFTER eabling trigger ??
        // serial bit trigger
        irq_BUSY.rise(&bitReady);
        irq_BUSY.fall(NULL);
        wait_us (ACK_DELAY) ;
    
    }
}

void btnRaised() 
{
    uint8_t i = 20;
    while (i--) { infoLed =! infoLed; wait_ms(20); }
    // printout debug buffer
    pc.printf ( "%s", (char*)debugBuf );
    // reset status
    out_ACK = 0;
    infoLed = 0;
    irq_BUSY.rise(NULL);
    irq_BUSY.fall(NULL);
    sprintf ( (char*)debugBuf, "Reset\n\r" ); 
}

int main(void) {
    uint8_t i = 20;

    pc.baud ( 57600 ); 
    pc.printf ( "CE140F emulator init\n\r" );
    while (i--) { infoLed =! infoLed; wait_ms(20); }
    
    btn.rise(&btnRaised);
    inBufPosition = 0;
    
    // default input pull-down
    in_BUSY.mode(PullDown);
    in_D_OUT.mode(PullDown);
    in_X_OUT.mode(PullDown);
    in_D_IN.mode(PullDown);
    in_SEL_2.mode(PullDown);
    in_SEL_1.mode(PullDown);    
    
    // default outputs
    out_ACK = 0;
    out_X_OUT = 0;
    out_D_IN  = 0;     
    out_SEL_2 = 0;     
    out_SEL_1 = 0;

    // initial triggers (device sequence handshake)
    irq_X_OUT.rise(&startDeviceCodeSeq);
    
    mainTimer.reset();
    mainTimer.start();
    //sprintf ( (char*)debugBuf, "Start\n\r" );
    
    while (1) {
        wait (1); // logic handled with interrupts and timers
    }
}