#include "ir/ir.h"
#include "ast/ast.h"
#include "sema/builtins.h"
#include "sema/symbol.h"
#include "util/arena.h"
#include <string.h>
#include <stdio.h>

#define MAX_LOOP_DEPTH 64

static IrFunc *new_func(ChArena *a, const char *name, const char *sym, ChType *ret);

/* 模块级生成上下文：被提升的匿名函数登记表 */
typedef struct {
    ChArena  *arena;
    AstNode **decls;
    int       ndecls;
    ChVec     extra_funcs;   /* IrFunc *：匿名函数提升产物 */
    int       lambda_seq;    /* 匿名函数符号序号 */
} IrModCtx;

typedef struct {
    ChArena *arena;
    IrFunc  *fn;
    ChVec   *instrs;     /* 指向 fn->instrs */
    int      ret_label;  /* 函数返回统一标签 */
    int      break_lbl[MAX_LOOP_DEPTH];
    int      cont_lbl[MAX_LOOP_DEPTH];
    int      loop_depth;
    IrModCtx *mx;        /* 模块上下文 */
    const char **caps;   /* 当前匿名函数的捕获名表（普通函数为 NULL） */
    int      ncaps;
} IrGen;

/* 查命名函数声明（用于默认值补齐与可变参数打包） */
static Decl *find_func_decl(IrGen *g, const char *name) {
    for (int i = 0; i < g->mx->ndecls; i++) {
        Decl *d = g->mx->decls[i]->as.decl;
        if (d->kind == DECL_FUNC && strcmp(d->name, name) == 0) return d;
    }
    return NULL;
}

/* 当前函数是否为被提升的匿名函数，且 name 是其第 idx 个捕获变量 */
static int capture_index(IrGen *g, const char *name) {
    for (int i = 0; i < g->ncaps; i++)
        if (strcmp(g->caps[i], name) == 0) return i;
    return -1;
}

static int new_temp(IrGen *g) { return g->fn->next_temp++; }
static int new_label(IrGen *g) { return g->fn->next_label++; }

static const char *mangle(ChArena *a, const char *prefix, const char *name) {
    size_t n = strlen(prefix) + strlen(name) + 1;
    char *s = ch_arena_alloc(a, n);
    strcpy(s, prefix);
    strcat(s, name);
    return s;
}

static IrInstr *emit(IrGen *g, IrOp op) {
    IrInstr *i = ch_vec_push(g->instrs);
    memset(i, 0, sizeof(*i));
    i->op = op;
    i->dst = -1;
    i->src1 = -1;
    i->src2 = -1;
    i->label = -1;
    i->label2 = -1;
    return i;
}

static int emit_const_int(IrGen *g, long long v) {
    int t = new_temp(g);
    IrInstr *i = emit(g, IR_CONST);
    i->dst = t; i->ival = v; i->type = ch_type_get(g->arena, TY_INT);
    return t;
}
static int emit_const_float(IrGen *g, double v) {
    int t = new_temp(g);
    IrInstr *i = emit(g, IR_CONST);
    i->dst = t; i->fval = v; i->type = ch_type_get(g->arena, TY_FLOAT);
    return t;
}
static int emit_const_string(IrGen *g, const char *s) {
    int t = new_temp(g);
    IrInstr *i = emit(g, IR_CONST);
    i->dst = t; i->sval = s; i->type = ch_type_get(g->arena, TY_STRING);
    return t;
}
static int emit_const_bool(IrGen *g, bool v) {
    int t = new_temp(g);
    IrInstr *i = emit(g, IR_CONST);
    i->dst = t; i->ival = v ? 1 : 0; i->type = ch_type_get(g->arena, TY_BOOL);
    return t;
}

static void emit_label(IrGen *g, int lbl) {
    IrInstr *i = emit(g, IR_LABEL);
    i->label = lbl;
}

static int gen_expr(IrGen *g, Expr *e);
static int coerce_value(IrGen *g, int v, ChType *from, ChType *to);

/* 解析调用目标符号 */
static const char *resolve_call_sym(IrGen *g, Expr *callee, Expr *first_arg) {
    if (callee->kind == EX_IDENT) {
        const ChBuiltin *b = ch_builtin_find(callee->u.sval);
        if (b) {
            if (b->sym && b->sym[0]) return b->sym;
            /* 转文字：按实参静态类型分派 */
            TypeKind k = first_arg && first_arg->type ? first_arg->type->kind : TY_INT;
            switch (k) {
            case TY_STRING: return "rt_str_identity";
            case TY_FLOAT:  return "rt_float_to_str";
            case TY_BOOL:   return "rt_bool_to_str";
            case TY_NIL:    return "rt_nil_to_str";
            case TY_FUNC:   return "rt_func_to_str";
            default:        return "rt_int_to_str";
            }
        }
        return mangle(g->arena, "chf_", callee->u.sval);
    }
    return NULL;
}

/* 按名读取：匿名函数内优先读捕获环境，否则按普通变量加载 */
static int load_named(IrGen *g, const char *name, ChType *type) {
    int idx = capture_index(g, name);
    if (idx >= 0) {
        int t = new_temp(g);
        IrInstr *i = emit(g, IR_ENV_LOAD);
        i->dst = t; i->ival = idx; i->type = type;
        return t;
    }
    int t = new_temp(g);
    IrInstr *i = emit(g, IR_LOAD);
    i->dst = t; i->sval = name; i->type = type;
    return t;
}

