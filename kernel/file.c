#include "file.h"

#include <abi/errno.h>
#include <apic.h>
#include <fs.h>
#include <idt.h>
#include <pmm.h>
#include <sched.h>
#include <serial.h>
#include <slab.h>
#include <usercopy.h>

#define PIPE_SIZE PAGE_SIZE

struct pipe {
  char *buf;          /* ring of PIPE_SIZE bytes */
  size_t head, count; /* next byte to read, bytes buffered */
  int readers, writers;
};

enum file_kind { F_CONSOLE, F_PIPE_READ, F_PIPE_WRITE, F_NODE };

struct file {
  enum file_kind kind;
  int refs;
  struct pipe *pipe;
  /* F_NODE: a file or directory, opened with `flags`. The offset is shared
   * by every descriptor naming this file (fork, dup2), as on Unix; for a
   * directory it counts entries read. */
  struct node *node;
  int flags;
  uint64_t off;
};

/* One console, shared by everyone and never freed. */
static struct file console = {.kind = F_CONSOLE};

/*
 * Console input: COM1, polled from the timer tick (no receive interrupt
 * yet). Typing is line-buffered here, as a terminal in canonical mode:
 * characters are echoed and gathered in `line`, backspace rubs one out,
 * and a whole line (with its newline) reaches readers at once. Ctrl-D
 * sends what is typed, or end-of-file on an empty line.
 */
#define LINE_MAX 256
#define CIN_SIZE 1024

static struct {
  char buf[CIN_SIZE];
  size_t head, count; /* next byte to read, bytes buffered */
  char line[LINE_MAX];
  size_t len; /* bytes typed since the last newline */
  int eof;    /* Ctrl-D on an empty line: one read returns 0 */
} cin;

static size_t min(size_t a, size_t b) { return a < b ? a : b; }

/* ---- the console ---------------------------------------------------------- */

static int64_t console_write(const char *ubuf, size_t n) {
  char chunk[64];
  for (size_t done = 0; done < n;) {
    size_t k = min(n - done, sizeof chunk);
    if (copy_from_user(chunk, ubuf + done, k) != 0)
      return done > 0 ? (int64_t)done : -EFAULT;
    for (size_t i = 0; i < k; i++)
      serial_putc(COM1, chunk[i]);
    done += k;
  }
  return n;
}

/* One typed byte into the ring; dropped if nobody is keeping up. */
static void cin_put(char c) {
  if (cin.count == CIN_SIZE)
    return;
  cin.buf[(cin.head + cin.count) % CIN_SIZE] = c;
  cin.count++;
}

/* The gathered line (plus `extra`, a newline or nothing) to the readers. */
static void cin_commit(char extra) {
  for (size_t i = 0; i < cin.len; i++)
    cin_put(cin.line[i]);
  if (extra != '\0')
    cin_put(extra);
  cin.len = 0;
}

/*
 * One typed byte, as a terminal in canonical mode sees it. Separate from
 * console_poll so tests can type into the console without a UART.
 * Interrupts must be off: it wakes the readers.
 */
void console_feed(int c) {
  switch (c) {
  case '\r':
  case '\n':
    serial_putc(COM1, '\n');
    cin_commit('\n');
    thread_wakeup(&cin);
    break;
  case 0x7f: /* backspace, however the terminal spells it */
  case '\b':
    if (cin.len > 0) {
      cin.len--;
      serial_puts(COM1, "\b \b");
    }
    break;
  case 0x04: /* Ctrl-D: send the line, or end the input if there is none */
    if (cin.len == 0)
      cin.eof = 1;
    else
      cin_commit('\0');
    thread_wakeup(&cin);
    break;
  default:
    if (c >= ' ' && c < 0x7f && cin.len < LINE_MAX) {
      cin.line[cin.len++] = (char)c;
      serial_putc(COM1, (char)c);
    }
    break;
  }
}

void console_poll(void) {
  int c;
  while ((c = serial_getc(COM1)) >= 0)
    console_feed(c);
}

static void on_console_irq(struct trap_frame *f) {
  (void)f;
  console_poll();
  apic_eoi();
}

void console_init(void) {
  idt_register(T_CONSOLE, on_console_irq);
  apic_route_irq(IRQ_COM1, T_CONSOLE);
  serial_enable_rx_irq(COM1);
}

