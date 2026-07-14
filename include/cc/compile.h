#ifndef CC_COMPILE_H
#define CC_COMPILE_H

#include <stdint.h>

/* The one entry point gui/shell.c's `cc` command needs. Compiles
 * `source` (a single translation unit, `source_len` bytes, need not be
 * NUL-terminated) all the way through to a complete ELF32 executable
 * matching kernel/elf.c's loader (see include/cc/elf_writer.h).
 *
 * On success, returns 1 and fills *out_elf (kmalloc'd -- caller must
 * kfree() it) and *out_elf_len.
 *
 * On failure (lex/parse/codegen/link error -- anything from a syntax
 * error to "no main function" to "program too large for the load
 * window"), returns 0 and writes a single NUL-terminated, human-
 * readable line into errbuf (truncated to fit errbuf_size); *out_elf is
 * untouched. */
int cc_compile(const char *source, uint32_t source_len, uint8_t **out_elf, uint32_t *out_elf_len,
               char *errbuf, uint32_t errbuf_size);

#endif
