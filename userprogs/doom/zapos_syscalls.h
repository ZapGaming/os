#ifndef ZAPOS_SYSCALLS_H
#define ZAPOS_SYSCALLS_H

/* Mirrors include/kernel/syscall.h's numbering -- this ELF program has
 * no way to #include a kernel header (different build, no shared -I),
 * so the numbers are just duplicated here. Keep the two in sync. */
#define SYS_EXIT      0
#define SYS_WRITE     1
#define SYS_YIELD     2
#define SYS_GET_TICKS 3
#define SYS_SLEEP     4
#define SYS_POLL_KEY  5
#define SYS_BLIT      6

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

static inline unsigned int sys_get_ticks(void) {
    unsigned int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(SYS_GET_TICKS));
    return ret;
}

static inline void sys_sleep(unsigned int ms) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_SLEEP), "b"(ms));
}

/* Returns -1 if no key event is pending, else (pressed<<8)|scancode. */
static inline int sys_poll_key(void) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(SYS_POLL_KEY));
    return ret;
}

static inline void sys_blit(const void *pixels) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_BLIT), "b"(pixels));
}

#endif
