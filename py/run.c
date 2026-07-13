#include <py/py.h>
#include <py/lexer.h>
#include <py/error.h>
#include <py/builtins.h>
#include <string.h>

static void emit_error(void (*out)(const char *)) {
    if (!out) return;
    char buf[160];
    const char *msg = py_error_message();
    uint32_t len = (uint32_t)strlen(msg);
    if (len == 0) { out("SyntaxError: could not parse script\n"); return; }
    if (len > 150) len = 150;
    memcpy(buf, msg, len);
    buf[len] = '\n';
    buf[len + 1] = 0;
    out(buf);
}

void py_run(const char *source, void (*out)(const char *)) {
    py_arena_reset();
    py_error_reset();
    py_builtins_set_out(out);

    struct py_lexer lx;
    py_lexer_init(&lx, source, (uint32_t)strlen(source)); /* may itself set an error (e.g. bad indentation) */

    struct py_node *program = py_has_error() ? NULL : py_parse_program(&lx);

    if (!program || py_has_error()) {
        emit_error(out);
        return;
    }

    struct py_env *global = py_env_new(NULL);
    py_register_builtins(global);

    py_run_program(program, global);

    if (py_has_error()) emit_error(out);
}
