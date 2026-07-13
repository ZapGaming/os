#include <kernel/elf.h>
#include <kernel/scheduler.h>
#include <kernel/paging.h>
#include <kernel/pmm.h>
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

/* Fixed window reserved for user programs -- a loaded program's own
 * linker script has to target this same range (see userprogs/user.ld).
 * Every task now gets a PRIVATE mapping for its own slice of this
 * window (see paging_new_isolated_directory()/paging_map_user_page()
 * in kernel/paging.c) rather than sharing one identity-mapped window
 * across every task -- two different loaded programs can both use
 * vaddr 0x04000000 and genuinely not see each other's memory, since
 * each program's page directory maps that address to its own private
 * physical frames. The window itself just bounds how big a program's
 * segments (and its stack, carved out of the top of it -- see
 * USER_STACK_TOP below) are allowed to be; it's not shared storage. */
#define USER_LOAD_MIN 0x04000000u /* 64MB */
#define USER_LOAD_MAX 0x0F000000u /* 240MB */

/* The stack lives at a fixed offset near the top of the same window,
 * comfortably above where any realistically-sized program's own
 * segments (which start at USER_LOAD_MIN and grow upward) would reach
 * -- checked explicitly below rather than just assumed. */
#define USER_STACK_TOP (USER_LOAD_MIN + 0x00F00000u) /* 15MB into the window */

#define PAGE_SIZE 4096u
static uint32_t page_floor(uint32_t x) { return x & ~(PAGE_SIZE - 1); }
static uint32_t page_ceil(uint32_t x) { return (x + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1); }

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

    /* Validate every segment before mapping anything -- a partially
     * set-up address space is harder to reason about than simply not
     * starting. */
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
        uint32_t seg_start = page_floor(ph->p_vaddr);
        uint32_t seg_end = page_ceil(ph->p_vaddr + ph->p_memsz);
        if (seg_end > USER_STACK_TOP - TASK_STACK_SIZE && seg_start < USER_STACK_TOP) {
            serial_printf("elf: segment %d reaches into the reserved stack region\n", i);
            return -1;
        }
    }

    uint32_t dir_phys = paging_new_isolated_directory();
    if (!dir_phys) {
        serial_printf("elf: out of memory allocating a page directory\n");
        return -1;
    }

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf32_phdr *ph = (const struct elf32_phdr *)
            (data + eh->e_phoff + (uint32_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;

        uint32_t seg_start = page_floor(ph->p_vaddr);
        uint32_t seg_end = page_ceil(ph->p_vaddr + ph->p_memsz);

        for (uint32_t page_vaddr = seg_start; page_vaddr < seg_end; page_vaddr += PAGE_SIZE) {
            uint32_t frame = pmm_alloc_frame();
            if (!frame) {
                serial_printf("elf: out of memory allocating program pages\n");
                paging_free_isolated_directory(dir_phys);
                return -1;
            }
            memset((void *)frame, 0, PAGE_SIZE);

            /* Copy whatever part of this page overlaps the segment's
             * file-backed range; anything beyond p_filesz (BSS) stays
             * zero from the memset above. */
            uint32_t file_start = ph->p_vaddr;
            uint32_t file_end = ph->p_vaddr + ph->p_filesz;
            uint32_t ov_start = page_vaddr > file_start ? page_vaddr : file_start;
            uint32_t ov_end = (page_vaddr + PAGE_SIZE) < file_end ? (page_vaddr + PAGE_SIZE) : file_end;
            if (ov_start < ov_end) {
                memcpy((uint8_t *)frame + (ov_start - page_vaddr),
                       data + ph->p_offset + (ov_start - file_start),
                       ov_end - ov_start);
            }

            if (!paging_map_user_page(dir_phys, page_vaddr, frame)) {
                serial_printf("elf: out of memory building page tables\n");
                pmm_free_frame(frame);
                paging_free_isolated_directory(dir_phys);
                return -1;
            }
        }
    }

    for (uint32_t off = 0; off < TASK_STACK_SIZE; off += PAGE_SIZE) {
        uint32_t frame = pmm_alloc_frame();
        if (!frame) {
            serial_printf("elf: out of memory allocating the user stack\n");
            paging_free_isolated_directory(dir_phys);
            return -1;
        }
        memset((void *)frame, 0, PAGE_SIZE);
        if (!paging_map_user_page(dir_phys, USER_STACK_TOP - TASK_STACK_SIZE + off, frame)) {
            serial_printf("elf: out of memory building page tables\n");
            pmm_free_frame(frame);
            paging_free_isolated_directory(dir_phys);
            return -1;
        }
    }

    struct task *t = task_create_user_isolated((void (*)(void))eh->e_entry, dir_phys, USER_STACK_TOP);
    if (!t) {
        serial_printf("elf: task_create_user_isolated failed\n");
        paging_free_isolated_directory(dir_phys);
        return -1;
    }
    serial_printf("elf: loaded, entry=%x pid=%d (isolated address space)\n", eh->e_entry, t->pid);
    return t->pid;
}
