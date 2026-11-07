#include "util/common.h"
#include "util/arena.h"
#include "util/diag.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "sema/sema.h"
#include "ir/ir.h"
#include "backend/x64.h"
#include "coff/coff.h"
#include "link/link.h"
#include <stdio.h>
#include <string.h>

/* 阶段五/六：x86-64 后端、COFF 读取、PE 链接的单元测试。
 * 不执行生成的 exe（端到端执行由 build.ps1 -Test 的样例运行覆盖）。 */

extern const unsigned char ch_runtime_obj[];
extern const unsigned int ch_runtime_obj_len;

static int passed = 0, failed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { passed++; } \
    else { failed++; printf("失败: %s\n", msg); } \
} while (0)

static IrModule *gen(ChArena *a, const char *src) {
    ChDiagList d; ch_diag_init(&d, a);
    ChVec toks; ch_vec_init(&toks, sizeof(Token));
    ch_lex(a, &d, src, strlen(src), &toks);
    int n = 0;
    AstNode **decls = ch_parse(a, &d, toks.data, toks.len, &n);
    ch_sema_check(a, &d, decls, n);
    return ch_ir_gen(a, decls, n);
}

/* ---------- 后端 ---------- */

static void t_x64_emit(void) {
    ChArena *a = ch_arena_new();
    IrModule *m = gen(a, "函数 f() -> 整数\n  返回 42\n结束\n");
    ch_ir_optimize(a, m);
    ObjModule *o = ch_x64_gen(a, m);
    CHECK(o->secs[SEC_TEXT].data.len > 0, "生成 .text 机器码");
    CHECK(ch_obj_find_sym(o, "chf_f") != NULL, "定义用户函数符号 chf_f");
    CHECK(ch_obj_find_sym(o, "chf_f")->defined, "用户函数符号为已定义");
    ch_arena_destroy(a);
}

static void t_x64_entry_stub(void) {
    ChArena *a = ch_arena_new();
    IrModule *m = gen(a, "打印 \"你好\"\n");
    ch_ir_optimize(a, m);
    ObjModule *o = ch_x64_gen(a, m);
    CHECK(m->entry_sym != NULL, "顶层语句生成隐式主函数");
    CHECK(ch_obj_find_sym(o, "__ch_main") != NULL, "存在 __ch_main 符号");
    CHECK(ch_obj_find_sym(o, "__ch_main")->defined, "__ch_main 已定义");
    ch_arena_destroy(a);
}

static void t_x64_call_reloc(void) {
    ChArena *a = ch_arena_new();
    IrModule *m = gen(a, "打印 1\n");
    ch_ir_optimize(a, m);
    ObjModule *o = ch_x64_gen(a, m);
    /* 打印语句应产生一个对 rt_print_int 的重定位 */
    bool found = false;
    for (size_t i = 0; i < o->relocs.len; i++) {
        ObjReloc *r = &CH_VEC_AT(o->relocs, ObjReloc, i);
        if (strcmp(r->sym, "rt_print_int") == 0) found = true;
    }
    CHECK(found, "打印语句产生对 rt_print_int 的重定位");
    ch_arena_destroy(a);
}

/* ---------- COFF 读取 ---------- */

static ObjModule *read_runtime(ChArena *a) {
    ObjModule *rt = ch_arena_alloc(a, sizeof(ObjModule));
    char err[256] = {0};
    if (!ch_coff_read(a, ch_runtime_obj, ch_runtime_obj_len, rt, err, sizeof(err))) {
        printf("失败: 读取内嵌运行时目标文件：%s\n", err);
        failed++;
        return NULL;
    }
    passed++;
    return rt;
}

