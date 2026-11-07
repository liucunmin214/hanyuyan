#ifndef CH_LEXER_H
#define CH_LEXER_H

#include "util/common.h"
#include "util/arena.h"
#include "util/diag.h"
#include "lexer/token.h"

/* 对规范化后的源码做词法分析，记号追加到 out（元素类型 Token）。
 * 返回 true 表示没有错误。 */
bool ch_lex(ChArena *arena, ChDiagList *diags, const char *src, size_t len, ChVec *out);

#endif /* CH_LEXER_H */
