#ifndef CC_PARSER_H
#define CC_PARSER_H

#include <cc/ast.h>
#include <cc/lexer.h>
#include <cc/module.h>

/* Parses a whole translation unit (top-level int/void declarations --
 * functions with bodies, and single-declarator global variables) into
 * a CC_PROGRAM node. String literals are interned into m->rodata as
 * they're lexed (see cc_add_rodata_string()), so by the time this
 * returns, m->rodata already holds every string constant the program
 * uses. On any syntax error, sets m->error/m->errmsg and returns
 * whatever partial tree it had -- callers must check m->error, not the
 * return value, before doing anything with it. */
struct cc_node *cc_parse_program(struct cc_lexer *lx, struct cc_module *m);

#endif
