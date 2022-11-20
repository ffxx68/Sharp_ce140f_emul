////////////////////////////////////////////////////////
// Sharp CE-140F diskette emulator
// 
// Reverse engineering by contact@pockemul.com
// v1.0  1/6/2022 - adaptation to MBed NUCLEO L053R8
// v1.1  16/11/2022 - adaptation to MBed NUCLEO L432KC
//
////////////////////////////////////////////////////////
#include "mbed.h"
#include "commands.h"

#define DEBUG 1
#define DEBUG_SIZE 10000 // debug buffer
#define NIBBLE_DELAY_1 100 // us
#define NIBBLE_ACK_DELAY 100 // us
#define BIT_DELAY_1 200 // us
#define BIT_DELAY_2 2000 // us
#define ACK_DELAY 20000 // us
#define ACK_TIMEOUT 1 // s
#define DATA_WAIT 9000 // us
#define IN_DATAREADY_TIMEOUT 50000 // us
#define OUT_NIBBLE_DELAY 1000 // us

#if defined TARGET_NUCLEO_L053R8
// input ports
DigitalIn   in_BUSY     (PC_0);    
InterruptIn irq_BUSY    (PC_0);    
DigitalIn   in_D_OUT    (PC_1);    
InterruptIn irq_D_OUT   (PC_1);
DigitalIn   in_X_OUT    (D6);     
InterruptIn irq_X_OUT   (D6);
DigitalIn   in_D_IN     (D7);  
DigitalIn   in_SEL_1    (D9);
DigitalIn   in_SEL_2    (D8); 
// output ports  
DigitalOut        out_ACK     (D10);  
DigitalOut        out_D_OUT   (D11);     
DigitalOut        out_D_IN    (D12);     
DigitalOut        out_SEL_1   (D15);     
DigitalOut        out_SEL_2   (D14);     
// info led
DigitalOut        infoLed    (LED1); // D13
InterruptIn       user_BTN   (USER_BUTTON);
#endif

#if defined TARGET_NUCLEO_L432KC
// input ports
DigitalIn         in_BUSY     (PA_8);    
InterruptIn       irq_BUSY    (PA_8);    
DigitalIn         in_D_OUT    (PA_11);    
InterruptIn       irq_D_OUT   (PA_11);
DigitalIn         in_X_OUT    (PA_0);     
InterruptIn       irq_X_OUT   (PA_0);
DigitalIn         in_D_IN     (PA_1);  
DigitalIn         in_SEL_1    (PA_12);
DigitalIn         in_SEL_2    (PB_0); 
// output ports  
DigitalOut        out_ACK     (PB_7);  
DigitalOut        out_D_OUT   (PB_6); 
DigitalOut        out_D_IN    (PB_1);     
DigitalOut        out_SEL_1   (PA_9);   
DigitalOut        out_SEL_2   (PA_10);     
// others
DigitalOut        infoLed     (LED1);
InterruptIn       user_BTN    (PB_4);
#endif

// timers
Timer             mainTimer;
Timeout           debugOutTimeout;
Timeout           ackOffTimeout;
Timeout           inDataReadyTimeout;
Timer             testTimer;
#ifdef ASYNCHOUT
Ticker            outDataTicker;       
#endif     

// PC comms
RawSerial         pc(USBTX, USBRX); // D0, D1 ?

volatile uint8_t  deviceCode;
volatile uint8_t  bitCount;
volatile uint8_t  debugBuf[DEBUG_SIZE];
volatile bool     highNibbleIn = false;
volatile bool     highNibbleOut = false;
volatile uint8_t  dataInByte;
volatile uint8_t  dataOutByte;
volatile uint16_t outDataGetPosition;
volatile uint8_t  checksum;
volatile uint16_t debugPush = 0 ;
volatile uint16_t debugPull = 0 ;

extern volatile bool cmdComplete;

// prototypes
void startDeviceCodeSeq ( void );

// code
#ifdef DEBUG
void debug_log(const char *fmt, ...)
{
    uint8_t     debugLine[80];
    va_list va;
    va_start (va, fmt);
    sprintf((char*)debugLine,"%d ",mainTimer.read_us());
    strcat((char*)debugBuf,(char*)debugLine);
    vsprintf ((char*)debugLine, fmt, va);
    strcat((char*)debugBuf,(char*)debugLine);
    va_end (va);
}
void debug_hex(volatile uint8_t *buf, volatile uint16_t len)
{
    int j;
    char tmp[15];
    sprintf(tmp,"%d <",mainTimer.read_us(), len);
    strcat((char*)debugBuf, tmp);
    for (j=0;j<len;j++) {
        sprintf(tmp, "%02X", (char)buf[j]);
        strcat((char*)debugBuf, tmp);
    }
    sprintf(tmp,">\n");
    strcat((char*)debugBuf, tmp);
}
#else
void debug_log(const uint8_t *fmt, ...)
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
    // watchdog on ack line high (might lock the Sharp-PC)
    ackOffTimeout.attach( &ResetACK, ACK_TIMEOUT ); 
}


