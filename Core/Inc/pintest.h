// Sharp CE-140F emulator - interactive pin test console
//
// Purpose: drive / read the individual bus lines by hand from the serial
// console, so the electrical path to the Sharp PC (level converter, wiring,
// polarity) can be validated independently of the protocol state machine.
//
// Entered by typing "test" on the serial console, left with "exit".
// While the test console is active the whole CE-140F protocol engine is
// suspended (EXTI sources masked, software timers frozen), so nothing fights
// us for the bus.

#ifndef PINTEST_H
#define PINTEST_H

#include "main.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// True while the interactive pin test console owns the bus.
bool pintest_is_active(void);

// Feed one complete, NUL-terminated console line (without CR/LF).
// Returns true if the line was consumed by the test console.
// When inactive, only the "test" command is consumed.
bool pintest_process_line(const char *line);

#ifdef __cplusplus
}
#endif

#endif // PINTEST_H
