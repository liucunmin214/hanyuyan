#include "backend/x64.h"
#include "ir/ir.h"
#include "obj/obj.h"
#include "sema/builtins.h"
#include <string.h>
#include <stdio.h>

/* 寄存器编号 */
enum { RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSP = 4, RBP = 5, RSI = 6, RDI = 7, R8 = 8, R9 = 9 };

/* 实参寄存器（Microsoft x64） */
static const int ARG_GPR[4] = { RCX, RDX, R8, R9 };

typedef struct {
    const char *name;
    int         slot;
} SlotEnt;

typedef struct {
    int      label;
    uint32_t pos;      /* 段内偏移（disp32 字段位置） */
} JumpFix;

typedef struct {
    ChArena    *arena;
    ObjModule  *obj;
    IrModule   *irm;

    IrFunc     *fn;
    ChBytes    *text;
    int         ntemps;
    int         nslot;
    ChVec       locals;    /* SlotEnt */
    ChVec       fixups;    /* JumpFix */
    int        *labels;
    int         nlabels;
    int         frame;
    int         max_stack_args;
} Cg;

/* ==================== 指令编码辅助 ==================== */

static void rex(ChBytes *b, int w, int r, int x, int bb) {
    unsigned char v = (unsigned char)(0x40 | (w ? 8 : 0) | (r ? 4 : 0) | (x ? 2 : 0) | (bb ? 1 : 0));
    ch_bytes_u8(b, v);
}

static void modrm(ChBytes *b, int mod, int reg, int rm) {
    ch_bytes_u8(b, (unsigned char)((mod << 6) | ((reg & 7) << 3) | (rm & 7)));
}

static void mov_rr(ChBytes *b, int dst, int src) {
    rex(b, 1, (src >> 3) & 1, 0, (dst >> 3) & 1);
    ch_bytes_u8(b, 0x89);
    modrm(b, 3, src, dst);
}

static void mov_ri64(ChBytes *b, int dst, uint64_t v) {
    rex(b, 1, 0, 0, (dst >> 3) & 1);
    ch_bytes_u8(b, (unsigned char)(0xB8 + (dst & 7)));
    ch_bytes_u64(b, v);
}

static void mov_r_mem(ChBytes *b, int dst, int32_t disp) {
    rex(b, 1, (dst >> 3) & 1, 0, 0);
    ch_bytes_u8(b, 0x8B);
    modrm(b, 2, dst, 5);
    ch_bytes_i32(b, disp);
}

static void mov_mem_r(ChBytes *b, int src, int32_t disp) {
    rex(b, 1, (src >> 3) & 1, 0, 0);
    ch_bytes_u8(b, 0x89);
    modrm(b, 2, src, 5);
    ch_bytes_i32(b, disp);
}

static void mov_r_mem_rsp(ChBytes *b, int src, int32_t disp) {
    rex(b, 1, (src >> 3) & 1, 0, 0);
    ch_bytes_u8(b, 0x89);
    modrm(b, 2, src, 4);
    ch_bytes_u8(b, 0x24);
    ch_bytes_i32(b, disp);
}

/* mov dst, [base + disp32]（base 限 RAX/RCX/RDX/R8 等非 RSP 寄存器） */
static void mov_r_base(ChBytes *b, int dst, int base, int32_t disp) {
    rex(b, 1, (dst >> 3) & 1, 0, (base >> 3) & 1);
    ch_bytes_u8(b, 0x8B);
    modrm(b, 2, dst, base);
    ch_bytes_i32(b, disp);
}

/* mov [base + disp32], src */
static void mov_base_r(ChBytes *b, int base, int32_t disp, int src) {
    rex(b, 1, (src >> 3) & 1, 0, (base >> 3) & 1);
    ch_bytes_u8(b, 0x89);
    modrm(b, 2, src, base);
    ch_bytes_i32(b, disp);
}

/* call rax（间接调用闭包函数指针） */
static void call_rax(ChBytes *b) {
    ch_bytes_u8(b, 0xFF);
    modrm(b, 3, 2, RAX);
}

/* RIP 相对 */
static void rip_reloc(Cg *c, RelType type, const char *sym, int64_t addend) {
    ch_obj_reloc(c->obj, SEC_TEXT, (uint32_t)c->text->len, type, sym, addend);
    ch_bytes_i32(c->text, 0);
}

