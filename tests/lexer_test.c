#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "util/common.h"
#include "util/arena.h"
#include "util/diag.h"
#include "util/source.h"
#include "lexer/lexer.h"
#include "lexer/token.h"

#include <math.h>

static ChArena *g_arena;
static int g_pass = 0;
static int g_fail = 0;

typedef struct {
    ChVec      toks;
    ChDiagList diags;
} Lexed;

static Lexed lex_raw(const char *raw) {
    Lexed L;
    ch_diag_init(&L.diags, g_arena);
    size_t len = 0;
    char *src = ch_source_normalize(raw, strlen(raw), &len, g_arena);
    ch_vec_init(&L.toks, sizeof(Token));
    ch_lex(g_arena, &L.diags, src, len, &L.toks);
    return L;
}

static void dump_actual(const Lexed *L) {
    printf("  实际记号：");
    for (size_t i = 0; i < L->toks.len; i++) {
        const Token *t = &CH_VEC_AT(L->toks, Token, i);
        printf("[%s", ch_token_kind_name(t->kind));
        if (t->sval) printf(" \"%s\"", t->sval);
        else if (t->text) printf(" %s", t->text);
        printf("]");
    }
    printf("\n");
}

static void kind_case(const char *name, const char *raw, const char *const *want,
                      size_t nwant, int want_errors) {
    Lexed L = lex_raw(raw);
    int errors = ch_diag_error_count(&L.diags);
    size_t bad = (size_t)-1;
    bool ok = (errors == want_errors) && (L.toks.len == nwant);
    if (ok) {
        for (size_t i = 0; i < nwant; i++) {
            const Token *t = &CH_VEC_AT(L.toks, Token, i);
            if (strcmp(ch_token_kind_name(t->kind), want[i]) != 0) {
                ok = false;
                bad = i;
                break;
            }
        }
    }
    if (ok) {
        g_pass++;
        return;
    }
    g_fail++;
    printf("失败：%s\n", name);
    printf("  错误数：期望 %d，实际 %d；记号数：期望 %zu，实际 %zu\n",
           want_errors, errors, nwant, L.toks.len);
    if (bad != (size_t)-1) {
        const Token *t = &CH_VEC_AT(L.toks, Token, bad);
        printf("  第 %zu 个记号：期望 %s，实际 %s\n", bad, want[bad],
               ch_token_kind_name(t->kind));
    }
    dump_actual(&L);
}

static void int_case(const char *name, const char *raw, size_t idx, long long want) {
    Lexed L = lex_raw(raw);
    if (idx < L.toks.len) {
        const Token *t = &CH_VEC_AT(L.toks, Token, idx);
        if (t->kind == TK_INT && t->ival == want) {
            g_pass++;
            return;
        }
    }
    g_fail++;
    printf("失败：%s（期望整数 %lld）\n", name, want);
    dump_actual(&L);
}

static void float_case(const char *name, const char *raw, size_t idx, double want) {
    Lexed L = lex_raw(raw);
    if (idx < L.toks.len) {
        const Token *t = &CH_VEC_AT(L.toks, Token, idx);
        if (t->kind == TK_FLOAT && fabs(t->fval - want) < 1e-9) {
            g_pass++;
            return;
        }
    }
    g_fail++;
    printf("失败：%s（期望小数 %g）\n", name, want);
    dump_actual(&L);
}

static void text_case(const char *name, const char *raw, size_t idx, const char *want) {
    Lexed L = lex_raw(raw);
    const char *got = NULL;
    if (idx < L.toks.len) {
        const Token *t = &CH_VEC_AT(L.toks, Token, idx);
        got = t->sval ? t->sval : t->text;
    }
    if (got && strcmp(got, want) == 0) {
        g_pass++;
        return;
    }
    g_fail++;
    printf("失败：%s（期望文字 \"%s\"，实际 \"%s\"）\n", name, want, got ? got : "(空)");
    dump_actual(&L);
}

