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
ASM_SOURCES := $(shell find boot kernel drivers gui net fs js -name '*.asm')

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
