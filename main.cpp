// Sharp CE-140F diskette emulator
// 
// Reverse engineering by contact@pockemul.com
// v1.0  1/6/2022 - adaptation to MBed NUCLEO L053R8
// v1.1  21/11/2022 - adaptation to MBed NUCLEO L432KC
//
////////////////////////////////////////////////////////
#include "mbed.h"
#include "commands.h"

#define DEBUG 1

#if defined TARGET_NUCLEO_L432KC
//#define BREADBOARD // prototype
#define PCB_V1 // PCB make
#define DEBUG_SIZE 10000 // debug buffer
#define DEBUG_TIMEOUT 3000 // ms

#endif
#if defined TARGET_NUCLEO_L053R8
#define DEBUG_SIZE 1000 // debug buffer
#define DEBUG_TIMEOUT 2000 // ms
#endif
// about DEBUG_TIMEOUT:
// should be fast enough to keep buffer empty,
// but slow enough to allow for serial printf to complete
// (1000 chars @ 9600 bps => about 0.9 s)
// Possibily, increase serial speed...

#define NIBBLE_DELAY_1 1000 // us
#define NIBBLE_DELAY_2 1000 // us
#define NIBBLE_ACK_DELAY 100 // us
#define BIT_DELAY_1 1000 // us
#define BIT_DELAY_2 2000 // us
#define ACK_DELAY 20000 // us
#define ACK_TIMEOUT 1 // s
#define DATA_WAIT 9000 // us
#define IN_DATAREADY_TIMEOUT 50000 // us
#define OUT_NIBBLE_DELAY 500 // us

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
#if defined BREADBOARD
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
#if defined PCB_V1
// input ports
DigitalIn         in_BUSY     (PA_9);    
InterruptIn       irq_BUSY    (PA_9);    
DigitalIn         in_D_OUT    (PA_10);    
InterruptIn       irq_D_OUT   (PA_10);
DigitalIn         in_SEL_2    (PB_6); 
DigitalIn         in_SEL_1    (PB_1);
DigitalIn         in_D_IN     (PA_1); 
DigitalIn         in_X_OUT    (PA_0);     
InterruptIn       irq_X_OUT   (PA_0);
// output ports  
DigitalOut        out_SEL_2   (PA_8); 
DigitalOut        out_SEL_1   (PA_11);   
DigitalOut        out_ACK     (PB_7);  
DigitalOut        out_D_OUT   (PA_12); 
DigitalOut        out_D_IN    (PB_0);     
// others
DigitalOut        infoLed     (LED1);
InterruptIn       user_BTN    (PB_4);
#endif
#endif

// timers
Timer             mainTimer;
Timeout           ackOffTimeout;
Timeout           inDataReadyTimeout;
Timer             testTimer;
Ticker            debugOutTimeout;
#ifdef ASYNCHOUT // under development ...
Ticker            outDataTicker;       
#endif     

// PC comms
RawSerial         pc(USBTX, USBRX); // D0, D1 ?

volatile uint8_t  deviceCode;
volatile uint8_t  bitCount;
volatile bool     highNibbleIn = false;
volatile bool     highNibbleOut = false;
volatile uint8_t  dataInByte;
volatile uint8_t  dataOutByte;
volatile uint16_t outDataGetPosition;
volatile uint8_t  checksum;
volatile uint16_t debuglock = 0 ;

extern volatile bool     cmdComplete;
extern volatile uint8_t  skipDeviceCode;

// prototypes
void startDeviceCodeSeq ( void );
void inDataReady ( void );

// code
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

#ifdef DEBUG
volatile uint8_t  debugBuf[DEBUG_SIZE];
void debug_log(const char *fmt, ...)
{
    uint8_t debugLine[120];
    va_list va;
    debuglock = 1;
    va_start (va, fmt);
    sprintf((char*)debugLine,"%d ",mainTimer.read_us());
    strcat((char*)debugBuf,(char*)debugLine);
    vsprintf ((char*)debugLine, fmt, va);
    strcat((char*)debugBuf,(char*)debugLine);
    va_end (va);
    debuglock = 0;
}