/* 生成匿名函数：提升为带隐藏环境参数的普通函数，并在当前位置创建闭包值 */
static int gen_lambda(IrGen *g, Expr *e) {
    ChArena *a = g->arena;
    IrModCtx *mx = g->mx;
    /* 提升函数符号：chlam_<序号> */
    char symbuf[32];
    snprintf(symbuf, sizeof(symbuf), "chlam_%d", mx->lambda_seq++);
    const char *liftsym = ch_arena_strdup(a, symbuf);

    IrFunc *lf = new_func(a, "匿名", liftsym,
                          e->type ? e->type->elem : NULL);
    int np = e->u.lambda.nparams;
    lf->nparams = np + 1;   /* 第 0 参数为隐藏环境指针 */
    lf->param_types = ch_arena_alloc(a, (size_t)lf->nparams * sizeof(ChType *));
    lf->param_names = ch_arena_alloc(a, (size_t)lf->nparams * sizeof(const char *));
    lf->param_types[0] = NULL;          /* 环境指针：按整数寄存器传递 */
    lf->param_names[0] = "__env";
    for (int k = 0; k < np; k++) {
        lf->param_types[1 + k] = e->u.lambda.params[k].type_annot;
        lf->param_names[1 + k] = e->u.lambda.params[k].name;
    }
    IrGen lg = { a, lf, &lf->instrs, -1, {0}, {0}, 0, mx,
                 e->u.lambda.caps, e->u.lambda.ncaps };
    int rv = gen_expr(&lg, e->u.lambda.body);
    IrInstr *r = emit(&lg, IR_RET);
    r->src1 = rv;
    IrFunc **slot = ch_vec_push(&mx->extra_funcs);
    *slot = lf;

    /* 创建闭包值：捕获值按当前上下文求值（可按环境链传递） */
    int nc = e->u.lambda.ncaps;
    int *cvals = ch_arena_alloc(a, (size_t)(nc ? nc : 1) * sizeof(int));
    for (int k = 0; k < nc; k++)
        cvals[k] = load_named(g, e->u.lambda.caps[k], NULL);
    int t = new_temp(g);
    IrInstr *i = emit(g, IR_LAMBDA_NEW);
    i->dst = t;
    i->sval = liftsym;
    i->args = cvals;
    i->nargs = nc;
    i->type = e->type;
    return t;
}

/* 命名函数作为值：生成带隐藏环境参数的包装函数，统一闭包调用约定 */
static int gen_funcref(IrGen *g, Expr *e) {
    ChArena *a = g->arena;
    IrModCtx *mx = g->mx;
    char symbuf[32];
    snprintf(symbuf, sizeof(symbuf), "chwrap_%d", mx->lambda_seq++);
    const char *wrapsym = ch_arena_strdup(a, symbuf);
    ChType *ft = e->type;
    int np = ft && ft->kind == TY_FUNC ? ft->nparams : 0;
    IrFunc *wf = new_func(a, "包装", wrapsym, ft ? ft->elem : NULL);
    wf->nparams = np + 1;   /* 第 0 参数为隐藏环境指针（包装函数不使用） */
    wf->param_types = ch_arena_alloc(a, (size_t)wf->nparams * sizeof(ChType *));
    wf->param_names = ch_arena_alloc(a, (size_t)wf->nparams * sizeof(const char *));
    wf->param_types[0] = NULL;
    wf->param_names[0] = "__env";
    IrGen wg = { a, wf, &wf->instrs, -1, {0}, {0}, 0, mx, NULL, 0 };
    int *cargs = ch_arena_alloc(a, (size_t)(np ? np : 1) * sizeof(int));
    ChType **ctypes = ch_arena_alloc(a, (size_t)(np ? np : 1) * sizeof(ChType *));
    for (int k = 0; k < np; k++) {
        char pn[16];
        snprintf(pn, sizeof(pn), "__p%d", k);
        wf->param_types[1 + k] = ft->params[k];
        wf->param_names[1 + k] = ch_arena_strdup(a, pn);
        int t = new_temp(&wg);
        IrInstr *ld = emit(&wg, IR_LOAD);
        ld->dst = t; ld->sval = wf->param_names[1 + k]; ld->type = ft->params[k];
        cargs[k] = t;
        ctypes[k] = ft->params[k];
    }
    int rt = new_temp(&wg);
    IrInstr *call = emit(&wg, IR_CALL);
    call->dst = rt;
    call->sval = mangle(a, "chf_", e->u.sval);
    call->nargs = np;
    call->args = cargs;
    call->arg_types = ctypes;
    call->type = ft ? ft->elem : NULL;
    IrInstr *r = emit(&wg, IR_RET);
    r->src1 = rt;
    IrFunc **slot = ch_vec_push(&mx->extra_funcs);
    *slot = wf;

    int t = new_temp(g);
    IrInstr *i = emit(g, IR_LAMBDA_NEW);
    i->dst = t;
    i->sval = wrapsym;
    i->args = NULL;
    i->nargs = 0;
    i->type = e->type;
    return t;
}

