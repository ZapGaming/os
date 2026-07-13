CC      := gcc
LD      := ld
ASM     := nasm

GCC_FREESTANDING_INC := $(shell gcc -m32 -print-file-name=include)

CFLAGS  := -m32 -std=gnu11 -ffreestanding -fno-pie -fno-stack-protector \
           -fno-builtin -nostdlib -nostdinc -Wall -Wextra -O2 \
           -mno-sse -mno-sse2 -mno-mmx -mno-80387 -mgeneral-regs-only \
           -Iinclude -isystem $(GCC_FREESTANDING_INC) -MMD -MP
LDFLAGS := -m elf_i386 -T linker.ld -nostdlib
ASFLAGS := -f elf32

BUILD   := build
ISODIR  := isodir

C_SOURCES   := $(shell find boot kernel drivers gui net fs -name '*.c')
ASM_SOURCES := $(shell find boot kernel drivers gui net fs -name '*.asm')

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

$(BUILD)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(ASM) $(ASFLAGS) $< -o $@

$(KERNEL): $(OBJECTS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS)

-include $(C_OBJECTS:.o=.d)

iso: $(KERNEL)
	@mkdir -p $(ISODIR)/boot/grub
	cp $(KERNEL) $(ISODIR)/boot/kernel.elf
	cp iso/grub.cfg $(ISODIR)/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) $(ISODIR)

run: iso disk
	qemu-system-i386 -boot order=d -cdrom $(ISO) -drive file=$(DISK),format=raw,if=ide,index=0 \
		-serial stdio -m 256M -netdev user,id=net0 -device rtl8139,netdev=net0

clean:
	rm -rf $(BUILD) $(ISODIR) $(ISO)