static int64_t console_read(char *ubuf, size_t n) {
  if (n == 0)
    return 0;
  while (cin.count == 0) {
    if (cin.eof) {
      cin.eof = 0;
      return 0;
    }
    thread_sleep(&cin);
  }
  size_t got = 0;
  int err = 0;
  while (got < n && cin.count > 0) {
    size_t k = min(min(n - got, cin.count), CIN_SIZE - cin.head);
    if (copy_to_user(ubuf + got, cin.buf + cin.head, k) != 0) {
      err = -EFAULT;
      break;
    }
    cin.head = (cin.head + k) % CIN_SIZE;
    cin.count -= k;
    got += k;
  }
  return got > 0 ? (int64_t)got : err;
}

/* ---- pipes ---------------------------------------------------------------- */

static int64_t pipe_read(struct pipe *p, char *ubuf, size_t n) {
  if (n == 0)
    return 0;
  while (p->count == 0) {
    if (p->writers == 0)
      return 0; /* end of file */
    thread_sleep(p);
  }
  size_t got = 0;
  int err = 0;
  while (got < n && p->count > 0) {
    size_t k = min(min(n - got, p->count), PIPE_SIZE - p->head);
    if (copy_to_user(ubuf + got, p->buf + p->head, k) != 0) {
      err = -EFAULT;
      break;
    }
    p->head = (p->head + k) % PIPE_SIZE;
    p->count -= k;
    got += k;
  }
  thread_wakeup(p); /* writers waiting for room */
  return got > 0 ? (int64_t)got : err;
}

static int64_t pipe_write(struct pipe *p, const char *ubuf, size_t n) {
  size_t put = 0;
  while (put < n) {
    if (p->readers == 0)
      return put > 0 ? (int64_t)put : -EPIPE;
    if (p->count == PIPE_SIZE) {
      thread_sleep(p);
      continue;
    }
    size_t tail = (p->head + p->count) % PIPE_SIZE;
    size_t k = min(min(n - put, PIPE_SIZE - p->count), PIPE_SIZE - tail);
    if (copy_from_user(p->buf + tail, ubuf + put, k) != 0) {
      thread_wakeup(p);
      return put > 0 ? (int64_t)put : -EFAULT;
    }
    p->count += k;
    put += k;
    thread_wakeup(p); /* readers waiting for data */
  }
  return put;
}

/* One end is closed: the other side may be sleeping on it. */
static void pipe_close_end(struct pipe *p, enum file_kind end) {
  if (end == F_PIPE_READ)
    p->readers--;
  else
    p->writers--;
  thread_wakeup(p);
  if (p->readers == 0 && p->writers == 0) {
    kfree(p->buf);
    kfree(p);
  }
}

/* ---- fs nodes ------------------------------------------------------------- */

/* Bounce through a kernel buffer: fs.c knows nothing of user memory. */
#define BOUNCE 256

static int64_t node_read(struct file *f, char *ubuf, size_t n) {
  if ((f->flags & O_ACCMODE) == O_WRONLY)
    return -EBADF;
  if (fs_is_dir(f->node))
    return -EISDIR;
  char tmp[BOUNCE];
  size_t got = 0;
  while (got < n) {
    int64_t r = fs_read(f->node, f->off, tmp, min(n - got, sizeof tmp));
    if (r <= 0)
      break;
    if (copy_to_user(ubuf + got, tmp, (size_t)r) != 0)
      return got > 0 ? (int64_t)got : -EFAULT;
    f->off += (uint64_t)r;
    got += (size_t)r;
  }
  return (int64_t)got;
}

static int64_t node_write(struct file *f, const char *ubuf, size_t n) {
  if ((f->flags & O_ACCMODE) == O_RDONLY)
    return -EBADF;
  if (f->flags & O_APPEND)
    f->off = fs_size(f->node);
  char tmp[BOUNCE];
  size_t put = 0;
  while (put < n) {
    size_t k = min(n - put, sizeof tmp);
    if (copy_from_user(tmp, ubuf + put, k) != 0)
      return put > 0 ? (int64_t)put : -EFAULT;
    int64_t w = fs_write(f->node, f->off, tmp, k);
    if (w < 0)
      return put > 0 ? (int64_t)put : w;
    f->off += k;
    put += k;
  }
  return (int64_t)put;
}

/* ---- files ---------------------------------------------------------------- */

