#include "util/arena.h"
#include "util/common.h"

#define CH_ARENA_BLOCK (64u * 1024u)

typedef struct Block {
    struct Block *next;
    size_t used;
    size_t cap;
    char   data[];
} Block;

struct ChArena {
    Block       *head;
    ChTypeTable  types;   /* 类型驻留表（arena 本地） */
};

static Block *ch_block_new(size_t cap) {
    Block *b = (Block *)malloc(sizeof(Block) + cap);
    if (!b) {
        fprintf(stderr, "内存不足：区域分配器无法申请内存\n");
        exit(2);
    }
    b->next = NULL;
    b->used = 0;
    b->cap = cap;
    return b;
}

ChArena *ch_arena_new(void) {
    ChArena *a = (ChArena *)malloc(sizeof(ChArena));
    if (!a) {
        fprintf(stderr, "内存不足：区域分配器初始化失败\n");
        exit(2);
    }
    a->head = NULL;
    a->types.data = NULL;
    a->types.len = 0;
    a->types.cap = 0;
    return a;
}

void ch_arena_destroy(ChArena *a) {
    if (!a) return;
    Block *b = a->head;
    while (b) {
        Block *n = b->next;
        free(b);
        b = n;
    }
    free(a->types.data);
    free(a);
}

void *ch_arena_alloc(ChArena *a, size_t n) {
    if (n == 0) n = 1;
    n = (n + 15u) & ~(size_t)15u;
    if (!a->head || a->head->used + n > a->head->cap) {
        size_t cap = n > CH_ARENA_BLOCK ? n : CH_ARENA_BLOCK;
        Block *b = ch_block_new(cap);
        b->next = a->head;
        a->head = b;
    }
    char *p = a->head->data + a->head->used;
    a->head->used += n;
    memset(p, 0, n);
    return p;
}

char *ch_arena_strndup(ChArena *a, const char *s, size_t n) {
    char *p = (char *)ch_arena_alloc(a, n + 1);
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

char *ch_arena_strdup(ChArena *a, const char *s) {
    return ch_arena_strndup(a, s, strlen(s));
}

ChTypeTable *ch_arena_type_table(ChArena *a) {
    return &a->types;
}
