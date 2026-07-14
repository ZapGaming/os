/* Top-level orchestration: lex -> parse -> codegen -> link/serialize.
 * See include/cc/compile.h. The only extra thing this stage does
 * beyond wiring the other four stages together is build the tiny
 * "_start" stub (call main; SYS_EXIT) that becomes the ELF's actual
 * entry point -- kernel/elf.c jumps straight to e_entry with no
 * runtime/libc to call main() for it, so something has to, and
 * whatever it is also has to make sure the process actually exits via
 * SYS_EXIT if/when main() returns instead of falling off the end of
 * mapped memory into whatever garbage bytes follow. */
#include <cc/compile.h>
#include <cc/lexer.h>
#include <cc/parser.h>
#include <cc/codegen.h>
#include <cc/elf_writer.h>
#include <cc/emit.h>
#include <kernel/syscall.h>
#include <kernel/kheap.h>
#include <string.h>

static void copy_error(struct cc_module *m, char *errbuf, uint32_t errbuf_size) {
    if (!errbuf || errbuf_size == 0) return;
    strncpy(errbuf, m->errmsg, errbuf_size - 1);
    errbuf[errbuf_size - 1] = 0;
}

int cc_compile(const char *source, uint32_t source_len, uint8_t **out_elf, uint32_t *out_elf_len,
               char *errbuf, uint32_t errbuf_size) {
    /* kmalloc'd, NOT a stack local: struct cc_module (include/cc/module.h)
     * is dominated by its fixed-size CC_MAX_RELOCS=4096 relocation table
     * (4096 * sizeof(struct cc_reloc) = 48KiB alone) plus the funcs/
     * globals tables -- ~71KiB total, REGARDLESS of how small the
     * program being compiled is. That's already bigger than this
     * kernel's entire 64KiB boot stack (boot/boot.asm's stack_bottom/
     * stack_top -- what task pid 0, e.g. gui/shell.c's `cc` command,
     * always runs on) and dwarfs the 16KiB per-task stack every other
     * task gets (kernel/scheduler.h's TASK_STACK_SIZE). Every physical
     * page is identity-mapped RW with no guard page (paging_init()'s 4MB
     * large pages cover the whole 4GB range) so a stack-local overflow
     * of that size doesn't fault -- it silently scribbles over whatever
     * memory happens to sit below the stack, corrupting arbitrary state
     * without necessarily crashing anything, which is exactly the kind
     * of bug that hides until something later stumbles over the
     * corruption (caught here by calling cc_compile() twice in a row
     * from the same call chain -- see the temporary self-test this fix
     * unblocked). kmalloc gives it its own >64KiB heap allocation
     * instead, entirely off any task's stack. */
    struct cc_module *m = (struct cc_module *)kmalloc(sizeof(struct cc_module));
    if (!m) {
        if (errbuf && errbuf_size) { strncpy(errbuf, "out of memory allocating compiler state", errbuf_size - 1); errbuf[errbuf_size - 1] = 0; }
        return 0;
    }
    cc_module_init(m);
    cc_register_builtins(m);

    struct cc_lexer lx;
    cc_lexer_init(&lx, source, source_len);
    if (lx.error) {
        if (errbuf && errbuf_size) { strncpy(errbuf, lx.errmsg, errbuf_size - 1); errbuf[errbuf_size - 1] = 0; }
        cc_module_free(m);
        kfree(m);
        return 0;
    }

    struct cc_node *program = cc_parse_program(&lx, m);
    if (lx.error && !m->error) cc_module_errorf(m, lx.cur.line, "%s", lx.errmsg);
    if (m->error) { copy_error(m, errbuf, errbuf_size); cc_module_free(m); kfree(m); return 0; }

    if (!cc_codegen(m, program)) { copy_error(m, errbuf, errbuf_size); cc_module_free(m); kfree(m); return 0; }

    struct cc_func *mainf = cc_find_func(m, "main");
    if (!mainf || !mainf->has_body) {
        cc_module_errorf(m, 0, "no 'main' function found");
        copy_error(m, errbuf, errbuf_size); cc_module_free(m); kfree(m); return 0;
    }
    if (mainf->param_count != 0) {
        cc_module_errorf(m, 0, "'main' must take no parameters in this subset");
        copy_error(m, errbuf, errbuf_size); cc_module_free(m); kfree(m); return 0;
    }
    int main_idx = (int)(mainf - m->funcs);

    struct cc_func *start = cc_add_func(m, "_start");
    if (!start) { copy_error(m, errbuf, errbuf_size); cc_module_free(m); kfree(m); return 0; }
    start->has_body = 1;
    start->is_void = 1;
    start->text_offset = m->text.len;
    {
        uint32_t at = emit_mov_reg_imm32(&m->text, EAX, 0);
        cc_add_reloc(m, at, CC_SEC_FUNC, (uint32_t)main_idx);
        emit_call_reg(&m->text, EAX);
        /* main()'s return value is in EAX already; SYS_EXIT ignores it
         * (task_exited() takes no code -- see kernel/syscall.c), but
         * there's no reason to disturb it before the trap either. */
        emit_mov_reg_imm32(&m->text, EAX, SYS_EXIT);
        emit_int80(&m->text);
    }
    int start_idx = (int)(start - m->funcs);

    uint8_t *buf;
    uint32_t len;
    if (!cc_elf_link_and_write(m, start_idx, &buf, &len)) {
        copy_error(m, errbuf, errbuf_size);
        cc_module_free(m);
        kfree(m);
        return 0;
    }

    *out_elf = buf;
    *out_elf_len = len;
    cc_module_free(m);
    kfree(m);
    return 1;
}
