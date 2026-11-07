#ifndef CH_ARENA_H
#define CH_ARENA_H

#include <stddef.h>

/* 只增不删的区域分配器：编译期所有短生命周期对象都放在这里 */
typedef struct ChArena ChArena;

/* 类型驻留表（arena 本地，随 arena 一起销毁） */
typedef struct ChType ChType;
typedef struct {
    ChType *data;
    int     len;
    int     cap;
} ChTypeTable;

ChArena *ch_arena_new(void);
void     ch_arena_destroy(ChArena *a);

void    *ch_arena_alloc(ChArena *a, size_t n);
char    *ch_arena_strdup(ChArena *a, const char *s);
char    *ch_arena_strndup(ChArena *a, const char *s, size_t n);

/* 获取 arena 本地的类型驻留表 */
ChTypeTable *ch_arena_type_table(ChArena *a);

#endif /* CH_ARENA_H */
