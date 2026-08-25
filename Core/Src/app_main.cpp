// Sharp CE-140F diskette emulator
// Reverse engineering by contact@pockemul.com
// Ported to bare-metal STM32Cube HAL (Nucleo-L432KC)

#include "main.h"
#include "commands.h"
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
#define IN_DATAREADY_TIMEOUT 50000
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
void DWT_Init(void) {
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

void wait_us(uint32_t us) {
    uint32_t startTick = DWT->CYCCNT;
    uint32_t delayTicks = us * (SystemCoreClock / 1000000);
    while ((DWT->CYCCNT - startTick) < delayTicks);
}

uint32_t read_us(void) {
    return DWT->CYCCNT / (SystemCoreClock / 1000000);
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

void debug_log(const char *fmt, ...) {
    uint8_t debugLine[120];
    va_list va;
    debuglock = 1;
    va_start(va, fmt);
    sprintf((char*)debugLine, "%lu ", read_us());
    strcat((char*)debugBuf, (char*)debugLine);
    vsprintf((char*)debugLine, fmt, va);
    strcat((char*)debugBuf, (char*)debugLine);
    va_end(va);
    debuglock = 0;
}

void debug_hex(volatile uint8_t *buf, volatile uint16_t len) {
    int j;
    char tmp[15];
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

void outDebugDump(void) {
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
            bitReady();
            inNibbleReady();
        } else {
            inNibbleAck();
        }
    }
}

void SendOutputData(void) {
    uint8_t t;
    uint32_t nTimeout;
    uint32_t startTime = read_us();

    while (outDataGetPosition < outDataPutPosition) {
        wait_us(OUT_NIBBLE_DELAY);

        nTimeout = 50000;
        while ((HAL_GPIO_ReadPin(in_BUSY_GPIO_Port, in_BUSY_Pin) != GPIO_PIN_RESET) && (nTimeout-- > 0)) {
            wait_us(100);
        }
        if (nTimeout == 0) {
            ERR_PRINTOUT("Send error 1\n");
            ResetACK();
            break;
        }

        if (highNibbleOut) {
            highNibbleOut = false;
            t = (dataOutByte >> 4);
            outDataGetPosition++;
        } else {
            highNibbleOut = true;
            dataOutByte = outDataBuf[outDataGetPosition];
            t = (dataOutByte & 0x0F);
        }

        HAL_GPIO_WritePin(out_SEL_1_GPIO_Port, out_SEL_1_Pin, (t & 0x01) ? GPIO_PIN_SET : GPIO_PIN_RESET);
        HAL_GPIO_WritePin(out_SEL_2_GPIO_Port, out_SEL_2_Pin, (t & 0x02) ? GPIO_PIN_SET : GPIO_PIN_RESET);
        HAL_GPIO_WritePin(out_D_OUT_GPIO_Port, out_D_OUT_Pin, (t & 0x04) ? GPIO_PIN_SET : GPIO_PIN_RESET);
        HAL_GPIO_WritePin(out_D_IN_GPIO_Port, out_D_IN_Pin, (t & 0x08) ? GPIO_PIN_SET : GPIO_PIN_RESET);

        wait_us(OUT_NIBBLE_DELAY);
        SetACK();

        nTimeout = 50000;
        while ((HAL_GPIO_ReadPin(in_BUSY_GPIO_Port, in_BUSY_Pin) == GPIO_PIN_RESET) && (nTimeout-- > 0)) {
            wait_us(100);
        }
        if (nTimeout == 0) {
            ERR_PRINTOUT("Send error 2\n");
            ResetACK();
            break;
        }
        ResetACK();
    }
    uint8_t nl = '\n';
    HAL_UART_Transmit(&huart2, &nl, 1, HAL_MAX_DELAY);
    debug_log("send complete\n");
    debug_log("avg output timing (ms/byte): %.2f\n", (read_us() - startTime) / outDataGetPosition / 1000.0);
}

void inNibbleReady(void) {
    if (HAL_GPIO_ReadPin(in_BUSY_GPIO_Port, in_BUSY_Pin) == GPIO_PIN_RESET) return; // Verify line state matches trace

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
            // FIXED: Instantiated a dedicated character array buffer for safe string compilation
            char outStr[32];
            sprintf(outStr, "d 0x%02X\n", deviceCode);
            HAL_UART_Transmit(&huart2, (uint8_t*)outStr, strlen(outStr), HAL_MAX_DELAY);
            debug_log("Device ID 0x%02X\n", deviceCode);

            if (deviceCode == 0x41) {
                debug_log("CE140F\n");
                inBufPosition = 0;
                highNibbleIn = false;
                checksum = 0;
                skipDeviceCode = 0;

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
        wait_us(ACK_DELAY);
    }
}

char sio_buf[80];
int sio_pos = 0;
uint8_t rxChar;

void check_serial_input(void) {
    // Non-blocking Poll-based Serial Receiver replacing nested Mbed callbacks
    if (HAL_UART_Receive(&huart2, &rxChar, 1, 0) == HAL_OK) {
        if (sio_pos < 80) {
            HAL_UART_Transmit(&huart2, &rxChar, 1, HAL_MAX_DELAY);
            sio_buf[sio_pos] = rxChar;
            sio_pos++;
            if (rxChar == 0x0D) {
                HAL_GPIO_TogglePin(infoLed_GPIO_Port, infoLed_Pin);
                HAL_Delay(20);
                HAL_GPIO_TogglePin(infoLed_GPIO_Port, infoLed_Pin);
                sio_pos = 0;
            }
        }
    }
}

// Background scheduler loops replacing complex interval ticker layers
void run_software_timers(void) {
    uint32_t currentTick = HAL_GetTick();

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

    uint8_t readyMsg[] = "ready\n";
    HAL_UART_Transmit(&huart2, readyMsg, sizeof(readyMsg)-1, HAL_MAX_DELAY);
    debug_log("ready\n");

    debugDumpTimestamp = HAL_GetTick();

    while (1) {
        check_serial_input();
        run_software_timers();
    }
}