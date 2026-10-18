# warden top-level Makefile

.SUFFIXES:
.DEFAULT_GOAL := help

# ---- Toolchain ------------------------------------------------------------
ifneq ($(shell command -v x86_64-elf-gcc 2>/dev/null),)
    TOOLCHAIN_PREFIX ?= x86_64-elf-
endif
CC      := $(TOOLCHAIN_PREFIX)gcc
LD      := $(TOOLCHAIN_PREFIX)ld
NASM    := nasm
HOST_CC := cc
PYTHON  := python3
QEMU    := qemu-system-x86_64

# Apple's clang ships an ASan runtime that hangs at startup on macOS 26, so
# prefer a Homebrew LLVM clang for the host tests when one is installed.
# Override with `make host-test HOST_CC=<compiler>`.
ifeq ($(shell uname -s),Darwin)
    BREW_CLANG := $(firstword $(wildcard /opt/homebrew/opt/llvm/bin/clang \
        /usr/local/opt/llvm/bin/clang))
    ifneq ($(BREW_CLANG),)
        HOST_CC := $(BREW_CLANG)
    endif
endif

# ---- Paths ----------------------------------------------------------------
BUILD  := build$(if $(TEST),-test)
ISO    := $(BUILD)/warden.iso
KERNEL := $(BUILD)/kernel.elf

HOST_BUILD := build-host
# Not every warning switch exists in both gcc and clang: ask the compiler.
cc-option = $(shell $(HOST_CC) $(1) -Werror -x c -c /dev/null -o /dev/null \
    2>/dev/null && echo $(1))

# `make host-test SAN=0` builds without the sanitizers, for a host whose
# ASan runtime doesn't work.
HOST_SAN := $(if $(filter 0,$(SAN)),,-fsanitize=address,undefined \
    -fno-sanitize-recover=all)

HOST_CFLAGS := -std=gnu11 -g -O1 -Wall -Wextra -Werror \
							 $(HOST_SAN) \
							 -fno-omit-frame-pointer \
							 -Ikernel/include -Iabi/include -Itests/host

# Files that prove dependencies were fetched (so `make deps` runs only once)
DEPS_STAMP := limine/limine third_party/limine.h

# ---- kernel build flags ----------------------------------------------------
KCFLAGS := -g -O2 -pipe -std=gnu11 -Wall -Wextra -Werror \
    -ffreestanding -fno-stack-protector -fno-stack-check -fno-lto \
    -fno-PIC -fno-pie -ffunction-sections -fdata-sections \
    -m64 -march=x86-64 -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
    -mno-red-zone -mcmodel=kernel \
    -MMD -MP \
    -Ikernel -Ikernel/include -Ithird_party \
		-Iabi/include \
    -Itests/kernel/include $(if $(TEST),-DWARDEN_TEST)
		

# ---- kernel link flags ------------------------------------------------------
KLDFLAGS := -m elf_x86_64 -nostdlib -static \
    -z max-page-size=0x1000 --gc-sections -T kernel/linker.ld

# ---- kernel source files ---------------------------------------------------
KSRC_C   := $(shell find kernel -name '*.c')
ifdef TEST
KSRC_C   += $(shell find tests/kernel -name '*.c')
endif

# --- kernel assembly source files ---------------------------------------------------
KSRC_ASM := $(shell find kernel -name '*.asm')

# --- kernel object files ---------------------------------------------------
KOBJ     := $(KSRC_C:%.c=$(BUILD)/obj/%.o) $(KSRC_ASM:%.asm=$(BUILD)/obj/%.asm.o)

# ---- QEMU flags -----------------------------------------------------------
# The display backend is whatever this qemu was actually built with: gtk on a
# typical Linux build, cocoa on macOS, sdl otherwise, none if it has no GUI.
# Override explicitly with `make run QEMU_DISPLAY=none` for headless.
# Recursive (`=`), so the probe only runs when a recipe needs it.
QEMU_DISPLAYS = $(shell $(QEMU) -display help 2>/dev/null)
QEMU_DISPLAY ?= $(firstword $(filter $(QEMU_DISPLAYS),gtk cocoa sdl) none)
# COM1 is the console and the debug output; COM2 is the agent channel,
# waiting on a unix socket for host/bridge.py.
AGENT_SOCK ?= /tmp/warden-agent.sock
QEMUFLAGS = -M q35 -cpu max -m 512M -cdrom $(ISO) -no-reboot \
    -display $(QEMU_DISPLAY) -serial stdio \
    -serial unix:$(AGENT_SOCK),server,nowait

