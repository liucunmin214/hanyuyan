#ifndef CH_BUILTINS_H
#define CH_BUILTINS_H

#include "sema/type.h"
#include "util/arena.h"

/* 内建（标准库）函数表
 *
 * 这些函数在语义阶段作为普通函数符号可见，在代码生成阶段直接绑定到
 * 运行时符号（由 runtime/运行时 提供实现）。返回值/参数用 TypeKind 表示，
 * TY_UNKNOWN 表示「任意类型」。
 */

typedef struct {
    const char *name;      /* 语言内建名，如 "文字长度" */
    const char *sym;       /* 运行时符号，如 "rt_str_len"；为空表示特殊处理 */
    int         nparams;   /* 参数个数 */
    TypeKind    params[4];
    TypeKind    ret;
} ChBuiltin;

/* 按语言名查找内建函数；未找到返回 NULL */
const ChBuiltin *ch_builtin_find(const char *name);

/* 构造内建函数的函数类型（用于语义阶段的表达式类型） */
ChType *ch_builtin_func_type(ChArena *a, const ChBuiltin *b);

/* 打印语句使用的运行时符号（按静态类型选择） */
const char *ch_builtin_print_sym(const ChType *t);

#endif /* CH_BUILTINS_H */