static void t_coff_sections(ObjModule *rt) {
    CHECK(rt != NULL, "内嵌运行时读取成功");
    if (!rt) return;
    CHECK(rt->secs[SEC_TEXT].data.len > 0, "运行时 .text 非空");
    CHECK(rt->secs[SEC_RDATA].data.len > 0, "运行时 .rdata 非空");
    ObjSym *e = ch_obj_find_sym(rt, "rt_entry");
    CHECK(e && e->defined && e->sec == SEC_TEXT, "rt_entry 定义在 .text");
    ObjSym *s = ch_obj_find_sym(rt, "rt_print_int");
    CHECK(s && s->defined, "rt_print_int 已定义");
    ObjSym *imp = ch_obj_find_sym(rt, "__imp_GetStdHandle");
    CHECK(imp && !imp->defined, "__imp_GetStdHandle 为外部导入");
}

static void t_coff_relocs(ObjModule *rt) {
    if (!rt) return;
    CHECK(rt->relocs.len > 20, "运行时重定位数量合理");
    int bad = 0;
    for (size_t i = 0; i < rt->relocs.len; i++) {
        ObjReloc *r = &CH_VEC_AT(rt->relocs, ObjReloc, i);
        if (r->type == REL_REL32 && r->addend != 0 && r->addend != 0x30) {
            /* 导入调用的 addend 必须为 0；.rdata 引用允许非零偏移 */
            if (strncmp(r->sym, "__imp_", 6) == 0) bad++;
        }
    }
    CHECK(bad == 0, "导入调用的 REL32 addend 均为 0");
}

/* ---------- 链接 ---------- */

static void t_link_pe(ChArena *a, ObjModule *rt) {
    if (!rt) return;
    /* 输出路径用 ASCII，避免测试程序自身的 fopen 受系统 ANSI 代码页影响 */
    const char *out = "build/backend_test_pe.exe";
    IrModule *m = gen(a, "打印 \"链接测试\"\n");
    ch_ir_optimize(a, m);
    ObjModule *user = ch_x64_gen(a, m);
    ObjModule *mods[2] = { user, rt };
    char err[256] = {0};
    bool ok = ch_link(a, mods, 2, "rt_entry", out, err, sizeof(err));
    CHECK(ok, "链接生成 PE 文件");
    if (!ok) { printf("链接失败：%s\n", err); return; }

    FILE *fp = fopen(out, "rb");
    CHECK(fp != NULL, "输出文件可打开");
    if (!fp) return;
    unsigned char buf[0x400];
    size_t n = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);
    CHECK(n >= 0x400, "PE 文件头长度足够");
    CHECK(buf[0] == 'M' && buf[1] == 'Z', "DOS 头 MZ");
    uint32_t pe = (uint32_t)buf[0x3C] | ((uint32_t)buf[0x3D] << 8) |
                  ((uint32_t)buf[0x3E] << 16) | ((uint32_t)buf[0x3F] << 24);
    CHECK(pe == 0x40, "PE 头偏移 0x40");
    CHECK(memcmp(buf + pe, "PE\0\0", 4) == 0, "PE 签名");
    CHECK(buf[pe + 4] == 0x64 && buf[pe + 5] == 0x86, "Machine = AMD64");
    uint16_t nsec = (uint16_t)buf[pe + 6];
    CHECK(nsec >= 3, "至少三个输出节（.text/.data/.idata）");
    uint16_t magic = (uint16_t)(buf[pe + 24] | ((uint16_t)buf[pe + 25] << 8));
    CHECK(magic == 0x20B, "可选头 PE32+");
    uint32_t ent = (uint32_t)buf[pe + 40] | ((uint32_t)buf[pe + 41] << 8) |
                   ((uint32_t)buf[pe + 42] << 16) | ((uint32_t)buf[pe + 43] << 24);
    CHECK(ent >= 0x1000 && ent < 0x8000, "入口点在 .text 范围内");
}

int main(void) {
    ChArena *a = ch_arena_new();
    t_x64_emit();
    t_x64_entry_stub();
    t_x64_call_reloc();
    ObjModule *rt = read_runtime(a);
    t_coff_sections(rt);
    t_coff_relocs(rt);
    t_link_pe(a, rt);
    ch_arena_destroy(a);
    printf("后端测试：通过 %d 例，失败 %d 例\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
