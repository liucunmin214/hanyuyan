#ifndef CH_KEYWORDS_H
#define CH_KEYWORDS_H

#include "lexer/token.h"

/* 查找关键字（含同义词）；不是关键字时返回 TK_IDENT */
TokenKind ch_keyword_lookup(const char *word);

#endif /* CH_KEYWORDS_H */
