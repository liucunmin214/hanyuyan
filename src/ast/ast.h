#ifndef CH_AST_H
#define CH_AST_H

#include <stdio.h>
#include "lexer/token.h"
#include "sema/type.h"

/* 汉语言抽象语法树
 *
 * 所有节点分配在 arena 上。节点用 tag 区分，表达式节点携带已推断类型。
 */

typedef struct AstNode AstNode;
typedef struct Expr Expr;
typedef struct Stmt Stmt;

/* ===== 表达式节点 ===== */

typedef enum {
    EX_INT,         /* 整数字面量 */
    EX_FLOAT,       /* 小数字面量 */
    EX_STRING,      /* 文字字面量 */
    EX_BOOL,        /* 真/假 */
    EX_NIL,         /* 空 */
    EX_IDENT,       /* 标识符 */
    EX_LIST,        /* 列表字面量 [a, b, c] */
    EX_DICT,        /* 字典字面量 {k: v, ...} */
    EX_MEMBER,      /* 表.名（字典成员访问糖） */
    EX_INDEX,       /* a[i] */
    EX_CALL,        /* f(args) */
    EX_LAMBDA,      /* (参数) => 表达式（匿名函数/闭包） */
    EX_UNARY,       /* -x, !x */
    EX_BINARY,      /* a + b */
    EX_ASSIGN,      /* a = b */
    EX_GROUP        /* (expr) */
} ExprKind;

/* 函数/匿名函数参数 */
typedef struct {
    const char *name;
    ChType     *type_annot;   /* 可空 */
    Expr       *default_val;  /* 可空 */
} Param;

struct Expr {
    ExprKind kind;
    int line, col;
    ChType *type;        /* 语义阶段填充 */
    bool is_funcref;     /* EX_IDENT：引用已声明函数名作为值 */
    bool call_indirect;  /* EX_CALL：通过闭包值间接调用 */
    union {
        long long   ival;   /* EX_INT */
        double      fval;   /* EX_FLOAT */
        const char *sval;   /* EX_STRING, EX_IDENT */
        bool        bval;   /* EX_BOOL */
        struct { Expr **items; int count; } list;     /* EX_LIST */
        struct { const char **keys; Expr **vals; int count; } dict; /* EX_DICT */
        struct { Expr *obj; const char *name; } member; /* EX_MEMBER */
        struct { Expr *obj; Expr *idx; } index;        /* EX_INDEX */
        struct { Expr *callee; Expr **args; int nargs; } call; /* EX_CALL */
        struct { Param *params; int nparams; Expr *body;
                 const char **caps; int ncaps; } lambda; /* EX_LAMBDA，caps 为按值捕获名表 */
        struct { TokenKind op; Expr *operand; } unary; /* EX_UNARY */
        struct { TokenKind op; Expr *lhs; Expr *rhs; } binary; /* EX_BINARY */
        struct { Expr *target; Expr *value; } assign;  /* EX_ASSIGN */
        Expr *grouped;   /* EX_GROUP */
    } u;
};

/* ===== 语句节点 ===== */

typedef enum {
    ST_VAR,         /* 变量 名[:类型] = 初值 */
    ST_CONST,       /* 常量 名 = 初值 */
    ST_ASSIGN,      /* 目标 = 值 */
    ST_RETURN,      /* 返回 [expr] */
    ST_IF,          /* 如果 cond then [否则 else] 结束 */
    ST_WHILE,       /* 当 cond body 结束 */
    ST_FOR,         /* 对于 var = start 到 end body 结束 */
    ST_BREAK,       /* 跳出 */
    ST_CONTINUE,    /* 继续 */
    ST_PRINT,       /* 打印 expr */
    ST_EXPR,        /* 表达式语句 */
    ST_BLOCK        /* 语句块 */
} StmtKind;

struct Stmt {
    StmtKind kind;
    int line, col;
    union {
        struct { const char *name; ChType *annot; Expr *init; } var;  /* ST_VAR */
        struct { const char *name; Expr *init; } konst;              /* ST_CONST */
        struct { Expr *target; Expr *value; } assign;                /* ST_ASSIGN */
        struct { Expr *value; } ret;                                 /* ST_RETURN */
        struct { Expr *cond; Stmt *then_b; Stmt *else_b; } iff;      /* ST_IF */
        struct { Expr *cond; Stmt *body; } whil;                     /* ST_WHILE */
        struct { const char *var; Expr *start; Expr *end; Stmt *body; } forn; /* ST_FOR */
        struct { Expr *expr; } prnt;                                 /* ST_PRINT */
        struct { Expr *expr; } expr;                                 /* ST_EXPR */
        struct { Stmt **stmts; int count; } block;                   /* ST_BLOCK */
    } u;
};

/* ===== 顶层声明 ===== */

typedef enum {
    DECL_FUNC,    /* 函数 */
    DECL_VAR,     /* 变量 */
    DECL_CONST,   /* 常量 */
    DECL_STMT,    /* 顶层语句（赋值/打印/表达式等） */
    DECL_IMPORT   /* 导入 "相对路径.汉" */
} DeclKind;

