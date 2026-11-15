#include "agent.h"

#include <abi/errno.h>
#include <apic.h>
#include <idt.h>
#include <kprintf.h>
#include <sched.h>
#include <serial.h>
#include <usercopy.h>

#define RING_SIZE 4096

static int present; /* 0: no COM2 on this machine, so nothing to poll */

static struct {
  char buf[RING_SIZE];
  size_t head, count; /* next byte to read, bytes buffered */
  uint64_t dropped;
} rx;

static size_t min(size_t a, size_t b) { return a < b ? a : b; }

void agent_feed(int c) {
  if (rx.count == RING_SIZE) {
    /* Only the kernel test feeds bytes past agent_poll's check. Say so
     * once: a flood of kprintf down COM1 would only make it worse. */
    if (rx.dropped++ == 0)
      kprintf("agent: receive buffer full, dropping\n");
    return;
  }
  rx.buf[(rx.head + rx.count) % RING_SIZE] = (char)c;
  rx.count++;
  thread_wakeup(&rx);
}

/*
 * Move what COM2 holds into the ring. A full ring takes no more: the
 * bytes wait in the UART, its interrupt off so it doesn't fire on and on,
 * and once its FIFO fills the sender is held back (QEMU's socket simply
 * stops being read). agent_read starts it again. A byte is never taken
 * from the UART without room for it, so nothing is dropped: the framed
 * protocol above can't recover from a hole in the stream.
 */
void agent_poll(void) {
  int c;
  if (!present)
    return;
  while (rx.count < RING_SIZE && (c = serial_getc(COM2)) >= 0)
    agent_feed(c);
  if (rx.count == RING_SIZE)
    serial_disable_rx_irq(COM2);
}

static void on_agent_irq(struct trap_frame *f) {
  (void)f;
  agent_poll();
  apic_eoi();
}

void agent_init(void) {
  if (!serial_present(COM2)) {
    kprintf("agent: no COM2, the channel is closed\n");
    return;
  }
  present = 1;
  serial_init(COM2);
  idt_register(T_AGENT, on_agent_irq);
  apic_route_irq(IRQ_COM2, T_AGENT);
  serial_enable_rx_irq(COM2);
}

int64_t agent_read(void *ubuf, size_t n) {
  if (!present)
    return -ENXIO;
  if (n == 0)
    return 0;
  while (rx.count == 0)
    thread_sleep(&rx); /* the channel never ends: no EOF to report */

  char *out = ubuf;
  size_t got = 0;
  int err = 0;
  while (got < n && rx.count > 0) {
    size_t k = min(min(n - got, rx.count), RING_SIZE - rx.head);
    if (copy_to_user(out + got, rx.buf + rx.head, k) != 0) {
      err = -EFAULT;
      break;
    }
    rx.head = (rx.head + k) % RING_SIZE;
    rx.count -= k;
    got += k;
  }
  if (got > 0) { /* room again: take what waited in the UART */
    serial_enable_rx_irq(COM2);
    agent_poll();
  }
  return got > 0 ? (int64_t)got : err;
}

int64_t agent_write(const void *ubuf, size_t n) {
  if (!present)
    return -ENXIO;
  const char *in = ubuf;
  char chunk[64];
  for (size_t done = 0; done < n;) {
    size_t k = min(n - done, sizeof chunk);
    if (copy_from_user(chunk, in + done, k) != 0)
      return done > 0 ? (int64_t)done : -EFAULT;
    for (size_t i = 0; i < k; i++)
      serial_putc_raw(COM2, chunk[i]);
    done += k;
  }
  return (int64_t)n;
}

uint64_t agent_dropped(void) { return rx.dropped; }