static int gen_expr(IrGen *g, Expr *e) {
    if (!e) return -1;
    switch (e->kind) {
    case EX_INT:    return emit_const_int(g, e->u.ival);
    case EX_FLOAT:  return emit_const_float(g, e->u.fval);
    case EX_STRING: return emit_const_string(g, e->u.sval);
    case EX_BOOL:   return emit_const_bool(g, e->u.bval);
    case EX_NIL:    return emit_const_int(g, 0);
    case EX_IDENT: {
        if (e->is_funcref) return gen_funcref(g, e);
        return load_named(g, e->u.sval, e->type);
    }
    case EX_DICT: {
        /* 字典字面量：新建后逐对写入 */
        int t = new_temp(g);
        IrInstr *i = emit(g, IR_DICT_NEW);
        i->dst = t; i->type = e->type;
        for (int k = 0; k < e->u.dict.count; k++) {
            int key = emit_const_string(g, e->u.dict.keys[k]);
            int val = gen_expr(g, e->u.dict.vals[k]);
            IrInstr *s = emit(g, IR_DICT_SET);
            s->src1 = t; s->src2 = key; s->dst = val;
        }
        return t;
    }
    case EX_MEMBER: {
        /* 表.名 读取糖：等价 字典取(表, "名") */
        int obj = gen_expr(g, e->u.member.obj);
        int key = emit_const_string(g, e->u.member.name);
        int t = new_temp(g);
        IrInstr *i = emit(g, IR_DICT_GET);
        i->dst = t; i->src1 = obj; i->src2 = key; i->type = e->type;
        return t;
    }
    case EX_LAMBDA:
        return gen_lambda(g, e);
    case EX_LIST: {
        /* 先求值各元素，再发射「新建列表」指令 */
        int n = e->u.list.count;
        int *vals = ch_arena_alloc(g->arena, (size_t)n * sizeof(int));
        for (int k = 0; k < n; k++) vals[k] = gen_expr(g, e->u.list.items[k]);
        int t = new_temp(g);
        IrInstr *i = emit(g, IR_LIST_NEW);
        i->dst = t;
        i->nargs = n;
        i->args = ch_arena_alloc(g->arena, (size_t)n * sizeof(int));
        for (int k = 0; k < n; k++) i->args[k] = vals[k];
        i->type = e->type;
        return t;
    }
    case EX_INDEX: {
        int obj = gen_expr(g, e->u.index.obj);
        int idx = gen_expr(g, e->u.index.idx);
        int t = new_temp(g);
        IrInstr *i = emit(g, IR_INDEX_GET);
        i->dst = t; i->src1 = obj; i->src2 = idx; i->type = e->type;
        return t;
    }
    case EX_CALL: {
        Expr *callee = e->u.call.callee;
        int n = e->u.call.nargs;
        if (e->call_indirect) {
            /* 间接调用：先求闭包值，再求实参 */
            int clos = gen_expr(g, callee);
            int *vals = ch_arena_alloc(g->arena, (size_t)(n ? n : 1) * sizeof(int));
            ChType **types = ch_arena_alloc(g->arena, (size_t)(n ? n : 1) * sizeof(ChType *));
            for (int k = 0; k < n; k++) {
                vals[k] = gen_expr(g, e->u.call.args[k]);
                types[k] = e->u.call.args[k] ? e->u.call.args[k]->type : NULL;
            }
            int t = new_temp(g);
            IrInstr *i = emit(g, IR_CALL_IND);
            i->dst = t; i->src1 = clos;
            i->nargs = n; i->args = vals; i->arg_types = types;
            i->type = e->type;
            return t;
        }
        /* 静态调用：默认参数补齐 + 可变参数打包为列表 */
        Decl *fd = (callee->kind == EX_IDENT) ? find_func_decl(g, callee->u.sval) : NULL;
        int fixed = fd ? fd->u.fn.nparams : n;
        bool pack = fd && fd->u.fn.is_variadic && fd->u.fn.variadic_name != NULL;
        int total = pack ? fixed + 1 : fixed;
        /* 先求值全部已给实参（保持从左到右的求值顺序） */
        int *given = ch_arena_alloc(g->arena, (size_t)(n ? n : 1) * sizeof(int));
        ChType **gtypes = ch_arena_alloc(g->arena, (size_t)(n ? n : 1) * sizeof(ChType *));
        for (int k = 0; k < n; k++) {
            given[k] = gen_expr(g, e->u.call.args[k]);
            gtypes[k] = e->u.call.args[k] ? e->u.call.args[k]->type : NULL;
        }
        int *vals = ch_arena_alloc(g->arena, (size_t)(total ? total : 1) * sizeof(int));
        ChType **types = ch_arena_alloc(g->arena, (size_t)(total ? total : 1) * sizeof(ChType *));
        int use = n < fixed ? n : fixed;
        for (int k = 0; k < use; k++) {
            /* 形参声明为小数、实参为整数时，先转成 double 再传 */
            ChType *want = (fd && fd->u.fn.params) ? fd->u.fn.params[k].type_annot : NULL;
            vals[k] = given[k];
            types[k] = gtypes[k];
            if (want && want->kind == TY_FLOAT && gtypes[k] && gtypes[k]->kind == TY_INT) {
                vals[k] = coerce_value(g, given[k], gtypes[k], want);
                types[k] = want;
            }
        }
        /* 缺失的固定参数：用声明处的默认值表达式 */
        for (int k = use; k < fixed; k++) {
            ChType *want = fd->u.fn.params[k].type_annot;
            int dv = gen_expr(g, fd->u.fn.params[k].default_val);
            vals[k] = coerce_value(g, dv,
                                   fd->u.fn.params[k].default_val
                                       ? fd->u.fn.params[k].default_val->type : NULL,
                                   want);
            types[k] = want;
        }
        if (pack) {
            /* 多余实参收集为列表，作为最后一个隐式实参 */
            int extra = n > fixed ? n - fixed : 0;
            int lt = new_temp(g);
            IrInstr *li = emit(g, IR_LIST_NEW);
            li->dst = lt; li->nargs = extra;
            li->args = ch_arena_alloc(g->arena, (size_t)(extra ? extra : 1) * sizeof(int));
            for (int k = 0; k < extra; k++) li->args[k] = given[fixed + k];
            vals[fixed] = lt;
            types[fixed] = NULL;
        }
        const char *sym = resolve_call_sym(g, callee,
                                           n > 0 ? e->u.call.args[0] : NULL);
        int t = new_temp(g);
        IrInstr *i = emit(g, IR_CALL);
        i->dst = t;
        i->sval = sym;
        i->nargs = total;
        i->args = vals;
        i->arg_types = types;
        i->type = e->type;
        return t;
    }
    case EX_UNARY: {
        int src = gen_expr(g, e->u.unary.operand);
        int t = new_temp(g);
        IrInstr *i = emit(g, e->u.unary.op == TK_BANG ? IR_NOT : IR_NEG);
        i->dst = t; i->src1 = src; i->type = e->type;
        i->opnd_type = e->u.unary.operand ? e->u.unary.operand->type : NULL;
        return t;
    }
    case EX_BINARY: {
        /* 逻辑与/或：短路求值，用控制流而不是按位运算 */
        if (e->u.binary.op == TK_ANDAND || e->u.binary.op == TK_OROR) {
            bool is_and = e->u.binary.op == TK_ANDAND;
            int t = new_temp(g);
            int l = gen_expr(g, e->u.binary.lhs);
            IrInstr *mv = emit(g, IR_MOV);
            mv->dst = t; mv->src1 = l; mv->type = ch_type_get(g->arena, TY_BOOL);
            int end_l = new_label(g);
            int rhs_l = new_label(g);
            if (is_and) {
                /* 左边为假 -> 直接到结束（保留 0） */
                IrInstr *br = emit(g, IR_BRANCH);
                br->src1 = t; br->label = end_l;
            } else {
                /* 左边为真 -> 直接到结束；否则求右边 */
                IrInstr *br = emit(g, IR_BRANCH);
                br->src1 = t; br->label = rhs_l;
                IrInstr *j = emit(g, IR_JUMP);
                j->label = end_l;
                emit_label(g, rhs_l);
            }
            int r = gen_expr(g, e->u.binary.rhs);
            IrInstr *mv2 = emit(g, IR_MOV);
            mv2->dst = t; mv2->src1 = r; mv2->type = ch_type_get(g->arena, TY_BOOL);
            emit_label(g, end_l);
            return t;
        }
        int l = gen_expr(g, e->u.binary.lhs);
        int r = gen_expr(g, e->u.binary.rhs);
        int t = new_temp(g);
        IrOp op;
        switch (e->u.binary.op) {
        case TK_PLUS: op = IR_ADD; break;
        case TK_MINUS: op = IR_SUB; break;
        case TK_STAR: op = IR_MUL; break;
        case TK_SLASH: op = IR_DIV; break;
        case TK_PERCENT: op = IR_MOD; break;
        case TK_CARET: op = IR_POW; break;
        case TK_EQ: op = IR_EQ; break;
        case TK_NE: op = IR_NE; break;
        case TK_LT: op = IR_LT; break;
        case TK_LE: op = IR_LE; break;
        case TK_GT: op = IR_GT; break;
        case TK_GE: op = IR_GE; break;
        default: op = IR_NOP;
        }
        IrInstr *i = emit(g, op);
        i->dst = t; i->src1 = l; i->src2 = r; i->type = e->type;
        i->opnd_type = e->u.binary.lhs ? e->u.binary.lhs->type : NULL;
        i->opnd2_type = e->u.binary.rhs ? e->u.binary.rhs->type : NULL;
        return t;
    }
    case EX_ASSIGN: {
        int val = gen_expr(g, e->u.assign.value);
        Expr *tgt = e->u.assign.target;
        if (tgt->kind == EX_IDENT) {
            val = coerce_value(g, val, e->u.assign.value->type, tgt->type);
            IrInstr *i = emit(g, IR_STORE);
            i->sval = tgt->u.sval; i->src1 = val;
        } else if (tgt->kind == EX_INDEX) {
            int obj = gen_expr(g, tgt->u.index.obj);
            int idx = gen_expr(g, tgt->u.index.idx);
            IrInstr *i = emit(g, IR_INDEX_SET);
            i->src1 = obj; i->src2 = idx; i->dst = val;
        } else if (tgt->kind == EX_MEMBER) {
            /* 表.名 = 值 糖：等价 字典设(表, "名", 值) */
            int obj = gen_expr(g, tgt->u.member.obj);
            int key = emit_const_string(g, tgt->u.member.name);
            IrInstr *i = emit(g, IR_DICT_SET);
            i->src1 = obj; i->src2 = key; i->dst = val;
        }
        return val;
    }
    case EX_GROUP:
        return gen_expr(g, e->u.grouped);
    }
    return -1;
}

