#include "sema/sema.h"
#include "sema/symbol.h"
#include "sema/type.h"
#include "sema/builtins.h"
#include "ast/ast.h"
#include "util/arena.h"
#include "util/diag.h"
#include <string.h>
#include <stdarg.h>

typedef struct {
    ChArena     *arena;
    ChDiagList  *diags;
    Scope       *scope;
    Scope       *global;        /* 模块根作用域（全局符号无需捕获） */
    Scope       *lam_scope;     /* 当前匿名函数参数作用域，不在匿名函数内为 NULL */
    ChType      *fn_ret_type;   /* 当前函数返回类型（用于检查返回语句） */
    bool         in_loop;       /* 是否在循环内（检查跳出/继续） */
} Sema;

static void sema_err(Sema *s, int line, int col, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ch_diag_add(s->diags, true, line, col, "%s", buf);
}

/* 仅在当前作用域（不沿父链）查找 */
static bool scope_has(Scope *sc, const char *name, SymKind *kind) {
    for (size_t i = 0; i < sc->symbols.len; i++) {
        Symbol *sym = &CH_VEC_AT(sc->symbols, Symbol, i);
        if (strcmp(sym->name, name) == 0) {
            if (kind) *kind = sym->kind;
            return true;
        }
    }
    return false;
}

/* 赋值/返回/实参的类型相容判断：结构等价、整数可升为小额、函数类型互相相容 */
static bool type_compatible(ChType *want, ChType *got) {
    if (!want || !got) return true;
    if (want->kind == TY_UNKNOWN || got->kind == TY_UNKNOWN ||
        got->kind == TY_ERROR || want->kind == TY_ERROR) return true;
    if (ch_type_equal(want, got)) return true;
    if (want->kind == TY_FLOAT && got->kind == TY_INT) return true;
    if (want->kind == TY_FUNC && got->kind == TY_FUNC) return true;
    /* 列表：无元素标注或元素为「未知」时互相相容（内建函数参数多为裸列表） */
    if (want->kind == TY_LIST && got->kind == TY_LIST) {
        ChType *we = want->elem, *ge = got->elem;
        if (!we || !ge) return true;
        if (we->kind == TY_UNKNOWN || ge->kind == TY_UNKNOWN) return true;
        return ch_type_equal(we, ge);
    }
    return false;
}

/* 判断名字是否为「当前匿名函数之外、模块级之下」的局部变量（按值捕获对象） */
static bool is_outer_local(Sema *s, const char *name) {
    if (!s->lam_scope) return false;
    SymKind k;
    if (scope_has(s->lam_scope, name, &k)) return false;
    for (Scope *sc = s->lam_scope->parent; sc; sc = sc->parent) {
        if (!scope_has(sc, name, &k)) continue;
        if (sc == s->global || k == SYM_FUNC) return false;
        return true;
    }
    return false;
}

/* ===== 按值捕获分析 ===== */

typedef struct {
    Sema   *s;
    Scope  *lam_scope;   /* 当前匿名函数的参数作用域 */
    ChVec   skip;        /* const char *：嵌套内层匿名函数的参数名（对本层不可见） */
    ChVec   caps;        /* const char *：捕获名（去重，首次出现序） */
} CapWalk;

static bool name_in_vec(ChVec *v, const char *name) {
    for (size_t i = 0; i < v->len; i++)
        if (strcmp(CH_VEC_AT(*v, const char *, i), name) == 0) return true;
    return false;
}

