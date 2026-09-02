// Sharp CE-140F diskette emulator
// Reverse engineering of CE-140F protocol by contact@pockemul.com
// Ported to bare-metal STM32Cube HAL (Nucleo-L432KC)

#include "main.h"
#include "commands.h"
#include "pintest.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define DEBUG 1
#define DEBUG_SIZE 1024
#define DEBUG_TIMEOUT 3000

#define NIBBLE_DELAY_1 1000
#define NIBBLE_DELAY_2 1000
#define NIBBLE_ACK_DELAY 100
#define BIT_DELAY_1 1000
#define BIT_DELAY_2 2000
#define ACK_DELAY 20000
#define ACK_TIMEOUT 1000 // In milliseconds for HAL
#define DATA_WAIT 9000
#define IN_DATAREADY_TIMEOUT 15000 // In us (Service Manual: ACK low >10ms and data out <50ms)
#define OUT_NIBBLE_DELAY 500

// Extern hardware handles auto-instantiated by STM32CubeMX
extern UART_HandleTypeDef huart2;

// Protocol tracking variables
volatile uint8_t  deviceCode;
volatile uint8_t  bitCount;
volatile bool     highNibbleIn = false;
volatile bool     highNibbleOut = false;
volatile uint8_t  dataInByte;
volatile uint8_t  dataOutByte;
volatile uint16_t outDataGetPosition;
volatile uint8_t  checksum;
volatile uint16_t debuglock = 0;

extern volatile bool     cmdComplete;
extern volatile uint8_t  skipDeviceCode;

// Volatile function pointers to replicate MBed's dynamic interrupt attach/detach
void (*irq_BUSY_rise)(void) = NULL;
void (*irq_BUSY_fall)(void) = NULL;

// Virtual software timers running via main loop or callback tracking
uint32_t ackOffTimestamp = 0;
bool ackOffActive = false;
uint32_t inDataReadyTimestamp = 0;
bool inDataReadyActive = false;
uint32_t debugDumpTimestamp = 0;

// Function declarations
void startDeviceCodeSeq(void);
void inDataReady(void);
void inNibbleAck(void);
void inNibbleReady(void);
void bitReady(void);

// Microsecond delay engine using CPU Cycle Counter (DWT)
//
// NOTE: DWT->CYCCNT is a free running 32-bit counter that wraps every
// 2^32 cycles (~53.7 s @ 80 MHz). All *elapsed time* computations must
// therefore be done on the raw cycle count (unsigned 32-bit subtraction is
// wrap-safe), NEVER on an already-divided microsecond value - dividing first
// moves the wrap point to a non-power-of-two boundary and makes the
// subtraction produce garbage (spurious instant timeouts, or ~50 s hangs).
static uint32_t cyclesPerUs = 1;
static volatile uint32_t dwtLastCyc = 0;
static volatile uint64_t dwtAccumCyc = 0;

void DWT_Init(void) {
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    cyclesPerUs = SystemCoreClock / 1000000U;
    if (cyclesPerUs == 0) cyclesPerUs = 1;
    dwtLastCyc = DWT->CYCCNT;
    dwtAccumCyc = 0;
}

void wait_us(uint32_t us) {
    uint32_t startCyc = DWT->CYCCNT;
    uint32_t delayTicks = us * cyclesPerUs;
    while ((uint32_t)(DWT->CYCCNT - startCyc) < delayTicks);
}

// Wrap-safe elapsed microseconds since a snapshot taken with DWT->CYCCNT
static inline uint32_t us_since(uint32_t startCyc) {
    return (uint32_t)(DWT->CYCCNT - startCyc) / cyclesPerUs;
}

// Monotonic microsecond timestamp for logging (rollover-extended, IRQ safe)
uint32_t read_us(void) {
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    uint32_t now = DWT->CYCCNT;
    dwtAccumCyc += (uint32_t)(now - dwtLastCyc);
    dwtLastCyc = now;
    uint64_t us = dwtAccumCyc / cyclesPerUs;
    if (!primask) __enable_irq();
    return (uint32_t)us;
}

