#include "util/common.h"
#include "util/arena.h"
#include "util/diag.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include <stdio.h>
#include <string.h>

static int passed = 0, failed = 0;

static AstNode **parse_str(ChArena *a, ChDiagList *d, const char *src, int *count) {
    ChVec toks;
    ch_vec_init(&toks, sizeof(Token));
    ch_lex(a, d, src, strlen(src), &toks);
    return ch_parse(a, d, toks.data, toks.len, count);
}

#define CHECK(cond, msg) do { \
    if (cond) { passed++; } \
    else { failed++; printf("失败: %s\n", msg); } \
} while (0)

static void t_function_decl(void) {
    ChArena *a = ch_arena_new();
    ChDiagList d; ch_diag_init(&d, a);
    int n = 0;
    AstNode **decls = parse_str(a, &d, "函数 加(甲: 整数, 乙: 整数) -> 整数\n  返回 甲 + 乙\n结束\n", &n);
    CHECK(n == 1, "函数声明数量");
    CHECK(decls[0]->as.decl->kind == DECL_FUNC, "声明类型为函数");
    CHECK(strcmp(decls[0]->as.decl->name, "加") == 0, "函数名为加");
    CHECK(decls[0]->as.decl->u.fn.nparams == 2, "参数数量为2");
    CHECK(decls[0]->as.decl->u.fn.ret_annot->kind == TY_INT, "返回类型整数");
    ch_arena_destroy(a);
}

static void t_var_decl(void) {
    ChArena *a = ch_arena_new();
    ChDiagList d; ch_diag_init(&d, a);
    int n = 0;
    AstNode **decls = parse_str(a, &d, "变量 x = 42\n", &n);
    CHECK(n == 1, "变量声明数量");
    CHECK(decls[0]->as.decl->kind == DECL_VAR, "声明类型为变量");
    CHECK(strcmp(decls[0]->as.decl->name, "x") == 0, "变量名为x");
    CHECK(decls[0]->as.decl->u.var.init->kind == EX_INT, "初值为整数");
    CHECK(decls[0]->as.decl->u.var.init->u.ival == 42, "初值为42");
    ch_arena_destroy(a);
}

static void t_const_decl(void) {
    ChArena *a = ch_arena_new();
    ChDiagList d; ch_diag_init(&d, a);
    int n = 0;
    AstNode **decls = parse_str(a, &d, "常量 PI = 3.14\n", &n);
    CHECK(n == 1, "常量声明数量");
    CHECK(decls[0]->as.decl->kind == DECL_CONST, "声明类型为常量");
    CHECK(decls[0]->as.decl->u.konst.init->kind == EX_FLOAT, "初值为小数");
    ch_arena_destroy(a);
}

static void t_if_stmt(void) {
    ChArena *a = ch_arena_new();
    ChDiagList d; ch_diag_init(&d, a);
    int n = 0;
    AstNode **decls = parse_str(a, &d,
        "函数 主()\n  如果 x > 0\n    打印 1\n  否则\n    打印 2\n  结束\n结束\n", &n);
    Decl *fn = decls[0]->as.decl;
    Stmt *blk = fn->u.fn.body;
    CHECK(blk->kind == ST_BLOCK, "函数体为块");
    Stmt *s = blk->u.block.stmts[0];
    CHECK(s->kind == ST_IF, "第一条语句为如果");
    CHECK(s->u.iff.else_b != NULL, "有否则分支");
    ch_arena_destroy(a);
}

static void t_while_stmt(void) {
    ChArena *a = ch_arena_new();
    ChDiagList d; ch_diag_init(&d, a);
    int n = 0;
    AstNode **decls = parse_str(a, &d,
        "函数 主()\n  当 i < 10\n    i = i + 1\n  结束\n结束\n", &n);
    Stmt *s = decls[0]->as.decl->u.fn.body->u.block.stmts[0];
    CHECK(s->kind == ST_WHILE, "语句为当");
    ch_arena_destroy(a);
}

static void t_for_stmt(void) {
    ChArena *a = ch_arena_new();
    ChDiagList d; ch_diag_init(&d, a);
    int n = 0;
    AstNode **decls = parse_str(a, &d,
        "函数 主()\n  对于 i = 1 到 10\n    打印 i\n  结束\n结束\n", &n);
    Stmt *s = decls[0]->as.decl->u.fn.body->u.block.stmts[0];
    CHECK(s->kind == ST_FOR, "语句为对于");
    CHECK(strcmp(s->u.forn.var, "i") == 0, "循环变量为i");
    ch_arena_destroy(a);
}