static void gen_stmt(IrGen *g, Stmt *st);

static void gen_block(IrGen *g, Stmt *block) {
    if (!block || block->kind != ST_BLOCK) return;
    for (int i = 0; i < block->u.block.count; i++) {
        gen_stmt(g, block->u.block.stmts[i]);
    }
}

/* 按目标类型调整值：整数写入小数槽必须显式转换，
 * 否则会把整数的位模式当成 double 读出来（打印为 0 或极小值）。 */
static int coerce_value(IrGen *g, int v, ChType *from, ChType *to) {
    if (to && to->kind == TY_FLOAT && from && from->kind == TY_INT) {
        int t = new_temp(g);
        IrInstr *i = emit(g, IR_ITOF);
        i->dst = t; i->src1 = v;
        i->opnd_type = from;
        i->type = to;
        return t;
    }
    return v;
}

static void gen_stmt(IrGen *g, Stmt *st) {
    if (!st) return;
    switch (st->kind) {
    case ST_VAR:
    case ST_CONST: {
        Expr *init = st->kind == ST_VAR ? st->u.var.init : st->u.konst.init;
        const char *name = st->kind == ST_VAR ? st->u.var.name : st->u.konst.name;
        if (init) {
            int v = gen_expr(g, init);
            ChType *want = st->kind == ST_VAR ? st->u.var.annot : NULL;
            v = coerce_value(g, v, init->type, want);
            IrInstr *i = emit(g, IR_STORE);
            i->sval = name; i->src1 = v;
        }
        return;
    }
    case ST_ASSIGN: {
        Expr *tgt = st->u.assign.target;
        int val = gen_expr(g, st->u.assign.value);
        if (tgt->kind == EX_IDENT) {
            val = coerce_value(g, val, st->u.assign.value->type, tgt->type);
            IrInstr *i = emit(g, IR_STORE);
            i->sval = tgt->u.sval; i->src1 = val;
        } else if (tgt->kind == EX_INDEX) {
            int obj = gen_expr(g, tgt->u.index.obj);
            int idx = gen_expr(g, tgt->u.index.idx);
            IrInstr *i = emit(g, IR_INDEX_SET);
            i->src1 = obj; i->src2 = idx; i->dst = val;
        } else if (tgt->kind == EX_MEMBER) {
            int obj = gen_expr(g, tgt->u.member.obj);
            int key = emit_const_string(g, tgt->u.member.name);
            IrInstr *i = emit(g, IR_DICT_SET);
            i->src1 = obj; i->src2 = key; i->dst = val;
        }
        return;
    }
    case ST_RETURN: {
        if (st->u.ret.value) {
            int v = gen_expr(g, st->u.ret.value);
            v = coerce_value(g, v, st->u.ret.value->type, g->fn->ret_type);
            IrInstr *i = emit(g, IR_RET);
            i->src1 = v;
        } else {
            emit(g, IR_RET);
        }
        return;
    }
    case ST_IF: {
        int cond = gen_expr(g, st->u.iff.cond);
        int else_l = new_label(g);
        int end_l = new_label(g);
        IrInstr *br = emit(g, IR_BRANCH);
        br->src1 = cond; br->label = st->u.iff.else_b ? else_l : end_l; br->label2 = -1;
        gen_block(g, st->u.iff.then_b);
        if (st->u.iff.else_b) {
            IrInstr *j = emit(g, IR_JUMP);
            j->label = end_l;
            emit_label(g, else_l);
            gen_block(g, st->u.iff.else_b);
        }
        emit_label(g, end_l);
        return;
    }
    case ST_WHILE: {
        int head = new_label(g);
        int exit_l = new_label(g);
        emit_label(g, head);
        int cond = gen_expr(g, st->u.whil.cond);
        IrInstr *br = emit(g, IR_BRANCH);
        br->src1 = cond; br->label = exit_l; br->label2 = -1;
        if (g->loop_depth < MAX_LOOP_DEPTH) {
            g->break_lbl[g->loop_depth] = exit_l;
            g->cont_lbl[g->loop_depth] = head;
            g->loop_depth++;
        }
        gen_block(g, st->u.whil.body);
        if (g->loop_depth > 0) g->loop_depth--;
        IrInstr *j = emit(g, IR_JUMP);
        j->label = head;
        emit_label(g, exit_l);
        return;
    }
    case ST_FOR: {
        /* 对于 var = start 到 end  =>  while (var <= end) { ...; var = var + 1 } */
        int s = gen_expr(g, st->u.forn.start);
        IrInstr *store_s = emit(g, IR_STORE);
        store_s->sval = st->u.forn.var; store_s->src1 = s;
        int head = new_label(g);
        int exit_l = new_label(g);
        int cont_l = new_label(g);
        emit_label(g, head);
        /* 条件: var <= end */
        int v = new_temp(g);
        IrInstr *load = emit(g, IR_LOAD);
        load->dst = v; load->sval = st->u.forn.var; load->type = ch_type_get(g->arena, TY_INT);
        int endv = gen_expr(g, st->u.forn.end);
        int cmp = new_temp(g);
        IrInstr *le = emit(g, IR_LE);
        le->dst = cmp; le->src1 = v; le->src2 = endv;
        le->type = ch_type_get(g->arena, TY_BOOL);
        le->opnd_type = ch_type_get(g->arena, TY_INT);
        IrInstr *br = emit(g, IR_BRANCH);
        br->src1 = cmp; br->label = exit_l; br->label2 = -1;
        if (g->loop_depth < MAX_LOOP_DEPTH) {
            g->break_lbl[g->loop_depth] = exit_l;
            g->cont_lbl[g->loop_depth] = cont_l;
            g->loop_depth++;
        }
        gen_block(g, st->u.forn.body);
        if (g->loop_depth > 0) g->loop_depth--;
        emit_label(g, cont_l);
        /* var = var + 1 */
        int one = emit_const_int(g, 1);
        int v2 = new_temp(g);
        IrInstr *load2 = emit(g, IR_LOAD);
        load2->dst = v2; load2->sval = st->u.forn.var; load2->type = ch_type_get(g->arena, TY_INT);
        int inc = new_temp(g);
        IrInstr *add = emit(g, IR_ADD);
        add->dst = inc; add->src1 = v2; add->src2 = one;
        add->type = ch_type_get(g->arena, TY_INT);
        add->opnd_type = ch_type_get(g->arena, TY_INT);
        IrInstr *store_i = emit(g, IR_STORE);
        store_i->sval = st->u.forn.var; store_i->src1 = inc;
        IrInstr *j = emit(g, IR_JUMP);
        j->label = head;
        emit_label(g, exit_l);
        return;
    }
    case ST_BREAK: {
        if (g->loop_depth > 0) {
            IrInstr *j = emit(g, IR_JUMP);
            j->label = g->break_lbl[g->loop_depth - 1];
        }
        return;
    }
    case ST_CONTINUE: {
        if (g->loop_depth > 0) {
            IrInstr *j = emit(g, IR_JUMP);
            j->label = g->cont_lbl[g->loop_depth - 1];
        }
        return;
    }
    case ST_PRINT: {
        int v = gen_expr(g, st->u.prnt.expr);
        IrInstr *i = emit(g, IR_PRINT);
        i->src1 = v;
        i->type = st->u.prnt.expr ? st->u.prnt.expr->type : NULL;
        return;
    }
    case ST_EXPR:
        gen_expr(g, st->u.expr.expr);
        return;
    case ST_BLOCK:
        gen_block(g, st);
        return;
    }
}

