#ifndef CH_TOKEN_H
#define CH_TOKEN_H

#include <stdbool.h>
#include <stddef.h>

/* 记号类型。所有同义中文关键字在词法阶段合并为同一类型。 */
typedef enum {
    TK_EOF = 0,        /* 文件结尾 */
    TK_NEWLINE,        /* 换行（语句分隔符） */
    TK_IDENT,          /* 标识符 */
    TK_INT,            /* 整数 */
    TK_FLOAT,          /* 小数 */
    TK_STRING,         /* 文字 */

    TK_KW_FN,          /* 函数 / 定义 */
    TK_KW_RETURN,      /* 返回 */
    TK_KW_VAR,         /* 变量 / 令 */
    TK_KW_CONST,       /* 常量 */
    TK_KW_IF,          /* 如果 / 若 */
    TK_KW_ELSE,        /* 否则 / 不然 */
    TK_KW_WHILE,       /* 当 / 循环 / 只要 */
    TK_KW_FOR,         /* 对于 / 遍历 */
    TK_KW_END,         /* 结束 / 完了 */
    TK_KW_PRINT,       /* 打印 / 输出 / 显示 */
    TK_KW_TRUE,        /* 真 / 是 */
    TK_KW_FALSE,       /* 假 / 否 */
    TK_KW_NIL,         /* 空 / 无 */
    TK_KW_AND,         /* 与 / 且 */
    TK_KW_OR,          /* 或 */
    TK_KW_NOT,         /* 非 */
    TK_KW_BREAK,       /* 跳出 / 停止 */
    TK_KW_CONTINUE,    /* 继续 / 跳过 */
    TK_KW_IMPORT,      /* 导入 / 引用 */
    TK_KW_FROM,        /* 从 */

    TK_LPAREN,         /* ( */
    TK_RPAREN,         /* ) */
    TK_LBRACKET,       /* [ */
    TK_RBRACKET,       /* ] */
    TK_LBRACE,         /* { */
    TK_RBRACE,         /* } */
    TK_COMMA,          /* , */
    TK_COLON,          /* : */
    TK_SEMI,           /* ; */
    TK_DOT,            /* . */
    TK_ARROW,          /* -> */
    TK_FATARROW,       /* => */
    TK_ELLIPSIS,       /* ... */

    TK_PLUS,           /* + */
    TK_MINUS,          /* - */
    TK_STAR,           /* * */
    TK_SLASH,          /* / */
    TK_PERCENT,        /* % */
    TK_CARET,          /* ^ */
    TK_ASSIGN,         /* = */
    TK_EQ,             /* == */
    TK_NE,             /* != */
    TK_LT,             /* < */
    TK_LE,             /* <= */
    TK_GT,             /* > */
    TK_GE,             /* >= */
    TK_ANDAND,         /* && */
    TK_OROR,           /* || */
    TK_BANG,           /* ! */

    TK_KIND_COUNT
} TokenKind;

typedef struct {
    TokenKind   kind;
    int         line;      /* 1 起，按字符计 */
    int         col;
    size_t      byte_off;  /* 在规范化源码中的起始字节偏移 */
    int         byte_len;
    const char *text;      /* 归一化后的原文（关键字/标识符/数字），可为 NULL */
    const char *sval;      /* 文字字面量解码后的内容，可为 NULL */
    long long   ival;      /* 整数记号的值 */
    double      fval;      /* 小数记号的值 */
} Token;

const char *ch_token_kind_name(TokenKind kind);

#endif /* CH_TOKEN_H */
