/////////////////////////////////////////////////
// Sharp CE-140F diskette emulator
// Original got by email by
// De : Fabio Fumi <ffumi68@gmail.com>
// Envoyé : jeudi 7 avril 2022 14:50
// À : contact@pockemul.com
// Objet : Re: SL-16 documentation or info
// - Adapted to MBed NUCLEO board
/////////////////////////////////////////////////
#include "mbed.h"

// TODO: Finish to emulate missing disk functions

#define DOWN    0
#define UP      1
#define BUF_SIZE 1024

#define RECEIVE_MODE    1
#define SEND_MODE       2
#define TEST_MODE       3

DigitalOut      busyLed(LED1);
RawSerial       pc(USBTX, USBRX);
InterruptIn     btn(USER_BUTTON);
Timer           mainTimer;
Timer           perfTimer;
AnalogIn        debug_A0 ( A0 );

static char            debugBuf[BUF_SIZE];
static char            debugLine[80];
static uint16_t        dataPtr;
static char            dataBuf[BUF_SIZE];


static bool            ctrl_char;
static char            t;
static char            c;

static uint8_t             code_transfer_step;
static uint8_t             device_code;
static uint32_t            wait_data_function;
static bool            halfdata ;
static bool            halfdata_out ;
static uint32_t             run_oldstate;
static uint32_t             lastRunState;
static uint32_t             lastState;
static uint8_t         ce140f_Mode;
static bool            Previous_PIN_BUSY;
static bool            Previous_PIN_MT_OUT1;
static bool            Previous_PIN_D_OUT;
static uint32_t        perfTime;
static uint32_t        perfNrCalls;

// Sharp 11-pin connector positions
//********************************************************/
// PIN_MT_OUT2  1
// PIN_VGG      2
// PIN_GND      3
// PIN_BUSY     4
// PIN_D_OUT    5
// PIN_MT_IN    6 // also X_IN
// PIN_MT_OUT1  7 // also X_OUT
// PIN_D_IN     8
// PIN_ACK      9
// PIN_SEL2     10
// PIN_SEL1     11
//********************************************************/
bool MT_OUT2 = false;  
bool BUSY    = false;  
bool D_OUT   = false;  
bool MT_IN   = false;
bool MT_OUT1 = false;
bool D_IN    = false;
bool ACK     = false;
bool SEL2    = false;
bool SEL1    = false;
    
bool Cce140f_init(void);
bool Cce140f_run(void);

void btnRaised() 
{
    pc.printf ("Tot time (us) %d\n\r", perfTime );
    pc.printf ("nr calls %d\n\r", perfNrCalls);
    pc.printf ("%s\n\r", debugBuf);
}

int main()
{

    busyLed  = 0;
    ctrl_char = false;
    dataPtr = 0;
    bool ret ;
    
    pc.baud ( 57600 ); 
    
    perfNrCalls = 0;
    perfTime = 0;
 
    btn.rise(&btnRaised);
   
    Cce140f_init();
    // main loop
    while ( true ) {
        perfTimer.reset();
        perfTimer.start();
        ret = Cce140f_run(); // about 90 us ~ 10 samples / ms
        perfTime += perfTimer.read_us();
        perfNrCalls += 1;
        // wait_us (100); Cce140f_run takes long enough already
    }
    
}

// Assign each Nucleo pin as input
bool get_MT_OUT2  ( void ) { DigitalIn  in_MT_OUT2 (D3);   return in_MT_OUT2 ; }
bool get_BUSY     ( void ) { DigitalIn  in_BUSY    (D4);   return in_BUSY    ; }
bool get_D_OUT    ( void ) { DigitalIn  in_D_OUT   (D5);   return in_D_OUT   ; }
bool get_MT_IN    ( void ) { DigitalIn  in_MT_IN   (D6);   return in_MT_IN   ; }
bool get_MT_OUT1  ( void ) { DigitalIn  in_MT_OUT1 (D7);   return in_MT_OUT1 ; }
bool get_D_IN     ( void ) { DigitalIn  in_D_IN    (D8);   return in_D_IN    ; }
bool get_ACK      ( void ) { DigitalIn  in_ACK     (D9);   return in_ACK     ; }
bool get_SEL2     ( void ) { DigitalIn  in_SEL2    (D10);  return in_SEL2    ; }
bool get_SEL1     ( void ) { DigitalIn  in_SEL1    (D11);  return in_SEL1    ; }