static void lea_rip(ChBytes *b, int dst) {
    rex(b, 1, (dst >> 3) & 1, 0, 0);
    ch_bytes_u8(b, 0x8D);
    modrm(b, 0, dst, 5);
}

static void mov_r_rip(ChBytes *b, int dst) {
    rex(b, 1, (dst >> 3) & 1, 0, 0);
    ch_bytes_u8(b, 0x8B);
    modrm(b, 0, dst, 5);
}

static void mov_rip_r(ChBytes *b, int src) {
    rex(b, 1, (src >> 3) & 1, 0, 0);
    ch_bytes_u8(b, 0x89);
    modrm(b, 0, src, 5);
}

static void alu_rr(ChBytes *b, unsigned char opcode, int dst, int src) {
    rex(b, 1, (src >> 3) & 1, 0, (dst >> 3) & 1);
    ch_bytes_u8(b, opcode);
    modrm(b, 3, src, dst);
}

static void call_sym(Cg *c, const char *sym) {
    ch_bytes_u8(c->text, 0xE8);
    ch_obj_reloc(c->obj, SEC_TEXT, (uint32_t)c->text->len, REL_REL32, sym, 0);
    ch_bytes_i32(c->text, 0);
}

static void jmp_label(Cg *c, int label) {
    ch_bytes_u8(c->text, 0xE9);
    JumpFix *f = CH_VEC_PUSH(c->fixups, JumpFix);
    f->label = label;
    f->pos = (uint32_t)c->text->len;
    ch_bytes_i32(c->text, 0);
}

static void jcc_label(Cg *c, unsigned char cc, int label) {
    ch_bytes_u8(c->text, 0x0F);
    ch_bytes_u8(c->text, cc);
    JumpFix *f = CH_VEC_PUSH(c->fixups, JumpFix);
    f->label = label;
    f->pos = (uint32_t)c->text->len;
    ch_bytes_i32(c->text, 0);
}

static void setcc_al(ChBytes *b, unsigned char cc) {
    ch_bytes_u8(b, 0x0F);
    ch_bytes_u8(b, cc);
    modrm(b, 3, 0, 0);
    /* movzx eax, al */
    ch_bytes_u8(b, 0x0F);
    ch_bytes_u8(b, 0xB6);
    ch_bytes_u8(b, 0xC0);
}

/* SSE 指令（固定 xmm0/xmm1） */
static void movq_xmm_rax(ChBytes *b) {
    ch_bytes_u8(b, 0x66); rex(b, 1, 0, 0, 0);
    ch_bytes_u8(b, 0x0F); ch_bytes_u8(b, 0x6E); ch_bytes_u8(b, 0xC0);
}
static void movq_rax_xmm(ChBytes *b) {
    ch_bytes_u8(b, 0x66); rex(b, 1, 0, 0, 0);
    ch_bytes_u8(b, 0x0F); ch_bytes_u8(b, 0x7E); ch_bytes_u8(b, 0xC0);
}
static void xmm_bin(ChBytes *b, unsigned char op) {
    ch_bytes_u8(b, 0xF2);
    ch_bytes_u8(b, 0x0F);
    ch_bytes_u8(b, op);
    ch_bytes_u8(b, 0xC1);   /* xmm0, xmm1 */
}
static void ucomisd(ChBytes *b) {
    ch_bytes_u8(b, 0x66);
    ch_bytes_u8(b, 0x0F);
    ch_bytes_u8(b, 0x2E);
    ch_bytes_u8(b, 0xC1);
}
static void cvtsi2sd_xmm(ChBytes *b, int xreg) {
    ch_bytes_u8(b, 0xF2); rex(b, 1, (xreg >> 3) & 1, 0, 0);
    ch_bytes_u8(b, 0x0F); ch_bytes_u8(b, 0x2A); modrm(b, 3, xreg, RAX);
}
static void movq_xmmN_rax(ChBytes *b, int n) {
    ch_bytes_u8(b, 0x66); rex(b, 1, (n >> 3) & 1, 0, 0);
    ch_bytes_u8(b, 0x0F); ch_bytes_u8(b, 0x6E); modrm(b, 3, n, RAX);
}
static void movq_rax_xmmN(ChBytes *b, int n) {
    ch_bytes_u8(b, 0x66); rex(b, 1, 0, 0, 0);
    ch_bytes_u8(b, 0x0F); ch_bytes_u8(b, 0x7E); modrm(b, 3, RAX, n);
}

