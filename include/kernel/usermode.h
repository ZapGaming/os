#ifndef KERNEL_USERMODE_H
#define KERNEL_USERMODE_H

#include <stdint.h>

void enter_usermode(void (*entry)(void), uint32_t user_stack_top);

#endif
