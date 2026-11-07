#ifndef CH_OBJ_H
#define CH_OBJ_H

#include "util/common.h"
#include "util/arena.h"

/* 内部目标对象模型：代码生成与 COFF 读取都产出这个结构，链接器消费它。
 * 段固定为 .text/.rdata/.data/.bss 四类，便于合并与布局。 */

typedef struct {
    unsigned char *data;
    size_t         len;
    size_t         cap;
} ChBytes;

void ch_bytes_init(ChBytes *b);
void ch_bytes_append(ChBytes *b, const void *p, size_t n);
void ch_bytes_u8(ChBytes *b, unsigned char v);
void ch_bytes_u16(ChBytes *b, uint16_t v);
void ch_bytes_u32(ChBytes *b, uint32_t v);
void ch_bytes_i32(ChBytes *b, int32_t v);
void ch_bytes_u64(ChBytes *b, uint64_t v);

typedef enum {
    SEC_TEXT = 0,   /* 可执行只读 */
    SEC_RDATA,      /* 只读数据 */
    SEC_DATA,       /* 可读写数据 */
    SEC_BSS,        /* 零初始化 */
    SEC_COUNT
} SecKind;

typedef enum {
    REL_REL32 = 0,  /* 相对下一条指令的 32 位偏移 */
    REL_ABS64,      /* 绝对 64 位地址（含镜像基址） */
    REL_ABS32NB     /* 32 位 RVA */
} RelType;

typedef struct {
    const char *name;
    int         sec;      /* 段号；-1 表示外部符号 */
    uint64_t    value;    /* 段内偏移 */
    bool        defined;
} ObjSym;

typedef struct {
    int         sec;
    uint32_t    off;      /* 段内偏移（重定位字段位置） */
    RelType     type;
    const char *sym;
    int64_t     addend;
} ObjReloc;

typedef struct {
    ChBytes  data;
    uint32_t mem_size;    /* 段内存大小（BSS 用） */
} ObjSection;

typedef struct {
    ObjSection secs[SEC_COUNT];
    ChVec      syms;      /* ObjSym */
    ChVec      relocs;    /* ObjReloc */
} ObjModule;

void ch_obj_init(ObjModule *m);
ObjSym *ch_obj_find_sym(ObjModule *m, const char *name);
ObjSym *ch_obj_define(ObjModule *m, const char *name, int sec, uint64_t value);
ObjSym *ch_obj_extern(ObjModule *m, const char *name);
void    ch_obj_reloc(ObjModule *m, int sec, uint32_t off, RelType type,
                     const char *sym, int64_t addend);

#endif /* CH_OBJ_H */
