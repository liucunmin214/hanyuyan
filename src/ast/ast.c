#include "ast/ast.h"
#include "util/arena.h"
#include <stdio.h>
#include <string.h>

/* ==================== 表达式构造 ==================== */

static Expr *new_expr(ChArena *a, ExprKind k, int line, int col) {
    Expr *e = ch_arena_alloc(a, sizeof(Expr));
    memset(e, 0, sizeof(*e));
    e->kind = k;
    e->line = line;
    e->col = col;
    return e;
}

Expr *ch_ast_int(ChArena *a, int line, int col, long long v) {
    Expr *e = new_expr(a, EX_INT, line, col); e->u.ival = v; return e;
}
Expr *ch_ast_float(ChArena *a, int line, int col, double v) {
    Expr *e = new_expr(a, EX_FLOAT, line, col); e->u.fval = v; return e;
}
Expr *ch_ast_string(ChArena *a, int line, int col, const char *s) {
    Expr *e = new_expr(a, EX_STRING, line, col); e->u.sval = s; return e;
}
Expr *ch_ast_bool(ChArena *a, int line, int col, bool v) {
    Expr *e = new_expr(a, EX_BOOL, line, col); e->u.bval = v; return e;
}
Expr *ch_ast_nil(ChArena *a, int line, int col) {
    return new_expr(a, EX_NIL, line, col);
}
Expr *ch_ast_ident(ChArena *a, int line, int col, const char *name) {
    Expr *e = new_expr(a, EX_IDENT, line, col); e->u.sval = name; return e;
}
Expr *ch_ast_list(ChArena *a, int line, int col, Expr **items, int count) {
    Expr *e = new_expr(a, EX_LIST, line, col);
    e->u.list.items = items; e->u.list.count = count; return e;
}
Expr *ch_ast_dict(ChArena *a, int line, int col, const char **keys, Expr **vals, int count) {
    Expr *e = new_expr(a, EX_DICT, line, col);
    e->u.dict.keys = keys; e->u.dict.vals = vals; e->u.dict.count = count; return e;
}
Expr *ch_ast_member(ChArena *a, int line, int col, Expr *obj, const char *name) {
    Expr *e = new_expr(a, EX_MEMBER, line, col);
    e->u.member.obj = obj; e->u.member.name = name; return e;
}
Expr *ch_ast_lambda(ChArena *a, int line, int col, Param *params, int nparams, Expr *body) {
    Expr *e = new_expr(a, EX_LAMBDA, line, col);
    e->u.lambda.params = params; e->u.lambda.nparams = nparams;
    e->u.lambda.body = body;
    e->u.lambda.caps = NULL; e->u.lambda.ncaps = 0; return e;
}
Expr *ch_ast_index(ChArena *a, int line, int col, Expr *obj, Expr *idx) {
    Expr *e = new_expr(a, EX_INDEX, line, col);
    e->u.index.obj = obj; e->u.index.idx = idx; return e;
}
Expr *ch_ast_call(ChArena *a, int line, int col, Expr *callee, Expr **args, int nargs) {
    Expr *e = new_expr(a, EX_CALL, line, col);
    e->u.call.callee = callee; e->u.call.args = args; e->u.call.nargs = nargs; return e;
}
Expr *ch_ast_unary(ChArena *a, int line, int col, TokenKind op, Expr *operand) {
    Expr *e = new_expr(a, EX_UNARY, line, col);
    e->u.unary.op = op; e->u.unary.operand = operand; return e;
}
Expr *ch_ast_binary(ChArena *a, int line, int col, TokenKind op, Expr *lhs, Expr *rhs) {
    Expr *e = new_expr(a, EX_BINARY, line, col);
    e->u.binary.op = op; e->u.binary.lhs = lhs; e->u.binary.rhs = rhs; return e;
}
Expr *ch_ast_assign(ChArena *a, int line, int col, Expr *target, Expr *value) {
    Expr *e = new_expr(a, EX_ASSIGN, line, col);
    e->u.assign.target = target; e->u.assign.value = value; return e;
}
Expr *ch_ast_group(ChArena *a, int line, int col, Expr *inner) {
    Expr *e = new_expr(a, EX_GROUP, line, col);
    e->u.grouped = inner; return e;
}

