#include "com.h"

#include <stdlib.h>
#include <string.h>

#define MAX_OBJS 65536
static X2ComObj *objs[MAX_OBJS];
static u32 free_hint = 1;
static pthread_mutex_t com_lock = PTHREAD_MUTEX_INITIALIZER;

u32 x2_com_vtable(const char *iface, const X2Method *m, int count) {
    u32 vt = x2_galloc((u32)count * 4);
    for (int i = 0; i < count; i++) {
        char name[160];
        snprintf(name, sizeof name, "%s::%s", iface, m[i].name ? m[i].name : "?");
        WR32(vt + 4 * (u32)i, x2_thunk_new(strdup(name), m[i].fn, m[i].nargs));
    }
    return vt;
}

u32 x2_com_wrap(X2ComObj *o, u32 vtable, const char *iface) {
    pthread_mutex_lock(&com_lock);
    u32 id = 0;
    for (u32 n = 0; n < MAX_OBJS; n++) {
        u32 i = (free_hint + n) % MAX_OBJS;
        if (i && !objs[i]) { id = i; break; }
    }
    if (!id) x2_fatal("out of COM object slots");
    objs[id] = o;
    free_hint = id + 1;
    pthread_mutex_unlock(&com_lock);
    o->guest = x2_galloc(8);
    o->refs = 1;
    o->iface = iface;
    WR32(o->guest, vtable);
    WR32(o->guest + 4, id);
    return o->guest;
}

X2ComObj *x2_com_get(u32 g) {
    if (!g) return NULL;
    u32 id = RD32(g + 4);
    if (!id || id >= MAX_OBJS) return NULL;
    X2ComObj *o = objs[id];
    return o && o->guest == g ? o : NULL;
}

void x2_com_AddRef(X2Cpu *c) {
    X2ComObj *o = x2_com_get(ARG(0));
    RET(o ? ++o->refs : 0);
}

void x2_com_Release(X2Cpu *c) {
    X2ComObj *o = x2_com_get(ARG(0));
    if (!o) { RET(0); return; }
    if (o->immortal) { RET(o->refs > 1 ? --o->refs : 1); return; }
    u32 r = --o->refs;
    if (r == 0) {
        u32 g = o->guest;
        u32 id = RD32(g + 4);
        pthread_mutex_lock(&com_lock);
        objs[id] = NULL;
        pthread_mutex_unlock(&com_lock);
        WR32(g + 4, 0);
        if (o->destroy) o->destroy(o);
        else free(o);
        x2_heap_free(0, g);
    }
    RET(r);
}

void x2_com_QueryInterface_self(X2Cpu *c) {
    X2ComObj *o = x2_com_get(ARG(0));
    if (!o || !ARG(2)) { RET(E_NOINTERFACE); return; }
    o->refs++;
    WR32(ARG(2), ARG(0));
    RET(S_OK);
}
