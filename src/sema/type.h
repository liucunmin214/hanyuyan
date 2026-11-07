#ifndef CH_TYPE_H
#define CH_TYPE_H

#include <stdbool.h>
#include <stddef.h>

/* 汉语言类型系统
 *
 * 类型以规范形式表示，相同结构的类型共享同一指针，可直接用 == 比较。
 * 阶段三的语义分析器负责创建并驻留（intern）类型。
 */

typedef enum {
    TY_VOID = 0,    /* 空 */
    TY_INT,         /* 整数 (int64) */
    TY_FLOAT,       /* 小数 (float64) */
    TY_STRING,      /* 文字 */
    TY_BOOL,        /* 布尔（内部使用，字面量真/假） */
    TY_LIST,        /* 列表（元素类型 elem） */
    TY_DICT,        /* 字典（文字键 -> 任意值） */
    TY_FUNC,        /* 函数（参数 + 返回） */
    TY_NIL,         /* 空值的类型 */
    TY_UNKNOWN,     /* 尚未推断 */
    TY_ERROR        /* 类型错误占位，避免级联报错 */
} TypeKind;

typedef struct ChType ChType;

struct ChType {
    TypeKind kind;
    ChType  *elem;   /* TY_LIST 的元素类型；TY_FUNC 的返回类型 */
    ChType **params; /* TY_FUNC 的参数类型数组 */
    int      nparams;
    bool     is_variadic;
};

/* 类型可读名，用于诊断 */
const char *ch_type_name(const ChType *t);

/* 类型相等判断（结构等价） */
bool ch_type_equal(const ChType *a, const ChType *b);

/* 是否为数值类型（整数或小数） */
bool ch_type_is_numeric(const ChType *t);

#endif /* CH_TYPE_H */
