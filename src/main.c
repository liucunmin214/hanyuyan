#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "util/common.h"
#include "util/arena.h"
#include "util/source.h"
#include "util/diag.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "sema/sema.h"
#include "ir/ir.h"
#include "backend/x64.h"
#include "coff/coff.h"
#include "link/link.h"

/* 由 build.ps1 生成的运行时目标文件字节数组 */
extern const unsigned char ch_runtime_obj[];
extern const unsigned int  ch_runtime_obj_len;

static void usage(void) {
    printf("汉语言 编译器\n");
    printf("用法：han.exe <选项> <源文件.汉>\n");
    printf("不带选项时：编译到源文件同目录并自动弹窗运行（最简单用法）\n");
    printf("选项：\n");
    printf("  -o <文件>       指定输出可执行文件（仅编译，不自动运行）\n");
    printf("  --dump-tokens   输出词法记号流\n");
    printf("  --dump-ast      输出抽象语法树\n");
    printf("  --dump-ir       输出中间表示（IR）\n");
    printf("  --check         仅做语法与语义检查，不输出产物\n");
    printf("  --help          显示此帮助\n");
}

/* 由源文件路径推导默认输出路径：去掉目录与扩展名，加 .exe */
static void default_output(const char *src, char *buf, size_t n) {
    /* 输出 exe 与源文件同目录：把扩展名替换为 .exe，
     * 代码保存在任何位置都能就地生成、就地运行。 */
    const char *dot = strrchr(src, '.');
    const char *slash1 = strrchr(src, '/');
    const char *slash2 = strrchr(src, '\\');
    const char *slash = slash1 > slash2 ? slash1 : slash2;
    int stem = (dot && (!slash || dot > slash)) ? (int)(dot - src)
                                                : (int)strlen(src);
    snprintf(buf, n, "%.*s.exe", stem, src);
}

static void print_escaped(FILE *out, const char *s) {
    fputc('"', out);
    for (const char *p = s; *p; p++) {
        switch (*p) {
        case '\n': fputs("\\n", out); break;
        case '\t': fputs("\\t", out); break;
        case '\r': fputs("\\r", out); break;
        case '\\': fputs("\\\\", out); break;
        case '"': fputs("\\\"", out); break;
        default: fputc(*p, out); break;
        }
    }
    fputc('"', out);
}

/* ==================== 多文件导入加载器 ====================
 *
 * 「导入 "相对路径.汉"」在语义检查前展开：
 *   - 路径相对导入者所在目录解析，规范化为绝对路径去重；
 *   - 正在展开的路径入栈做环检测；
 *   - 被导入文件独立词法/语法分析（诊断按各自文件打印）；
 *   - 展开结果按声明顺序置于导入点，导入声明本身被剥除。
 */

typedef struct {
    ChArena    *arena;
    ChDiagList *diags;
    ChVec       paths;     /* const char *：已展开的规范路径（去重） */
    ChVec       loading;   /* const char *：正在展开的路径栈（环检测） */
    bool        failed;    /* 子文件读取/解析失败 */
} Loader;

/* 规范化为绝对路径，失败时返回原路径副本 */
static const char *canon_path(Loader *L, const char *path) {
    char buf[1024];
    DWORD n = GetFullPathNameA(path, (DWORD)sizeof(buf), buf, NULL);
    if (n == 0 || n >= sizeof(buf)) return ch_arena_strdup(L->arena, path);
    buf[n] = 0;
    return ch_arena_strdup(L->arena, buf);
}

static bool path_in_vec(ChVec *v, const char *p) {
    for (size_t i = 0; i < v->len; i++)
        if (_stricmp(CH_VEC_AT(*v, const char *, i), p) == 0) return true;
    return false;
}

/* 相对导入者目录解析目标路径。
 * importer 来自 argv（系统 ANSI 编码），rel 来自源文件字符串（UTF-8）；
 * 先把 rel 转成 ANSI 再拼接，保证后续 fopen 能打开。 */