/* ==================== 槽与标签 ==================== */

static int32_t slot_disp(int slot) { return (int32_t)(-8 * (slot + 1)); }

static int find_local(Cg *c, const char *name) {
    for (size_t i = 0; i < c->locals.len; i++)
        if (strcmp(CH_VEC_AT(c->locals, SlotEnt, i).name, name) == 0)
            return CH_VEC_AT(c->locals, SlotEnt, i).slot;
    return -1;
}

static int add_local(Cg *c, const char *name) {
    int idx = find_local(c, name);
    if (idx >= 0) return idx;
    SlotEnt *e = CH_VEC_PUSH(c->locals, SlotEnt);
    e->name = name;
    e->slot = c->nslot++;
    return e->slot;
}

static bool is_global(IrModule *m, const char *name) {
    for (int i = 0; i < m->nglobals; i++)
        if (strcmp(m->globals[i].name, name) == 0) return true;
    return false;
}

static const char *global_sym(IrModule *m, const char *name) {
    for (int i = 0; i < m->nglobals; i++)
        if (strcmp(m->globals[i].name, name) == 0) return m->globals[i].sym;
    return name;
}

static IrFunc *find_func(IrModule *m, const char *sym) {
    for (int i = 0; i < m->nfuncs; i++)
        if (strcmp(m->funcs[i]->sym, sym) == 0) return m->funcs[i];
    return NULL;
}

/* ==================== 值装载 ==================== */

static void load_slot(ChBytes *b, int slot, int reg) {
    if (slot < 0) { mov_ri64(b, reg, 0); return; }
    mov_r_mem(b, reg, slot_disp(slot));
}

static void load_slot_xmm(ChBytes *b, int slot, const ChType *t, int xmmreg) {
    mov_r_mem(b, RAX, slot_disp(slot));
    if (t && t->kind == TY_INT) {
        cvtsi2sd_xmm(b, xmmreg);  /* 整数自动提升为小数 */
    } else {
        movq_xmmN_rax(b, xmmreg);
    }
}

/* ==================== 字符串常量池 ==================== */

typedef struct { const char *s; const char *sym; } StrEnt;

static ChVec g_strs;

static const char *intern_string(ObjModule *obj, ChArena *a, const char *s) {
    for (size_t i = 0; i < g_strs.len; i++)
        if (strcmp(CH_VEC_AT(g_strs, StrEnt, i).s, s) == 0)
            return CH_VEC_AT(g_strs, StrEnt, i).sym;
    /* 生成符号名 */
    char name[32];
    snprintf(name, sizeof(name), "ch_str_%zu", g_strs.len);
    const char *sym = ch_arena_strdup(a, name);
    StrEnt *e = CH_VEC_PUSH(g_strs, StrEnt);
    e->s = s;
    e->sym = sym;
    /* .rdata: 8 字节长度 + 字节 + 结尾 0 */
    uint64_t len = (uint64_t)strlen(s);
    ch_obj_define(obj, sym, SEC_RDATA, obj->secs[SEC_RDATA].data.len);
    ch_bytes_u64(&obj->secs[SEC_RDATA].data, len);
    ch_bytes_append(&obj->secs[SEC_RDATA].data, s, (size_t)len);
    ch_bytes_u8(&obj->secs[SEC_RDATA].data, 0);
    /* 8 字节对齐 */
    while (obj->secs[SEC_RDATA].data.len % 8) ch_bytes_u8(&obj->secs[SEC_RDATA].data, 0);
    return sym;
}

/* ==================== 单条指令生成 ==================== */

