#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

/* Create the 4 UARTs (ttyS0-3) and start the stdin poll thread. */
int  serial8250__init(int vmfd);

/* Call from KVM_EXIT_IO. Returns false if the port is not a UART. */
int serial8250__io(uint16_t port, uint8_t *data, int is_write);

#endif