static void cap_walk_expr(CapWalk *w, Expr *e) {
    if (!e) return;
    switch (e->kind) {
    case EX_IDENT: {
        const char *name = e->u.sval;
        SymKind k;
        if (name_in_vec(&w->skip, name)) return;
        if (scope_has(w->lam_scope, name, &k)) return;   /* 自身参数 */
        for (Scope *sc = w->lam_scope->parent; sc; sc = sc->parent) {
            if (!scope_has(sc, name, &k)) continue;
            if (sc == w->s->global) return;              /* 模块级符号无需捕获 */
            if (k == SYM_FUNC) return;                   /* 函数按符号引用 */
            if (!name_in_vec(&w->caps, name)) {
                const char **slot = ch_vec_push(&w->caps);
                *slot = name;
            }
            return;
        }
        return;
    }
    case EX_LAMBDA:
        /* 内层匿名函数：其参数对本层不可见，其余引用可能穿透到本层（传递捕获） */
        for (int i = 0; i < e->u.lambda.nparams; i++) {
            const char **slot = ch_vec_push(&w->skip);
            *slot = e->u.lambda.params[i].name;
        }
        cap_walk_expr(w, e->u.lambda.body);
        w->skip.len -= (size_t)e->u.lambda.nparams;
        return;
    case EX_LIST:
        for (int i = 0; i < e->u.list.count; i++) cap_walk_expr(w, e->u.list.items[i]);
        return;
    case EX_DICT:
        for (int i = 0; i < e->u.dict.count; i++) cap_walk_expr(w, e->u.dict.vals[i]);
        return;
    case EX_MEMBER: cap_walk_expr(w, e->u.member.obj); return;
    case EX_INDEX:
        cap_walk_expr(w, e->u.index.obj);
        cap_walk_expr(w, e->u.index.idx);
        return;
    case EX_CALL:
        cap_walk_expr(w, e->u.call.callee);
        for (int i = 0; i < e->u.call.nargs; i++) cap_walk_expr(w, e->u.call.args[i]);
        return;
    case EX_UNARY: cap_walk_expr(w, e->u.unary.operand); return;
    case EX_BINARY:
        cap_walk_expr(w, e->u.binary.lhs);
        cap_walk_expr(w, e->u.binary.rhs);
        return;
    case EX_ASSIGN:
        cap_walk_expr(w, e->u.assign.target);
        cap_walk_expr(w, e->u.assign.value);
        return;
    case EX_GROUP: cap_walk_expr(w, e->u.grouped); return;
    default: return;
    }
}

/* 解析类型标注（从 AST 中的 ChType* 转为驻留类型） */
static ChType *resolve_type(Sema *s, ChType *annot) {
    if (!annot) return ch_type_get(s->arena, TY_UNKNOWN);
    if (annot->kind == TY_LIST) {
        ChType *elem = annot->elem ? resolve_type(s, annot->elem) : ch_type_get(s->arena, TY_UNKNOWN);
        return ch_type_list(s->arena, elem);
    }
    return ch_type_get(s->arena, annot->kind);
}

static void check_expr_ex(Sema *s, Expr *e, bool as_callee);
static void check_expr(Sema *s, Expr *e) { check_expr_ex(s, e, false); }

static ChType *binary_result_type(Sema *s, TokenKind op, ChType *l, ChType *r, int line, int col) {
    (void)s; (void)line; (void)col;
    bool lnum = ch_type_is_numeric(l);
    bool rnum = ch_type_is_numeric(r);
    switch (op) {
    case TK_PLUS:
        if (l->kind == TY_STRING && r->kind == TY_STRING) return ch_type_get(s->arena, TY_STRING);
        /* fallthrough */
    case TK_MINUS: case TK_STAR: case TK_SLASH: case TK_PERCENT: case TK_CARET:
        if (lnum && rnum) {
            if (l->kind == TY_FLOAT || r->kind == TY_FLOAT)
                return ch_type_get(s->arena, TY_FLOAT);
            return ch_type_get(s->arena, TY_INT);
        }
        return ch_type_get(s->arena, TY_ERROR);
    case TK_EQ: case TK_NE: case TK_LT: case TK_LE: case TK_GT: case TK_GE:
        return ch_type_get(s->arena, TY_BOOL);
    case TK_ANDAND: case TK_OROR:
        return ch_type_get(s->arena, TY_BOOL);
    default:
        return ch_type_get(s->arena, TY_ERROR);
    }
}

