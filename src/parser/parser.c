#include "parser/parser.h"
#include "lexer/token.h"
#include "util/arena.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* 递归下降解析器。优先级（低→高）：
 *   assignment -> or -> and -> equality -> relational
 *   -> additive -> multiplicative -> power(右结合) -> unary -> postfix -> primary
 */

typedef struct {
    ChArena     *arena;
    ChDiagList  *diags;
    const Token *tokens;
    size_t       pos;
    size_t       ntokens;
} Parser;

/* ---------- 工具 ---------- */

static const Token *peek(Parser *p) {
    return &p->tokens[p->pos < p->ntokens ? p->pos : p->ntokens - 1];
}
static void advance(Parser *p) { if (p->pos < p->ntokens) p->pos++; }

static bool check(Parser *p, TokenKind k) { return peek(p)->kind == k; }

static bool match(Parser *p, TokenKind k) {
    if (check(p, k)) { advance(p); return true; }
    return false;
}

static const Token *expect(Parser *p, TokenKind k, const char *what) {
    const Token *t = peek(p);
    if (t->kind != k) {
        ch_diag_add(p->diags, true, t->line, t->col,
                    "期望 %s，实际为 %s", what, ch_token_kind_name(t->kind));
    } else {
        advance(p);
    }
    return t;
}

/* 跳过换行记号（语句分隔符） */
static void skip_newlines(Parser *p) {
    while (check(p, TK_NEWLINE)) advance(p);
}

/* 错误恢复：跳到下一条语句边界（换行/结束/EOF） */
static void sync_stmt(Parser *p) {
    while (!check(p, TK_EOF) && !check(p, TK_NEWLINE) && !check(p, TK_KW_END))
        advance(p);
    if (check(p, TK_NEWLINE)) advance(p);
}

static void err(Parser *p, int line, int col, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ch_diag_add(p->diags, true, line, col, "%s", buf);
}

/* ---------- 类型标注解析 ---------- */
/* 类型名：整数 / 小数 / 文字 / 空 / 列表 / 列表<类型> */
static ChType *parse_type(Parser *p) {
    const Token *t = peek(p);
    /* 「函数」是关键字，作为类型名时直接接受 */
    if (t->kind == TK_KW_FN) {
        advance(p);
        ChType *ty = ch_arena_alloc(p->arena, sizeof(ChType));
        memset(ty, 0, sizeof(*ty));
        ty->kind = TY_FUNC;
        return ty;
    }
    if (t->kind != TK_IDENT) {
        err(p, t->line, t->col, "期望类型名");
        return NULL;
    }
    const char *name = t->text;
    advance(p);
    ChType *ty = ch_arena_alloc(p->arena, sizeof(ChType));
    memset(ty, 0, sizeof(*ty));
    if (strcmp(name, "整数") == 0) ty->kind = TY_INT;
    else if (strcmp(name, "小数") == 0) ty->kind = TY_FLOAT;
    else if (strcmp(name, "文字") == 0) ty->kind = TY_STRING;
    else if (strcmp(name, "空") == 0) ty->kind = TY_VOID;
    else if (strcmp(name, "布尔") == 0) ty->kind = TY_BOOL;
    else if (strcmp(name, "字典") == 0) ty->kind = TY_DICT;
    else if (strcmp(name, "函数") == 0) ty->kind = TY_FUNC;
    else if (strcmp(name, "列表") == 0) {
        ty->kind = TY_LIST;
        /* 可选 <元素类型> */
        if (match(p, TK_LT)) {
            ty->elem = parse_type(p);
            expect(p, TK_GT, "'>'");
        }
    } else {
        err(p, t->line, t->col, "未知类型 '%s'", name);
        ty->kind = TY_ERROR;
    }
    return ty;
}

/* ---------- 前向声明 ---------- */
static Expr *parse_expr(Parser *p);
static Stmt *parse_stmt(Parser *p);
static Stmt *parse_block(Parser *p);

