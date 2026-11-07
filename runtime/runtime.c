/* 汉语言 运行时（无 CRT 依赖）
 *
 * 只依赖 kernel32.dll 的少量导出函数，提供：
 *   打印、内存分配、文字、列表、字典、文件、时间
 * 所有值在运行时统一按 64 位表示：整数/布尔为数值，文字/列表/字典为指针。
 *
 * 编译方式（由 build.ps1 调用）：
 *   gcc -ffreestanding -nostdlib -fno-pic -fno-stack-protector \
 *       -fno-asynchronous-unwind-tables -fno-builtin -O2 -c runtime.c
 */

typedef long long           i64;
typedef unsigned long long  u64;
typedef unsigned int        u32;
typedef unsigned short      u16;
typedef unsigned char       u8;

/* ==================== kernel32 导入 ==================== */

__declspec(dllimport) void *GetStdHandle(unsigned int which);
__declspec(dllimport) int   WriteFile(void *h, const void *buf, unsigned int len,
                                     unsigned int *written, void *ov);
__declspec(dllimport) int   ReadFile(void *h, void *buf, unsigned int len,
                                    unsigned int *read, void *ov);
__declspec(dllimport) void *CreateFileW(const u16 *name, unsigned int access,
                                        unsigned int share, void *sa,
                                        unsigned int disp, unsigned int flags,
                                        void *tmpl);
__declspec(dllimport) int   CloseHandle(void *h);
__declspec(dllimport) void *VirtualAlloc(void *addr, u64 size, unsigned int type,
                                         unsigned int protect);
__declspec(dllimport) void  ExitProcess(unsigned int code);
__declspec(dllimport) int   SetConsoleOutputCP(unsigned int cp);
__declspec(dllimport) int   SetConsoleCP(unsigned int cp);
__declspec(dllimport) void  GetLocalTime(void *st);
__declspec(dllimport) void *GetConsoleWindow(void);
__declspec(dllimport) unsigned int GetConsoleProcessList(unsigned int *list,
                                                          unsigned int count);
__declspec(dllimport) int   GetConsoleMode(void *h, unsigned int *mode);
__declspec(dllimport) int   ReadConsoleW(void *h, void *buf, unsigned int len,
                                          unsigned int *read, void *reserved);

#define STD_OUTPUT_HANDLE 0xFFFFFFF5u
#define STD_INPUT_HANDLE  0xFFFFFFF6u

/* 供 gcc 生成代码时可能引用的内建函数（避免链接 CRT） */
void *memset(void *d, int v, u64 n) {
    u8 *p = (u8 *)d;
    for (u64 i = 0; i < n; i++) p[i] = (u8)v;
    return d;
}
void *memcpy(void *d, const void *s, u64 n) {
    u8 *dp = (u8 *)d;
    const u8 *sp = (const u8 *)s;
    for (u64 i = 0; i < n; i++) dp[i] = sp[i];
    return d;
}

/* ==================== 内存 ==================== */

static void *rt_alloc_bytes(u64 n) {
    u64 size = (n + 4095) & ~(u64)4095;
    if (size == 0) size = 4096;
    /* MEM_COMMIT | MEM_RESERVE = 0x3000，PAGE_READWRITE = 0x4 */
    return VirtualAlloc((void *)0, size, 0x3000u, 0x04u);
}

/* 供编译器后端调用的通用分配（闭包对象、捕获环境等） */
void *rt_alloc(u64 n) { return rt_alloc_bytes(n); }

/* ==================== 字符串 ==================== */

typedef struct { i64 len; char data[]; } RtStr;

static RtStr *str_alloc(i64 len) {
    i64 n = len < 0 ? 0 : len;
    RtStr *s = (RtStr *)rt_alloc_bytes((u64)(sizeof(i64) + n + 1));
    s->len = n;
    s->data[n] = 0;
    return s;
}

static int str_eq(RtStr *a, RtStr *b) {
    if (!a || !b) return a == b;
    if (a->len != b->len) return 0;
    for (i64 i = 0; i < a->len; i++)
        if (a->data[i] != b->data[i]) return 0;
    return 1;
}

static void write_bytes(const char *p, u64 n) {
    unsigned int written = 0;
    void *h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h) return;
    WriteFile(h, p, (unsigned int)n, &written, (void *)0);
}

