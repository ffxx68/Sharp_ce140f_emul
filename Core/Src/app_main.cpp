// Sharp CE-140F diskette emulator
// Reverse engineering by contact@pockemul.com
// Ported to bare-metal STM32Cube HAL (Nucleo-L432KC)

#include "main.h"
#include "commands.h"
#include "pintest.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* Firmware version. Each functional change adds 0.1 */
#define FW_VERSION "4.5"

#define DEBUG 1
/* Per-command detail: hex dumps of every buffer and the average timings. Handy
 * when tracing a problem, but it is ~200 bytes of UART traffic per command that
 * the main loop has to push out between commands - and the Sharp can chain
 * several with no pause at all (SHIFT + down pages through the listing four
 * files at a time). Off by default. */
#define DEBUG_VERBOSE 0
#define DEBUG_SIZE 4096
// Per-byte trace. Costs an snprintf() inside the BUSY interrupt and floods the
// buffer (a 256-byte SAVE block is ~5 KB of text), so keep it off unless a
// byte-level problem is being chased.
#define DEBUG_BYTES 0
#define DEBUG_TIMEOUT 3000

#define NIBBLE_DELAY_1 1000
#define NIBBLE_DELAY_2 1000
#define NIBBLE_ACK_DELAY 100
// The level converter on PCB v1 only drives about 3.76 V into the Sharp's 5 V
// CMOS inputs (issue #4), so ACK sits right at the detection threshold and is
// sometimes missed - the device code then stalls mid-sequence. Holding ACK
// high longer gives the Sharp more chance to sample it. The whole bit cycle
// must still fit inside the ~5 ms the Sharp allows.
/* DO NOT TUNE THESE WITHOUT MEASURING ON HARDWARE.
 *
 * Verified on a PC-E650 with the v1.5 PoyokomaDanna board: these values give 0
 * device-code stalls in 90 sequences and walk a whole FILES listing cleanly.
 * The listing is the sensitive case - it runs one device code sequence per file.
 *
 * Tried and rejected: 1000/2000 (the Mbed values, ~7% stalls) and 2600/1500,
 * which broke navigating the listing. The bit cycle must also stay inside the
 * ~5 ms the service manual allows between BUSY rising and ACK. */
#define BIT_DELAY_1 2000    /* ACK stays high this long after BUSY rises */
#define BIT_DELAY_2 1500    /* and low this long before going high again */
#define ACK_DELAY 20000
#define ACK_TIMEOUT 1000 // In milliseconds for HAL
#define DATA_WAIT 9000
#define IN_DATAREADY_TIMEOUT 50000
#define OUT_NIBBLE_DELAY 500
#define SEND_TIMEOUT_MS 5000 // max wait on a BUSY transition, per nibble

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

extern volatile bool     cmdComplete;
extern volatile uint8_t  skipDeviceCode;

// Volatile function pointers to replicate MBed's dynamic interrupt attach/detach
void (*irq_BUSY_rise)(void) = NULL;
void (*irq_BUSY_fall)(void) = NULL;

// Virtual software timers running via main loop or callback tracking
volatile uint32_t ackOffTimestamp = 0;
volatile bool ackOffActive = false;
uint32_t inDataReadyTimestamp = 0;
bool inDataReadyActive = false;
uint32_t debugDumpTimestamp = 0;
volatile bool debugDumpRequest = false;
uint32_t inStartTime = 0;
volatile bool     busActive = false;
volatile uint32_t busActiveSince = 0;

// Function declarations
void startDeviceCodeSeq(void);
void inDataReady(void);
void inNibbleAck(void);
void inNibbleReady(void);
void bitReady(void);
void bitAck(void);
void outDebugDump(void);
void outDebugDumpAll(void);

// Microsecond delay engine using CPU Cycle Counter (DWT)
static uint32_t cyclesPerUs = 32;
static volatile uint32_t lastCyc = 0;
static volatile uint64_t cycAccum = 0;

void DWT_Init(void) {
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    cyclesPerUs = SystemCoreClock / 1000000u;
    if (cyclesPerUs == 0) cyclesPerUs = 1;
    lastCyc = DWT->CYCCNT;
    cycAccum = 0;
}