static void check_expr_ex(Sema *s, Expr *e, bool as_callee) {
    if (!e) return;
    switch (e->kind) {
    case EX_INT:    e->type = ch_type_get(s->arena, TY_INT); return;
    case EX_FLOAT:  e->type = ch_type_get(s->arena, TY_FLOAT); return;
    case EX_STRING: e->type = ch_type_get(s->arena, TY_STRING); return;
    case EX_BOOL:   e->type = ch_type_get(s->arena, TY_BOOL); return;
    case EX_NIL:    e->type = ch_type_get(s->arena, TY_NIL); return;
    case EX_IDENT: {
        Symbol *sym = ch_scope_lookup(s->scope, e->u.sval);
        if (!sym) {
            const ChBuiltin *b = ch_builtin_find(e->u.sval);
            if (b) {
                if (!as_callee)
                    sema_err(s, e->line, e->col,
                             "内建函数 '%s' 不能直接作为值（可用 (参数) => %s(参数) 包装）",
                             e->u.sval, e->u.sval);
                e->type = ch_builtin_func_type(s->arena, b);
            } else {
                sema_err(s, e->line, e->col, "未定义的标识符 '%s'", e->u.sval);
                e->type = ch_type_get(s->arena, TY_ERROR);
            }
        } else {
            e->type = sym->type;
            if (sym->kind == SYM_FUNC) {
                if (sym->type && sym->type->is_variadic && !as_callee)
                    sema_err(s, e->line, e->col,
                             "可变参数函数 '%s' 不能作为值传递", e->u.sval);
                else
                    e->is_funcref = true;
            }
        }
        return;
    }
    case EX_LIST: {
        ChType *elem = NULL;
        for (int i = 0; i < e->u.list.count; i++) {
            Expr *item = e->u.list.items[i];
            check_expr(s, item);
            if (!elem) elem = item->type;
        }
        if (!elem) elem = ch_type_get(s->arena, TY_UNKNOWN);
        e->type = ch_type_list(s->arena, elem);
        return;
    }
    case EX_DICT: {
        for (int i = 0; i < e->u.dict.count; i++)
            check_expr(s, e->u.dict.vals[i]);
        e->type = ch_type_dict(s->arena);
        return;
    }
    case EX_MEMBER: {
        check_expr(s, e->u.member.obj);
        ChType *ot = e->u.member.obj->type;
        if (ot && ot->kind != TY_DICT && ot->kind != TY_UNKNOWN && ot->kind != TY_ERROR)
            sema_err(s, e->line, e->col, "成员访问需要字典类型，实际为 %s", ch_type_name(ot));
        /* 字典值为任意类型，成员读取结果视为未知 */
        e->type = ch_type_get(s->arena, TY_UNKNOWN);
        return;
    }
    case EX_INDEX: {
        check_expr(s, e->u.index.obj);
        check_expr(s, e->u.index.idx);
        ChType *ot = e->u.index.obj->type;
        if (ot && ot->kind == TY_LIST) {
            e->type = ot->elem ? ot->elem : ch_type_get(s->arena, TY_UNKNOWN);
        } else if (ot && (ot->kind == TY_UNKNOWN || ot->kind == TY_ERROR)) {
            e->type = ch_type_get(s->arena, TY_UNKNOWN);
        } else {
            sema_err(s, e->line, e->col, "索引操作需要列表类型，实际为 %s", ch_type_name(ot));
            e->type = ch_type_get(s->arena, TY_ERROR);
        }
        return;
    }
    case EX_CALL: {
        check_expr_ex(s, e->u.call.callee, true);
        for (int i = 0; i < e->u.call.nargs; i++)
            check_expr(s, e->u.call.args[i]);
        /* 判断是否可静态解析（命名函数或内建），否则经闭包值间接调用 */
        Decl *fd = NULL;
        bool is_static = false;
        if (e->u.call.callee->kind == EX_IDENT) {
            Symbol *sym = ch_scope_lookup(s->scope, e->u.call.callee->u.sval);
            if (sym && sym->kind == SYM_FUNC) {
                is_static = true;
                fd = (Decl *)sym->ast;   /* 可能为 NULL（旧测试手造的符号） */
            } else if (!sym && ch_builtin_find(e->u.call.callee->u.sval)) {
                is_static = true;
            }
        }
        e->call_indirect = !is_static;
        ChType *ft = e->u.call.callee->type;
        if (ft && ft->kind == TY_FUNC) {
            if (!ft->params && ft->nparams == 0 && !ft->elem) {
                /* 裸「函数」注解：无签名信息，跳过实参检查，结果视为未知 */
                e->type = ch_type_get(s->arena, TY_UNKNOWN);
                return;
            }
            if (fd) {
                /* 命名函数：默认值参数可省略，可变参数可多于固定数 */
                int nreq = 0;
                for (int j = 0; j < fd->u.fn.nparams; j++)
                    if (!fd->u.fn.params[j].default_val) nreq++;
                if (e->u.call.nargs < nreq) {
                    sema_err(s, e->line, e->col,
                             "参数数量不足：函数 '%s' 至少需要 %d 个实参，实际 %d",
                             fd->name, nreq, e->u.call.nargs);
                } else if (!fd->u.fn.is_variadic && e->u.call.nargs > ft->nparams) {
                    sema_err(s, e->line, e->col,
                             "参数数量过多：函数 '%s' 期望 %d 个，实际 %d",
                             fd->name, ft->nparams, e->u.call.nargs);
                }
            } else if (!ft->is_variadic && e->u.call.nargs != ft->nparams) {
                sema_err(s, e->line, e->col, "参数数量不匹配：期望 %d，实际 %d",
                         ft->nparams, e->u.call.nargs);
            } else if (ft->is_variadic && e->u.call.nargs < ft->nparams) {
                sema_err(s, e->line, e->col, "参数数量不足：期望至少 %d，实际 %d",
                         ft->nparams, e->u.call.nargs);
            }
            int n = e->u.call.nargs < ft->nparams ? e->u.call.nargs : ft->nparams;
            for (int i = 0; i < n; i++) {
                ChType *want = ft->params[i];
                ChType *got = e->u.call.args[i]->type;
                if (!type_compatible(want, got))
                    sema_err(s, e->u.call.args[i]->line, e->u.call.args[i]->col,
                             "第 %d 个实参类型不匹配：期望 %s，实际 %s",
                             i + 1, ch_type_name(want), ch_type_name(got));
            }
            e->type = ft->elem ? ft->elem : ch_type_get(s->arena, TY_VOID); /* 返回类型 */
        } else if (ft && ft->kind != TY_ERROR) {
            sema_err(s, e->line, e->col, "不可调用的类型 %s", ch_type_name(ft));
            e->type = ch_type_get(s->arena, TY_ERROR);
        } else {
            e->type = ch_type_get(s->arena, TY_ERROR);
        }
        return;
    }
    case EX_LAMBDA: {
        int np = e->u.lambda.nparams;
        Scope *lam = ch_scope_new(s->arena, s->scope);
        ChType **pts = ch_arena_alloc(s->arena, (size_t)(np ? np : 1) * sizeof(ChType *));
        for (int i = 0; i < np; i++) {
            Param *p = &e->u.lambda.params[i];
            pts[i] = p->type_annot ? resolve_type(s, p->type_annot)
                                   : ch_type_get(s->arena, TY_UNKNOWN);
            if (scope_has(lam, p->name, NULL))
                sema_err(s, e->line, e->col, "重复的参数名 '%s'", p->name);
            else
                ch_scope_define(s->arena, lam, SYM_VAR, p->name, pts[i]);
        }
        Scope *saved = s->scope;
        Scope *saved_lam = s->lam_scope;
        s->scope = lam;
        s->lam_scope = lam;
        check_expr(s, e->u.lambda.body);
        s->scope = saved;
        s->lam_scope = saved_lam;
        ChType *rt = e->u.lambda.body && e->u.lambda.body->type
                   ? e->u.lambda.body->type : ch_type_get(s->arena, TY_UNKNOWN);
        e->type = ch_type_func(s->arena, pts, np, false, rt);
        /* 按值捕获分析：收集体内引用的外层局部变量（首次出现序） */
        CapWalk w;
        w.s = s;
        w.lam_scope = lam;
        ch_vec_init(&w.skip, sizeof(const char *));
        ch_vec_init(&w.caps, sizeof(const char *));
        cap_walk_expr(&w, e->u.lambda.body);
        int nc = (int)w.caps.len;
        if (nc > 0) {
            const char **caps = ch_arena_alloc(s->arena, (size_t)nc * sizeof(const char *));
            memcpy(caps, w.caps.data, (size_t)nc * sizeof(const char *));
            e->u.lambda.caps = caps;
            e->u.lambda.ncaps = nc;
        }
        ch_vec_free(&w.skip);
        ch_vec_free(&w.caps);
        return;
    }
    case EX_UNARY: {
        check_expr(s, e->u.unary.operand);
        ChType *ot = e->u.unary.operand->type;
        if (e->u.unary.op == TK_BANG) {
            e->type = ch_type_get(s->arena, TY_BOOL);
        } else if (ch_type_is_numeric(ot)) {
            e->type = ot;
        } else {
            sema_err(s, e->line, e->col, "一元运算符需要数值类型");
            e->type = ch_type_get(s->arena, TY_ERROR);
        }
        return;
    }
    case EX_BINARY: {
        check_expr(s, e->u.binary.lhs);
        check_expr(s, e->u.binary.rhs);
        ChType *lt = e->u.binary.lhs->type;
        ChType *rt = e->u.binary.rhs->type;
        ChType *res = binary_result_type(s, e->u.binary.op, lt, rt, e->line, e->col);
        if (res->kind == TY_ERROR) {
            sema_err(s, e->line, e->col,
                     "运算符 %s 不能用于 %s 和 %s",
                     ch_token_kind_name(e->u.binary.op),
                     ch_type_name(lt), ch_type_name(rt));
        }
        e->type = res;
        return;
    }
    case EX_ASSIGN: {
        check_expr(s, e->u.assign.value);
        check_expr(s, e->u.assign.target);
        if (e->u.assign.target->kind == EX_IDENT &&
            is_outer_local(s, e->u.assign.target->u.sval))
            sema_err(s, e->line, e->col,
                     "匿名函数按值捕获外层变量 '%s'，不能对其赋值",
                     e->u.assign.target->u.sval);
        e->type = e->u.assign.target->type;
        return;
    }
    case EX_GROUP: {
        check_expr(s, e->u.grouped);
        e->type = e->u.grouped->type;
        return;
    }
    }
}