/* ==================== 语句构造 ==================== */

static Stmt *new_stmt(ChArena *a, StmtKind k, int line, int col) {
    Stmt *s = ch_arena_alloc(a, sizeof(Stmt));
    memset(s, 0, sizeof(*s));
    s->kind = k;
    s->line = line;
    s->col = col;
    return s;
}

Stmt *ch_stmt_var(ChArena *a, int line, int col, const char *name, ChType *annot, Expr *init) {
    Stmt *s = new_stmt(a, ST_VAR, line, col);
    s->u.var.name = name; s->u.var.annot = annot; s->u.var.init = init; return s;
}
Stmt *ch_stmt_const(ChArena *a, int line, int col, const char *name, Expr *init) {
    Stmt *s = new_stmt(a, ST_CONST, line, col);
    s->u.konst.name = name; s->u.konst.init = init; return s;
}
Stmt *ch_stmt_assign(ChArena *a, int line, int col, Expr *target, Expr *value) {
    Stmt *s = new_stmt(a, ST_ASSIGN, line, col);
    s->u.assign.target = target; s->u.assign.value = value; return s;
}
Stmt *ch_stmt_return(ChArena *a, int line, int col, Expr *value) {
    Stmt *s = new_stmt(a, ST_RETURN, line, col);
    s->u.ret.value = value; return s;
}
Stmt *ch_stmt_if(ChArena *a, int line, int col, Expr *cond, Stmt *then_b, Stmt *else_b) {
    Stmt *s = new_stmt(a, ST_IF, line, col);
    s->u.iff.cond = cond; s->u.iff.then_b = then_b; s->u.iff.else_b = else_b; return s;
}
Stmt *ch_stmt_while(ChArena *a, int line, int col, Expr *cond, Stmt *body) {
    Stmt *s = new_stmt(a, ST_WHILE, line, col);
    s->u.whil.cond = cond; s->u.whil.body = body; return s;
}
Stmt *ch_stmt_for(ChArena *a, int line, int col, const char *var, Expr *start, Expr *end, Stmt *body) {
    Stmt *s = new_stmt(a, ST_FOR, line, col);
    s->u.forn.var = var; s->u.forn.start = start; s->u.forn.end = end; s->u.forn.body = body; return s;
}
Stmt *ch_stmt_break(ChArena *a, int line, int col) { return new_stmt(a, ST_BREAK, line, col); }
Stmt *ch_stmt_continue(ChArena *a, int line, int col) { return new_stmt(a, ST_CONTINUE, line, col); }
Stmt *ch_stmt_print(ChArena *a, int line, int col, Expr *expr) {
    Stmt *s = new_stmt(a, ST_PRINT, line, col);
    s->u.prnt.expr = expr; return s;
}
Stmt *ch_stmt_expr(ChArena *a, int line, int col, Expr *expr) {
    Stmt *s = new_stmt(a, ST_EXPR, line, col);
    s->u.expr.expr = expr; return s;
}
Stmt *ch_stmt_block(ChArena *a, int line, int col, Stmt **stmts, int count) {
    Stmt *s = new_stmt(a, ST_BLOCK, line, col);
    s->u.block.stmts = stmts; s->u.block.count = count; return s;
}

/* ==================== 声明构造 ==================== */

static Decl *new_decl(ChArena *a, DeclKind k, int line, int col) {
    Decl *d = ch_arena_alloc(a, sizeof(Decl));
    memset(d, 0, sizeof(*d));
    d->kind = k;
    d->line = line;
    d->col = col;
    return d;
}

