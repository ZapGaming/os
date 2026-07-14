/* Codegen: walks the AST cc/parser.c built and emits x86-32 machine
 * code straight into m->text (no assembler involved -- see
 * include/cc/codegen.h for the overall strategy). Every expression
 * leaves its result in EAX; PUSH/POP on the real machine stack hold
 * temporaries across sub-expression evaluation, which is what makes
 * this "stack machine" rather than a register allocator.
 *
 * Two passes, run back to back by cc_codegen():
 *   Pass 1 (register_decls): walks the top-level list once, assigning
 *     every function a slot in m->funcs and every global variable a
 *     section+offset+size in m->globals -- no machine code emitted.
 *     This is what lets a function call another one defined later in
 *     the same file (or itself, or mutually with another function):
 *     by the time Pass 2 emits any call site, every possible callee
 *     already has a m->funcs entry, even if that entry's text_offset
 *     isn't filled in yet (see CC_SEC_FUNC in include/cc/module.h --
 *     resolved in cc/compile.c's final link pass instead).
 *   Pass 2 (gen_function): emits each function with a body, in
 *     whatever order they appear in the source.
 *
 * Local variables get one flat per-function name table (struct
 * cc_func_ctx.locals), built by a small pre-walk (collect_locals) that
 * runs before any code for that function is emitted, so `sub esp, N`
 * in the prologue can reserve the whole frame up front. This is
 * simpler than tracking real block scopes at the cost of a known,
 * documented gap: two variables of the same name in unrelated sibling
 * blocks collide instead of shadowing block-locally, and a variable is
 * technically visible slightly before its declaration point within the
 * same function. Neither matters for straight-line typical C written
 * against this subset; see the top-level report. */
#include <cc/codegen.h>
#include <cc/emit.h>
#include <string.h>

#define CC_MAX_LOCALS 256
#define CC_MAX_LOOP_DEPTH 16
#define CC_MAX_LOOP_PATCHES 64
#define CC_MAX_RETURN_PATCHES 128

struct cc_local {
    char name[64];
    struct cc_type type;
    int offset; /* ebp-relative; params are >=8, locals are negative */
};

struct cc_loop_ctx {
    uint32_t break_patches[CC_MAX_LOOP_PATCHES];
    int break_count;
    uint32_t continue_patches[CC_MAX_LOOP_PATCHES];
    int continue_count;
};

struct cc_func_ctx {
    struct cc_module *m;
    struct cc_local locals[CC_MAX_LOCALS];
    int local_count;
    int next_offset; /* next (more negative) free local slot; 0 before any local is allocated */

    uint32_t return_patches[CC_MAX_RETURN_PATCHES];
    int return_patch_count;

    struct cc_loop_ctx loops[CC_MAX_LOOP_DEPTH];
    int loop_depth;
};

static void gen_stmt(struct cc_func_ctx *ctx, struct cc_node *node);
static void gen_expr(struct cc_func_ctx *ctx, struct cc_node *node);
static void gen_addr(struct cc_func_ctx *ctx, struct cc_node *node);
static int expr_ptr_depth(struct cc_func_ctx *ctx, struct cc_node *node);

/* ---- Local symbol table --------------------------------------------- */

static struct cc_local *find_local(struct cc_func_ctx *ctx, const char *name) {
    for (int i = ctx->local_count - 1; i >= 0; i--) /* most-recently-added wins (best-effort shadowing) */
        if (strcmp(ctx->locals[i].name, name) == 0) return &ctx->locals[i];
    return NULL;
}

static struct cc_local *add_local(struct cc_func_ctx *ctx, const char *name, struct cc_type type, int line) {
    if (ctx->local_count >= CC_MAX_LOCALS) {
        cc_module_errorf(ctx->m, line, "too many local variables/parameters in one function");
        return NULL;
    }
    int count = (type.is_array && type.array_len > 0) ? type.array_len : 1;
    ctx->next_offset -= 4 * count;
    struct cc_local *l = &ctx->locals[ctx->local_count++];
    strncpy(l->name, name, sizeof(l->name) - 1);
    l->name[sizeof(l->name) - 1] = 0;
    l->type = type;
    l->offset = ctx->next_offset;
    return l;
}

/* Pre-walk: finds every CC_VAR_DECL reachable in `node` (recursing into
 * blocks/if/while/for) and gives it a stack slot -- run once, before
 * any code for the function is emitted, so the prologue's `sub esp, N`
 * already knows the whole frame size. */
