#include <kernel/paging.h>
#include <kernel/serial.h>
#include <stdint.h>

/* Identity-map the full 4 GiB address space using 4 MiB pages (PSE), so
 * physical == virtual everywhere. This keeps every other subsystem (the
 * framebuffer in particular, whose physical address can land almost
 * anywhere) trivially addressable without a higher-half kernel or
 * on-demand page tables yet. */
static uint32_t page_directory[1024] __attribute__((aligned(4096)));

void paging_init(void) {
    for (uint32_t i = 0; i < 1024; i++) {
        page_directory[i] = (i * 0x400000) | 0x83; /* present, rw, 4MB page */
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