static void check_stmt(Sema *s, Stmt *st);

static void check_block(Sema *s, Stmt *block) {
    if (!block || block->kind != ST_BLOCK) return;
    Scope *inner = ch_scope_new(s->arena, s->scope);
    Scope *saved = s->scope;
    s->scope = inner;
    for (int i = 0; i < block->u.block.count; i++) {
        check_stmt(s, block->u.block.stmts[i]);
    }
    s->scope = saved;
}

static void check_stmt(Sema *s, Stmt *st) {
    if (!st) return;
    switch (st->kind) {
    case ST_VAR: {
        if (st->u.var.init) check_expr(s, st->u.var.init);
        ChType *t;
        if (st->u.var.annot) {
            t = resolve_type(s, st->u.var.annot);
            if (st->u.var.init && !type_compatible(t, st->u.var.init->type)) {
                sema_err(s, st->line, st->col, "类型不匹配：标注 %s，初值 %s",
                         ch_type_name(t), ch_type_name(st->u.var.init->type));
            }
        } else {
            t = st->u.var.init ? st->u.var.init->type : ch_type_get(s->arena, TY_UNKNOWN);
        }
        /* 检查重复定义 */
        if (ch_scope_lookup(s->scope, st->u.var.name))
            sema_err(s, st->line, st->col, "重复定义 '%s'", st->u.var.name);
        else
            ch_scope_define(s->arena, s->scope, SYM_VAR, st->u.var.name, t);
        return;
    }
    case ST_CONST: {
        check_expr(s, st->u.konst.init);
        ChType *t = st->u.konst.init->type;
        if (ch_scope_lookup(s->scope, st->u.konst.name))
            sema_err(s, st->line, st->col, "重复定义 '%s'", st->u.konst.name);
        else
            ch_scope_define(s->arena, s->scope, SYM_CONST, st->u.konst.name, t);
        return;
    }
    case ST_ASSIGN:
        check_expr(s, st->u.assign.value);
        check_expr(s, st->u.assign.target);
        if (st->u.assign.target->kind == EX_IDENT &&
            is_outer_local(s, st->u.assign.target->u.sval))
            sema_err(s, st->line, st->col,
                     "匿名函数按值捕获外层变量 '%s'，不能对其赋值",
                     st->u.assign.target->u.sval);
        return;
    case ST_RETURN: {
        if (st->u.ret.value) check_expr(s, st->u.ret.value);
        if (s->fn_ret_type && st->u.ret.value) {
            ChType *rt = st->u.ret.value->type;
            if (!type_compatible(s->fn_ret_type, rt)) {
                sema_err(s, st->line, st->col, "返回类型不匹配：函数返回 %s，实际 %s",
                         ch_type_name(s->fn_ret_type), ch_type_name(rt));
            }
        }
        return;
    }
    case ST_IF:
        check_expr(s, st->u.iff.cond);
        check_block(s, st->u.iff.then_b);
        if (st->u.iff.else_b) check_block(s, st->u.iff.else_b);
        return;
    case ST_WHILE: {
        check_expr(s, st->u.whil.cond);
        bool saved_loop = s->in_loop;
        s->in_loop = true;
        check_block(s, st->u.whil.body);
        s->in_loop = saved_loop;
        return;
    }
    case ST_FOR: {
        check_expr(s, st->u.forn.start);
        check_expr(s, st->u.forn.end);
        Scope *inner = ch_scope_new(s->arena, s->scope);
        Scope *saved = s->scope;
        s->scope = inner;
        ChType *it = ch_type_get(s->arena, TY_INT);
        ch_scope_define(s->arena, inner, SYM_VAR, st->u.forn.var, it);
        bool saved_loop = s->in_loop;
        s->in_loop = true;
        check_block(s, st->u.forn.body);
        s->in_loop = saved_loop;
        s->scope = saved;
        return;
    }
    case ST_BREAK:
    case ST_CONTINUE:
        if (!s->in_loop)
            sema_err(s, st->line, st->col, "%s 只能在循环内使用",
                     st->kind == ST_BREAK ? "跳出" : "继续");
        return;
    case ST_PRINT:
        check_expr(s, st->u.prnt.expr);
        return;
    case ST_EXPR:
        check_expr(s, st->u.expr.expr);
        return;
    case ST_BLOCK:
        check_block(s, st);
        return;
    }
}