/* ==================== 模块生成 ==================== */

static IrFunc *new_func(ChArena *a, const char *name, const char *sym, ChType *ret) {
    IrFunc *fn = ch_arena_alloc(a, sizeof(IrFunc));
    fn->name = name;
    fn->sym = sym;
    fn->ret_type = ret;
    fn->next_temp = 0;
    fn->next_label = 0;
    fn->nparams = 0;
    fn->param_types = NULL;
    fn->param_names = NULL;
    ch_vec_init(&fn->instrs, sizeof(IrInstr));
    return fn;
}

IrModule *ch_ir_gen(ChArena *arena, AstNode **decls, int ndecls) {
    IrModule *mod = ch_arena_alloc(arena, sizeof(IrModule));
    mod->nfuncs = 0;
    mod->globals = ch_arena_alloc(arena, (size_t)(ndecls ? ndecls : 1) * sizeof(IrGlobal));
    mod->nglobals = 0;
    mod->entry_sym = NULL;

    /* 模块上下文：匿名函数提升登记 */
    IrModCtx mx;
    mx.arena = arena;
    mx.decls = decls;
    mx.ndecls = ndecls;
    ch_vec_init(&mx.extra_funcs, sizeof(IrFunc *));
    mx.lambda_seq = 0;

    ChVec funcs;   /* IrFunc *：普通函数 + 隐式主函数 + 提升的匿名函数 */
    ch_vec_init(&funcs, sizeof(IrFunc *));

    /* 先登记全局变量（模块级 变量/常量） */
    for (int i = 0; i < ndecls; i++) {
        Decl *d = decls[i]->as.decl;
        if (d->kind == DECL_VAR || d->kind == DECL_CONST) {
            IrGlobal *g = &mod->globals[mod->nglobals++];
            g->name = d->name;
            g->sym = mangle(arena, "g_", d->name);
            g->type = d->type;
        }
    }

    /* 函数 */
    for (int i = 0; i < ndecls; i++) {
        Decl *d = decls[i]->as.decl;
        if (d->kind != DECL_FUNC) continue;
        ChType *ft = d->type;
        IrFunc *fn = new_func(arena, d->name, mangle(arena, "chf_", d->name),
                              ft ? ft->elem : NULL);
        /* 可变参数收集名作为末尾的隐式列表形参 */
        int extra = (d->u.fn.is_variadic && d->u.fn.variadic_name) ? 1 : 0;
        fn->nparams = d->u.fn.nparams + extra;
        if (fn->nparams > 0) {
            fn->param_types = ch_arena_alloc(arena, (size_t)fn->nparams * sizeof(ChType *));
            fn->param_names = ch_arena_alloc(arena, (size_t)fn->nparams * sizeof(const char *));
            for (int k = 0; k < d->u.fn.nparams; k++) {
                fn->param_types[k] = d->u.fn.params[k].type_annot
                                   ? d->u.fn.params[k].type_annot : NULL;
                fn->param_names[k] = d->u.fn.params[k].name;
            }
            if (extra) {
                fn->param_types[fn->nparams - 1] = NULL;   /* 列表按整数寄存器传递 */
                fn->param_names[fn->nparams - 1] = d->u.fn.variadic_name;
            }
        }
        IrGen g = { arena, fn, &fn->instrs, -1, {0}, {0}, 0, &mx, NULL, 0 };
        if (d->u.fn.body) gen_block(&g, d->u.fn.body);
        emit(&g, IR_RET);
        IrFunc **slot = ch_vec_push(&funcs);
        *slot = fn;
    }

    /* 隐式主函数：承载模块级语句 */
    bool need_main = false;
    for (int i = 0; i < ndecls; i++) {
        Decl *d = decls[i]->as.decl;
        if (d->kind == DECL_VAR || d->kind == DECL_CONST || d->kind == DECL_STMT) {
            need_main = true;
            break;
        }
    }
    if (need_main) {
        IrFunc *main_fn = new_func(arena, "主", "__ch_main",
                                   ch_type_get(arena, TY_VOID));
        IrGen g = { arena, main_fn, &main_fn->instrs, -1, {0}, {0}, 0, &mx, NULL, 0 };
        for (int i = 0; i < ndecls; i++) {
            Decl *d = decls[i]->as.decl;
            switch (d->kind) {
            case DECL_VAR:
                if (d->u.var.init) {
                    int v = gen_expr(&g, d->u.var.init);
                    v = coerce_value(&g, v, d->u.var.init->type, d->u.var.annot);
                    IrInstr *ins = emit(&g, IR_STORE);
                    ins->sval = d->name; ins->src1 = v;
                }
                break;
            case DECL_CONST: {
                int v = gen_expr(&g, d->u.konst.init);
                IrInstr *ins = emit(&g, IR_STORE);
                ins->sval = d->name; ins->src1 = v;
                break;
            }
            case DECL_STMT:
                gen_stmt(&g, d->u.stmt);
                break;
            default:
                break;
            }
        }
        emit(&g, IR_RET);
        IrFunc **slot = ch_vec_push(&funcs);
        *slot = main_fn;
        mod->entry_sym = "__ch_main";
    }

    /* 追加提升的匿名函数/包装函数 */
    for (size_t k = 0; k < mx.extra_funcs.len; k++) {
        IrFunc **slot = ch_vec_push(&funcs);
        *slot = CH_VEC_AT(mx.extra_funcs, IrFunc *, k);
    }

    mod->nfuncs = (int)funcs.len;
    mod->funcs = ch_arena_alloc(arena, (size_t)(mod->nfuncs ? mod->nfuncs : 1) * sizeof(IrFunc *));
    if (mod->nfuncs) memcpy(mod->funcs, funcs.data, (size_t)mod->nfuncs * sizeof(IrFunc *));
    ch_vec_free(&funcs);
    ch_vec_free(&mx.extra_funcs);
    return mod;
}