static void t_binary_ops(void) {
    ChArena *a = ch_arena_new();
    ChDiagList d; ch_diag_init(&d, a);
    int n = 0;
    AstNode **decls = parse_str(a, &d, "变量 x = 1 + 2 * 3 - 4 / 2\n", &n);
    Expr *e = decls[0]->as.decl->u.var.init;
    /* 1 + (2*3) - (4/2) => 减法在最外层 */
    CHECK(e->kind == EX_BINARY, "最外层为二元");
    CHECK(e->u.binary.op == TK_MINUS, "最外层运算符为减");
    ch_arena_destroy(a);
}

static void t_power_right_assoc(void) {
    ChArena *a = ch_arena_new();
    ChDiagList d; ch_diag_init(&d, a);
    int n = 0;
    AstNode **decls = parse_str(a, &d, "变量 x = 2 ^ 3 ^ 2\n", &n);
    Expr *e = decls[0]->as.decl->u.var.init;
    /* 右结合：2 ^ (3 ^ 2) */
    CHECK(e->kind == EX_BINARY && e->u.binary.op == TK_CARET, "最外层为幂");
    CHECK(e->u.binary.rhs->kind == EX_BINARY, "右操作数仍为幂（右结合）");
    ch_arena_destroy(a);
}

static void t_unary(void) {
    ChArena *a = ch_arena_new();
    ChDiagList d; ch_diag_init(&d, a);
    int n = 0;
    AstNode **decls = parse_str(a, &d, "变量 x = -5\n变量 y = !真\n", &n);
    CHECK(decls[0]->as.decl->u.var.init->kind == EX_UNARY, "负号为一元");
    CHECK(decls[1]->as.decl->u.var.init->kind == EX_UNARY, "非为一元");
    ch_arena_destroy(a);
}

static void t_call(void) {
    ChArena *a = ch_arena_new();
    ChDiagList d; ch_diag_init(&d, a);
    int n = 0;
    AstNode **decls = parse_str(a, &d, "变量 x = 加(1, 2)\n", &n);
    Expr *e = decls[0]->as.decl->u.var.init;
    CHECK(e->kind == EX_CALL, "表达式为调用");
    CHECK(e->u.call.nargs == 2, "参数数量为2");
    ch_arena_destroy(a);
}

static void t_list_literal(void) {
    ChArena *a = ch_arena_new();
    ChDiagList d; ch_diag_init(&d, a);
    int n = 0;
    AstNode **decls = parse_str(a, &d, "变量 x = [1, 2, 3]\n", &n);
    Expr *e = decls[0]->as.decl->u.var.init;
    CHECK(e->kind == EX_LIST, "表达式为列表");
    CHECK(e->u.list.count == 3, "列表长度为3");
    ch_arena_destroy(a);
}

static void t_index(void) {
    ChArena *a = ch_arena_new();
    ChDiagList d; ch_diag_init(&d, a);
    int n = 0;
    AstNode **decls = parse_str(a, &d, "变量 x = arr[0]\n", &n);
    Expr *e = decls[0]->as.decl->u.var.init;
    CHECK(e->kind == EX_INDEX, "表达式为索引");
    ch_arena_destroy(a);
}

static void t_string(void) {
    ChArena *a = ch_arena_new();
    ChDiagList d; ch_diag_init(&d, a);
    int n = 0;
    AstNode **decls = parse_str(a, &d, "变量 x = \"你好\"\n", &n);
    Expr *e = decls[0]->as.decl->u.var.init;
    CHECK(e->kind == EX_STRING, "表达式为文字");
    CHECK(strcmp(e->u.sval, "你好") == 0, "文字内容为你好");
    ch_arena_destroy(a);
}

static void t_return(void) {
    ChArena *a = ch_arena_new();
    ChDiagList d; ch_diag_init(&d, a);
    int n = 0;
    AstNode **decls = parse_str(a, &d,
        "函数 f() -> 整数\n  返回 42\n结束\n", &n);
    Stmt *s = decls[0]->as.decl->u.fn.body->u.block.stmts[0];
    CHECK(s->kind == ST_RETURN, "语句为返回");
    CHECK(s->u.ret.value->kind == EX_INT, "返回值为整数");
    ch_arena_destroy(a);
}