static void collect_locals(struct cc_func_ctx *ctx, struct cc_node *node) {
    if (!node || ctx->m->error) return;
    switch (node->type) {
        case CC_VAR_DECL: {
            struct cc_local *l = add_local(ctx, node->u.var_decl.name, node->u.var_decl.type, node->line);
            node->u.var_decl.local_index = l ? (int)(l - ctx->locals) : -1;
            break;
        }
        case CC_BLOCK:
            for (struct cc_node *s = node->u.block.stmts; s; s = s->next) collect_locals(ctx, s);
            break;
        case CC_IF:
            collect_locals(ctx, node->u.if_stmt.then_branch);
            collect_locals(ctx, node->u.if_stmt.else_branch);
            break;
        case CC_WHILE:
            collect_locals(ctx, node->u.while_stmt.body);
            break;
        case CC_FOR:
            collect_locals(ctx, node->u.for_stmt.init);
            collect_locals(ctx, node->u.for_stmt.body);
            break;
        default:
            break; /* expressions in this subset never contain declarations */
    }
}

/* Best-effort static type inference -- just enough to know whether an
 * expression's value is a pointer (and therefore whether +/-/++/--
 * needs to scale by 4) or a plain int. Not a real type checker: it
 * never rejects anything by itself, callers just use the ptr_depth to
 * choose how to scale arithmetic. See include/cc/codegen.h. */
static int expr_ptr_depth(struct cc_func_ctx *ctx, struct cc_node *node) {
    if (!node) return 0;
    switch (node->type) {
        case CC_IDENT: {
            struct cc_local *l = find_local(ctx, node->u.ident.name);
            if (l) return l->type.is_array ? l->type.ptr_depth + 1 : l->type.ptr_depth;
            struct cc_global *g = cc_find_global(ctx->m, node->u.ident.name);
            if (g) return g->type.is_array ? g->type.ptr_depth + 1 : g->type.ptr_depth;
            return 0;
        }
        case CC_DEREF: { int d = expr_ptr_depth(ctx, node->u.addr_deref.operand); return d > 0 ? d - 1 : 0; }
        case CC_ADDR: return expr_ptr_depth(ctx, node->u.addr_deref.operand) + 1;
        case CC_INDEX: { int d = expr_ptr_depth(ctx, node->u.index_expr.array); return d > 0 ? d - 1 : 0; }
        case CC_STR_LIT: return 1;
        case CC_CALL: {
            struct cc_func *f = cc_find_func(ctx->m, node->u.call.name);
            return f ? f->ret_type.ptr_depth : 0;
        }
        case CC_ASSIGN: return expr_ptr_depth(ctx, node->u.assign.target);
        case CC_BINARY: {
            if (strcmp(node->u.binary.op, "-") == 0) {
                int dl = expr_ptr_depth(ctx, node->u.binary.left);
                int dr = expr_ptr_depth(ctx, node->u.binary.right);
                if (dl > 0 && dr > 0) return 0; /* pointer difference is an int */
                return dl > dr ? dl : dr;
            }
            if (strcmp(node->u.binary.op, "+") == 0) {
                int dl = expr_ptr_depth(ctx, node->u.binary.left);
                int dr = expr_ptr_depth(ctx, node->u.binary.right);
                return dl > dr ? dl : dr;
            }
            return 0;
        }
        default: return 0;
    }
}

/* ---- Expression codegen --------------------------------------------- */

static void gen_call_args_reverse(struct cc_func_ctx *ctx, struct cc_node *args) {
    if (!args) return;
    gen_call_args_reverse(ctx, args->next);
    gen_expr(ctx, args);
    emit_push_reg(&ctx->m->text, EAX);
}

static void gen_call(struct cc_func_ctx *ctx, struct cc_node *node) {
    struct cc_module *m = ctx->m;
    struct cc_func *f = cc_find_func(m, node->u.call.name);
    if (!f) { cc_module_errorf(m, node->line, "call to undefined function '%s'", node->u.call.name); return; }
    int argc = 0;
    for (struct cc_node *a = node->u.call.args; a; a = a->next) argc++;
    if (argc != f->param_count) {
        cc_module_errorf(m, node->line, "'%s' expects %d argument(s)", node->u.call.name, f->param_count);
        return;
    }
    gen_call_args_reverse(ctx, node->u.call.args);
    if (m->error) return;
    int idx = (int)(f - m->funcs);
    uint32_t at = emit_mov_reg_imm32(&m->text, EAX, 0);
    cc_add_reloc(m, at, CC_SEC_FUNC, (uint32_t)idx);
    emit_call_reg(&m->text, EAX);
    if (argc > 0) emit_add_reg_imm32(&m->text, ESP, (uint32_t)(4 * argc));
}

