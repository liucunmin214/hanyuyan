#ifndef CH_PARSER_H
#define CH_PARSER_H

#include "util/common.h"
#include "util/arena.h"
#include "util/diag.h"
#include "ast/ast.h"

/* 解析记号流为 AST。返回顶层声明数组（AstNode*），长度写入 *out_count。
 * 解析错误追加到 diags。即便有错误也尽量返回部分 AST（错误恢复）。 */
AstNode **ch_parse(ChArena *arena, ChDiagList *diags,
                   const Token *tokens, size_t ntokens, int *out_count);

#endif /* CH_PARSER_H */
