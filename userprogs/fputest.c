/* Proves the scheduler's per-task FPU state save/restore (kernel/fpu.c,
 * switch_task.asm) actually works: compiled twice with different
 * constants (see tools/make_disk_image.sh), each variant multiplies two
 * floats every iteration and checks the exact expected result, yielding
 * after each check to force a context switch. If FPU state leaked
 * between tasks, one variant's registers/control word would eventually
 * get clobbered by the other's arithmetic mid-computation and the check
 * would fail. `volatile` on the operands stops the compiler from just
 * constant-folding the whole thing at compile time -- the multiply has
 * to genuinely happen on real FPU registers every iteration. */

#define SYS_EXIT  0
#define SYS_WRITE 1
#define SYS_YIELD 2

static inline int sys_write(const char *s) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(SYS_WRITE), "b"(s));
    return ret;
}

static inline void sys_yield(void) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_YIELD));
}

static inline void sys_exit(void) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_EXIT));
}

static void write_int(int v) {
    char buf[16];
    int i = 14;
    buf[15] = 0;
    int neg = v < 0;
    unsigned int u = neg ? (unsigned int)(-v) : (unsigned int)v;
    if (u == 0) buf[i--] = '0';
    while (u) { buf[i--] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) buf[i--] = '-';
    sys_write(&buf[i + 1]);
}

#define ITERATIONS 100000

void _start(void) {
    sys_write(FPUTEST_NAME ": starting float cross-contamination check\n");

    int fails = 0;
    for (int i = 0; i < ITERATIONS; i++) {
        volatile float x = FPUTEST_A;
        volatile float y = FPUTEST_B;
        float z = x * y;
        int iz = (int)z;
        if (iz != FPUTEST_EXPECT) fails++;
        sys_yield();
    }

    if (fails == 0) {
        sys_write(FPUTEST_NAME ": PASS -- all iterations correct\n");
    } else {
        sys_write(FPUTEST_NAME ": FAIL -- corrupted ");
        write_int(fails);
        sys_write(" times\n");
    }
    sys_exit();
}
