////////////////////////////////////////////////////////
// Sharp CE-140F diskette emulator
// 
// Reverse engineering by contact@pockemul.com
// v1 1/6/2022 - adaptation to MBed NUCLEO L053R8
//
////////////////////////////////////////////////////////
#include "mbed.h"
#include "commands.h"

#define DEBUG 1
#define DEBUG_SIZE 2000 // debug buffer
#define NIBBLE_DELAY_1 100 // us
#define NIBBLE_ACK_DELAY 100 // us
#define BIT_DELAY_1 200 // us
#define BIT_DELAY_2 2000 // us
#define ACK_DELAY 20000 // us
#define ACK_TIMEOUT 1 // s
#define DATA_WAIT 9000 // us
#define IN_DATAREADY_TIMEOUT 50000 // us
#define OUT_NIBBLE_DELAY 5000 // us

// input ports
DigitalIn   in_BUSY     (PC_0);    
InterruptIn irq_BUSY    (PC_0);    
DigitalIn   in_D_OUT    (PC_1);    
InterruptIn irq_D_OUT   (PC_1);
DigitalIn   in_X_OUT    (D6);     
InterruptIn irq_X_OUT   (D6);
DigitalIn   in_D_IN     (D7);  
DigitalIn   in_SEL_2    (D8); 
DigitalIn   in_SEL_1    (D9);

// output ports  
DigitalOut        out_ACK     (D10);  
DigitalOut        out_D_OUT   (D11);     
DigitalOut        out_D_IN    (D12);     
DigitalOut        out_SEL_2   (D14);     
DigitalOut        out_SEL_1   (D15);     

// info led
DigitalOut        infoLed    (LED1); // D13

// timers
Timer             mainTimer;
Timeout           ackOffTimeout;
Timeout           inDataReadyTimeout;

// PC comms
RawSerial         pc(USBTX, USBRX); // D0, D1 ?
InterruptIn       btn(USER_BUTTON);

volatile uint8_t  deviceCode;
volatile uint8_t  bitCount;
volatile char     debugBuf[DEBUG_SIZE];
volatile char     debugLine[80];
volatile bool     highNibbleIn = false;
volatile bool     highNibbleOut = false;
volatile uint8_t  dataInByte;
volatile uint8_t  dataOutByte;
volatile uint16_t outDataPointer;

// prototypes
void startDeviceCodeSeq ( void );

#ifdef DEBUG
void debug_log(const char *fmt, ...)
{
    va_list va;
    va_start (va, fmt);
    sprintf((char*)debugLine,"%d ",mainTimer.read_us());
    strcat((char*)debugBuf,(char*)debugLine);
    vsprintf ((char*)debugLine, fmt, va);
    strcat((char*)debugBuf,(char*)debugLine);
    va_end (va);
}
#else
void debug_log(const char *fmt, ...)
{
    return;
}
#endif

void  ResetACK ( void ) {
    out_ACK = 0; 
    infoLed = 0;
}

void  SetACK ( void ) {
    out_ACK = 1; 
    infoLed = 1;
    // watchdog on ack line high (it locks the Sharp-PC)
    ackOffTimeout.attach( &ResetACK, ACK_TIMEOUT ); 
}

void btnRaised() 
{
    uint8_t i = 20;
    while (i--) { infoLed =! infoLed; wait_ms(20); }
    // printout debug buffer
    pc.printf ( "%s", (char*)debugBuf );
    // reset status
    ResetACK();
    irq_BUSY.rise(NULL);
    irq_BUSY.fall(NULL);
    sprintf ( (char*)debugBuf, "Reset\n\r" ); 
}

