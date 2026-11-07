#include "coff/coff.h"
#include "util/arena.h"
#include <string.h>
#include <stdlib.h>

/* COFF 结构常量 */
#define COFF_HDR_SIZE  20
#define SEC_HDR_SIZE   40
#define SYM_REC_SIZE   18
#define REL_REC_SIZE   10

#define MACHINE_AMD64  0x8664
#define SYM_CLASS_FILE 103

/* 重定位类型（取值按 winnt.h 的 IMAGE_REL_AMD64_* 定义） */
#define REL_AMD64_ABSOLUTE 0x0000
#define REL_AMD64_ADDR64   0x0001
#define REL_AMD64_ADDR32NB 0x0003
#define REL_AMD64_REL32    0x0004
#define REL_AMD64_REL32_5  0x0009

static uint16_t rd16(const unsigned char *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int16_t rd16s(const unsigned char *p) { return (int16_t)rd16(p); }

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t rd32s(const unsigned char *p) { return (int32_t)rd32(p); }

static void set_err(char *err, size_t errlen, const char *msg) {
    if (err && errlen) {
        strncpy(err, msg, errlen - 1);
        err[errlen - 1] = 0;
    }
}

/* 从 8 字节名字段取名字。
 * sym_style 为真时表示符号表名字（前 4 字节全 0 则后 4 字节是字符串表偏移）；
 * 否则是节表名字（首字符 '/' 时其后是字符串表偏移的十进制文本）。 */
static const char *name_from_8(ChArena *a, const unsigned char *p,
                               const unsigned char *strtab, uint32_t strsize,
                               bool sym_style) {
    if (sym_style) {
        if (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 0) {
            uint32_t off = rd32(p + 4);
            if (strtab && off > 3 && off < strsize)
                return ch_arena_strdup(a, (const char *)(strtab + off));
            return "";
        }
    } else if (p[0] == '/') {
        uint32_t off = 0;
        for (int i = 1; i < 8 && p[i] >= '0' && p[i] <= '9'; i++)
            off = off * 10 + (uint32_t)(p[i] - '0');
        if (strtab && off > 3 && off < strsize)
            return ch_arena_strdup(a, (const char *)(strtab + off));
        return "";
    }
    char buf[9];
    memcpy(buf, p, 8);
    buf[8] = 0;
    return ch_arena_strdup(a, buf);
}

static void align_data(ChBytes *b, size_t align) {
    while (b->len % align) ch_bytes_u8(b, 0);
}

/* 节名 → 内部节号；无法识别的返回 -1（丢弃） */
static int map_section(const char *name) {
    if (strcmp(name, ".text") == 0) return SEC_TEXT;
    if (strcmp(name, ".rdata") == 0) return SEC_RDATA;
    if (strcmp(name, ".data") == 0) return SEC_DATA;
    if (strcmp(name, ".bss") == 0) return SEC_BSS;
    return -1;
}

bool ch_coff_read(ChArena *a, const unsigned char *data, size_t len,
                  ObjModule *out, char *err, size_t errlen) {
    if (len < COFF_HDR_SIZE) { set_err(err, errlen, "目标文件过短"); return false; }
    if (rd16(data) != MACHINE_AMD64) {
        set_err(err, errlen, "不是 x86-64 COFF 目标文件");
        return false;
    }

    int nsec = (int)rd16(data + 2);
    uint32_t symoff = rd32(data + 8);
    uint32_t nsyms = rd32(data + 12);
    int optsz = (int)rd16(data + 16);
    size_t shoff = COFF_HDR_SIZE + (size_t)optsz;
    if (shoff + (size_t)nsec * SEC_HDR_SIZE > len) {
        set_err(err, errlen, "COFF 节表越界");
        return false;
    }
    if (symoff && (size_t)symoff + (size_t)nsyms * SYM_REC_SIZE > len) {
        set_err(err, errlen, "COFF 符号表越界");
        return false;
    }

    /* 字符串表位于符号表之后，前 4 字节为总长度 */
    const unsigned char *strtab = NULL;
    uint32_t strsize = 0;
    if (symoff && nsyms) {
        size_t st = (size_t)symoff + (size_t)nsyms * SYM_REC_SIZE;
        if (st + 4 <= len) {
            strsize = rd32(data + st);
            if (strsize >= 4 && st + strsize <= len) strtab = data + st;
            else strsize = 0;
        }
    }

    ch_obj_init(out);

    int *kinds = (int *)ch_arena_alloc(a, (size_t)nsec * sizeof(int));
    uint64_t *bases = (uint64_t *)ch_arena_alloc(a, (size_t)nsec * sizeof(uint64_t));
    if (nsec) memset(kinds, 0, (size_t)nsec * sizeof(int));

    /* 第一节：复制节数据 */
    for (int i = 0; i < nsec; i++) {
        const unsigned char *sh = data + shoff + (size_t)i * SEC_HDR_SIZE;
        const char *name = name_from_8(a, sh, strtab, strsize, false);
        int k = map_section(name);
        kinds[i] = k;
        bases[i] = 0;
        if (k < 0) continue;

        uint32_t vsize = rd32(sh + 8);
        uint32_t rsize = rd32(sh + 16);
        uint32_t rptr = rd32(sh + 20);
        ObjSection *sec = &out->secs[k];

        if (k == SEC_BSS) {
            sec->mem_size += (vsize ? vsize : rsize);
            bases[i] = sec->data.len;
            continue;
        }
        if (rsize == 0) { bases[i] = sec->data.len; continue; }
        if ((size_t)rptr + rsize > len) {
            set_err(err, errlen, "COFF 节数据越界");
            return false;
        }
        align_data(&sec->data, 8);
        bases[i] = sec->data.len;
        ch_bytes_append(&sec->data, data + rptr, rsize);
        sec->mem_size = (uint32_t)sec->data.len;
    }

    /* 第二节：重定位记录 */
    for (int i = 0; i < nsec; i++) {
        const unsigned char *sh = data + shoff + (size_t)i * SEC_HDR_SIZE;
        int k = kinds[i];
        if (k < 0 || k == SEC_BSS) continue;
        uint32_t relptr = rd32(sh + 24);
        uint32_t nrel = rd16(sh + 32);
        if (nrel == 0) continue;
        if ((size_t)relptr + (size_t)nrel * REL_REC_SIZE > len) {
            set_err(err, errlen, "COFF 重定位表越界");
            return false;
        }
        ObjSection *sec = &out->secs[k];
        for (uint32_t r = 0; r < nrel; r++) {
            const unsigned char *rec = data + relptr + (size_t)r * REL_REC_SIZE;
            uint32_t va = rd32(rec);
            uint32_t symidx = rd32(rec + 4);
            uint16_t type = rd16(rec + 8);
            if (type == REL_AMD64_ABSOLUTE) continue;
            if (symidx >= nsyms) {
                set_err(err, errlen, "COFF 重定位引用了无效符号");
                return false;
            }
            const unsigned char *sr = data + symoff + (size_t)symidx * SYM_REC_SIZE;
            const char *sname = name_from_8(a, sr, strtab, strsize, true);
            if (!sname || !sname[0]) continue;

            RelType rt;
            int64_t addend = 0;
            size_t field = (size_t)bases[i] + va;
            if (type >= REL_AMD64_REL32 && type <= REL_AMD64_REL32_5) {
                if (field + 4 > sec->data.len) {
                    set_err(err, errlen, "COFF 重定位字段越界");
                    return false;
                }
                addend = (int64_t)rd32s(sec->data.data + field) + (int64_t)(type - REL_AMD64_REL32);
                rt = REL_REL32;
            } else if (type == REL_AMD64_ADDR64) {
                if (field + 8 > sec->data.len) {
                    set_err(err, errlen, "COFF 重定位字段越界");
                    return false;
                }
                memcpy(&addend, sec->data.data + field, 8);
                rt = REL_ABS64;
            } else if (type == REL_AMD64_ADDR32NB) {
                if (field + 4 > sec->data.len) {
                    set_err(err, errlen, "COFF 重定位字段越界");
                    return false;
                }
                addend = (int64_t)rd32s(sec->data.data + field);
                rt = REL_ABS32NB;
            } else {
                set_err(err, errlen, "不支持的重定位类型");
                return false;
            }
            ch_obj_reloc(out, k, (uint32_t)va, rt,
                         ch_arena_strdup(a, sname), addend);
        }
    }

    /* 第三节：符号表（跳过辅助记录） */
    for (uint32_t idx = 0; idx < nsyms; ) {
        const unsigned char *s = data + symoff + (size_t)idx * SYM_REC_SIZE;
        int16_t secnum = rd16s(s + 12);
        unsigned char scl = s[16];
        unsigned char naux = s[17];
        idx += 1u + (uint32_t)naux;

        if (scl == SYM_CLASS_FILE || secnum == -2) continue;
        const char *name = name_from_8(a, s, strtab, strsize, true);
        if (!name || !name[0]) continue;

        if (secnum == 0) {
            ch_obj_extern(out, name);
        } else if (secnum > 0 && secnum <= nsec && kinds[secnum - 1] >= 0) {
            uint64_t value = bases[secnum - 1] + rd32(s + 8);
            ch_obj_define(out, name, kinds[secnum - 1], value);
        }
    }

    return true;
}
