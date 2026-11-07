#ifndef CH_X64_H
#define CH_X64_H

#include "ir/ir.h"
#include "obj/obj.h"
#include "util/arena.h"

/* x86-64 后端：把 IR 翻译成机器码，产出内部目标对象。
 * 寄存器分配采用「栈槽分配 + 寄存器复用」：每个 IR 临时值与局部变量各占
 * 一个 8 字节栈槽，运算时载入 rax/rcx 或 xmm 寄存器，结果写回栈槽。
 * 调用约定遵循 Microsoft x64（rcx/rdx/r8/r9 + shadow space）。 */
ObjModule *ch_x64_gen(ChArena *arena, const IrModule *mod);

#endif /* CH_X64_H */