void debug_hex(volatile uint8_t *buf, volatile uint16_t len)
{
    int j;
    char tmp[15];
    debuglock = 1;
    sprintf(tmp,"%d <",mainTimer.read_us(), len);
    strcat((char*)debugBuf, tmp);
    for (j=0;j<len;j++) {
        sprintf(tmp, "%02X", (char)buf[j]);
        strcat((char*)debugBuf, tmp);
    }
    sprintf(tmp,">\n");
    strcat((char*)debugBuf, tmp);
    debuglock = 0;
}

// dumping periodically, by a timer-issued thread,
// in order to de-sync from the main functions.
// Much cleaner solution would be to use a circular buffer,
// but it's complicated to format-write into it.
void outDebugDump (void ) {
    while ( debuglock != 0 ) // semaphore
        wait_us (100);
    if ( debugBuf[0]!=0x00 ) {
        debuglock = 1;
        pc.printf ( "%s", (char*)debugBuf ); // slow serial comm
        debugBuf[0]=0x00;
    }
    debuglock = 0;
}

// manually triggered (button push)
void outDebugDumpManual( void ){
    uint8_t i = 20;
    while (i--) { infoLed =! infoLed; wait_ms(20); }
    // printout debug buffers
    pc.printf ( "%s", (char*)debugBuf );
    // reset status
    ResetACK();
    irq_BUSY.rise(NULL);
    irq_BUSY.fall(NULL);
    sprintf ( (char*)debugBuf, "ok\n" ); 
}
#else
void debug_log(const uint8_t *fmt, ...)
{
    return;
}
void debug_hex(volatile uint8_t *buf, volatile uint16_t len)
{
    return;
}
#endif

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
        ERR_PRINTOUT( "sendNibble ERR 1\n" );
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
        ERR_PRINTOUT( "sendNibble ERR 2\n" );
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
                ERR_PRINTOUT( "outDataSpooler ERR 1\n" );
            }
        } else {
            // data stream complete
            pc.putc('t');
            outDataEnd();
            // last wait for BUSY to go DOWN
            uint32_t nTimeout = 50000; // max wait
            while ( in_BUSY && (nTimeout--) )
                wait_us (100);
            if (!nTimeout) {
                ERR_PRINTOUT( "outDataSpooler ERR 2\n" );
            }
            debug_log ( "send complete\n" );
        } 
    } 
}
#else

void debugBUSY_rise ( void ) {
    debug_log ( " BUSY_rise\n" );
}

void debugBUSY_fall ( void ) {
    debug_log ( " BUSY_fall\n" );
}

// Synchronous output data sender 
void SendOutputData ( void ) {
    uint8_t t;
    uint32_t nTimeout;
    
    // set for OUTPUT mode
    in_D_OUT.mode(PullNone);
    in_D_IN.mode(PullNone);
    in_SEL_2.mode(PullNone);
    in_SEL_1.mode(PullNone);  

    irq_BUSY.rise(&debugBUSY_rise);
    irq_BUSY.fall(&debugBUSY_fall);

    testTimer.reset(); 
    testTimer.start(); 
    //pc.printf("outDataGetPosition %d outDataPutPosition %d ", outDataGetPosition, outDataPutPosition);
    while ( outDataGetPosition < outDataPutPosition ) { // outDataPointer < outBufPosition
        wait_us (OUT_NIBBLE_DELAY); // here ?

        // wait for BUSY to go DOWN
        nTimeout = 50000; // max wait: 5 s
        while ( in_BUSY!=0 && (nTimeout--)>0 ) {
            wait_us (100);
        }  
        if (nTimeout < 1) {
            ERR_PRINTOUT("Send error 1\n");
            ResetACK();
            break;
        };
        //debug_log ( " %1X timeout 1 %d\n", (in_BUSY!=0), nTimeout);

        if ( highNibbleOut ) {
            highNibbleOut = false;
            t = (dataOutByte >> 4);
            outDataGetPosition++;
        } else {
            highNibbleOut = true;
            dataOutByte = outDataBuf[outDataGetPosition];
            //pc.putc('.');
            //debug_log (" %d: %02X\n", outDataGetPosition, dataOutByte); // debug ONLY (can fill up space)
            t = (dataOutByte & 0x0F);
        }
        out_SEL_1 = (t&0x01);
        out_SEL_2 = ((t&0x02)>>1);
        out_D_OUT = ((t&0x04)>>2);
        out_D_IN  = ((t&0x08)>>3);
        // nibble is ready for Sharp-PC to get it        
        wait_us (OUT_NIBBLE_DELAY); // here too?
        SetACK();
        // then wait for BUSY to go UP 
        nTimeout = 50000; // max wait: 5 s
        while ( in_BUSY==0 && (nTimeout--)>0 ) {
            wait_us (100);
        }  
        if (nTimeout < 1) {
            ERR_PRINTOUT("Send error 1\n");
            ResetACK();
            break;
        };
        //debug_log ( " %1X timeout 2 %d\n", (in_BUSY!=0), nTimeout);

        ResetACK();
    } 
    testTimer.stop();
    pc.putc('\n');
    debug_log ( "send complete\n" );
    debug_log ( "avg output timing (ms/byte): %.2f\n", testTimer.read_us()/outDataGetPosition/1000.0); 
    //wait_us ( IN_DATAREADY_TIMEOUT );
    // set for INPUT mode
    in_D_OUT.mode(PullDown);
    in_D_IN.mode(PullDown);
    in_SEL_2.mode(PullDown);
    in_SEL_1.mode(PullDown); 
    out_D_OUT = 0;
    out_D_IN  = 0;     
    out_SEL_2 = 0;     
    out_SEL_1 = 0;

    irq_BUSY.fall(NULL);
    irq_BUSY.rise(NULL);
}
#endif

