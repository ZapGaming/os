/* Runtime support for compiled programs -- see the syscall ABI doc
 * comment on include/kernel/syscall.h. A compiled program has no libc
 * to link against (int 0x80 is *everything* it can touch -- see the
 * task brief), so these are hand-assembled function bodies (using the
 * same emit_* encoder cc/codegen.c's own codegen uses) spliced into
 * m->text before any user code, then registered into m->funcs exactly
 * like an ordinary user-defined function. That means a user's
 * `print("hi")` or `ipc_send(ch, buf, 10)` call compiles through the
 * *exact same* call-site codegen as calling any function they wrote
 * themselves (see gen_call() in cc/codegen.c) -- nothing about calling
 * a builtin is special-cased at the call site.
 *
 * Provided: print(s), print_int(n), yield(), sleep(ms), get_ticks(),
 * poll_key(), exit(code), ipc_open(name), ipc_send(id,buf,len),
 * ipc_recv(id,buf,cap), ipc_close(id), win_open(title,w,h),
 * win_blit(handle,pixels) -- one thin wrapper per syscall in
 * include/kernel/syscall.h, plus the two print variants since there's
 * no other way for a compiled program to produce output at all (no
 * libc means no sprintf/itoa either). SYS_BLIT is deliberately not
 * wrapped -- it wants a raw pointer to a large fixed-size pixel
 * buffer, which is far more useful to a program that already has
 * `int screen[76800];`-style global arrays than a canned builtin.
 * win_blit's `pixels` is exactly that same idiom, just for a much
 * smaller, app-owned window instead of the whole screen -- an
 * `int buf[W*H];` global array is a uint32_t 0xRRGGBB pixel buffer on
 * this architecture, same as SYS_BLIT/DOOM already documents. */
#include <cc/codegen.h>
#include <cc/emit.h>
#include <kernel/syscall.h>
#include <string.h>

static void emit_prologue(struct cc_buf *b, int frame_size) {
    emit_push_reg(b, EBP);
    emit_mov_reg_reg(b, EBP, ESP);
    if (frame_size > 0) emit_sub_reg_imm32(b, ESP, (uint32_t)frame_size);
}

static void emit_epilogue(struct cc_buf *b) {
    emit_mov_reg_reg(b, ESP, EBP);
    emit_pop_reg(b, EBP);
    emit_ret(b);
}

/* void print(int *s) -- SYS_WRITE(s). */
static void emit_print(struct cc_buf *b) {
    emit_prologue(b, 0);
    emit_mov_reg_mem(b, EBX, EBP, 8);
    emit_mov_reg_imm32(b, EAX, SYS_WRITE);
    emit_int80(b);
    emit_epilogue(b);
}

/* void print_int(int n) -- hand-written signed itoa + SYS_WRITE. Digits
 * are produced least-significant-first into a 16-byte stack buffer,
 * written back-to-front (EBX walks backward from the buffer's last
 * byte), which avoids needing a separate reverse pass. ESI (otherwise
 * unused here) holds the sign flag across the division loop since
 * EAX/ECX/EDX/EBX are all busy with the division and write cursor. */