void wait_us(uint32_t us) {
    uint32_t startTick = DWT->CYCCNT;
    uint32_t delayTicks = us * cyclesPerUs;
    while ((DWT->CYCCNT - startTick) < delayTicks);
}

// CYCCNT wraps every ~134 s at 32 MHz. The previous read_us() divided the raw
// counter, so timestamps jumped back to zero on every wrap and any
// "elapsed = read_us() - start" check could fire instantly. Keep a 64-bit
// extension instead; it only needs to be polled more often than one wrap,
// which run_software_timers() guarantees.
static uint64_t cycles64(void) {
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    uint32_t now = DWT->CYCCNT;
    cycAccum += (uint32_t)(now - lastCyc);
    lastCyc = now;
    uint64_t res = cycAccum;
    if (!primask) __enable_irq();
    return res;
}

uint32_t read_us(void) {
    return (uint32_t)(cycles64() / cyclesPerUs);
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

// Watchdog on the ACK line staying high (it might lock the Sharp-PC).
// MBed had this on a hardware Timeout, so it also fired while the blocking
// send loop was running - hence it must be serviced from the busy-waits too,
// not only from the main loop.
void service_ack_watchdog(void) {
    if (ackOffActive && ((HAL_GetTick() - ackOffTimestamp) >= ACK_TIMEOUT)) {
        ResetACK();
    }
}

#ifdef DEBUG
SRAM2_DATA volatile uint8_t debugBuf[DEBUG_SIZE];
volatile uint16_t debugLen = 0;
volatile uint16_t debugOut = 0;   /* how much of debugBuf has been sent */
#define DEBUG_CHUNK 32            /* bytes per main-loop pass (~2.8 ms) */

// Appends under a short critical section: debug_log() is called both from the
// EXTI handlers and from the main loop.
static void debug_append(const char *s) {
    uint16_t n = (uint16_t)strlen(s);
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    if ((uint32_t)debugLen + n < DEBUG_SIZE - 1) {
        memcpy((void *)&debugBuf[debugLen], s, n);
        debugLen += n;
        debugBuf[debugLen] = 0x00;
    }
    if (!primask) __enable_irq();
}

// Progress marks used to go straight to the UART, including from inside the
// EXTI handlers. HAL_UART_Transmit() is not reentrant: a call from an interrupt
// while the main loop was mid-transfer returned HAL_BUSY and the text was lost
// (hence the shredded log), and "d 0x41\n" alone blocked the BUSY handler for
// ~700 us at the worst possible moment. Everything goes through the buffer now.
void debug_putc(char c) {
    char b[2] = { c, 0 };
    debug_append(b);
}

void debug_puts(const char *s) {
    debug_append(s);
}

void debug_log(const char *fmt, ...) {
    char debugLine[140];
    int len;
    va_list va;

    len = snprintf(debugLine, sizeof(debugLine), "%lu ", (unsigned long)read_us());
    if (len < 0 || len >= (int)sizeof(debugLine)) return;
    va_start(va, fmt);
    vsnprintf(debugLine + len, sizeof(debugLine) - len, fmt, va);
    va_end(va);
    debug_append(debugLine);
}

void debug_hex(volatile uint8_t *buf, volatile uint16_t len) {
    char tmp[16];
    int j;

    snprintf(tmp, sizeof(tmp), "%lu <", (unsigned long)read_us());
    debug_append(tmp);
    for (j = 0; j < len; j++) {
        snprintf(tmp, sizeof(tmp), "%02X", (unsigned)buf[j]);
        debug_append(tmp);
    }
    debug_append(">\n");
}

// Timings are printed as integers: newlib-nano leaves printf() without float
// support unless the link line adds -u _printf_float, and the old "%.2f"
// silently printed nothing at all (see the empty "avg output timing" line in
// issue #10).
__attribute__((unused))
static void debug_log_avg(const char *what, uint32_t total_us, uint16_t nbytes) {
    if (nbytes == 0) return;
    uint32_t us_per_byte = total_us / nbytes;
    debug_log("avg %s timing (ms/byte): %lu.%02lu\n", what,
              (unsigned long)(us_per_byte / 1000),
              (unsigned long)((us_per_byte % 1000) / 10));
}

// Sends at most DEBUG_CHUNK bytes per call. Dumping the whole buffer in one
// blocking HAL_UART_Transmit() held the main loop for ~180 ms, and inDataReady()
// is dispatched from that same loop - long enough for the Sharp to time out
// mid-command.
void outDebugDump(void) {
    uint16_t n;

    if (debugOut >= debugLen) {
        if (debugLen) {
            uint32_t primask = __get_PRIMASK();
            __disable_irq();
            debugLen = 0;
            debugOut = 0;
            debugBuf[0] = 0x00;
            if (!primask) __enable_irq();
        }
        return;
    }

    n = debugLen - debugOut;
    if (n > DEBUG_CHUNK) n = DEBUG_CHUNK;
    HAL_UART_Transmit(&huart2, (uint8_t *)&debugBuf[debugOut], n, HAL_MAX_DELAY);
    debugOut += n;
}

// Blocking full flush, for the manual (button) dump only
void outDebugDumpAll(void) {
    while (debugOut < debugLen) outDebugDump();
    outDebugDump();
}

void outDebugDumpManual(void) {
    uint8_t i = 20;
    while (i--) {
        HAL_GPIO_TogglePin(infoLed_GPIO_Port, infoLed_Pin);
        HAL_Delay(20);
    }
    outDebugDumpAll();
    ResetACK();
    debug_log("ok\n");
}
#else
void debug_log(const char *fmt, ...) {}
void debug_putc(char c) {}
void debug_puts(const char *s) {}
void debug_hex(volatile uint8_t *buf, volatile uint16_t len) {}
void outDebugDump(void) {}
void outDebugDumpAll(void) {}
#endif

// Shared Callback routing for Edge EXTI pins
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin) {
    if (GPIO_Pin == user_BTN_Pin) {
        #ifdef DEBUG
        // Only raise a flag: the dump blocks for ~200 ms on the UART and must
        // not run inside an interrupt handler.
        debugDumpRequest = true;
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

// Counter for BUSY transitions during send (for debug)
volatile uint16_t busyRiseCount = 0;
volatile uint16_t busyFallCount = 0;

// Debug handlers for BUSY during send - minimal to avoid timing issues.
// The important part is what they do NOT do: while data is being sent, the
// BUSY edges must not touch the ACK line. Routing them to inNibbleReady() /
// inNibbleAck() (as the first port did) let an interrupt clear ACK in the
// middle of the output handshake, which is what broke the last nibble.
void debugBUSY_rise(void) {
    busyRiseCount++;
}

void debugBUSY_fall(void) {
    busyFallCount++;
}

// Software logic analyser: samples the six protocol lines and logs every
// transition. Used to tell "the Sharp sends nothing" apart from "the Sharp
// sends and we do not see it" in the window after a skipDeviceCode reply.
void sniff_lines(uint32_t ms, uint16_t maxEvents) {
    uint32_t start = HAL_GetTick();
    uint8_t last = 0xFF;
    uint16_t events = 0;

    while (((HAL_GetTick() - start) < ms) && (events < maxEvents)) {
        uint8_t now =
            ((in_BUSY_GPIO_Port->IDR  & in_BUSY_Pin)  ? 0x01 : 0) |
            ((in_X_OUT_GPIO_Port->IDR & in_X_OUT_Pin) ? 0x02 : 0) |
            ((in_D_OUT_GPIO_Port->IDR & in_D_OUT_Pin) ? 0x04 : 0) |
            ((in_D_IN_GPIO_Port->IDR  & in_D_IN_Pin)  ? 0x08 : 0) |
            ((in_SEL_1_GPIO_Port->IDR & in_SEL_1_Pin) ? 0x10 : 0) |
            ((in_SEL_2_GPIO_Port->IDR & in_SEL_2_Pin) ? 0x20 : 0) |
            ((out_ACK_GPIO_Port->ODR  & out_ACK_Pin)  ? 0x40 : 0);
        if (now != last) {
            debug_log("L%02X @%lu\n", now, (unsigned long)(HAL_GetTick() - start));
            last = now;
            events++;
        }
    }
    debug_log("sniff done: %u events in %lu ms\n", events,
              (unsigned long)(HAL_GetTick() - start));
}

void SendOutputData(void) {
    uint8_t t = 0;
    #if DEBUG_VERBOSE
    uint32_t startTime = read_us();
    #endif
    uint32_t startWait;

    // Reset BUSY counters
    busyRiseCount = 0;
    busyFallCount = 0;

    // Attach minimal BUSY triggers during send
    irq_BUSY_rise = &debugBUSY_rise;
    irq_BUSY_fall = &debugBUSY_fall;

    // Set input data pins to high impedance (no pull) during output
    // This prevents pull resistors from interfering with the level converter
    // (matches MBed's in_xxx.mode(PullNone) behavior)
    GPIO_InitTypeDef GPIO_InitStruct = {0};

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

        // Wait for BUSY to go DOWN (direct register read for speed)
        bool timedout1 = false;
        startWait = HAL_GetTick();
        while ((in_BUSY_GPIO_Port->IDR & in_BUSY_Pin) != 0) {
            service_ack_watchdog();
    loadWatchdogService();
            if ((HAL_GetTick() - startWait) > SEND_TIMEOUT_MS) {
                timedout1 = true;
                break;
            }
            wait_us(100);
        }
        if (timedout1) {
            ERR_PRINTOUT("Send error 1\n");
            debug_log("SO Err1 pos: %u\n", outDataGetPosition);
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

        // Set data on output lines
        HAL_GPIO_WritePin(out_SEL_1_GPIO_Port, out_SEL_1_Pin, (t & 0x01) ? GPIO_PIN_SET : GPIO_PIN_RESET);
        HAL_GPIO_WritePin(out_SEL_2_GPIO_Port, out_SEL_2_Pin, (t & 0x02) ? GPIO_PIN_SET : GPIO_PIN_RESET);
        HAL_GPIO_WritePin(out_D_OUT_GPIO_Port, out_D_OUT_Pin, (t & 0x04) ? GPIO_PIN_SET : GPIO_PIN_RESET);
        HAL_GPIO_WritePin(out_D_IN_GPIO_Port, out_D_IN_Pin, (t & 0x08) ? GPIO_PIN_SET : GPIO_PIN_RESET);

        // Nibble is ready for Sharp-PC to get it
        wait_us(OUT_NIBBLE_DELAY);

        SetACK();

        // Wait for BUSY to go UP.
        // Sampled every 100 us, as Mbed did. Polling this flat out and dropping
        // ACK the instant BUSY rises is too quick for the Sharp: it lowers BUSY
        // only after seeing ACK low, and if the pulse is gone before it samples
        // the line it waits out its own timeout (~214 ms measured) and then
        // abandons the transfer.
        bool timedout2 = false;
        startWait = HAL_GetTick();
        while ((in_BUSY_GPIO_Port->IDR & in_BUSY_Pin) == 0) {
            service_ack_watchdog();
            if ((HAL_GetTick() - startWait) > SEND_TIMEOUT_MS) {
                timedout2 = true;
                break;
            }
            wait_us(100);
        }
        if (timedout2) {
            ERR_PRINTOUT("Send error 2\n");
            debug_log("SO Err2 pos: %u, nib: %X, rise:%u fall:%u\n",
                      outDataGetPosition, t, busyRiseCount, busyFallCount);
            ResetACK();
            break;
        }
        // Nibble successfully acknowledged
        ResetACK();
    }

    // Reset output data lines to 0 (match MBed cleanup)
    HAL_GPIO_WritePin(out_D_OUT_GPIO_Port, out_D_OUT_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(out_D_IN_GPIO_Port, out_D_IN_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(out_SEL_2_GPIO_Port, out_SEL_2_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(out_SEL_1_GPIO_Port, out_SEL_1_Pin, GPIO_PIN_RESET);

    // Restore input data pins to pull-down mode
    // (matches MBed's in_xxx.mode(PullDown) behavior)
    GPIO_InitStruct = {0};

    // in_SEL_1 = PB1, in_SEL_2 = PB6 (on GPIOB)
    GPIO_InitStruct.Pin = in_SEL_1_Pin | in_SEL_2_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    // in_D_OUT = PA10, in_D_IN = PA1 (on GPIOA)
    GPIO_InitStruct.Pin = in_D_OUT_Pin | in_D_IN_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    // Detach debug BUSY handlers
    irq_BUSY_rise = NULL;
    irq_BUSY_fall = NULL;

    debug_putc('\n');
    #if DEBUG_VERBOSE
    debug_log("send complete, BUSY rise:%u fall:%u\n", busyRiseCount, busyFallCount);
    #endif
    #if DEBUG_VERBOSE
    debug_log_avg("output", read_us() - startTime, outDataGetPosition);
    #endif
}

static inline uint8_t read_nibble(void) {
    return (uint8_t)(HAL_GPIO_ReadPin(in_SEL_1_GPIO_Port, in_SEL_1_Pin) |
                    (HAL_GPIO_ReadPin(in_SEL_2_GPIO_Port, in_SEL_2_Pin) << 1) |
                    (HAL_GPIO_ReadPin(in_D_OUT_GPIO_Port, in_D_OUT_Pin) << 2) |
                    (HAL_GPIO_ReadPin(in_D_IN_GPIO_Port, in_D_IN_Pin) << 3));
}

void inNibbleReady(void) {
    if (HAL_GPIO_ReadPin(in_BUSY_GPIO_Port, in_BUSY_Pin) == GPIO_PIN_RESET) return; // Verify line state matches trace

    if (HAL_GPIO_ReadPin(out_ACK_GPIO_Port, out_ACK_Pin) == GPIO_PIN_RESET) {
        // Sample three times across the settling window and take a bitwise
        // majority, instead of one read at the instant of the edge. A single
        // wrong bit corrupts the byte and fails the command's checksum, and a
        // binary SAVE block is 512 nibbles - by far the most exposed operation.
        // Same total delay as before; only where the reads happen changes.
        uint8_t s1, s2, s3;
        wait_us(NIBBLE_DELAY_1 / 3);
        s1 = read_nibble();
        wait_us(NIBBLE_DELAY_1 / 3);
        s2 = read_nibble();
        wait_us(NIBBLE_DELAY_1 / 3);
        s3 = read_nibble();
        uint8_t inNibble = (s1 & s2) | (s2 & s3) | (s1 & s3);

        SetACK();
        if (highNibbleIn) {
            highNibbleIn = false;
            inDataBuf[inBufPosition] = (inNibble << 4) + inDataBuf[inBufPosition];
            checksum = (inDataBuf[inBufPosition] + checksum) & 0xff;
            #if DEBUG_BYTES
            debug_log(" %u:0x%02X [%02X]\n", inBufPosition, inDataBuf[inBufPosition], checksum);
            #endif
            if (inBufPosition < IN_BUF_SIZE - 1) inBufPosition++;

            inDataReadyTimestamp = HAL_GetTick();
            inDataReadyActive = true;
        } else {
            if (inBufPosition == 0) inStartTime = read_us();
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
    debug_putc('c');

    // Detach BUSY triggers during data processing
    irq_BUSY_rise = NULL;
    irq_BUSY_fall = NULL;

    if (inBufPosition > 0) {
        #if DEBUG_VERBOSE
        debug_log("in: %d bytes (first 40 below)\n", inBufPosition);
        debug_hex(inDataBuf, (inBufPosition < 40) ? inBufPosition : 40);
        #endif
        #if DEBUG_VERBOSE
        debug_log_avg("input", (read_us() - inStartTime) - IN_DATAREADY_TIMEOUT,
                      inBufPosition);
        #endif

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
                #if DEBUG_VERBOSE
                debug_log("out: %u bytes (first 40 below)\n", outDataPutPosition);
                debug_hex(outDataBuf, (outDataPutPosition < 40) ? outDataPutPosition : 40);
                #endif
                debug_putc('o');
                SendOutputData();

                if (skipDeviceCode != 0x00) {
                    // Re-attach first, log afterwards: the Sharp can start
                    // clocking the next block as soon as our last nibble was
                    // acknowledged, and formatting a debug line takes long
                    // enough to miss the first edges.
                    inBufPosition = 0;
                    highNibbleIn = false;
                    checksum = 0;
                    wait_us(NIBBLE_DELAY_2);

                    irq_BUSY_fall = &inNibbleAck;
                    irq_BUSY_rise = &inNibbleReady;

                    // The Sharp raises BUSY for the first nibble of the next
                    // block while we are still tearing the send down, so the
                    // rising edge is already gone by the time the handler is
                    // attached and we would wait forever. Bus trace showed
                    // exactly that: BUSY high with a nibble on the data lines,
                    // held for 214 ms until the Sharp gave up. Sample the level
                    // once and pick the nibble up if it is already there.
                    if ((in_BUSY_GPIO_Port->IDR & in_BUSY_Pin) != 0) {
                        debug_log("BUSY already high, taking nibble\n");
                        inNibbleReady();
                    }

                    debug_putc('n');
                    debug_log("next: 0x%02X\n", skipDeviceCode);
                }
            } else {
                ERR_PRINTOUT("Command processing error\n");
                SendErrorOut();
            }
        } else {
            ERR_PRINTOUT("checksum error\n");
            // Abandon the transfer cleanly. Leaving skipDeviceCode armed after
            // a bad block leaves the Sharp waiting for a continuation that will
            // never make sense, which is the hang that needs BREAK to clear.
            skipDeviceCode = 0x00;
            SendErrorOut();
        }
    }
    if (skipDeviceCode == 0x00) busActive = false;
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
                        HAL_GPIO_ReadPin(in_BUSY_GPIO_Port, in_BUSY_Pin) == GPIO_PIN_SET) && nTimeout > 0) {
                    nTimeout--;
                    wait_us(100);
                }
                if (nTimeout > 0) {
                    SetACK();
                    wait_us(DATA_WAIT);
                    ResetACK();
                    // and the first nibble may already be waiting on the bus
                    // (inNibbleReady() does nothing unless ACK is low)
                    if ((in_BUSY_GPIO_Port->IDR & in_BUSY_Pin) != 0) {
                        inNibbleReady();
                    }
                } else {
                    ERR_PRINTOUT("bitReady Timeout!\n\r");
                }
            }
        }
        /* ACK is NOT re-asserted here. The Sharp lowers BUSY once it has seen
         * ACK go low, and only then is it ready for the next bit - so bitAck()
         * raises ACK on that falling edge. Waiting a fixed BIT_DELAY_2 and
         * guessing made the whole sequence hostage to that constant, which is
         * why re-tuning it kept trading one failure for another. The nibble
         * path has always been event driven this way, and it is the part that
         * works. */
    }
}

