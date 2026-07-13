#ifndef PY_ERROR_H
#define PY_ERROR_H

/* Internal error plumbing shared by the lexer, parser, and interpreter.
 * Not part of the documented public surface (py_run() in py.h is) --
 * this just lets every layer report a Python-style "SomeError: detail"
 * message without needing exceptions/longjmp (neither is available in
 * this freestanding environment). The first error wins; everything
 * reported after it is silently dropped, and every evaluation function
 * is expected to check py_has_error() after recursing and bail out
 * early (returning a harmless dummy value) so a script that errors
 * mid-loop or mid-recursion unwinds promptly instead of continuing to
 * run, and so an error while lexing can't get overwritten by cascading
 * parser complaints that follow from it. */
void py_error_reset(void);
void py_error_set(const char *prefix, const char *detail1, const char *detail2);
int py_has_error(void);
/* e.g. "NameError: 'x' is not defined" -- no trailing newline. */
const char *py_error_message(void);

#endif