void SendOutputData ( void ) {
     uint8_t t;
    uint32_t nTimeout;
    
    pc.putc('o');
    // set for OUTPUT mode
    in_D_OUT.mode(PullNone);
    in_D_IN.mode(PullNone);
    in_SEL_2.mode(PullNone);
    in_SEL_1.mode(PullNone);   
    while ( outDataPointer < outBufPosition ) {
        // wait for BUSY to go DOWN
        nTimeout = 50000; // max wait
        while ( in_BUSY && (nTimeout--) ) {
            wait_us (10);
        }  
        if (!nTimeout) {
            pc.putc('x');
            debug_log ( "PC error 1\n" );
            ResetACK();
            break;
        };

        wait_us (OUT_NIBBLE_DELAY);

        if ( highNibbleOut ) {
            highNibbleOut = false;
            t = (dataOutByte >> 4);
            outDataPointer++;
        } else {
            highNibbleOut = true;
            dataOutByte = outDataBuf[outDataPointer];
            debug_log ("%d: 0x%02X\n", outDataPointer, dataOutByte);
            t = (dataOutByte & 0x0F);
        }
        
        out_SEL_1 = (t&0x01);
        out_SEL_2 = ((t&0x02)>>1);
        out_D_OUT = ((t&0x04)>>2);
        out_D_IN  = ((t&0x08)>>3);
        
        // nibble ready for PC to process
        SetACK();
        
        
        // then wait for busy to go UP 
        nTimeout = 50000; // max wait
        while ( !in_BUSY && (nTimeout--) ) {
            wait_us (10);
        }
        if (!nTimeout) {
            pc.putc('x');
            debug_log ( "PC error 2\n" );
            ResetACK();
            break;
        };
        // data successfully received by PC
        // acknowledge before next nibble
        ResetACK();
    } 
    debug_log ( "send complete\n" );
    wait ( IN_DATAREADY_TIMEOUT );

    // set for INPUT mode
    in_D_OUT.mode(PullDown);
    in_D_IN.mode(PullDown);
    in_SEL_2.mode(PullDown);
    in_SEL_1.mode(PullDown); 
    out_D_OUT = 0;
    out_D_IN  = 0;     
    out_SEL_2 = 0;     
    out_SEL_1 = 0;
    
}

void inDataReady ( void ) {
    pc.putc('c');
    // Command processing starts here ...
    debug_log ( "Processing...\n" ) ;
    if ( inBufPosition > 0 ) {
        debug_log ( "inBufPosition %d\n", inBufPosition) ; 
        // Verify checksum
        checksum=0;
        for (int i=0;i<inBufPosition-1;i++) {
            checksum = (inDataBuf[i]+checksum) & 0xff;
        }   
        debug_log ( "checksum 0x%02X vs 0x%02X\n" ,  checksum, inDataBuf[inBufPosition-1]); 
        if ( checksum == inDataBuf[inBufPosition-1]) {
            pc.printf(" 0x%02X\n\r", inDataBuf[0]);
            debug_log ( "command 0x%02X\n" , inDataBuf[0]); 
            // outDataTicker.detach();   // safety ?       
            // decode and process command
            ProcessCommand ();  
            inBufPosition = 0;
            if ( outBufPosition > 0 ) {
                // data ready for sending 
                debug_log ( "dataout %u [%02X]\n" , outBufPosition, checksum); 
                outDataPointer = 0;
                highNibbleOut = false;
                // stop BUSY triggers
                irq_BUSY.fall(NULL);
                irq_BUSY.rise(NULL);
                // Take control and send back processed data
                SendOutputData();
            } else {
                pc.putc('x');
                debug_log ( "data prepare error\n"); 
            }
        } else
        {
            pc.putc('x');
            debug_log ( "checksum error\n"); 
        }
    }
}