static void check_decl(Sema *s, Decl *d);

/* 预扫描：把所有顶层函数签名加入全局作用域（支持前向引用） */
static void predeclare_funcs(Sema *s, AstNode **decls, int ndecls) {
    for (int i = 0; i < ndecls; i++) {
        Decl *d = decls[i]->as.decl;
        if (d->kind != DECL_FUNC) continue;
        ChType **params = ch_arena_alloc(s->arena, d->u.fn.nparams * sizeof(ChType*));
        for (int j = 0; j < d->u.fn.nparams; j++)
            params[j] = resolve_type(s, d->u.fn.params[j].type_annot);
        ChType *ret = d->u.fn.ret_annot ? resolve_type(s, d->u.fn.ret_annot)
                                         : ch_type_get(s->arena, TY_VOID);
        ChType *ft = ch_type_func(s->arena, params, d->u.fn.nparams, d->u.fn.is_variadic, ret);
        d->type = ft;
        if (!ch_scope_lookup(s->scope, d->name)) {
            Symbol *fsym = ch_scope_define(s->arena, s->scope, SYM_FUNC, d->name, ft);
            fsym->ast = d;   /* 供默认值/可变参数的调用检查取回声明 */
        } else {
            sema_err(s, d->line, d->col, "重复定义函数 '%s'", d->name);
        }
    }
}