/* 从当前 '(' 起扫描到配对的 ')'，若其后是 '=>' 则按匿名函数解析 */
static bool is_lambda_ahead(Parser *p) {
    if (!check(p, TK_LPAREN)) return false;
    size_t i = p->pos;
    int depth = 0;
    for (; i < p->ntokens; i++) {
        TokenKind k = p->tokens[i].kind;
        if (k == TK_LPAREN) depth++;
        else if (k == TK_RPAREN) { depth--; if (depth == 0) break; }
        else if (k == TK_EOF) break;
    }
    return i < p->ntokens && depth == 0 && i + 1 < p->ntokens &&
           p->tokens[i + 1].kind == TK_FATARROW;
}

/* 匿名函数：(参数[:类型], ...) => 表达式 */
static Expr *parse_lambda(Parser *p) {
    const Token *t = peek(p);   /* '(' */
    int line = t->line, col = t->col;
    advance(p);
    ChVec params;
    ch_vec_init(&params, sizeof(Param));
    skip_newlines(p);
    if (!check(p, TK_RPAREN)) {
        do {
            skip_newlines(p);
            const Token *pn = expect(p, TK_IDENT, "参数名");
            ChType *pa = NULL;
            if (match(p, TK_COLON)) pa = parse_type(p);
            Param *slot = ch_vec_push(&params);
            slot->name = pn->text;
            slot->type_annot = pa;
            slot->default_val = NULL;
            skip_newlines(p);
        } while (match(p, TK_COMMA));
    }
    skip_newlines(p);
    expect(p, TK_RPAREN, "')'");
    expect(p, TK_FATARROW, "'=>'");
    Expr *body = parse_expr(p);
    int pc = (int)params.len;
    Param *parr = ch_arena_alloc(p->arena, (size_t)(pc ? pc : 1) * sizeof(Param));
    if (pc) memcpy(parr, params.data, (size_t)pc * sizeof(Param));
    ch_vec_free(&params);
    return ch_ast_lambda(p->arena, line, col, parr, pc, body);
}

/* ---------- 主表达式 ---------- */
static Expr *parse_primary(Parser *p) {
    const Token *t = peek(p);
    int line = t->line, col = t->col;
    switch (t->kind) {
    case TK_INT:    advance(p); return ch_ast_int(p->arena, line, col, t->ival);
    case TK_FLOAT:  advance(p); return ch_ast_float(p->arena, line, col, t->fval);
    case TK_STRING: advance(p); return ch_ast_string(p->arena, line, col, t->sval);
    case TK_KW_TRUE:  advance(p); return ch_ast_bool(p->arena, line, col, true);
    case TK_KW_FALSE: advance(p); return ch_ast_bool(p->arena, line, col, false);
    case TK_KW_NIL: advance(p); return ch_ast_nil(p->arena, line, col);
    case TK_IDENT: {
        advance(p);
        return ch_ast_ident(p->arena, line, col, t->text);
    }
    case TK_LPAREN: {
        if (is_lambda_ahead(p)) return parse_lambda(p);
        advance(p);
        Expr *e = parse_expr(p);
        expect(p, TK_RPAREN, "')'");
        return ch_ast_group(p->arena, line, col, e);
    }
    case TK_LBRACE: {
        /* 字典字面量 {键: 值, ...}；键为标识符或文字 */
        advance(p);
        ChVec keys, vals;
        ch_vec_init(&keys, sizeof(const char *));
        ch_vec_init(&vals, sizeof(Expr *));
        skip_newlines(p);
        if (!check(p, TK_RBRACE)) {
            do {
                skip_newlines(p);
                const Token *k = peek(p);
                const char **ks = ch_vec_push(&keys);
                if (k->kind == TK_IDENT) { *ks = k->text; advance(p); }
                else if (k->kind == TK_STRING) { *ks = k->sval; advance(p); }
                else {
                    err(p, k->line, k->col, "字典键应为标识符或文字");
                    *ks = "";
                    advance(p);
                }
                expect(p, TK_COLON, "':'");
                skip_newlines(p);
                Expr *v = parse_expr(p);
                Expr **vs = ch_vec_push(&vals);
                *vs = v;
                skip_newlines(p);
            } while (match(p, TK_COMMA));
        }
        skip_newlines(p);
        expect(p, TK_RBRACE, "'}'");
        int n = (int)vals.len;
        const char **ka = ch_arena_alloc(p->arena, (size_t)(n ? n : 1) * sizeof(const char *));
        Expr **va = ch_arena_alloc(p->arena, (size_t)(n ? n : 1) * sizeof(Expr *));
        memcpy(ka, keys.data, (size_t)n * sizeof(const char *));
        memcpy(va, vals.data, (size_t)n * sizeof(Expr *));
        ch_vec_free(&keys);
        ch_vec_free(&vals);
        return ch_ast_dict(p->arena, line, col, ka, va, n);
    }
    case TK_LBRACKET: {
        /* 列表字面量 */
        advance(p);
        ChVec items;
        ch_vec_init(&items, sizeof(Expr*));
        skip_newlines(p);
        if (!check(p, TK_RBRACKET)) {
            do {
                skip_newlines(p);
                Expr *e = parse_expr(p);
                Expr **slot = ch_vec_push(&items);
                *slot = e;
                skip_newlines(p);
            } while (match(p, TK_COMMA));
        }
        skip_newlines(p);
        expect(p, TK_RBRACKET, "']'");
        int ic = (int)items.len;
        Expr **arr = ch_arena_alloc(p->arena, ic * sizeof(Expr*));
        memcpy(arr, items.data, ic * sizeof(Expr*));
        ch_vec_free(&items);
        return ch_ast_list(p->arena, line, col, arr, ic);
    }
    default:
        err(p, line, col, "意外的记号 %s", ch_token_kind_name(t->kind));
        advance(p);
        return ch_ast_nil(p->arena, line, col);
    }
}