static void gen_incdec(struct cc_func_ctx *ctx, struct cc_node *operand, int is_inc, int is_post) {
    struct cc_buf *b = &ctx->m->text;
    int delta = expr_ptr_depth(ctx, operand) > 0 ? 4 : 1;
    gen_addr(ctx, operand); /* eax = address */
    if (ctx->m->error) return;
    emit_mov_reg_reg(b, ECX, EAX);
    emit_mov_reg_mem(b, EDX, ECX, 0); /* edx = old value */
    if (is_post) emit_mov_reg_reg(b, EAX, EDX); /* result = old value, decided now before EDX changes */
    if (is_inc) emit_add_reg_imm32(b, EDX, (uint32_t)delta);
    else emit_sub_reg_imm32(b, EDX, (uint32_t)delta);
    emit_mov_mem_reg(b, ECX, 0, EDX);
    if (!is_post) emit_mov_reg_reg(b, EAX, EDX); /* result = new value */
}

static const char *cmp_ops[] = { "==", "!=", "<", ">", "<=", ">=", NULL };
static enum cc_cond cmp_conds[] = { CC_EQ, CC_NE, CC_LT, CC_GT, CC_LE, CC_GE };

static void gen_binary(struct cc_func_ctx *ctx, struct cc_node *node) {
    struct cc_buf *b = &ctx->m->text;
    const char *op = node->u.binary.op;
    struct cc_node *left = node->u.binary.left, *right = node->u.binary.right;

    if (strcmp(op, "/") == 0 || strcmp(op, "%") == 0) {
        gen_expr(ctx, left);
        emit_push_reg(b, EAX);
        gen_expr(ctx, right);
        if (ctx->m->error) return;
        emit_mov_reg_reg(b, ECX, EAX); /* ecx = divisor */
        emit_pop_reg(b, EAX);          /* eax = dividend */
        emit_cdq(b);
        emit_idiv_reg(b, ECX);
        if (strcmp(op, "%") == 0) emit_mov_reg_reg(b, EAX, EDX);
        return;
    }

    gen_expr(ctx, left);
    emit_push_reg(b, EAX);
    gen_expr(ctx, right);
    if (ctx->m->error) return;
    emit_pop_reg(b, ECX); /* ecx = left, eax = right */

    for (int i = 0; cmp_ops[i]; i++) {
        if (strcmp(op, cmp_ops[i]) == 0) {
            emit_cmp_reg_reg(b, ECX, EAX); /* left - right */
            emit_setcc_al(b, cmp_conds[i]);
            emit_movzx_eax_al(b);
            return;
        }
    }

    if (strcmp(op, "&") == 0) { emit_and_reg_reg(b, ECX, EAX); emit_mov_reg_reg(b, EAX, ECX); return; }
    if (strcmp(op, "|") == 0) { emit_or_reg_reg(b, ECX, EAX); emit_mov_reg_reg(b, EAX, ECX); return; }
    if (strcmp(op, "^") == 0) { emit_xor_reg_reg(b, ECX, EAX); emit_mov_reg_reg(b, EAX, ECX); return; }

    if (strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0) {
        emit_mov_reg_reg(b, EDX, ECX); /* edx = value to shift */
        emit_mov_reg_reg(b, ECX, EAX); /* ecx = shift count (only CL is used) */
        if (strcmp(op, "<<") == 0) emit_shl_reg_cl(b, EDX);
        else emit_sar_reg_cl(b, EDX);
        emit_mov_reg_reg(b, EAX, EDX);
        return;
    }

    if (strcmp(op, "*") == 0) { emit_imul_reg_reg(b, ECX, EAX); emit_mov_reg_reg(b, EAX, ECX); return; }

    if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0) {
        int dl = expr_ptr_depth(ctx, left), dr = expr_ptr_depth(ctx, right);
        if (strcmp(op, "+") == 0) {
            if (dl > 0 && dr == 0) { emit_shl_reg_imm8(b, EAX, 2); emit_add_reg_reg(b, ECX, EAX); emit_mov_reg_reg(b, EAX, ECX); return; }
            if (dl == 0 && dr > 0) { emit_shl_reg_imm8(b, ECX, 2); emit_add_reg_reg(b, EAX, ECX); return; }
            emit_add_reg_reg(b, ECX, EAX); emit_mov_reg_reg(b, EAX, ECX); return;
        } else {
            if (dl > 0 && dr > 0) { emit_sub_reg_reg(b, ECX, EAX); emit_sar_reg_imm8(b, ECX, 2); emit_mov_reg_reg(b, EAX, ECX); return; }
            if (dl > 0 && dr == 0) { emit_shl_reg_imm8(b, EAX, 2); emit_sub_reg_reg(b, ECX, EAX); emit_mov_reg_reg(b, EAX, ECX); return; }
            emit_sub_reg_reg(b, ECX, EAX); emit_mov_reg_reg(b, EAX, ECX); return;
        }
    }

    cc_module_errorf(ctx->m, node->line, "unsupported binary operator '%s'", op);
}