// BUSY (EXTI9_5) interrupt masking.
// The reference MBed firmware reached SendOutputData() from a us-ticker
// callback, i.e. from interrupt context, so the BUSY EXTI (same NVIC
// priority) could never preempt the send loop. Here SendOutputData() runs in
// thread mode, so we must mask the BUSY interrupt explicitly to get the same
// uninterrupted, deterministic nibble timing.
static inline void BUSY_IRQ_Disable(void) {
    HAL_NVIC_DisableIRQ(EXTI9_5_IRQn);
}

static inline void BUSY_IRQ_Enable(void) {
    __HAL_GPIO_EXTI_CLEAR_IT(in_BUSY_Pin);
    HAL_NVIC_ClearPendingIRQ(EXTI9_5_IRQn);
    HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);
}

void ResetACK(void) {
    HAL_GPIO_WritePin(out_ACK_GPIO_Port, out_ACK_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(infoLed_GPIO_Port, infoLed_Pin, GPIO_PIN_RESET);
    ackOffActive = false;
}

void SetACK(void) {
    HAL_GPIO_WritePin(out_ACK_GPIO_Port, out_ACK_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(infoLed_GPIO_Port, infoLed_Pin, GPIO_PIN_SET);
    ackOffTimestamp = HAL_GetTick();
    ackOffActive = true;
}

#ifdef DEBUG
volatile uint8_t debugBuf[DEBUG_SIZE];

extern "C" void debug_log(const char *fmt, ...) {
    uint8_t debugLine[120];
    va_list va;
    
    // Check if buffer has space (leave room for new entry)
    // BUFFER OVERFLOW possible - security checks needed!
    size_t currentLen = strlen((char*)debugBuf);
    if (currentLen > DEBUG_SIZE - 150) {
        return; // Buffer full, skip logging
    }
    
    debuglock = 1;
    va_start(va, fmt);
    sprintf((char*)debugLine, "%lu ", read_us());
    strcat((char*)debugBuf, (char*)debugLine);
    vsprintf((char*)debugLine, fmt, va);
    strcat((char*)debugBuf, (char*)debugLine);
    va_end(va);
    debuglock = 0;
}

extern "C" void debug_hex(volatile uint8_t *buf, volatile uint16_t len) {
    int j;
    char tmp[15];
    
    // Check if buffer has space
    size_t currentLen = strlen((char*)debugBuf);
    if (currentLen > DEBUG_SIZE - ((size_t)len * 3 + 30)) {
        return; // Buffer full, skip logging
    }
    
    debuglock = 1;
    sprintf(tmp, "%lu <", read_us());
    strcat((char*)debugBuf, tmp);
    for (j = 0; j < len; j++) {
        sprintf(tmp, "%02X", (char)buf[j]);
        strcat((char*)debugBuf, tmp);
    }
    sprintf(tmp, ">\n");
    strcat((char*)debugBuf, tmp);
    debuglock = 0;
}

extern "C" void outDebugDump(void) {
    while (debuglock != 0) wait_us(100);
    if (debugBuf[0] != 0x00) {
        debuglock = 1;
        HAL_UART_Transmit(&huart2, (uint8_t*)debugBuf, strlen((char*)debugBuf), HAL_MAX_DELAY);
        debugBuf[0] = 0x00;
    }
    debuglock = 0;
}

void outDebugDumpManual(void) {
    uint8_t i = 20;
    while (i--) {
        HAL_GPIO_TogglePin(infoLed_GPIO_Port, infoLed_Pin);
        HAL_Delay(20);
    }
    HAL_UART_Transmit(&huart2, (uint8_t*)debugBuf, strlen((char*)debugBuf), HAL_MAX_DELAY);
    ResetACK();
    sprintf((char*)debugBuf, "ok\n");
}
#else
void debug_log(const char *fmt, ...) {}
void debug_hex(volatile uint8_t *buf, volatile uint16_t len) {}
void outDebugDump(void) {}
#endif

// Shared Callback routing for Edge EXTI pins
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin) {
    if (GPIO_Pin == user_BTN_Pin) {
        #ifdef DEBUG
        outDebugDumpManual();
        #endif
    }
    else if (GPIO_Pin == in_X_OUT_Pin) {
        startDeviceCodeSeq();
    }
    else if (GPIO_Pin == in_BUSY_Pin) {
        if (HAL_GPIO_ReadPin(in_BUSY_GPIO_Port, in_BUSY_Pin) == GPIO_PIN_SET) {
            if (irq_BUSY_rise != NULL) {
                irq_BUSY_rise();
            }
        } else {
            if (irq_BUSY_fall != NULL) {
                irq_BUSY_fall();
            }
        }
    }
}

// Max wait for a BUSY transition during output, in microseconds.
#define OUT_BUSY_TIMEOUT_US 250000UL

void SendOutputData(void) {
    uint8_t t = 0;
    uint32_t startCyc;
    uint32_t waitCyc;
    bool aborted = false;

    // Restore the input data lines to pull-down mode and release the bus.
    // Single cleanup path - every exit from this function must go through it.
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    startCyc = DWT->CYCCNT;

    // Mask BUSY interrupt during output data sending to ensure deterministic timing
    BUSY_IRQ_Disable();
    irq_BUSY_rise = NULL;
    irq_BUSY_fall = NULL;

    // Set input data pins to high impedance (no pull) during output
    // This prevents pull resistors from interfering with the level converter
    // (matches MBed's in_xxx.mode(PullNone) behavior)

    // in_SEL_1 = PB1, in_SEL_2 = PB6 (on GPIOB)
    GPIO_InitStruct.Pin = in_SEL_1_Pin | in_SEL_2_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    // in_D_OUT = PA10, in_D_IN = PA1 (on GPIOA)
    GPIO_InitStruct.Pin = in_D_OUT_Pin | in_D_IN_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    // Ensure ACK is low before starting
    ResetACK();

    while (outDataGetPosition < outDataPutPosition) {
        wait_us(OUT_NIBBLE_DELAY);

        // Wait for BUSY to go DOWN
        waitCyc = DWT->CYCCNT;
        while ((in_BUSY_GPIO_Port->IDR & in_BUSY_Pin) != 0) {
            if (us_since(waitCyc) > OUT_BUSY_TIMEOUT_US) {
                debug_log("SO Err1 pos: %u\n", outDataGetPosition);
                aborted = true;
                break;
            }
            wait_us(100);
        }
        if (aborted) break;

        if (highNibbleOut) {
            highNibbleOut = false;
            t = (dataOutByte >> 4);
            outDataGetPosition++;
        } else {
            highNibbleOut = true;
            dataOutByte = outDataBuf[outDataGetPosition];
            t = (dataOutByte & 0x0F);
        }

        // Set data on output lines
        HAL_GPIO_WritePin(out_SEL_1_GPIO_Port, out_SEL_1_Pin, (t & 0x01) ? GPIO_PIN_SET : GPIO_PIN_RESET);
        HAL_GPIO_WritePin(out_SEL_2_GPIO_Port, out_SEL_2_Pin, (t & 0x02) ? GPIO_PIN_SET : GPIO_PIN_RESET);
        HAL_GPIO_WritePin(out_D_OUT_GPIO_Port, out_D_OUT_Pin, (t & 0x04) ? GPIO_PIN_SET : GPIO_PIN_RESET);
        HAL_GPIO_WritePin(out_D_IN_GPIO_Port, out_D_IN_Pin, (t & 0x08) ? GPIO_PIN_SET : GPIO_PIN_RESET);

        // Nibble is ready for Sharp-PC to get it
        wait_us(OUT_NIBBLE_DELAY);

        SetACK();

        // Wait for BUSY to go UP
        waitCyc = DWT->CYCCNT;
        while ((in_BUSY_GPIO_Port->IDR & in_BUSY_Pin) == 0) {
            if (us_since(waitCyc) > OUT_BUSY_TIMEOUT_US) {
                debug_log("SO Err2 pos: %u, nib: %X\n", outDataGetPosition, t);
                aborted = true;
                break;
            }
            wait_us(100);
        }

        if (aborted) {
            ResetACK();
            break;
        }

        // Nibble successfully acknowledged
        ResetACK();
    }

    ResetACK();

    // Reset output data lines to 0 (match MBed cleanup)
    HAL_GPIO_WritePin(out_D_OUT_GPIO_Port, out_D_OUT_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(out_D_IN_GPIO_Port, out_D_IN_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(out_SEL_2_GPIO_Port, out_SEL_2_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(out_SEL_1_GPIO_Port, out_SEL_1_Pin, GPIO_PIN_RESET);

    // Restore input data pins to pull-down mode
    // (matches MBed's in_xxx.mode(PullDown) behavior)
    GPIO_InitStruct.Pin = in_SEL_1_Pin | in_SEL_2_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = in_D_OUT_Pin | in_D_IN_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    irq_BUSY_rise = NULL;
    irq_BUSY_fall = NULL;
    BUSY_IRQ_Enable();

    uint32_t elapsedUs = us_since(startCyc);
    uint32_t nBytes = outDataGetPosition ? outDataGetPosition : 1;
    if (aborted) {
        ERR_PRINTOUT("Send aborted\n");
    }
    debug_log("send complete: %u bytes in %lu us (%lu us/byte)\n",
              outDataGetPosition, elapsedUs, elapsedUs / nBytes);
}

void inNibbleReady(void) {
    uint8_t inNibble = HAL_GPIO_ReadPin(in_SEL_1_GPIO_Port, in_SEL_1_Pin) |
                       (HAL_GPIO_ReadPin(in_SEL_2_GPIO_Port, in_SEL_2_Pin) << 1) |
                       (HAL_GPIO_ReadPin(in_D_OUT_GPIO_Port, in_D_OUT_Pin) << 2) |
                       (HAL_GPIO_ReadPin(in_D_IN_GPIO_Port, in_D_IN_Pin) << 3);

    if (HAL_GPIO_ReadPin(out_ACK_GPIO_Port, out_ACK_Pin) == GPIO_PIN_RESET) {
        wait_us(NIBBLE_DELAY_1);
        SetACK();
        if (highNibbleIn) {
            highNibbleIn = false;
            inDataBuf[inBufPosition] = (inNibble << 4) + inDataBuf[inBufPosition];
            checksum = (inDataBuf[inBufPosition] + checksum) & 0xff;
            debug_log(" %u:0x%02X [%02X]\n", inBufPosition, inDataBuf[inBufPosition], checksum);
            inBufPosition++;

            inDataReadyTimestamp = HAL_GetTick();
            inDataReadyActive = true;
        } else {
            highNibbleIn = true;
            inDataBuf[inBufPosition] = inNibble;
        }
    }
}

void inNibbleAck(void) {
    if (HAL_GPIO_ReadPin(out_ACK_GPIO_Port, out_ACK_Pin) == GPIO_PIN_SET) {
        wait_us(NIBBLE_ACK_DELAY);
        ResetACK();
    }
}

void SendErrorOut(void) {
    outDataBuf[0] = 0xFF;
    outDataPutPosition = 1;
    SendOutputData();
}

void inDataReady(void) {
    uint8_t visualChar = 'c';
    HAL_UART_Transmit(&huart2, &visualChar, 1, HAL_MAX_DELAY);
    debug_log("Processing...\n");

    // Detach BUSY triggers during data processing
    irq_BUSY_rise = NULL;
    irq_BUSY_fall = NULL;

    if (inBufPosition > 0) {
        debug_log("in: %d bytes (first 40 below)\n", inBufPosition);
        debug_hex(inDataBuf, (inBufPosition < 40) ? inBufPosition : 40);

        checksum = 0;
        for (int i = 0; i < inBufPosition - 1; i++) {
            checksum = (inDataBuf[i] + checksum) & 0xff;
        }
        debug_log("checksum 0x%02X vs 0x%02X\n", checksum, inDataBuf[inBufPosition - 1]);
        if (checksum == inDataBuf[inBufPosition - 1]) {
            debug_log("command 0x%02X\n", inDataBuf[0]);
            outDataGetPosition = 0;
            outDataPutPosition = 0;
            highNibbleOut = false;

            ProcessCommand();
            inBufPosition = 0;

            if (outDataPutPosition > 0) {
                debug_log("out: %u bytes (first 40 below)\n", outDataPutPosition);
                debug_hex(outDataBuf, (outDataPutPosition < 40) ? outDataPutPosition : 40);
                visualChar = 'o';
                HAL_UART_Transmit(&huart2, &visualChar, 1, HAL_MAX_DELAY);
                SendOutputData();

                if (skipDeviceCode != 0x00) {
                    visualChar = 'n';
                    HAL_UART_Transmit(&huart2, &visualChar, 1, HAL_MAX_DELAY);
                    debug_log("next: 0x%02X\n", skipDeviceCode);
                    inBufPosition = 0;
                    highNibbleIn = false;
                    checksum = 0;
                    wait_us(NIBBLE_DELAY_2);

                    // Re-attach triggers for skipping device code handshake
                    irq_BUSY_fall = &inNibbleAck;
                    irq_BUSY_rise = &inNibbleReady;
                }
            } else {
                ERR_PRINTOUT("Command processing error\n");
                SendErrorOut();
            }
        } else {
            ERR_PRINTOUT("checksum error\n");
            SendErrorOut();
        }
    }
}
void bitReady(void) {
    if (bitCount >= 8) return; // Ensure safety lock
    uint32_t nTimeout;
    if (HAL_GPIO_ReadPin(out_ACK_GPIO_Port, out_ACK_Pin) == GPIO_PIN_SET) {
        bool bit;
        wait_us(BIT_DELAY_1);
        bit = (HAL_GPIO_ReadPin(in_D_OUT_GPIO_Port, in_D_OUT_Pin) == GPIO_PIN_SET);
        ResetACK();
        deviceCode >>= 1;
        if (bit) deviceCode |= 0x80;
        bitCount++;

        if (bitCount == 8) {
            // Detach BUSY rising edge trigger
            irq_BUSY_rise = NULL;

            // Do NOT block on the UART here: this runs in EXTI ISR context and
            // a blocking transmit stalls the device-code handshake for ~800 us.
            debug_log("Device ID 0x%02X\n", deviceCode);

            if (deviceCode == 0x41) {
                debug_log("CE140F\n");
                inBufPosition = 0;
                highNibbleIn = false;
                checksum = 0;
                skipDeviceCode = 0;

                // Re-register Busy edge callbacks for direct Nibble Handshaking
                irq_BUSY_fall = &inNibbleAck;
                irq_BUSY_rise = &inNibbleReady;

                nTimeout = 10000;
                while ((HAL_GPIO_ReadPin(in_X_OUT_GPIO_Port, in_X_OUT_Pin) == GPIO_PIN_SET ||
                        HAL_GPIO_ReadPin(in_BUSY_GPIO_Port, in_BUSY_Pin) == GPIO_PIN_SET) && (nTimeout--)) {
                    wait_us(100);
                }
                if (nTimeout) {
                    SetACK();
                    wait_us(DATA_WAIT);
                    ResetACK();
                } else {
                    ERR_PRINTOUT("bitReady Timeout!\n\r");
                }
            }
        } else {
            wait_us(BIT_DELAY_2);
            SetACK();
        }
    }
}

void startDeviceCodeSeq(void) {
    uint32_t nTimeout = 100;
    debug_log("startDeviceCodeSeq start\n");
    while ((HAL_GPIO_ReadPin(in_D_OUT_GPIO_Port, in_D_OUT_Pin) == GPIO_PIN_RESET) && (nTimeout--)) {
        wait_us(BIT_DELAY_1);
    }
    wait_us(BIT_DELAY_1);
    debug_log("startDeviceCodeSeq in_D_OUT\n");
    if (HAL_GPIO_ReadPin(in_D_OUT_GPIO_Port, in_D_OUT_Pin) == GPIO_PIN_SET) {
        SetACK();
        bitCount = 0;
        deviceCode = 0;
        inBufPosition = 0;
        debug_log("Device\n");
        wait_us(ACK_DELAY);

        // Bind rise trigger to bitReady and detach fall trigger 
        irq_BUSY_rise = &bitReady;
        irq_BUSY_fall = NULL;

        wait_us(ACK_DELAY);
    }
}

char sio_buf[80];
int sio_pos = 0;
uint8_t rxChar;

void check_serial_input(void) {
    // Non-blocking Poll-based Serial Receiver replacing nested Mbed callbacks.
    // Assembles one CR/LF terminated line and hands it to the pin test console.
    if (HAL_UART_Receive(&huart2, &rxChar, 1, 0) != HAL_OK) return;

    if (rxChar == 0x08 || rxChar == 0x7F) {         // backspace / delete
        if (sio_pos > 0) {
            sio_pos--;
            HAL_UART_Transmit(&huart2, (uint8_t*)"\b \b", 3, HAL_MAX_DELAY);
        }
        return;
    }

    if (rxChar == 0x0D || rxChar == 0x0A) {         // end of line
        HAL_UART_Transmit(&huart2, (uint8_t*)"\r\n", 2, HAL_MAX_DELAY);
        sio_buf[sio_pos] = '\0';
        if (sio_pos > 0 || pintest_is_active()) {
            pintest_process_line(sio_buf);
        }
        sio_pos = 0;
        return;
    }

    if (rxChar >= 0x20 && rxChar < 0x7F && sio_pos < (int)sizeof(sio_buf) - 1) {
        HAL_UART_Transmit(&huart2, &rxChar, 1, HAL_MAX_DELAY);
        sio_buf[sio_pos++] = (char)rxChar;
    }
}

// Background scheduler loops replacing complex interval ticker layers
void run_software_timers(void) {
    uint32_t currentTick = HAL_GetTick();

    // While the interactive pin test console owns the bus, no protocol timer
    // may fire: ResetACK()/inDataReady() would fight the manual pin commands.
    if (pintest_is_active()) {
        debugDumpTimestamp = currentTick;
        return;
    }

    if (ackOffActive && ((currentTick - ackOffTimestamp) >= ACK_TIMEOUT)) {
        ResetACK();
    }

    if (inDataReadyActive && ((currentTick - inDataReadyTimestamp) >= (IN_DATAREADY_TIMEOUT / 1000))) {
        inDataReadyActive = false;
        inDataReady();
    }

    #ifdef DEBUG
    if ((currentTick - debugDumpTimestamp) >= DEBUG_TIMEOUT) {
        debugDumpTimestamp = currentTick;
        outDebugDump();
    }
    #endif
}

// Main operational tracking loop
void app_main(void) {
    uint8_t i = 20;
    DWT_Init();

    uint8_t greet[] = "CE140F emulator init\n";
    HAL_UART_Transmit(&huart2, greet, sizeof(greet)-1, HAL_MAX_DELAY);

    while (i--) {
        HAL_GPIO_TogglePin(infoLed_GPIO_Port, infoLed_Pin);
        HAL_Delay(20);
    }

    inBufPosition = 0;
    #ifdef DEBUG
    // FIXED: Corrected tracking reference boundary clear
    debugBuf[0] = 0x00;
    #endif

    // Set default initial pin outputs safely
    ResetACK();
    HAL_GPIO_WritePin(out_D_OUT_GPIO_Port, out_D_OUT_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(out_D_IN_GPIO_Port, out_D_IN_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(out_SEL_2_GPIO_Port, out_SEL_2_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(out_SEL_1_GPIO_Port, out_SEL_1_Pin, GPIO_PIN_RESET);

    uint8_t readyMsg[] = "ready (type 'test' for the pin test console)\n";
    HAL_UART_Transmit(&huart2, readyMsg, sizeof(readyMsg)-1, HAL_MAX_DELAY);
    debug_log("ready\n");

    debugDumpTimestamp = HAL_GetTick();

    while (1) {
        check_serial_input();
        run_software_timers();
    }
}
