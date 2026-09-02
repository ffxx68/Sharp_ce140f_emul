// Sharp CE-140F emulator - interactive pin test console
// See pintest.h for the rationale.

#include "pintest.h"
#include "commands.h"
#include "ff.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>

extern UART_HandleTypeDef huart2;

// Provided by app_main.cpp
extern void wait_us(uint32_t us);
extern uint32_t read_us(void);
extern void (*irq_BUSY_rise)(void);
extern void (*irq_BUSY_fall)(void);

// ---------------------------------------------------------------------------
// Pin table
// ---------------------------------------------------------------------------

typedef struct {
    const char   *name;     // canonical name typed by the user
    GPIO_TypeDef *port;
    uint16_t      pin;
    bool          isOutput; // true = we drive it, false = the Sharp PC drives it
} PinDef;

// Names are matched case-insensitively. Keep the outputs first so "pins"
// prints them grouped.
static const PinDef pinTable[] = {
    // --- outputs (emulator -> Sharp PC, via the level converter) ---
    { "ack",    out_ACK_GPIO_Port,   out_ACK_Pin,   true  },
    { "dout",   out_D_OUT_GPIO_Port, out_D_OUT_Pin, true  },
    { "din",    out_D_IN_GPIO_Port,  out_D_IN_Pin,  true  },
    { "sel1",   out_SEL_1_GPIO_Port, out_SEL_1_Pin, true  },
    { "sel2",   out_SEL_2_GPIO_Port, out_SEL_2_Pin, true  },
    { "led",    infoLed_GPIO_Port,   infoLed_Pin,   true  },
    // --- inputs (Sharp PC -> emulator) ---
    { "ibusy",  in_BUSY_GPIO_Port,   in_BUSY_Pin,   false },
    { "ixout",  in_X_OUT_GPIO_Port,  in_X_OUT_Pin,  false },
    { "idout",  in_D_OUT_GPIO_Port,  in_D_OUT_Pin,  false },
    { "idin",   in_D_IN_GPIO_Port,   in_D_IN_Pin,   false },
    { "isel1",  in_SEL_1_GPIO_Port,  in_SEL_1_Pin,  false },
    { "isel2",  in_SEL_2_GPIO_Port,  in_SEL_2_Pin,  false },
};

#define PIN_COUNT ((int)(sizeof(pinTable) / sizeof(pinTable[0])))

// The four data lines, LSB first, as used by the protocol nibble encoding.
static const int nibbleOutIdx[4] = { 3, 4, 1, 2 }; // sel1, sel2, dout, din
static const int nibbleInIdx[4]  = { 10, 11, 8, 9 }; // isel1, isel2, idout, idin

static bool testActive = false;

// ---------------------------------------------------------------------------
// Console helpers
// ---------------------------------------------------------------------------

