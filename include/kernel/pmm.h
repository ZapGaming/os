#ifndef KERNEL_PMM_H
#define KERNEL_PMM_H

#include <stdint.h>
#include <kernel/multiboot.h>

#define PMM_FRAME_SIZE 4096

void pmm_init(const struct mb_parsed_info *info);
uint32_t pmm_alloc_frame(void);
void pmm_free_frame(uint32_t frame_addr);
uint32_t pmm_free_frame_count(void);

/* Narrow, explicit escape hatch into the sub-1MB region pmm_init()
 * marks entirely reserved (see mark_used_range(0, 0x100000) in pmm.c).
 * This does NOT reopen the general allocator onto low memory --
 * pmm_alloc_frame() still never returns anything below 1MB. It only
 * lets a caller that already knows exactly which physical page it
 * needs (currently just the AP trampoline -- kernel/apic.c -- which the
 * x86 SIPI protocol requires to sit below 1MB and 4KB-aligned) claim
 * that one specific frame, once, with a clear paper trail in the serial
 * log, instead of removing the whole-region reservation wholesale (which
 * would let ordinary allocations wander into low memory and risk
 * clobbering BIOS data areas other code might assume are stable).
 *
 * phys_addr must be page-aligned and below 0x100000. Returns phys_addr
 * back on success (for convenient chaining), or 0 if the address is
 * invalid or a low frame has already been claimed -- this MVP only
 * ever needs to hand out exactly one. */
uint32_t pmm_alloc_low_frame(uint32_t phys_addr);

#endif
