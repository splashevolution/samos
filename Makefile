# ============================================================
# SAM OS — Makefile
# Produces: build/sam_os.iso  (bootable via GRUB2 multiboot2)
#
# Prerequisites (install on Ubuntu/Debian/Lubuntu):
#   sudo apt update
#   sudo apt install -y \
#       nasm \
#       gcc-x86-64-linux-gnu \
#       binutils-x86-64-linux-gnu \
#       grub-pc-bin \
#       grub-common \
#       xorriso \
#       mtools \
#       qemu-system-x86
#
# Note: We use the HOST gcc with -m64 and a custom linker script.
# For full cross-compilation replace CC with x86_64-elf-gcc if available.
#
# Usage:
#   make          — build sam_os.iso
#   make run      — boot in QEMU (no display, serial output)
#   make run-vga  — boot in QEMU with VGA window
#   make clean    — remove build artefacts
# ============================================================

SHELL    := /bin/bash
export PATH := /usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:$(PATH)

TARGET   := sam_os
BUILD    := build
ISO_DIR  := $(BUILD)/iso
BOOT_DIR := $(ISO_DIR)/boot
GRUB_DIR := $(BOOT_DIR)/grub

# ── Toolchain ────────────────────────────────────────────────
ASM      := nasm
CC       := gcc
LD       := ld

ASMFLAGS := -f elf64

CFLAGS   := -m64 \
             -std=c11 \
             -ffreestanding \
             -fno-builtin \
             -fno-stack-protector \
             -fno-pie \
             -mno-red-zone \
             -msse4.2 \
             -O2 \
             -fno-tree-vectorize \
             -Wall \
             -Wextra \
             -Ikernel

LDFLAGS  := -m elf_x86_64 \
             -T kernel/linker.ld \
             -nostdlib \
             -z max-page-size=0x1000 \
             -Map $(BUILD)/kernel.map

# ── Sources ───────────────────────────────────────────────────
ASM_SRC  := kernel/boot.asm kernel/userasm.asm
C_SRC    := kernel/main.c

ASM_OBJ  := $(BUILD)/boot.o $(BUILD)/userasm.o
C_OBJ    := $(BUILD)/main.o
KERNEL   := $(BUILD)/$(TARGET).elf
ISO      := $(BUILD)/$(TARGET).iso

# ── Default target ────────────────────────────────────────────
.PHONY: all run run-vga clean

all: $(ISO)

# ── Compile assembly ─────────────────────────────────────────
$(BUILD)/boot.o: kernel/boot.asm | $(BUILD)
	$(ASM) $(ASMFLAGS) $< -o $@

$(BUILD)/userasm.o: kernel/userasm.asm | $(BUILD)
	$(ASM) $(ASMFLAGS) $< -o $@

# ── Kernel headers (any change triggers C recompile) ─────────
KERNEL_HEADERS := kernel/mcp.h kernel/fb.h kernel/boot_config.h \
                  kernel/panic.h kernel/idt.h kernel/syscall.h kernel/vfs.h \
                  kernel/acpi.h kernel/ps2kbd.h kernel/ata.h \
                  kernel/wizard.h kernel/shell.h kernel/simd.h \
                  kernel/scheduler.h kernel/stf.h