Decl *ch_decl_func(ChArena *a, int line, int col, const char *name,
                   Param *params, int nparams, bool is_variadic,
                   const char *variadic_name, ChType *ret_annot, Stmt *body) {
    Decl *d = new_decl(a, DECL_FUNC, line, col);
    d->name = name;
    d->u.fn.params = params;
    d->u.fn.nparams = nparams;
    d->u.fn.is_variadic = is_variadic;
    d->u.fn.variadic_name = variadic_name;
    d->u.fn.ret_annot = ret_annot;
    d->u.fn.body = body;
    return d;
}
Decl *ch_decl_var(ChArena *a, int line, int col, const char *name, ChType *annot, Expr *init) {
    Decl *d = new_decl(a, DECL_VAR, line, col);
    d->name = name; d->u.var.annot = annot; d->u.var.init = init; return d;
}
Decl *ch_decl_const(ChArena *a, int line, int col, const char *name, Expr *init) {
    Decl *d = new_decl(a, DECL_CONST, line, col);
    d->name = name; d->u.konst.init = init; return d;
}
Decl *ch_decl_stmt(ChArena *a, int line, int col, Stmt *stmt) {
    Decl *d = new_decl(a, DECL_STMT, line, col);
    d->u.stmt = stmt; return d;
}
Decl *ch_decl_import(ChArena *a, int line, int col, const char *path) {
    Decl *d = new_decl(a, DECL_IMPORT, line, col);
    d->u.import.path = path; return d;
}

/* ==================== 节点包装 ==================== */

AstNode *ch_node_expr(ChArena *a, Expr *e) {
    AstNode *n = ch_arena_alloc(a, sizeof(AstNode));
    n->kind = NODE_EXPR; n->as.expr = e; return n;
}
AstNode *ch_node_stmt(ChArena *a, Stmt *s) {
    AstNode *n = ch_arena_alloc(a, sizeof(AstNode));
    n->kind = NODE_STMT; n->as.stmt = s; return n;
}
AstNode *ch_node_decl(ChArena *a, Decl *d) {
    AstNode *n = ch_arena_alloc(a, sizeof(AstNode));
    n->kind = NODE_DECL; n->as.decl = d; return n;
}

/* ==================== Dump ==================== */

static void pad(int n, FILE *out) {
    for (int i = 0; i < n; i++) fputc(' ', out);
}

static const char *op_name(TokenKind k) {
    switch (k) {
    case TK_PLUS: return "+";
    case TK_MINUS: return "-";
    case TK_STAR: return "*";
    case TK_SLASH: return "/";
    case TK_PERCENT: return "%";
    case TK_CARET: return "^";
    case TK_EQ: return "==";
    case TK_NE: return "!=";
    case TK_LT: return "<";
    case TK_LE: return "<=";
    case TK_GT: return ">";
    case TK_GE: return ">=";
    case TK_ANDAND: return "&&";
    case TK_OROR: return "||";
    case TK_BANG: return "!";
    default: return "?";
    }
}

