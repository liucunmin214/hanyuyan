#ifndef CH_SYMBOL_H
#define CH_SYMBOL_H

#include "sema/type.h"
#include "util/common.h"
#include "util/arena.h"

/* 符号：变量/常量/函数 */
typedef enum {
    SYM_VAR,
    SYM_CONST,
    SYM_FUNC
} SymKind;

typedef struct {
    SymKind     kind;
    const char *name;
    ChType     *type;
    bool         is_param;     /* 函数参数 */
    int          param_index;  /* 参数序号 */
    const void  *ast;          /* 关联的 AST 节点（用于 IR） */
} Symbol;

/* 作用域：单链表，支持嵌套 */
typedef struct Scope {
    struct Scope *parent;
    ChVec         symbols;   /* Symbol */
} Scope;

Scope *ch_scope_new(ChArena *a, Scope *parent);
Symbol *ch_scope_lookup(Scope *s, const char *name);
Symbol *ch_scope_define(ChArena *a, Scope *s, SymKind kind, const char *name, ChType *type);

/* 类型驻留（intern）：相同结构返回同一指针 */
ChType *ch_type_intern(ChArena *a, ChType *t);
ChType *ch_type_get(ChArena *a, TypeKind k);
ChType *ch_type_list(ChArena *a, ChType *elem);
ChType *ch_type_dict(ChArena *a);
ChType *ch_type_func(ChArena *a, ChType **params, int nparams, bool variadic, ChType *ret);

#endif /* CH_SYMBOL_H */
