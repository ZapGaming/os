#include <kernel/fpu.h>
#include <string.h>

static uint8_t fpu_clean_state[FPU_STATE_SIZE];

void fpu_init(void) {
    /* CR0.EM (bit 2) set means "no FPU, trap to #NM on any x87
     * instruction"; CR0.MP (bit 1) governs whether WAIT/FWAIT also
     * trap while a task switch is pending. Clearing EM and setting MP
     * is the standard "real FPU, please" configuration -- without it,
     * FNINIT/FNSAVE below would fault. FNSAVE's documented side effect
     * re-initializes the FPU after saving, so the capture below leaves
     * the FPU in the same clean state it started in. */
    __asm__ volatile (
        "mov %%cr0, %%eax\n"
        "and $0xFFFFFFFB, %%eax\n"
        "or $0x2, %%eax\n"
        "mov %%eax, %%cr0\n"
        "fninit\n"
        "fnsave %0\n"
        : "=m"(fpu_clean_state)
        :
        : "eax"
    );
}

void fpu_get_clean_state(void *dst) {
    memcpy(dst, fpu_clean_state, FPU_STATE_SIZE);
}