static void gen_logical(struct cc_func_ctx *ctx, struct cc_node *node) {
    struct cc_buf *b = &ctx->m->text;
    int is_and = strcmp(node->u.binary.op, "&&") == 0;
    gen_expr(ctx, node->u.binary.left);
    if (ctx->m->error) return;
    emit_test_reg_reg(b, EAX, EAX);
    uint32_t j1 = emit_jcc(b, is_and ? CC_EQ : CC_NE); /* short-circuit */
    gen_expr(ctx, node->u.binary.right);
    if (ctx->m->error) return;
    emit_test_reg_reg(b, EAX, EAX);
    uint32_t j2 = emit_jcc(b, is_and ? CC_EQ : CC_NE);
    emit_mov_reg_imm32(b, EAX, is_and ? 1 : 0);
    uint32_t j3 = emit_jmp(b);
    uint32_t shortcircuit_pos = b->len;
    emit_mov_reg_imm32(b, EAX, is_and ? 0 : 1);
    uint32_t end_pos = b->len;
    cc_patch_rel32(b, j1, shortcircuit_pos);
    cc_patch_rel32(b, j2, shortcircuit_pos);
    cc_patch_rel32(b, j3, end_pos);
}

static void gen_unary(struct cc_func_ctx *ctx, struct cc_node *node) {
    struct cc_buf *b = &ctx->m->text;
    gen_expr(ctx, node->u.unary.operand);
    if (ctx->m->error) return;
    if (strcmp(node->u.unary.op, "-") == 0) emit_neg_reg(b, EAX);
    else if (strcmp(node->u.unary.op, "~") == 0) emit_not_reg(b, EAX);
    else if (strcmp(node->u.unary.op, "!") == 0) {
        emit_test_reg_reg(b, EAX, EAX);
        emit_setcc_al(b, CC_EQ);
        emit_movzx_eax_al(b);
    } else cc_module_errorf(ctx->m, node->line, "unsupported unary operator '%s'", node->u.unary.op);
}

