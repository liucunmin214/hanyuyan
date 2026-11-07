#include "obj/obj.h"
#include <string.h>

void ch_bytes_init(ChBytes *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static void grow(ChBytes *b, size_t need) {
    if (b->len + need <= b->cap) return;
    size_t nc = b->cap ? b->cap * 2 : 256;
    while (nc < b->len + need) nc *= 2;
    b->data = (unsigned char *)realloc(b->data, nc);
    if (!b->data) {
        fprintf(stderr, "内存不足：目标代码缓冲扩展失败\n");
        exit(2);
    }
    b->cap = nc;
}

void ch_bytes_append(ChBytes *b, const void *p, size_t n) {
    grow(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

void ch_bytes_u8(ChBytes *b, unsigned char v) { ch_bytes_append(b, &v, 1); }

void ch_bytes_u16(ChBytes *b, uint16_t v) { ch_bytes_append(b, &v, 2); }

void ch_bytes_u32(ChBytes *b, uint32_t v) { ch_bytes_append(b, &v, 4); }

void ch_bytes_i32(ChBytes *b, int32_t v) { ch_bytes_append(b, &v, 4); }

void ch_bytes_u64(ChBytes *b, uint64_t v) { ch_bytes_append(b, &v, 8); }

void ch_obj_init(ObjModule *m) {
    memset(m, 0, sizeof(*m));
    for (int i = 0; i < SEC_COUNT; i++) ch_bytes_init(&m->secs[i].data);
    ch_vec_init(&m->syms, sizeof(ObjSym));
    ch_vec_init(&m->relocs, sizeof(ObjReloc));
}

ObjSym *ch_obj_find_sym(ObjModule *m, const char *name) {
    for (size_t i = 0; i < m->syms.len; i++) {
        ObjSym *s = &CH_VEC_AT(m->syms, ObjSym, i);
        if (strcmp(s->name, name) == 0) return s;
    }
    return NULL;
}

ObjSym *ch_obj_define(ObjModule *m, const char *name, int sec, uint64_t value) {
    ObjSym *s = ch_obj_find_sym(m, name);
    if (!s) s = CH_VEC_PUSH(m->syms, ObjSym);
    s->name = name;
    s->sec = sec;
    s->value = value;
    s->defined = true;
    return s;
}

ObjSym *ch_obj_extern(ObjModule *m, const char *name) {
    ObjSym *s = ch_obj_find_sym(m, name);
    if (s) return s;
    s = CH_VEC_PUSH(m->syms, ObjSym);
    s->name = name;
    s->sec = -1;
    s->value = 0;
    s->defined = false;
    return s;
}

void ch_obj_reloc(ObjModule *m, int sec, uint32_t off, RelType type,
                  const char *sym, int64_t addend) {
    ObjReloc *r = CH_VEC_PUSH(m->relocs, ObjReloc);
    r->sec = sec;
    r->off = off;
    r->type = type;
    r->sym = sym;
    r->addend = addend;
}
