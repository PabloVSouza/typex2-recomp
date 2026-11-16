#include "com.h"

#include <stdlib.h>
#include <string.h>

#define MAX_OBJS 65536
static X2ComObj *objs[MAX_OBJS];
static u32 free_hint = 1;
static pthread_mutex_t com_lock = PTHREAD_MUTEX_INITIALIZER;

/* released objects kept for save states (see X2ComObj.keep), oldest first */
#define MAX_ZOMBIES 4096
static bool keep_released; /* a state has been taken */
static X2ComObj *zhead, *ztail;
static u32 nzombies;
static void zombie_add(X2ComObj *o) { /* under com_lock */
    o->zombie = true;
    o->znext = NULL;
    if (ztail)
        ztail->znext = o;
    else
        zhead = o;
    ztail = o;
    nzombies++;
}
static void zombie_remove(X2ComObj *o) { /* under com_lock */
    X2ComObj *prev = NULL;
    for (X2ComObj *z = zhead; z; prev = z, z = z->znext)
        if (z == o) {
            if (prev)
                prev->znext = z->znext;
            else
                zhead = z->znext;
            if (ztail == z) ztail = prev;
            nzombies--;
            break;
        }
    o->zombie = false;
}
/* the oldest zombie beyond the limit, taken out of its slot; the caller destroys it */
static X2ComObj *zombie_evict(void) { /* under com_lock */
    if (nzombies <= MAX_ZOMBIES || !zhead) return NULL;
    X2ComObj *o = zhead;
    zombie_remove(o);
    for (u32 i = 1; i < MAX_OBJS; i++)
        if (objs[i] == o) {
            objs[i] = NULL;
            break;
        }
    return o;
}

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
    return o && !o->zombie && o->guest == g ? o : NULL;
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
        X2ComObj *gone = o;
        pthread_mutex_lock(&com_lock);
        if (o->keep && keep_released) { /* kept for a state taken earlier; its slot stays taken */
            zombie_add(o);
            gone = zombie_evict();
        } else {
            objs[id] = NULL;
        }
        pthread_mutex_unlock(&com_lock);
        WR32(g + 4, 0);
        if (o->keep && o->free_guest) o->free_guest(o);
        if (o->keep && o->retire && gone != o) o->retire(o);
        if (gone) {
            if (gone->destroy)
                gone->destroy(gone);
            else
                free(gone);
        }
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

/* ---- save states ------------------------------------------------------------ *
 * The objects of the state must still be there (same slots, guest addresses, interfaces):
 * a state restores guest memory, not the host objects behind it. Their reference counts
 * are restored. Objects created since are left alone: the restored guest memory no longer
 * refers to them (host objects may, e.g. a texture to the level surfaces it handed out). */
typedef struct {
    u32 id, guest, refs;
    char iface[40];
} SsObj;
void x2_com_ss_save(X2SsBuf *b) {
    pthread_mutex_lock(&com_lock);
    keep_released = true;
    for (u32 i = 1; i < MAX_OBJS; i++) {
        if (!objs[i] || objs[i]->zombie) continue;
        SsObj s = {i, objs[i]->guest, objs[i]->refs, ""};
        snprintf(s.iface, sizeof s.iface, "%s", objs[i]->iface ? objs[i]->iface : "");
        X2_SS_PUT(b, s);
    }
    SsObj end = {0};
    X2_SS_PUT(b, end);
    pthread_mutex_unlock(&com_lock);
}
bool x2_com_ss_load(X2SsBuf *b, bool apply) {
    bool ok = true;
    pthread_mutex_lock(&com_lock);
    for (;;) {
        SsObj s;
        if (!X2_SS_GET(b, s)) {
            ok = false;
            break;
        }
        if (!s.id) break;
        X2ComObj *o = s.id < MAX_OBJS ? objs[s.id] : NULL;
        char iface[40];
        snprintf(iface, sizeof iface, "%s", o && o->iface ? o->iface : "");
        if (!o || o->guest != s.guest || strcmp(iface, s.iface)) {
            if (ok)
                snprintf(x2_ss_error, sizeof x2_ss_error, "the game's %s %u is gone: load the state where it was saved",
                         s.iface[0] ? s.iface : "object", s.id);
            ok = false;
        } else if (o->zombie && !o->keep) {
            ok = false;
        } else if (apply) {
            if (o->zombie) { /* released since: back */
                zombie_remove(o);
                if (o->revive) o->revive(o);
            }
            o->refs = s.refs;
            o->ss_seen = true;
        }
    }
    /* objects created since the state was taken: the restored guest memory has no references
     * to them; those that can be kept are, as if released (their guest memory is the state's) */
    if (ok && apply) {
        u32 left = 0;
        for (u32 i = 1; i < MAX_OBJS; i++) {
            X2ComObj *o = objs[i];
            if (!o) continue;
            if (o->ss_seen) {
                o->ss_seen = false;
                continue;
            }
            if (o->zombie) continue;
            if (o->keep) {
                zombie_add(o);
                if (o->retire) o->retire(o);
            } else {
                left++;
            }
        }
        if (left) X2_TRACE("state: %u objects created since it was taken are left alive", left);
    }
    pthread_mutex_unlock(&com_lock);
    return ok;
}

/* objects in slot order (save states): the next one after *id, or NULL */
X2ComObj *x2_com_next(u32 *id) {
    pthread_mutex_lock(&com_lock);
    X2ComObj *o = NULL;
    for (u32 i = *id + 1; i < MAX_OBJS && !o; i++)
        if (objs[i]) {
            o = objs[i];
            *id = i;
        }
    pthread_mutex_unlock(&com_lock);
    return o;
}