static void emit_print_int(struct cc_buf *b) {
    emit_prologue(b, 16); /* buffer at [ebp-16 .. ebp-1] */
    emit_lea_mem(b, EBX, EBP, -1);
    emit_mov_mem8_imm8(b, EBX, 0, 0); /* NUL terminator */
    emit_mov_reg_mem(b, EAX, EBP, 8); /* eax = n */
    emit_mov_reg_imm32(b, ESI, 0);    /* esi = is_negative */

    emit_test_reg_reg(b, EAX, EAX);
    uint32_t j_neg = emit_jcc(b, CC_LT); /* SF after TEST with OF cleared == genuine "negative" test */
    uint32_t j_absdone = emit_jmp(b);
    uint32_t neg_pos = b->len;
    emit_neg_reg(b, EAX);
    emit_mov_reg_imm32(b, ESI, 1);
    uint32_t absdone_pos = b->len;
    cc_patch_rel32(b, j_neg, neg_pos);
    cc_patch_rel32(b, j_absdone, absdone_pos);

    emit_mov_reg_imm32(b, ECX, 10);
    uint32_t loop_pos = b->len;
    emit_mov_reg_imm32(b, EDX, 0); /* clear high dividend half -- eax is nonnegative here */
    emit_idiv_reg(b, ECX);
    emit_add_reg_imm32(b, EDX, '0');
    emit_sub_reg_imm32(b, EBX, 1);
    emit_mov_mem8_reg8(b, EBX, 0, EDX);
    emit_test_reg_reg(b, EAX, EAX);
    uint32_t j_loop = emit_jcc(b, CC_NE);
    cc_patch_rel32(b, j_loop, loop_pos);

    emit_test_reg_reg(b, ESI, ESI);
    uint32_t j_nosign = emit_jcc(b, CC_EQ);
    emit_sub_reg_imm32(b, EBX, 1);
    emit_mov_mem8_imm8(b, EBX, 0, '-');
    uint32_t nosign_pos = b->len;
    cc_patch_rel32(b, j_nosign, nosign_pos);

    emit_mov_reg_imm32(b, EAX, SYS_WRITE);
    emit_int80(b);
    emit_epilogue(b);
}

/* int yield(void) -- SYS_YIELD. */
static void emit_yield(struct cc_buf *b) {
    emit_prologue(b, 0);
    emit_mov_reg_imm32(b, EAX, SYS_YIELD);
    emit_int80(b);
    emit_epilogue(b);
}

/* int sleep(int ms) -- SYS_SLEEP(ms). */
static void emit_sleep(struct cc_buf *b) {
    emit_prologue(b, 0);
    emit_mov_reg_mem(b, EBX, EBP, 8);
    emit_mov_reg_imm32(b, EAX, SYS_SLEEP);
    emit_int80(b);
    emit_epilogue(b);
}

/* int get_ticks(void) -- SYS_GET_TICKS. */
static void emit_get_ticks(struct cc_buf *b) {
    emit_prologue(b, 0);
    emit_mov_reg_imm32(b, EAX, SYS_GET_TICKS);
    emit_int80(b);
    emit_epilogue(b);
}

/* int poll_key(void) -- SYS_POLL_KEY. */
static void emit_poll_key(struct cc_buf *b) {
    emit_prologue(b, 0);
    emit_mov_reg_imm32(b, EAX, SYS_POLL_KEY);
    emit_int80(b);
    emit_epilogue(b);
}

/* void exit(int code) -- SYS_EXIT. `code` is accepted for C-familiarity
 * but ignored: task_exited() (see kernel/syscall.c) takes no exit-code
 * argument at all. Never returns; the epilogue after it is unreachable
 * but kept for structural symmetry with every other builtin here. */
static void emit_exit(struct cc_buf *b) {
    emit_prologue(b, 0);
    emit_mov_reg_imm32(b, EAX, SYS_EXIT);
    emit_int80(b);
    emit_epilogue(b);
}

/* int ipc_open(char *name) -- SYS_IPC_OPEN(name). */
static void emit_ipc_open(struct cc_buf *b) {
    emit_prologue(b, 0);
    emit_mov_reg_mem(b, EBX, EBP, 8);
    emit_mov_reg_imm32(b, EAX, SYS_IPC_OPEN);
    emit_int80(b);
    emit_epilogue(b);
}

/* int ipc_send(int id, char *buf, int len) -- SYS_IPC_SEND(id, buf, len).
 * Three cdecl stack args, read the same way sleep()'s single `[ebp+8]`
 * already does, just one slot further along for each: `id` at
 * [ebp+8], `buf` at [ebp+12], `len` at [ebp+16] -- the call site
 * (cc/codegen.c's gen_call_args_reverse()) pushes them right-to-left,
 * so they land at ascending offsets above the saved EBP/return address
 * in declaration order, same as for any ordinary (non-builtin) 3-param
 * function this compiler emits. */
