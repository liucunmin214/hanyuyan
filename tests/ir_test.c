#include "util/common.h"
#include "util/arena.h"
#include "util/diag.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "sema/sema.h"
#include "ir/ir.h"
#include <stdio.h>
#include <string.h>

static int passed = 0, failed = 0;

static IrModule *gen(ChArena *a, const char *src) {
    ChDiagList d; ch_diag_init(&d, a);
    ChVec toks; ch_vec_init(&toks, sizeof(Token));
    ch_lex(a, &d, src, strlen(src), &toks);
    int n = 0;
    AstNode **decls = ch_parse(a, &d, toks.data, toks.len, &n);
    ch_sema_check(a, &d, decls, n);
    return ch_ir_gen(a, decls, n);
}

#define CHECK(cond, msg) do { \
    if (cond) { passed++; } \
    else { failed++; printf("失败: %s\n", msg); } \
} while (0)

static bool has_op(IrFunc *fn, IrOp op) {
    IrInstr *is = fn->instrs.data;
    for (size_t i = 0; i < fn->instrs.len; i++)
        if (is[i].op == op) return true;
    return false;
}

static int count_op(IrFunc *fn, IrOp op) {
    int c = 0;
    IrInstr *is = fn->instrs.data;
    for (size_t i = 0; i < fn->instrs.len; i++)
        if (is[i].op == op) c++;
    return c;
}

static void t_func_gen(void) {
    ChArena *a = ch_arena_new();
    IrModule *m = gen(a, "函数 f() -> 整数\n  返回 42\n结束\n");
    CHECK(m->nfuncs == 1, "生成1个函数");
    CHECK(strcmp(m->funcs[0]->name, "f") == 0, "函数名f");
    CHECK(has_op(m->funcs[0], IR_RET), "有返回指令");
    ch_arena_destroy(a);
}

static void t_const_int(void) {
    ChArena *a = ch_arena_new();
    IrModule *m = gen(a, "函数 f() -> 整数\n  返回 42\n结束\n");
    IrFunc *fn = m->funcs[0];
    bool found = false;
    IrInstr *is = fn->instrs.data;
    for (size_t i = 0; i < fn->instrs.len; i++)
        if (is[i].op == IR_CONST && is[i].ival == 42) found = true;
    CHECK(found, "生成整数常量42");
    ch_arena_destroy(a);
}

static void t_binary_add(void) {
    ChArena *a = ch_arena_new();
    IrModule *m = gen(a, "函数 f() -> 整数\n  返回 1 + 2\n结束\n");
    CHECK(has_op(m->funcs[0], IR_ADD), "生成加法指令");
    ch_arena_destroy(a);
}

static void t_if_branch(void) {
    ChArena *a = ch_arena_new();
    IrModule *m = gen(a,
        "函数 f()\n  如果 真\n    打印 1\n  结束\n结束\n");
    CHECK(has_op(m->funcs[0], IR_BRANCH), "生成条件跳转");
    CHECK(has_op(m->funcs[0], IR_LABEL), "生成标签");
    ch_arena_destroy(a);
}

static void t_while_loop(void) {
    ChArena *a = ch_arena_new();
    IrModule *m = gen(a,
        "函数 f()\n  当 假\n    打印 1\n  结束\n结束\n");
    CHECK(has_op(m->funcs[0], IR_JUMP), "while生成无条件跳转");
    CHECK(has_op(m->funcs[0], IR_BRANCH), "while生成条件跳转");
    ch_arena_destroy(a);
}

static void t_call(void) {
    ChArena *a = ch_arena_new();
    IrModule *m = gen(a,
        "函数 g() -> 整数\n  返回 1\n结束\n"
        "函数 f() -> 整数\n  返回 g()\n结束\n");
    CHECK(has_op(m->funcs[1], IR_CALL), "生成调用指令");
    ch_arena_destroy(a);
}

static void t_print(void) {
    ChArena *a = ch_arena_new();
    IrModule *m = gen(a, "函数 f()\n  打印 123\n结束\n");
    CHECK(has_op(m->funcs[0], IR_PRINT), "生成打印指令");
    ch_arena_destroy(a);
}

static void t_var_store_load(void) {
    ChArena *a = ch_arena_new();
    IrModule *m = gen(a,
        "函数 f() -> 整数\n  变量 x = 5\n  返回 x\n结束\n");
    CHECK(has_op(m->funcs[0], IR_STORE), "生成存储指令");
    CHECK(has_op(m->funcs[0], IR_LOAD), "生成加载指令");
    ch_arena_destroy(a);
}

static void t_const_fold(void) {
    ChArena *a = ch_arena_new();
    IrModule *m = gen(a, "函数 f() -> 整数\n  返回 1 + 2\n结束\n");
    ch_ir_optimize(a, m);
    /* 优化后 1+2 应被折叠为常量 3，不再有 ADD 指令 */
    CHECK(count_op(m->funcs[0], IR_ADD) == 0, "常量折叠消除加法");
    /* 检查是否有常量 3 */
    IrInstr *is = m->funcs[0]->instrs.data;
    bool has3 = false;
    for (size_t i = 0; i < m->funcs[0]->instrs.len; i++)
        if (is[i].op == IR_CONST && is[i].ival == 3) has3 = true;
    CHECK(has3, "折叠后产生常量3");
    ch_arena_destroy(a);
}

static void t_for_loop_gen(void) {
    ChArena *a = ch_arena_new();
    IrModule *m = gen(a,
        "函数 f()\n  对于 i = 1 到 3\n    打印 i\n  结束\n结束\n");
    CHECK(has_op(m->funcs[0], IR_JUMP), "for生成回跳");
    CHECK(has_op(m->funcs[0], IR_LE), "for生成比较");
    ch_arena_destroy(a);
}

int main(void) {
    t_func_gen();
    t_const_int();
    t_binary_add();
    t_if_branch();
    t_while_loop();
    t_call();
    t_print();
    t_var_store_load();
    t_const_fold();
    t_for_loop_gen();
    printf("IR测试：通过 %d 例，失败 %d 例\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