/* 输出以 NUL 结尾的 UTF-8 字符串 */
static void write_cstr(const char *s) {
    u64 n = 0;
    while (s[n]) n++;
    write_bytes(s, n);
}

/* ==================== 打印 ==================== */

void rt_print_int(i64 v) {
    char buf[64];
    int n = 0;
    if (v < 0) { buf[n++] = '-'; }
    unsigned long long u = v < 0 ? (unsigned long long)(-(v + 1)) + 1ULL : (unsigned long long)v;
    char digits[20];
    int d = 0;
    if (u == 0) digits[d++] = '0';
    while (u > 0) { digits[d++] = (char)('0' + (u % 10)); u /= 10; }
    while (d > 0) buf[n++] = digits[--d];
    buf[n++] = '\n';
    write_bytes(buf, (u64)n);
}

void rt_print_float(double v) {
    char buf[96];
    int n = 0;
    if (v != v) { write_bytes("nan\n", 4); return; }
    if (v < 0) { buf[n++] = '-'; v = -v; }
    i64 ip = (i64)v;
    double frac = v - (double)ip;
    if (frac < 0) frac = 0;
    /* 四舍五入到 6 位小数 */
    i64 fp = (i64)(frac * 1000000.0 + 0.5);
    if (fp >= 1000000) { fp -= 1000000; ip += 1; }
    char digits[24];
    int d = 0;
    i64 t = ip;
    if (t == 0) digits[d++] = '0';
    while (t > 0) { digits[d++] = (char)('0' + (t % 10)); t /= 10; }
    while (d > 0) buf[n++] = digits[--d];
    if (fp != 0) {
        buf[n++] = '.';
        char f[6];
        for (int i = 5; i >= 0; i--) { f[i] = (char)('0' + (fp % 10)); fp /= 10; }
        int last = 5;
        while (last >= 0 && f[last] == '0') last--;
        for (int i = 0; i <= last; i++) buf[n++] = f[i];
    }
    buf[n++] = '\n';
    write_bytes(buf, (u64)n);
}

void rt_print_str(RtStr *s) {
    if (!s) { write_bytes("\n", 1); return; }
    write_bytes(s->data, (u64)s->len);
    write_bytes("\n", 1);
}

void rt_print_bool(i64 v) {
    if (v) write_bytes("\xE7\x9C\x9F\n", 4);   /* 真 */
    else   write_bytes("\xE5\x81\x87\n", 4);   /* 假 */
}

void rt_print_nil(void) {
    write_bytes("\xE7\xA9\xBA\n", 4);           /* 空 */
}

/* 函数值打印：不暴露地址，固定输出 <函数> */
void rt_print_func(i64 v) {
    (void)v;
    write_bytes("<\xE5\x87\xBD\xE6\x95\xB0>\n", 9);   /* <函数> */
}

/* ==================== 文字函数 ==================== */

/* UTF-8 单字节所属字符的长度 */
static i64 utf8_char_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/* 文字长度按字符（UTF-8 码点）计，而不是字节数 */
i64 rt_str_len(RtStr *s) {
    if (!s) return 0;
    i64 n = 0, i = 0;
    while (i < s->len) { i += utf8_char_len((unsigned char)s->data[i]); n++; }
    return n;
}

RtStr *rt_str_concat(RtStr *a, RtStr *b) {
    i64 la = a ? a->len : 0, lb = b ? b->len : 0;
    RtStr *r = str_alloc(la + lb);
    for (i64 i = 0; i < la; i++) r->data[i] = a->data[i];
    for (i64 i = 0; i < lb; i++) r->data[la + i] = b->data[i];
    return r;
}

/* 文字子串：起点与长度均按字符（UTF-8 码点）计 */
RtStr *rt_str_sub(RtStr *s, i64 start, i64 len) {
    if (!s) return str_alloc(0);
    if (start < 0) start = 0;
    i64 off = 0, i = 0;
    while (i < start && off < s->len) { off += utf8_char_len((unsigned char)s->data[off]); i++; }
    if (len < 0) len = 0;
    i64 end = off;
    i = 0;
    while (i < len && end < s->len) { end += utf8_char_len((unsigned char)s->data[end]); i++; }
    i64 n = end - off;
    RtStr *r = str_alloc(n);
    for (i64 k = 0; k < n; k++) r->data[k] = s->data[off + k];
    return r;
}