/* ---------- 后缀：调用 / 索引 ---------- */
static Expr *parse_postfix(Parser *p) {
    Expr *e = parse_primary(p);
    for (;;) {
        const Token *t = peek(p);
        if (t->kind == TK_LPAREN) {
            advance(p);
            ChVec args;
            ch_vec_init(&args, sizeof(Expr*));
            skip_newlines(p);
            if (!check(p, TK_RPAREN)) {
                do {
                    skip_newlines(p);
                    Expr *a = parse_expr(p);
                    Expr **slot = ch_vec_push(&args);
                    *slot = a;
                    skip_newlines(p);
                } while (match(p, TK_COMMA));
            }
            skip_newlines(p);
            expect(p, TK_RPAREN, "')'");
            int ac = (int)args.len;
            Expr **arr = ch_arena_alloc(p->arena, ac * sizeof(Expr*));
            memcpy(arr, args.data, ac * sizeof(Expr*));
            ch_vec_free(&args);
            e = ch_ast_call(p->arena, e->line, e->col, e, arr, ac);
        } else if (t->kind == TK_LBRACKET) {
            advance(p);
            Expr *idx = parse_expr(p);
            expect(p, TK_RBRACKET, "']'");
            e = ch_ast_index(p->arena, e->line, e->col, e, idx);
        } else if (t->kind == TK_DOT) {
            advance(p);
            const Token *name = expect(p, TK_IDENT, "成员名");
            e = ch_ast_member(p->arena, e->line, e->col, e, name->text);
        } else {
            break;
        }
    }
    return e;
}

/* ---------- 一元 ---------- */
static Expr *parse_unary(Parser *p) {
    const Token *t = peek(p);
    if (t->kind == TK_MINUS || t->kind == TK_BANG || t->kind == TK_KW_NOT) {
        advance(p);
        Expr *operand = parse_unary(p);
        return ch_ast_unary(p->arena, t->line, t->col, t->kind == TK_KW_NOT ? TK_BANG : t->kind, operand);
    }
    return parse_postfix(p);
}

/* ---------- 幂（右结合） ---------- */
static Expr *parse_power(Parser *p) {
    Expr *base = parse_unary(p);
    if (check(p, TK_CARET)) {
        const Token *t = peek(p);
        advance(p);
        Expr *exp = parse_power(p); /* 右结合 */
        return ch_ast_binary(p->arena, t->line, t->col, TK_CARET, base, exp);
    }
    return base;
}

/* ---------- 乘除 ---------- */
static Expr *parse_multiplicative(Parser *p) {
    Expr *e = parse_power(p);
    while (check(p, TK_STAR) || check(p, TK_SLASH) || check(p, TK_PERCENT)) {
        TokenKind op = peek(p)->kind;
        int line = peek(p)->line, col = peek(p)->col;
        advance(p);
        Expr *rhs = parse_power(p);
        e = ch_ast_binary(p->arena, line, col, op, e, rhs);
    }
    return e;
}

