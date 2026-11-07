#include "sema/type.h"
#include <string.h>

const char *ch_type_name(const ChType *t) {
    if (!t) return "未知";
    switch (t->kind) {
    case TY_VOID:    return "空";
    case TY_INT:     return "整数";
    case TY_FLOAT:   return "小数";
    case TY_STRING:  return "文字";
    case TY_BOOL:    return "布尔";
    case TY_LIST:    return "列表";
    case TY_DICT:    return "字典";
    case TY_FUNC:    return "函数";
    case TY_NIL:     return "空值";
    case TY_UNKNOWN: return "未知";
    case TY_ERROR:   return "错误类型";
    }
    return "?";
}

bool ch_type_equal(const ChType *a, const ChType *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    if (a->kind == TY_LIST)
        return ch_type_equal(a->elem, b->elem);
    if (a->kind == TY_FUNC) {
        if (a->nparams != b->nparams) return false;
        if (a->is_variadic != b->is_variadic) return false;
        for (int i = 0; i < a->nparams; i++)
            if (!ch_type_equal(a->params[i], b->params[i])) return false;
        return ch_type_equal(a->elem, b->elem);
    }
    return true;
}

bool ch_type_is_numeric(const ChType *t) {
    return t && (t->kind == TY_INT || t->kind == TY_FLOAT);
}
