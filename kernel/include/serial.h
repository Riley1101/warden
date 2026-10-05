// serial.h - Serial communication 

#pragma once
#include <stdint.h>

#define COM1 0x3F8 /* console + debug channel */
#define COM2 0x2F8 /* agent channel (TODO!) */

/** 
 * @brief 
 * Initialize the serial port at the given I/O port address 
 **/
void serial_init(uint16_t port);

/** 
 * @brief 
 * Send a character to the serial port at the given I/O port address 
 **/
void serial_putc(uint16_t port, char c);

/** 
 * @brief 
 * Send a null-terminated string to the serial port at the given I/O port address 
 **/
void serial_puts(uint16_t port, const char* str);

