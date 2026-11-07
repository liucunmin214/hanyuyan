#ifndef CH_COFF_H
#define CH_COFF_H

#include "obj/obj.h"
#include "util/arena.h"

/* COFF（PE-x86-64 目标文件）读取器
 *
 * 读取运行时目标文件，转换成内部 ObjModule：
 *   .text -> SEC_TEXT，.rdata -> SEC_RDATA，.data -> SEC_DATA，.bss -> SEC_BSS
 *   其余节（.rdata$zzz / .pdata / .xdata / .debug*）丢弃
 * 支持的重定位：REL32（含 REL32_1..5）、ADDR64、ADDR32NB。
 *
 * 成功返回 true；失败时把原因写入 err 并返回 false。
 */
bool ch_coff_read(ChArena *a, const unsigned char *data, size_t len,
                  ObjModule *out, char *err, size_t errlen);

#endif /* CH_COFF_H */