/* ---------- 加减 ---------- */
static Expr *parse_additive(Parser *p) {
    Expr *e = parse_multiplicative(p);
    while (check(p, TK_PLUS) || check(p, TK_MINUS)) {
        TokenKind op = peek(p)->kind;
        int line = peek(p)->line, col = peek(p)->col;
        advance(p);
        Expr *rhs = parse_multiplicative(p);
        e = ch_ast_binary(p->arena, line, col, op, e, rhs);
    }
    return e;
}

/* ---------- 关系 ---------- */
static Expr *parse_relational(Parser *p) {
    Expr *e = parse_additive(p);
    while (check(p, TK_LT) || check(p, TK_LE) || check(p, TK_GT) || check(p, TK_GE)) {
        TokenKind op = peek(p)->kind;
        int line = peek(p)->line, col = peek(p)->col;
        advance(p);
        Expr *rhs = parse_additive(p);
        e = ch_ast_binary(p->arena, line, col, op, e, rhs);
    }
    return e;
}

/* ---------- 相等 ---------- */
static Expr *parse_equality(Parser *p) {
    Expr *e = parse_relational(p);
    while (check(p, TK_EQ) || check(p, TK_NE)) {
        TokenKind op = peek(p)->kind;
        int line = peek(p)->line, col = peek(p)->col;
        advance(p);
        Expr *rhs = parse_relational(p);
        e = ch_ast_binary(p->arena, line, col, op, e, rhs);
    }
    return e;
}

/* ---------- 逻辑与 ---------- */
static Expr *parse_and(Parser *p) {
    Expr *e = parse_equality(p);
    while (check(p, TK_ANDAND) || check(p, TK_KW_AND)) {
        int line = peek(p)->line, col = peek(p)->col;
        advance(p);
        Expr *rhs = parse_equality(p);
        e = ch_ast_binary(p->arena, line, col, TK_ANDAND, e, rhs);
    }
    return e;
}

/* ---------- 逻辑或 ---------- */
static Expr *parse_or(Parser *p) {
    Expr *e = parse_and(p);
    while (check(p, TK_OROR) || check(p, TK_KW_OR)) {
        int line = peek(p)->line, col = peek(p)->col;
        advance(p);
        Expr *rhs = parse_and(p);
        e = ch_ast_binary(p->arena, line, col, TK_OROR, e, rhs);
    }
    return e;
}

/* ---------- 赋值（右结合） ---------- */
static Expr *parse_assignment(Parser *p) {
    Expr *e = parse_or(p);
    if (check(p, TK_ASSIGN)) {
        int line = peek(p)->line, col = peek(p)->col;
        advance(p);
        Expr *val = parse_assignment(p); /* 右结合 */
        e = ch_ast_assign(p->arena, line, col, e, val);
    }
    return e;
}

static Expr *parse_expr(Parser *p) {
    return parse_assignment(p);
}

/* ---------- 语句块 ---------- */
static Stmt *parse_block(Parser *p) {
    ChVec stmts;
    ch_vec_init(&stmts, sizeof(Stmt*));
    skip_newlines(p);
    while (!check(p, TK_EOF) && !check(p, TK_KW_END) && !check(p, TK_KW_ELSE)) {
        Stmt *s = parse_stmt(p);
        if (s) {
            Stmt **slot = ch_vec_push(&stmts);
            *slot = s;
        }
        skip_newlines(p);
    }
    int sc = (int)stmts.len;
    Stmt **arr = ch_arena_alloc(p->arena, sc * sizeof(Stmt*));
    memcpy(arr, stmts.data, sc * sizeof(Stmt*));
    ch_vec_free(&stmts);
    return ch_stmt_block(p->arena, 0, 0, arr, sc);
}

