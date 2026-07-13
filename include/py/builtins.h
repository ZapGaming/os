#ifndef PY_BUILTINS_H
#define PY_BUILTINS_H

/* Internal wiring between py_run() (py/run.c) and the print() builtin
 * (py/builtins.c) -- not part of the public surface in py.h. */
void py_builtins_set_out(void (*out)(const char *));

#endif
