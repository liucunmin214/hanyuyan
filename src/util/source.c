#include "util/source.h"
#include "util/utf8.h"

bool ch_utf8_validate(const char *s, size_t len, size_t *bad_off) {
    size_t i = 0;
    while (i < len) {
        int cp;
        int n = ch_utf8_decode(s + i, len - i, &cp);
        if (n == 0) {
            if (bad_off) *bad_off = i;
            return false;
        }
        i += (size_t)n;
    }
    return true;
}

char *ch_source_normalize(const char *bytes, size_t len, size_t *out_len, ChArena *a) {
    size_t start = 0;
    /* 去 UTF-8 BOM */
    if (len >= 3 && (unsigned char)bytes[0] == 0xEF &&
        (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF) {
        start = 3;
    }
    char *out = (char *)ch_arena_alloc(a, len - start + 2);
    size_t o = 0;
    for (size_t i = start; i < len; i++) {
        char c = bytes[i];
        if (c == '\r') {
            out[o++] = '\n';
            if (i + 1 < len && bytes[i + 1] == '\n') i++;
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
    if (out_len) *out_len = o;
    return out;
}

bool ch_source_read(const char *path, ChArena *a, ChSource *out, char *err, size_t errcap) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errcap, "无法打开源文件：%s", path);
        return false;
    }
    size_t cap = 4096;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        fclose(f);
        snprintf(err, errcap, "内存不足：读取源文件失败");
        return false;
    }
    while (true) {
        if (len == cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) {
                free(buf);
                fclose(f);
                snprintf(err, errcap, "内存不足：读取源文件失败");
                return false;
            }
            buf = nb;
        }
        size_t n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (n == 0) break;
    }
    if (ferror(f)) {
        free(buf);
        fclose(f);
        snprintf(err, errcap, "读取源文件时发生错误：%s", path);
        return false;
    }
    fclose(f);

    size_t bad = 0;
    if (!ch_utf8_validate(buf, len, &bad)) {
        free(buf);
        snprintf(err, errcap, "源文件不是合法的 UTF-8 编码（首个非法字节偏移 %zu）", bad);
        return false;
    }

    size_t out_len = 0;
    out->text = ch_source_normalize(buf, len, &out_len, a);
    out->len = out_len;
    free(buf);
    return true;
}
