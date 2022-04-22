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


static int             dataPtr;
static char            dataBuf[BUF_SIZE];

static bool            ctrl_char;
static char            t;
static char            c;

static int             code_transfer_step;
static int             device_code;
static int             wait_data_function;
static bool            halfdata ;
static bool            halfdata_out ;
static int             run_oldstate;
static int             lastRunState;
static int             lastState;
static uint8_t         ce140f_Mode;
static bool            Previous_PIN_BUSY;
static bool            Previous_PIN_MT_OUT1;
static bool            Previous_PIN_D_OUT;
static uint32_t        perfTime;
static uint32_t        perfNrCalls;

volatile bool          D_OUT_Up;


// Sharp 11-pin connector positions
//********************************************************/
// PIN_MT_OUT2  1
// PIN_GND      2
// PIN_VGG      3
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
        ret = Cce140f_run();
        perfTime += perfTimer.read_us();
        perfNrCalls += 1;
        wait_us (100); // about 10 samples per ms
    }
    
}

void Get_Connector (void)
{
    
    // Assign Nucleo pins as inputs
    DigitalIn  in_MT_OUT2 (D3);
    DigitalIn  in_BUSY    (D4);
    DigitalIn  in_D_OUT   (D5);
    DigitalIn  in_MT_IN   (D6);
    DigitalIn  in_MT_OUT1 (D7);
    DigitalIn  in_D_IN    (D8);
    DigitalIn  in_ACK     (D9); 
    DigitalIn  in_SEL2    (D10);
    DigitalIn  in_SEL1    (D11);
    // get values
    MT_OUT2 = in_MT_OUT2;
    BUSY    = in_BUSY;
    D_OUT   = in_D_OUT;
    MT_IN   = in_MT_IN;
    MT_OUT1 = in_MT_OUT1;
    D_IN    = in_D_IN;
    ACK     = in_ACK;
    SEL2    = in_SEL2;
    SEL1    = in_SEL1;

}

void Set_Connector (void)
{
    
    // Assign Nucleo pins as outputs
    DigitalOut out_MT_OUT2 (D3);
    DigitalOut out_BUSY    (D4);
    DigitalOut out_D_OUT   (D5);
    DigitalOut out_MT_IN   (D6);
    DigitalOut out_MT_OUT1 (D7);
    DigitalOut out_D_IN    (D8);
    DigitalOut out_ACK     (D9); 
    DigitalOut out_SEL2    (D10);
    DigitalOut out_SEL1    (D11);
    // set values
    out_MT_OUT2 = MT_OUT2;
    out_BUSY    = BUSY;
    out_D_OUT   = D_OUT;
    out_MT_IN   = MT_IN;
    out_MT_OUT1 = MT_OUT1;
    out_D_IN    = D_IN;
    out_ACK     = ACK;
    out_SEL2    = SEL2;
    out_SEL1    = SEL1;

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
    Get_Connector();

    bool bit = false;
    ce140f_Mode=RECEIVE_MODE;

    bool PIN_BUSY_GoDown = ( ( BUSY == DOWN ) && (Previous_PIN_BUSY == UP)) ? true:false;
    bool PIN_BUSY_GoUp   = ( ( BUSY == UP ) && (Previous_PIN_BUSY == DOWN)) ? true:false;

    if (code_transfer_step >0) {
        lastRunState = mainTimer.read_ms();
    }

    switch (code_transfer_step) {
    case 0 :    if ((MT_OUT1 == UP) && (D_OUT==UP)) {
                    // Device Code protocol started with XOUT & DOUT up
                    lastState = mainTimer.read_ms(); //time.restart();
                    code_transfer_step=1;
                }
                busyLed = 0;
                break;
    case 1 :    if ((MT_OUT1 == UP) && (D_OUT==UP)) {
                    if ((mainTimer.read_ms() - lastState) > 40) {
                        // XOUT & DOUT up for 40 ms: Device bit sending started 
                        // Raise ACK
                        code_transfer_step = 2;
                        ACK = UP;
                        busyLed = 1; 
                    }
                }
                else {
                    code_transfer_step=0;
                }
                break;
    case 2:     if ( BUSY == UP ){ // New bit available
                    if( D_OUT == UP )
                        bit = true;
                    else
                        bit = false;
                    t>>=1;
                    if (bit) t|=0x80;
                    if ((c=(++c)&7)==0)  {
                        pc.printf(" device code: 0x%x\n\r", t);
                        device_code = t;
                        if (device_code==0x41) // This is for the CE-140F
                            code_transfer_step=4;
                        else {
                            code_transfer_step = 0; // Not for us
                            t=0; c=0;
                        }
                    }
                    else  
                        code_transfer_step=3;
                    lastState=mainTimer.read_ms();
                    ACK = DOWN; // bit received
                }
                break;
    case 3:     if ((mainTimer.read_ms() - lastState) > 2) {
                    code_transfer_step=2;
                    // wait 2 ms then raise again ACK
                    ACK = UP;
                }
                break;
    case 4:     if ((BUSY == DOWN)&&(MT_OUT1 == DOWN)) {
                    ACK = UP;
                    code_transfer_step=5;
                    lastState=mainTimer.read_ms(); //time.restart();
                    t=0; c=0;
                }
                break;
    case 5:     if ((mainTimer.read_ms() - lastState)>9) {
                    ACK = DOWN;
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
    Set_Connector();
    
    return true;
    
}


// ....  Cce140f_processCommand,  etc.