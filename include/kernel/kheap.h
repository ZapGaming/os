#ifndef KERNEL_KHEAP_H
#define KERNEL_KHEAP_H

#include <stddef.h>

void kheap_init(void);
void *kmalloc(size_t size);
void kfree(void *ptr);

#endif
