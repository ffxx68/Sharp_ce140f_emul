#include "mbed.h"

#define BUF_SIZE 1024 // data depth
#define DEBUG_SIZE 1024 // print debug buffer
#define NIBBLE_DELAY_1 20 // us
#define NIBBLE_DELAY_2 20 // us
#define BIT_DELAY_1 100 // us
#define BIT_DELAY_2 2000 // us
#define ACK_DELAY 20000 // us
#define ACK_TIMEOUT 1 // s
#define DATA_WAIT 9000 // us
#define IN_DATAREADY_TIMEOUT 50000 // ms

// input ports
DigitalIn   in_BUSY   (D4);    
InterruptIn irq_BUSY  (D4);    
DigitalIn   in_D_OUT  (D5);    
InterruptIn irq_DOUT  (D5);
DigitalIn   in_X_OUT  (D7);     
InterruptIn irq_X_OUT (D7);
DigitalIn   in_D_IN   (D8);  
DigitalIn   in_SEL_2   (D10);     
DigitalIn   in_SEL_1   (D11);
// output ports  
DigitalOut        out_ACK   (D9);     
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
volatile bool     highNibble = false;
volatile char     inDataBuf[BUF_SIZE];
volatile char     outDataBuf[BUF_SIZE];
volatile uint16_t inBufPosition;
volatile uint16_t outBufPosition;
volatile int checksum = 0;

void debug_log(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    sprintf((char*)debugLine,format,args);
    sprintf((char*)debugLine,"%d %s\n\r",mainTimer.read_us(),(char*)debugLine);
    strcat((char*)debugBuf,(char*)debugLine);
    va_end(args);
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
    // Command processing here ...
    sprintf ( (char*)debugLine, "%d Processing...\n\r", mainTimer.read_us()) ; strcat ((char*)debugBuf, (char*)debugLine) ;
    if (  inBufPosition > 0 ) {
        sprintf ( (char*)debugLine, "%d inBufPosition %d...\n\r", mainTimer.read_us(), inBufPosition) ; strcat ((char*)debugBuf, (char*)debugLine) ;

        // Verify checksum
        checksum=0;
        for (int i =0;i<inBufPosition;i++) {
            checksum = (inDataBuf[i]+checksum) & 0xff;
        }   
        debug_log ( "checksum %d vs %d" , checksum, inDataBuf[inBufPosition]); 
        
        // ....
        debug_log ( "command 0x%02X" , inDataBuf[0]); 
        checksum=0;
        outBufPosition = 0;
        
        if ( inDataBuf[0] == 0x07 ) {
            // case 0x07: process_FILES_LIST(1);break;
            outDataAppend(0x00);
            sendString("X:TEST    .BAS ");        
            outDataAppend(checksum);
            debug_log ( "dataout %d" , outBufPosition); 
        }
    }
}

void nibbleReady ( void ) {
    
    //pc.putc('n');
    if ( out_ACK == 0 ) {
        uint8_t dataByte;
        wait_us ( NIBBLE_DELAY_1 );
        if ( highNibble ) {
            highNibble = false;
            dataByte <<= 4;
            dataByte |= in_SEL_1 + (in_SEL_2<<1) + (in_D_OUT<<2) + (in_D_IN<<3);
            //pc.putc(dataByte);
            inDataBuf [ (inBufPosition++)%BUF_SIZE ] = dataByte ; // circular storage for safety; may cut off data!
            // Data processing starts after last byte (timeout reset at each byte received) 
            inDataReadyTimeout.attach( &inDataReady, IN_DATAREADY_TIMEOUT );
        } else {
            dataByte = in_SEL_1 + (in_SEL_1<<1) + (in_D_OUT<<2) + (in_D_IN<<3);
            //pc.putc(dataByte);
            highNibble = true;
        }
        sprintf ( (char*)debugLine, "%d nibble %d 0x%02X\n\r", mainTimer.read_us(), highNibble, dataByte ); strcat ( (char*)debugBuf, (char*)debugLine) ;
        out_ACK = 1;
        infoLed = 1;
        ackOffTimeout.attach( &ackOff, ACK_TIMEOUT ); // max time high ack
    } else {
        out_ACK = 0;
    }
}

void nibbleAck ( void ) {
    if ( out_ACK == 1 ) {
        wait_us ( NIBBLE_DELAY_2 );
        out_ACK = 0;
        infoLed = 0;
    }
}


// Serial bit receive
void bitReady ( void ) {
    uint32_t nTimeout;
    
    //pc.putc('b');
    if ( out_ACK == 1 ) {
        bool bit;
        wait_us ( BIT_DELAY_1 );
        
        bit = in_D_OUT; // get bit value
        //pc.putc(0x30+bit);pc.putc(' ');
        sprintf ( (char*)debugLine, "%d bit %d: %d\n\r", mainTimer.read_us(), bitCount, bit ); strcat ( (char*)debugBuf, (char*)debugLine) ;
        out_ACK = 0; // tell PC a bit has been received
        infoLed = 0;
        
        deviceCode>>=1;
        if (bit) deviceCode|=0x80;
        if ((bitCount=(++bitCount)&7)==0) {
            // 8 bits received
            irq_BUSY.rise(NULL); // detach this IRQ
            pc.printf(" Device 0x%02X\n\r",deviceCode);
            sprintf ( (char*)debugLine, "%d device 0x%02X\n\r", mainTimer.read_us(), deviceCode ); strcat ((char*)debugBuf, (char*)debugLine) ;
            if ( deviceCode == 0x41 ) {
                // PC is asking for a CE140F (device code SHOULD be 0x41) - Here we are!
                sprintf ( (char*)debugLine, "%d CE140F\n\r", mainTimer.read_us()); strcat ((char*)debugBuf, (char*)debugLine) ;
                // ... start data handling ...
                highNibble = false;
                // wait for both BUSY and X_OUT to go down, before starting data receive
                nTimeout = 10000; // timeout: 1s
                while ( ( in_X_OUT || in_BUSY ) && (nTimeout--) ) {
                    wait_us (100);
                }
                if (nTimeout) {
                    sprintf ( (char*)debugLine, "%d Data...\n\r", mainTimer.read_us()) ; strcat ((char*)debugBuf, (char*)debugLine) ;
                    out_ACK = 1;
                    infoLed = 1;
                    wait_us ( DATA_WAIT ); 
                    highNibble = false;             
                    out_ACK = 0; // ready for receiving new data
                    infoLed = 0;
                    inBufPosition = 0;
                    irq_BUSY.rise(&nibbleReady); 
                    irq_BUSY.fall(&nibbleAck);
                } else {
                    pc.putc('x');
                    sprintf ( (char*)debugLine, "%d Timeout!\n\r", mainTimer.read_us()) ; strcat ((char*)debugBuf, (char*)debugLine) ;
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

        wait_us (ACK_DELAY) ;   //?? wait after eabling trigger?
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
    in_BUSY.mode(PullNone);
    in_D_OUT.mode(PullNone);
    in_X_OUT.mode(PullNone);
    in_D_IN.mode(PullNone);
    in_SEL_2.mode(PullNone);
    in_SEL_1.mode(PullNone);    
    
    // initial triggers
    irq_X_OUT.rise(&startDeviceCodeSeq);
    
    mainTimer.reset();
    mainTimer.start();
    //sprintf ( (char*)debugBuf, "Start\n\r" );
    
    while (1) {
        wait (1); // logic handled with interrupts and timers
    }
}