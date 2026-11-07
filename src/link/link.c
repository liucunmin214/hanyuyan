#include "link/link.h"
#include "obj/obj.h"
#include <string.h>

#define IMAGE_BASE        0x140000000ULL
#define SEC_ALIGN         0x1000u
#define FILE_ALIGN        0x200u
#define OPT_HDR_SIZE      240
#define DOS_HDR_SIZE      0x40
#define PE_HDR_BASE       (DOS_HDR_SIZE + 4 + 20 + OPT_HDR_SIZE)  /* 0x148 */

typedef struct {
    ObjModule *m;
    uint64_t   base[SEC_COUNT];
} InMod;

typedef struct {
    const char *sym;    /* __imp_XXX */
    const char *fn;     /* XXX */
    uint32_t    hn_off; /* .idata 内偏移 */
    uint32_t    hn_rva;
    uint32_t    iat_rva;
} Import;

typedef struct {
    const char *name;
    ChBytes    *buf;
    uint32_t    vsize;
    uint32_t    rva;
    uint32_t    raw;
    uint32_t    rawsize;
    uint32_t    chars;
} OutSec;

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

static void set_err(char *err, size_t errlen, const char *msg) {
    if (err && errlen) {
        strncpy(err, msg, errlen - 1);
        err[errlen - 1] = 0;
    }
}

static ObjSym *defined_local(InMod *in, int i, const char *name) {
    ObjSym *s = ch_obj_find_sym(in[i].m, name);
    return (s && s->defined) ? s : NULL;
}

/* 先查重定位所在模块自己的符号表，再查其他模块（COFF 语义：模块内符号优先） */
static bool resolve_sym(InMod *in, int nmods, int from, const char *name,
                        int *tmod, int *tsec, uint64_t *tval) {
    ObjSym *s = defined_local(in, from, name);
    if (s) { *tmod = from; *tsec = s->sec; *tval = s->value; return true; }
    for (int j = 0; j < nmods; j++) {
        if (j == from) continue;
        s = defined_local(in, j, name);
        if (s) { *tmod = j; *tsec = s->sec; *tval = s->value; return true; }
    }
    return false;
}

static Import *find_import(ChVec *imports, const char *sym) {
    for (size_t i = 0; i < imports->len; i++)
        if (strcmp(CH_VEC_AT(*imports, Import, i).sym, sym) == 0)
            return &CH_VEC_AT(*imports, Import, i);
    return NULL;
}

static void pad_zeros(ChBytes *b, size_t align) {
    while (b->len % align) ch_bytes_u8(b, 0);
}

static void put_u32(ChBytes *b, size_t off, uint32_t v) {
    memcpy(b->data + off, &v, 4);
}
static void put_u64(ChBytes *b, size_t off, uint64_t v) {
    memcpy(b->data + off, &v, 8);
}

static uint32_t sec_rva(const OutSec *secs, int nsecs, int kind) {
    const char *want = kind == SEC_TEXT ? ".text"
                     : kind == SEC_RDATA ? ".rdata" : ".data";
    for (int i = 0; i < nsecs; i++)
        if (strcmp(secs[i].name, want) == 0) return secs[i].rva;
    return 0;
}

static ChBytes *sec_buf(OutSec *secs, int nsecs, int kind) {
    const char *want = kind == SEC_TEXT ? ".text"
                     : kind == SEC_RDATA ? ".rdata" : ".data";
    for (int i = 0; i < nsecs; i++)
        if (strcmp(secs[i].name, want) == 0) return secs[i].buf;
    return NULL;
}

/* 把 .idata 的偏移换算成 RVA 后回填描述符、ILT、IAT */
static void build_import_tables(ChBytes *idata, ChVec *imports,
                                uint32_t idata_rva,
                                size_t ilt_off, size_t iat_off, size_t dll_off) {
    size_t n = imports->len;
    if (n == 0) return;
    /* 只有一个导入 DLL（kernel32），因此只写一个描述符 + 终止项 */
    put_u32(idata, 0, (uint32_t)(idata_rva + ilt_off));
    put_u32(idata, 4, 0);
    put_u32(idata, 8, 0);
    put_u32(idata, 12, (uint32_t)(idata_rva + dll_off));
    put_u32(idata, 16, (uint32_t)(idata_rva + iat_off));
    for (size_t i = 0; i < n; i++) {
        Import *im = &CH_VEC_AT(*imports, Import, i);
        im->hn_rva = idata_rva + im->hn_off;
        im->iat_rva = idata_rva + iat_off + i * 8;
        put_u64(idata, ilt_off + i * 8, (uint64_t)im->hn_rva);
        put_u64(idata, iat_off + i * 8, (uint64_t)im->hn_rva);
    }
}

