#ifndef CH_LINK_H
#define CH_LINK_H

#include "obj/obj.h"
#include "util/arena.h"

/* 链接器：把多个内部目标模块合并成一个独立的 PE32+ 可执行文件。
 *
 * 处理内容：
 *   1. 合并 .text/.rdata/.data/.bss，并对模块内偏移做重定位
 *   2. 把 __imp_XXX 未定义符号生成 kernel32.dll 导入表（ILT/IAT/Hint-Name）
 *   3. 按 PE32+ 规范写出 DOS 头、COFF 头、可选头、节表与节数据
 *
 * 生成的 exe 只导入 kernel32，不依赖 CRT 与任何运行时 DLL。
 */
bool ch_link(ChArena *a, ObjModule **mods, int nmods, const char *entry_sym,
             const char *out_path, char *err, size_t errlen);

#endif /* CH_LINK_H */
