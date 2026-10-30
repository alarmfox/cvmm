/*
 * 16550A UART emulation, minimal port of kvmtool hw/serial.c.
 * ttyS0 is wired to stdin/stdout, ttyS1-3 exist but are unconnected.
 */
#include "serial.h"

#include <linux/kvm.h>
#include <linux/serial_reg.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#define FIFO_LEN 64   /* the kernel expects 64 for 16550A autodetection */
#define ESCAPE_CHAR 0x01   /* Ctrl-A */

struct serial8250_device {
	pthread_mutex_t mutex;
	uint16_t	iobase;
	uint8_t		irq;
	uint8_t		irq_state;
	int		in_fd, out_fd;
	int		rxcnt, rxdone;
	uint8_t		rxbuf[FIFO_LEN];
	uint8_t		dll, dlm, iir, ier, fcr, lcr, mcr, lsr, msr, scr;
};

#define SERIAL_DEV(base, _irq, in, out) {			\
	.mutex	= PTHREAD_MUTEX_INITIALIZER,			\
	.iobase	= base, \
	.irq = _irq,				\
	.in_fd	= in, .out_fd = out,				\
	.iir	= UART_IIR_NO_INT,				\
	.lsr	= UART_LSR_TEMT | UART_LSR_THRE,		\
	.msr	= UART_MSR_DCD | UART_MSR_DSR | UART_MSR_CTS,	\
	.mcr	= UART_MCR_OUT2,				\
}

static struct serial8250_device devices[] = {
	SERIAL_DEV(0x3f8, 4, STDIN_FILENO, STDOUT_FILENO),	/* ttyS0 */
	SERIAL_DEV(0x2f8, 3, -1, -1),				/* ttyS1 */
	SERIAL_DEV(0x3e8, 4, -1, -1),				/* ttyS2 */
	SERIAL_DEV(0x2e8, 3, -1, -1),				/* ttyS3 */
};

#define NR_DEVICES (sizeof(devices) / sizeof(devices[0]))

static int vm_fd = -1;

static void irq_line(int irq, int level) {
	struct kvm_irq_level l = { .irq = irq, .level = level };
	ioctl(vm_fd, KVM_IRQ_LINE, &l);
}

static void serial8250_update_irq(struct serial8250_device *dev) {
	uint8_t iir = 0;

	if (dev->fcr & UART_FCR_CLEAR_RCVR) {
		dev->fcr &= ~UART_FCR_CLEAR_RCVR;
		dev->rxcnt = dev->rxdone = 0;
		dev->lsr &= ~UART_LSR_DR;
	}

	if ((dev->ier & UART_IER_RDI) && (dev->lsr & UART_LSR_DR))
		iir |= UART_IIR_RDI;
	if ((dev->ier & UART_IER_THRI) && (dev->lsr & UART_LSR_TEMT))
		iir |= UART_IIR_THRI;

	dev->iir = iir ? iir : UART_IIR_NO_INT;
	if (!!iir != !!dev->irq_state)
		irq_line(dev->irq, !!iir);
	dev->irq_state = iir;
}

static void serial8250_receive(struct serial8250_device *dev) {
	static int got_escape;
	struct pollfd p = { .fd = dev->in_fd, .events = POLLIN };

	if (dev->in_fd < 0 || (dev->mcr & UART_MCR_LOOP) || dev->rxcnt)
		return;
	if (poll(&p, 1, 0) <= 0)
		return;

	ssize_t n = read(dev->in_fd, dev->rxbuf, FIFO_LEN);
	if (n <= 0)
		return;

	/* Ctrl-A x: quit VMM. Ctrl-A Ctrl-A: send a literal Ctrl-A. */
	int out = 0;
	for (ssize_t i = 0; i < n; i++) {
		uint8_t c = dev->rxbuf[i];

		if (got_escape) {
			got_escape = 0;
			if (c == 'x') {
				fprintf(stderr, "\r\n[vmm] quit\r\n");
				exit(0);	/* runs atexit(term_restore) */
			}
			dev->rxbuf[out++] = c;
			continue;
		}
		if (c == ESCAPE_CHAR) {
			got_escape = 1;
			continue;
		}
		dev->rxbuf[out++] = c;
	}

	if (out) {
		dev->rxcnt = out;
		dev->lsr |= UART_LSR_DR;
	}
}