void resetDebug ( void) {
  inBufPosition = 0;
  debugBuf[inBufPosition] = 0x00;
}

void debugOutput( void ){
    uint8_t i = 20;
    while (i--) { infoLed =! infoLed; wait_ms(20); }
    // printout debug buffer
    pc.printf ( "%s", (char*)debugBuf );
    // reset status
    ResetACK();
    irq_BUSY.rise(NULL);
    irq_BUSY.fall(NULL);
    sprintf ( (char*)debugBuf, "ok\n" ); 
}

#ifdef ASYNCHOUT
void sendNibble ( void )
{
    uint8_t t;
    uint32_t nTimeout;
    uint32_t t1, t2, t3;

    testTimer.reset();
    // wait for BUSY to go DOWN
    nTimeout = 5000; // max wait
    while ( in_BUSY && (nTimeout--) )
        wait_us (100);
    if ( !nTimeout ) {
        pc.putc('x');
        debug_log ( "sendNibble ERR 1\n" );
        ResetACK();
        return;
    };
    t1 = testTimer.read_us();
    if ( highNibbleOut ) {
        highNibbleOut = false;
        t = (dataOutByte >> 4);
        outDataGetPosition++; // done - next byte
    } else {
        highNibbleOut = true;
        dataOutByte = outDataBuf[outDataGetPosition];
        //debug_log ("%d: 0x%02X\n", outDataGetPosition, dataOutByte);
        t = (dataOutByte & 0x0F);
    }
    out_SEL_1 = (t&0x01);
    out_SEL_2 = ((t&0x02)>>1);
    out_D_OUT = ((t&0x04)>>2);
    out_D_IN  = ((t&0x08)>>3);

    // nibble ready for PC to process
    SetACK();
    t2 = testTimer.read_us();
    // then wait for busy to go UP 
    nTimeout = 5000; // max wait
    while ( !in_BUSY && (nTimeout--) )
        wait_us (100);
    if (!nTimeout) {
        pc.putc('x');
        debug_log ( "sendNibble ERR 2\n" );
        ResetACK();
        return;
    };
    // data successfully received by PC
    // acknowledge before next nibble
    ResetACK();
    t3 = testTimer.read_us();
    //debug_log ("%d: 0x%02X %d %d %d\n", outDataGetPosition, dataOutByte, t1, t2, t3);
}

void outDataEnd ( void ) {
    // Stop the spooler ticking
    outDataTicker.detach();
    // set lines for INPUT mode
    in_D_OUT.mode(PullDown);
    in_D_IN.mode(PullDown);
    in_SEL_2.mode(PullDown);
    in_SEL_1.mode(PullDown); 
    out_D_OUT = 0;
    out_D_IN  = 0;     
    out_SEL_2 = 0;     
    out_SEL_1 = 0;
    ResetACK();
}