# ---- Dependencies ---------------------------------------------------------
.PHONY: deps
deps: $(DEPS_STAMP)                  ## Fetch Limine and limine.h

$(DEPS_STAMP):
	./scripts/get-deps.sh

# ---- Kernel      --------------------------------------------------------------
.PHONY: kernel
kernel : $(KERNEL)

$(KERNEL): $(KOBJ) kernel/linker.ld
	$(LD) $(KLDFLAGS) $(KOBJ) -o $@ 

# kstring.c implements memset & co.: stop GCC turning its loops into calls
# to the very functions being defined.
$(BUILD)/obj/kernel/kstring.o: KCFLAGS += -fno-tree-loop-distribute-patterns

$(BUILD)/obj/%.o: %.c | $(DEPS_STAMP)
	@mkdir -p $(@D)
	$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/obj/%.asm.o: %.asm
	@mkdir -p $(@D)
	$(NASM) -f elf64 -g -F dwarf -MD $(@:.o=.d) -MP $< -o $@

-include $(KOBJ:.o=.d)


# ---- User programs, libc, and the initramfs ---------------------------------
# No floating point in ring 3 (CR0.EM): -mgeneral-regs-only keeps gcc from
# emitting SSE. -nostdinc keeps the host's headers out: only libc's, the
# ABI's, and gcc's own freestanding ones (stddef.h, stdarg.h, stdint.h).
# Each program is crt0 + its object + libc.a, linked at 0x400000.
AR := $(TOOLCHAIN_PREFIX)ar
USER_CFLAGS := -g -O2 -pipe -std=gnu11 -Wall -Wextra -Werror \
    -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
    -m64 -march=x86-64 -mgeneral-regs-only \
    -nostdinc -isystem $(shell $(CC) -print-file-name=include) \
    -MMD -MP -Ilibc/include -Iabi/include
USER_LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
    -T userland/user.ld
USER_CRT0 := $(BUILD)/user/crt0.o
LIBC      := $(BUILD)/user/libc.a