void ch_ast_dump_expr(const Expr *e, int indent, FILE *out) {
    if (!e) { pad(indent, out); fprintf(out, "(空)\n"); return; }
    pad(indent, out);
    switch (e->kind) {
    case EX_INT:    fprintf(out, "整数 %lld", e->u.ival); break;
    case EX_FLOAT:  fprintf(out, "小数 %g", e->u.fval); break;
    case EX_STRING: fprintf(out, "文字 \"%s\"", e->u.sval); break;
    case EX_BOOL:   fprintf(out, "布尔 %s", e->u.bval ? "真" : "假"); break;
    case EX_NIL:    fprintf(out, "空"); break;
    case EX_IDENT:  fprintf(out, "标识符 %s", e->u.sval); break;
    case EX_LIST:
        fprintf(out, "列表[%d]\n", e->u.list.count);
        for (int i = 0; i < e->u.list.count; i++)
            ch_ast_dump_expr(e->u.list.items[i], indent + 2, out);
        return;
    case EX_DICT:
        fprintf(out, "字典[%d]\n", e->u.dict.count);
        for (int i = 0; i < e->u.dict.count; i++) {
            pad(indent + 2, out);
            fprintf(out, "%s:\n", e->u.dict.keys[i]);
            ch_ast_dump_expr(e->u.dict.vals[i], indent + 4, out);
        }
        return;
    case EX_MEMBER:
        fprintf(out, "成员 .%s\n", e->u.member.name);
        ch_ast_dump_expr(e->u.member.obj, indent + 2, out);
        return;
    case EX_LAMBDA:
        fprintf(out, "匿名函数(");
        for (int i = 0; i < e->u.lambda.nparams; i++) {
            if (i) fprintf(out, ", ");
            fprintf(out, "%s", e->u.lambda.params[i].name);
            if (e->u.lambda.params[i].type_annot)
                fprintf(out, ":%s", ch_type_name(e->u.lambda.params[i].type_annot));
        }
        fprintf(out, ") =>");
        if (e->u.lambda.ncaps > 0) {
            fprintf(out, " [捕获");
            for (int i = 0; i < e->u.lambda.ncaps; i++)
                fprintf(out, " %s", e->u.lambda.caps[i]);
            fprintf(out, "]");
        }
        fputc('\n', out);
        ch_ast_dump_expr(e->u.lambda.body, indent + 2, out);
        return;
    case EX_INDEX:
        fprintf(out, "索引\n");
        ch_ast_dump_expr(e->u.index.obj, indent + 2, out);
        ch_ast_dump_expr(e->u.index.idx, indent + 2, out);
        return;
    case EX_CALL:
        fprintf(out, "调用\n");
        ch_ast_dump_expr(e->u.call.callee, indent + 2, out);
        for (int i = 0; i < e->u.call.nargs; i++)
            ch_ast_dump_expr(e->u.call.args[i], indent + 2, out);
        return;
    case EX_UNARY:
        fprintf(out, "一元 %s\n", op_name(e->u.unary.op));
        ch_ast_dump_expr(e->u.unary.operand, indent + 2, out);
        return;
    case EX_BINARY:
        fprintf(out, "二元 %s\n", op_name(e->u.binary.op));
        ch_ast_dump_expr(e->u.binary.lhs, indent + 2, out);
        ch_ast_dump_expr(e->u.binary.rhs, indent + 2, out);
        return;
    case EX_ASSIGN:
        fprintf(out, "赋值\n");
        ch_ast_dump_expr(e->u.assign.target, indent + 2, out);
        ch_ast_dump_expr(e->u.assign.value, indent + 2, out);
        return;
    case EX_GROUP:
        fprintf(out, "分组\n");
        ch_ast_dump_expr(e->u.grouped, indent + 2, out);
        return;
    }
    if (e->type) fprintf(out, " : %s", ch_type_name(e->type));
    fputc('\n', out);
}

