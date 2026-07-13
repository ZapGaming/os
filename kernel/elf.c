#include <kernel/elf.h>
#include <kernel/scheduler.h>
#include <kernel/serial.h>
#include <string.h>

#define EI_NIDENT 16

struct elf32_header {
    uint8_t e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

struct elf32_phdr {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} __attribute__((packed));

#define ET_EXEC 2
#define EM_386 3
#define PT_LOAD 1

/* Fixed window reserved for user programs -- there's no per-process
 * page directories yet (every task shares this one identity-mapped
 * address space), so a loaded program's segments just have to fall
 * inside a range known not to collide with the kernel's own image
 * plus its 32MB heap arena (kernel_end measures ~33.24 MiB). A
 * program's linker script must target this same range. */
#define USER_LOAD_MIN 0x04000000u /* 64MB -- comfortably above kernel_end */
#define USER_LOAD_MAX 0x0F000000u /* 240MB -- comfortably below the 256MB QEMU is run with */

static int in_user_window(uint32_t addr, uint32_t size) {
    if (addr < USER_LOAD_MIN) return 0;
    if (size > USER_LOAD_MAX - addr) return 0; /* overflow-safe: addr+size > USER_LOAD_MAX */
    return 1;
}

int elf_load_and_run(const uint8_t *data, uint32_t len) {
    if (len < sizeof(struct elf32_header)) {
        serial_printf("elf: file too small for an ELF header\n");
        return -1;
    }

    const struct elf32_header *eh = (const struct elf32_header *)data;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') {
        serial_printf("elf: bad magic\n");
        return -1;
    }
    if (eh->e_ident[4] != 1 /* ELFCLASS32 */) {
        serial_printf("elf: not a 32-bit executable\n");
        return -1;
    }
    if (eh->e_ident[5] != 1 /* ELFDATA2LSB */) {
        serial_printf("elf: not little-endian\n");
        return -1;
    }
    if (eh->e_type != ET_EXEC) {
        serial_printf("elf: not a static executable (e_type=%d)\n", eh->e_type);
        return -1;
    }
    if (eh->e_machine != EM_386) {
        serial_printf("elf: not i386 (e_machine=%d)\n", eh->e_machine);
        return -1;
    }
    if (eh->e_phnum == 0) {
        serial_printf("elf: no program headers\n");
        return -1;
    }

    uint32_t phtable_end = eh->e_phoff + (uint32_t)eh->e_phentsize * eh->e_phnum;
    if (eh->e_phoff > len || phtable_end > len || phtable_end < eh->e_phoff ||
        eh->e_phentsize < sizeof(struct elf32_phdr)) {
        serial_printf("elf: program header table out of bounds\n");
        return -1;
    }

    if (!in_user_window(eh->e_entry, 1)) {
        serial_printf("elf: entry point %x outside allowed range\n", eh->e_entry);
        return -1;
    }

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf32_phdr *ph = (const struct elf32_phdr *)
            (data + eh->e_phoff + (uint32_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;

        if (ph->p_filesz > ph->p_memsz) {
            serial_printf("elf: segment %d has filesz > memsz\n", i);
            return -1;
        }
        if ((uint64_t)ph->p_offset + ph->p_filesz > len) {
            serial_printf("elf: segment %d file range out of bounds\n", i);
            return -1;
        }
        if (!in_user_window(ph->p_vaddr, ph->p_memsz)) {
            serial_printf("elf: segment %d vaddr=%x memsz=%x outside allowed range\n",
                          i, ph->p_vaddr, ph->p_memsz);
            return -1;
        }

        memcpy((void *)ph->p_vaddr, data + ph->p_offset, ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz) {
            memset((void *)(ph->p_vaddr + ph->p_filesz), 0, ph->p_memsz - ph->p_filesz);
        }
    }

    struct task *t = task_create_user((void (*)(void))eh->e_entry);
    if (!t) {
        serial_printf("elf: task_create_user failed\n");
        return -1;
    }
    serial_printf("elf: loaded, entry=%x pid=%d\n", eh->e_entry, t->pid);
    return t->pid;
}