# libc shares the kernel's pure code: the formatter, and mem*/strlen.
LIBC_SRC := $(wildcard libc/*.c) kernel/fmt.c kernel/kstring.c
LIBC_OBJ := $(LIBC_SRC:%.c=$(BUILD)/user/obj/%.o)
$(BUILD)/user/obj/kernel/%.o $(BUILD)/user/obj/libc/stdio.o: \
    USER_CFLAGS += -Ikernel/include
$(BUILD)/user/obj/kernel/kstring.o: \
    USER_CFLAGS += -fno-tree-loop-distribute-patterns

$(USER_CRT0): libc/crt0.asm
	@mkdir -p $(@D)
	$(NASM) -f elf64 -g -F dwarf $< -o $@

$(BUILD)/user/obj/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(LIBC): $(LIBC_OBJ)
	rm -f $@
	$(AR) rcs $@ $^

-include $(shell find $(BUILD)/user -name '*.d' 2>/dev/null)

# The initramfs tree. Test builds add /test: the ring 3 test programs.
INITRD     := $(BUILD)/initrd.tar
INITRD_DIR := $(BUILD)/initrd
INITRD_FILES :=

# /bin: the shell and its tools, one program per userland/*.c
USER_PROGS   := $(patsubst userland/%.c,%,$(wildcard userland/*.c))
INITRD_FILES += $(USER_PROGS:%=$(INITRD_DIR)/bin/%)

ifdef TEST
TEST_PROGS   := $(patsubst tests/user/%.c,%,$(wildcard tests/user/*.c))
INITRD_FILES += $(TEST_PROGS:%=$(INITRD_DIR)/test/%) $(INITRD_DIR)/test/notelf
endif

$(INITRD_DIR)/test/%: $(BUILD)/user/obj/tests/user/%.o $(USER_CRT0) $(LIBC) \
    userland/user.ld
	@mkdir -p $(@D)
	$(LD) $(USER_LDFLAGS) $(USER_CRT0) $< $(LIBC) -o $@

$(INITRD_DIR)/bin/%: $(BUILD)/user/obj/userland/%.o $(USER_CRT0) $(LIBC) \
    userland/user.ld
	@mkdir -p $(@D)
	$(LD) $(USER_LDFLAGS) $(USER_CRT0) $< $(LIBC) -o $@

$(INITRD_DIR)/test/notelf: tests/user/notelf
	@mkdir -p $(@D)
	cp $< $@

$(INITRD): $(INITRD_FILES)
	@mkdir -p $(INITRD_DIR)
	tar --format=ustar --owner=0 --group=0 --numeric-owner \
	    -C $(INITRD_DIR) -cf $@ .

# --- ISO ----------------------------------------------------------------------
.PHONY: iso
iso: $(ISO)

$(ISO): $(KERNEL) $(INITRD) boot/limine.conf $(DEPS_STAMP)
	rm -rf $(BUILD)/iso_root
	mkdir -p $(BUILD)/iso_root/boot/limine $(BUILD)/iso_root/EFI/BOOT
	cp $(KERNEL) $(BUILD)/iso_root/boot/kernel
	cp $(INITRD) $(BUILD)/iso_root/boot/initrd.tar
	cp boot/limine.conf limine/limine-bios.sys limine/limine-bios-cd.bin \
	   limine/limine-uefi-cd.bin $(BUILD)/iso_root/boot/limine/
	cp limine/BOOTX64.EFI $(BUILD)/iso_root/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
	    -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
	    -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
	    -efi-boot-part --efi-boot-image --protective-msdos-label \
	    $(BUILD)/iso_root -o $@ 2>/dev/null
	./limine/limine bios-install $@


# ---- Build / run / test  ------------------------
.PHONY: run run-debug 
run : $(ISO)                        ## Run the kernel in QEMU
	$(QEMU) $(QEMUFLAGS)

run-debug : $(ISO)                  ## Run the kernel in QEMU with GDB stub
	$(QEMU) $(QEMUFLAGS) -s -S -d int,guest_errors -D $(BUILD)/qemu.log
 
 # ---- Tests  -----------------------------------------------------
.PHONY: test host-test python-test
test: host-test python-test          ## Run all tests
	$(MAKE) TEST=1 iso
	./tests/qemu/run.sh build-test/warden.iso build-test/serial.log

HOST_TESTS := path fmt pmm pt slab elf tar malloc fs
$(HOST_BUILD)/test_path: kernel/path.c
$(HOST_BUILD)/test_fmt: kernel/fmt.c
$(HOST_BUILD)/test_pmm: kernel/pmm.c tests/host/lock_stub.c
$(HOST_BUILD)/test_pt: kernel/pt.c kernel/pmm.c tests/host/lock_stub.c
$(HOST_BUILD)/test_slab: kernel/slab.c kernel/pmm.c tests/host/lock_stub.c
$(HOST_BUILD)/test_elf: kernel/elf.c
$(HOST_BUILD)/test_tar: kernel/tar.c tests/host/tar_builder.h
$(HOST_BUILD)/test_fs: kernel/fs.c kernel/path.c kernel/tar.c kernel/slab.c kernel/pmm.c \
    tests/host/lock_stub.c tests/host/tar_builder.h
$(HOST_BUILD)/test_malloc: libc/malloc.c tests/host/libc_rename.h
# the tests ask for absurd sizes; the -Walloc-size* warnings are gcc-only
$(HOST_BUILD)/test_malloc: HOST_CFLAGS += -include tests/host/libc_rename.h \
    $(call cc-option,-Wno-alloc-size) $(call cc-option,-Wno-alloc-size-larger-than)

host-test: $(HOST_TESTS:%=$(HOST_BUILD)/test_%)   ## Unit tests on the host, with sanitizers
	@for t in $^; do ./$$t || exit 1; done

python-test:                        ## Run host-side Python unit tests
	$(PYTHON) -m unittest discover -s tests/python -p 'test_*.py'

$(HOST_BUILD)/test_%: tests/host/include/test_%.c tests/host/include/check.h
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) $(filter %.c,$^) -o $@

# ---- Housekeeping ---------------------------------------------------------
.PHONY: clean distclean compdb lsp help

clean:                               ## Remove build output
	rm -rf build build-test iso_root *.log

distclean: clean                     ## Also remove fetched deps (keeps vendored limine.h)
	rm -rf limine

compdb:                              ## Generate compile_commands.json for your LSP
	bear -- $(MAKE) TEST=1 iso # the test build: kernel, tests, libc, programs

lsp: clean compdb                    ## Regenerate compile_commands.json from a clean build (fixes stale LSP)

help:                                ## Show available targets
	@grep -hE '^[a-zA-Z_-]+:.*## ' $(MAKEFILE_LIST) | \
	  awk -F':.*## ' '{printf "  \033[36m%-12s\033[0m %s\n", $$1, $$2}'