static const char *resolve_rel(Loader *L, const char *importer, const char *rel) {
    wchar_t w[1024];
    char relA[1024];
    relA[0] = 0;
    int wn = MultiByteToWideChar(CP_UTF8, 0, rel, -1, w, 1024);
    if (wn > 0)
        WideCharToMultiByte(CP_ACP, 0, w, -1, relA, (int)sizeof(relA), NULL, NULL);
    if (!relA[0]) snprintf(relA, sizeof(relA), "%s", rel);
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", importer);
    char *p = buf + strlen(buf);
    while (p > buf && p[-1] != '\\' && p[-1] != '/') p--;
    *p = 0;
    strncat(buf, relA, sizeof(buf) - strlen(buf) - 1);
    return canon_path(L, buf);
}

/* 读取并解析被导入文件；诊断按该文件自身打印 */
static AstNode **load_module(Loader *L, const char *path, const char *rel,
                             int line, int col, int *out_count) {
    ChSource src;
    char err[256];
    if (!ch_source_read(path, L->arena, &src, err, sizeof(err))) {
        ch_diag_add(L->diags, true, line, col,
                    "无法读取导入文件 \"%s\"：%s", rel, err);
        L->failed = true;
        *out_count = 0;
        return NULL;
    }
    ChVec tokens;
    ch_vec_init(&tokens, sizeof(Token));
    ChDiagList diags2;
    ch_diag_init(&diags2, L->arena);
    ch_lex(L->arena, &diags2, src.text, src.len, &tokens);
    int n = 0;
    AstNode **ds = ch_parse(L->arena, &diags2, tokens.data, tokens.len, &n);
    if (diags2.items.len > 0) ch_diag_print(&diags2, &src, path, stdout);
    if (ch_diag_error_count(&diags2) > 0) L->failed = true;
    ch_vec_free(&tokens);
    *out_count = n;
    return ds;
}

/* 递归展开导入声明，返回剥除导入后的扁平声明数组 */
static AstNode **expand_decls(Loader *L, const char *importer,
                              AstNode **decls, int ndecls, int *out_count) {
    ChVec out;
    ch_vec_init(&out, sizeof(AstNode *));
    for (int i = 0; i < ndecls; i++) {
        Decl *d = decls[i]->as.decl;
        if (!d || d->kind != DECL_IMPORT) {
            AstNode **slot = ch_vec_push(&out);
            *slot = decls[i];
            continue;
        }
        const char *target = resolve_rel(L, importer, d->u.import.path);
        if (path_in_vec(&L->loading, target)) {
            ch_diag_add(L->diags, true, d->line, d->col,
                        "导入成环：\"%s\"", d->u.import.path);
            L->failed = true;
            continue;
        }
        if (path_in_vec(&L->paths, target)) continue;   /* 已展开过，去重 */
        const char **mk = ch_vec_push(&L->loading);
        *mk = target;
        int nsub = 0;
        AstNode **sub = load_module(L, target, d->u.import.path,
                                    d->line, d->col, &nsub);
        if (sub) {
            int nex = 0;
            AstNode **ex = expand_decls(L, target, sub, nsub, &nex);
            for (int k = 0; k < nex; k++) {
                AstNode **slot = ch_vec_push(&out);
                *slot = ex[k];
            }
        }
        L->loading.len--;   /* 弹出路径栈 */
        const char **done = ch_vec_push(&L->paths);
        *done = target;
    }
    int n = (int)out.len;
    AstNode **arr = ch_arena_alloc(L->arena, (size_t)(n ? n : 1) * sizeof(AstNode *));
    if (n) memcpy(arr, out.data, (size_t)n * sizeof(AstNode *));
    ch_vec_free(&out);
    *out_count = n;
    return arr;
}

/* 在独立控制台窗口中运行刚编译出的 exe（不等待返回）。
 * 程序结束时运行时自动暂停等待回车，窗口不会一闪而过。 */