/* ---------- 语句 ---------- */
static Stmt *parse_stmt(Parser *p) {
    skip_newlines(p);
    const Token *t = peek(p);
    int line = t->line, col = t->col;

    switch (t->kind) {
    case TK_KW_VAR: {
        advance(p);
        const Token *name = expect(p, TK_IDENT, "变量名");
        ChType *annot = NULL;
        if (match(p, TK_COLON)) annot = parse_type(p);
        Expr *init = NULL;
        if (match(p, TK_ASSIGN)) init = parse_expr(p);
        skip_newlines(p);
        /* 可选语句结束符 */
        match(p, TK_SEMI);
        return ch_stmt_var(p->arena, line, col, name->text, annot, init);
    }
    case TK_KW_CONST: {
        advance(p);
        const Token *name = expect(p, TK_IDENT, "常量名");
        expect(p, TK_ASSIGN, "'='");
        Expr *init = parse_expr(p);
        match(p, TK_SEMI);
        return ch_stmt_const(p->arena, line, col, name->text, init);
    }
    case TK_KW_RETURN: {
        advance(p);
        Expr *val = NULL;
        if (!check(p, TK_NEWLINE) && !check(p, TK_SEMI) && !check(p, TK_KW_END) && !check(p, TK_EOF))
            val = parse_expr(p);
        match(p, TK_SEMI);
        return ch_stmt_return(p->arena, line, col, val);
    }
    case TK_KW_IF: {
        advance(p);
        Expr *cond = parse_expr(p);
        skip_newlines(p);
        Stmt *then_b = parse_block(p);
        expect(p, TK_KW_END, "'结束'");
        Stmt *else_b = NULL;
        skip_newlines(p);          /* 允许「结束」与「否则」分行书写 */
        if (match(p, TK_KW_ELSE)) {
            skip_newlines(p);
            /* 否则 块 */
            else_b = parse_block(p);
            expect(p, TK_KW_END, "'结束'");
        }
        return ch_stmt_if(p->arena, line, col, cond, then_b, else_b);
    }
    case TK_KW_WHILE: {
        advance(p);
        Expr *cond = parse_expr(p);
        skip_newlines(p);
        Stmt *body = parse_block(p);
        expect(p, TK_KW_END, "'结束'");
        return ch_stmt_while(p->arena, line, col, cond, body);
    }
    case TK_KW_FOR: {
        advance(p);
        const Token *var = expect(p, TK_IDENT, "循环变量");
        expect(p, TK_ASSIGN, "'='");
        Expr *start = parse_expr(p);
        /* 到 */
        if (check(p, TK_IDENT) && strcmp(peek(p)->text, "到") == 0) advance(p);
        else err(p, peek(p)->line, peek(p)->col, "期望 '到'");
        Expr *end = parse_expr(p);
        skip_newlines(p);
        Stmt *body = parse_block(p);
        expect(p, TK_KW_END, "'结束'");
        return ch_stmt_for(p->arena, line, col, var->text, start, end, body);
    }
    case TK_KW_BREAK:    advance(p); match(p, TK_SEMI); return ch_stmt_break(p->arena, line, col);
    case TK_KW_CONTINUE: advance(p); match(p, TK_SEMI); return ch_stmt_continue(p->arena, line, col);
    case TK_KW_PRINT: {
        advance(p);
        Expr *e = parse_expr(p);
        match(p, TK_SEMI);
        return ch_stmt_print(p->arena, line, col, e);
    }
    case TK_KW_END:
        /* 不应在语句解析中遇到，交给上层 */
        return NULL;
    default: {
        /* 表达式语句或赋值 */
        Expr *e = parse_expr(p);
        match(p, TK_SEMI);
        /* 如果顶层是赋值，用 ST_ASSIGN；否则 ST_EXPR */
        if (e && e->kind == EX_ASSIGN)
            return ch_stmt_assign(p->arena, line, col, e->u.assign.target, e->u.assign.value);
        return ch_stmt_expr(p->arena, line, col, e);
    }
    }
}