static void pos_case(const char *name, const char *raw, size_t idx, int line, int col) {
    Lexed L = lex_raw(raw);
    if (idx < L.toks.len) {
        const Token *t = &CH_VEC_AT(L.toks, Token, idx);
        if (t->line == line && t->col == col) {
            g_pass++;
            return;
        }
    }
    g_fail++;
    printf("失败：%s（期望 %d:%d）\n", name, line, col);
    dump_actual(&L);
}

int main(void) {
    SetConsoleOutputCP(65001); /* 失败信息中的中文可读 */
    g_arena = ch_arena_new();

    /* 基础 */
    kind_case("空源码", "", (const char *const[]){"文件结尾"}, 1, 0);
    kind_case("仅换行", "\n\n", (const char *const[]){"换行", "换行", "文件结尾"}, 3, 0);
    kind_case("变量声明", "变量 计数 = 0\n",
              (const char *const[]){"关键字.变量", "标识符", "运算符.=", "整数", "换行", "文件结尾"}, 6, 0);

    /* 关键字同义词 */
    kind_case("输出同义词", "输出 \"hi\"\n",
              (const char *const[]){"关键字.打印", "文字", "换行", "文件结尾"}, 4, 0);
    kind_case("显示同义词 + 全角括号句号", "显示（1）。",
              (const char *const[]){"关键字.打印", "标点.(", "整数", "标点.)", "标点.;", "文件结尾"}, 6, 0);
    kind_case("条件同义词", "若 真\n 完了",
              (const char *const[]){"关键字.如果", "关键字.真", "换行", "关键字.结束", "文件结尾"}, 5, 0);
    kind_case("循环同义词", "循环 1 只要 0 结束",
              (const char *const[]){"关键字.当", "整数", "关键字.当", "整数", "关键字.结束", "文件结尾"}, 6, 0);
    kind_case("遍历同义词", "遍历 甲 对于 乙",
              (const char *const[]){"关键字.对于", "标识符", "关键字.对于", "标识符", "文件结尾"}, 5, 0);
    kind_case("跳出与继续同义词", "跳出 停止 继续 跳过",
              (const char *const[]){"关键字.跳出", "关键字.跳出", "关键字.继续", "关键字.继续", "文件结尾"}, 5, 0);
    kind_case("导入与从", "导入 模块 从 文件",
              (const char *const[]){"关键字.导入", "标识符", "关键字.从", "标识符", "文件结尾"}, 5, 0);
    kind_case("空值同义词", "空 无",
              (const char *const[]){"关键字.空", "关键字.空", "文件结尾"}, 3, 0);
    kind_case("逻辑关键字", "真 与 假 或 非 真",
              (const char *const[]){"关键字.真", "关键字.与", "关键字.假", "关键字.或",
                                    "关键字.非", "关键字.真", "文件结尾"}, 7, 0);
    kind_case("真同义词", "是 否",
              (const char *const[]){"关键字.真", "关键字.假", "文件结尾"}, 3, 0);

    /* 字面量 */
    kind_case("十六进制", "0xFF 0X1F",
              (const char *const[]){"整数", "整数", "文件结尾"}, 3, 0);
    kind_case("二与八进制", "0b1010 0o17 0B111 0O10",
              (const char *const[]){"整数", "整数", "整数", "整数", "文件结尾"}, 5, 0);
    kind_case("下划线数字", "1_000_000",
              (const char *const[]){"整数", "文件结尾"}, 2, 0);
    kind_case("浮点", "3.14 1e-9 2.5E+3",
              (const char *const[]){"小数", "小数", "小数", "文件结尾"}, 4, 0);
    kind_case("字符串转义", "\"a\\nb\\t\\\\\\\"c\"",
              (const char *const[]){"文字", "文件结尾"}, 2, 0);

    /* 全角归一 */
    kind_case("中文引号", "显示“你好，世界”。",
              (const char *const[]){"关键字.打印", "文字", "标点.;", "文件结尾"}, 4, 0);
    kind_case("全角字母数字", "变量 ＡＢＣ = １２３",
              (const char *const[]){"关键字.变量", "标识符", "运算符.=", "整数", "文件结尾"}, 5, 0);
    kind_case("全角方括号", "列表【0】",
              (const char *const[]){"标识符", "标点.[", "整数", "标点.]", "文件结尾"}, 5, 0);
    kind_case("书名号", "《甲》",
              (const char *const[]){"运算符.<", "标识符", "运算符.>", "文件结尾"}, 4, 0);
    kind_case("顿号作逗号", "甲、乙",
              (const char *const[]){"标识符", "标点.,", "标识符", "文件结尾"}, 4, 0);
    kind_case("全角空格分隔", "变量\xE3\x80\x80x",
              (const char *const[]){"关键字.变量", "标识符", "文件结尾"}, 3, 0);

    /* 标识符与运算符 */
    kind_case("混合标识符", "x_值2 = 1",
              (const char *const[]){"标识符", "运算符.=", "整数", "文件结尾"}, 4, 0);
    kind_case("多字符运算符",
              "a == b != c <= d >= 1 && 2 || 3 -> 4 => 5 ...",
              (const char *const[]){"标识符", "运算符.==", "标识符", "运算符.!=",
                                    "标识符", "运算符.<=", "标识符", "运算符.>=",
                                    "整数", "运算符.&&", "整数", "运算符.||", "整数",
                                    "标点.->", "整数", "标点.=>", "整数",
                                    "标点....", "文件结尾"}, 19, 0);

    /* 注释与换行 */
    kind_case("行注释", "# 注释\n变量 x = 1",
              (const char *const[]){"换行", "关键字.变量", "标识符", "运算符.=", "整数", "文件结尾"}, 6, 0);
    kind_case("块注释嵌套", "/* 外 /* 内 */ 外 */变量 x",
              (const char *const[]){"关键字.变量", "标识符", "文件结尾"}, 3, 0);
    kind_case("CRLF 归一", "变量 值\r\n= 1",
              (const char *const[]){"关键字.变量", "标识符", "换行", "运算符.=", "整数", "文件结尾"}, 6, 0);
    kind_case("BOM 兼容", "\xEF\xBB\xBF变量 x",
              (const char *const[]){"关键字.变量", "标识符", "文件结尾"}, 3, 0);

    /* 错误 */
    kind_case("非法字符", "甲 @ 乙",
              (const char *const[]){"标识符", "标识符", "文件结尾"}, 3, 1);
    kind_case("未闭合字符串", "\"abc",
              (const char *const[]){"文字", "文件结尾"}, 2, 1);
    kind_case("未闭合块注释", "/* abc",
              (const char *const[]){"文件结尾"}, 1, 1);
    kind_case("数字后紧跟标识符", "123abc",
              (const char *const[]){"整数", "标识符", "文件结尾"}, 3, 1);
    kind_case("整数溢出", "99999999999999999999999",
              (const char *const[]){"整数", "文件结尾"}, 2, 1);
    kind_case("未知转义", "\"a\\q\"",
              (const char *const[]){"文字", "文件结尾"}, 2, 1);

    /* 值 */
    int_case("十六进制值", "0xFF", 0, 255);
    int_case("二进制值", "0b1010", 0, 10);
    int_case("八进制值", "0o17", 0, 15);
    int_case("下划线数值", "1_000_000", 0, 1000000);
    float_case("浮点值", "3.14", 0, 3.14);
    float_case("科学计数值", "1e-9", 0, 1e-9);
    text_case("全角标识符归一", "ＡＢＣ", 0, "ABC");
    text_case("字符串内保留全角", "“你好，世界”", 0, "你好，世界");
    text_case("转义解码", "\"a\\nb\\t\\\\\\\"c\"", 0, "a\nb\t\\\"c");

    /* 位置 */
    pos_case("中文列号", "变量 值 = 1", 1, 1, 4);
    pos_case("换行后列号", "变量\n甲", 2, 2, 1);

    printf("\n词法单元测试：通过 %d 例，失败 %d 例\n", g_pass, g_fail);
    ch_arena_destroy(g_arena);
    return g_fail == 0 ? 0 : 1;
}
