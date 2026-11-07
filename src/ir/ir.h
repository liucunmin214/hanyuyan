#ifndef CH_IR_H
#define CH_IR_H

#include "sema/type.h"
#include "util/common.h"
#include "util/arena.h"
#include "ast/ast.h"

/* 三地址码中间表示
 *
 * 值用编号表示：%0, %1, ...，每个函数有自己的值编号空间。
 * IR 只描述数据流与控制流，不涉及目标架构细节；后端据此做指令选择与分配。
 */

typedef enum {
    IR_NOP = 0,
    IR_CONST,       /* dst = 常量 (ival 或 fval 或 sval) */
    IR_LOAD,        /* dst = 变量 name（全局或局部） */
    IR_STORE,       /* 变量 name = src1 */
    IR_MOV,         /* dst = src1 */
    IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_MOD, IR_POW,
    IR_NEG,         /* dst = -src1 */
    IR_ITOF,        /* dst = (小数)src1：整数按值转换为 double 位模式 */
    IR_NOT,         /* dst = !src1 */
    IR_EQ, IR_NE, IR_LT, IR_LE, IR_GT, IR_GE,
    IR_AND, IR_OR,
    IR_CALL,        /* dst = sym(args)，sym 为函数目标符号 */
    IR_CALL_IND,    /* dst = src1(args)，经闭包值间接调用；隐藏第 0 实参为环境 */
    IR_PRINT,       /* 打印 src1（按 type 选择输出方式，附换行） */
    IR_RET,         /* 返回 [src1] */
    IR_JUMP,        /* 无条件跳转到 label */
    IR_BRANCH,      /* if src1 跳转到 label（为假时跳到 label2） */
    IR_LABEL,       /* 标签定义 */
    IR_INDEX_GET,   /* dst = arr[idx] */
    IR_INDEX_SET,   /* arr[idx] = val（val 在 src2） */
    IR_LIST_NEW,    /* dst = 新建列表，元素来自 args */
    IR_DICT_NEW,    /* dst = 新建空字典 */
    IR_DICT_GET,    /* dst = src1[src2]（字典取，src2 为文字键） */
    IR_DICT_SET,    /* src1[src2] = dst（字典设，dst 存值） */
    IR_LAMBDA_NEW,  /* dst = 新建闭包 {函数指针 sval, 环境}，捕获值来自 args */
    IR_ENV_LOAD     /* dst = 环境[ival]（匿名函数内读取第 ival 个捕获变量） */
} IrOp;

typedef struct {
    IrOp op;
    int  dst;          /* 目标值编号，-1 表示无 */
    int  src1;         /* 源操作数 1 */
    int  src2;         /* 源操作数 2 */
    int  label;        /* 跳转目标标签号 */
    int  label2;       /* 第二标签 */
    int *args;         /* IR_CALL / IR_LIST_NEW 的参数 */
    int  nargs;
    long long ival;    /* IR_CONST 整数 / IR_PRINT 布尔 */
    double    fval;    /* IR_CONST 小数 */
    const char *sval;  /* IR_CONST 文字 / IR_LOAD/STORE 变量名 / IR_CALL 目标符号 */
    ChType *type;      /* 该值的类型（结果类型） */
    ChType *opnd_type; /* 二元运算左操作数类型（区分整数/小数/文字） */
    ChType *opnd2_type;/* 二元运算右操作数类型 */
    ChType **arg_types;/* IR_CALL 各实参类型（决定整数寄存器或 xmm） */
    int line, col;
} IrInstr;

typedef struct {
    ChVec instrs;      /* IrInstr */
    int   next_temp;   /* 下一个临时值编号 */
    int   next_label;  /* 下一个标签编号 */
    const char *name;  /* 函数名 */
    const char *sym;   /* 目标符号名 */
    ChType *ret_type;
    ChType **param_types;
    const char **param_names;
    int    nparams;
} IrFunc;

/* 全局变量（模块级 变量/常量） */
typedef struct {
    const char *name;
    const char *sym;
    ChType     *type;
} IrGlobal;

typedef struct {
    IrFunc  **funcs;
    int       nfuncs;
    IrGlobal *globals;
    int       nglobals;
    const char *entry_sym;  /* 隐式主函数符号（可为 NULL） */
} IrModule;

/* 从已语义检查的 AST 生成 IR */
IrModule *ch_ir_gen(ChArena *arena, AstNode **decls, int ndecls);

/* 常量折叠等优化（原地修改） */
void ch_ir_optimize(ChArena *arena, IrModule *mod);

/* 打印 IR（用于 --dump-ir） */
void ch_ir_dump(const IrModule *mod, FILE *out);

#endif /* CH_IR_H */
