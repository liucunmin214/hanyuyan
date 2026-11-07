#include "util/diag.h"
#include "util/utf8.h"
#include <stdarg.h>

void ch_diag_init(ChDiagList *d, ChArena *a) {
    ch_vec_init(&d->items, sizeof(ChDiag));
    d->arena = a;
}

void ch_diag_add(ChDiagList *d, bool is_error, int line, int col, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    const char *msg = ch_arena_strdup(d->arena, tmp);

    ChDiag *dg = CH_VEC_PUSH(d->items, ChDiag);
    dg->is_error = is_error;
    dg->line = line;
    dg->col = col;
    dg->msg = msg;
}

int ch_diag_error_count(const ChDiagList *d) {
    int n = 0;
    for (size_t i = 0; i < d->items.len; i++) {
        if (CH_VEC_AT(d->items, ChDiag, i).is_error) n++;
    }
    return n;
}

/* 找到第 line 行（1 起）的字节区间 [begin, end) */
static void line_bounds(const char *t, size_t len, int line, size_t *begin, size_t *end) {
    int cur = 1;
    size_t i = 0;
    while (i < len && cur < line) {
        if (t[i] == '\n') cur++;
        i++;
    }
    *begin = i;
    while (i < len && t[i] != '\n') i++;
    *end = i;
}

/* 把第 col 列（字符计）换算成字节偏移 */
static size_t col_to_byte(const char *t, size_t begin, size_t end, int col) {
    size_t i = begin;
    int c = 1;
    while (i < end && c < col) {
        int cp;
        int n = ch_utf8_decode(t + i, end - i, &cp);
        i += (n > 0) ? (size_t)n : 1;
        c++;
    }
    return i;
}

void ch_diag_print(const ChDiagList *d, const ChSource *src, const char *path, FILE *out) {
    for (size_t i = 0; i < d->items.len; i++) {
        const ChDiag *dg = &CH_VEC_AT(d->items, ChDiag, i);
        fprintf(out, "%s:%d:%d: %s: %s\n", path, dg->line, dg->col,
                dg->is_error ? "错误" : "警告", dg->msg);
        if (!src || !src->text) continue;
        size_t b, e;
        line_bounds(src->text, src->len, dg->line, &b, &e);
        if (b >= e) continue;
        fprintf(out, "  ");
        fwrite(src->text + b, 1, e - b, out);
        fprintf(out, "\n  ");
        size_t caret = col_to_byte(src->text, b, e, dg->col);
        for (size_t k = b; k < caret; k++) {
            fputc(src->text[k] == '\t' ? '\t' : ' ', out);
        }
        fprintf(out, "^\n");
    }
}