void ch_ast_dump_stmt(const Stmt *s, int indent, FILE *out) {
    if (!s) { pad(indent, out); fprintf(out, "(空语句)\n"); return; }
    pad(indent, out);
    switch (s->kind) {
    case ST_VAR:
        fprintf(out, "变量 %s", s->u.var.name);
        if (s->u.var.annot) fprintf(out, " : %s", ch_type_name(s->u.var.annot));
        fputc('\n', out);
        if (s->u.var.init) ch_ast_dump_expr(s->u.var.init, indent + 2, out);
        return;
    case ST_CONST:
        fprintf(out, "常量 %s\n", s->u.konst.name);
        ch_ast_dump_expr(s->u.konst.init, indent + 2, out);
        return;
    case ST_ASSIGN:
        fprintf(out, "赋值语句\n");
        ch_ast_dump_expr(s->u.assign.target, indent + 2, out);
        ch_ast_dump_expr(s->u.assign.value, indent + 2, out);
        return;
    case ST_RETURN:
        fprintf(out, "返回\n");
        if (s->u.ret.value) ch_ast_dump_expr(s->u.ret.value, indent + 2, out);
        return;
    case ST_IF:
        fprintf(out, "如果\n");
        ch_ast_dump_expr(s->u.iff.cond, indent + 2, out);
        pad(indent + 2, out); fprintf(out, "那么\n");
        ch_ast_dump_stmt(s->u.iff.then_b, indent + 4, out);
        if (s->u.iff.else_b) {
            pad(indent + 2, out); fprintf(out, "否则\n");
            ch_ast_dump_stmt(s->u.iff.else_b, indent + 4, out);
        }
        return;
    case ST_WHILE:
        fprintf(out, "当\n");
        ch_ast_dump_expr(s->u.whil.cond, indent + 2, out);
        ch_ast_dump_stmt(s->u.whil.body, indent + 2, out);
        return;
    case ST_FOR:
        fprintf(out, "对于 %s\n", s->u.forn.var);
        ch_ast_dump_expr(s->u.forn.start, indent + 2, out);
        ch_ast_dump_expr(s->u.forn.end, indent + 2, out);
        ch_ast_dump_stmt(s->u.forn.body, indent + 2, out);
        return;
    case ST_BREAK:    fprintf(out, "跳出\n"); return;
    case ST_CONTINUE: fprintf(out, "继续\n"); return;
    case ST_PRINT:
        fprintf(out, "打印\n");
        ch_ast_dump_expr(s->u.prnt.expr, indent + 2, out);
        return;
    case ST_EXPR:
        fprintf(out, "表达式语句\n");
        ch_ast_dump_expr(s->u.expr.expr, indent + 2, out);
        return;
    case ST_BLOCK:
        fprintf(out, "块[%d]\n", s->u.block.count);
        for (int i = 0; i < s->u.block.count; i++)
            ch_ast_dump_stmt(s->u.block.stmts[i], indent + 2, out);
        return;
    }
}

void ch_ast_dump_decl(const Decl *d, int indent, FILE *out) {
    pad(indent, out);
    switch (d->kind) {
    case DECL_FUNC:
        fprintf(out, "函数 %s(", d->name);
        for (int i = 0; i < d->u.fn.nparams; i++) {
            if (i) fprintf(out, ", ");
            fprintf(out, "%s", d->u.fn.params[i].name);
            if (d->u.fn.params[i].type_annot)
                fprintf(out, ":%s", ch_type_name(d->u.fn.params[i].type_annot));
        }
        if (d->u.fn.is_variadic) {
            if (d->u.fn.nparams) fprintf(out, ", ");
            fprintf(out, "...");
            if (d->u.fn.variadic_name) fprintf(out, "%s", d->u.fn.variadic_name);
        }
        fprintf(out, ")");
        if (d->u.fn.ret_annot) fprintf(out, " -> %s", ch_type_name(d->u.fn.ret_annot));
        fputc('\n', out);
        if (d->u.fn.body) ch_ast_dump_stmt(d->u.fn.body, indent + 2, out);
        return;
    case DECL_VAR:
        fprintf(out, "变量 %s", d->name);
        if (d->u.var.annot) fprintf(out, " : %s", ch_type_name(d->u.var.annot));
        fputc('\n', out);
        if (d->u.var.init) ch_ast_dump_expr(d->u.var.init, indent + 2, out);
        return;
    case DECL_CONST:
        fprintf(out, "常量 %s\n", d->name);
        ch_ast_dump_expr(d->u.konst.init, indent + 2, out);
        return;
    case DECL_STMT:
        ch_ast_dump_stmt(d->u.stmt, indent, out);
        return;
    case DECL_IMPORT:
        fprintf(out, "导入 \"%s\"\n", d->u.import.path);
        return;
    }
}
