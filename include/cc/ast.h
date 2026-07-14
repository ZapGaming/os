#ifndef CC_AST_H
#define CC_AST_H

#include <stdint.h>

/* A "type" in this subset is just an int with a pointer-indirection
 * depth (0 = int, 1 = int*, 2 = int**, ...) plus an array flag/length
 * for declarations -- see the doc comment on include/cc/codegen.h for
 * why there's nothing richer than this (no struct/union/enum, no
 * float/double, no char -- string literals are just addresses). Every
 * scalar this subset knows about, pointer or int, is exactly 4 bytes,
 * which is what lets codegen.c get away with a single uniform "4 bytes
 * per slot" rule everywhere (locals, params, array elements, pointer
 * arithmetic scaling). */
struct cc_type {
    int ptr_depth;
    int is_array;
    int array_len; /* valid when is_array */
};

enum cc_node_type {
    CC_PROGRAM,
    CC_FUNC_DECL,
    CC_GLOBAL_VAR,
    CC_PARAM,
    CC_BLOCK,
    CC_IF,
    CC_WHILE,
    CC_FOR,
    CC_RETURN,
    CC_BREAK,
    CC_CONTINUE,
    CC_EXPR_STMT,
    CC_VAR_DECL,
    CC_NUM_LIT,
    CC_STR_LIT,
    CC_IDENT,
    CC_ASSIGN,
    CC_BINARY,
    CC_LOGICAL,
    CC_UNARY,
    CC_ADDR,
    CC_DEREF,
    CC_PREINC, CC_PREDEC, CC_POSTINC, CC_POSTDEC,
    CC_INDEX,
    CC_CALL,
};

struct cc_node {
    enum cc_node_type type;
    struct cc_node *next; /* sibling link: top-level decls, statement lists, param lists, arg lists */
    int line;
    union {
        struct { struct cc_node *decls; } program;
        struct { char *name; struct cc_type ret_type; int is_void; struct cc_node *params; struct cc_node *body; } func_decl;
        struct { char *name; struct cc_type type; int has_init; int32_t init_value; } global_var;
        struct { char *name; struct cc_type type; } param;
        struct { char *name; struct cc_type type; struct cc_node *init; } var_decl;
        struct { struct cc_node *stmts; } block;
        struct { struct cc_node *cond, *then_branch, *else_branch; } if_stmt;
        struct { struct cc_node *cond, *body; } while_stmt;
        struct { struct cc_node *init, *cond, *post, *body; } for_stmt;
        struct { struct cc_node *value; } return_stmt;
        struct { struct cc_node *expr; } expr_stmt;
        struct { int32_t value; } num_lit;
        struct { uint32_t rodata_offset; uint32_t len; } str_lit;
        struct { char *name; } ident;
        struct { struct cc_node *target, *value; } assign;
        struct { char op[3]; struct cc_node *left, *right; } binary;
        struct { char op[3]; struct cc_node *left, *right; } logical;
        struct { char op[2]; struct cc_node *operand; } unary;
        struct { struct cc_node *operand; } addr_deref;
        struct { struct cc_node *operand; } incdec;
        struct { struct cc_node *array, *index; } index_expr;
        struct { char *name; struct cc_node *args; } call;
    } u;
};

#endif
