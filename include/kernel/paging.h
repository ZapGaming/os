#ifndef KERNEL_PAGING_H
#define KERNEL_PAGING_H

#include <stdint.h>

void paging_init(void);

/* Physical address of the original, fully-identity-mapped-and-user-
 * accessible directory paging_init() sets up -- every task that isn't
 * individually isolated (the boot/GUI task, background kernel tasks,
 * and the compiled-in ring-3 demo task) runs under this one, unchanged
 * from before per-process isolation existed. */
uint32_t paging_kernel_directory_phys(void);

/* Loads CR3 -- also flushes the TLB, which is required (not just
 * incidental) here: switching between tasks with different private
 * mappings for the same virtual address depends on stale translations
 * actually being dropped. */
void paging_switch_directory(uint32_t dir_phys);

/* Allocates a fresh page directory that identity-maps all of physical
 * memory the same way the kernel directory does, but SUPERVISOR-ONLY
 * (no user bit) -- so kernel code (interrupt/syscall handlers, the
 * scheduler, drivers) keeps working unmodified no matter which task's
 * directory is currently loaded, while ring-3 code running under this
 * directory faults immediately on touching any of it. Callers add
 * their own private, user-accessible mappings on top via
 * paging_map_user_page(). Returns the new directory's physical address
 * (also a valid kernel pointer, since it's carved out of the same
 * identity-mapped physical memory), or 0 on allocation failure. */
uint32_t paging_new_isolated_directory(void);

/* Maps one 4KB page of `phys` at `virt` inside the directory at
 * `dir_phys`, present + writable + user-accessible. Replaces that
 * virt's covering 4MB entry with a fresh, empty 4KB page table on
 * first use in a given 4MB region (allocated via pmm_alloc_frame()),
 * so a directory can host private pages inside what was previously
 * part of the shared supervisor-only identity map. Returns 1 on
 * success, 0 if a frame allocation failed. */
int paging_map_user_page(uint32_t dir_phys, uint32_t virt, uint32_t phys);

/* Reclaims every private (user-accessible) frame and page table this
 * directory owns, plus the directory itself -- called once a task
 * that had its own isolated directory has fully exited, so repeated
 * program launches don't leak physical memory. Never touches anything
 * still marked supervisor-only (the shared identity map), since that's
 * not this directory's to free. */
void paging_free_isolated_directory(uint32_t dir_phys);

#endif
