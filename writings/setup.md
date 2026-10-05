## Setup and Installation

Required dependencies for warden to run.

```
yay -S nasm qemu-system-x86 xorriso git
```

## Makefile

Warden comes with a Makefile that simplifies the build process and tooling.

```
warden main  ❯ make
  deps         Fetch Limine and limine.h
  kernel       Build the kernel ELF
  iso          Build the bootable ISO
  run          Boot in QEMU, serial on this terminal
  run-debug    Boot paused, waiting for GDB on :1234
  test         Run all tests (host + QEMU boot)
  host-test    Unit tests on the host, with sanitizers
  clean        Remove build output
  distclean    Also remove fetched deps (keeps vendored limine.h)
  compdb       Generate compile_commands.json for your LSP
  help         Show available targets
```