static void gen_instr(Cg *c, IrInstr *ins) {
    ChBytes *b = c->text;
    switch (ins->op) {
    case IR_NOP:
        break;
    case IR_CONST:
        if (ins->type && ins->type->kind == TY_STRING) {
            const char *sym = intern_string(c->obj, c->arena, ins->sval ? ins->sval : "");
            lea_rip(b, RAX);
            rip_reloc(c, REL_REL32, sym, 0);
            mov_mem_r(b, RAX, slot_disp(ins->dst));
        } else if (ins->type && ins->type->kind == TY_FLOAT) {
            union { double d; uint64_t u; } v;
            v.d = ins->fval;
            mov_ri64(b, RAX, v.u);
            mov_mem_r(b, RAX, slot_disp(ins->dst));
        } else {
            mov_ri64(b, RAX, (uint64_t)ins->ival);
            mov_mem_r(b, RAX, slot_disp(ins->dst));
        }
        break;
    case IR_LOAD: {
        if (is_global(c->irm, ins->sval)) {
            mov_r_rip(b, RAX);
            rip_reloc(c, REL_REL32, global_sym(c->irm, ins->sval), 0);
        } else {
            int slot = find_local(c, ins->sval);
            if (slot < 0) slot = add_local(c, ins->sval);
            load_slot(b, slot, RAX);
        }
        mov_mem_r(b, RAX, slot_disp(ins->dst));
        break;
    }
    case IR_STORE: {
        load_slot(b, ins->src1, RAX);
        if (is_global(c->irm, ins->sval)) {
            mov_rip_r(b, RAX);
            ch_obj_reloc(c->obj, SEC_TEXT, (uint32_t)b->len, REL_REL32,
                         global_sym(c->irm, ins->sval), 0);
            ch_bytes_i32(b, 0);
        } else {
            int slot = find_local(c, ins->sval);
            if (slot < 0) slot = add_local(c, ins->sval);
            mov_mem_r(b, RAX, slot_disp(slot));
        }
        break;
    }
    case IR_MOV:
        load_slot(b, ins->src1, RAX);
        mov_mem_r(b, RAX, slot_disp(ins->dst));
        break;
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_POW: {
        bool isstr = ins->type && ins->type->kind == TY_STRING;
        if (isstr) {
            /* 文字 + 文字：调用运行时拼接 */
            load_slot(b, ins->src1, RCX);
            load_slot(b, ins->src2, RDX);
            call_sym(c, "rt_str_concat");
            mov_mem_r(b, RAX, slot_disp(ins->dst));
            break;
        }
        bool isfloat = ins->type && ins->type->kind == TY_FLOAT;
        if (isfloat) {
            load_slot_xmm(b, ins->src1, ins->opnd_type, 0);
            load_slot_xmm(b, ins->src2, ins->opnd2_type, 1);
            if (ins->op == IR_ADD) xmm_bin(b, 0x58);
            else if (ins->op == IR_SUB) xmm_bin(b, 0x5C);
            else if (ins->op == IR_MUL) xmm_bin(b, 0x59);
            else if (ins->op == IR_DIV) xmm_bin(b, 0x5E);
            else {
                /* 小数取余/幂：调用运行时，结果在 xmm0 */
                call_sym(c, ins->op == IR_MOD ? "rt_mod_f64" : "rt_pow_f64");
                movq_rax_xmm(b);
                mov_mem_r(b, RAX, slot_disp(ins->dst));
                break;
            }
            movq_rax_xmm(b);
            mov_mem_r(b, RAX, slot_disp(ins->dst));
            break;
        }
        load_slot(b, ins->src1, RAX);
        load_slot(b, ins->src2, RCX);
        switch (ins->op) {
        case IR_ADD: alu_rr(b, 0x01, RAX, RCX); break;
        case IR_SUB: alu_rr(b, 0x29, RAX, RCX); break;
        case IR_MUL:
            rex(b, 1, 0, 0, (RCX >> 3) & 1);
            ch_bytes_u8(b, 0x0F); ch_bytes_u8(b, 0xAF);
            modrm(b, 3, RAX, RCX);
            break;
        case IR_DIV:
        case IR_MOD:
            /* cqo; idiv rcx，商在 rax、余数在 rdx */
            rex(b, 1, 0, 0, 0);
            ch_bytes_u8(b, 0x99);
            rex(b, 1, 0, 0, (RCX >> 3) & 1);
            ch_bytes_u8(b, 0xF7); modrm(b, 3, 7, RCX);
            if (ins->op == IR_MOD) mov_rr(b, RAX, RDX);
            break;
        case IR_POW:
            mov_rr(b, RCX, RAX);
            load_slot(b, ins->src2, RDX);
            call_sym(c, "rt_pow_i64");
            break;
        default: break;
        }
        mov_mem_r(b, RAX, slot_disp(ins->dst));
        break;
    }
    case IR_ITOF:
        /* 整数 -> double：cvtsi2sd xmm0, rax，再把位模式存回槽 */
        load_slot(b, ins->src1, RAX);
        cvtsi2sd_xmm(b, 0);
        movq_rax_xmm(b);
        mov_mem_r(b, RAX, slot_disp(ins->dst));
        break;
    case IR_NEG: {
        bool isfloat = ins->type && ins->type->kind == TY_FLOAT;
        load_slot(b, ins->src1, RAX);
        if (isfloat) {
            /* 取反符号位：rax ^= 0x8000000000000000 */
            mov_ri64(b, RCX, 0x8000000000000000ULL);
            alu_rr(b, 0x31, RAX, RCX); /* xor rax, rcx */
        } else {
            rex(b, 1, 0, 0, (RAX >> 3) & 1);
            ch_bytes_u8(b, 0xF7); modrm(b, 3, 3, RAX);
        }
        mov_mem_r(b, RAX, slot_disp(ins->dst));
        break;
    }
    case IR_NOT:
        load_slot(b, ins->src1, RAX);
        rex(b, 1, 0, 0, 0);
        ch_bytes_u8(b, 0x85); modrm(b, 3, RAX, RAX);
        setcc_al(b, 0x94); /* sete */
        mov_mem_r(b, RAX, slot_disp(ins->dst));
        break;
    case IR_EQ: case IR_NE: case IR_LT: case IR_LE: case IR_GT: case IR_GE: {
        bool isstr = ins->opnd_type && ins->opnd_type->kind == TY_STRING &&
                     (ins->op == IR_EQ || ins->op == IR_NE);
        if (isstr) {
            /* 文字相等：按内容比较 */
            load_slot(b, ins->src1, RCX);
            load_slot(b, ins->src2, RDX);
            call_sym(c, "rt_str_eq");
            if (ins->op == IR_NE) {
                ch_bytes_u8(b, 0x83); ch_bytes_u8(b, 0xF0); ch_bytes_u8(b, 0x01); /* xor eax, 1 */
            }
            mov_mem_r(b, RAX, slot_disp(ins->dst));
            break;
        }
        bool isfloat = (ins->opnd_type && ins->opnd_type->kind == TY_FLOAT) ||
                       (ins->opnd2_type && ins->opnd2_type->kind == TY_FLOAT);
        unsigned char cc;
        if (isfloat) {
            load_slot_xmm(b, ins->src1, ins->opnd_type, 0);
            load_slot_xmm(b, ins->src2, ins->opnd2_type, 1);
            ucomisd(b);
            switch (ins->op) {
            case IR_EQ: cc = 0x94; break;
            case IR_NE: cc = 0x95; break;
            case IR_LT: cc = 0x92; break; /* setb */
            case IR_LE: cc = 0x96; break; /* setbe */
            case IR_GT: cc = 0x97; break; /* seta */
            default:    cc = 0x93; break; /* setae */
            }
        } else {
            load_slot(b, ins->src1, RAX);
            load_slot(b, ins->src2, RCX);
            alu_rr(b, 0x39, RAX, RCX); /* cmp rax, rcx */
            switch (ins->op) {
            case IR_EQ: cc = 0x94; break;
            case IR_NE: cc = 0x95; break;
            case IR_LT: cc = 0x9C; break; /* setl */
            case IR_LE: cc = 0x9E; break; /* setle */
            case IR_GT: cc = 0x9F; break; /* setg */
            default:    cc = 0x9D; break; /* setge */
            }
        }
        setcc_al(b, cc);
        mov_mem_r(b, RAX, slot_disp(ins->dst));
        break;
    }
    case IR_AND:
        load_slot(b, ins->src1, RAX);
        load_slot(b, ins->src2, RCX);
        alu_rr(b, 0x21, RAX, RCX);
        mov_mem_r(b, RAX, slot_disp(ins->dst));
        break;
    case IR_OR:
        load_slot(b, ins->src1, RAX);
        load_slot(b, ins->src2, RCX);
        alu_rr(b, 0x09, RAX, RCX);
        mov_mem_r(b, RAX, slot_disp(ins->dst));
        break;
    case IR_CALL: {
        IrFunc *callee = find_func(c->irm, ins->sval);
        int n = ins->nargs;
        for (int i = 0; i < n; i++) {
            /* 只有用户函数按形参类型决定；运行时函数除少数小数参数外都用整数寄存器 */
            bool farg = false;
            if (callee) {
                farg = callee->param_types && i < callee->nparams &&
                       callee->param_types[i] && callee->param_types[i]->kind == TY_FLOAT;
            } else if (ins->sval && i == 0) {
                farg = strcmp(ins->sval, "rt_float_to_str") == 0 ||
                       strcmp(ins->sval, "rt_print_float") == 0;
            }
            if (i >= 4) {
                load_slot(b, ins->args[i], RAX);
                mov_r_mem_rsp(b, RAX, 32 + 8 * (i - 4));
            } else if (farg) {
                load_slot_xmm(b, ins->args[i], NULL, i);
            } else {
                load_slot(b, ins->args[i], ARG_GPR[i]);
            }
        }
        call_sym(c, ins->sval);
        if (ins->dst >= 0) {
            if (ins->type && ins->type->kind == TY_FLOAT) movq_rax_xmm(b);
            mov_mem_r(b, RAX, slot_disp(ins->dst));
        }
        break;
    }
    case IR_PRINT: {
        ChType *t = ins->type;
        bool isfloat = t && t->kind == TY_FLOAT;
        const char *sym = ch_builtin_print_sym(t);
        if (isfloat) {
            load_slot_xmm(b, ins->src1, t, 0);
        } else {
            load_slot(b, ins->src1, RCX);
        }
        call_sym(c, sym);
        break;
    }
    case IR_RET:
        if (ins->src1 >= 0) {
            load_slot(b, ins->src1, RAX);
            /* 返回小数的函数：结果放 xmm0 */
            if (c->fn->ret_type && c->fn->ret_type->kind == TY_FLOAT) {
                /* 值在 rax 位模式 */
                movq_xmm_rax(b);
            }
        } else {
            rex(b, 1, 0, 0, 0);
            ch_bytes_u8(b, 0x31); ch_bytes_u8(b, 0xC0); /* xor eax, eax */
        }
        jmp_label(c, c->nlabels - 1);
        break;
    case IR_JUMP:
        jmp_label(c, ins->label);
        break;
    case IR_BRANCH:
        load_slot(b, ins->src1, RAX);
        rex(b, 1, 0, 0, 0);
        ch_bytes_u8(b, 0x85); modrm(b, 3, RAX, RAX);
        jcc_label(c, 0x84, ins->label); /* je：为假跳转 */
        break;
    case IR_LABEL:
        c->labels[ins->label] = (int)b->len;
        break;
    case IR_INDEX_GET:
        load_slot(b, ins->src1, RCX);
        load_slot(b, ins->src2, RDX);
        call_sym(c, "rt_list_get");
        mov_mem_r(b, RAX, slot_disp(ins->dst));
        break;
    case IR_INDEX_SET:
        load_slot(b, ins->src1, RCX);
        load_slot(b, ins->src2, RDX);
        load_slot(b, ins->dst, RAX);
        mov_rr(b, R8, RAX);
        call_sym(c, "rt_list_set");
        break;
    case IR_LIST_NEW: {
        call_sym(c, "rt_list_new");
        mov_mem_r(b, RAX, slot_disp(ins->dst));
        for (int i = 0; i < ins->nargs; i++) {
            mov_r_mem(b, RCX, slot_disp(ins->dst));
            load_slot(b, ins->args[i], RDX);
            call_sym(c, "rt_list_push");
        }
        break;
    }
    case IR_DICT_NEW:
        call_sym(c, "rt_dict_new");
        mov_mem_r(b, RAX, slot_disp(ins->dst));
        break;
    case IR_DICT_GET:
        load_slot(b, ins->src1, RCX);
        load_slot(b, ins->src2, RDX);
        call_sym(c, "rt_dict_get");
        mov_mem_r(b, RAX, slot_disp(ins->dst));
        break;
    case IR_DICT_SET:
        load_slot(b, ins->src1, RCX);
        load_slot(b, ins->src2, RDX);
        load_slot(b, ins->dst, RAX);
        mov_rr(b, R8, RAX);
        call_sym(c, "rt_dict_set");
        break;
    case IR_ENV_LOAD: {
        /* rax = 环境指针（__env 形参槽）；rax = [rax + 8*ival] */
        int slot = find_local(c, "__env");
        load_slot(b, slot, RAX);
        mov_r_base(b, RAX, RAX, (int32_t)(8 * ins->ival));
        mov_mem_r(b, RAX, slot_disp(ins->dst));
        break;
    }
    case IR_LAMBDA_NEW: {
        int env_slot = find_local(c, "__clos_env");
        if (env_slot < 0) env_slot = add_local(c, "__clos_env");
        if (ins->nargs > 0) {
            /* 环境：ncap*8 字节，按序存放捕获值 */
            mov_ri64(b, RCX, (uint64_t)(8 * ins->nargs));
            call_sym(c, "rt_alloc");
            mov_mem_r(b, RAX, slot_disp(env_slot));
            for (int k = 0; k < ins->nargs; k++) {
                load_slot(b, ins->args[k], RDX);
                mov_r_mem(b, RAX, slot_disp(env_slot));
                mov_base_r(b, RAX, 8 * k, RDX);
            }
        }
        /* 闭包对象 16 字节：{函数指针, 环境指针} */
        mov_ri64(b, RCX, 16);
        call_sym(c, "rt_alloc");
        if (ins->nargs > 0) {
            mov_r_mem(b, RCX, slot_disp(env_slot));
        } else {
            alu_rr(b, 0x31, RCX, RCX);   /* xor rcx, rcx：无捕获环境为 0 */
        }
        mov_base_r(b, RAX, 8, RCX);      /* [rax+8] = 环境 */
        lea_rip(b, RCX);
        rip_reloc(c, REL_REL32, ins->sval, 0);
        mov_base_r(b, RAX, 0, RCX);      /* [rax+0] = 函数指针 */
        mov_mem_r(b, RAX, slot_disp(ins->dst));
        break;
    }
    case IR_CALL_IND: {
        /* 闭包调用约定：隐藏第 0 实参 rcx=环境，用户实参顺移到位置 1..3 */
        int n = ins->nargs;
        /* 第 5 个及以后的位置（用户实参下标 >=3）先上栈 */
        for (int i = n - 1; i >= 3; i--) {
            load_slot(b, ins->args[i], RAX);
            mov_r_mem_rsp(b, RAX, 32 + 8 * (i + 1 - 4));
        }
        /* rcx = [闭包+8] 环境指针 */
        load_slot(b, ins->src1, RAX);
        mov_r_base(b, RCX, RAX, 8);
        /* 用户实参放位置 1..3：rdx/r8/r9，小数走 xmm1-3 */
        for (int i = 0; i < n && i < 3; i++) {
            bool farg = ins->arg_types && ins->arg_types[i] &&
                        ins->arg_types[i]->kind == TY_FLOAT;
            if (farg) load_slot_xmm(b, ins->args[i], ins->arg_types[i], i + 1);
            else load_slot(b, ins->args[i], ARG_GPR[i + 1]);
        }
        /* rax = [闭包+0] 函数指针；call rax */
        load_slot(b, ins->src1, RAX);
        mov_r_base(b, RAX, RAX, 0);
        call_rax(b);
        if (ins->dst >= 0) {
            if (ins->type && ins->type->kind == TY_FLOAT) movq_rax_xmm(b);
            mov_mem_r(b, RAX, slot_disp(ins->dst));
        }
        break;
    }
    }
}

