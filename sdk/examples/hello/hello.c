/* Minimal ZapOS SDK example -- the "full gcc path" (see sdk/README.md
 * section 2). Mirrors userprogs/hello.c in spirit (same entry-point
 * convention, same syscalls), just built against sdk/zapos.h instead
 * of hand-rolling the inline asm wrappers again.
 *
 * Build + run:
 *   gcc -m32 -std=gnu11 -ffreestanding -fno-pie -fno-stack-protector \
 *       -fno-builtin -nostdlib -O2 -mno-sse -mno-sse2 -mno-mmx \
 *       -mno-80387 -mgeneral-regs-only -I../.. \
 *       -c hello.c -o hello.o
 *   ld -m elf_i386 -T ../../../userprogs/user.ld -nostdlib \
 *       -o HELLO.ELF hello.o
 * Then get HELLO.ELF onto zapos_disk.img (see sdk/README.md section 2
 * for the exact mechanism this repo already uses for TEST.ELF/DOOM.ELF)
 * and launch it from the File Manager (double-click) or the Terminal
 * (`run HELLO.ELF`).
 */
#include "../../zapos.h"

/* The ELF loader (kernel/elf.c) jumps straight to this entry point with
 * no C runtime set up beforehand -- no argc/argv, no libc, no main()
 * wrapper. That "main() shim" convention only exists for the in-kernel
 * `cc` compiler's OWN tiny subset (see cc/compile.c) -- a real gcc-built
 * program like this one defines _start() directly. */
void _start(void) {
    for (int i = 0; i < 5; i++) {
        zos_write("hello from the ZapOS SDK -- a real ELF binary loaded off disk!\n");
        zos_yield();
    }
    zos_exit();
}