// BUSY falling edge during the device code: the Sharp has taken our ACK low
// acknowledgement and is ready for the next bit.
void bitAck(void) {
    if (bitCount >= 8) return;
    if (HAL_GPIO_ReadPin(out_ACK_GPIO_Port, out_ACK_Pin) == GPIO_PIN_RESET) {
        SetACK();
    }
}

void startDeviceCodeSeq(void) {
    uint32_t nTimeout = 100;
    inDataReadyActive = false;
    // Minimal marker only: this runs in interrupt context, and anything longer
    // delays the bit handshake the Sharp is timing. "dcN" means the previous
    // device code stalled after N bits.
    if (bitCount > 0 && bitCount < 8) {
        char m[8];
        m[0] = 'd'; m[1] = 'c'; m[2] = (char)('0' + bitCount);
        m[3] = '\n'; m[4] = 0;
        debug_append(m);
    }
    busActive = true;
    busActiveSince = HAL_GetTick();
    while ((HAL_GPIO_ReadPin(in_D_OUT_GPIO_Port, in_D_OUT_Pin) == GPIO_PIN_RESET) && (nTimeout--)) {
        wait_us(BIT_DELAY_1);
    }
    wait_us(BIT_DELAY_1);
    if (HAL_GPIO_ReadPin(in_D_OUT_GPIO_Port, in_D_OUT_Pin) == GPIO_PIN_SET) {
        bitCount = 0;
        deviceCode = 0;
        inBufPosition = 0;

        // Bind the handler before raising ACK, not 20 ms after it: the Sharp
        // starts clocking bits as soon as it sees ACK go high.
        irq_BUSY_rise = &bitReady;
        irq_BUSY_fall = &bitAck;

        SetACK();

        // All EXTI lines share one priority (as on Mbed), so bitReady() cannot
        // pre-empt this handler: the first BUSY edge sits here as a pending
        // interrupt and is only served once we return. The Sharp allows roughly
        // 5 ms for the ACK, so return promptly instead of sitting out two
        // 20 ms ACK_DELAY waits.
        //
        // Do NOT poll the BUSY level here either: bitReady() leaves ACK high
        // while waiting for the next bit and the Sharp holds BUSY high until it
        // has seen ACK go low, so a level check finds both high and clocks in a
        // bit that was never sent.
        wait_us(BIT_DELAY_1);
    }
}