static void tp_print(const char *fmt, ...) {
    char buf[128];
    va_list va;
    va_start(va, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    HAL_UART_Transmit(&huart2, (uint8_t *)buf, n, HAL_MAX_DELAY);
}

static int tp_strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        int ca = tolower((unsigned char)*a++);
        int cb = tolower((unsigned char)*b++);
        if (ca != cb) return ca - cb;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

// Resolve a pin name. Accepts the canonical names above plus a few aliases.
static const PinDef *findPin(const char *name) {
    if (name == NULL) return NULL;

    // Aliases -> canonical, to be forgiving about the naming used in the
    // schematic vs. the firmware.
    static const struct { const char *alias; const char *canon; } aliases[] = {
        { "out_ack",   "ack"   }, { "o_ack",  "ack"   },
        { "out_dout",  "dout"  }, { "o_dout", "dout"  }, { "d_out", "dout" },
        { "out_din",   "din"   }, { "o_din",  "din"   }, { "d_in",  "din"  },
        { "out_sel1",  "sel1"  }, { "o_sel1", "sel1"  }, { "sel_1", "sel1" },
        { "out_sel2",  "sel2"  }, { "o_sel2", "sel2"  }, { "sel_2", "sel2" },
        { "infoled",   "led"   },
        { "busy",      "ibusy" }, { "in_busy",  "ibusy" },
        { "xout",      "ixout" }, { "in_xout",  "ixout" }, { "x_out", "ixout" },
        { "in_dout",   "idout" }, { "in_din",   "idin"  },
        { "in_sel1",   "isel1" }, { "in_sel2",  "isel2" },
    };
    for (int i = 0; i < (int)(sizeof(aliases) / sizeof(aliases[0])); i++) {
        if (tp_strcasecmp(name, aliases[i].alias) == 0) {
            name = aliases[i].canon;
            break;
        }
    }

    for (int i = 0; i < PIN_COUNT; i++) {
        if (tp_strcasecmp(name, pinTable[i].name) == 0) return &pinTable[i];
    }
    return NULL;
}

static int pinRead(const PinDef *p) {
    return (p->port->IDR & p->pin) ? 1 : 0;
}

// For outputs report what we are driving (ODR), which is what matters when
// checking the level converter; IDR of a push-pull output follows the pad.
static int pinLevel(const PinDef *p) {
    if (p->isOutput) return (p->port->ODR & p->pin) ? 1 : 0;
    return pinRead(p);
}

static void pinWrite(const PinDef *p, int level) {
    HAL_GPIO_WritePin(p->port, p->pin, level ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static char portLetter(GPIO_TypeDef *port) {
    if (port == GPIOA) return 'A';
    if (port == GPIOB) return 'B';
    if (port == GPIOC) return 'C';
    return '?';
}

static int pinNumber(uint16_t pinMask) {
    for (int i = 0; i < 16; i++) {
        if (pinMask & (1u << i)) return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Bus ownership
// ---------------------------------------------------------------------------

// Mask every protocol interrupt so the state machine cannot touch the bus
// while we are poking at it by hand.
static void suspendProtocol(void) {
    HAL_NVIC_DisableIRQ(EXTI0_IRQn);    // in_X_OUT
    HAL_NVIC_DisableIRQ(EXTI4_IRQn);    // user_BTN
    HAL_NVIC_DisableIRQ(EXTI9_5_IRQn);  // in_BUSY
    irq_BUSY_rise = NULL;
    irq_BUSY_fall = NULL;
}

static void resumeProtocol(void) {
    irq_BUSY_rise = NULL;
    irq_BUSY_fall = NULL;
    __HAL_GPIO_EXTI_CLEAR_IT(in_X_OUT_Pin);
    __HAL_GPIO_EXTI_CLEAR_IT(user_BTN_Pin);
    __HAL_GPIO_EXTI_CLEAR_IT(in_BUSY_Pin);
    HAL_NVIC_ClearPendingIRQ(EXTI0_IRQn);
    HAL_NVIC_ClearPendingIRQ(EXTI4_IRQn);
    HAL_NVIC_ClearPendingIRQ(EXTI9_5_IRQn);
    HAL_NVIC_EnableIRQ(EXTI0_IRQn);
    HAL_NVIC_EnableIRQ(EXTI4_IRQn);
    HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);
}

static void allOutputsLow(void) {
    for (int i = 0; i < PIN_COUNT; i++) {
        if (pinTable[i].isOutput) pinWrite(&pinTable[i], 0);
    }
}

// Input data lines: PullDown is the normal protocol setting, PullNone is what
// SendOutputData() switches to while the emulator drives the bus. Being able
// to flip this by hand is essential when chasing level-converter problems.
static void setInputPull(uint32_t pull) {
    GPIO_InitTypeDef gi = {0};

    gi.Pin  = in_SEL_1_Pin | in_SEL_2_Pin;
    gi.Mode = GPIO_MODE_INPUT;
    gi.Pull = pull;
    HAL_GPIO_Init(GPIOB, &gi);

    gi.Pin  = in_D_OUT_Pin | in_D_IN_Pin;
    gi.Mode = GPIO_MODE_INPUT;
    gi.Pull = pull;
    HAL_GPIO_Init(GPIOA, &gi);
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

static void printPins(void) {
    tp_print("     name    port  lvl\r\n");
    for (int i = 0; i < PIN_COUNT; i++) {
        const PinDef *p = &pinTable[i];
        tp_print("  %-3s %-6s P%c%-2d  %d\r\n",
                 p->isOutput ? "out" : "in",
                 p->name,
                 portLetter(p->port), pinNumber(p->pin),
                 pinLevel(p));
    }

    uint8_t outNib = 0, inNib = 0;
    for (int b = 0; b < 4; b++) {
        if (pinLevel(&pinTable[nibbleOutIdx[b]])) outNib |= (1u << b);
        if (pinLevel(&pinTable[nibbleInIdx[b]]))  inNib  |= (1u << b);
    }
    tp_print("  out nibble=%X  in nibble=%X\r\n", outNib, inNib);
}

static void printHelp(void) {
    tp_print("\r\n-- CE140F pin test console --\r\n");
    tp_print("  pins             list all pins and their level\r\n");
    tp_print("  set <pin>        drive an output high\r\n");
    tp_print("  clr <pin>        drive an output low\r\n");
    tp_print("  tog <pin>        toggle an output\r\n");
    tp_print("  get <pin>        read one pin\r\n");
    tp_print("  nib <0-F>        drive sel1/sel2/dout/din as a nibble\r\n");
    tp_print("  pulse <pin> <us> pulse an output (inverted, then restored)\r\n");
    tp_print("  wait <pin> <lvl> [ms]  wait for an input level, report us\r\n");
    tp_print("  mon [ms]         watch inputs, count edges (default 2000)\r\n");
    tp_print("  pull down|none   input data lines pull mode\r\n");
    tp_print("  sd               test SD card mount, init and directory listing\r\n");
    tp_print("  low              drive every output low\r\n");
    tp_print("  help             this text\r\n");
    tp_print("  exit             leave test mode, resume protocol\r\n");
    tp_print("pins: ack dout din sel1 sel2 led | ibusy ixout idout idin isel1 isel2\r\n");
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

// Poll an input until it reaches `level` or the timeout expires.
static void cmdWait(const PinDef *p, int level, uint32_t timeoutMs) {
    uint32_t startTick = HAL_GetTick();
    uint32_t startUs   = read_us();
    while (pinRead(p) != level) {
        if ((HAL_GetTick() - startTick) >= timeoutMs) {
            tp_print("wait %s=%d TIMEOUT after %lu ms (now %d)\r\n",
                     p->name, level, (unsigned long)timeoutMs, pinRead(p));
            return;
        }
    }
    tp_print("wait %s=%d ok after %lu us\r\n",
             p->name, level, (unsigned long)(read_us() - startUs));
}

// Sample every input line and count transitions, so a hand-run program on the
// Sharp PC can be observed without a logic analyser.
static void cmdMonitor(uint32_t durationMs) {
    int prev[PIN_COUNT];
    uint16_t rise[PIN_COUNT];
    uint16_t fall[PIN_COUNT];

    for (int i = 0; i < PIN_COUNT; i++) {
        prev[i] = pinTable[i].isOutput ? 0 : pinRead(&pinTable[i]);
        rise[i] = 0;
        fall[i] = 0;
    }

    tp_print("monitoring inputs for %lu ms...\r\n", (unsigned long)durationMs);

    uint32_t startTick = HAL_GetTick();
    while ((HAL_GetTick() - startTick) < durationMs) {
        for (int i = 0; i < PIN_COUNT; i++) {
            if (pinTable[i].isOutput) continue;
            int now = pinRead(&pinTable[i]);
            if (now != prev[i]) {
                if (now) { if (rise[i] < 0xFFFF) rise[i]++; }
                else     { if (fall[i] < 0xFFFF) fall[i]++; }
                prev[i] = now;
            }
        }
    }

    for (int i = 0; i < PIN_COUNT; i++) {
        if (pinTable[i].isOutput) continue;
        tp_print("  %-6s rise=%u fall=%u now=%d\r\n",
                 pinTable[i].name, rise[i], fall[i], prev[i]);
    }
}

// Parse a level argument: 0/1, low/high, l/h.
static int parseLevel(const char *s, int *ok) {
    *ok = 1;
    if (s == NULL) { *ok = 0; return 0; }
    if (tp_strcasecmp(s, "1") == 0 || tp_strcasecmp(s, "high") == 0 ||
        tp_strcasecmp(s, "h") == 0) return 1;
    if (tp_strcasecmp(s, "0") == 0 || tp_strcasecmp(s, "low") == 0 ||
        tp_strcasecmp(s, "l") == 0) return 0;
    *ok = 0;
    return 0;
}

static bool requireOutput(const PinDef *p, const char *what) {
    if (p == NULL) {
        tp_print("unknown pin (try 'pins')\r\n");
        return false;
    }
    if (!p->isOutput) {
        tp_print("%s: '%s' is an input, cannot %s it\r\n", what, p->name, what);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Line dispatch
// ---------------------------------------------------------------------------

bool pintest_is_active(void) {
    return testActive;
}

bool pintest_process_line(const char *line) {
    char buf[80];
    char *tok[5] = {0};
    int ntok = 0;

    if (line == NULL) return false;

    // Trim leading blanks and copy into a mutable buffer for tokenising.
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0') {
        // A bare Enter in test mode just re-prints the prompt.
        if (testActive) tp_print("test> ");
        return testActive;
    }
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    for (char *t = strtok(buf, " \t"); t != NULL && ntok < 5; t = strtok(NULL, " \t")) {
        tok[ntok++] = t;
    }
    if (ntok == 0) return testActive;

    const char *cmd = tok[0];

    // --- entry point, recognised even when the console is not active ---
    if (!testActive) {
        if (tp_strcasecmp(cmd, "test") == 0) {
            testActive = true;
            suspendProtocol();
            allOutputsLow();
            tp_print("\r\nprotocol SUSPENDED - bus is yours\r\n");
            printHelp();
            printPins();
            tp_print("test> ");
            return true;
        }
        return false; // not ours
    }

    // --- from here on the test console owns every line ---
    if (tp_strcasecmp(cmd, "exit") == 0 || tp_strcasecmp(cmd, "quit") == 0 ||
        tp_strcasecmp(cmd, "end") == 0) {
        allOutputsLow();
        setInputPull(GPIO_PULLDOWN);
        resumeProtocol();
        testActive = false;
        tp_print("\r\nprotocol RESUMED\r\n");
        return true;
    }
    else if (tp_strcasecmp(cmd, "help") == 0 || tp_strcasecmp(cmd, "?") == 0) {
        printHelp();
    }
    else if (tp_strcasecmp(cmd, "pins") == 0 || tp_strcasecmp(cmd, "p") == 0) {
        printPins();
    }
    else if (tp_strcasecmp(cmd, "set") == 0) {
        const PinDef *p = findPin(tok[1]);
        if (requireOutput(p, "set")) {
            pinWrite(p, 1);
            tp_print("%s = 1\r\n", p->name);
        }
    }
    else if (tp_strcasecmp(cmd, "clr") == 0 || tp_strcasecmp(cmd, "reset") == 0) {
        const PinDef *p = findPin(tok[1]);
        if (requireOutput(p, "clr")) {
            pinWrite(p, 0);
            tp_print("%s = 0\r\n", p->name);
        }
    }
    else if (tp_strcasecmp(cmd, "tog") == 0 || tp_strcasecmp(cmd, "toggle") == 0) {
        const PinDef *p = findPin(tok[1]);
        if (requireOutput(p, "toggle")) {
            HAL_GPIO_TogglePin(p->port, p->pin);
            tp_print("%s = %d\r\n", p->name, pinLevel(p));
        }
    }
    else if (tp_strcasecmp(cmd, "get") == 0 || tp_strcasecmp(cmd, "read") == 0) {
        const PinDef *p = findPin(tok[1]);
        if (p == NULL) tp_print("unknown pin (try 'pins')\r\n");
        else tp_print("%s = %d\r\n", p->name, pinLevel(p));
    }
    else if (tp_strcasecmp(cmd, "nib") == 0 || tp_strcasecmp(cmd, "nibble") == 0) {
        if (tok[1] == NULL) {
            tp_print("usage: nib <0-F>\r\n");
        } else {
            unsigned v = (unsigned)strtoul(tok[1], NULL, 16) & 0x0F;
            for (int b = 0; b < 4; b++) {
                pinWrite(&pinTable[nibbleOutIdx[b]], (v >> b) & 1);
            }
            tp_print("nibble %X -> sel1=%u sel2=%u dout=%u din=%u\r\n",
                     v, v & 1, (v >> 1) & 1, (v >> 2) & 1, (v >> 3) & 1);
        }
    }
    else if (tp_strcasecmp(cmd, "pulse") == 0) {
        const PinDef *p = findPin(tok[1]);
        if (requireOutput(p, "pulse")) {
            uint32_t us = (tok[2] != NULL) ? (uint32_t)strtoul(tok[2], NULL, 10) : 1000;
            if (us > 1000000UL) us = 1000000UL;
            int start = pinLevel(p);
            pinWrite(p, !start);
            wait_us(us);
            pinWrite(p, start);
            tp_print("pulsed %s to %d for %lu us\r\n",
                     p->name, !start, (unsigned long)us);
        }
    }
    else if (tp_strcasecmp(cmd, "wait") == 0) {
        const PinDef *p = findPin(tok[1]);
        int ok = 0;
        int lvl = parseLevel(tok[2], &ok);
        if (p == NULL) {
            tp_print("unknown pin (try 'pins')\r\n");
        } else if (!ok) {
            tp_print("usage: wait <pin> <0|1> [ms]\r\n");
        } else {
            uint32_t ms = (tok[3] != NULL) ? (uint32_t)strtoul(tok[3], NULL, 10) : 3000;
            cmdWait(p, lvl, ms);
        }
    }
    else if (tp_strcasecmp(cmd, "mon") == 0 || tp_strcasecmp(cmd, "monitor") == 0) {
        uint32_t ms = (tok[1] != NULL) ? (uint32_t)strtoul(tok[1], NULL, 10) : 2000;
        if (ms > 60000UL) ms = 60000UL;
        cmdMonitor(ms);
    }
    else if (tp_strcasecmp(cmd, "pull") == 0) {
        if (tok[1] == NULL) {
            tp_print("usage: pull down|none\r\n");
        } else if (tp_strcasecmp(tok[1], "down") == 0) {
            setInputPull(GPIO_PULLDOWN);
            tp_print("input data lines: PullDown\r\n");
        } else if (tp_strcasecmp(tok[1], "none") == 0) {
            setInputPull(GPIO_NOPULL);
            tp_print("input data lines: PullNone\r\n");
        } else {
            tp_print("usage: pull down|none\r\n");
        }
    }
    else if (tp_strcasecmp(cmd, "sd") == 0 || tp_strcasecmp(cmd, "sdtest") == 0) {
        tp_print("Testing SD card SPI initialization...\r\n");
        extern volatile uint8_t debugBuf[];
        debugBuf[0] = 0x00;
        extern FATFS FatFs;
        FRESULT fr = f_mount(&FatFs, "0:", 1);
        tp_print("f_mount result: %d (0=FR_OK, 1=FR_DISK_ERR, 3=FR_NOT_READY)\r\n", (int)fr);
        if (debugBuf[0] != 0x00) {
            tp_print("SPI log:\r\n%s\r\n", (char*)debugBuf);
            debugBuf[0] = 0x00;
        }
        if (fr == FR_OK) {
            DIR dir;
            FILINFO fno;
            if (f_opendir(&dir, "0:/") == FR_OK) {
                tp_print("Files on SD card (0:/):\r\n");
                int count = 0;
                while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
                    tp_print("  %s (%lu bytes)\r\n", fno.fname, (unsigned long)fno.fsize);
                    count++;
                }
                f_closedir(&dir);
                tp_print("Total: %d file(s) found\r\n", count);
            } else {
                tp_print("f_opendir failed!\r\n");
            }
        }
    }
    else if (tp_strcasecmp(cmd, "low") == 0 || tp_strcasecmp(cmd, "all0") == 0) {
        allOutputsLow();
        tp_print("all outputs low\r\n");
    }
    else if (tp_strcasecmp(cmd, "test") == 0) {
        tp_print("already in test mode\r\n");
    }
    else {
        tp_print("unknown command '%s' (try 'help')\r\n", cmd);
    }

    tp_print("test> ");
    return true;
}
