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

typedef struct {
    const char *src;
    size_t      len;
    size_t      pos;
    int         line;
    int         col;
    ChArena    *arena;
    ChDiagList *diags;
    ChVec      *tokens;
} Lexer;

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

static void scan_string(Lexer *lx, int sl, int sc, size_t start) {
    advance(lx, false); /* 开引号 */
    size_t cap = lx->len - lx->pos + 2;
    char *buf = (char *)ch_arena_alloc(lx->arena, cap);
    size_t bn = 0;
    bool closed = false;

    while (lx->pos < lx->len) {
        Cp c = peek_raw(lx);
        if (!c.ok) {
            err_at(lx, lx->line, lx->col, "文字字面量内出现非法 UTF-8 字节");
            lx->pos++;
            lx->col++;
            continue;
        }
        if (is_str_quote(c.cp)) {
            advance(lx, true);
            closed = true;
            break;
        }
        if (c.cp == '\n') break; /* 不允许裸换行，留给主循环 */
        if (c.cp == '\\') {
            advance(lx, true);
            Cp e = peek_raw(lx);
            if (!e.ok) break;
            advance(lx, true);
            switch (e.cp) {
            case 'n': buf[bn++] = '\n'; break;
            case 't': buf[bn++] = '\t'; break;
            case 'r': buf[bn++] = '\r'; break;
            case '\\': buf[bn++] = '\\'; break;
            case '"': buf[bn++] = '"'; break;
            case '0': buf[bn++] = '\0'; break;
            default: {
                err_at(lx, lx->line, lx->col, "未知转义序列 \\%c",
                       (e.cp >= 32 && e.cp < 127) ? (char)e.cp : '?');
                char enc[4];
                int k = ch_utf8_encode(e.cp, enc);
                memcpy(buf + bn, enc, (size_t)k);
                bn += (size_t)k;
                break;
            }
            }
        } else {
            char enc[4];
            int k = ch_utf8_encode(c.cp, enc);
            memcpy(buf + bn, enc, (size_t)k);
            bn += (size_t)k;
            advance(lx, true);
        }
    }
    buf[bn] = '\0';
    if (!closed) {
        err_at(lx, sl, sc, "文字字面量未闭合（缺少结束引号）");
    }
    Token *t = tok_add(lx, TK_STRING, sl, sc, start);
    t->sval = buf;
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

/* 前进 n 个字符并产出单/双字符记号 */
static void emit_n(Lexer *lx, TokenKind kind, int sl, int sc, size_t start, int nchars) {
    for (int i = 0; i < nchars; i++) advance(lx, false);
    tok_add(lx, kind, sl, sc, start);
}

static void scan_punct(Lexer *lx, int sl, int sc, size_t start, Cp c) {
    Cp a = peek_at(lx, lx->pos + c.nbytes);
    switch (c.cp) {
    case '(': emit_n(lx, TK_LPAREN, sl, sc, start, 1); return;
    case ')': emit_n(lx, TK_RPAREN, sl, sc, start, 1); return;
    case '[': emit_n(lx, TK_LBRACKET, sl, sc, start, 1); return;
    case ']': emit_n(lx, TK_RBRACKET, sl, sc, start, 1); return;
    case '{': emit_n(lx, TK_LBRACE, sl, sc, start, 1); return;
    case '}': emit_n(lx, TK_RBRACE, sl, sc, start, 1); return;
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

    int errors_before = ch_diag_error_count(diags);

    while (lx.pos < lx.len) {
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
            tok_add(&lx, TK_NEWLINE, sl, sc, start);
            continue;
        }
        if (c.cp == '#') {
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
                advance(&lx, false);
                advance(&lx, false);
                int depth = 1;
                while (lx.pos < lx.len && depth > 0) {
                    Cp q = peek_raw(&lx);
                    if (!q.ok) {
                        lx.pos++;
                        lx.col++;
                        continue;
                    }
                    if (q.cp == '*') {
                        Cp z = peek_at(&lx, lx.pos + q.nbytes);
                        if (z.ok && z.cp == '/') {
                            advance(&lx, true);
                            advance(&lx, true);
                            depth--;
                            continue;
                        }
                    } else if (q.cp == '/') {
                        Cp z = peek_at(&lx, lx.pos + q.nbytes);
                        if (z.ok && z.cp == '*') {
                            advance(&lx, true);
                            advance(&lx, true);
                            depth++;
                            continue;
                        }
                    }
                    advance(&lx, true);
                }
                if (depth > 0) {
                    err_at(&lx, sl, sc, "块注释未闭合（缺少 */）");
                }
                continue;
            }
            scan_punct(&lx, sl, sc, start, c);
            continue;
        }
        if (c.cp == '"') {
            scan_string(&lx, sl, sc, start);
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
        scan_punct(&lx, sl, sc, start, c);
    }

    Token *e = CH_VEC_PUSH(*out, Token);
    e->kind = TK_EOF;
    e->line = lx.line;
    e->col = lx.col;
    e->byte_off = lx.pos;
    e->byte_len = 0;

    return ch_diag_error_count(diags) == errors_before;
}