static void emit_ipc_send(struct cc_buf *b) {
    emit_prologue(b, 0);
    emit_mov_reg_mem(b, EBX, EBP, 8);
    emit_mov_reg_mem(b, ECX, EBP, 12);
    emit_mov_reg_mem(b, EDX, EBP, 16);
    emit_mov_reg_imm32(b, EAX, SYS_IPC_SEND);
    emit_int80(b);
    emit_epilogue(b);
}

/* int ipc_recv(int id, char *buf, int cap) -- SYS_IPC_RECV(id, buf, cap).
 * Same 3-param stack shape as ipc_send() above. */
static void emit_ipc_recv(struct cc_buf *b) {
    emit_prologue(b, 0);
    emit_mov_reg_mem(b, EBX, EBP, 8);
    emit_mov_reg_mem(b, ECX, EBP, 12);
    emit_mov_reg_mem(b, EDX, EBP, 16);
    emit_mov_reg_imm32(b, EAX, SYS_IPC_RECV);
    emit_int80(b);
    emit_epilogue(b);
}

/* int ipc_close(int id) -- SYS_IPC_CLOSE(id). */
static void emit_ipc_close(struct cc_buf *b) {
    emit_prologue(b, 0);
    emit_mov_reg_mem(b, EBX, EBP, 8);
    emit_mov_reg_imm32(b, EAX, SYS_IPC_CLOSE);
    emit_int80(b);
    emit_epilogue(b);
}

/* int win_open(char *title, int w, int h) -- SYS_WIN_OPEN(title, w, h).
 * Three cdecl stack args, same "one slot further along per parameter"
 * layout ipc_send()'s comment above already explains, just with the
 * pointer first instead of second: `title` at [ebp+8], `w` at
 * [ebp+12], `h` at [ebp+16]. Returns a window handle (>=0), or -1 if
 * w/h are invalid or no window slot is free -- see gui/compositor.c's
 * gui_app_window_open(). */
static void emit_win_open(struct cc_buf *b) {
    emit_prologue(b, 0);
    emit_mov_reg_mem(b, EBX, EBP, 8);
    emit_mov_reg_mem(b, ECX, EBP, 12);
    emit_mov_reg_mem(b, EDX, EBP, 16);
    emit_mov_reg_imm32(b, EAX, SYS_WIN_OPEN);
    emit_int80(b);
    emit_epilogue(b);
}

/* int win_blit(int handle, int *pixels) -- SYS_WIN_BLIT(handle, pixels).
 * Two cdecl stack args: `handle` (from win_open) at [ebp+8], `pixels`
 * at [ebp+12]. `pixels` must point at exactly width*height ints (see
 * this file's header comment on the "int array as raw pixel buffer"
 * idiom) matching the window's own w/h from win_open(). */
static void emit_win_blit(struct cc_buf *b) {
    emit_prologue(b, 0);
    emit_mov_reg_mem(b, EBX, EBP, 8);
    emit_mov_reg_mem(b, ECX, EBP, 12);
    emit_mov_reg_imm32(b, EAX, SYS_WIN_BLIT);
    emit_int80(b);
    emit_epilogue(b);
}

/* `param_ptr_depths` is a `param_count`-length array (NULL when
 * param_count == 0), one ptr_depth slot per parameter in declaration
 * order -- e.g. ipc_send(int id, char *buf, int len) passes {0, 1, 0}.
 *
 * This used to be a single `int param_ptr_depth` that only ever set
 * f->param_types[0], silently leaving every later parameter's
 * ptr_depth at its zero-initialized default (i.e. "int") regardless of
 * its real type -- harmless *only* by accident, and only so long as
 * every builtin had at most one parameter. Confirmed by reading both
 * consumers of struct cc_func.param_types before relying on that: the
 * call-site type checker (gen_call() in cc/codegen.c) only ever compares
 * argument COUNT against f->param_count, never any argument's type
 * against f->param_types[i] -- so a wrong-or-missing ptr_depth there
 * can't cause a real ipc_send(ch, buf, 10) call to mis-typecheck or
 * mis-codegen today (confirmed by actually compiling one -- see the
 * host-side test harness). ipc_send/ipc_recv need a SECOND parameter to
 * be a pointer (`buf`, index 1) though, which the old single-index
 * version could never express at all, so it's fixed here to take one
 * ptr_depth per parameter -- correct, not just "happens not to break"
 * -- in case anything (future type-checking, tooling, error messages)
 * ever starts reading param_types[i] for i > 0. */
