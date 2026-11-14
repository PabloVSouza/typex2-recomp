/* Runtime-internal interfaces. */
#pragma once
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

#include "compat_win.h"
#include "x2/recomp.h"

/* mkdir with the mode POSIX wants and Windows lacks */
#ifdef _WIN32
static inline int x2_mkdir(const char *path) { return _mkdir(path); }
#else
#include <sys/stat.h>
static inline int x2_mkdir(const char *path) { return mkdir(path, 0755); }
#endif

/* ---- guest memory layout ---------------------------------------------- */
#define X2_GUEST_SPACE   0x100000000ull
#define X2_GUEST_SLACK   0x10000u         /* readable tail for unaligned wide loads */
#define X2_VM_LOW        0x00010000u      /* first allocatable guest address */
#define X2_VM_HIGH       0x7FFE0000u      /* end of user space */
#define X2_GRANULE       0x10000u         /* VirtualAlloc allocation granularity */
#define X2_PAGE          0x1000u
#define X2_THUNK_BASE    0xF0000000u      /* host-implemented functions live here */
#define X2_THUNK_STRIDE  8u
#define X2_THUNK_MAX     0x8000u

static inline char *GSTR(u32 a) { return a ? (char *)X2P(a) : NULL; }
static inline u16 *GWSTR(u32 a) { return a ? (u16 *)X2P(a) : NULL; }

void x2_mem_init(void);
u32 x2_vm_alloc(u32 addr, u32 size, const char *tag);  /* 0 on failure */
bool x2_vm_free(u32 addr);
bool x2_vm_query(u32 addr, u32 *base, u32 *size);  /* region containing addr */
u32 x2_heap_create(void);
u32 x2_heap_alloc(u32 heap, u32 size, bool zero);
bool x2_heap_free(u32 heap, u32 p);
u32 x2_heap_realloc(u32 heap, u32 p, u32 size, bool zero);
u32 x2_heap_size(u32 heap, u32 p);
u32 x2_process_heap(void);
/* convenience: allocate guest memory from the process heap */
u32 x2_galloc(u32 size);
u32 x2_gstrdup(const char *s);
/* UTF-16 guest string -> UTF-8 (NULL -> "") */
void x2_wstr_utf8(const u16 *w, char *out, size_t n);
/* UTF-8 -> UTF-16 into guest memory at g, at most cap units with the terminator;
 * returns the length in units without it */
u32 x2_put_wstr(u32 g, const char *s, u32 cap);
u32 x2_wait_many(u32 n, const u32 *hs, bool all, u32 ms); /* WaitForMultipleObjects */

/* ---- host functions callable from guest code (imports, COM vtables) ---- */
/* stdcall: the dispatcher pops the return address plus 4*nargs bytes after the
 * call. nargs < 0 means cdecl (caller cleans). Arguments: ARG(0) is first. */
typedef void (*X2HostFn)(X2Cpu *c);
typedef struct {
    const char *name;
    X2HostFn fn;
    int nargs;
} X2Hle;
/* nargs of a data export (a variable such as msvcr80's _acmdln): the loader calls
 * fn once, which leaves the guest address of the variable in eax */
#define X2_HLE_DATA 0x7FFF

#define ARG(i) RD32(c->esp + 4 + 4 * (i))
#define ARGF(i) RDF32(c->esp + 4 + 4 * (i))
#define RET(v) (c->eax = (u32)(v))

u32 x2_thunk_new(const char *name, X2HostFn fn, int nargs);
const char *x2_thunk_name(u32 addr);
X2Func x2_lookup(u32 addr);            /* recompiled function or NULL */
void x2_call(X2Cpu *c, u32 target);    /* push nothing; run target to its ret */
/* Call a guest stdcall/cdecl function from host code with the given args. */
u32 x2_guest_call(X2Cpu *c, u32 fn, int nargs, ...);

/* ---- loader ----------------------------------------------------------- */
typedef struct {
    char exe_path[1024];
    char game_dir[1024];
    u32 base, size, entry;
    u32 stack_reserve;
    u32 tls_start, tls_end, tls_index_addr, tls_zero;
    u32 tls_callbacks;
} X2Module;
extern X2Module x2_module;
void x2_load_image(const char *exe_path);
void x2_resolve_imports(void);

/* ---- threads ---------------------------------------------------------- */
typedef struct X2Thread {
    X2Cpu cpu;
    u32 teb;
    u32 stack_base, stack_size;
    u32 start, param;
    u32 id;
    u32 handle;
    pthread_t pt;
    u32 tls_slots[64];
    u32 last_error;
    u32 exit_code;
    bool finished;
    bool internal; /* started by the runtime (timers, feeders): its handle is released when it ends */
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int suspend;
    u32 *trace_ring, *trace_pos; /* X2_FN_TRACE builds: this thread's ring */
    /* queued user APCs (ReadFileEx completions): routine, err, bytes, overlapped */
    u32 apc[64][4];
    u32 apc_head, apc_tail;
    long host_tid; /* kernel thread id (Linux), 0 until the thread runs */
    int priority;  /* SetThreadPriority value */
} X2Thread;
/* SetThreadPriority: HIGHEST and TIME_CRITICAL threads also get the fast cores */
void x2_thread_set_priority(X2Thread *t, int priority);