static void serial8250_out(struct serial8250_device *dev, int off, uint8_t v)
{
	switch (off) {
	case UART_TX:
		if (dev->lcr & UART_LCR_DLAB)
			dev->dll = v;
		else if (dev->mcr & UART_MCR_LOOP) {
			if (dev->rxcnt < FIFO_LEN) {
				dev->rxbuf[dev->rxcnt++] = v;
				dev->lsr |= UART_LSR_DR;
			}
		} else if (dev->out_fd >= 0)
			(void)!write(dev->out_fd, &v, 1);
		break;
	case UART_IER:
		if (dev->lcr & UART_LCR_DLAB)
			dev->dlm = v;
		else
			dev->ier = v & 0x0f;
		break;
	case UART_FCR: dev->fcr = v; break;
	case UART_LCR: dev->lcr = v; break;
	case UART_MCR: dev->mcr = v; break;
	case UART_SCR: dev->scr = v; break;
	}
}

static uint8_t serial8250_in(struct serial8250_device *dev, int off)
{
	uint8_t v;

	switch (off) {
	case UART_RX:
		if (dev->lcr & UART_LCR_DLAB)
			return dev->dll;
		if (dev->rxdone == dev->rxcnt)
			return 0;
		v = dev->rxbuf[dev->rxdone++];
		if (dev->rxdone == dev->rxcnt) {
			dev->lsr &= ~UART_LSR_DR;
			dev->rxcnt = dev->rxdone = 0;
		}
		return v;
	case UART_IER: return (dev->lcr & UART_LCR_DLAB) ? dev->dlm : dev->ier;
	case UART_IIR: return dev->iir | 0xc0;	/* FIFOs enabled */
	case UART_LCR: return dev->lcr;
	case UART_MCR: return dev->mcr;
	case UART_LSR: return dev->lsr;
	case UART_MSR: return dev->msr;
	case UART_SCR: return dev->scr;
	}
	return 0xff;
}

int serial8250__io(uint16_t port, uint8_t *data, int is_write) {
	for (unsigned i = 0; i < NR_DEVICES; i++) {
		struct serial8250_device *dev = &devices[i];

		if (port < dev->iobase || port >= dev->iobase + 8)
			continue;

		pthread_mutex_lock(&dev->mutex);
		if (is_write)
			serial8250_out(dev, port - dev->iobase, *data);
		else
			*data = serial8250_in(dev, port - dev->iobase);
		serial8250_update_irq(dev);
		pthread_mutex_unlock(&dev->mutex);
		return 1;
	}
	return 0;
}

/* Replaces kvmtool's term poll thread + serial8250__update_consoles() */
static void *serial8250_poll_thread(void *arg) {
	(void)arg;

	for (;;) {
		struct pollfd p = { .fd = STDIN_FILENO, .events = POLLIN };
		if (poll(&p, 1, 10) > 0)
			usleep(1000);	/* guest may not have drained the FIFO yet */

		for (unsigned i = 0; i < NR_DEVICES; i++) {
			pthread_mutex_lock(&devices[i].mutex);
			serial8250_receive(&devices[i]);
			serial8250_update_irq(&devices[i]);
			pthread_mutex_unlock(&devices[i].mutex);
		}
	}
	return NULL;
}

int serial8250__init(int vmfd) {
	pthread_t tid;

	vm_fd = vmfd;
	for (unsigned i = 0; i < NR_DEVICES; i++)
		irq_line(devices[i].irq, 0);

	if (pthread_create(&tid, NULL, serial8250_poll_thread, NULL))
		return -1;
	pthread_detach(tid);
	return 0;
}