/* ==================== 优化：常量折叠 ==================== */

static IrInstr *find_const(IrInstr *instrs, int n, int temp, int before) {
    for (int j = 0; j < before && j < n; j++) {
        if (instrs[j].dst == temp && instrs[j].op == IR_CONST)
            return &instrs[j];
    }
    return NULL;
}

void ch_ir_optimize(ChArena *arena, IrModule *mod) {
    (void)arena;
    for (int f = 0; f < mod->nfuncs; f++) {
        IrFunc *fn = mod->funcs[f];
        IrInstr *instrs = fn->instrs.data;
        int n = (int)fn->instrs.len;
        for (int i = 0; i < n; i++) {
            IrInstr *ins = &instrs[i];
            IrInstr *c1 = (ins->src1 >= 0) ? find_const(instrs, n, ins->src1, i) : NULL;
            IrInstr *c2 = (ins->src2 >= 0) ? find_const(instrs, n, ins->src2, i) : NULL;
            if (!c1 || !c2) continue;
            if (ins->type && ins->type->kind == TY_INT) {
                long long a = c1->ival, b = c2->ival, r = 0;
                bool ok = true;
                switch (ins->op) {
                case IR_ADD: r = a + b; break;
                case IR_SUB: r = a - b; break;
                case IR_MUL: r = a * b; break;
                case IR_DIV: if (b) r = a / b; else ok = false; break;
                case IR_MOD: if (b) r = a % b; else ok = false; break;
                case IR_POW: { long long p = 1; for (long long k = 0; k < b; k++) p *= a; r = p; break; }
                default: ok = false;
                }
                if (ok) {
                    ins->op = IR_CONST;
                    ins->ival = r;
                    ins->src1 = ins->src2 = -1;
                }
            } else if (ins->type && ins->type->kind == TY_FLOAT) {
                /* 整数常量参与小数运算时，要按值取整数（其 fval 并未填写） */
                double a = (c1->type && c1->type->kind == TY_INT) ? (double)c1->ival : c1->fval;
                double b = (c2->type && c2->type->kind == TY_INT) ? (double)c2->ival : c2->fval;
                double r = 0;
                bool ok = true;
                switch (ins->op) {
                case IR_ADD: r = a + b; break;
                case IR_SUB: r = a - b; break;
                case IR_MUL: r = a * b; break;
                case IR_DIV: if (b != 0.0) r = a / b; else ok = false; break;
                default: ok = false;
                }
                if (ok) {
                    ins->op = IR_CONST;
                    ins->fval = r;
                    ins->src1 = ins->src2 = -1;
                }
            }
        }
    }
}

