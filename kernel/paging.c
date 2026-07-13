#include <kernel/paging.h>
#include <kernel/pmm.h>
#include <kernel/serial.h>
#include <stdint.h>

#define PAGE_PRESENT 0x01
#define PAGE_RW      0x02
#define PAGE_USER    0x04
#define PAGE_PSE     0x80 /* 4MB page (only meaningful on a PDE) */

/* Identity-map the full 4 GiB address space using 4 MiB pages (PSE), so
 * physical == virtual everywhere. This keeps every other subsystem (the
 * framebuffer in particular, whose physical address can land almost
 * anywhere) trivially addressable without a higher-half kernel or
 * on-demand page tables yet.
 *
 * This is also the directory every task still uses unless it was given
 * its own isolated one (see paging_new_isolated_directory()) -- fully
 * user-accessible, exactly as before per-process isolation existed. It
 * stays that way deliberately: the compiled-in ring-3 demo task
 * (kernel/demo_user_task.c) is kernel-authored code linked directly
 * into the kernel image, not something loaded from an untrusted file,
 * so leaving it on the original shared mapping is a documented scope
 * choice, not an oversight -- real isolation is for code this kernel
 * didn't write itself, i.e. ELF binaries loaded from disk. */
static uint32_t page_directory[1024] __attribute__((aligned(4096)));

void paging_init(void) {
    for (uint32_t i = 0; i < 1024; i++) {
        page_directory[i] = (i * 0x400000) | (PAGE_PSE | PAGE_USER | PAGE_RW | PAGE_PRESENT);
    }

    __asm__ volatile (
        "mov %0, %%cr3\n"
        "mov %%cr4, %%eax\n"
        "or $0x10, %%eax\n"   /* CR4.PSE */
        "mov %%eax, %%cr4\n"
        "mov %%cr0, %%eax\n"
        "or $0x80000000, %%eax\n" /* CR0.PG */
        "mov %%eax, %%cr0\n"
        :
        : "r"(page_directory)
        : "eax", "memory"
    );

    serial_printf("paging: identity-mapped 4GB with 4MB pages\n");
}

uint32_t paging_kernel_directory_phys(void) {
    return (uint32_t)page_directory;
}

void paging_switch_directory(uint32_t dir_phys) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(dir_phys) : "memory");
}

uint32_t paging_new_isolated_directory(void) {
    uint32_t dir_phys = pmm_alloc_frame();
    if (!dir_phys) return 0;

    /* Safe to treat as a pointer directly: whichever directory is
     * CURRENTLY active (this always runs from kernel code, under
     * either the original kernel directory or another already-
     * isolated one) identity-maps all physical RAM for supervisor
     * use, so this frame's physical address doubles as a valid
     * kernel-mode virtual address regardless of which CR3 is loaded
     * right now. */
    uint32_t *dir = (uint32_t *)dir_phys;
    for (uint32_t i = 0; i < 1024; i++) {
        dir[i] = (i * 0x400000) | (PAGE_PSE | PAGE_RW | PAGE_PRESENT); /* no PAGE_USER */
    }
    return dir_phys;
}

int paging_map_user_page(uint32_t dir_phys, uint32_t virt, uint32_t phys) {
    uint32_t *dir = (uint32_t *)dir_phys;
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    uint32_t pde = dir[pd_index];
    uint32_t *table;
    if (!(pde & PAGE_PRESENT) || (pde & PAGE_PSE)) {
        /* First private page in this 4MB region -- replace whatever
         * was here (nothing, or the restricted identity super-page)
         * with a fresh, empty 4KB page table. Nothing of value lives
         * in the identity super-page it replaces: this only ever runs
         * on PDEs inside the ELF loader's reserved user window, which
         * the kernel itself never uses for anything. */
        uint32_t table_phys = pmm_alloc_frame();
        if (!table_phys) return 0;
        table = (uint32_t *)table_phys;
        for (int i = 0; i < 1024; i++) table[i] = 0;
        dir[pd_index] = (table_phys & 0xFFFFF000) | (PAGE_USER | PAGE_RW | PAGE_PRESENT);
    } else {
        table = (uint32_t *)(pde & 0xFFFFF000);
    }

    table[pt_index] = (phys & 0xFFFFF000) | (PAGE_USER | PAGE_RW | PAGE_PRESENT);
    return 1;
}

void paging_free_isolated_directory(uint32_t dir_phys) {
    uint32_t *dir = (uint32_t *)dir_phys;
    for (int i = 0; i < 1024; i++) {
        uint32_t pde = dir[i];
        /* Only a PDE this task's own paging_map_user_page() calls
         * replaced (present, 4KB page table, user-accessible) is ours
         * to free -- everything else is either not present or still
         * part of the shared supervisor-only identity map. */
        if (!(pde & PAGE_PRESENT) || (pde & PAGE_PSE) || !(pde & PAGE_USER)) continue;

        uint32_t *table = (uint32_t *)(pde & 0xFFFFF000);
        for (int j = 0; j < 1024; j++) {
            uint32_t pte = table[j];
            if (pte & PAGE_PRESENT) pmm_free_frame(pte & 0xFFFFF000);
        }
        pmm_free_frame((uint32_t)table);
    }
    pmm_free_frame(dir_phys);
}
