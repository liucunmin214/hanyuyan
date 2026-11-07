#ifndef CH_COMMON_H
#define CH_COMMON_H

/* 汉语言 编译器公共基础设施 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 通用动态数组：按元素大小存储，追加时零初始化 */
typedef struct {
    void  *data;
    size_t len;
    size_t cap;
    size_t elem;
} ChVec;

void  ch_vec_init(ChVec *v, size_t elem_size);
void *ch_vec_push(ChVec *v);
void  ch_vec_free(ChVec *v);

#define CH_VEC_AT(v, T, i) ((T *)(v).data)[(i)]
#define CH_VEC_PUSH(v, T)  ((T *)ch_vec_push(&(v)))

#endif /* CH_COMMON_H */