i64 rt_str_contains(RtStr *a, RtStr *b) {
    if (!a || !b) return 0;
    if (b->len == 0) return 1;
    if (b->len > a->len) return 0;
    for (i64 i = 0; i + b->len <= a->len; i++) {
        int ok = 1;
        for (i64 j = 0; j < b->len; j++)
            if (a->data[i + j] != b->data[j]) { ok = 0; break; }
        if (ok) return 1;
    }
    return 0;
}

i64 rt_str_to_int(RtStr *s) {
    if (!s) return 0;
    i64 i = 0, sign = 1, v = 0;
    while (i < s->len && (s->data[i] == ' ' || s->data[i] == '\t')) i++;
    if (i < s->len && (s->data[i] == '-' || s->data[i] == '+')) {
        if (s->data[i] == '-') sign = -1;
        i++;
    }
    while (i < s->len && s->data[i] >= '0' && s->data[i] <= '9') {
        v = v * 10 + (s->data[i] - '0');
        i++;
    }
    return v * sign;
}

/* 文字转小数：支持符号、整数部分与小数点，非法前缀按已解析部分返回 */
double rt_str_to_float(RtStr *s) {
    if (!s) return 0;
    i64 i = 0;
    double sign = 1.0, v = 0.0;
    while (i < s->len && (s->data[i] == ' ' || s->data[i] == '\t')) i++;
    if (i < s->len && (s->data[i] == '-' || s->data[i] == '+')) {
        if (s->data[i] == '-') sign = -1.0;
        i++;
    }
    while (i < s->len && s->data[i] >= '0' && s->data[i] <= '9') {
        v = v * 10.0 + (double)(s->data[i] - '0');
        i++;
    }
    if (i < s->len && s->data[i] == '.') {
        double scale = 0.1;
        i++;
        while (i < s->len && s->data[i] >= '0' && s->data[i] <= '9') {
            v += (double)(s->data[i] - '0') * scale;
            scale /= 10.0;
            i++;
        }
    }
    return v * sign;
}

RtStr *rt_int_to_str(i64 v) {
    char tmp[32];
    int n = 0;
    if (v < 0) tmp[n++] = '-';
    unsigned long long u = v < 0 ? (unsigned long long)(-(v + 1)) + 1ULL : (unsigned long long)v;
    char dig[20];
    int d = 0;
    if (u == 0) dig[d++] = '0';
    while (u > 0) { dig[d++] = (char)('0' + (u % 10)); u /= 10; }
    while (d > 0) tmp[n++] = dig[--d];
    RtStr *s = str_alloc(n);
    for (int i = 0; i < n; i++) s->data[i] = tmp[i];
    return s;
}

RtStr *rt_float_to_str(double v) {
    char tmp[96];
    int n = 0;
    if (v < 0) { tmp[n++] = '-'; v = -v; }
    i64 ip = (i64)v;
    double frac = v - (double)ip;
    if (frac < 0) frac = 0;
    i64 fp = (i64)(frac * 1000000.0 + 0.5);
    if (fp >= 1000000) { fp -= 1000000; ip += 1; }
    char dig[24];
    int d = 0;
    i64 t = ip;
    if (t == 0) dig[d++] = '0';
    while (t > 0) { dig[d++] = (char)('0' + (t % 10)); t /= 10; }
    while (d > 0) tmp[n++] = dig[--d];
    if (fp != 0) {
        tmp[n++] = '.';
        char f[6];
        for (int i = 5; i >= 0; i--) { f[i] = (char)('0' + (fp % 10)); fp /= 10; }
        int last = 5;
        while (last >= 0 && f[last] == '0') last--;
        for (int i = 0; i <= last; i++) tmp[n++] = f[i];
    }
    RtStr *s = str_alloc(n);
    for (int i = 0; i < n; i++) s->data[i] = tmp[i];
    return s;
}

RtStr *rt_bool_to_str(i64 v) {
    RtStr *s = str_alloc(v ? 3 : 3);
    if (v) { s->data[0] = (char)0xE7; s->data[1] = (char)0x9C; s->data[2] = (char)0x9F; }
    else   { s->data[0] = (char)0xE5; s->data[1] = (char)0x81; s->data[2] = (char)0x87; }
    return s;
}

