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

void serial_putc(uint16_t port, char c) {
    if (c == '\n')
        serial_putc(port, '\r');
    while ((inb(port + LINE_STATUS) & LSR_TX_EMPTY) == 0)
        ;
    outb(port + DATA, (uint8_t)c);
}

void serial_puts(uint16_t port, const char *s) {
    while (*s)
        serial_putc(port, *s++);
}
