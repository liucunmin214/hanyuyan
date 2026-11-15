#include "lexer/lexer.h"
#include "lexer/keywords.h"
#include "util/utf8.h"

#include <errno.h>
#include <stdarg.h>

#define CH_DIAG_MSG_MAX 512

typedef struct {
    int    cp;
    size_t nbytes;
    bool   ok;
} Cp;

/* 扫描模式栈：支持字符串内插值、插值内嵌套字符串与字典 */
typedef enum {
    MODE_CODE = 0,    /* 普通代码 */
    MODE_STRING,      /* 字符串字面量内部 */
    MODE_INTERP       /* 插值表达式内部 */
} LexMode;

typedef struct {
    LexMode mode;
    bool    saved_interp;   /* 进入本层前的 str_interp 值（供外层字符串恢复） */
    int     saved_depth;    /* 进入本层时的括号深度（插值层据此判断自身的 '}'） */
} ModeFrame;

typedef struct {
    const char *src;
    size_t      len;
    size_t      pos;
    int         line;
    int         col;
    ChArena    *arena;
    ChDiagList *diags;
    ChVec      *tokens;
    /* 缩进块支持（Python 风格） */
    int         indents[64];
    int         ind_top;        /* 栈顶下标；0 层为基准缩进 0 */
    bool        at_line_start;  /* 下一记号位于行首，需先处理缩进 */
    int         bracket_depth;  /* 未闭合括号数：>0 时抑制换行与缩进 */
    /* 字符串与插值支持 */
    ModeFrame   modes[16];
    int         mode_top;       /* 0 = 普通代码模式 */
    char       *sbuf;           /* 当前字符串字面段缓冲 */
    size_t      scap;
    size_t      sbn;
    bool        str_interp;     /* 当前字符串是否含有插值 */
    size_t      seg_start;      /* 当前字面段的起始字节偏移 */
    int         seg_line, seg_col;
} Lexer;

static LexMode cur_mode(const Lexer *lx) {
    return lx->mode_top > 0 ? lx->modes[lx->mode_top - 1].mode : MODE_CODE;
}

static void push_mode(Lexer *lx, LexMode m) {
    if (lx->mode_top < (int)(sizeof(lx->modes) / sizeof(lx->modes[0]))) {
        lx->modes[lx->mode_top].mode = m;
        lx->modes[lx->mode_top].saved_interp = lx->str_interp;
        lx->modes[lx->mode_top].saved_depth = lx->bracket_depth;
        lx->mode_top++;
    }
}

static void pop_mode(Lexer *lx) {
    if (lx->mode_top > 0) {
        lx->mode_top--;
        lx->str_interp = lx->modes[lx->mode_top].saved_interp;
    }
}

/* 全角 / 中文标点归一：仅在“代码上下文”里生效，字符串内部保持原样 */
static int map_cp(int cp) {
    if (cp >= 0xFF01 && cp <= 0xFF5E) return cp - 0xFEE0; /* 全角 ASCII */
    if (cp == 0x3000) return ' ';                          /* 全角空格 */
    switch (cp) {
    case 0x3001: return ',';  /* 、 */
    case 0x3002: return ';';  /* 。 —— 中文句号作为语句结束符 */
    case 0x3010: case 0x3014: return '['; /* 【〔 */
    case 0x3011: case 0x3015: return ']'; /* 】〕 */
    case 0x300A: case 0x3008: return '<'; /* 《〈 */
    case 0x300B: case 0x3009: return '>'; /* 》〉 */
    case 0x300C: case 0x300E: return '"'; /* 「『 */
    case 0x300D: case 0x300F: return '"'; /* 」』 */
    case 0x201C: case 0x201D: return '"'; /* “” */
    case 0x2014: case 0x2015: case 0x2013: return '-'; /* 破折号 */
    case 0x2026: return '.'; /* … */
    default: return cp;
    }
}