/* ==================== Dump ==================== */

static void dump_value(int v, FILE *out) {
    if (v < 0) fprintf(out, "_");
    else fprintf(out, "%%%d", v);
}

void ch_ir_dump(const IrModule *mod, FILE *out) {
    for (int k = 0; k < mod->nglobals; k++)
        fprintf(out, "全局 %s (%s)\n", mod->globals[k].name,
                mod->globals[k].type ? ch_type_name(mod->globals[k].type) : "未知");
    for (int f = 0; f < mod->nfuncs; f++) {
        IrFunc *fn = mod->funcs[f];
        fprintf(out, "函数 %s(%d 参数) -> %s\n", fn->name, fn->nparams,
                fn->ret_type ? ch_type_name(fn->ret_type) : "空");
        IrInstr *instrs = fn->instrs.data;
        for (size_t i = 0; i < fn->instrs.len; i++) {
            IrInstr *ins = &instrs[i];
            fprintf(out, "  ");
            switch (ins->op) {
            case IR_NOP: fprintf(out, "nop"); break;
            case IR_CONST:
                fprintf(out, "%%%d = ", ins->dst);
                if (ins->type && ins->type->kind == TY_FLOAT) fprintf(out, "%g", ins->fval);
                else if (ins->type && ins->type->kind == TY_STRING) fprintf(out, "\"%s\"", ins->sval ? ins->sval : "");
                else fprintf(out, "%lld", ins->ival);
                break;
            case IR_LOAD:  fprintf(out, "%%%d = 加载 %s", ins->dst, ins->sval); break;
            case IR_STORE: fprintf(out, "存储 %s = ", ins->sval); dump_value(ins->src1, out); break;
            case IR_MOV:   fprintf(out, "%%%d = ", ins->dst); dump_value(ins->src1, out); break;
            case IR_ADD: fprintf(out, "%%%d = ", ins->dst); dump_value(ins->src1, out); fprintf(out, " + "); dump_value(ins->src2, out); break;
            case IR_SUB: fprintf(out, "%%%d = ", ins->dst); dump_value(ins->src1, out); fprintf(out, " - "); dump_value(ins->src2, out); break;
            case IR_MUL: fprintf(out, "%%%d = ", ins->dst); dump_value(ins->src1, out); fprintf(out, " * "); dump_value(ins->src2, out); break;
            case IR_DIV: fprintf(out, "%%%d = ", ins->dst); dump_value(ins->src1, out); fprintf(out, " / "); dump_value(ins->src2, out); break;
            case IR_MOD: fprintf(out, "%%%d = ", ins->dst); dump_value(ins->src1, out); fprintf(out, " %% "); dump_value(ins->src2, out); break;
            case IR_POW: fprintf(out, "%%%d = ", ins->dst); dump_value(ins->src1, out); fprintf(out, " ^ "); dump_value(ins->src2, out); break;
            case IR_NEG: fprintf(out, "%%%d = -", ins->dst); dump_value(ins->src1, out); break;
            case IR_ITOF: fprintf(out, "%%%d = 转小数(", ins->dst); dump_value(ins->src1, out); fprintf(out, ")"); break;
            case IR_NOT: fprintf(out, "%%%d = !", ins->dst); dump_value(ins->src1, out); break;
            case IR_EQ: fprintf(out, "%%%d = ", ins->dst); dump_value(ins->src1, out); fprintf(out, " == "); dump_value(ins->src2, out); break;
            case IR_NE: fprintf(out, "%%%d = ", ins->dst); dump_value(ins->src1, out); fprintf(out, " != "); dump_value(ins->src2, out); break;
            case IR_LT: fprintf(out, "%%%d = ", ins->dst); dump_value(ins->src1, out); fprintf(out, " < "); dump_value(ins->src2, out); break;
            case IR_LE: fprintf(out, "%%%d = ", ins->dst); dump_value(ins->src1, out); fprintf(out, " <= "); dump_value(ins->src2, out); break;
            case IR_GT: fprintf(out, "%%%d = ", ins->dst); dump_value(ins->src1, out); fprintf(out, " > "); dump_value(ins->src2, out); break;
            case IR_GE: fprintf(out, "%%%d = ", ins->dst); dump_value(ins->src1, out); fprintf(out, " >= "); dump_value(ins->src2, out); break;
            case IR_AND: fprintf(out, "%%%d = ", ins->dst); dump_value(ins->src1, out); fprintf(out, " && "); dump_value(ins->src2, out); break;
            case IR_OR:  fprintf(out, "%%%d = ", ins->dst); dump_value(ins->src1, out); fprintf(out, " || "); dump_value(ins->src2, out); break;
            case IR_CALL:
                if (ins->dst >= 0) fprintf(out, "%%%d = ", ins->dst);
                fprintf(out, "调用 %s(", ins->sval ? ins->sval : "?");
                for (int k = 0; k < ins->nargs; k++) { if (k) fprintf(out, ", "); dump_value(ins->args[k], out); }
                fprintf(out, ")");
                break;
            case IR_PRINT: fprintf(out, "打印 "); dump_value(ins->src1, out); break;
            case IR_RET:
                fprintf(out, "返回");
                if (ins->src1 >= 0) { fprintf(out, " "); dump_value(ins->src1, out); }
                break;
            case IR_JUMP: fprintf(out, "跳转 L%d", ins->label); break;
            case IR_BRANCH:
                fprintf(out, "如果 "); dump_value(ins->src1, out);
                fprintf(out, " 跳转 L%d", ins->label);
                break;
            case IR_LABEL: fprintf(out, "L%d:", ins->label); break;
            case IR_INDEX_GET: fprintf(out, "%%%d = ", ins->dst); dump_value(ins->src1, out); fprintf(out, "["); dump_value(ins->src2, out); fprintf(out, "]"); break;
            case IR_INDEX_SET: dump_value(ins->src1, out); fprintf(out, "["); dump_value(ins->src2, out); fprintf(out, "] = "); dump_value(ins->dst, out); break;
            case IR_LIST_NEW:
                fprintf(out, "%%%d = 列表[", ins->dst);
                for (int k = 0; k < ins->nargs; k++) { if (k) fprintf(out, ", "); dump_value(ins->args[k], out); }
                fprintf(out, "]");
                break;
            case IR_CALL_IND:
                if (ins->dst >= 0) fprintf(out, "%%%d = ", ins->dst);
                fprintf(out, "间接调用 ");
                dump_value(ins->src1, out);
                fprintf(out, "(");
                for (int k = 0; k < ins->nargs; k++) { if (k) fprintf(out, ", "); dump_value(ins->args[k], out); }
                fprintf(out, ")");
                break;
            case IR_DICT_NEW: fprintf(out, "%%%d = 字典{}", ins->dst); break;
            case IR_DICT_GET:
                fprintf(out, "%%%d = ", ins->dst); dump_value(ins->src1, out);
                fprintf(out, "{"); dump_value(ins->src2, out); fprintf(out, "}");
                break;
            case IR_DICT_SET:
                dump_value(ins->src1, out);
                fprintf(out, "{"); dump_value(ins->src2, out); fprintf(out, "} = ");
                dump_value(ins->dst, out);
                break;
            case IR_LAMBDA_NEW:
                fprintf(out, "%%%d = 闭包 %s[", ins->dst, ins->sval ? ins->sval : "?");
                for (int k = 0; k < ins->nargs; k++) { if (k) fprintf(out, ", "); dump_value(ins->args[k], out); }
                fprintf(out, "]");
                break;
            case IR_ENV_LOAD:
                fprintf(out, "%%%d = 环境[%lld]", ins->dst, ins->ival);
                break;
            }
            fputc('\n', out);
        }
    }
}
