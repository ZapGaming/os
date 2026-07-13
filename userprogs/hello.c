/* A real, standalone ELF32 executable -- built and linked completely
 * outside the kernel image, then dropped onto the FAT32 disk image as
 * TEST.ELF. Loaded and run via kernel/elf.c's ELF loader, exercised
 * from the File Manager. Uses the exact same int-0x80 ABI as
 * kernel/demo_user_task.c (SYS_WRITE=1, SYS_YIELD=2, SYS_EXIT=0) since
 * it runs at ring 3 with no other way to reach the kernel. */

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

void _start(void) {
    for (int i = 0; i < 5; i++) {
        sys_write("hello from TEST.ELF, a real ELF binary loaded off disk!\n");
        sys_yield();
    }
    sys_exit();
}
