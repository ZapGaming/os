#ifndef KERNEL_TSS_H
#define KERNEL_TSS_H

#include <stdint.h>

#define TSS_SELECTOR 0x28

void tss_init(void);
void tss_set_kernel_stack(uint32_t esp0);

#endif
