#include "util/common.h"
#include "util/arena.h"
#include "util/diag.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "sema/sema.h"
#include <stdio.h>
#include <string.h>

static int passed = 0, failed = 0;

static bool check_str(ChArena *a, const char *src) {
    ChDiagList d; ch_diag_init(&d, a);
    ChVec toks; ch_vec_init(&toks, sizeof(Token));
    ch_lex(a, &d, src, strlen(src), &toks);
    int n = 0;
    AstNode **decls = ch_parse(a, &d, toks.data, toks.len, &n);
    return ch_sema_check(a, &d, decls, n);
}

static int error_count(ChArena *a, const char *src) {
    ChDiagList d; ch_diag_init(&d, a);
    ChVec toks; ch_vec_init(&toks, sizeof(Token));
    ch_lex(a, &d, src, strlen(src), &toks);
    int n = 0;
    AstNode **decls = ch_parse(a, &d, toks.data, toks.len, &n);
    ch_sema_check(a, &d, decls, n);
    return ch_diag_error_count(&d);
}

#define CHECK(cond, msg) do { \
    if (cond) { passed++; } \
    else { failed++; printf("失败: %s\n", msg); } \
} while (0)

static void t_type_inference_int(void) {
    ChArena *a = ch_arena_new();
    CHECK(check_str(a, "变量 x = 42\n"), "整数推断无错误");
    ch_arena_destroy(a);
}

static void t_type_inference_float(void) {
    ChArena *a = ch_arena_new();
    CHECK(check_str(a, "变量 x = 3.14\n"), "小数推断无错误");
    ch_arena_destroy(a);
}

static void t_type_inference_string(void) {
    ChArena *a = ch_arena_new();
    CHECK(check_str(a, "变量 x = \"hi\"\n"), "文字推断无错误");
    ch_arena_destroy(a);
}

static void t_undefined_var(void) {
    ChArena *a = ch_arena_new();
    int errs = error_count(a, "函数 f()\n  打印 y\n结束\n");
    CHECK(errs > 0, "未定义变量应报错");
    ch_arena_destroy(a);
}

static void t_duplicate_def(void) {
    ChArena *a = ch_arena_new();
    int errs = error_count(a, "变量 x = 1\n变量 x = 2\n");
    CHECK(errs > 0, "重复定义应报错");
    ch_arena_destroy(a);
}

static void t_func_call_ok(void) {
    ChArena *a = ch_arena_new();
    CHECK(check_str(a,
        "函数 加(甲: 整数, 乙: 整数) -> 整数\n  返回 甲 + 乙\n结束\n"
        "变量 r = 加(1, 2)\n"), "合法函数调用无错误");
    ch_arena_destroy(a);
}

static void t_func_call_arg_count(void) {
    ChArena *a = ch_arena_new();
    int errs = error_count(a,
        "函数 加(甲: 整数, 乙: 整数) -> 整数\n  返回 甲 + 乙\n结束\n"
        "变量 r = 加(1)\n");
    CHECK(errs > 0, "参数数量不匹配应报错");
    ch_arena_destroy(a);
}

static void t_return_type_mismatch(void) {
    ChArena *a = ch_arena_new();
    int errs = error_count(a,
        "函数 f() -> 整数\n  返回 \"文字\"\n结束\n");
    CHECK(errs > 0, "返回类型不匹配应报错");
    ch_arena_destroy(a);
}

static void t_return_type_ok(void) {
    ChArena *a = ch_arena_new();
    CHECK(check_str(a,
        "函数 f() -> 整数\n  返回 42\n结束\n"), "正确返回类型无错误");
    ch_arena_destroy(a);
}

static void t_break_outside_loop(void) {
    ChArena *a = ch_arena_new();
    int errs = error_count(a, "函数 f()\n  跳出\n结束\n");
    CHECK(errs > 0, "循环外跳出应报错");
    ch_arena_destroy(a);
}

static void t_break_inside_loop(void) {
    ChArena *a = ch_arena_new();
    CHECK(check_str(a,
        "函数 f()\n  当 真\n    跳出\n  结束\n结束\n"), "循环内跳出无错误");
    ch_arena_destroy(a);
}

static void t_arith_int(void) {
    ChArena *a = ch_arena_new();
    CHECK(check_str(a, "变量 x = 1 + 2 * 3\n"), "整数算术无错误");
    ch_arena_destroy(a);
}

static void t_string_concat(void) {
    ChArena *a = ch_arena_new();
    CHECK(check_str(a, "变量 x = \"a\" + \"b\"\n"), "文字拼接无错误");
    ch_arena_destroy(a);
}

static void t_comparison(void) {
    ChArena *a = ch_arena_new();
    CHECK(check_str(a, "变量 x = 1 < 2\n变量 y = 3 == 3\n"), "比较运算无错误");
    ch_arena_destroy(a);
}

static void t_logical_ops(void) {
    ChArena *a = ch_arena_new();
    CHECK(check_str(a, "变量 x = 真 && 假 || 真\n"), "逻辑运算无错误");
    ch_arena_destroy(a);
}

static void t_annot_mismatch(void) {
    ChArena *a = ch_arena_new();
    int errs = error_count(a, "变量 x: 整数 = \"文字\"\n");
    CHECK(errs > 0, "类型标注不匹配应报错");
    ch_arena_destroy(a);
}

static void t_list_index(void) {
    ChArena *a = ch_arena_new();
    CHECK(check_str(a,
        "变量 arr = [1, 2, 3]\n变量 x = arr[0]\n"), "列表索引无错误");
    ch_arena_destroy(a);
}

static void t_forward_ref(void) {
    ChArena *a = ch_arena_new();
    CHECK(check_str(a,
        "变量 r = 加(1, 2)\n"
        "函数 加(甲: 整数, 乙: 整数) -> 整数\n  返回 甲 + 乙\n结束\n"),
        "函数前向引用无错误");
    ch_arena_destroy(a);
}

static void t_recursion(void) {
    ChArena *a = ch_arena_new();
    CHECK(check_str(a,
        "函数 阶乘(甲: 整数) -> 整数\n"
        "  如果 甲 <= 1\n    返回 1\n  结束\n"
        "  返回 甲 * 阶乘(甲 - 1)\n"
        "结束\n"), "递归函数无错误");
    ch_arena_destroy(a);
}

static void t_for_loop(void) {
    ChArena *a = ch_arena_new();
    CHECK(check_str(a,
        "函数 f()\n  对于 i = 1 到 10\n    打印 i\n  结束\n结束\n"), "对于循环无错误");
    ch_arena_destroy(a);
}

int main(void) {
    t_type_inference_int();
    t_type_inference_float();
    t_type_inference_string();
    t_undefined_var();
    t_duplicate_def();
    t_func_call_ok();
    t_func_call_arg_count();
    t_return_type_mismatch();
    t_return_type_ok();
    t_break_outside_loop();
    t_break_inside_loop();
    t_arith_int();
    t_string_concat();
    t_comparison();
    t_logical_ops();
    t_annot_mismatch();
    t_list_index();
    t_forward_ref();
    t_recursion();
    t_for_loop();
    printf("语义测试：通过 %d 例，失败 %d 例\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
