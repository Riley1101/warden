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
BUILD  := build
ISO    := $(BUILD)/warden.iso
KERNEL := $(BUILD)/kernel.elf

# Files that prove dependencies were fetched (so `make deps` runs only once)
DEPS_STAMP := limine/limine third_party/limine.h

# ---- kernel build flags ----------------------------------------------------
KCFLAGS := -g -O2 -pipe -std=gnu11 -Wall -Wextra -Werror \
    -ffreestanding -fno-stack-protector -fno-stack-check -fno-lto \
    -fno-PIC -fno-pie -ffunction-sections -fdata-sections \
    -m64 -march=x86-64 -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
    -mno-red-zone -mcmodel=kernel \
    -MMD -MP \
    -Ikernel -Ikernel/include -Ithird_party

# ---- kernel link flags ------------------------------------------------------
KLDFLAGS := -m elf_x86_64 -nostdlib -static \
    -z max-page-size=0x1000 --gc-sections -T kernel/linker.ld

# ---- kernel source files ---------------------------------------------------
KSRC_C   := $(shell find kernel -name '*.c')

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


# ---- Build / run / test (filled in during Steps 2-4) ------------------------
.PHONY: run run-debug 
run : $(ISO)                        ## Run the kernel in QEMU
	$(QEMU) $(QEMUFLAGS)

run-debug : $(ISO)                  ## Run the kernel in QEMU with GDB stub
	$(QEMU) $(QEMUFLAGS) -s -S -d int,guest_errors -D $(BUILD)/qemu.log
 
 # ---- Tests (Steps 3-4) -----------------------------------------------------
.PHONY: test host-test
test: host-test                      ## Run all tests
	@echo "TODO (Step 3): headless QEMU boot test"

host-test:                           ## Unit tests on the host, with sanitizers
	@echo "TODO (Step 4): host unit tests"

# ---- Housekeeping ---------------------------------------------------------
.PHONY: clean distclean compdb lsp help

clean:                               ## Remove build output
	rm -rf $(BUILD) iso_root *.log

distclean: clean                     ## Also remove fetched deps (keeps vendored limine.h)
	rm -rf limine

compdb:                              ## Generate compile_commands.json for your LSP
	bear -- $(MAKE) kernel

lsp: clean compdb                    ## Regenerate compile_commands.json from a clean build (fixes stale LSP)

help:                                ## Show available targets
	@grep -hE '^[a-zA-Z_-]+:.*## ' $(MAKEFILE_LIST) | \
	  awk -F':.*## ' '{printf "  \033[36m%-12s\033[0m %s\n", $$1, $$2}'
