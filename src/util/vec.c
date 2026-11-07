#include "util/common.h"

void ch_vec_init(ChVec *v, size_t elem_size) {
    v->data = NULL;
    v->len = 0;
    v->cap = 0;
    v->elem = elem_size;
}

void *ch_vec_push(ChVec *v) {
    if (v->len == v->cap) {
        size_t ncap = v->cap ? v->cap * 2 : 8;
        void *p = realloc(v->data, ncap * v->elem);
        if (!p) {
            fprintf(stderr, "内存不足：动态数组扩容失败\n");
            exit(2);
        }
        v->data = p;
        v->cap = ncap;
    }
    void *slot = (char *)v->data + v->len * v->elem;
    memset(slot, 0, v->elem);
    v->len++;
    return slot;
}

void ch_vec_free(ChVec *v) {
    free(v->data);
    ch_vec_init(v, v->elem);
}