bool ch_link(ChArena *a, ObjModule **mods, int nmods, const char *entry_sym,
             const char *out_path, char *err, size_t errlen) {
    if (nmods <= 0) { set_err(err, errlen, "没有输入目标模块"); return false; }

    InMod *in = (InMod *)ch_arena_alloc(a, (size_t)nmods * sizeof(InMod));
    for (int i = 0; i < nmods; i++) {
        in[i].m = mods[i];
        memset(in[i].base, 0, sizeof(in[i].base));
    }

    /* ========== 1. 合并节 ========== */
    ChBytes text, rdata, data;
    ch_bytes_init(&text);
    ch_bytes_init(&rdata);
    ch_bytes_init(&data);

    for (int i = 0; i < nmods; i++) {
        ObjModule *m = in[i].m;
        /* 16 字节对齐：gcc -O2 生成的运行时会用 movdqa 等对齐 SSE 访问，
         * 不足 16 对齐会在运行期触发一般保护性异常 */
        pad_zeros(&text, 16);
        in[i].base[SEC_TEXT] = text.len;
        ch_bytes_append(&text, m->secs[SEC_TEXT].data.data, m->secs[SEC_TEXT].data.len);

        pad_zeros(&rdata, 16);
        in[i].base[SEC_RDATA] = rdata.len;
        ch_bytes_append(&rdata, m->secs[SEC_RDATA].data.data, m->secs[SEC_RDATA].data.len);

        pad_zeros(&data, 16);
        in[i].base[SEC_DATA] = data.len;
        ch_bytes_append(&data, m->secs[SEC_DATA].data.data, m->secs[SEC_DATA].data.len);

        /* .bss：在数据段尾部按零填充，只占内存不占文件 */
        pad_zeros(&data, 16);
        in[i].base[SEC_BSS] = data.len;
        for (uint32_t k = 0; k < m->secs[SEC_BSS].mem_size; k++) ch_bytes_u8(&data, 0);
    }
    pad_zeros(&text, 16);

    /* ========== 2. 收集未定义符号与导入 ========== */
    ChVec imports;
    ch_vec_init(&imports, sizeof(Import));

    for (int i = 0; i < nmods; i++) {
        ObjModule *m = in[i].m;
        for (size_t r = 0; r < m->relocs.len; r++) {
            ObjReloc *rel = &CH_VEC_AT(m->relocs, ObjReloc, r);
            int tm, ts; uint64_t tv;
            if (resolve_sym(in, nmods, i, rel->sym, &tm, &ts, &tv)) continue;
            if (strncmp(rel->sym, "__imp_", 6) == 0) {
                if (!find_import(&imports, rel->sym)) {
                    Import *im = CH_VEC_PUSH(imports, Import);
                    im->sym = rel->sym;
                    im->fn = ch_arena_strdup(a, rel->sym + 6);
                    im->hn_off = 0;
                    im->hn_rva = 0;
                    im->iat_rva = 0;
                }
            } else {
                char buf[160];
                snprintf(buf, sizeof(buf), "未定义的符号：%s", rel->sym);
                set_err(err, errlen, buf);
                return false;
            }
        }
    }

    /* ========== 3. 构造 .idata ========== */
    ChBytes idata;
    ch_bytes_init(&idata);
    size_t nimp = imports.len;
    size_t desc_bytes = 40;          /* 一个导入描述符（20）+ 终止项（20） */
    size_t ilt_off = 0, iat_off = 0, dll_off = 0;

    if (nimp) {
        for (size_t i = 0; i < desc_bytes; i++) ch_bytes_u8(&idata, 0);
        pad_zeros(&idata, 8);
        ilt_off = idata.len;
        for (size_t i = 0; i < (nimp + 1) * 8; i++) ch_bytes_u8(&idata, 0);
        pad_zeros(&idata, 8);
        iat_off = idata.len;
        for (size_t i = 0; i < (nimp + 1) * 8; i++) ch_bytes_u8(&idata, 0);

        for (size_t i = 0; i < nimp; i++) {
            Import *im = &CH_VEC_AT(imports, Import, i);
            pad_zeros(&idata, 2);
            im->hn_off = (uint32_t)idata.len;
            ch_bytes_u16(&idata, 0);                    /* Hint */
            ch_bytes_append(&idata, im->fn, strlen(im->fn));
            ch_bytes_u8(&idata, 0);                     /* 名字结尾 */
        }
        pad_zeros(&idata, 2);
        dll_off = idata.len;
        ch_bytes_append(&idata, "kernel32.dll", 12);    /* 含结尾 0 */
    }

    /* ========== 4. 版面：节 RVA 与文件偏移 ========== */
    OutSec secs[4];
    int nsecs = 0;
    if (text.len)  { secs[nsecs].name = ".text";  secs[nsecs].buf = &text;  secs[nsecs].chars = 0x60000020u; nsecs++; }
    if (rdata.len) { secs[nsecs].name = ".rdata"; secs[nsecs].buf = &rdata; secs[nsecs].chars = 0x40000040u; nsecs++; }
    if (data.len)  { secs[nsecs].name = ".data";  secs[nsecs].buf = &data;  secs[nsecs].chars = 0xC0000040u; nsecs++; }
    if (nimp)      { secs[nsecs].name = ".idata"; secs[nsecs].buf = &idata; secs[nsecs].chars = 0xC0000040u; nsecs++; }
    if (nsecs == 0) { set_err(err, errlen, "没有任何可输出的节"); return false; }

    uint32_t size_headers = (uint32_t)align_up(PE_HDR_BASE + (uint64_t)nsecs * 40, FILE_ALIGN);
    uint32_t rva = (uint32_t)align_up(size_headers, SEC_ALIGN);
    uint32_t raw = size_headers;
    for (int i = 0; i < nsecs; i++) {
        secs[i].vsize = (uint32_t)secs[i].buf->len;
        secs[i].rva = rva;
        secs[i].rawsize = (uint32_t)align_up(secs[i].vsize, FILE_ALIGN);
        secs[i].raw = raw;
        rva = (uint32_t)align_up((uint64_t)rva + secs[i].vsize, SEC_ALIGN);
        raw += secs[i].rawsize;
    }
    uint32_t size_image = rva;
    uint32_t idata_rva = 0;
    for (int i = 0; i < nsecs; i++)
        if (strcmp(secs[i].name, ".idata") == 0) idata_rva = secs[i].rva;

    if (nimp)
        build_import_tables(&idata, &imports, idata_rva,
                            ilt_off, iat_off, dll_off);

    /* ========== 5. 应用重定位 ========== */
    for (int i = 0; i < nmods; i++) {
        ObjModule *m = in[i].m;
        for (size_t r = 0; r < m->relocs.len; r++) {
            ObjReloc *rel = &CH_VEC_AT(m->relocs, ObjReloc, r);
            int tk = rel->sec;
            ChBytes *buf = sec_buf(secs, nsecs, tk == SEC_BSS ? SEC_DATA : tk);
            if (!buf) { set_err(err, errlen, "重定位目标节缺失"); return false; }
            uint64_t P = sec_rva(secs, nsecs, tk == SEC_BSS ? SEC_DATA : tk)
                       + in[i].base[tk] + rel->off;

            int64_t S;
            int tm, ts; uint64_t tv;
            if (resolve_sym(in, nmods, i, rel->sym, &tm, &ts, &tv)) {
                S = (int64_t)sec_rva(secs, nsecs, ts == SEC_BSS ? SEC_DATA : ts)
                  + (int64_t)in[tm].base[ts] + (int64_t)tv;
            } else {
                Import *im = find_import(&imports, rel->sym);
                if (!im) { set_err(err, errlen, "内部错误：导入符号丢失"); return false; }
                S = (int64_t)im->iat_rva;
            }

            size_t field = (size_t)in[i].base[tk] + rel->off;
            if (rel->type == REL_REL32) {
                if (field + 4 > buf->len) { set_err(err, errlen, "重定位越界"); return false; }
                int64_t d = S + rel->addend - ((int64_t)P + 4);
                int32_t d32 = (int32_t)d;
                memcpy(buf->data + field, &d32, 4);
            } else if (rel->type == REL_ABS64) {
                if (field + 8 > buf->len) { set_err(err, errlen, "重定位越界"); return false; }
                uint64_t v = (uint64_t)(S + rel->addend) + IMAGE_BASE;
                memcpy(buf->data + field, &v, 8);
            } else {
                if (field + 4 > buf->len) { set_err(err, errlen, "重定位越界"); return false; }
                uint32_t v = (uint32_t)(S + rel->addend);
                memcpy(buf->data + field, &v, 4);
            }
        }
    }

    /* ========== 6. 入口点 ========== */
    int em, es; uint64_t ev;
    if (!resolve_sym(in, nmods, nmods - 1, entry_sym, &em, &es, &ev)) {
        char buf[160];
        snprintf(buf, sizeof(buf), "找不到入口符号：%s", entry_sym);
        set_err(err, errlen, buf);
        return false;
    }
    uint32_t entry_rva = sec_rva(secs, nsecs, es == SEC_BSS ? SEC_DATA : es)
                       + (uint32_t)in[em].base[es] + (uint32_t)ev;

    /* ========== 7. 写出 PE32+ ========== */
    uint32_t size_code = 0, size_init = 0;
    uint32_t base_of_code = sec_rva(secs, nsecs, SEC_TEXT);
    for (int i = 0; i < nsecs; i++) {
        if (strcmp(secs[i].name, ".text") == 0) size_code += secs[i].rawsize;
        else size_init += secs[i].rawsize;
    }

    ChBytes f;
    ch_bytes_init(&f);

    unsigned char dos[DOS_HDR_SIZE];
    memset(dos, 0, sizeof(dos));
    dos[0] = 'M'; dos[1] = 'Z';
    dos[0x3C] = DOS_HDR_SIZE;
    ch_bytes_append(&f, dos, DOS_HDR_SIZE);
    ch_bytes_append(&f, "PE\0\0", 4);

    /* COFF 头 */
    ch_bytes_u16(&f, 0x8664);                 /* Machine: AMD64 */
    ch_bytes_u16(&f, (uint16_t)nsecs);
    ch_bytes_u32(&f, 0);
    ch_bytes_u32(&f, 0);
    ch_bytes_u32(&f, 0);
    ch_bytes_u16(&f, OPT_HDR_SIZE);
    ch_bytes_u16(&f, 0x0022);                 /* 可执行 | 大地址感知 */

    /* 可选头（PE32+） */
    ch_bytes_u16(&f, 0x20B);
    ch_bytes_u8(&f, 0);
    ch_bytes_u8(&f, 0);
    ch_bytes_u32(&f, size_code);
    ch_bytes_u32(&f, size_init);
    ch_bytes_u32(&f, 0);                      /* 未初始化数据 */
    ch_bytes_u32(&f, entry_rva);
    ch_bytes_u32(&f, base_of_code);
    ch_bytes_u64(&f, IMAGE_BASE);
    ch_bytes_u32(&f, SEC_ALIGN);
    ch_bytes_u32(&f, FILE_ALIGN);
    ch_bytes_u16(&f, 6); ch_bytes_u16(&f, 0);
    ch_bytes_u16(&f, 0); ch_bytes_u16(&f, 0);
    ch_bytes_u16(&f, 6); ch_bytes_u16(&f, 0);
    ch_bytes_u32(&f, 0);
    ch_bytes_u32(&f, size_image);
    ch_bytes_u32(&f, size_headers);
    ch_bytes_u32(&f, 0);
    ch_bytes_u16(&f, 3);                       /* 控制台子系统 */
    ch_bytes_u16(&f, 0);
    ch_bytes_u64(&f, 0x100000);                /* 栈保留 */
    ch_bytes_u64(&f, 0x1000);                  /* 栈提交 */
    ch_bytes_u64(&f, 0x100000);                /* 堆保留 */
    ch_bytes_u64(&f, 0x1000);                  /* 堆提交 */
    ch_bytes_u32(&f, 0);
    ch_bytes_u32(&f, 16);                      /* 数据目录数量 */
    for (int i = 0; i < 16; i++) {
        if (i == 1 && nimp) {
            ch_bytes_u32(&f, idata_rva);
            ch_bytes_u32(&f, (uint32_t)desc_bytes);
        } else {
            ch_bytes_u32(&f, 0);
            ch_bytes_u32(&f, 0);
        }
    }

    /* 节表 */
    for (int i = 0; i < nsecs; i++) {
        char name[8];
        memset(name, 0, sizeof(name));
        memcpy(name, secs[i].name, strlen(secs[i].name) < 8 ? strlen(secs[i].name) : 7);
        ch_bytes_append(&f, name, 8);
        ch_bytes_u32(&f, secs[i].vsize);
        ch_bytes_u32(&f, secs[i].rva);
        ch_bytes_u32(&f, secs[i].rawsize);
        ch_bytes_u32(&f, secs[i].raw);
        ch_bytes_u32(&f, 0);
        ch_bytes_u32(&f, 0);
        ch_bytes_u16(&f, 0);
        ch_bytes_u16(&f, 0);
        ch_bytes_u32(&f, secs[i].chars);
    }

    while (f.len < size_headers) ch_bytes_u8(&f, 0);

    /* 节数据 */
    for (int i = 0; i < nsecs; i++) {
        while (f.len < secs[i].raw) ch_bytes_u8(&f, 0);
        ch_bytes_append(&f, secs[i].buf->data, secs[i].vsize);
        while (f.len < (size_t)secs[i].raw + secs[i].rawsize) ch_bytes_u8(&f, 0);
    }

    FILE *fp = fopen(out_path, "wb");
    if (!fp) { set_err(err, errlen, "无法创建输出文件"); return false; }
    size_t wrote = fwrite(f.data, 1, f.len, fp);
    fclose(fp);
    if (wrote != f.len) { set_err(err, errlen, "写输出文件失败"); return false; }
    return true;
}