# ── Compile C ────────────────────────────────────────────────
$(C_OBJ): $(C_SRC) $(KERNEL_HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# ── Link kernel ELF ──────────────────────────────────────────
$(KERNEL): $(ASM_OBJ) $(C_OBJ) kernel/linker.ld | $(BUILD)
	$(LD) $(LDFLAGS) $(ASM_OBJ) $(C_OBJ) -o $@

# ── Build ISO ────────────────────────────────────────────────
GGUF_TOOL := tools/make_gguf_header.py
GGUF_FILE := $(BUILD)/test.gguf
STF_TOOL  := tools/to_stf.py
STF_FILE  := $(BUILD)/test_model.stf

$(GGUF_FILE): $(GGUF_TOOL) | $(BUILD)
	python3 $(GGUF_TOOL)

$(STF_FILE): $(STF_TOOL) | $(BUILD)
	python3 $(STF_TOOL) --synthetic --output $(STF_FILE)

# ── Sprint 16: initrd (ustar) with the first SAM OS user program ─────
USER_CODE := user/hello.asm
HELLO_BIN := $(BUILD)/hello.bin
INITRD    := $(BUILD)/initrd.tar

$(HELLO_BIN): $(USER_CODE) | $(BUILD)
	nasm -f bin -o $@ $<

$(INITRD): $(HELLO_BIN) | $(BUILD)
	rm -rf $(BUILD)/initrd
	mkdir -p $(BUILD)/initrd
	cp $(HELLO_BIN) $(BUILD)/initrd/hello.bin
	tar --format=ustar -cf $@ -C $(BUILD)/initrd hello.bin

$(ISO): $(KERNEL) $(GGUF_FILE) $(STF_FILE) $(INITRD) | $(GRUB_DIR)
	cp $(KERNEL)    $(BOOT_DIR)/$(TARGET).elf
	cp $(GGUF_FILE) $(BOOT_DIR)/test.gguf
	cp $(STF_FILE)  $(BOOT_DIR)/test_model.stf
	cp $(INITRD)    $(BOOT_DIR)/initrd.tar
	cp kernel/grub.cfg $(GRUB_DIR)/grub.cfg
	GRUB_MKRESCUE_XORRISO=/usr/bin/xorriso /usr/bin/grub-mkrescue -o $@ $(ISO_DIR)
	@echo ""
	@echo "  ============================================"
	@echo "  SAM OS ISO built: $@"
	@echo "  Flash to USB:  sudo dd if=$@ of=/dev/sdX bs=4M"
	@echo "  Or run QEMU:   make run-vga"
	@echo "  ============================================"

# ── Create directories ────────────────────────────────────────
$(BUILD):
	mkdir -p $(BUILD)

$(GRUB_DIR):
	mkdir -p $(GRUB_DIR)

# ── QEMU targets ─────────────────────────────────────────────
QEMU     := qemu-system-x86_64
QEMUOPTS := -m 512M \
             -no-reboot \
             -cdrom $(ISO) \
             -enable-kvm \
             -cpu host

run: $(ISO)
	$(QEMU) $(QEMUOPTS) -display none -serial mon:stdio

run-vga: $(ISO)
	$(QEMU) $(QEMUOPTS) -vga std

# Sprint 7: run with VGA window so you can see and play Pong
run-game: $(ISO)
	$(QEMU) $(QEMUOPTS) -vga std -serial mon:stdio

# ── CI ISO: same payload, grub config passes "ci" to skip the wizard ──
CI_ISO := $(BUILD)/sam_os_ci.iso

.PHONY: iso-ci
iso-ci: $(KERNEL) $(GGUF_FILE) $(STF_FILE) $(INITRD) | $(GRUB_DIR)
	cp $(KERNEL)    $(BOOT_DIR)/$(TARGET).elf
	cp $(GGUF_FILE) $(BOOT_DIR)/test.gguf
	cp $(STF_FILE)  $(BOOT_DIR)/test_model.stf
	cp $(INITRD)    $(BOOT_DIR)/initrd.tar
	cp kernel/grub.ci.cfg $(GRUB_DIR)/grub.cfg
	GRUB_MKRESCUE_XORRISO=/usr/bin/xorriso /usr/bin/grub-mkrescue -o $(CI_ISO) $(ISO_DIR)

# ── Headless test (Phase 1: truth stabilization) ─────────────
# Boots the ISO in QEMU, captures serial output, checks for PASS strings.
# Exits 0 on success, 1 if any expected marker is missing or FAIL is found.
# Requires: qemu-system-x86_64 with KVM or TCG fallback.
#
# Expected markers (all must appear in serial output):
#   [PASS] dot product
#   [PASS] matmul
#   [PASS] benchmark
#   [PASS] STF
#   Sprint 16 PASS   (ring-3 hello process)
#
SERIAL_LOG := $(BUILD)/serial.log
TEST_TIMEOUT := 45   # seconds before QEMU is killed (TCG is slow)

.PHONY: test
test: iso-ci
	@echo "=== SAM OS headless test ($(TEST_TIMEOUT)s timeout) ==="
	@rm -f $(SERIAL_LOG)
	@timeout $(TEST_TIMEOUT) $(QEMU) \
	    -m 512M -no-reboot -cdrom $(CI_ISO) \
	    -display none \
	    -serial file:$(SERIAL_LOG) \
	    -cpu qemu64 \
	    2>/dev/null || true
	@echo "--- Serial output ---"
	@cat $(SERIAL_LOG) 2>/dev/null || echo "(no serial output captured)"
	@echo "--- Test checks ---"
	@PASS=1; \
	for marker in \
	    "dot\(\[1\.\.32\]" \
	    "\[PASS\]" \
	    "Sprint 16 PASS"; \
	do \
	    if grep -qE "$$marker" $(SERIAL_LOG) 2>/dev/null; then \
	        echo "  [OK]  $$marker"; \
	    else \
	        echo "  [MISSING] $$marker"; \
	        PASS=0; \
	    fi; \
	done; \
	if grep -q "\[FAIL\]" $(SERIAL_LOG) 2>/dev/null; then \
	    echo "  [FAIL lines found in serial output:]"; \
	    grep "\[FAIL\]" $(SERIAL_LOG); \
	    PASS=0; \
	fi; \
	if [ "$$PASS" = "1" ]; then \
	    echo "=== make test: PASS ==="; \
	else \
	    echo "=== make test: FAIL ==="; \
	    exit 1; \
	fi

# ── Clean ─────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD)
