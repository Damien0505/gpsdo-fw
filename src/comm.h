#ifndef _COMM_H_
#define _COMM_H_

#include <stdint.h>

// Serial protocol on the external link (USART2), see doc/PROTOCOL.md.
// Everything here runs from the main loop, never from an interrupt.

// Call once at boot, after settings are loaded and before the main loop
void comm_init(void);

// Moves data between the GPS and the external link, handles protocol sentences. Never blocks.
void comm_task(void);

#endif
