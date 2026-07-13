/* Deliberately misbehaves, to prove kernel/elf.c's per-process
 * isolation actually contains it rather than just being decoration:
 * writes to a kernel-image address (0x00100000, well below this
 * program's own 0x04000000+ window) that is present but supervisor-
 * only in this task's own page directory. Before per-process
 * isolation existed, every task shared one identity-mapped, fully
 * user-accessible address space, so this same write would have
 * silently corrupted the kernel instead of faulting. */

#define SYS_EXIT  0
#define SYS_WRITE 1

static inline int sys_write(const char *s) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(SYS_WRITE), "b"(s));
    return ret;
}

static inline void sys_exit(void) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_EXIT));
}

void _start(void) {
    sys_write("EVIL.ELF: about to touch kernel memory at 0x00100000...\n");
    *(volatile unsigned int *)0x00100000 = 0xDEADBEEF;
    sys_write("EVIL.ELF: if you see this, isolation FAILED\n");
    sys_exit();
}
