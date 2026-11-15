// serial.c
#include "serial.h"
#include "io.h"
#include <stdint.h>

/* 16550 UART register offsets */
#define DATA        0 /* data register (DLAB=0) / divisor low (DLAB=1) */
#define INT_ENABLE  1 /* interrupt enable (DLAB=0) / divisor high (DLAB=1) */
#define FIFO_CTRL   2
#define LINE_CTRL   3
#define MODEM_CTRL  4
#define LINE_STATUS 5
#define SCRATCH     7

#define LSR_RX_READY 0x01
#define LSR_TX_EMPTY 0x20

void serial_init(uint16_t port) {
    outb(port + INT_ENABLE, 0x00); /* no interrupts yet */
    outb(port + LINE_CTRL, 0x80);  /* DLAB on: next two writes set baud divisor */
    outb(port + DATA, 0x01);       /* divisor 1 = 115200 baud */
    outb(port + INT_ENABLE, 0x00);
    outb(port + LINE_CTRL, 0x03);  /* DLAB off, 8 bits, no parity, 1 stop */
    outb(port + FIFO_CTRL, 0xC7);  /* enable + clear FIFOs, 14-byte threshold */
    outb(port + MODEM_CTRL, 0x0B); /* DTR, RTS, OUT2 */
}

int serial_present(uint16_t port) {
    /* The scratch register exists only to be written and read back. An
     * absent UART is an unmapped port: every read gives 0xFF. */
    outb(port + SCRATCH, 0xAE);
    if (inb(port + SCRATCH) != 0xAE)
        return 0;
    outb(port + SCRATCH, 0x00);
    return inb(port + SCRATCH) == 0x00;
}

void serial_putc_raw(uint16_t port, char c) {
    while ((inb(port + LINE_STATUS) & LSR_TX_EMPTY) == 0)
        ;
    outb(port + DATA, (uint8_t)c);
}

void serial_putc(uint16_t port, char c) {
    if (c == '\n')
        serial_putc_raw(port, '\r');
    serial_putc_raw(port, c);
}

int serial_getc(uint16_t port) {
    if ((inb(port + LINE_STATUS) & LSR_RX_READY) == 0)
        return -1;
    return inb(port + DATA);
}

void serial_enable_rx_irq(uint16_t port) {
    outb(port + INT_ENABLE, 0x01); /* data available */
}

void serial_disable_rx_irq(uint16_t port) {
    outb(port + INT_ENABLE, 0x00);
}

void serial_puts(uint16_t port, const char *s) {
    while (*s)
        serial_putc(port, *s++);
}
