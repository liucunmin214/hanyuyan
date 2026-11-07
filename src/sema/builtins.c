#include "sema/builtins.h"
#include "sema/symbol.h"
#include <string.h>

/* 内建函数表：语言名 -> 运行时符号
 * 参数/返回用 TypeKind 描述；TY_UNKNOWN 表示任意类型。
 * sym 为空表示该函数需在 IR 生成阶段做按类型分派（如 转文字）。 */
static const ChBuiltin g_builtins[] = {
    /* 文字 */
    { "文字长度",   "rt_str_len",      1, { TY_STRING }, TY_INT },
    { "文字子串",   "rt_str_sub",      3, { TY_STRING, TY_INT, TY_INT }, TY_STRING },
    { "文字包含",   "rt_str_contains", 2, { TY_STRING, TY_STRING }, TY_BOOL },
    { "文字转整数", "rt_str_to_int",   1, { TY_STRING }, TY_INT },
    { "文字转数",   "rt_str_to_float", 1, { TY_STRING }, TY_FLOAT },
    { "转文字",     "",                1, { TY_UNKNOWN }, TY_STRING },

    /* 键盘输入（读一行，无提示符；提示请先用 打印） */
    { "输入文字",   "rt_input_str",    0, { TY_VOID }, TY_STRING },
    { "输入整数",   "rt_input_int",    0, { TY_VOID }, TY_INT },
    { "输入数",     "rt_input_float",  0, { TY_VOID }, TY_FLOAT },

    /* 列表 */
    { "列表创建",   "rt_list_new",     0, { TY_VOID }, TY_LIST },
    { "列表新增",   "rt_list_push",    2, { TY_LIST, TY_UNKNOWN }, TY_VOID },
    { "列表长度",   "rt_list_len",     1, { TY_LIST }, TY_INT },
    { "列表取",     "rt_list_get",     2, { TY_LIST, TY_INT }, TY_UNKNOWN },
    { "列表取整数", "rt_list_get",     2, { TY_LIST, TY_INT }, TY_INT },
    { "列表取文字", "rt_list_get",     2, { TY_LIST, TY_INT }, TY_STRING },
    { "列表取数",   "rt_list_get_f64", 2, { TY_LIST, TY_INT }, TY_FLOAT },
    { "列表设",     "rt_list_set",     3, { TY_LIST, TY_INT, TY_UNKNOWN }, TY_VOID },
    { "列表删除",   "rt_list_del",     2, { TY_LIST, TY_INT }, TY_VOID },

    /* 字典 */
    { "字典创建",   "rt_dict_new",     0, { TY_VOID }, TY_DICT },
    { "字典设",     "rt_dict_set",     3, { TY_DICT, TY_STRING, TY_UNKNOWN }, TY_VOID },
    { "字典取",     "rt_dict_get",     2, { TY_DICT, TY_STRING }, TY_UNKNOWN },
    { "字典取整数", "rt_dict_get",     2, { TY_DICT, TY_STRING }, TY_INT },
    { "字典取文字", "rt_dict_get",     2, { TY_DICT, TY_STRING }, TY_STRING },
    { "字典取数",   "rt_dict_get_f64", 2, { TY_DICT, TY_STRING }, TY_FLOAT },
    { "字典有",     "rt_dict_has",     2, { TY_DICT, TY_STRING }, TY_BOOL },
    { "字典删",     "rt_dict_del",     2, { TY_DICT, TY_STRING }, TY_BOOL },
    { "字典长度",   "rt_dict_len",     1, { TY_DICT }, TY_INT },
    { "字典键",     "rt_dict_keys",    1, { TY_DICT }, TY_LIST },

    /* 文件 */
    { "读文件",     "rt_read_file",    1, { TY_STRING }, TY_STRING },
    { "写文件",     "rt_write_file",   2, { TY_STRING, TY_STRING }, TY_BOOL },
    { "追加文件",   "rt_append_file",  2, { TY_STRING, TY_STRING }, TY_BOOL },
    { "文件存在",   "rt_file_exists",  1, { TY_STRING }, TY_BOOL },

    /* 时间 */
    { "现在",       "rt_now_text",     0, { TY_VOID }, TY_STRING },
    { "现在毫秒",   "rt_now_ms",       0, { TY_VOID }, TY_INT },
};

#define NB ((int)(sizeof(g_builtins) / sizeof(g_builtins[0])))

const ChBuiltin *ch_builtin_find(const char *name) {
    for (int i = 0; i < NB; i++)
        if (strcmp(g_builtins[i].name, name) == 0) return &g_builtins[i];
    return NULL;
}

ChType *ch_builtin_func_type(ChArena *a, const ChBuiltin *b) {
    if (b->nparams == 0)
        return ch_type_func(a, NULL, 0, false, ch_type_get(a, b->ret));
    ChType **ps = ch_arena_alloc(a, sizeof(ChType *) * (size_t)b->nparams);
    for (int i = 0; i < b->nparams; i++)
        ps[i] = ch_type_get(a, b->params[i]);
    return ch_type_func(a, ps, b->nparams, false, ch_type_get(a, b->ret));
}

const char *ch_builtin_print_sym(const ChType *t) {
    if (!t) return "rt_print_int";
    switch (t->kind) {
    case TY_FLOAT:  return "rt_print_float";
    case TY_STRING: return "rt_print_str";
    case TY_BOOL:   return "rt_print_bool";
    case TY_NIL:    return "rt_print_nil";
    case TY_FUNC:   return "rt_print_func";
    default:        return "rt_print_int";
    }
}
