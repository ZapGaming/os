#ifndef CC_EMIT_H
#define CC_EMIT_H

#include <stdint.h>

/* A tiny growable byte buffer, used for every section the compiler
 * builds (.text, .rodata, .data) -- same "one big kmalloc'd, bump/grow
 * as needed" spirit as js/value.c's arena, just resizable via
 * kmalloc+memcpy+kfree instead of a fixed block, since section sizes
 * aren't known ahead of time the way one page's AST arena size isn't
 * either. */
struct cc_buf {
    uint8_t *data;
    uint32_t len;
    uint32_t cap;
};

void cc_buf_init(struct cc_buf *b);
void cc_buf_free(struct cc_buf *b);
void cc_buf_push_byte(struct cc_buf *b, uint8_t byte);
void cc_buf_push_u32(struct cc_buf *b, uint32_t v);
void cc_buf_push_bytes(struct cc_buf *b, const void *data, uint32_t len);
/* Overwrites 4 bytes already in the buffer (little-endian) -- used for
 * both forward-jump patching (same function, resolved a few lines
 * later) and the final relocation pass (cc_link.c-in-spirit code inside
 * codegen.c/elf_writer.c, resolved once every section's final base
 * address is known). */
void cc_buf_patch_u32(struct cc_buf *b, uint32_t at, uint32_t v);

/* ---- x86-32 register numbers -----------------------------------------
 * These are exactly the ModRM/opcode-embedded register encodings, so a
 * `enum cc_reg` value can be used directly wherever the encoders below
 * need a 3-bit register field. Only EAX/ECX/EDX/EBX have single-byte
 * (AL/CL/DL/BL) sub-registers reachable without a REX prefix (which
 * doesn't exist in 32-bit mode at all) -- SETcc/MOVZX-from-byte below
 * only ever target EAX via AL for exactly that reason. */
enum cc_reg { EAX = 0, ECX = 1, EDX = 2, EBX = 3, ESP = 4, EBP = 5, ESI = 6, EDI = 7 };

enum cc_cond { CC_EQ, CC_NE, CC_LT, CC_GE, CC_LE, CC_GT };

/* ---- Instruction encoders ---------------------------------------------
 * Deliberately narrow: only the handful of forms the codegen actually
 * needs (see cc/codegen.c). Every one appends to `b`. None of them ever
 * use ESP as a memory-operand base (would need a SIB byte we don't
 * bother encoding) -- the codegen never needs that. */
void emit_push_reg(struct cc_buf *b, enum cc_reg reg);
void emit_pop_reg(struct cc_buf *b, enum cc_reg reg);
void emit_push_imm32(struct cc_buf *b, uint32_t imm);
/* Returns the buffer offset of the imm32 field, so a caller that needs
 * this to resolve to a section-relative address later (a global
 * variable, a string literal, a function's entry point -- anything
 * whose final absolute address isn't known until link time, see
 * cc_add_reloc()) can register that offset for patching. Callers that
 * just want a plain constant loaded (CC_NUM_LIT) simply ignore it. */
uint32_t emit_mov_reg_imm32(struct cc_buf *b, enum cc_reg reg, uint32_t imm);
void emit_mov_reg_reg(struct cc_buf *b, enum cc_reg dst, enum cc_reg src);
/* mov reg, [base+disp] / mov [base+disp], reg -- base must not be ESP. */
void emit_mov_reg_mem(struct cc_buf *b, enum cc_reg reg, enum cc_reg base, int32_t disp);
void emit_mov_mem_reg(struct cc_buf *b, enum cc_reg base, int32_t disp, enum cc_reg reg);
/* mov reg, [ABSOLUTE]  /  mov [ABSOLUTE], reg -- absolute 32-bit address
 * operand (ModRM mod=00, rm=101 with no base register at all -- the one
 * case where rm=101 does NOT mean "use EBP"). Returns the buffer offset
 * of the imm32 field so the caller can register a relocation there. */
uint32_t emit_mov_reg_absmem(struct cc_buf *b, enum cc_reg reg, uint32_t placeholder);
uint32_t emit_mov_absmem_reg(struct cc_buf *b, uint32_t placeholder, enum cc_reg reg);
void emit_lea_mem(struct cc_buf *b, enum cc_reg reg, enum cc_reg base, int32_t disp);
/* Single-byte memory forms -- only used by cc/builtins.c's hand-written
 * print_int() itoa loop (writing individual ASCII digit/sign/NUL
 * bytes). `reg` must be EAX/ECX/EDX/EBX (the only four with an 8-bit
 * sub-register reachable without a REX prefix, which 32-bit mode has
 * no such thing as anyway). */
void emit_mov_mem8_reg8(struct cc_buf *b, enum cc_reg base, int32_t disp, enum cc_reg reg);
void emit_mov_mem8_imm8(struct cc_buf *b, enum cc_reg base, int32_t disp, uint8_t imm);

void emit_add_reg_reg(struct cc_buf *b, enum cc_reg dst, enum cc_reg src);
void emit_sub_reg_reg(struct cc_buf *b, enum cc_reg dst, enum cc_reg src);
void emit_and_reg_reg(struct cc_buf *b, enum cc_reg dst, enum cc_reg src);
void emit_or_reg_reg(struct cc_buf *b, enum cc_reg dst, enum cc_reg src);
void emit_xor_reg_reg(struct cc_buf *b, enum cc_reg dst, enum cc_reg src);
void emit_imul_reg_reg(struct cc_buf *b, enum cc_reg dst, enum cc_reg src);
void emit_cmp_reg_reg(struct cc_buf *b, enum cc_reg lhs, enum cc_reg rhs);
void emit_test_reg_reg(struct cc_buf *b, enum cc_reg a, enum cc_reg b2);
void emit_add_reg_imm32(struct cc_buf *b, enum cc_reg reg, uint32_t imm);
void emit_sub_reg_imm32(struct cc_buf *b, enum cc_reg reg, uint32_t imm);

void emit_neg_reg(struct cc_buf *b, enum cc_reg reg);
void emit_not_reg(struct cc_buf *b, enum cc_reg reg);
void emit_cdq(struct cc_buf *b);
void emit_idiv_reg(struct cc_buf *b, enum cc_reg reg);
void emit_shl_reg_imm8(struct cc_buf *b, enum cc_reg reg, uint8_t imm);
void emit_sar_reg_imm8(struct cc_buf *b, enum cc_reg reg, uint8_t imm);
void emit_shl_reg_cl(struct cc_buf *b, enum cc_reg reg);
void emit_sar_reg_cl(struct cc_buf *b, enum cc_reg reg);

void emit_setcc_al(struct cc_buf *b, enum cc_cond cond);
void emit_movzx_eax_al(struct cc_buf *b);

/* Near jumps/calls, always the 32-bit-displacement form (simplest to
 * backpatch -- no short/near size decision to make). Returns the
 * buffer offset of the rel32 field for the caller to patch once the
 * target address is known. */
uint32_t emit_jmp(struct cc_buf *b);
uint32_t emit_jcc(struct cc_buf *b, enum cc_cond cond);
void emit_call_reg(struct cc_buf *b, enum cc_reg reg);
void emit_ret(struct cc_buf *b);
void emit_int80(struct cc_buf *b);

/* Patches a rel32 field at `site` (as returned by emit_jmp/emit_jcc) so
 * it jumps to `target_pos` (both are offsets within the same buffer). */
void cc_patch_rel32(struct cc_buf *b, uint32_t site, uint32_t target_pos);

#endif