void Get_Connector (void)
{
   MT_OUT2 = get_MT_OUT2(); 
   BUSY    = get_BUSY   (); 
   D_OUT   = get_D_OUT  (); 
   MT_IN   = get_MT_IN  (); 
   MT_OUT1 = get_MT_OUT1(); 
   D_IN    = get_D_IN   (); 
   ACK     = get_ACK    (); 
   SEL2    = get_SEL2   (); 
   SEL1    = get_SEL1   ();
} 

// Assign each Nucleo pins as output
void set_MT_OUT2  ( bool value ) {  DigitalOut out_MT_OUT2 (D3);    out_MT_OUT2  = value ; }
void set_BUSY     ( bool value ) {  DigitalOut out_BUSY    (D4);    out_BUSY     = value ; }
void set_D_OUT    ( bool value ) {  DigitalOut out_D_OUT   (D5);    out_D_OUT    = value ; }
void set_MT_IN    ( bool value ) {  DigitalOut out_MT_IN   (D6);    out_MT_IN    = value ; }
void set_MT_OUT1  ( bool value ) {  DigitalOut out_MT_OUT1 (D7);    out_MT_OUT1  = value ; }
void set_D_IN     ( bool value ) {  DigitalOut out_D_IN    (D8);    out_D_IN     = value ; }
void set_ACK      ( bool value ) {  DigitalOut out_ACK     (D9);    out_ACK      = value ; }
void set_SEL2     ( bool value ) {  DigitalOut out_SEL2    (D10);   out_SEL2     = value ; }
void set_SEL1     ( bool value ) {  DigitalOut out_SEL1    (D11);   out_SEL1     = value ; }

void Set_Connector (void)
{  
  set_MT_OUT2  ( MT_OUT2 );
  set_BUSY     ( BUSY    );
  set_D_OUT    ( D_OUT   );
  set_MT_IN    ( MT_IN   );
  set_MT_OUT1  ( MT_OUT1 );
  set_D_IN     ( D_IN    );
  set_ACK      ( ACK     );
  set_SEL2     ( SEL2    );
  set_SEL1     ( SEL1    );
}

bool Cce140f_init(void)
{
    
    pc.printf("CE-140F initializing...\n\r");

    //  SET_PIN(PIN_ACK,DOWN);
    //pc.printf("Initial value for PIN_BUSY %c",PIN_BUSY);
    Get_Connector();
    Previous_PIN_BUSY = BUSY;
    Previous_PIN_MT_OUT1 = MT_OUT1;
    
    code_transfer_step = 0;
    device_code = 0;
    wait_data_function=0;
    halfdata = false;
    halfdata_out = false;

    run_oldstate = -1;
    lastRunState = 0;
    dataPtr = 0;

    MT_OUT2 = false;  
    BUSY    = false;  
    D_OUT   = false;  
    MT_IN   = false;
    MT_OUT1 = false;
    D_IN    = false;
    ACK     = false;
    SEL2    = false;
    SEL1    = false;

    mainTimer.reset();
    mainTimer.start();

    t = 0;
    c = 0;  
}