static void gen_expr(struct cc_func_ctx *ctx, struct cc_node *node) {
    if (!node || ctx->m->error) return;
    struct cc_buf *b = &ctx->m->text;
    struct cc_module *m = ctx->m;
    switch (node->type) {
        case CC_NUM_LIT:
            emit_mov_reg_imm32(b, EAX, (uint32_t)node->u.num_lit.value);
            return;
        case CC_STR_LIT: {
            uint32_t at = emit_mov_reg_imm32(b, EAX, 0);
            cc_add_reloc(m, at, CC_SEC_RODATA, node->u.str_lit.rodata_offset);
            return;
        }
        case CC_IDENT: {
            struct cc_local *l = find_local(ctx, node->u.ident.name);
            if (l) {
                if (l->type.is_array) emit_lea_mem(b, EAX, EBP, l->offset);
                else emit_mov_reg_mem(b, EAX, EBP, l->offset);
                return;
            }
            struct cc_global *g = cc_find_global(m, node->u.ident.name);
            if (g) {
                if (g->type.is_array) {
                    uint32_t at = emit_mov_reg_imm32(b, EAX, 0);
                    cc_add_reloc(m, at, g->section, g->offset);
                } else {
                    uint32_t at = emit_mov_reg_absmem(b, EAX, 0);
                    cc_add_reloc(m, at, g->section, g->offset);
                }
                return;
            }
            cc_module_errorf(m, node->line, "use of undeclared identifier '%s'", node->u.ident.name);
            return;
        }
        case CC_ASSIGN: {
            gen_expr(ctx, node->u.assign.value);
            if (m->error) return;
            emit_push_reg(b, EAX);
            gen_addr(ctx, node->u.assign.target);
            if (m->error) return;
            emit_mov_reg_reg(b, ECX, EAX);
            emit_pop_reg(b, EAX);
            emit_mov_mem_reg(b, ECX, 0, EAX);
            return;
        }
        case CC_BINARY: gen_binary(ctx, node); return;
        case CC_LOGICAL: gen_logical(ctx, node); return;
        case CC_UNARY: gen_unary(ctx, node); return;
        case CC_ADDR: gen_addr(ctx, node->u.addr_deref.operand); return;
        case CC_DEREF:
            gen_expr(ctx, node->u.addr_deref.operand);
            if (m->error) return;
            emit_mov_reg_mem(b, EAX, EAX, 0);
            return;
        case CC_PREINC: gen_incdec(ctx, node->u.incdec.operand, 1, 0); return;
        case CC_PREDEC: gen_incdec(ctx, node->u.incdec.operand, 0, 0); return;
        case CC_POSTINC: gen_incdec(ctx, node->u.incdec.operand, 1, 1); return;
        case CC_POSTDEC: gen_incdec(ctx, node->u.incdec.operand, 0, 1); return;
        case CC_INDEX:
            gen_addr(ctx, node);
            if (m->error) return;
            emit_mov_reg_mem(b, EAX, EAX, 0);
            return;
        case CC_CALL: gen_call(ctx, node); return;
        default:
            cc_module_errorf(m, node->line, "unsupported expression");
            return;
    }
}

static void gen_addr(struct cc_func_ctx *ctx, struct cc_node *node) {
    if (!node || ctx->m->error) return;
    struct cc_buf *b = &ctx->m->text;
    struct cc_module *m = ctx->m;
    switch (node->type) {
        case CC_IDENT: {
            struct cc_local *l = find_local(ctx, node->u.ident.name);
            if (l) { emit_lea_mem(b, EAX, EBP, l->offset); return; }
            struct cc_global *g = cc_find_global(m, node->u.ident.name);
            if (g) {
                uint32_t at = emit_mov_reg_imm32(b, EAX, 0);
                cc_add_reloc(m, at, g->section, g->offset);
                return;
            }
            cc_module_errorf(m, node->line, "use of undeclared identifier '%s'", node->u.ident.name);
            return;
        }
        case CC_DEREF:
            gen_expr(ctx, node->u.addr_deref.operand);
            return;
        case CC_INDEX: {
            gen_expr(ctx, node->u.index_expr.array); /* base address (decays if it's an array) */
            if (m->error) return;
            emit_push_reg(b, EAX);
            gen_expr(ctx, node->u.index_expr.index);
            if (m->error) return;
            emit_pop_reg(b, ECX); /* ecx = base */
            emit_shl_reg_imm8(b, EAX, 2);
            emit_add_reg_reg(b, EAX, ECX);
            return;
        }
        default:
            cc_module_errorf(m, node->line, "expression is not assignable");
            return;
    }
}

/* ---- Statement codegen ------------------------------------------------*/

