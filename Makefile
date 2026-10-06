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
QEMU    := qemu-system-x86_64

# ---- Paths ----------------------------------------------------------------
BUILD  := build$(if $(TEST),-test)
ISO    := $(BUILD)/warden.iso
KERNEL := $(BUILD)/kernel.elf

HOST_BUILD := build-host
HOST_CFLAGS := -std=gnu11 -g -O1 -Wall -Wextra -Werror \
							 -fsanitize=address,undefined -fno-sanitize-recover=all \
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
KSRC_C   += tests/tests.c $(shell find tests/kernel -name '*.c')
endif

# --- kernel assembly source files ---------------------------------------------------
KSRC_ASM := $(shell find kernel -name '*.asm')

# --- kernel object files ---------------------------------------------------
KOBJ     := $(KSRC_C:%.c=$(BUILD)/obj/%.o) $(KSRC_ASM:%.asm=$(BUILD)/obj/%.asm.o)

# ---- QEMU flags -----------------------------------------------------------
QEMUFLAGS := -M q35 -m 512M -cdrom $(ISO) -no-reboot -display none \
    -serial stdio

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

$(BUILD)/obj/%.o: %.c | $(DEPS_STAMP)
	@mkdir -p $(@D)
	$(CC) $(KCFLAGS) -c $< -o $@

-include $(KOBJ:.o=.d)


# --- ISO ----------------------------------------------------------------------
.PHONY: iso
iso: $(ISO)
	
$(ISO): $(KERNEL) boot/limine.conf $(DEPS_STAMP)
	rm -rf $(BUILD)/iso_root
	mkdir -p $(BUILD)/iso_root/boot/limine $(BUILD)/iso_root/EFI/BOOT
	cp $(KERNEL) $(BUILD)/iso_root/boot/kernel
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
.PHONY: test host-test
test: host-test                      ## Run all tests
	$(MAKE) TEST=1 iso
	./tests/qemu/run.sh build-test/warden.iso build-test/serial.log

HOST_TESTS := path fmt
$(HOST_BUILD)/test_path: kernel/path.c
$(HOST_BUILD)/test_fmt: kernel/fmt.c

host-test: $(HOST_TESTS:%=$(HOST_BUILD)/test_%)   ## Unit tests on the host, with sanitizers
	@for t in $^; do ./$$t || exit 1; done

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
	bear -- $(MAKE) kernel

lsp: clean compdb                    ## Regenerate compile_commands.json from a clean build (fixes stale LSP)

help:                                ## Show available targets
	@grep -hE '^[a-zA-Z_-]+:.*## ' $(MAKEFILE_LIST) | \
	  awk -F':.*## ' '{printf "  \033[36m%-12s\033[0m %s\n", $$1, $$2}'
