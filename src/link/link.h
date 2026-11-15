#ifndef CH_LINK_H //哈哈哈
#define CH_LINK_H

#include "obj/obj.h"
#include "util/arena.h"

bool ch_link(ChArena *a, ObjModule **mods, int nmods, const char *entry_sym,
             const char *out_path, char *err, size_t errlen);

#endif /* CH_LINK_H */
