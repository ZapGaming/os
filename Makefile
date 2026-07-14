CC      := gcc
LD      := ld
ASM     := nasm

GCC_FREESTANDING_INC := $(shell gcc -m32 -print-file-name=include)

CFLAGS  := -m32 -std=gnu11 -ffreestanding -fno-pie -fno-stack-protector \
           -fno-builtin -nostdlib -nostdinc -Wall -Wextra -O2 \
           -mno-sse -mno-sse2 -mno-mmx -mno-80387 -mgeneral-regs-only \
           -Iinclude -isystem $(GCC_FREESTANDING_INC) -MMD -MP

# py/*.c (the Python-subset interpreter) is the one part of this kernel
# that needs real floating point (a tagged int/float value union, per
# js/js.h's comment explaining why the JS engine deliberately avoids
# this). -mgeneral-regs-only and -mno-80387 make emitting any float
# instruction at all a hard compile error, so those two flags are
# dropped here -- everything else, including -mno-sse/-mno-sse2/-mno-mmx,
# stays: the per-task FPU save/restore added in kernel/fpu.c only
# FSAVE/FRSTORs the x87 register file, not SSE/XMM state, so float math
# must be forced through x87 (which -mno-sse already does on this
# target) rather than SSE for it to survive a context switch correctly.
PY_CFLAGS := -m32 -std=gnu11 -ffreestanding -fno-pie -fno-stack-protector \
           -fno-builtin -nostdlib -nostdinc -Wall -Wextra -O2 \
           -mno-sse -mno-sse2 -mno-mmx \
           -Iinclude -isystem $(GCC_FREESTANDING_INC) -MMD -MP

LDFLAGS := -m elf_i386 -T linker.ld -nostdlib
ASFLAGS := -f elf32

BUILD   := build
ISODIR  := isodir

C_SOURCES   := $(shell find boot kernel drivers gui net fs js py -name '*.c')

# boot/ap_trampoline.asm is excluded here -- it's raw 16-bit real-mode
# code that must run at a fixed low physical address (see that file's
# header comment), assembled with its own `nasm -f bin` rule below
# instead of the generic `-f elf32` pattern rule every other .asm file
# uses. boot/ap_trampoline_blob.asm (which embeds the resulting flat
# binary into the normal kernel image) stays in this glob like any
# other .asm file.
ASM_SOURCES := $(filter-out boot/ap_trampoline.asm, $(shell find boot kernel drivers gui net fs js -name '*.asm'))

C_OBJECTS   := $(patsubst %.c,$(BUILD)/%.o,$(C_SOURCES))
ASM_OBJECTS := $(patsubst %.asm,$(BUILD)/%.o,$(ASM_SOURCES))
OBJECTS     := $(ASM_OBJECTS) $(C_OBJECTS)

KERNEL  := $(BUILD)/kernel.elf
ISO     := zapos.iso
DISK    := zapos_disk.img

.PHONY: all clean run iso disk

all: $(KERNEL)

$(DISK):
	./tools/make_disk_image.sh

disk: $(DISK)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# More specific than the generic rule above (shorter stem: "lexer" vs
# "py/lexer"), so GNU Make prefers this one for anything under py/ --
# no changes needed to the generic rule.
$(BUILD)/py/%.o: py/%.c
	@mkdir -p $(dir $@)
	$(CC) $(PY_CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(ASM) $(ASFLAGS) $< -o $@

# Flat real-mode AP trampoline (see boot/ap_trampoline.asm's header
# comment) -- assembled as a raw binary, NOT a normal elf32 object,
# since it must run at a fixed low physical address rather than
# wherever the linker would place a regular .o. boot/ap_trampoline_blob.asm
# incbin's this .bin to pull the bytes into the normal kernel image; its
# object file explicitly depends on this one so it's always rebuilt
# first. This overrides the generic `%.o: %.asm` pattern rule above for
# this one target (an explicit rule with an extra prerequisite beats a
# pattern rule), which is why boot/ap_trampoline.asm itself is filtered
# out of ASM_SOURCES above -- otherwise Make would also try to build it
# as a plain elf32 object, which fails (ORG is invalid in that format).
$(BUILD)/boot/ap_trampoline.bin: boot/ap_trampoline.asm
	@mkdir -p $(dir $@)
	$(ASM) -f bin $< -o $@

$(BUILD)/boot/ap_trampoline_blob.o: boot/ap_trampoline_blob.asm $(BUILD)/boot/ap_trampoline.bin
	@mkdir -p $(dir $@)
	$(ASM) $(ASFLAGS) $< -o $@

$(KERNEL): $(OBJECTS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS)

-include $(C_OBJECTS:.o=.d)

# The disk image is baked directly into the ISO as a GRUB module (see
# iso/grub.cfg's `module2` line and drivers/ata.c's RAM-disk fallback),
# so zapos.iso alone -- no separate file to carry around -- boots with
# the full filesystem, DOOM.ELF included.
iso: $(KERNEL) $(DISK)
	@mkdir -p $(ISODIR)/boot/grub
	cp $(KERNEL) $(ISODIR)/boot/kernel.elf
	cp $(DISK) $(ISODIR)/boot/disk.img
	cp iso/grub.cfg $(ISODIR)/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) $(ISODIR)

# Real ATA hardware disk still takes priority over the embedded module
# (see kernel.c), so attaching zapos_disk.img as a second drive here
# keeps behaving exactly as before -- persisting writes across reboots.
run: iso disk
	qemu-system-i386 -boot order=d -cdrom $(ISO) -drive file=$(DISK),format=raw,if=ide,index=0 \
		-serial stdio -m 512M -netdev user,id=net0 -device rtl8139,netdev=net0 -device AC97

# Boots zapos.iso with NO second drive at all -- proves the ISO is
# self-contained (embedded disk image module + RAM-disk fallback).
run-iso-only: iso
	qemu-system-i386 -boot order=d -cdrom $(ISO) \
		-serial stdio -m 512M -netdev user,id=net0 -device rtl8139,netdev=net0 -device AC97

clean:
	rm -rf $(BUILD) $(ISODIR) $(ISO)