bool Cce140f_run(void)
{

    // get signals from connector
    //Get_Connector();

    bool bit = false;
    ce140f_Mode=RECEIVE_MODE;

    bool PIN_BUSY_GoDown = ( ( BUSY == DOWN ) && (Previous_PIN_BUSY == UP)) ? true:false;
    bool PIN_BUSY_GoUp   = ( ( BUSY == UP ) && (Previous_PIN_BUSY == DOWN)) ? true:false;

    if (code_transfer_step >0) {
        lastRunState = mainTimer.read_ms();
    }

    switch (code_transfer_step) {
    case 0 :    if ((get_MT_OUT1() == UP) && (get_D_OUT() == UP)) {
                    // Device Code protocol started with XOUT & DOUT up
                    mainTimer.reset();
                    lastState = mainTimer.read_ms(); //time.restart();
                    code_transfer_step=1;
                }
                busyLed = 0;
                break;
    case 1 :    if ((get_MT_OUT1() == UP) && (get_D_OUT() == UP)) {
                    if ((mainTimer.read_ms() - lastState) > 40) {
                        // XOUT & DOUT up for 40 ms: Device bit sending started 
                        // Raise ACK
                        code_transfer_step = 2;
                        set_ACK ( UP );
                        busyLed = 1; 
                        sprintf(debugBuf, "1 - %d %d %d\n\r", mainTimer.read_us(), lastState, mainTimer.read_ms());
                    }
                }
                else {
                    code_transfer_step=0;
                }
                break;
    case 2:     if ( get_BUSY() == UP ){ // New bit available
                    if( get_D_OUT() == UP )
                        bit = true;
                    else
                        bit = false;
                    sprintf(debugLine, "2 - %d bit %d %d\n\r", mainTimer.read_us(), bit, debug_A0.read()); strcat ( debugBuf, debugLine );
                    t>>=1;
                    if (bit) t|=0x80;
                    if ((c=(++c)&7)==0)  {
                        sprintf(debugLine, "2 - %d code %x\n\r", mainTimer.read_us(), t); strcat ( debugBuf, debugLine );
                        pc.printf(" device code: 0x%x\n\r", t);
                        device_code = t;
                        if (device_code==0x41) // This is for the CE-140F
                            code_transfer_step=4;
                        else {
                            code_transfer_step = 0; // This is not for us
                            t=0; c=0;
                        }
                    } else  {
                        code_transfer_step=3;
                        lastState=mainTimer.read_ms();
                    }
                    set_ACK ( DOWN ); // bit received
                }
                break;
    case 3:     if ((mainTimer.read_ms() - lastState) > 2) {
                    sprintf(debugLine, "3 - %d\n\r", mainTimer.read_us()); strcat ( debugBuf, debugLine );
                    code_transfer_step=2;
                    // wait 2 ms then raise again ACK - ready for next bit
                    set_ACK ( UP );
                }
                break;
    case 4:     if ((get_BUSY() == DOWN)&&(get_MT_OUT1() == DOWN)) {
                    sprintf(debugLine, "4 - %d\n\r", mainTimer.read_us()); strcat ( debugBuf, debugLine );
                    set_ACK ( UP );
                    code_transfer_step=5;
                    lastState=mainTimer.read_ms(); //time.restart();
                    t=0; c=0;
                }
                break;
    case 5:     if ((mainTimer.read_ms() - lastState) > 9) {
                    sprintf(debugLine, "5 - %d\n\r", mainTimer.read_us()); strcat ( debugBuf, debugLine );
                    set_ACK ( DOWN );
                    code_transfer_step=0;
                }
                break;
            }

/* 
    if ( (device_code == 0x41) && (code_transfer_step==0)) {
        if (PIN_BUSY_GoUp && (ACK==DOWN)) {
            
            lastRunState = mainTimer.read_ms();
            // read the 4 bits
            t = SEL1 + (SEL2<<1) + (D_OUT<<2) + (D_IN<<3);
            Cce140f_Push4(t);
            ACK = UP;
            lastState=mainTimer.read_ms(); //time.restart();

        }
        else
        if (PIN_BUSY_GoDown && (ACK==UP)) {
            lastRunState = mainTimer.read_ms();
            ACK = DOWN;
            lastState=mainTimer.read_ms();//time.restart();
        }

    else

        if ( !data_out.empty() &&
             ((mainTimer.read_ms() - lastState)>5) &&
             (BUSY==DOWN) &&
             (ACK==DOWN)) {
            lastRunState = mainTimer.read_ms();
            uint8_t t = Cce140f_Pop_out4();

            SEL1 = (t&0x01);
            SEL2 = ((t&0x02)>>1);
            D_OUT= ((t&0x04)>>2);
            D_IN = ((t&0x08)>>3);

            ACK = UP;
        }
        else
            if ( (ACK==UP) && PIN_BUSY_GoUp) {
                lastRunState = mainTimer.read_ms();
                ACK = DOWN;
            }
        else
            if ( ((mainTimer.read_ms() - lastState)>50) && !data.empty()) {
                lastRunState = mainTimer.read_ms();
                processCommand();
                data.clear();
            }

    }




    if (mainTimer->msElapsed(lastRunState)<3000) {

        if (!busyLed) {
            busyLed = true;
            Refresh_Display = true;
//            update();
        }
    }
    else {
        if (busyLed) {
            busyLed = false;
            Refresh_Display = true;
//            update();
        }
    }
*/
    
    Previous_PIN_BUSY = BUSY;
    Previous_PIN_MT_OUT1 = MT_OUT1;
    Previous_PIN_D_OUT = D_OUT;

    // send signals out
    //Set_Connector();
    
    return true;
    
}


// ....  Cce140f_processCommand,  etc.