static struct cc_func *cc_register_builtin(struct cc_module *m, const char *name, int param_count,
                                            const int *param_ptr_depths, int is_void, int ret_ptr_depth) {
    struct cc_func *f = cc_add_func(m, name);
    if (!f) return NULL;
    f->is_builtin = 1;
    f->has_body = 1;
    f->is_void = is_void;
    f->ret_type.ptr_depth = ret_ptr_depth;
    f->ret_type.is_array = 0;
    f->param_count = param_count;
    for (int i = 0; i < param_count; i++) {
        f->param_types[i].ptr_depth = param_ptr_depths ? param_ptr_depths[i] : 0;
        f->param_types[i].is_array = 0;
    }
    return f;
}

void cc_register_builtins(struct cc_module *m) {
    struct cc_func *f;
    static const int ptrs_1_ptr[]      = { 1 };    /* (T*) */
    static const int ptrs_1_int[]      = { 0 };    /* (int) */
    static const int ptrs_int_ptr_int[] = { 0, 1, 0 }; /* (int, T*, int) */
    static const int ptrs_ptr_int_int[] = { 1, 0, 0 }; /* (T*, int, int) */
    static const int ptrs_int_ptr[]     = { 0, 1 };    /* (int, T*) */

    f = cc_register_builtin(m, "print", 1, ptrs_1_ptr, 1, 0);
    if (f) { f->text_offset = m->text.len; emit_print(&m->text); }

    f = cc_register_builtin(m, "print_int", 1, ptrs_1_int, 1, 0);
    if (f) { f->text_offset = m->text.len; emit_print_int(&m->text); }

    f = cc_register_builtin(m, "yield", 0, NULL, 0, 0);
    if (f) { f->text_offset = m->text.len; emit_yield(&m->text); }

    f = cc_register_builtin(m, "sleep", 1, ptrs_1_int, 0, 0);
    if (f) { f->text_offset = m->text.len; emit_sleep(&m->text); }

    f = cc_register_builtin(m, "get_ticks", 0, NULL, 0, 0);
    if (f) { f->text_offset = m->text.len; emit_get_ticks(&m->text); }

    f = cc_register_builtin(m, "poll_key", 0, NULL, 0, 0);
    if (f) { f->text_offset = m->text.len; emit_poll_key(&m->text); }

    f = cc_register_builtin(m, "exit", 1, ptrs_1_int, 1, 0);
    if (f) { f->text_offset = m->text.len; emit_exit(&m->text); }

    f = cc_register_builtin(m, "ipc_open", 1, ptrs_1_ptr, 0, 0);
    if (f) { f->text_offset = m->text.len; emit_ipc_open(&m->text); }

    f = cc_register_builtin(m, "ipc_send", 3, ptrs_int_ptr_int, 0, 0);
    if (f) { f->text_offset = m->text.len; emit_ipc_send(&m->text); }

    f = cc_register_builtin(m, "ipc_recv", 3, ptrs_int_ptr_int, 0, 0);
    if (f) { f->text_offset = m->text.len; emit_ipc_recv(&m->text); }

    f = cc_register_builtin(m, "ipc_close", 1, ptrs_1_int, 0, 0);
    if (f) { f->text_offset = m->text.len; emit_ipc_close(&m->text); }

    f = cc_register_builtin(m, "win_open", 3, ptrs_ptr_int_int, 0, 0);
    if (f) { f->text_offset = m->text.len; emit_win_open(&m->text); }

    f = cc_register_builtin(m, "win_blit", 2, ptrs_int_ptr, 0, 0);
    if (f) { f->text_offset = m->text.len; emit_win_blit(&m->text); }
}