/* ---------- 顶层声明 ---------- */
static Decl *parse_decl(Parser *p) {
    skip_newlines(p);
    const Token *t = peek(p);
    int line = t->line, col = t->col;

    if (t->kind == TK_KW_FN) {
        advance(p);
        const Token *name = expect(p, TK_IDENT, "函数名");
        expect(p, TK_LPAREN, "'('");
        ChVec params;
        ch_vec_init(&params, sizeof(Param));
        bool is_variadic = false;
        const char *variadic_name = NULL;
        skip_newlines(p);
        if (!check(p, TK_RPAREN)) {
            do {
                skip_newlines(p);
                if (check(p, TK_ELLIPSIS)) {
                    is_variadic = true;
                    advance(p);
                    /* 可选收集列表名：...其余 */
                    if (check(p, TK_IDENT)) {
                        variadic_name = peek(p)->text;
                        advance(p);
                    }
                    break;
                }
                const Token *pn = expect(p, TK_IDENT, "参数名");
                ChType *pa = NULL;
                if (match(p, TK_COLON)) pa = parse_type(p);
                Expr *def = NULL;
                if (match(p, TK_ASSIGN)) def = parse_expr(p);
                Param *slot = ch_vec_push(&params);
                slot->name = pn->text;
                slot->type_annot = pa;
                slot->default_val = def;
                skip_newlines(p);
            } while (match(p, TK_COMMA));
        }
        skip_newlines(p);
        expect(p, TK_RPAREN, "')'");
        ChType *ret = NULL;
        if (match(p, TK_ARROW)) ret = parse_type(p);
        skip_newlines(p);
        Stmt *body = parse_block(p);
        expect(p, TK_KW_END, "'结束'");
        int pc = (int)params.len;
        Param *parr = ch_arena_alloc(p->arena, (size_t)(pc ? pc : 1) * sizeof(Param));
        if (pc) memcpy(parr, params.data, (size_t)pc * sizeof(Param));
        ch_vec_free(&params);
        return ch_decl_func(p->arena, line, col, name->text, parr, pc,
                            is_variadic, variadic_name, ret, body);
    }
    if (t->kind == TK_KW_VAR) {
        advance(p);
        const Token *name = expect(p, TK_IDENT, "变量名");
        ChType *annot = NULL;
        if (match(p, TK_COLON)) annot = parse_type(p);
        Expr *init = NULL;
        if (match(p, TK_ASSIGN)) init = parse_expr(p);
        match(p, TK_SEMI);
        return ch_decl_var(p->arena, line, col, name->text, annot, init);
    }
    if (t->kind == TK_KW_CONST) {
        advance(p);
        const Token *name = expect(p, TK_IDENT, "常量名");
        expect(p, TK_ASSIGN, "'='");
        Expr *init = parse_expr(p);
        match(p, TK_SEMI);
        return ch_decl_const(p->arena, line, col, name->text, init);
    }
    /* 导入 "相对路径.汉"：由加载器展开 */
    if (t->kind == TK_KW_IMPORT) {
        advance(p);
        const Token *path = expect(p, TK_STRING, "模块路径文字");
        match(p, TK_SEMI);
        return ch_decl_import(p->arena, line, col, path->sval ? path->sval : "");
    }
    if (t->kind == TK_KW_FROM) {
        err(p, line, col, "「从」形式的选择性导入尚未实现，已跳过");
        sync_stmt(p);
        return NULL;
    }
    /* 顶层语句：赋值 / 打印 / 表达式 / 控制流等 */
    {
        Stmt *s = parse_stmt(p);
        if (s) return ch_decl_stmt(p->arena, line, col, s);
    }
    err(p, line, col, "顶层期望声明，实际为 %s", ch_token_kind_name(t->kind));
    sync_stmt(p);
    return NULL;
}

AstNode **ch_parse(ChArena *arena, ChDiagList *diags,
                   const Token *tokens, size_t ntokens, int *out_count) {
    Parser p = { arena, diags, tokens, 0, ntokens };
    ChVec decls;
    ch_vec_init(&decls, sizeof(AstNode*));
    skip_newlines(&p);
    while (!check(&p, TK_EOF)) {
        size_t before = p.pos;
        Decl *d = parse_decl(&p);
        if (d) {
            AstNode **slot = ch_vec_push(&decls);
            *slot = ch_node_decl(arena, d);
        }
        /* 保证错误恢复一定前进，避免顶层出现孤立「结束」时死循环 */
        if (!d && p.pos == before) advance(&p);
        skip_newlines(&p);
    }
    int count = (int)decls.len;
    AstNode **arr = ch_arena_alloc(arena, count * sizeof(AstNode*));
    memcpy(arr, decls.data, count * sizeof(AstNode*));
    ch_vec_free(&decls);
    *out_count = count;
    return arr;
}
