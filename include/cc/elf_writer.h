#ifndef CC_ELF_WRITER_H
#define CC_ELF_WRITER_H

#include <cc/module.h>

/* Must match kernel/elf.c's USER_LOAD_MIN/USER_LOAD_MAX exactly (and
 * userprogs/user.ld's link base) -- there's no shared header between
 * cc/ and kernel/elf.c to pull these from without creating a
 * dependency neither side otherwise needs, so they're duplicated here
 * deliberately, the same way userprogs/user.ld independently hardcodes
 * 0x04000000 rather than including anything from kernel/. */
#define CC_USER_LOAD_MIN 0x04000000u
#define CC_USER_LOAD_MAX 0x0F000000u

/* Resolves every relocation cc/codegen.c recorded (global variable and
 * string-literal addresses, function call targets -- see
 * include/cc/module.h's cc_reloc) now that every section's size (and
 * therefore its final base address in the fixed load window) and every
 * function's text_offset are known, then serializes m->text/rodata/
 * data plus a single PT_LOAD program header into a complete, valid
 * ELF32 ET_EXEC/EM_386 byte image -- exactly the shape kernel/elf.c's
 * loader accepts (see its doc comment on USER_LOAD_MIN/MAX). Bss isn't
 * written to the file at all; the segment's p_memsz simply extends
 * p_filesz bytes further so the loader zero-fills it (see
 * kernel/elf.c's PT_LOAD handling).
 *
 * `entry_func_index` is main's index into m->funcs (see
 * cc/compile.c) -- the ELF header's e_entry is computed from that
 * function's own text_offset, since compile.c has already appended a
 * tiny `_start`-equivalent stub (call main; SYS_EXIT) as its own
 * function-shaped chunk of text and this just points e_entry straight
 * at IT, not at main directly (see cc/compile.c).
 *
 * On success returns 1 and fills *out_buf (kmalloc'd -- caller
 * kfree()s it) / *out_len. Returns 0 (m->error/m->errmsg already set)
 * if the whole image wouldn't fit inside the fixed user-program load
 * window. */
int cc_elf_link_and_write(struct cc_module *m, int entry_func_index, uint8_t **out_buf, uint32_t *out_len);

#endif