static Cp peek_raw(const Lexer *lx) {
    Cp c = {0, 0, false};
    if (lx->pos >= lx->len) return c;
    int cp = 0;
    int n = ch_utf8_decode(lx->src + lx->pos, lx->len - lx->pos, &cp);
    if (n == 0) return c;
    c.cp = cp;
    c.nbytes = (size_t)n;
    c.ok = true;
    return c;
}

static Cp peek_at(const Lexer *lx, size_t off) {
    Cp c = {0, 0, false};
    if (off >= lx->len) return c;
    int cp = 0;
    int n = ch_utf8_decode(lx->src + off, lx->len - off, &cp);
    if (n == 0) return c;
    c.cp = map_cp(cp);
    c.nbytes = (size_t)n;
    c.ok = true;
    return c;
}

static Cp peek(const Lexer *lx) {
    Cp c = peek_raw(lx);
    if (c.ok) c.cp = map_cp(c.cp);
    return c;
}

/* 前进一个字符；raw=true 时不做归一（用于字符串内部） */
static void advance(Lexer *lx, bool raw) {
    Cp c = peek_raw(lx);
    if (!c.ok) {
        lx->pos++;
        lx->col++;
        return;
    }
    int cp = raw ? c.cp : map_cp(c.cp);
    lx->pos += c.nbytes;
    if (cp == '\n') {
        lx->line++;
        lx->col = 1;
    } else {
        lx->col++;
    }
}

