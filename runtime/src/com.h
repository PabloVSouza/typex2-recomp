/* Host-implemented COM objects visible to guest code.
 *
 * A guest object is 8 bytes of guest memory: { u32 vtable; u32 host_id }.
 * The vtable is a guest array of thunk addresses, one per method, so a guest
 * `call [eax+0x44]` lands in a host function. Methods are stdcall with
 * `this` as ARG(0). Methods without an implementation get a named thunk that
 * stops with "unimplemented import IDirect3DDevice9::Foo". */
#pragma once
#include "core.h"

typedef struct {
    const char *name;
    X2HostFn fn;
    int nargs;  /* including `this` */
} X2Method;

typedef struct X2ComObj {
    u32 guest;      /* guest address of the object */
    u32 refs;
    const char *iface;
    void (*destroy)(struct X2ComObj *o);
    bool immortal; /* never destroyed by Release (stand-in objects with loose refcounting) */
} X2ComObj;

/* Builds a guest vtable for interface `iface`; `methods` is in vtable order. */
u32 x2_com_vtable(const char *iface, const X2Method *methods, int count);
/* Wraps host object `o` (which must start with X2ComObj) as a guest object. */
u32 x2_com_wrap(X2ComObj *o, u32 vtable, const char *iface);
/* Host object behind a guest pointer, or NULL. */
X2ComObj *x2_com_get(u32 guest);
#define COM_THIS(type) ((type *)x2_com_get(ARG(0)))

/* Generic IUnknown methods operating on X2ComObj. */
void x2_com_AddRef(X2Cpu *c);
void x2_com_Release(X2Cpu *c);
void x2_com_QueryInterface_self(X2Cpu *c);  /* returns `this` for any IID */

#define S_OK 0u
#define E_FAIL 0x80004005u
#define E_NOTIMPL 0x80004001u
#define E_NOINTERFACE 0x80004002u
#define E_OUTOFMEMORY 0x8007000Eu