static void t_break_continue(void) {
    ChArena *a = ch_arena_new();
    ChDiagList d; ch_diag_init(&d, a);
    int n = 0;
    AstNode **decls = parse_str(a, &d,
        "函数 f()\n  当 真\n    跳出\n    继续\n  结束\n结束\n", &n);
    Stmt *blk = decls[0]->as.decl->u.fn.body;
    Stmt *wh = blk->u.block.stmts[0];
    Stmt *body = wh->u.whil.body;
    CHECK(body->u.block.stmts[0]->kind == ST_BREAK, "跳出");
    CHECK(body->u.block.stmts[1]->kind == ST_CONTINUE, "继续");
    ch_arena_destroy(a);
}

static void t_print(void) {
    ChArena *a = ch_arena_new();
    ChDiagList d; ch_diag_init(&d, a);
    int n = 0;
    AstNode **decls = parse_str(a, &d,
        "函数 f()\n  打印 123\n结束\n", &n);
    Stmt *s = decls[0]->as.decl->u.fn.body->u.block.stmts[0];
    CHECK(s->kind == ST_PRINT, "语句为打印");
    ch_arena_destroy(a);
}

static void t_var_with_type(void) {
    ChArena *a = ch_arena_new();
    ChDiagList d; ch_diag_init(&d, a);
    int n = 0;
    AstNode **decls = parse_str(a, &d, "变量 x: 整数 = 1\n", &n);
    CHECK(decls[0]->as.decl->u.var.annot != NULL, "有类型标注");
    CHECK(decls[0]->as.decl->u.var.annot->kind == TY_INT, "类型为整数");
    ch_arena_destroy(a);
}

static void t_variadic(void) {
    ChArena *a = ch_arena_new();
    ChDiagList d; ch_diag_init(&d, a);
    int n = 0;
    AstNode **decls = parse_str(a, &d,
        "函数 求和(...) -> 整数\n  返回 0\n结束\n", &n);
    CHECK(decls[0]->as.decl->u.fn.is_variadic, "函数为可变参数");
    ch_arena_destroy(a);
}

static void t_nested_if(void) {
    ChArena *a = ch_arena_new();
    ChDiagList d; ch_diag_init(&d, a);
    int n = 0;
    AstNode **decls = parse_str(a, &d,
        "函数 f()\n  如果 a\n    如果 b\n      打印 1\n    结束\n  结束\n结束\n", &n);
    Stmt *outer = decls[0]->as.decl->u.fn.body->u.block.stmts[0];
    CHECK(outer->kind == ST_IF, "外层如果");
    Stmt *inner = outer->u.iff.then_b->u.block.stmts[0];
    CHECK(inner->kind == ST_IF, "内层如果");
    ch_arena_destroy(a);
}

static void t_error_recovery(void) {
    ChArena *a = ch_arena_new();
    ChDiagList d; ch_diag_init(&d, a);
    int n = 0;
    /* 第一行有语法错误，第二行应能恢复解析 */
    AstNode **decls = parse_str(a, &d,
        "函数 f()\n  = = =\n  打印 1\n结束\n", &n);
    (void)decls;
    CHECK(ch_diag_error_count(&d) > 0, "有语法错误");
    CHECK(n == 1, "错误恢复后仍得到1个声明");
    ch_arena_destroy(a);
}

static void t_fullwidth_punct(void) {
    ChArena *a = ch_arena_new();
    ChDiagList d; ch_diag_init(&d, a);
    int n = 0;
    /* 中文逗号、句号 */
    AstNode **decls = parse_str(a, &d,
        "函数 加（甲：整数，乙：整数）-> 整数\n  返回 甲 + 乙。\n结束\n", &n);
    CHECK(n == 1, "全角标点解析成功");
    CHECK(decls[0]->as.decl->u.fn.nparams == 2, "参数数量2");
    ch_arena_destroy(a);
}

static void t_assignment_stmt(void) {
    ChArena *a = ch_arena_new();
    ChDiagList d; ch_diag_init(&d, a);
    int n = 0;
    AstNode **decls = parse_str(a, &d,
        "函数 f()\n  x = 10\n结束\n", &n);
    Stmt *s = decls[0]->as.decl->u.fn.body->u.block.stmts[0];
    CHECK(s->kind == ST_ASSIGN, "赋值语句");
    ch_arena_destroy(a);
}

int main(void) {
    t_function_decl();
    t_var_decl();
    t_const_decl();
    t_if_stmt();
    t_while_stmt();
    t_for_stmt();
    t_binary_ops();
    t_power_right_assoc();
    t_unary();
    t_call();
    t_list_literal();
    t_index();
    t_string();
    t_return();
    t_break_continue();
    t_print();
    t_var_with_type();
    t_variadic();
    t_nested_if();
    t_error_recovery();
    t_fullwidth_punct();
    t_assignment_stmt();
    printf("解析器测试：通过 %d 例，失败 %d 例\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