void inNibbleReady ( void ) {
    // probe input lines and get nibble value
    uint8_t inNibble = ( in_SEL_1 + (in_SEL_2<<1) + (in_D_OUT<<2) + (in_D_IN<<3) );
    //debug_log ( "(%d) %01X \n", highNibbleIn, inNibble ) ; 
    if ( out_ACK == 0 ) {
        wait_us ( NIBBLE_DELAY_1 );
        SetACK();
        if ( highNibbleIn ) {
            highNibbleIn = false;
            inDataBuf[inBufPosition] = (inNibble << 4) + inDataBuf[inBufPosition];
            checksum = (inDataBuf[inBufPosition] + checksum) & 0xff;
            debug_log(" %u:0x%02X [%02X]\n", 
                inBufPosition, inDataBuf[inBufPosition], checksum ) ;
            inBufPosition++; // should be circular for safety; may cut off data!
            // Data processing starts after last byte (timeout reset after each byte received) 
            inDataReadyTimeout.attach_us( &inDataReady, IN_DATAREADY_TIMEOUT );
        } else {
            highNibbleIn = true;
            inDataBuf[inBufPosition] = inNibble;
            //debug_log ( " %01X\n", inDataBuf[inBufPosition] ) ; 
        }
    } else {
        ERR_PRINTOUT( "inNibbleReady out_ACK!=0\n" ) ;
    }
}

void inNibbleAck ( void ) {
    // test lines
    // debug_log ( "ack (%01X)\n\r", ( in_SEL_1 + (in_SEL_2<<1) + (in_D_OUT<<2) + (in_D_IN<<3) )) ; 
    if ( out_ACK == 1 ) {
        wait_us ( NIBBLE_ACK_DELAY );
        ResetACK();
    } else {
        ERR_PRINTOUT( "inNibbleAck out_ACK!=1\n" ); 
    }
}

void SendErrorOut ( void ) {
    outDataBuf[ 0 ] = 0xFF; // error ?
    outDataPutPosition = 1;
    SendOutputData();
}

