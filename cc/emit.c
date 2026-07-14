/* Low-level x86-32 instruction encoder used by cc/codegen.c and
 * cc/builtins.c. See include/cc/emit.h for the exact narrow subset of
 * forms supported -- just enough for a correctness-first stack-machine
 * codegen, nothing more (no SIB bytes, no short jumps, no REX -- REX
 * doesn't exist in 32-bit mode anyway).
 *
 * This file (like the rest of cc/) is compiled straight into the
 * kernel image, so it uses kmalloc/kfree, not the host libc. */
#include <cc/emit.h>
#include <kernel/kheap.h>
#include <string.h>

void cc_buf_init(struct cc_buf *b) {
    b->cap = 256;
    b->len = 0;
    b->data = (uint8_t *)kmalloc(b->cap);
}

void cc_buf_free(struct cc_buf *b) {
    if (b->data) kfree(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static void cc_buf_ensure(struct cc_buf *b, uint32_t extra) {
    if (b->len + extra <= b->cap) return;
    uint32_t newcap = b->cap ? b->cap : 256;
    while (newcap < b->len + extra) newcap *= 2;
    uint8_t *nd = (uint8_t *)kmalloc(newcap);
    if (b->data) {
        memcpy(nd, b->data, b->len);
        kfree(b->data);
    }
    b->data = nd;
    b->cap = newcap;
}

void cc_buf_push_byte(struct cc_buf *b, uint8_t byte) {
    cc_buf_ensure(b, 1);
    b->data[b->len++] = byte;
}

void cc_buf_push_u32(struct cc_buf *b, uint32_t v) {
    cc_buf_ensure(b, 4);
    b->data[b->len++] = (uint8_t)(v & 0xFF);
    b->data[b->len++] = (uint8_t)((v >> 8) & 0xFF);
    b->data[b->len++] = (uint8_t)((v >> 16) & 0xFF);
    b->data[b->len++] = (uint8_t)((v >> 24) & 0xFF);
}

void cc_buf_push_bytes(struct cc_buf *b, const void *data, uint32_t len) {
    cc_buf_ensure(b, len);
    memcpy(b->data + b->len, data, len);
    b->len += len;
}

void cc_buf_patch_u32(struct cc_buf *b, uint32_t at, uint32_t v) {
    b->data[at + 0] = (uint8_t)(v & 0xFF);
    b->data[at + 1] = (uint8_t)((v >> 8) & 0xFF);
    b->data[at + 2] = (uint8_t)((v >> 16) & 0xFF);
    b->data[at + 3] = (uint8_t)((v >> 24) & 0xFF);
}

/* mod=10 (disp32) or mod=01 (disp8) memory operand for [base+disp],
 * base != ESP/EBP-special-cased. `base` may be EBP -- forcing at least
 * disp8 mode (never mod=00) is exactly what avoids the "mod=00,rm=101
 * means absolute, not [EBP]" trap. */
static void emit_modrm_mem(struct cc_buf *b, enum cc_reg reg_field, enum cc_reg base, int32_t disp) {
    if (disp == 0 && base != EBP) {
        cc_buf_push_byte(b, (uint8_t)(0x00 | (reg_field << 3) | base));
    } else if (disp >= -128 && disp <= 127) {
        cc_buf_push_byte(b, (uint8_t)(0x40 | (reg_field << 3) | base));
        cc_buf_push_byte(b, (uint8_t)(disp & 0xFF));
    } else {
        cc_buf_push_byte(b, (uint8_t)(0x80 | (reg_field << 3) | base));
        cc_buf_push_u32(b, (uint32_t)disp);
    }
}

void emit_push_reg(struct cc_buf *b, enum cc_reg reg) { cc_buf_push_byte(b, (uint8_t)(0x50 + reg)); }
void emit_pop_reg(struct cc_buf *b, enum cc_reg reg) { cc_buf_push_byte(b, (uint8_t)(0x58 + reg)); }

void emit_push_imm32(struct cc_buf *b, uint32_t imm) {
    cc_buf_push_byte(b, 0x68);
    cc_buf_push_u32(b, imm);
}

uint32_t emit_mov_reg_imm32(struct cc_buf *b, enum cc_reg reg, uint32_t imm) {
    cc_buf_push_byte(b, (uint8_t)(0xB8 + reg));
    uint32_t at = b->len;
    cc_buf_push_u32(b, imm);
    return at;
}

void emit_mov_reg_reg(struct cc_buf *b, enum cc_reg dst, enum cc_reg src) {
    cc_buf_push_byte(b, 0x89);
    cc_buf_push_byte(b, (uint8_t)(0xC0 | (src << 3) | dst));
}

void emit_mov_reg_mem(struct cc_buf *b, enum cc_reg reg, enum cc_reg base, int32_t disp) {
    cc_buf_push_byte(b, 0x8B);
    emit_modrm_mem(b, reg, base, disp);
}

void emit_mov_mem_reg(struct cc_buf *b, enum cc_reg base, int32_t disp, enum cc_reg reg) {
    cc_buf_push_byte(b, 0x89);
    emit_modrm_mem(b, reg, base, disp);
}

uint32_t emit_mov_reg_absmem(struct cc_buf *b, enum cc_reg reg, uint32_t placeholder) {
    cc_buf_push_byte(b, 0x8B);
    cc_buf_push_byte(b, (uint8_t)(0x00 | (reg << 3) | 0x05)); /* mod=00 rm=101: disp32, no base */
    uint32_t at = b->len;
    cc_buf_push_u32(b, placeholder);
    return at;
}

uint32_t emit_mov_absmem_reg(struct cc_buf *b, uint32_t placeholder, enum cc_reg reg) {
    cc_buf_push_byte(b, 0x89);
    cc_buf_push_byte(b, (uint8_t)(0x00 | (reg << 3) | 0x05));
    uint32_t at = b->len;
    cc_buf_push_u32(b, placeholder);
    return at;
}

void emit_lea_mem(struct cc_buf *b, enum cc_reg reg, enum cc_reg base, int32_t disp) {
    cc_buf_push_byte(b, 0x8D);
    emit_modrm_mem(b, reg, base, disp);
}

void emit_mov_mem8_reg8(struct cc_buf *b, enum cc_reg base, int32_t disp, enum cc_reg reg) {
    cc_buf_push_byte(b, 0x88);
    emit_modrm_mem(b, reg, base, disp);
}

void emit_mov_mem8_imm8(struct cc_buf *b, enum cc_reg base, int32_t disp, uint8_t imm) {
    cc_buf_push_byte(b, 0xC6);
    emit_modrm_mem(b, (enum cc_reg)0, base, disp); /* reg field = 0 (opcode extension /0) */
    cc_buf_push_byte(b, imm);
}

static void emit_rr(struct cc_buf *b, uint8_t opcode, enum cc_reg dst, enum cc_reg src) {
    cc_buf_push_byte(b, opcode);
    cc_buf_push_byte(b, (uint8_t)(0xC0 | (src << 3) | dst));
}

void emit_add_reg_reg(struct cc_buf *b, enum cc_reg dst, enum cc_reg src) { emit_rr(b, 0x01, dst, src); }
void emit_sub_reg_reg(struct cc_buf *b, enum cc_reg dst, enum cc_reg src) { emit_rr(b, 0x29, dst, src); }
void emit_and_reg_reg(struct cc_buf *b, enum cc_reg dst, enum cc_reg src) { emit_rr(b, 0x21, dst, src); }
void emit_or_reg_reg(struct cc_buf *b, enum cc_reg dst, enum cc_reg src) { emit_rr(b, 0x09, dst, src); }
void emit_xor_reg_reg(struct cc_buf *b, enum cc_reg dst, enum cc_reg src) { emit_rr(b, 0x31, dst, src); }
void emit_cmp_reg_reg(struct cc_buf *b, enum cc_reg lhs, enum cc_reg rhs) { emit_rr(b, 0x39, lhs, rhs); }
void emit_test_reg_reg(struct cc_buf *b, enum cc_reg a, enum cc_reg b2) { emit_rr(b, 0x85, a, b2); }

void emit_imul_reg_reg(struct cc_buf *b, enum cc_reg dst, enum cc_reg src) {
    cc_buf_push_byte(b, 0x0F);
    cc_buf_push_byte(b, 0xAF);
    cc_buf_push_byte(b, (uint8_t)(0xC0 | (dst << 3) | src));
}

void emit_add_reg_imm32(struct cc_buf *b, enum cc_reg reg, uint32_t imm) {
    cc_buf_push_byte(b, 0x81);
    cc_buf_push_byte(b, (uint8_t)(0xC0 | reg)); /* /0 */
    cc_buf_push_u32(b, imm);
}

void emit_sub_reg_imm32(struct cc_buf *b, enum cc_reg reg, uint32_t imm) {
    cc_buf_push_byte(b, 0x81);
    cc_buf_push_byte(b, (uint8_t)(0xE8 | reg)); /* /5 */
    cc_buf_push_u32(b, imm);
}

void emit_neg_reg(struct cc_buf *b, enum cc_reg reg) {
    cc_buf_push_byte(b, 0xF7);
    cc_buf_push_byte(b, (uint8_t)(0xD8 | reg)); /* /3 */
}

void emit_not_reg(struct cc_buf *b, enum cc_reg reg) {
    cc_buf_push_byte(b, 0xF7);
    cc_buf_push_byte(b, (uint8_t)(0xD0 | reg)); /* /2 */
}

void emit_cdq(struct cc_buf *b) { cc_buf_push_byte(b, 0x99); }

void emit_idiv_reg(struct cc_buf *b, enum cc_reg reg) {
    cc_buf_push_byte(b, 0xF7);
    cc_buf_push_byte(b, (uint8_t)(0xF8 | reg)); /* /7 */
}

void emit_shl_reg_imm8(struct cc_buf *b, enum cc_reg reg, uint8_t imm) {
    cc_buf_push_byte(b, 0xC1);
    cc_buf_push_byte(b, (uint8_t)(0xE0 | reg)); /* /4 */
    cc_buf_push_byte(b, imm);
}

void emit_sar_reg_imm8(struct cc_buf *b, enum cc_reg reg, uint8_t imm) {
    cc_buf_push_byte(b, 0xC1);
    cc_buf_push_byte(b, (uint8_t)(0xF8 | reg)); /* /7 (SAR) */
    cc_buf_push_byte(b, imm);
}

void emit_shl_reg_cl(struct cc_buf *b, enum cc_reg reg) {
    cc_buf_push_byte(b, 0xD3);
    cc_buf_push_byte(b, (uint8_t)(0xE0 | reg)); /* /4 */
}

void emit_sar_reg_cl(struct cc_buf *b, enum cc_reg reg) {
    cc_buf_push_byte(b, 0xD3);
    cc_buf_push_byte(b, (uint8_t)(0xF8 | reg)); /* /7 */
}

static const uint8_t cond_code[] = {
    [CC_EQ] = 0x4, [CC_NE] = 0x5, [CC_LT] = 0xC, [CC_GE] = 0xD, [CC_LE] = 0xE, [CC_GT] = 0xF,
};

void emit_setcc_al(struct cc_buf *b, enum cc_cond cond) {
    cc_buf_push_byte(b, 0x0F);
    cc_buf_push_byte(b, (uint8_t)(0x90 | cond_code[cond]));
    cc_buf_push_byte(b, 0xC0); /* modrm: mod=11 reg=000(unused) rm=000(AL) */
}

void emit_movzx_eax_al(struct cc_buf *b) {
    cc_buf_push_byte(b, 0x0F);
    cc_buf_push_byte(b, 0xB6);
    cc_buf_push_byte(b, (uint8_t)(0xC0 | (EAX << 3) | EAX)); /* movzx eax, al */
}

uint32_t emit_jmp(struct cc_buf *b) {
    cc_buf_push_byte(b, 0xE9);
    uint32_t at = b->len;
    cc_buf_push_u32(b, 0);
    return at;
}

uint32_t emit_jcc(struct cc_buf *b, enum cc_cond cond) {
    cc_buf_push_byte(b, 0x0F);
    cc_buf_push_byte(b, (uint8_t)(0x80 | cond_code[cond]));
    uint32_t at = b->len;
    cc_buf_push_u32(b, 0);
    return at;
}

void emit_call_reg(struct cc_buf *b, enum cc_reg reg) {
    cc_buf_push_byte(b, 0xFF);
    cc_buf_push_byte(b, (uint8_t)(0xD0 | reg)); /* /2 */
}

void emit_ret(struct cc_buf *b) { cc_buf_push_byte(b, 0xC3); }
void emit_int80(struct cc_buf *b) {
    cc_buf_push_byte(b, 0xCD);
    cc_buf_push_byte(b, 0x80);
}

void cc_patch_rel32(struct cc_buf *b, uint32_t site, uint32_t target_pos) {
    uint32_t rel = target_pos - (site + 4);
    cc_buf_patch_u32(b, site, rel);
}