// send a 4-bit nibble and pull a byte from the output buffer
// Called by a ticker at OUT_NIBBLE_DELAY interval
void outDataSpooler ( void ) {

    if ( outDataGetPosition < outDataPutPosition )
        // something to send
        sendNibble( );
    
    if (   !highNibbleOut // byte complete
        && outDataGetPosition == outDataPutPosition // data stream end reached
        && outDataGetPosition > 0 ) {
        pc.putc('p');
        if ( !cmdComplete ) {
            // might have reached buffer end, but stream isn't complete yet
            // wait for the feeder to reset buffer position 
            pc.putc('b');
            uint32_t nTimeout = 5000; // max wait
            while ( outDataGetPosition > 0 && (nTimeout--) )
                wait_us (100);
            if (!nTimeout) {
                outDataEnd();
                pc.putc('x');
                debug_log ( "outDataSpooler ERR 1\n" );
            }
        } else {
            // data stream complete
            pc.putc('t');
            outDataEnd();
            // last wait for BUSY to go DOWN
            uint32_t nTimeout = 5000; // max wait
            while ( in_BUSY && (nTimeout--) )
                wait_us (100);
            if (!nTimeout) {
                pc.putc('x');
                debug_log ( "outDataSpooler ERR 2\n" );
            }
            debug_log ( "send complete\n" );
        } 
    } 
}
#else
// Synchronous output data sender 
void SendOutputData ( void ) {
    uint8_t t;
    uint32_t nTimeout;
    float avg_byte_time;
    
    pc.putc('o');
    // set for OUTPUT mode
    in_D_OUT.mode(PullNone);
    in_D_IN.mode(PullNone);
    in_SEL_2.mode(PullNone);
    in_SEL_1.mode(PullNone);  
    testTimer.reset(); 
    testTimer.start(); 
    avg_byte_time = 0;
    //pc.printf("outDataGetPosition %d outDataPutPosition %d ", outDataGetPosition, outDataPutPosition);
    while ( outDataGetPosition < outDataPutPosition ) { // outDataPointer < outBufPosition
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

        wait_us (OUT_NIBBLE_DELAY); // ??? really ???

        if ( highNibbleOut ) {
            highNibbleOut = false;
            t = (dataOutByte >> 4);
            outDataGetPosition++;
        } else {
            highNibbleOut = true;
            dataOutByte = outDataBuf[outDataGetPosition];
            //pc.putc('.');
            avg_byte_time = avg_byte_time + testTimer.read_us();
            testTimer.reset(); 
            //debug_log (" %02X\n", dataOutByte);
            t = (dataOutByte & 0x0F);
        }
        out_SEL_1 = (t&0x01);
        out_SEL_2 = ((t&0x02)>>1);
        out_D_OUT = ((t&0x04)>>2);
        out_D_IN  = ((t&0x08)>>3);
        
        // nibble ready for Sharp-PC to process
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
    debug_log ( "average byte timing (us): %f\n",avg_byte_time/outDataPutPosition); 
    wait_us ( IN_DATAREADY_TIMEOUT );
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
#endif

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
        if ( checksum == inDataBuf[inBufPosition-1] ) {
            pc.printf(" 0x%02X\n", inDataBuf[0]);
            debug_log ( "command 0x%02X\n" , inDataBuf[0]); 
            outDataGetPosition = 0;
            outDataPutPosition = 0;
            highNibbleOut = false;
            // stop the BUSY triggers
            irq_BUSY.fall(NULL);
            irq_BUSY.rise(NULL);
            // decode and process command - feeding the output buffer
            ProcessCommand ();  
            inBufPosition = 0;
#ifdef ASYNCHOUT
            // set lines for OUTPUT
            in_D_OUT.mode(PullNone);
            in_D_IN.mode(PullNone);
            in_SEL_2.mode(PullNone);
            in_SEL_1.mode(PullNone);  
            testTimer.start(); 
            outDataTicker.attach_us ( outDataSpooler, OUT_NIBBLE_DELAY );
#else
            if ( outDataPutPosition > 0 ) {
                // data ready for sending 
                debug_log ( "dataout: %u bytes\n" , outDataPutPosition);
                // Take control and send back processed data
                SendOutputData(); 
            } else {
                pc.putc('x');
                debug_log ( "Command processing error\n"); 
                // should send something back in case of error too?
            }
#endif
        } else {
            pc.putc('x');
            debug_log ( "checksum error\n"); 
            // should send something back in case of error too?
        }
    }
    
}

void inNibbleReady ( void ) {
    // test lines
    uint8_t inNibble = ( in_SEL_1 + (in_SEL_2<<1) + (in_D_OUT<<2) + (in_D_IN<<3) );
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
    //pc.putc('b'); // debug 
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
                nTimeout = 10000; // timeout: 1s
                while ( ( in_X_OUT || in_BUSY ) && (nTimeout--) ) {
                    wait_us (100);
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
    pc.putc('\n');pc.putc('s'); // debug 
    if ( in_D_OUT == 1 ) {
        // Device Code transfer starts with both X_OUT and DOUT high
        // (X_OUT high with DOUT low is for cassette write)
        SetACK();
        bitCount = 0;
        deviceCode = 0;
        resetDebug (); 
        debug_log ("\nDevice\n");
        wait_us (ACK_DELAY) ;   //?? or, wait only AFTER enabling trigger ??
        // serial bit trigger
        irq_BUSY.rise(&bitReady);
        irq_BUSY.fall(NULL);
        wait_us (ACK_DELAY) ;
    }
}

int main(void) {
  uint8_t i = 20;

  pc.baud(9600);
  pc.printf("CE140F emulator init\n");
  while (i--) {
    infoLed = !infoLed;
    wait_ms(20);
  }

  resetDebug();
  user_BTN.rise(&debugOutput);

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
  out_D_IN = 0;
  out_SEL_2 = 0;
  out_SEL_1 = 0;

  // initial triggers (device sequence handshake)
  irq_X_OUT.rise(&startDeviceCodeSeq);
  pc.printf("ready\n");
  debug_log("ready\n");
  mainTimer.reset();
  mainTimer.start();

  while (1) {
    wait(1); // logic handled with interrupts and timers
  }
}