/* ==================== 函数生成 ==================== */

static void collect_locals(Cg *c) {
    IrInstr *is = c->fn->instrs.data;
    int n = (int)c->fn->instrs.len;
    for (int i = 0; i < n; i++) {
        if ((is[i].op == IR_LOAD || is[i].op == IR_STORE) && is[i].sval) {
            if (!is_global(c->irm, is[i].sval)) add_local(c, is[i].sval);
        }
    }
    /* 参数：按名分配栈槽，供序言落寄存器 */
    for (int i = 0; i < c->fn->nparams; i++) {
        const char *pn = c->fn->param_names ? c->fn->param_names[i] : NULL;
        if (pn) add_local(c, pn);
    }
    /* 闭包环境暂存槽（新建闭包时复用） */
    add_local(c, "__clos_env");
}

static void gen_function(Cg *c, IrFunc *fn) {
    memset(&c->locals, 0, sizeof(c->locals));
    ch_vec_init(&c->locals, sizeof(SlotEnt));
    ch_vec_init(&c->fixups, sizeof(JumpFix));
    c->fn = fn;
    c->ntemps = fn->next_temp;
    c->nslot = fn->next_temp;   /* 临时值 0..ntemps-1 */
    c->nlabels = fn->next_label + 2;
    c->labels = ch_arena_alloc(c->arena, (size_t)c->nlabels * sizeof(int));
    for (int i = 0; i < c->nlabels; i++) c->labels[i] = -1;

    collect_locals(c);

    /* 计算最大栈参数个数（间接调用含隐藏环境实参） */
    c->max_stack_args = 0;
    for (size_t i = 0; i < fn->instrs.len; i++) {
        IrInstr *ins = &CH_VEC_AT(fn->instrs, IrInstr, i);
        if (ins->op == IR_CALL && ins->nargs > 4) {
            int n = ins->nargs - 4;
            if (n > c->max_stack_args) c->max_stack_args = n;
        }
        if (ins->op == IR_CALL_IND && ins->nargs + 1 > 4) {
            int n = ins->nargs + 1 - 4;
            if (n > c->max_stack_args) c->max_stack_args = n;
        }
    }
    int base = 8 * c->nslot + 32 + 8 * c->max_stack_args;
    c->frame = (base + 15) & ~15;

    ch_obj_define(c->obj, fn->sym, SEC_TEXT, c->text->len);

    /* 序言 */
    ch_bytes_u8(c->text, 0x55);                 /* push rbp */
    rex(c->text, 1, 0, 0, 0);
    ch_bytes_u8(c->text, 0x89); modrm(c->text, 3, RSP, RBP); /* mov rbp, rsp */
    rex(c->text, 1, 0, 0, 0);
    ch_bytes_u8(c->text, 0x81); modrm(c->text, 3, 5, RSP);
    ch_bytes_i32(c->text, c->frame);            /* sub rsp, frame */

    /* 参数入栈槽：整数走 rcx/rdx/r8/r9，小数走 xmm0-3，超出部分在栈上 */
    for (int i = 0; i < fn->nparams; i++) {
        ChType *pt = fn->param_types ? fn->param_types[i] : NULL;
        const char *pn = fn->param_names ? fn->param_names[i] : NULL;
        if (!pn) continue;
        int slot = find_local(c, pn);
        if (slot < 0) continue;
        bool isfloat = pt && pt->kind == TY_FLOAT;
        if (i < 4) {
            if (isfloat) movq_rax_xmmN(c->text, i);
            else mov_rr(c->text, RAX, ARG_GPR[i]);
        } else {
            mov_r_mem(c->text, RAX, 48 + 8 * (i - 4));
        }
        mov_mem_r(c->text, RAX, slot_disp(slot));
    }

    for (size_t i = 0; i < fn->instrs.len; i++)
        gen_instr(c, &CH_VEC_AT(fn->instrs, IrInstr, i));

    /* 尾声标签 */
    c->labels[c->nlabels - 1] = (int)c->text->len;
    ch_bytes_u8(c->text, 0xC9); /* leave */
    ch_bytes_u8(c->text, 0xC3); /* ret */

    /* 回填跳转 */
    for (size_t i = 0; i < c->fixups.len; i++) {
        JumpFix *f = &CH_VEC_AT(c->fixups, JumpFix, i);
        int32_t disp = c->labels[f->label] - (int32_t)(f->pos + 4);
        memcpy(c->text->data + f->pos, &disp, 4);
    }
    ch_vec_free(&c->locals);
    ch_vec_free(&c->fixups);
}

