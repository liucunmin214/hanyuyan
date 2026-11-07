#include "sema/symbol.h"
#include "util/arena.h"
#include <string.h>

Scope *ch_scope_new(ChArena *a, Scope *parent) {
    Scope *s = ch_arena_alloc(a, sizeof(Scope));
    s->parent = parent;
    ch_vec_init(&s->symbols, sizeof(Symbol));
    return s;
}

Symbol *ch_scope_lookup(Scope *s, const char *name) {
    for (Scope *cur = s; cur; cur = cur->parent) {
        for (size_t i = 0; i < cur->symbols.len; i++) {
            Symbol *sym = &CH_VEC_AT(cur->symbols, Symbol, i);
            if (strcmp(sym->name, name) == 0) return sym;
        }
    }
    return NULL;
}

Symbol *ch_scope_define(ChArena *a, Scope *s, SymKind kind, const char *name, ChType *type) {
    Symbol *sym = ch_vec_push(&s->symbols);
    sym->kind = kind;
    sym->name = ch_arena_strdup(a, name);
    sym->type = type;
    sym->is_param = false;
    sym->param_index = 0;
    sym->ast = NULL;
    return sym;
}

/* ==================== 类型驻留（arena 本地表） ==================== */

static ChType *find_interned(ChTypeTable *tb, ChType *t) {
    for (int i = 0; i < tb->len; i++) {
        if (ch_type_equal(&tb->data[i], t)) return &tb->data[i];
    }
    return NULL;
}

static ChType *intern(ChTypeTable *tb, ChType *t) {
    ChType *found = find_interned(tb, t);
    if (found) return found;
    if (tb->len >= tb->cap) {
        tb->cap = tb->cap ? tb->cap * 2 : 32;
        tb->data = realloc(tb->data, tb->cap * sizeof(ChType));
    }
    tb->data[tb->len] = *t;
    return &tb->data[tb->len++];
}

ChType *ch_type_intern(ChArena *a, ChType *t) {
    if (!t) return NULL;
    /* 拷贝到局部变量，避免修改可能指向驻留表内部的入参 */
    ChType copy = *t;
    ChTypeTable *tb = ch_arena_type_table(a);
    /* 递归驻留子类型 */
    if (copy.elem) copy.elem = ch_type_intern(a, copy.elem);
    for (int i = 0; i < copy.nparams; i++)
        copy.params[i] = ch_type_intern(a, copy.params[i]);
    return intern(tb, &copy);
}

ChType *ch_type_get(ChArena *a, TypeKind k) {
    ChType t = {0};
    t.kind = k;
    return ch_type_intern(a, &t);
}

ChType *ch_type_list(ChArena *a, ChType *elem) {
    ChType t = {0};
    t.kind = TY_LIST;
    t.elem = elem;
    return ch_type_intern(a, &t);
}

ChType *ch_type_dict(ChArena *a) {
    ChType t = {0};
    t.kind = TY_DICT;
    return ch_type_intern(a, &t);
}

ChType *ch_type_func(ChArena *a, ChType **params, int nparams, bool variadic, ChType *ret) {
    ChType t = {0};
    t.kind = TY_FUNC;
    t.params = params;
    t.nparams = nparams;
    t.is_variadic = variadic;
    t.elem = ret; /* 复用 elem 字段存返回类型 */
    return ch_type_intern(a, &t);
}
