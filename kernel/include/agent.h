#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * The agent channel: COM2, a second serial port with nothing else on it.
 * The host speaks to warden through it (QEMU: a unix socket), and one
 * process in here reads and writes it as /dev/agent.
 *
 * Unlike the console there is no echo and no line discipline: bytes
 * arrive as they were sent, and a reader takes whatever has arrived.
 * Framing is the protocol's business, above this.
 */

/* The vector COM2's interrupt line is delivered on. */
#define T_AGENT 0x23

/* The path that opens the channel. */
#define AGENT_PATH "/dev/agent"

/* Set up COM2 and start taking its interrupt. Call after apic_init(). */
void agent_init(void);

/* One received byte, as the interrupt handler sees it. Interrupts must
 * be off: it wakes the reader. */
void agent_feed(int c);

/* Take whatever has arrived on COM2. Called from the interrupt, and from
 * the timer tick as a backstop, with interrupts off. */
void agent_poll(void);

/*
 * Up to `n` bytes into the user buffer `ubuf`, blocking until at least
 * one arrives. Bytes, or -EFAULT.
 */
int64_t agent_read(void *ubuf, size_t n);

/* `n` bytes from the user buffer out of COM2. Bytes, or -EFAULT. */
int64_t agent_write(const void *ubuf, size_t n);

/* Bytes dropped because the ring was full: the host sent faster than
 * anyone read. Reported in the audit log, once there is one. */
uint64_t agent_dropped(void);
