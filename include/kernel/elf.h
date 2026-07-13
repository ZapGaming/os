#ifndef KERNEL_ELF_H
#define KERNEL_ELF_H

#include <stdint.h>

/* Parses a 32-bit ELF executable already sitting in memory (`data`,
 * `len` bytes -- e.g. just read whole off the FAT32 disk), copies its
 * PT_LOAD segments to the virtual addresses they specify (identity-
 * mapped, so a vaddr doubles as a real pointer) and zero-fills any
 * BSS beyond each segment's file size, then creates and starts a new
 * ring-3 task at the ELF's entry point.
 *
 * Deliberately narrow: only a static (ET_EXEC, not PIE/shared/
 * relocatable), 32-bit little-endian, i386 executable is accepted,
 * and every PT_LOAD segment must fall inside a fixed address range
 * (see elf.c) reserved for user programs -- there's no per-process
 * page directories yet (see the README's roadmap), so every task
 * still shares this one identity-mapped address space, and a loaded
 * program's own linker script has to target a range known not to
 * collide with the kernel, by convention rather than enforced
 * isolation.
 *
 * Returns the new task's pid (>= 0) on success, -1 on any validation
 * failure (logged to serial with the specific reason). */
int elf_load_and_run(const uint8_t *data, uint32_t len);

#endif
