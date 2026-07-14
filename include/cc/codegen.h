#ifndef CC_CODEGEN_H
#define CC_CODEGEN_H

/* Codegen for the C subset cc/ compiles: `int` and pointers-to-`int`/
 * pointers-to-pointers only -- no float/double. Same house rule this
 * whole kernel already follows for exactly this reason (see
 * include/js/js.h's doc comment on why the JS engine is int32-only,
 * and net/wasm.c's i32-only scope cut): the kernel, and therefore this
 * self-hosted compiler's OWN source, is built with -mno-sse
 * -mno-80387 -mgeneral-regs-only, so no FPU/SSE state is ever
 * initialized -- emitting or even compiling a float instruction is a
 * hard compile error, not a style choice. There is consequently no
 * `float`/`double` (and no `char` either -- a string literal is just
 * an address into a byte blob, see cc/parser.c, which is all this
 * subset's builtin print() needs).
 *
 * Codegen strategy: a correctness-first stack-machine style -- every
 * expression evaluates into EAX, using PUSH/POP on the real x86 stack
 * for temporaries instead of a register allocator (matches this
 * codebase's established "pragmatic subset, not optimized" calibration
 * -- see js/interp.c's tree-walking evaluator). No struct/union/enum in
 * this pass -- see the top-level report for what's cut vs. what's a
 * nice-to-have for later. */
#include <cc/ast.h>
#include <cc/module.h>

/* Emits every builtin runtime function's machine code straight into
 * m->text and registers it into m->funcs -- must run before
 * cc_codegen() so ordinary user calls to print()/print_int()/etc.
 * resolve through the normal function-call path (see cc/builtins.c). */
void cc_register_builtins(struct cc_module *m);

/* Two passes over `program`'s top-level declarations: Pass 1 registers
 * every function's signature and every global variable's section/
 * offset/size (no code emitted yet, so forward references and mutual
 * recursion both just work); Pass 2 emits each function body's machine
 * code into m->text. Returns 1 on success, 0 on failure (m->error/
 * m->errmsg already set by whichever check failed). */
int cc_codegen(struct cc_module *m, struct cc_node *program);

#endif