static void check_decl(Sema *s, Decl *d) {
    switch (d->kind) {
    case DECL_FUNC: {
        ChType *ft = d->type;
        ChType *ret = ft->elem;
        /* 函数作用域 */
        Scope *fn_scope = ch_scope_new(s->arena, s->scope);
        bool seen_default = false;
        for (int i = 0; i < d->u.fn.nparams; i++) {
            Param *p = &d->u.fn.params[i];
            if (p->default_val) {
                seen_default = true;
            } else if (seen_default) {
                sema_err(s, d->line, d->col,
                         "带默认值的参数之后不能再出现无默认值参数（参数 '%s'）", p->name);
            }
            ChType *pt = resolve_type(s, p->type_annot);
            Symbol *sym = ch_scope_define(s->arena, fn_scope, SYM_VAR, p->name, pt);
            sym->is_param = true;
            sym->param_index = i;
            if (p->default_val) check_expr(s, p->default_val);
        }
        /* 可变参数收集名：函数体内视为 列表<未知> */
        if (d->u.fn.is_variadic && d->u.fn.variadic_name) {
            if (scope_has(fn_scope, d->u.fn.variadic_name, NULL))
                sema_err(s, d->line, d->col, "重复定义 '%s'", d->u.fn.variadic_name);
            else
                ch_scope_define(s->arena, fn_scope, SYM_VAR, d->u.fn.variadic_name,
                                ch_type_list(s->arena, ch_type_get(s->arena, TY_UNKNOWN)));
        }
        Scope *saved = s->scope;
        ChType *saved_ret = s->fn_ret_type;
        s->scope = fn_scope;
        s->fn_ret_type = ret;
        bool saved_loop = s->in_loop;
        s->in_loop = false;
        check_block(s, d->u.fn.body);
        s->scope = saved;
        s->fn_ret_type = saved_ret;
        s->in_loop = saved_loop;
        return;
    }
    case DECL_VAR: {
        if (d->u.var.init) check_expr(s, d->u.var.init);
        ChType *t;
        if (d->u.var.annot) {
            t = resolve_type(s, d->u.var.annot);
            if (d->u.var.init && !type_compatible(t, d->u.var.init->type)) {
                sema_err(s, d->line, d->col, "类型不匹配：标注 %s，初值 %s",
                         ch_type_name(t), ch_type_name(d->u.var.init->type));
            }
        } else {
            t = d->u.var.init ? d->u.var.init->type : ch_type_get(s->arena, TY_UNKNOWN);
        }
        d->type = t;
        if (ch_scope_lookup(s->scope, d->name))
            sema_err(s, d->line, d->col, "重复定义 '%s'", d->name);
        else
            ch_scope_define(s->arena, s->scope, SYM_VAR, d->name, t);
        return;
    }
    case DECL_CONST: {
        check_expr(s, d->u.konst.init);
        ChType *t = d->u.konst.init->type;
        d->type = t;
        if (ch_scope_lookup(s->scope, d->name))
            sema_err(s, d->line, d->col, "重复定义 '%s'", d->name);
        else
            ch_scope_define(s->arena, s->scope, SYM_CONST, d->name, t);
        return;
    }
    case DECL_STMT:
        check_stmt(s, d->u.stmt);
        return;
    case DECL_IMPORT:
        /* 正常路径下加载器已剥除导入声明；此处为保险 */
        return;
    }
}

bool ch_sema_check(ChArena *arena, ChDiagList *diags, AstNode **decls, int ndecls) {
    Scope *global = ch_scope_new(arena, NULL);
    Sema s = { arena, diags, global, global, NULL, NULL, false };
    predeclare_funcs(&s, decls, ndecls);
    for (int i = 0; i < ndecls; i++)
        check_decl(&s, decls[i]->as.decl);
    return ch_diag_error_count(diags) == 0;
}