static void run_new_window(const char *exe_utf8) {
    WCHAR wpath[1024];
    /* argv 中的路径是系统 ANSI 编码，不能用 CP_UTF8 转 */
    int n = MultiByteToWideChar(CP_ACP, 0, exe_utf8, -1, wpath, 1024);
    if (n <= 0) return;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    WCHAR cmd[1100];
    WCHAR dir[1024];
    _snwprintf(cmd, 1100, L"\"%s\"", wpath);
    cmd[1099] = 0;
    wcscpy(dir, wpath);
    {
        WCHAR *s = wcsrchr(dir, L'\\');
        if (s) *s = 0;
    }
    /* 工作目录设为 exe 所在目录，程序读写相对文件更符合直觉 */
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                       CREATE_NEW_CONSOLE, NULL, dir, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        printf("已在新窗口运行（可键盘输入，程序结束自动暂停）\n");
    }
}

/* argv 中的路径是系统 ANSI 编码（CP_ACP），而编译器自身文案是 UTF-8。
 * 混在一起打印会让编辑器/终端出现乱码，这里统一转成 UTF-8 再显示。 */
static const char *ansi_to_utf8(const char *s) {
    static char buf[4096];
    WCHAR w[2048];
    if (!MultiByteToWideChar(CP_ACP, 0, s, -1, w, 2048) ||
        !WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)sizeof(buf), NULL, NULL)) {
        snprintf(buf, sizeof(buf), "%s", s);
    }
    return buf;
}

/* 已经是合法 UTF-8 就原样返回，否则按系统 ANSI 解码（用于可能混有路径的错误串） */
static const char *text_to_utf8(const char *s) {
    static char buf[4096];
    WCHAR w[2048];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, 2048)) {
        snprintf(buf, sizeof(buf), "%s", s);
        return buf;
    }
    return ansi_to_utf8(s);
}