void inDataReady ( void ) {
    pc.putc('c');
    // receive complete
    testTimer.stop();
    debug_log ( "Processing...\n" ) ;
    // stop the BUSY triggers
    irq_BUSY.fall(NULL);
    irq_BUSY.rise(NULL);
    if ( inBufPosition > 0 ) {
        debug_log ( "in: %d bytes (first 40 below)\n", inBufPosition) ; 
        debug_hex ( inDataBuf, (inBufPosition) < (40) ? (inBufPosition) : (40) );
        debug_log ( "avg input timing (ms/byte): %.2f\n", 
           (testTimer.read_us() - IN_DATAREADY_TIMEOUT) / inBufPosition/1000.0); 
        // Verify checksum
        checksum=0;
        for (int i=0;i<inBufPosition-1;i++) {
            checksum = (inDataBuf[i]+checksum) & 0xff;
        }   
        debug_log ( "checksum 0x%02X vs 0x%02X\n" ,  checksum, inDataBuf[inBufPosition-1]); 
        if ( checksum == inDataBuf[inBufPosition-1] ) {
            //pc.printf(" 0x%02X\n", inDataBuf[0]);
            debug_log ( "command 0x%02X\n" , inDataBuf[0]); 
            outDataGetPosition = 0;
            outDataPutPosition = 0;
            highNibbleOut = false;
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
                debug_log ( "out: %u bytes (first 40 below)\n" , outDataPutPosition);
                debug_hex ( outDataBuf, (outDataPutPosition) < (40) ? (outDataPutPosition) : (40) );
                // Take control and send processed data to Sharp
                pc.putc('o');
                SendOutputData(); 
                // some commands do not have the device-code sequence
                // so we directly receive next byte 
                if ( skipDeviceCode != 0x00 ) {
                    pc.putc('n');
                    debug_log ( "next: 0x%02X\n", skipDeviceCode ) ;
                    inBufPosition = 0;
                    highNibbleIn = false;
                    checksum = 0;
                    testTimer.reset();
                    testTimer.start(); 
                    wait_us (NIBBLE_DELAY_2) ; // add a delay 
                    // set data handshake triggers on the BUSY line
                    irq_BUSY.fall(&inNibbleAck);
                    irq_BUSY.rise(&inNibbleReady);                
                } else {
                    irq_BUSY.rise(NULL);
                    irq_BUSY.fall(NULL);
                }
            } else {
                ERR_PRINTOUT("Command processing error\n"); 
                SendErrorOut();
            }
#endif
        } else {
            ERR_PRINTOUT("checksum error\n"); 
            SendErrorOut();
        }
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
                skipDeviceCode = 0;
                // set data handshake triggers on the BUSY line
                irq_BUSY.fall(&inNibbleAck);
                irq_BUSY.rise(&inNibbleReady);
                testTimer.reset();
                testTimer.start();
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
                    ERR_PRINTOUT("bitReady Timeout!\n\r") ;
                }
            } 
        } else {
            wait_us ( BIT_DELAY_2 );
            SetACK();
        }
    }
}

void startDeviceCodeSeq ( void ) {
    uint32_t nTimeout = 100;
    debug_log ( "startDeviceCodeSeq start\n" );
    while ( ( in_D_OUT == 0 ) && (nTimeout--) ) {
        wait_us (BIT_DELAY_1);
    }
    wait_us (BIT_DELAY_1);
    //pc.putc('s'); // debug 
    debug_log ( "startDeviceCodeSeq in_D_OUT\n" );
    if ( in_D_OUT == 1 ) {
        // Device Code transfer starts with both X_OUT and DOUT high
        // (X_OUT high with DOUT low is for cassette write)
        SetACK();
        bitCount = 0;
        deviceCode = 0;
        //debugBuf[0] = 0;  // with a periodic dump: buffer resets
        inBufPosition = 0;
        debug_log ("Device\n");
        wait_us (ACK_DELAY) ;   //?? or, wait only AFTER enabling trigger ??
        // serial bit trigger
        irq_BUSY.rise(&bitReady);
        irq_BUSY.fall(NULL);
        wait_us (ACK_DELAY) ;
    }
}

char sio_buf [80];
int sio_pos = 0;

// Here we could handle commands issued through the serial console
void sio_callback() {
    // Note: you need to actually read from the serial to clear the RX interrupt
    char c = pc.getc();
    
    // store char in buffer and process command on 'Enter'
    if ( sio_pos < 80 ) {
        pc.putc(c);
        sio_buf[sio_pos] = c;
        sio_pos++;
        if ( c == 0x0D) {
            infoLed = !infoLed;
            wait_ms(20);
            infoLed = !infoLed;
            
            // parse and process command in buffer
            // ... TO DO ...

            sio_pos = 0;
        }
    }
}

int main(void) {
  uint8_t i = 20;

  pc.baud(115200);
  pc.printf("CE140F emulator init\n");
  while (i--) {
    infoLed = !infoLed;
    wait_ms(20);
  }

  inBufPosition = 0;
#ifdef DEBUG
  debugBuf[0] = 0;
  user_BTN.rise(&outDebugDumpManual);
  debugOutTimeout.attach_us( &outDebugDump, DEBUG_TIMEOUT );
#endif

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



  // in the callback we handle commands issued through the serial console
  pc.attach(&sio_callback);

  while (1) {
     
    // Sharp CE140F emulator logic is handled by interrupts and timers
    
    wait(1);

  }
}
