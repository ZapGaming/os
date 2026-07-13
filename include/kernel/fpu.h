#ifndef KERNEL_FPU_H
#define KERNEL_FPU_H

#include <stdint.h>

#define FPU_STATE_SIZE 108 /* legacy 32-bit FSAVE/FRSTOR image, no alignment requirement */

/* Enables the x87 FPU (clears CR0.EM, sets CR0.MP) and runs FNINIT,
 * capturing the resulting clean image so every task can start from it
 * -- called once at boot, before the first task is created. */
void fpu_init(void);

/* Copies the clean post-FNINIT FPU image into dst (FPU_STATE_SIZE
 * bytes) -- used to seed a newly created task's saved FPU state so its
 * first FRSTOR (on first switch-in) doesn't load garbage/zeroed
 * control words. */
void fpu_get_clean_state(void *dst);

#endif