static void file_put(struct file *f) {
  if (f == &console || --f->refs > 0)
    return;
  if (f->kind == F_NODE)
    fs_put(f->node);
  else
    pipe_close_end(f->pipe, f->kind);
  kfree(f);
}

static struct file *file_ref(struct file *f) {
  if (f != &console)
    f->refs++;
  return f;
}

int64_t file_read(struct file *f, void *ubuf, size_t n) {
  switch (f->kind) {
  case F_CONSOLE:
    return console_read(ubuf, n);
  case F_PIPE_READ:
    return pipe_read(f->pipe, ubuf, n);
  case F_NODE:
    return node_read(f, ubuf, n);
  default:
    return -EBADF;
  }
}

int64_t file_write(struct file *f, const void *ubuf, size_t n) {
  switch (f->kind) {
  case F_CONSOLE:
    return console_write(ubuf, n);
  case F_PIPE_WRITE:
    return pipe_write(f->pipe, ubuf, n);
  case F_NODE:
    return node_write(f, ubuf, n);
  default:
    return -EBADF;
  }
}

/* ---- descriptor tables ---------------------------------------------------- */

void fd_init_console(struct file **table) {
  for (int i = 0; i < 3; i++)
    table[i] = file_ref(&console);
}

void fd_copy(struct file **table, struct file *const *src) {
  for (int i = 0; i < NFD; i++)
    table[i] = src[i] != NULL ? file_ref(src[i]) : NULL;
}

struct file *fd_get(struct file **table, int fd) {
  return fd >= 0 && fd < NFD ? table[fd] : NULL;
}

int fd_close(struct file **table, int fd) {
  struct file *f = fd_get(table, fd);
  if (f == NULL)
    return -EBADF;
  table[fd] = NULL;
  file_put(f);
  return 0;
}

void fd_close_all(struct file **table) {
  for (int i = 0; i < NFD; i++)
    fd_close(table, i);
}

int fd_dup2(struct file **table, int oldfd, int newfd) {
  struct file *f = fd_get(table, oldfd);
  if (f == NULL || newfd < 0 || newfd >= NFD)
    return -EBADF;
  if (oldfd == newfd)
    return newfd;
  fd_close(table, newfd);
  table[newfd] = file_ref(f);
  return newfd;
}

/* The lowest free descriptor, or -EMFILE. */
static int fd_free_slot(struct file **table, int after) {
  for (int i = after + 1; i < NFD; i++)
    if (table[i] == NULL)
      return i;
  return -EMFILE;
}

int fd_open(struct file **table, const char *path, int flags) {
  int fd = fd_free_slot(table, -1);
  if (fd < 0)
    return fd;
  struct file *f = kzalloc(sizeof *f);
  if (f == NULL)
    return -ENOMEM;
  int err = fs_open(path, flags, &f->node);
  if (err != 0) {
    kfree(f);
    return err;
  }
  f->kind = F_NODE;
  f->refs = 1;
  f->flags = flags;
  table[fd] = f;
  return fd;
}

int fd_getdent(struct file **table, int fd, struct dirent *out) {
  struct file *f = fd_get(table, fd);
  if (f == NULL)
    return -EBADF;
  if (f->kind != F_NODE || !fs_is_dir(f->node))
    return -ENOTDIR;
  int r = fs_readdir(f->node, f->off, out);
  if (r > 0)
    f->off++;
  return r;
}

int fd_pipe(struct file **table, int fds[2]) {
  int r = fd_free_slot(table, -1);
  int w = r < 0 ? r : fd_free_slot(table, r);
  if (w < 0)
    return -EMFILE;

  struct pipe *p = kzalloc(sizeof *p);
  char *buf = kmalloc(PIPE_SIZE);
  struct file *rf = kzalloc(sizeof *rf);
  struct file *wf = kzalloc(sizeof *wf);
  if (p == NULL || buf == NULL || rf == NULL || wf == NULL) {
    kfree(p);
    kfree(buf);
    kfree(rf);
    kfree(wf);
    return -ENOMEM;
  }
  *p = (struct pipe){.buf = buf, .readers = 1, .writers = 1};
  *rf = (struct file){.kind = F_PIPE_READ, .refs = 1, .pipe = p};
  *wf = (struct file){.kind = F_PIPE_WRITE, .refs = 1, .pipe = p};
  table[r] = rf;
  table[w] = wf;
  fds[0] = r;
  fds[1] = w;
  return 0;
}
