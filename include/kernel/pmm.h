#ifndef KERNEL_PMM_H
#define KERNEL_PMM_H

#include <stdint.h>
#include <kernel/multiboot.h>

#define PMM_FRAME_SIZE 4096

void pmm_init(const struct mb_parsed_info *info);
uint32_t pmm_alloc_frame(void);
void pmm_free_frame(uint32_t frame_addr);
uint32_t pmm_free_frame_count(void);

#endif