static void err_at(Lexer *lx, int line, int col, const char *fmt, ...) {
    char msg[CH_DIAG_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    ch_diag_add(lx->diags, true, line, col, "%s", msg);
}

static Token *tok_add(Lexer *lx, TokenKind kind, int line, int col, size_t start) {
    Token *t = CH_VEC_PUSH(*lx->tokens, Token);
    t->kind = kind;
    t->line = line;
    t->col = col;
    t->byte_off = start;
    t->byte_len = (int)(lx->pos - start);
    return t;
}

/* 把源码区间 [from, to) 归一化后拷贝到区域内 */
static char *slice_norm(Lexer *lx, size_t from, size_t to) {
    char *buf = (char *)ch_arena_alloc(lx->arena, to - from + 1);
    size_t o = 0;
    size_t p = from;
    while (p < to) {
        int cp = 0;
        int n = ch_utf8_decode(lx->src + p, to - p, &cp);
        if (n == 0) {
            p++;
            continue;
        }
        int m = map_cp(cp);
        char enc[4];
        int k = ch_utf8_encode(m, enc);
        memcpy(buf + o, enc, (size_t)k);
        o += (size_t)k;
        p += (size_t)n;
    }
    buf[o] = '\0';
    return buf;
}

static int digit_value(int cp) {
    if (cp >= '0' && cp <= '9') return cp - '0';
    if (cp >= 'a' && cp <= 'f') return cp - 'a' + 10;
    if (cp >= 'A' && cp <= 'F') return cp - 'A' + 10;
    return -1;
}

static void scan_number(Lexer *lx, int sl, int sc, size_t start) {
    char *clean = (char *)ch_arena_alloc(lx->arena, lx->len - start + 2);
    size_t cn = 0;
    int base = 10;
    bool is_float = false;

    Cp c0 = peek(lx);
    if (c0.ok && c0.cp == '0') {
        Cp n1 = peek_at(lx, lx->pos + c0.nbytes);
        if (n1.ok && (n1.cp == 'x' || n1.cp == 'X')) {
            base = 16;
            advance(lx, false);
            advance(lx, false);
        } else if (n1.ok && (n1.cp == 'b' || n1.cp == 'B')) {
            base = 2;
            advance(lx, false);
            advance(lx, false);
        } else if (n1.ok && (n1.cp == 'o' || n1.cp == 'O')) {
            base = 8;
            advance(lx, false);
            advance(lx, false);
        }
    }

    bool any = false;
    while (true) {
        Cp p = peek(lx);
        if (!p.ok) break;
        if (p.cp == '_') {
            advance(lx, false);
            continue;
        }
        int dv = digit_value(p.cp);
        if (!(dv >= 0 && dv < base)) break;
        clean[cn++] = (char)p.cp;
        any = true;
        advance(lx, false);
    }

    if (!any) {
        err_at(lx, sl, sc, "数字字面量缺少有效数字");
    } else if (base == 10) {
        Cp dot = peek(lx);
        if (dot.ok && dot.cp == '.') {
            Cp d1 = peek_at(lx, lx->pos + dot.nbytes);
            if (d1.ok && digit_value(d1.cp) >= 0 && digit_value(d1.cp) <= 9) {
                clean[cn++] = '.';
                is_float = true;
                advance(lx, false);
                while (true) {
                    Cp q = peek(lx);
                    if (!q.ok) break;
                    if (q.cp == '_') {
                        advance(lx, false);
                        continue;
                    }
                    int dv = digit_value(q.cp);
                    if (!(dv >= 0 && dv <= 9)) break;
                    clean[cn++] = (char)q.cp;
                    advance(lx, false);
                }
            }
        }
        Cp ex = peek(lx);
        if (ex.ok && (ex.cp == 'e' || ex.cp == 'E')) {
            size_t off = lx->pos + ex.nbytes;
            Cp s1 = peek_at(lx, off);
            bool has_sign = false;
            int sign = 0;
            if (s1.ok && (s1.cp == '+' || s1.cp == '-')) {
                has_sign = true;
                sign = s1.cp;
                off += s1.nbytes;
            }
            Cp s2 = peek_at(lx, off);
            if (s2.ok && digit_value(s2.cp) >= 0 && digit_value(s2.cp) <= 9) {
                clean[cn++] = 'e';
                advance(lx, false);
                if (has_sign) {
                    clean[cn++] = (char)sign;
                    advance(lx, false);
                }
                while (true) {
                    Cp q = peek(lx);
                    if (!q.ok) break;
                    if (q.cp == '_') {
                        advance(lx, false);
                        continue;
                    }
                    int dv = digit_value(q.cp);
                    if (!(dv >= 0 && dv <= 9)) break;
                    clean[cn++] = (char)q.cp;
                    advance(lx, false);
                }
                is_float = true;
            }
        }
    }

    Cp after = peek(lx);
    if (after.ok && ch_utf8_is_ident_start(after.cp)) {
        err_at(lx, sl, sc, "数字字面量后不能直接紧跟标识符");
    }

    clean[cn] = '\0';

    if (is_float) {
        Token *t = tok_add(lx, TK_FLOAT, sl, sc, start);
        t->text = clean;
        t->fval = strtod(clean, NULL);
    } else {
        errno = 0;
        long long v = strtoll(clean, NULL, base);
        if (errno == ERANGE) {
            err_at(lx, sl, sc, "整数超出 64 位有符号表示范围");
        }
        Token *t = tok_add(lx, TK_INT, sl, sc, start);
        t->text = clean;
        t->ival = v;
    }
}

static bool is_str_quote(int cp) {
    return cp == '"' || cp == 0x201C || cp == 0x201D ||
           cp == 0x300C || cp == 0x300D || cp == 0x300E || cp == 0x300F;
}

/* 把一个码点写入当前字符串段缓冲 */
static void strbuf_put(Lexer *lx, int cp) {
    if (lx->sbn + 4 >= lx->scap) return; /* 缓冲按剩余源码长度分配，正常不会触顶 */
    char enc[4];
    int k = ch_utf8_encode(cp, enc);
    memcpy(lx->sbuf + lx->sbn, enc, (size_t)k);
    lx->sbn += (size_t)k;
}

/* 把当前缓冲作为字面段记号发出：含插值的字符串发 TK_ISTRING，否则 TK_STRING */
static void flush_string(Lexer *lx) {
    char *s = (char *)ch_arena_alloc(lx->arena, lx->sbn + 1);
    memcpy(s, lx->sbuf, lx->sbn);
    s[lx->sbn] = '\0';
    Token *t = tok_add(lx, lx->str_interp ? TK_ISTRING : TK_STRING,
                       lx->seg_line, lx->seg_col, lx->seg_start);
    t->sval = s;
    lx->sbn = 0;
}

/* 主循环遇到引号（普通代码或插值模式）时进入字符串扫描 */
static void scan_string_start(Lexer *lx, int sl, int sc, size_t start) {
    push_mode(lx, MODE_STRING);
    lx->scap = lx->len - lx->pos + 2;
    lx->sbuf = (char *)ch_arena_alloc(lx->arena, lx->scap);
    lx->sbn = 0;
    lx->str_interp = false;
    lx->seg_start = start;
    lx->seg_line = sl;
    lx->seg_col = sc;
    advance(lx, true); /* 消费开引号 */
}

/* 字符串模式下处理一个字符（由主循环逐次调用） */
static void scan_string_cont(Lexer *lx) {
    Cp c = peek_raw(lx);
    if (!c.ok) { /* EOF：按未闭合处理 */
        err_at(lx, lx->seg_line, lx->seg_col, "文字字面量未闭合（缺少结束引号）");
        flush_string(lx);
        pop_mode(lx);
        return;
    }
    if (is_str_quote(c.cp)) {
        advance(lx, true);
        flush_string(lx);
        pop_mode(lx);
        return;
    }
    if (c.cp == '\n') { /* 不允许裸换行，留给主循环产生换行记号 */
        err_at(lx, lx->seg_line, lx->seg_col, "文字字面量未闭合（缺少结束引号）");
        flush_string(lx);
        pop_mode(lx);
        return;
    }
    if (c.cp == '{') {
        Cp a = peek_at(lx, lx->pos + c.nbytes);
        if (a.ok && a.cp == '{') { /* {{ → 字面 { */
            advance(lx, true);
            advance(lx, true);
            strbuf_put(lx, '{');
            return;
        }
        /* 插值开始：先标记含插值，再发出字面段与 { 记号，切换到插值模式 */
        lx->str_interp = true;
        flush_string(lx);
        size_t bstart = lx->pos;
        int bl = lx->line, bc = lx->col;
        advance(lx, true);
        tok_add(lx, TK_LBRACE, bl, bc, bstart);
        push_mode(lx, MODE_INTERP);
        return;
    }
    if (c.cp == '}') {
        Cp a = peek_at(lx, lx->pos + c.nbytes);
        if (a.ok && a.cp == '}') { /* }} → 字面 } */
            advance(lx, true);
            advance(lx, true);
            strbuf_put(lx, '}');
        } else {
            err_at(lx, lx->line, lx->col, "文字中的孤立 '}'，字面大括号请写作 '}}'");
            advance(lx, true);
            strbuf_put(lx, '}');
        }
        return;
    }
    if (c.cp == '\\') {
        advance(lx, true);
        Cp e = peek_raw(lx);
        if (!e.ok) return;
        advance(lx, true);
        switch (e.cp) {
        case 'n': strbuf_put(lx, '\n'); break;
        case 't': strbuf_put(lx, '\t'); break;
        case 'r': strbuf_put(lx, '\r'); break;
        case '\\': strbuf_put(lx, '\\'); break;
        case '"': strbuf_put(lx, '"'); break;
        case '0': strbuf_put(lx, 0); break;
        default: {
            err_at(lx, lx->line, lx->col, "未知转义序列 \\%c",
                   (e.cp >= 32 && e.cp < 127) ? (char)e.cp : '?');
            strbuf_put(lx, e.cp);
            break;
        }
        }
        return;
    }
    strbuf_put(lx, c.cp);
    advance(lx, true);
}

static void scan_word(Lexer *lx, int sl, int sc, size_t start) {
    while (true) {
        Cp c = peek(lx);
        if (!c.ok || !ch_utf8_is_ident_cont(c.cp)) break;
        advance(lx, false);
    }
    char *word = slice_norm(lx, start, lx->pos);
    TokenKind kind = ch_keyword_lookup(word);
    Token *t = tok_add(lx, kind, sl, sc, start);
    t->text = word;
}

/* 尝试扫描【变量名】。只有内容恰好是一个标识符时才作为变量引用；
 * 其他形式仍按普通中文方括号处理，保留【1, 2】等旧写法。 */
static bool scan_varref(Lexer *lx, int sl, int sc, size_t start) {
    Cp open = peek_raw(lx);
    if (!open.ok || open.cp != 0x3010) return false;

    size_t name_start = start + open.nbytes;
    size_t p = name_start;
    int cp = 0;
    int n = ch_utf8_decode(lx->src + p, lx->len - p, &cp);
    if (n == 0 || !ch_utf8_is_ident_start(cp)) return false;
    do {
        p += (size_t)n;
        n = ch_utf8_decode(lx->src + p, lx->len - p, &cp);
    } while (n > 0 && ch_utf8_is_ident_cont(cp));
    if (n == 0 || cp != 0x3011) return false;

    size_t name_end = p;
    char *name = slice_norm(lx, name_start, name_end);
    advance(lx, true); /* 【 */
    while (lx->pos < name_end) advance(lx, true);
    advance(lx, true); /* 】 */
    Token *t = tok_add(lx, TK_VARREF, sl, sc, start);
    t->text = name;
    return true;
}

/* 前进 n 个字符并产出单/双字符记号 */
static void emit_n(Lexer *lx, TokenKind kind, int sl, int sc, size_t start, int nchars) {
    for (int i = 0; i < nchars; i++) advance(lx, false);
    tok_add(lx, kind, sl, sc, start);
}

/* 消费一个块注释（调用点已知其起始，即斜杠后跟星号） */
static void consume_block_comment(Lexer *lx) {
    advance(lx, false); /* / */
    advance(lx, false); /* * */
    int depth = 1;
    while (lx->pos < lx->len && depth > 0) {
        Cp q = peek_raw(lx);
        if (!q.ok) {
            lx->pos++;
            lx->col++;
            continue;
        }
        if (q.cp == '*') {
            Cp z = peek_at(lx, lx->pos + q.nbytes);
            if (z.ok && z.cp == '/') {
                advance(lx, true);
                advance(lx, true);
                depth--;
                continue;
            }
        } else if (q.cp == '/') {
            Cp z = peek_at(lx, lx->pos + q.nbytes);
            if (z.ok && z.cp == '*') {
                advance(lx, true);
                advance(lx, true);
                depth++;
                continue;
            }
        }
        advance(lx, true);
    }
    if (depth > 0) {
        err_at(lx, lx->line, lx->col, "块注释未闭合（缺少 */）");
    }
}

/* 判断当前位置开始的块注释是否独占一行（其后到行尾只有空白） */
static bool block_comment_line_only(const Lexer *lx) {
    size_t p = lx->pos + 2; /* 跳过开头的 / * */
    bool closed = false;
    while (p < lx->len) {
        int cp = 0;
        int n = ch_utf8_decode(lx->src + p, lx->len - p, &cp);
        if (n == 0) { p++; continue; }
        if (cp == '*' ) {
            int cp2 = 0;
            int n2 = (p + (size_t)n < lx->len)
                     ? ch_utf8_decode(lx->src + p + (size_t)n, lx->len - p - (size_t)n, &cp2) : 0;
            if (n2 > 0 && cp2 == '/') { p += (size_t)n + (size_t)n2; closed = true; break; }
        }
        p += (size_t)n;
    }
    if (!closed) return true; /* 未闭合按独行处理，后续统一报错 */
    while (p < lx->len) {
        int cp = 0;
        int n = ch_utf8_decode(lx->src + p, lx->len - p, &cp);
        if (n == 0) { p++; continue; }
        if (cp == '\n') return true;
        if (cp == ' ' || cp == '\t' || cp == '\r' || cp == 0x3000 || cp == '\f' || cp == '\v') {
            p += (size_t)n;
            continue;
        }
        return false;
    }
    return true; /* EOF */
}

static void scan_punct(Lexer *lx, int sl, int sc, size_t start, Cp c) {
    Cp a = peek_at(lx, lx->pos + c.nbytes);
    switch (c.cp) {
    case '(': lx->bracket_depth++; emit_n(lx, TK_LPAREN, sl, sc, start, 1); return;
    case ')': if (lx->bracket_depth > 0) lx->bracket_depth--;
              emit_n(lx, TK_RPAREN, sl, sc, start, 1); return;
    case '[': lx->bracket_depth++; emit_n(lx, TK_LBRACKET, sl, sc, start, 1); return;
    case ']': if (lx->bracket_depth > 0) lx->bracket_depth--;
              emit_n(lx, TK_RBRACKET, sl, sc, start, 1); return;
    case '{': lx->bracket_depth++; emit_n(lx, TK_LBRACE, sl, sc, start, 1); return;
    case '}': if (lx->bracket_depth > 0) lx->bracket_depth--;
              emit_n(lx, TK_RBRACE, sl, sc, start, 1); return;
    case ',': emit_n(lx, TK_COMMA, sl, sc, start, 1); return;
    case ':': emit_n(lx, TK_COLON, sl, sc, start, 1); return;
    case ';': emit_n(lx, TK_SEMI, sl, sc, start, 1); return;
    case '+': emit_n(lx, TK_PLUS, sl, sc, start, 1); return;
    case '*': emit_n(lx, TK_STAR, sl, sc, start, 1); return;
    case '/': emit_n(lx, TK_SLASH, sl, sc, start, 1); return;
    case '%': emit_n(lx, TK_PERCENT, sl, sc, start, 1); return;
    case '^': emit_n(lx, TK_CARET, sl, sc, start, 1); return;
    case '.': {
        if (a.ok && a.cp == '.') {
            Cp d = peek_at(lx, lx->pos + c.nbytes + a.nbytes);
            if (d.ok && d.cp == '.') {
                emit_n(lx, TK_ELLIPSIS, sl, sc, start, 3);
                return;
            }
        }
        emit_n(lx, TK_DOT, sl, sc, start, 1);
        return;
    }
    case '-':
        if (a.ok && a.cp == '>') {
            emit_n(lx, TK_ARROW, sl, sc, start, 2);
        } else {
            emit_n(lx, TK_MINUS, sl, sc, start, 1);
        }
        return;
    case '=':
        if (a.ok && a.cp == '=') {
            emit_n(lx, TK_EQ, sl, sc, start, 2);
        } else if (a.ok && a.cp == '>') {
            emit_n(lx, TK_FATARROW, sl, sc, start, 2);
        } else {
            emit_n(lx, TK_ASSIGN, sl, sc, start, 1);
        }
        return;
    case '!':
        if (a.ok && a.cp == '=') {
            emit_n(lx, TK_NE, sl, sc, start, 2);
        } else {
            emit_n(lx, TK_BANG, sl, sc, start, 1);
        }
        return;
    case '<':
        if (a.ok && a.cp == '=') {
            emit_n(lx, TK_LE, sl, sc, start, 2);
        } else {
            emit_n(lx, TK_LT, sl, sc, start, 1);
        }
        return;
    case '>':
        if (a.ok && a.cp == '=') {
            emit_n(lx, TK_GE, sl, sc, start, 2);
        } else {
            emit_n(lx, TK_GT, sl, sc, start, 1);
        }
        return;
    case '&':
        if (a.ok && a.cp == '&') {
            emit_n(lx, TK_ANDAND, sl, sc, start, 2);
        } else {
            err_at(lx, sl, sc, "非法字符 '&'（逻辑与请写作 && 或使用关键字“与”）");
            advance(lx, false);
        }
        return;
    case '|':
        if (a.ok && a.cp == '|') {
            emit_n(lx, TK_OROR, sl, sc, start, 2);
        } else {
            err_at(lx, sl, sc, "非法字符 '|'（逻辑或请写作 || 或使用关键字“或”）");
            advance(lx, false);
        }
        return;
    default: {
        char enc[4];
        int k = ch_utf8_encode(c.cp, enc);
        enc[k] = '\0';
        err_at(lx, sl, sc, "非法字符 '%s'", enc);
        advance(lx, false);
        return;
    }
    }
}

bool ch_lex(ChArena *arena, ChDiagList *diags, const char *src, size_t len, ChVec *out) {
    Lexer lx;
    lx.src = src;
    lx.len = len;
    lx.pos = 0;
    lx.line = 1;
    lx.col = 1;
    lx.arena = arena;
    lx.diags = diags;
    lx.tokens = out;
    lx.ind_top = 0;
    lx.indents[0] = 0;
    lx.at_line_start = true;
    lx.bracket_depth = 0;
    lx.mode_top = 0;
    lx.sbuf = NULL;
    lx.scap = 0;
    lx.sbn = 0;
    lx.str_interp = false;
    lx.seg_start = 0;
    lx.seg_line = 1;
    lx.seg_col = 1;

    int errors_before = ch_diag_error_count(diags);

    while (lx.pos < lx.len) {
        /* 字符串模式：逐字符处理字符串内容 */
        if (cur_mode(&lx) == MODE_STRING) {
            scan_string_cont(&lx);
            continue;
        }

        /* 行首缩进处理（仅普通代码模式且不在未闭合括号内） */
        if (lx.at_line_start && lx.bracket_depth == 0 && cur_mode(&lx) == MODE_CODE) {
            size_t lstart = lx.pos;
            int ll = lx.line, lc = lx.col;
            /* 测量行首空白宽度：制表符按 4 列计（与编辑器插入 \t 一致） */
            int width = 0;
            for (;;) {
                Cp w = peek(&lx);
                if (!w.ok) break;
                if (w.cp == ' ') { width += 1; advance(&lx, false); }
                else if (w.cp == '\t') { width += 4; advance(&lx, false); }
                else if (w.cp == '\f' || w.cp == '\v') { advance(&lx, false); }
                else break;
            }
            Cp c0 = peek(&lx);
            if (c0.ok && c0.cp == '\n') {          /* 空白空行：只发换行记号 */
                advance(&lx, false);
                tok_add(&lx, TK_NEWLINE, ll, lc, lstart);
                continue;                          /* 行首状态保持到下一行 */
            }
            if (!c0.ok) {                          /* 文件末尾的空白 */
                lx.at_line_start = false;
                continue;
            }
            if (c0.cp == '#') {                    /* 纯注释行：不参与缩进处理 */
                lx.at_line_start = false;
                continue;                          /* 注释与换行交给下方分支 */
            }
            if (c0.cp == '/') {                    /* 块注释独占一行时不参与缩进处理 */
                Cp a0 = peek_at(&lx, lx.pos + c0.nbytes);
                if (a0.ok && a0.cp == '*' && block_comment_line_only(&lx)) {
                    consume_block_comment(&lx);
                    continue;
                }
            }
            /* 真实代码行：与缩进栈比较，产出缩进/取消缩进记号 */
            if (width > lx.indents[lx.ind_top]) {
                lx.ind_top++;
                lx.indents[lx.ind_top] = width;
                tok_add(&lx, TK_INDENT, lx.line, lx.col, lstart);
            } else if (width < lx.indents[lx.ind_top]) {
                while (lx.ind_top > 0 && lx.indents[lx.ind_top] > width) {
                    lx.ind_top--;
                    tok_add(&lx, TK_DEDENT, lx.line, lx.col, lstart);
                }
                if (lx.indents[lx.ind_top] != width) {
                    err_at(&lx, ll, lc, "缩进不一致：本行 %d 列与外层 %d 列不匹配",
                           width, lx.indents[lx.ind_top]);
                    lx.indents[lx.ind_top] = width; /* 容错：以本行为准，避免连锁报错 */
                }
            }
            lx.at_line_start = false;
            /* 落入下方正常分派处理行首字符 */
        }

        size_t start = lx.pos;
        int sl = lx.line;
        int sc = lx.col;
        Cp c = peek(&lx);
        if (!c.ok) {
            err_at(&lx, sl, sc, "非法的 UTF-8 字节序列");
            lx.pos++;
            lx.col++;
            continue;
        }
        if (c.cp == ' ' || c.cp == '\t' || c.cp == '\f' || c.cp == '\v') {
            advance(&lx, false);
            continue;
        }
        if (c.cp == '\n') {
            advance(&lx, false);
            if (cur_mode(&lx) == MODE_INTERP && lx.bracket_depth == 0) {
                err_at(&lx, sl, sc, "插值表达式内不能换行（跨行请用括号包含）");
                continue;
            }
            if (lx.bracket_depth > 0) continue;    /* 括号内换行：静默跳过 */
            tok_add(&lx, TK_NEWLINE, sl, sc, start);
            lx.at_line_start = true;
            continue;
        }
        if (c.cp == '#') {
            if (cur_mode(&lx) == MODE_INTERP) {
                err_at(&lx, sl, sc, "插值表达式内不能使用注释");
                advance(&lx, false);
                continue;
            }
            while (lx.pos < lx.len) {
                Cp q = peek(&lx);
                if (!q.ok || q.cp == '\n') break;
                advance(&lx, false);
            }
            continue;
        }
        if (c.cp == '/') {
            Cp a = peek_at(&lx, lx.pos + c.nbytes);
            if (a.ok && a.cp == '*') {
                consume_block_comment(&lx);
                continue;
            }
            scan_punct(&lx, sl, sc, start, c);
            continue;
        }
        if (c.cp == '"') {
            scan_string_start(&lx, sl, sc, start);
            continue;
        }
        if (c.cp == '}') {
            /* 插值表达式结束：括号深度回到进入插值时（更深则属于插值内的字典） */
            if (cur_mode(&lx) == MODE_INTERP &&
                lx.bracket_depth == lx.modes[lx.mode_top - 1].saved_depth) {
                advance(&lx, false);
                tok_add(&lx, TK_RBRACE, sl, sc, start);
                pop_mode(&lx);
                lx.sbn = 0;                 /* 恢复字符串扫描：新字面段 */
                lx.seg_start = lx.pos;
                lx.seg_line = lx.line;
                lx.seg_col = lx.col;
                continue;
            }
            scan_punct(&lx, sl, sc, start, c);
            continue;
        }
        if (c.cp >= '0' && c.cp <= '9') {
            scan_number(&lx, sl, sc, start);
            continue;
        }
        if (ch_utf8_is_ident_start(c.cp)) {
            scan_word(&lx, sl, sc, start);
            continue;
        }
        if (scan_varref(&lx, sl, sc, start)) continue;
        scan_punct(&lx, sl, sc, start, c);
    }

    /* 冲刷未结束的字符串/插值状态 */
    while (cur_mode(&lx) != MODE_CODE) {
        if (cur_mode(&lx) == MODE_STRING) {
            err_at(&lx, lx.seg_line, lx.seg_col, "文字字面量未闭合（缺少结束引号）");
            flush_string(&lx);
            pop_mode(&lx);
        } else {
            err_at(&lx, lx.seg_line, lx.seg_col, "插值表达式未闭合（缺少 '}'）");
            tok_add(&lx, TK_RBRACE, lx.line, lx.col, lx.pos);
            pop_mode(&lx);
        }
    }
    /* 文件结束：弹出全部缩进层级 */
    while (lx.ind_top > 0) {
        lx.ind_top--;
        tok_add(&lx, TK_DEDENT, lx.line, lx.col, lx.pos);
    }

    Token *e = CH_VEC_PUSH(*out, Token);
    e->kind = TK_EOF;
    e->line = lx.line;
    e->col = lx.col;
    e->byte_off = lx.pos;
    e->byte_len = 0;

    return ch_diag_error_count(diags) == errors_before;
}