int main(int argc, char **argv) {
    SetConsoleOutputCP(65001);

    /* 双击 han.exe（无参数）时暂停，避免窗口一闪而过看不清用法 */
    if (argc <= 1) {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        usage();
        if (h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
            printf("\n按回车键退出...");
            (void)getchar();
        }
        return 1;
    }

    bool dump_tokens = false, dump_ast = false, dump_ir = false, check = false;
    const char *file = NULL;
    const char *out_file = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump-tokens") == 0) dump_tokens = true;
        else if (strcmp(argv[i], "--dump-ast") == 0) dump_ast = true;
        else if (strcmp(argv[i], "--dump-ir") == 0) dump_ir = true;
        else if (strcmp(argv[i], "--check") == 0) check = true;
        else if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "选项 -o 缺少文件名\n");
                usage(); return 2;
            }
            out_file = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(); return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "未知选项：%s\n", argv[i]);
            usage(); return 2;
        } else if (!file) {
            file = argv[i];
        } else {
            fprintf(stderr, "多余的参数：%s\n", argv[i]);
            usage(); return 2;
        }
    }
    if (!file) { usage(); return 2; }
    const char *file_show = ansi_to_utf8(file);   /* 用于显示：统一 UTF-8 */

    ChArena *arena = ch_arena_new();
    ChDiagList diags;
    ch_diag_init(&diags, arena);

    ChSource src;
    char err[256];
    if (!ch_source_read(file, arena, &src, err, sizeof(err))) {
        fprintf(stderr, "错误：%s\n", text_to_utf8(err));
        ch_arena_destroy(arena);
        return 1;
    }

    ChVec tokens;
    ch_vec_init(&tokens, sizeof(Token));
    ch_lex(arena, &diags, src.text, src.len, &tokens);

    if (dump_tokens) {
        printf("记号流：%s\n", file_show);
        printf("行:列\t类型\t原文/值\n");
        for (size_t i = 0; i < tokens.len; i++) {
            Token *t = &CH_VEC_AT(tokens, Token, i);
            printf("%d:%d\t%s", t->line, t->col, ch_token_kind_name(t->kind));
            if (t->sval) { printf("\t"); print_escaped(stdout, t->sval); }
            else if (t->kind == TK_INT) printf("\t%lld", t->ival);
            else if (t->kind == TK_FLOAT) printf("\t%g", t->fval);
            else if (t->text) printf("\t%s", t->text);
            printf("\n");
        }
        printf("记号 %zu 个（含文件结尾），错误 %d 条\n",
               tokens.len, ch_diag_error_count(&diags));
    }

    /* 解析 */
    int ndecls = 0;
    AstNode **decls = ch_parse(arena, &diags,
                               tokens.data, tokens.len, &ndecls);

    /* 展开「导入」声明：被导入文件的声明按序并入当前模块 */
    {
        Loader L;
        L.arena = arena;
        L.diags = &diags;
        ch_vec_init(&L.paths, sizeof(const char *));
        ch_vec_init(&L.loading, sizeof(const char *));
        L.failed = false;
        const char *self = canon_path(&L, file);
        const char **p0 = ch_vec_push(&L.paths);   *p0 = self;
        const char **p1 = ch_vec_push(&L.loading); *p1 = self;
        decls = expand_decls(&L, self, decls, ndecls, &ndecls);
        L.loading.len--;
        if (L.failed && ch_diag_error_count(&diags) == 0)
            ch_diag_add(&diags, true, 0, 0, "导入展开失败");
        ch_vec_free(&L.paths);
        ch_vec_free(&L.loading);
    }

    if (dump_ast) {
        printf("抽象语法树：%s（声明 %d 个）\n", file_show, ndecls);
        for (int i = 0; i < ndecls; i++)
            ch_ast_dump_decl(decls[i]->as.decl, 0, stdout);
    }

    /* 语义检查 */
    bool sema_ok = ch_sema_check(arena, &diags, decls, ndecls);

    if (dump_ir && sema_ok) {
        IrModule *mod = ch_ir_gen(arena, decls, ndecls);
        ch_ir_optimize(arena, mod);
        printf("中间表示：%s\n", file_show);
        ch_ir_dump(mod, stdout);
    }

    if (diags.items.len > 0) {
        ch_diag_print(&diags, &src, file_show, stdout);
    }

    int code = ch_diag_error_count(&diags) > 0 ? 1 : 0;

    /* 默认模式：编译并链接成独立 exe */
    bool build = !dump_tokens && !dump_ast && !dump_ir && !check;

    /* 最简单的用法：han.exe 文件.汉（不带任何选项）
     * 编译到源文件同目录后自动弹出新窗口运行，代码可以保存在任何位置。 */
    bool auto_run = build && !out_file && code == 0;

    if (build && code == 0) {
        IrModule *mod = ch_ir_gen(arena, decls, ndecls);
        ch_ir_optimize(arena, mod);

        ObjModule *user = ch_x64_gen(arena, mod);
        ObjModule *rt = ch_arena_alloc(arena, sizeof(ObjModule));
        char lerr[256];
        lerr[0] = 0;
        if (!ch_coff_read(arena, ch_runtime_obj, ch_runtime_obj_len, rt,
                          lerr, sizeof(lerr))) {
            fprintf(stderr, "错误：读取运行时目标文件失败：%s\n", lerr);
            code = 1;
        } else {
            char outbuf[1024];
            if (!out_file) {
                default_output(file, outbuf, sizeof(outbuf));
                out_file = outbuf;
            }
            ObjModule *mods[2] = { user, rt };
            if (!ch_link(arena, mods, 2, "rt_entry", out_file, lerr, sizeof(lerr))) {
                fprintf(stderr, "错误：链接失败：%s\n", lerr);
                code = 1;
            } else {
                printf("已生成可执行文件：%s\n", ansi_to_utf8(out_file));
                if (auto_run) run_new_window(out_file);
            }
        }
    }

    ch_vec_free(&tokens);
    ch_arena_destroy(arena);
    return code;
}
