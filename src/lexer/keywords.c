#include "lexer/keywords.h"
#include "util/common.h"

/* 同义词表：多个词映射到同一个关键字类型 */
static const struct {
    const char *word;
    TokenKind   kind;
} KW_TABLE[] = {
    {"函数", TK_KW_FN}, {"定义", TK_KW_FN},
    {"返回", TK_KW_RETURN},
    {"变量", TK_KW_VAR}, {"令", TK_KW_VAR},
    {"常量", TK_KW_CONST},
    {"如果", TK_KW_IF}, {"若", TK_KW_IF},
    {"否则", TK_KW_ELSE}, {"不然", TK_KW_ELSE},
    {"当", TK_KW_WHILE}, {"循环", TK_KW_WHILE}, {"只要", TK_KW_WHILE},
    {"对于", TK_KW_FOR}, {"遍历", TK_KW_FOR},
    {"结束", TK_KW_END}, {"完了", TK_KW_END},
    {"打印", TK_KW_PRINT}, {"输出", TK_KW_PRINT}, {"显示", TK_KW_PRINT},
    {"真", TK_KW_TRUE}, {"是", TK_KW_TRUE},
    {"假", TK_KW_FALSE}, {"否", TK_KW_FALSE},
    {"空", TK_KW_NIL}, {"无", TK_KW_NIL},
    {"与", TK_KW_AND}, {"且", TK_KW_AND},
    {"或", TK_KW_OR},
    {"非", TK_KW_NOT},
    {"跳出", TK_KW_BREAK}, {"停止", TK_KW_BREAK},
    {"继续", TK_KW_CONTINUE}, {"跳过", TK_KW_CONTINUE},
    {"导入", TK_KW_IMPORT}, {"引用", TK_KW_IMPORT},
    {"从", TK_KW_FROM},
};

TokenKind ch_keyword_lookup(const char *word) {
    size_t n = sizeof(KW_TABLE) / sizeof(KW_TABLE[0]);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(word, KW_TABLE[i].word) == 0) return KW_TABLE[i].kind;
    }
    return TK_IDENT;
}
