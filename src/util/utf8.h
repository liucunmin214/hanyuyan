#ifndef CH_UTF8_H
#define CH_UTF8_H

#include <stdbool.h>
#include <stddef.h>

/* 严格解码：成功返回消耗的字节数（1..4），失败返回 0 */
int ch_utf8_decode(const char *s, size_t avail, int *cp_out);

/* 编码一个码点，返回写入的字节数（1..4） */
int ch_utf8_encode(int cp, char out[4]);

bool ch_utf8_is_cjk(int cp);
bool ch_utf8_is_ident_start(int cp);
bool ch_utf8_is_ident_cont(int cp);

#endif /* CH_UTF8_H */
