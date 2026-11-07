#ifndef CH_DIAG_H
#define CH_DIAG_H

#include "util/common.h"
#include "util/arena.h"
#include "util/source.h"

typedef struct {
    bool        is_error;
    int         line;
    int         col;
    const char *msg;
} ChDiag;

typedef struct {
    ChVec    items;
    ChArena *arena;
} ChDiagList;

void ch_diag_init(ChDiagList *d, ChArena *a);
void ch_diag_add(ChDiagList *d, bool is_error, int line, int col, const char *fmt, ...);
int  ch_diag_error_count(const ChDiagList *d);

/* 打印为：路径:行:列: 错误: 消息 + 源码片段 + 插入符 */
void ch_diag_print(const ChDiagList *d, const ChSource *src, const char *path, FILE *out);

#endif /* CH_DIAG_H */
