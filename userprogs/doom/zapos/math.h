#ifndef ZAPOS_SHIM_MATH_H
#define ZAPOS_SHIM_MATH_H
/* Deliberately empty: this kernel is built -mgeneral-regs-only (no
 * FPU/SSE state exists at all, ever), so no real floating-point
 * function could be called from compiled code regardless of what's
 * declared here. Every math.h call site in the DOOM source this port
 * actually compiles is inside dead `#if 0` blocks (see r_main.c/
 * tables.c) -- if a build ever fails here with an undeclared-function
 * error, that means a genuinely live float call site was missed and
 * needs patching out, not a declaration added here.
 */
#endif
