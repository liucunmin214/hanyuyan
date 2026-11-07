#ifndef CH_SEMA_H
#define CH_SEMA_H

#include "ast/ast.h"
#include "util/arena.h"
#include "util/diag.h"

/* 对 AST 做语义与类型检查，填充所有 Expr::type。
 * 返回 true 表示无错误。 */
bool ch_sema_check(ChArena *arena, ChDiagList *diags, AstNode **decls, int ndecls);

#endif /* CH_SEMA_H */