RtStr *rt_str_identity(RtStr *s) { return s; }

i64 rt_str_eq(RtStr *a, RtStr *b) { return str_eq(a, b); }

RtStr *rt_nil_to_str(i64 v) {
    (void)v;
    RtStr *s = str_alloc(3);
    /* "空" 的 UTF-8 */
    s->data[0] = (char)0xE7; s->data[1] = (char)0xA9; s->data[2] = (char)0xBA;
    return s;
}

RtStr *rt_func_to_str(i64 v) {
    (void)v;
    RtStr *s = str_alloc(8);
    s->data[0] = '<';
    s->data[1] = (char)0xE5; s->data[2] = (char)0x87; s->data[3] = (char)0xBD; /* 函 */
    s->data[4] = (char)0xE6; s->data[5] = (char)0x95; s->data[6] = (char)0xB0; /* 数 */
    s->data[7] = '>';
    return s;
}

/* ==================== 数值辅助 ==================== */

i64 rt_pow_i64(i64 base, i64 exp) {
    if (exp < 0) return 0;
    i64 r = 1;
    while (exp > 0) {
        if (exp & 1) r *= base;
        base *= base;
        exp >>= 1;
    }
    return r;
}

/* 自然对数：拆指数 + atanh 级数（freestanding，无 libm） */
static double ln_f64(double x) {
    union { double d; u64 u; } c;
    int e;
    double m, z, z2, sum, term;
    int k;
    if (x <= 0.0) return 0.0;
    c.d = x;
    e = (int)((c.u >> 52) & 0x7FFu) - 1023;
    c.u = (c.u & 0x000FFFFFFFFFFFFFULL) | 0x3FF0000000000000ULL;  /* 尾数归一化到 [1,2) */
    m = c.d;
    if (m > 1.4142135623730951) { m *= 0.5; e += 1; }
    z = (m - 1.0) / (m + 1.0);
    z2 = z * z;
    sum = 0.0;
    term = z;
    for (k = 1; k < 15; k += 2) { sum += term / (double)k; term *= z2; }
    return 2.0 * sum + (double)e * 0.6931471805599453;
}

/* 自然指数：拆 2^k * e^r，r 用泰勒级数 */
static double exp_f64(double x) {
    double kd, r, sum, term;
    i64 k;
    union { double d; u64 u; } c;
    if (x > 700.0) x = 700.0;
    if (x < -700.0) x = -700.0;
    kd = x / 0.6931471805599453;
    k = (i64)(kd >= 0.0 ? kd + 0.5 : kd - 0.5);
    r = x - (double)k * 0.6931471805599453;
    sum = 1.0;
    term = 1.0;
    for (i64 i = 1; i < 18; i++) { term *= r / (double)i; sum += term; }
    c.u = (u64)((k + 1023) << 52);
    return sum * c.d;
}

double rt_pow_f64(double base, double exp) {
    i64 e = (i64)exp;
    int neg;
    double r;
    if ((double)e != exp) {
        /* 非整数指数：e^(exp * ln(base))；负数底数无实数解，返回 0 */
        if (base <= 0.0) return 0.0;
        return exp_f64(exp * ln_f64(base));
    }
    neg = e < 0;
    if (neg) e = -e;
    r = 1.0;
    while (e > 0) {
        if (e & 1) r *= base;
        base *= base;
        e >>= 1;
    }
    return neg ? 1.0 / r : r;
}

double rt_mod_f64(double a, double b) {
    if (b == 0) return 0;
    i64 q = (i64)(a / b);
    return a - (double)q * b;
}

/* ==================== 列表 ==================== */

typedef struct { i64 len, cap; u64 *items; } RtList;

RtList *rt_list_new(void) {
    RtList *l = (RtList *)rt_alloc_bytes(sizeof(RtList));
    l->len = 0;
    l->cap = 8;
    l->items = (u64 *)rt_alloc_bytes((u64)l->cap * 8);
    return l;
}

static void list_grow(RtList *l) {
    i64 nc = l->cap * 2;
    u64 *ni = (u64 *)rt_alloc_bytes((u64)nc * 8);
    for (i64 i = 0; i < l->len; i++) ni[i] = l->items[i];
    l->items = ni;
    l->cap = nc;
}