static void gen_stmt(struct cc_func_ctx *ctx, struct cc_node *node) {
    if (!node || ctx->m->error) return;
    struct cc_buf *b = &ctx->m->text;
    struct cc_module *m = ctx->m;
    switch (node->type) {
        case CC_BLOCK:
            for (struct cc_node *s = node->u.block.stmts; s && !m->error; s = s->next) gen_stmt(ctx, s);
            return;
        case CC_EXPR_STMT:
            gen_expr(ctx, node->u.expr_stmt.expr);
            return;
        case CC_VAR_DECL: {
            if (!node->u.var_decl.init) return;
            if (node->u.var_decl.local_index < 0) return; /* collect_locals already errored */
            struct cc_local *l = &ctx->locals[node->u.var_decl.local_index];
            gen_expr(ctx, node->u.var_decl.init);
            if (m->error) return;
            emit_mov_mem_reg(b, EBP, l->offset, EAX);
            return;
        }
        case CC_IF: {
            gen_expr(ctx, node->u.if_stmt.cond);
            if (m->error) return;
            emit_test_reg_reg(b, EAX, EAX);
            uint32_t j_false = emit_jcc(b, CC_EQ);
            gen_stmt(ctx, node->u.if_stmt.then_branch);
            if (node->u.if_stmt.else_branch) {
                uint32_t j_end = emit_jmp(b);
                cc_patch_rel32(b, j_false, b->len);
                gen_stmt(ctx, node->u.if_stmt.else_branch);
                cc_patch_rel32(b, j_end, b->len);
            } else {
                cc_patch_rel32(b, j_false, b->len);
            }
            return;
        }
        case CC_WHILE: {
            if (ctx->loop_depth >= CC_MAX_LOOP_DEPTH) { cc_module_errorf(m, node->line, "loops nested too deeply"); return; }
            struct cc_loop_ctx *lc = &ctx->loops[ctx->loop_depth++];
            lc->break_count = lc->continue_count = 0;
            uint32_t l_cond = b->len;
            gen_expr(ctx, node->u.while_stmt.cond);
            if (m->error) { ctx->loop_depth--; return; }
            emit_test_reg_reg(b, EAX, EAX);
            uint32_t j_end = emit_jcc(b, CC_EQ);
            gen_stmt(ctx, node->u.while_stmt.body);
            uint32_t continue_pos = b->len;
            for (int i = 0; i < lc->continue_count; i++) cc_patch_rel32(b, lc->continue_patches[i], continue_pos);
            uint32_t j_back = emit_jmp(b);
            cc_patch_rel32(b, j_back, l_cond);
            uint32_t l_end = b->len;
            cc_patch_rel32(b, j_end, l_end);
            for (int i = 0; i < lc->break_count; i++) cc_patch_rel32(b, lc->break_patches[i], l_end);
            ctx->loop_depth--;
            return;
        }
        case CC_FOR: {
            gen_stmt(ctx, node->u.for_stmt.init);
            if (m->error) return;
            if (ctx->loop_depth >= CC_MAX_LOOP_DEPTH) { cc_module_errorf(m, node->line, "loops nested too deeply"); return; }
            struct cc_loop_ctx *lc = &ctx->loops[ctx->loop_depth++];
            lc->break_count = lc->continue_count = 0;
            uint32_t l_cond = b->len;
            uint32_t j_end = 0;
            if (node->u.for_stmt.cond) {
                gen_expr(ctx, node->u.for_stmt.cond);
                if (m->error) { ctx->loop_depth--; return; }
                emit_test_reg_reg(b, EAX, EAX);
                j_end = emit_jcc(b, CC_EQ);
            }
            gen_stmt(ctx, node->u.for_stmt.body);
            uint32_t continue_pos = b->len;
            for (int i = 0; i < lc->continue_count; i++) cc_patch_rel32(b, lc->continue_patches[i], continue_pos);
            if (node->u.for_stmt.post) gen_expr(ctx, node->u.for_stmt.post);
            if (m->error) { ctx->loop_depth--; return; }
            uint32_t j_back = emit_jmp(b);
            cc_patch_rel32(b, j_back, l_cond);
            uint32_t l_end = b->len;
            if (node->u.for_stmt.cond) cc_patch_rel32(b, j_end, l_end);
            for (int i = 0; i < lc->break_count; i++) cc_patch_rel32(b, lc->break_patches[i], l_end);
            ctx->loop_depth--;
            return;
        }
        case CC_RETURN: {
            if (node->u.return_stmt.value) {
                gen_expr(ctx, node->u.return_stmt.value);
                if (m->error) return;
            }
            if (ctx->return_patch_count >= CC_MAX_RETURN_PATCHES) {
                cc_module_errorf(m, node->line, "too many return statements in one function");
                return;
            }
            ctx->return_patches[ctx->return_patch_count++] = emit_jmp(b);
            return;
        }
        case CC_BREAK:
            if (ctx->loop_depth == 0) { cc_module_errorf(m, node->line, "'break' outside a loop"); return; }
            {
                struct cc_loop_ctx *lc = &ctx->loops[ctx->loop_depth - 1];
                if (lc->break_count >= CC_MAX_LOOP_PATCHES) { cc_module_errorf(m, node->line, "too many break statements in one loop"); return; }
                lc->break_patches[lc->break_count++] = emit_jmp(b);
            }
            return;
        case CC_CONTINUE:
            if (ctx->loop_depth == 0) { cc_module_errorf(m, node->line, "'continue' outside a loop"); return; }
            {
                struct cc_loop_ctx *lc = &ctx->loops[ctx->loop_depth - 1];
                if (lc->continue_count >= CC_MAX_LOOP_PATCHES) { cc_module_errorf(m, node->line, "too many continue statements in one loop"); return; }
                lc->continue_patches[lc->continue_count++] = emit_jmp(b);
            }
            return;
        default:
            cc_module_errorf(m, node->line, "unsupported statement");
            return;
    }
}