void inNibbleReady ( void ) {
    // test lines
    char inNibble = ( in_SEL_1 + (in_SEL_2<<1) + (in_D_OUT<<2) + (in_D_IN<<3) );
    //debug_log ( "(%d) %01X \n", highNibbleIn, inNibble ) ; 
    if ( out_ACK == 0 ) {
        wait_us ( NIBBLE_DELAY_1 );
        SetACK();
        if ( highNibbleIn ) {
            highNibbleIn = false;
            inDataBuf[inBufPosition] = (inNibble << 4) + inDataBuf[inBufPosition];
            checksum = (inDataBuf[inBufPosition] + checksum) & 0xff;
            //debug_log(" %u:%02X [%02X]\n", inBufPosition, inDataBuf[inBufPosition], checksum ) ;
            inBufPosition++; // should be circular for safety; may cut off data!
            // Data processing starts after last byte (timeout reset after each byte received) 
            inDataReadyTimeout.attach_us( &inDataReady, IN_DATAREADY_TIMEOUT );
        } else {
            highNibbleIn = true;
            inDataBuf[inBufPosition] = inNibble;
            //debug_log ( " %01X\n", inDataBuf[inBufPosition] ) ; 
        }
    } else {
        pc.putc('x');
        debug_log ( "inNibbleReady out_ACK!=0\n" ) ;
    }
}

void inNibbleAck ( void ) {
    // test lines
    //debug_log ( "ack (%01X)\n\r", ( in_SEL_1 + (in_SEL_2<<1) + (in_D_OUT<<2) + (in_D_IN<<3) )) ; 
    if ( out_ACK == 1 ) {
        wait_us ( NIBBLE_ACK_DELAY );
        ResetACK();
    } else {
        pc.putc('x');
        debug_log ( "inNibbleAck out_ACK!=1\n" ); 
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
        ResetACK(); // bit received
        deviceCode>>=1;
        if (bit) deviceCode|=0x80;
        if ((bitCount=(++bitCount)&7)==0) {
            // 8 bits received
            irq_BUSY.rise(NULL); // detach this IRQ
            pc.printf("d 0x%02X\n",deviceCode);
            debug_log ( "Device ID 0x%02X\n", deviceCode ); 
            if ( deviceCode == 0x41 ) {
                // Sharp-PC is looking for a CE140F (device code 0x41) - Here we are!
                debug_log ( "CE140F\n" ) ;
                inBufPosition = 0;
                highNibbleIn = false;
                checksum = 0;
                // set data handshake triggers on the BUSY line
                irq_BUSY.fall(&inNibbleAck);
                irq_BUSY.rise(&inNibbleReady);
                // check for both BUSY and X_OUT to go down, before starting data receive
                nTimeout = 100000; // timeout: 1s
                while ( ( in_X_OUT || in_BUSY ) && (nTimeout--) ) {
                    wait_us (10);
                }
                if (nTimeout) {
                    // trigger data handling
                    SetACK();
                    wait_us ( DATA_WAIT );
                    ResetACK();
                } else {
                    pc.putc('x');
                    debug_log ("Timeout!\n\r") ;
                }
            } 
        } else {
            wait_us ( BIT_DELAY_2 );
            SetACK();
        }
    }
}

void startDeviceCodeSeq ( void ) {
    wait_us (BIT_DELAY_1);
    pc.putc('s');
    if ( in_D_OUT == 1 ) {
        // Device Code transfer starts with both X_OUT and DOUT high
        // (X_OUT high with DOUT low is for cassette write)
        SetACK();
        bitCount = 0;
        deviceCode = 0;
        sprintf ( (char*)debugBuf, "Device\n" ); 
        wait_us (ACK_DELAY) ;   //?? or, wait AFTER eabling trigger ??
        // serial bit trigger
        irq_BUSY.rise(&bitReady);
        irq_BUSY.fall(NULL);
        wait_us (ACK_DELAY) ;
    }
}

int main(void) {
    uint8_t i = 20;

    pc.baud ( 9600 ); 
    pc.printf ( "CE140F emulator init\n" );
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
    out_D_OUT = 0;
    out_D_IN  = 0;     
    out_SEL_2 = 0;     
    out_SEL_1 = 0;

    // initial triggers (device sequence handshake)
    irq_X_OUT.rise(&startDeviceCodeSeq);
    
    mainTimer.reset();
    mainTimer.start();
    
    while (1) {
        wait (1); // logic handled with interrupts and timers
    }
}