char sio_buf[80];
int sio_pos = 0;
uint8_t rxChar;

void check_serial_input(void) {
    // Non-blocking Poll-based Serial Receiver replacing nested Mbed callbacks
	// Assembles one CR/LF terminated line and hands it to the pin test console
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

    // keeps the 64-bit cycle counter fed (must be polled at least once per
    // CYCCNT wrap, i.e. every ~134 s at 32 MHz)
    (void)read_us();

    service_ack_watchdog();

    if (inDataReadyActive && ((currentTick - inDataReadyTimestamp) >= (IN_DATAREADY_TIMEOUT / 1000))) {
        // Never close a command with ACK still asserted: a lost BUSY falling
        // edge leaves it high, the Sharp waits for it to drop, and the line
        // arrives truncated. Only while nibble handshaking is armed - during a
        // device code sequence ACK is held high on purpose and clearing it
        // would kill the bit handshake.
        if ((irq_BUSY_rise == &inNibbleReady)
         && (HAL_GPIO_ReadPin(out_ACK_GPIO_Port, out_ACK_Pin) == GPIO_PIN_SET)) {
            debug_log("R n%u\n", inBufPosition);
            ResetACK();
            if ((in_BUSY_GPIO_Port->IDR & in_BUSY_Pin) != 0) {
                inNibbleReady();
            }
            inDataReadyTimestamp = HAL_GetTick();
        } else {
            inDataReadyActive = false;
            inDataReady();
        }
    }

    #ifdef DEBUG
    if (debugDumpRequest) {
        debugDumpRequest = false;
        outDebugDumpManual();
    }
    // Never dump while the Sharp is mid-command: inDataReady() is dispatched
    // from this same loop and must not be kept waiting.
    else {
        // Drain continuously in small chunks. Gating this on "no command in
        // flight" starved the buffer during a burst of commands: it filled up
        // and debug_append() started dropping entries, which is what shredded
        // the captured logs. 32 bytes is 2.8 ms on the wire, far below the
        // Sharp's own timeout.
        outDebugDump();
    }
    #endif
}

// Main operational tracking loop
void app_main(void) {
    uint8_t i = 20;
    DWT_Init();

    uint8_t greet[] = "CE140F emulator v" FW_VERSION " init\n";
    HAL_UART_Transmit(&huart2, greet, sizeof(greet)-1, HAL_MAX_DELAY);

    while (i--) {
        HAL_GPIO_TogglePin(infoLed_GPIO_Port, infoLed_Pin);
        HAL_Delay(20);
    }

    inBufPosition = 0;
    commands_init();   // .sram2 is not cleared at reset
    #ifdef DEBUG
    debugBuf[0] = 0x00;
    debugLen = 0;
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