ObjModule *ch_x64_gen(ChArena *arena, const IrModule *mod) {
    ObjModule *obj = ch_arena_alloc(arena, sizeof(ObjModule));
    ch_obj_init(obj);
    ch_vec_init(&g_strs, sizeof(StrEnt));

    /* 全局变量：各占 8 字节 .data */
    for (int i = 0; i < mod->nglobals; i++) {
        ch_obj_define(obj, mod->globals[i].sym, SEC_DATA, obj->secs[SEC_DATA].data.len);
        ch_bytes_u64(&obj->secs[SEC_DATA].data, 0);
    }

    Cg c;
    memset(&c, 0, sizeof(c));
    c.arena = arena;
    c.obj = obj;
    c.irm = (IrModule *)mod;
    c.text = &obj->secs[SEC_TEXT].data;

    ((IrModule *)mod)->entry_sym = mod->entry_sym;
    for (int i = 0; i < mod->nfuncs; i++)
        gen_function(&c, mod->funcs[i]);

    /* 没有模块级语句时也要有入口：生成一个空主函数 */
    if (!mod->entry_sym) {
        ch_obj_define(obj, "__ch_main", SEC_TEXT, c.text->len);
        rex(c.text, 1, 0, 0, 0);
        ch_bytes_u8(c.text, 0x31); ch_bytes_u8(c.text, 0xC0); /* xor eax, eax */
        ch_bytes_u8(c.text, 0xC3);                            /* ret */
    }
    return obj;
}
