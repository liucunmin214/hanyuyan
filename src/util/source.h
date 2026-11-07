#ifndef CH_SOURCE_H
#define CH_SOURCE_H

#include "util/common.h"
#include "util/arena.h"

/* 规范化后的源码（UTF-8、已去 BOM、CRLF/CR 已统一为 LF） */
typedef struct {
    char  *text;
    size_t len;
} ChSource;

/* 读取文件 → 规范化 → 存入区域分配器 */
bool ch_source_read(const char *path, ChArena *a, ChSource *out, char *err, size_t errcap);

/* 去 BOM、统一换行；返回区域内 NUL 结尾的缓冲区 */
char *ch_source_normalize(const char *bytes, size_t len, size_t *out_len, ChArena *a);

/* 校验 UTF-8；失败时*bad_off 为第一个非法字节偏移 */
bool ch_utf8_validate(const char *s, size_t len, size_t *bad_off);

#endif /* CH_SOURCE_H */