typedef struct {
    DeclKind kind;
    int line, col;
    const char *name;
    ChType     *type;        /* 语义阶段填充：函数为 TY_FUNC，变量为具体类型 */
    union {
        struct {
            Param  *params;
            int     nparams;
            bool    is_variadic;
            const char *variadic_name; /* ...名字 的收集列表名，可空 */
            ChType *ret_annot;
            Stmt   *body;          /* ST_BLOCK */
        } fn;
        struct { ChType *annot; Expr *init; } var;
        struct { Expr *init; } konst;
        Stmt   *stmt;         /* DECL_STMT */
        struct { const char *path; } import;  /* DECL_IMPORT */
    } u;
} Decl;

/* ===== 统一节点 ===== */

typedef enum { NODE_EXPR, NODE_STMT, NODE_DECL } NodeKind;

struct AstNode {
    NodeKind kind;
    union {
        Expr  *expr;
        Stmt  *stmt;
        Decl  *decl;
    } as;
};

/* 辅助构造函数（全部从 arena 分配） */
typedef struct ChArena ChArena;

Expr *ch_ast_int(ChArena *a, int line, int col, long long v);
Expr *ch_ast_float(ChArena *a, int line, int col, double v);
Expr *ch_ast_string(ChArena *a, int line, int col, const char *s);
Expr *ch_ast_bool(ChArena *a, int line, int col, bool v);
Expr *ch_ast_nil(ChArena *a, int line, int col);
Expr *ch_ast_ident(ChArena *a, int line, int col, const char *name);
Expr *ch_ast_list(ChArena *a, int line, int col, Expr **items, int count);
Expr *ch_ast_dict(ChArena *a, int line, int col, const char **keys, Expr **vals, int count);
Expr *ch_ast_member(ChArena *a, int line, int col, Expr *obj, const char *name);
Expr *ch_ast_index(ChArena *a, int line, int col, Expr *obj, Expr *idx);
Expr *ch_ast_call(ChArena *a, int line, int col, Expr *callee, Expr **args, int nargs);
Expr *ch_ast_lambda(ChArena *a, int line, int col, Param *params, int nparams, Expr *body);
Expr *ch_ast_unary(ChArena *a, int line, int col, TokenKind op, Expr *operand);
Expr *ch_ast_binary(ChArena *a, int line, int col, TokenKind op, Expr *lhs, Expr *rhs);
Expr *ch_ast_assign(ChArena *a, int line, int col, Expr *target, Expr *value);
Expr *ch_ast_group(ChArena *a, int line, int col, Expr *inner);

Stmt *ch_stmt_var(ChArena *a, int line, int col, const char *name, ChType *annot, Expr *init);
Stmt *ch_stmt_const(ChArena *a, int line, int col, const char *name, Expr *init);
Stmt *ch_stmt_assign(ChArena *a, int line, int col, Expr *target, Expr *value);
Stmt *ch_stmt_return(ChArena *a, int line, int col, Expr *value);
Stmt *ch_stmt_if(ChArena *a, int line, int col, Expr *cond, Stmt *then_b, Stmt *else_b);
Stmt *ch_stmt_while(ChArena *a, int line, int col, Expr *cond, Stmt *body);
Stmt *ch_stmt_for(ChArena *a, int line, int col, const char *var, Expr *start, Expr *end, Stmt *body);
Stmt *ch_stmt_break(ChArena *a, int line, int col);
Stmt *ch_stmt_continue(ChArena *a, int line, int col);
Stmt *ch_stmt_print(ChArena *a, int line, int col, Expr *expr);
Stmt *ch_stmt_expr(ChArena *a, int line, int col, Expr *expr);
Stmt *ch_stmt_block(ChArena *a, int line, int col, Stmt **stmts, int count);

Decl *ch_decl_func(ChArena *a, int line, int col, const char *name,
                   Param *params, int nparams, bool is_variadic,
                   const char *variadic_name, ChType *ret_annot, Stmt *body);
Decl *ch_decl_var(ChArena *a, int line, int col, const char *name, ChType *annot, Expr *init);
Decl *ch_decl_const(ChArena *a, int line, int col, const char *name, Expr *init);
Decl *ch_decl_stmt(ChArena *a, int line, int col, Stmt *stmt);
Decl *ch_decl_import(ChArena *a, int line, int col, const char *path);

/* 把 Expr/Stmt/Decl 包装为 AstNode */
AstNode *ch_node_expr(ChArena *a, Expr *e);
AstNode *ch_node_stmt(ChArena *a, Stmt *s);
AstNode *ch_node_decl(ChArena *a, Decl *d);

/* AST 打印（用于 --dump-ast），缩进 indent 层 */
void ch_ast_dump_expr(const Expr *e, int indent, FILE *out);
void ch_ast_dump_stmt(const Stmt *s, int indent, FILE *out);
void ch_ast_dump_decl(const Decl *d, int indent, FILE *out);

#endif /* CH_AST_H */