void rt_list_push(RtList *l, u64 v) {
    if (!l) return;
    if (l->len >= l->cap) list_grow(l);
    l->items[l->len++] = v;
}

u64 rt_list_get(RtList *l, i64 i) {
    if (!l || i < 0 || i >= l->len) return 0;
    return l->items[i];
}

/* 小数视图：返回值经 xmm0（double 约定），供语言层「列表取数」使用 */
double rt_list_get_f64(RtList *l, i64 i) {
    union { u64 u; double d; } c;
    c.u = rt_list_get(l, i);
    return c.d;
}

void rt_list_set(RtList *l, i64 i, u64 v) {
    if (!l || i < 0 || i >= l->len) return;
    l->items[i] = v;
}

void rt_list_del(RtList *l, i64 i) {
    if (!l || i < 0 || i >= l->len) return;
    for (i64 k = i; k + 1 < l->len; k++) l->items[k] = l->items[k + 1];
    l->len--;
}

i64 rt_list_len(RtList *l) { return l ? l->len : 0; }

/* ==================== 字典 ==================== */

typedef struct { RtStr *key; u64 val; } RtEnt;
typedef struct { i64 cap, count; RtEnt *e; } RtDict;

#define DICT_TOMB ((RtStr *)1)

static u64 str_hash(RtStr *s) {
    u64 h = 1469598103934665603ULL;
    for (i64 i = 0; i < s->len; i++) {
        h ^= (u64)(u8)s->data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static RtDict *dict_new_cap(i64 cap) {
    RtDict *d = (RtDict *)rt_alloc_bytes(sizeof(RtDict));
    d->cap = cap;
    d->count = 0;
    d->e = (RtEnt *)rt_alloc_bytes((u64)cap * sizeof(RtEnt));
    for (i64 i = 0; i < cap; i++) { d->e[i].key = (RtStr *)0; d->e[i].val = 0; }
    return d;
}

RtDict *rt_dict_new(void) { return dict_new_cap(16); }

/* 查找槽位：返回命中位置；未命中返回第一个可用位置 */
static i64 dict_slot(RtDict *d, RtStr *k) {
    u64 mask = (u64)(d->cap - 1);
    u64 idx = str_hash(k) & mask;
    i64 first_tomb = -1;
    for (i64 probe = 0; probe < d->cap; probe++) {
        i64 pos = (i64)((idx + (u64)probe) & mask);
        RtEnt *e = &d->e[pos];
        if (!e->key) {
            return first_tomb >= 0 ? first_tomb : pos;
        }
        if (e->key == DICT_TOMB) {
            if (first_tomb < 0) first_tomb = pos;
            continue;
        }
        if (str_eq(e->key, k)) return pos;
    }
    return first_tomb >= 0 ? first_tomb : -1;
}

static void dict_rehash(RtDict *d) {
    i64 oldcap = d->cap;
    RtEnt *old = d->e;
    d->cap = oldcap * 2;
    d->count = 0;
    d->e = (RtEnt *)rt_alloc_bytes((u64)d->cap * sizeof(RtEnt));
    for (i64 i = 0; i < d->cap; i++) { d->e[i].key = (RtStr *)0; d->e[i].val = 0; }
    for (i64 i = 0; i < oldcap; i++) {
        if (old[i].key && old[i].key != DICT_TOMB) {
            i64 pos = dict_slot(d, old[i].key);
            d->e[pos].key = old[i].key;
            d->e[pos].val = old[i].val;
            d->count++;
        }
    }
}

void rt_dict_set(RtDict *d, RtStr *k, u64 v) {
    if (!d || !k) return;
    if ((d->count + 1) * 2 >= d->cap) dict_rehash(d);
    i64 pos = dict_slot(d, k);
    if (pos < 0) return;
    if (!d->e[pos].key || d->e[pos].key == DICT_TOMB) d->count++;
    d->e[pos].key = k;
    d->e[pos].val = v;
}

u64 rt_dict_get(RtDict *d, RtStr *k) {
    if (!d || !k) return 0;
    i64 pos = dict_slot(d, k);
    if (pos < 0 || !d->e[pos].key || d->e[pos].key == DICT_TOMB) return 0;
    return d->e[pos].val;
}

/* 小数视图：返回值经 xmm0，供语言层「字典取数」使用 */
double rt_dict_get_f64(RtDict *d, RtStr *k) {
    union { u64 u; double d; } c;
    c.u = rt_dict_get(d, k);
    return c.d;
}

i64 rt_dict_has(RtDict *d, RtStr *k) {
    if (!d || !k) return 0;
    i64 pos = dict_slot(d, k);
    if (pos < 0 || !d->e[pos].key || d->e[pos].key == DICT_TOMB) return 0;
    return 1;
}

i64 rt_dict_del(RtDict *d, RtStr *k) {
    if (!d || !k) return 0;
    i64 pos = dict_slot(d, k);
    if (pos < 0 || !d->e[pos].key || d->e[pos].key == DICT_TOMB) return 0;
    d->e[pos].key = DICT_TOMB;
    d->e[pos].val = 0;
    d->count--;
    return 1;
}

i64 rt_dict_len(RtDict *d) { return d ? d->count : 0; }

RtList *rt_dict_keys(RtDict *d) {
    RtList *l = rt_list_new();
    if (!d) return l;
    for (i64 i = 0; i < d->cap; i++) {
        if (d->e[i].key && d->e[i].key != DICT_TOMB)
            rt_list_push(l, (u64)d->e[i].key);
    }
    return l;
}

/* ==================== 文件 ==================== */

static u16 *utf8_to_utf16(RtStr *s) {
    if (!s) return (u16 *)0;
    u16 *out = (u16 *)rt_alloc_bytes((u64)(s->len * 2 + 4));
    i64 i = 0, n = 0;
    while (i < s->len) {
        u8 c = (u8)s->data[i];
        u32 cp = 0;
        if (c < 0x80) { cp = c; i += 1; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < s->len) {
            cp = (u32)((c & 0x1F) << 6) | (u32)((u8)s->data[i + 1] & 0x3F); i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < s->len) {
            cp = (u32)((c & 0x0F) << 12) | (u32)(((u8)s->data[i + 1] & 0x3F) << 6)
               | (u32)((u8)s->data[i + 2] & 0x3F); i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < s->len) {
            cp = (u32)((c & 0x07) << 18) | (u32)(((u8)s->data[i + 1] & 0x3F) << 12)
               | (u32)(((u8)s->data[i + 2] & 0x3F) << 6)
               | (u32)((u8)s->data[i + 3] & 0x3F); i += 4;
        } else { cp = 0; i += 1; }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out[n++] = (u16)(0xD800 + (cp >> 10));
            out[n++] = (u16)(0xDC00 + (cp & 0x3FF));
        } else {
            out[n++] = (u16)cp;
        }
    }
    out[n] = 0;
    return out;
}

#define GENERIC_READ    0x80000000u
#define GENERIC_WRITE   0x40000000u
#define FILE_APPEND_DATA 0x00000004u
#define OPEN_EXISTING   3u
#define CREATE_ALWAYS   2u
#define OPEN_ALWAYS     4u
#define FILE_ATTRIBUTE_NORMAL 0x80u

RtStr *rt_read_file(RtStr *path) {
    u16 *w = utf8_to_utf16(path);
    if (!w) return (RtStr *)0;
    void *h = CreateFileW(w, GENERIC_READ, 1u, (void *)0, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, (void *)0);
    if (!h) return (RtStr *)0;
    i64 cap = 65536, used = 0;
    char *buf = (char *)rt_alloc_bytes((u64)cap);
    while (1) {
        if (used >= cap) {
            i64 nc = cap * 2;
            char *nb = (char *)rt_alloc_bytes((u64)nc);
            for (i64 i = 0; i < used; i++) nb[i] = buf[i];
            buf = nb;
            cap = nc;
        }
        unsigned int read = 0;
        int ok = ReadFile(h, buf + used, (unsigned int)(cap - used), &read, (void *)0);
        if (!ok || read == 0) break;
        used += (i64)read;
    }
    CloseHandle(h);
    RtStr *s = str_alloc(used);
    for (i64 i = 0; i < used; i++) s->data[i] = buf[i];
    return s;
}

static i64 write_file_impl(RtStr *path, RtStr *content, unsigned int access,
                           unsigned int disp) {
    u16 *w = utf8_to_utf16(path);
    if (!w) return 0;
    void *h = CreateFileW(w, access, 0u, (void *)0, disp,
                          FILE_ATTRIBUTE_NORMAL, (void *)0);
    if (!h) return 0;
    unsigned int written = 0;
    int ok = 1;
    i64 off = 0;
    while (off < (content ? content->len : 0)) {
        i64 remain = content->len - off;
        unsigned int chunk = (unsigned int)(remain > 0x7FFFFFFF ? 0x7FFFFFFF : remain);
        if (!WriteFile(h, content->data + off, chunk, &written, (void *)0) || written == 0) {
            ok = 0;
            break;
        }
        off += (i64)written;
    }
    CloseHandle(h);
    return ok;
}

i64 rt_write_file(RtStr *path, RtStr *content) {
    return write_file_impl(path, content, GENERIC_WRITE, CREATE_ALWAYS);
}

/* 追加：用 FILE_APPEND_DATA 打开，写入自动落在文件末尾 */
i64 rt_append_file(RtStr *path, RtStr *content) {
    return write_file_impl(path, content, FILE_APPEND_DATA, OPEN_ALWAYS);
}

i64 rt_file_exists(RtStr *path) {
    u16 *w = utf8_to_utf16(path);
    if (!w) return 0;
    void *h = CreateFileW(w, GENERIC_READ, 1u, (void *)0, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, (void *)0);
    if (!h) return 0;
    CloseHandle(h);
    return 1;
}

/* ==================== 时间 ==================== */

typedef struct {
    u16 year, month, day_of_week, day, hour, minute, second, millis;
} RtSystemTime;

RtStr *rt_now_text(void) {
    RtSystemTime t;
    GetLocalTime(&t);
    char out[20];
    int o = 0;
    int parts[6] = { t.year, t.month, t.day, t.hour, t.minute, t.second };
    int digs[6] = { 4, 2, 2, 2, 2, 2 };
    for (int i = 0; i < 6; i++) {
        int v = parts[i];
        char tmp[5];
        for (int d = digs[i] - 1; d >= 0; d--) { tmp[d] = (char)('0' + (v % 10)); v /= 10; }
        for (int d = 0; d < digs[i]; d++) out[o++] = tmp[d];
        if (i == 2) out[o++] = ' ';
        else if (i == 0 || i == 1) out[o++] = '-';
        else if (i == 3 || i == 4) out[o++] = ':';
    }
    RtStr *s = str_alloc(o);
    for (int i = 0; i < o; i++) s->data[i] = out[i];
    return s;
}

i64 rt_now_ms(void) {
    RtSystemTime t;
    GetLocalTime(&t);
    return ((i64)t.hour * 3600 + (i64)t.minute * 60 + (i64)t.second) * 1000 + (i64)t.millis;
}

/* ==================== 键盘输入 ==================== */

/* UTF-16 转 UTF-8（配合 ReadConsoleW 读取中文输入） */
static RtStr *utf16_to_utf8(const u16 *w, i64 n) {
    i64 bytes = 0, i, o;
    u32 cp;
    for (i = 0; i < n; i++) {
        cp = w[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n &&
            w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (w[i + 1] - 0xDC00);
            i++;
        }
        bytes += cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    }
    RtStr *s = str_alloc(bytes);
    o = 0;
    for (i = 0; i < n; i++) {
        cp = w[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n &&
            w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (w[i + 1] - 0xDC00);
            i++;
        }
        if (cp < 0x80) {
            s->data[o++] = (char)cp;
        } else if (cp < 0x800) {
            s->data[o++] = (char)(0xC0 | (cp >> 6));
            s->data[o++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s->data[o++] = (char)(0xE0 | (cp >> 12));
            s->data[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            s->data[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            s->data[o++] = (char)(0xF0 | (cp >> 18));
            s->data[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            s->data[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            s->data[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    return s;
}

/* 从标准输入读一行（不含换行）。
 * 真实控制台走 ReadConsoleW（UTF-16，中文输入可靠）；
 * 管道/重定向退化为逐字节 ReadFile（输入按 UTF-8 解释）。
 * EOF 返回空文字。 */
RtStr *rt_input_str(void) {
    void *h = GetStdHandle(STD_INPUT_HANDLE);
    unsigned int mode = 0;
    if (!h) return str_alloc(0);
    if (GetConsoleMode(h, &mode)) {
        u16 wbuf[512];
        unsigned int rd = 0;
        i64 n;
        if (!ReadConsoleW(h, wbuf, 510, &rd, (void *)0)) return str_alloc(0);
        n = (i64)rd;
        while (n > 0 && (wbuf[n - 1] == L'\r' || wbuf[n - 1] == L'\n')) n--;
        return utf16_to_utf8(wbuf, n);
    }
    {
        i64 cap = 256, len = 0;
        char *buf = (char *)rt_alloc_bytes((u64)cap);
        RtStr *s;
        i64 i;
        while (1) {
            char c = 0;
            unsigned int rd = 0;
            int ok = ReadFile(h, &c, 1, &rd, (void *)0);
            if (!ok || rd == 0) break;      /* EOF */
            if (c == '\n') break;
            if (len >= cap) {
                i64 nc = cap * 2;
                char *nb = (char *)rt_alloc_bytes((u64)nc);
                for (i = 0; i < len; i++) nb[i] = buf[i];
                buf = nb;
                cap = nc;
            }
            buf[len++] = c;
        }
        while (len > 0 && buf[len - 1] == '\r') len--;
        s = str_alloc(len);
        for (i = 0; i < len; i++) s->data[i] = buf[i];
        return s;
    }
}

/* 输入整数：读一行并按整理解析（非法输入得 0） */
i64 rt_input_int(void) { return rt_str_to_int(rt_input_str()); }

/* 输入数：读一行并按小数解析（非法输入得 0） */
double rt_input_float(void) { return rt_str_to_float(rt_input_str()); }

/* ==================== 入口 ==================== */

void __ch_main(void);

/* 判断标准输出是否连着控制台。
 * Windows Terminal / conhost 的伪控制台句柄同样满足；管道或文件重定向时不满足，
 * 因此自动化测试（e2e 用管道/重定向）不会被暂停阻塞。 */
static int stdout_is_console(void) {
    void *h = GetStdHandle(STD_OUTPUT_HANDLE);
    unsigned int mode = 0;
    return h != (void *)0 && GetConsoleMode(h, &mode);
}

/* 等待用户按一次回车（或任意一个字符） */
static void wait_enter(void) {
    void *h = GetStdHandle(STD_INPUT_HANDLE);
    unsigned int mode = 0;
    if (h && GetConsoleMode(h, &mode)) {
        u16 wc[2];
        unsigned int rd = 0;
        ReadConsoleW(h, wc, 1, &rd, (void *)0);
    } else if (h) {
        char c = 0;
        unsigned int rd = 0;
        ReadFile(h, &c, 1, &rd, (void *)0);
    }
}

/* 正常结束：有控制台窗口时暂停，避免一闪而过 */
static void pause_if_own_console(void) {
    if (!stdout_is_console()) return;
    write_cstr("\n── 程序结束，按回车键退出 ──\n");
    wait_enter();
}

/* ==================== 崩溃保护 ==================== */

__declspec(dllimport) void *SetUnhandledExceptionFilter(void *f);

static void write_hex(unsigned int v) {
    char buf[9];
    int i;
    for (i = 7; i >= 0; i--) { buf[i] = "0123456789ABCDEF"[v & 0xF]; v >>= 4; }
    buf[8] = 0;
    write_cstr("0x");
    write_cstr(buf);
}

/* 程序异常（访问违规、除零等）时也把窗口留住，而不是直接闪退。
 * ExceptionPointers 的首字段是 ExceptionRecord，其首字段是 ExceptionCode。 */
static int crash_filter(void *ep) {
    void *rec = ep ? *(void **)ep : (void *)0;
    unsigned int code = rec ? *(unsigned int *)rec : 0;
    write_cstr("\n[汉语言] 程序异常终止，异常代码 ");
    write_hex(code);
    write_cstr("\n");
    if (stdout_is_console()) {
        write_cstr("── 按回车键退出 ──\n");
        wait_enter();
    }
    ExitProcess(1u);
    return 1;
}

void rt_entry(void) {
    SetConsoleOutputCP(65001u);
    SetConsoleCP(65001u);
    SetUnhandledExceptionFilter((void *)crash_filter);
    __ch_main();
    pause_if_own_console();
    ExitProcess(0u);
}