/* ---- Top-level: registration (Pass 1) and function emission (Pass 2) */

static void register_decls(struct cc_module *m, struct cc_node *program) {
    for (struct cc_node *d = program->u.program.decls; d && !m->error; d = d->next) {
        if (d->type == CC_FUNC_DECL) {
            struct cc_func *existing = cc_find_func(m, d->u.func_decl.name);
            if (existing) {
                cc_module_errorf(m, d->line, "redefinition of function '%s'", d->u.func_decl.name);
                return;
            }
            struct cc_func *f = cc_add_func(m, d->u.func_decl.name);
            if (!f) return;
            f->ret_type = d->u.func_decl.ret_type;
            f->is_void = d->u.func_decl.is_void;
            f->has_body = 1;
            int n = 0;
            for (struct cc_node *p = d->u.func_decl.params; p; p = p->next) {
                if (n >= CC_MAX_PARAMS) { cc_module_errorf(m, d->line, "function '%s' has too many parameters (max %d)", d->u.func_decl.name, CC_MAX_PARAMS); return; }
                f->param_types[n++] = p->u.param.type;
            }
            f->param_count = n;
        } else if (d->type == CC_GLOBAL_VAR) {
            if (cc_find_global(m, d->u.global_var.name) || cc_find_func(m, d->u.global_var.name)) {
                cc_module_errorf(m, d->line, "redefinition of '%s'", d->u.global_var.name);
                return;
            }
            struct cc_global *g = cc_add_global(m, d->u.global_var.name);
            if (!g) return;
            g->type = d->u.global_var.type;
            int count = (g->type.is_array && g->type.array_len > 0) ? g->type.array_len : 1;
            g->size = 4u * (uint32_t)count;
            if (d->u.global_var.has_init) {
                g->section = CC_SEC_DATA;
                g->offset = m->data.len;
                cc_buf_push_u32(&m->data, (uint32_t)d->u.global_var.init_value);
            } else {
                g->section = CC_SEC_BSS;
                g->offset = m->bss_size;
                m->bss_size += g->size;
            }
        }
    }
}

static void gen_function(struct cc_module *m, struct cc_node *fn) {
    struct cc_func *f = cc_find_func(m, fn->u.func_decl.name);
    if (!f) return; /* register_decls already errored */

    struct cc_func_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.m = m;

    int i = 0;
    for (struct cc_node *p = fn->u.func_decl.params; p; p = p->next, i++) {
        struct cc_local *l = &ctx.locals[ctx.local_count++];
        strncpy(l->name, p->u.param.name, sizeof(l->name) - 1);
        l->name[sizeof(l->name) - 1] = 0;
        l->type = p->u.param.type;
        l->offset = 8 + 4 * i;
    }

    collect_locals(&ctx, fn->u.func_decl.body);
    if (m->error) return;
    int frame_size = -ctx.next_offset;

    f->text_offset = m->text.len;
    emit_push_reg(&m->text, EBP);
    emit_mov_reg_reg(&m->text, EBP, ESP);
    if (frame_size > 0) emit_sub_reg_imm32(&m->text, ESP, (uint32_t)frame_size);

    gen_stmt(&ctx, fn->u.func_decl.body);
    if (m->error) return;

    uint32_t epilogue_pos = m->text.len;
    for (int j = 0; j < ctx.return_patch_count; j++) cc_patch_rel32(&m->text, ctx.return_patches[j], epilogue_pos);
    emit_mov_reg_reg(&m->text, ESP, EBP);
    emit_pop_reg(&m->text, EBP);
    emit_ret(&m->text);
}

int cc_codegen(struct cc_module *m, struct cc_node *program) {
    register_decls(m, program);
    if (m->error) return 0;

    for (struct cc_node *d = program->u.program.decls; d && !m->error; d = d->next) {
        if (d->type == CC_FUNC_DECL) gen_function(m, d);
    }
    return !m->error;
}
