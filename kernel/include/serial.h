// serial.h - Serial communication 

#pragma once
#include <stdint.h>

#define COM1 0x3F8 /* console + debug channel */
#define COM2 0x2F8 /* agent channel (TODO!) */

#define IRQ_COM1 4 /* the ISA interrupt line COM1 (and COM3) sit on */
#define IRQ_COM2 3 /* COM2 (and COM4) */

/** 
 * @brief 
 * Initialize the serial port at the given I/O port address 
 **/
void serial_init(uint16_t port);

/**
 * @brief
 * Is there a UART at this port at all? An absent one reads as 0xFF on
 * every register, which would look like a port with data waiting
 * forever. Check before polling a port that may not be wired up.
 **/
int serial_present(uint16_t port);

/** 
 * @brief 
 * Send a character to the serial port at the given I/O port address 
 **/
void serial_putc(uint16_t port, char c);

/**
 * @brief
 * One byte from the serial port's receive buffer, or -1 if none is waiting.
 * Polled (from the timer tick); there is no receive interrupt yet.
 **/
int serial_getc(uint16_t port);

/**
 * @brief
 * Have the port raise its interrupt line whenever a byte arrives. The
 * line still has to be routed (apic_route_irq) to reach the CPU.
 **/
void serial_enable_rx_irq(uint16_t port);

/**
 * @brief
 * Send one byte exactly as given, without turning a newline into CRLF.
 * For a channel carrying data rather than text.
 **/
void serial_putc_raw(uint16_t port, char c);

/** 
 * @brief 
 * Send a null-terminated string to the serial port at the given I/O port address 
 **/
void serial_puts(uint16_t port, const char* str);