/* queue a completion routine for an alertable wait on thread t */
void x2_queue_apc(X2Thread *t, u32 routine, u32 err, u32 bytes, u32 ov);
/* run pending APCs on the current thread; true if any ran */
bool x2_run_apcs(X2Cpu *c);

extern _Thread_local X2Thread *x2_self;
void x2_trace_dump(X2Thread *t, int n);
/* name the calling thread; critical = keep on the fastest cores (guest main, renderer) */
void x2_thread_perf(const char *name, bool critical);
/* a thread the runtime starts for itself (no guest holds its handle) */
X2Thread *x2_thread_create_internal(u32 start, u32 param);
X2Thread *x2_thread_create(u32 start, u32 param, u32 stack_size, bool suspended);
void x2_thread_resume(X2Thread *t);
int x2_thread_suspend_self(X2Thread *t);
X2Thread *x2_start_main(void);  /* guest main thread; host main stays with the platform */
void x2_sync_broadcast(void);   /* wake waiters after a waitable object changed */
bool x2_event_set(u32 handle);  /* host-side SetEvent */
bool x2_event_reset(u32 handle); /* host-side ResetEvent */
extern pthread_mutex_t x2_gil;
extern bool x2_gil_on;
#define X2_GIL_ENTER() do { if (x2_gil_on) pthread_mutex_lock(&x2_gil); } while (0)
#define X2_GIL_LEAVE() do { if (x2_gil_on) pthread_mutex_unlock(&x2_gil); } while (0)
void x2_exit_process(u32 code) __attribute__((noreturn));
/* set by front ends that must not end the host process (libretro): called by
 * ExitProcess instead of _exit; the calling guest thread is parked afterwards */
extern void (*x2_exit_hook)(u32 code);
/* Session over (libretro unload): guest threads block at their next import
 * call, since they cannot be stopped, and later host faults end the process
 * quietly instead of crashing during the frontend's exit. */
extern bool x2_parked;
void x2_park_all(void);
void x2_park_forever(void);
/* boot.c: load <dir>/game.exe for `game` and start its main thread */
X2Thread *x2_boot(const X2GameTables *game, const char *dir);
void x2_load_env_file(const char *dir);
/* the game draws a 16:9 picture with bars into a 4:3 backbuffer (KOF XIII
 * Climax): front ends show only the 16:9 band */
bool x2_game_letterboxed(void);
u32 x2_peb(void);
void x2_set_last_error(u32 e);

/* ---- handles ---------------------------------------------------------- */
typedef enum { H_FREE, H_FILE, H_EVENT, H_MUTEX, H_SEMAPHORE, H_THREAD, H_FIND, H_COMM, H_CRYPT, H_PIPE, H_MISC } X2HType;
typedef struct X2Handle {
    X2HType type;
    void *obj;
    void (*close)(struct X2Handle *h);
} X2Handle;
u32 x2_handle_new(X2HType type, void *obj, void (*close)(X2Handle *));
X2Handle *x2_handle_get(u32 h, X2HType type);
bool x2_handle_close(u32 h);

/* ---- registration ----------------------------------------------------- */
typedef struct {
    const char *dll;
    const X2Hle *funcs;
} X2HleModule;
extern const X2Hle x2_hle_kernel32[], x2_hle_k32_sync[], x2_hle_k32_file[], x2_hle_user32[], x2_hle_gdi32[],
    x2_hle_advapi32[], x2_hle_ole32[], x2_hle_winmm[], x2_hle_shlwapi[], x2_hle_d3d9[], x2_hle_d3dx9[],
    x2_hle_shell32[], x2_hle_imm32[], x2_hle_dsound[], x2_hle_oleaut32[], x2_hle_ws2_32[], x2_hle_iphlpapi[],
    x2_hle_dinput8[], x2_hle_ddraw[], x2_hle_msvcrt[], x2_hle_mmio[], x2_hle_d3d8[];
/* ExitThread for the calling guest thread (also the CRT's _endthreadex) */
void x2_exit_thread(u32 code) __attribute__((noreturn));
const X2Hle *x2_hle_find(const char *dll, const char *name);

/* ---- diagnostics ------------------------------------------------------ */
extern int x2_log_level;
void x2_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void x2_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
#define X2_TRACE(...) do { if (x2_log_level >= 2) x2_log(__VA_ARGS__); } while (0)

/* path translation: guest (Windows) path -> host path */
void x2_host_path(const char *guest, char *out, size_t n);

/* platform */
u64 x2_time_ns(void);
