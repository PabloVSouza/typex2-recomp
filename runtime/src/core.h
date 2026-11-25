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
/* session over (libretro unload): guest memory is freed and left inaccessible, so a guest
 * thread still running faults and is parked (see x2_park_all) */
void x2_mem_release(void);
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
    /* save states (savestate.c) */
    int ss_cs;            /* critical sections held (savestate.c: where a thread may stop) */
    u32 ss_held[16];      /* ... the first 16 of them, in order */
    u32 ss_blocked_cs;    /* the section EnterCriticalSection is blocked on */
    bool ss_bt_ok;        /* ... and ss_bt holds the chain it was blocked in */
    bool ss_guest;        /* has started running guest code */
    bool ss_parked;       /* stopped while a state is taken or restored */
    const char *ss_at;    /* the import it stopped in */
    uintptr_t ss_bt[256]; /* return addresses from the innermost generated function (the guest call chain) */
    int ss_nbt;
    uintptr_t host_top; /* frame address of the thread's start routine (ends stack walks) */
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
 * call, since they cannot be stopped. Later faults and fatal errors on the
 * runtime's own threads park them (another game may be running in a newer
 * instance of the core); on the frontend's threads (e.g. during its exit) they
 * end the process quietly instead. */
extern bool x2_parked;
void x2_park_all(void);
void x2_park_forever(void) __attribute__((noreturn));
bool x2_park_here(void); /* parked, and this is one of the runtime's threads */
/* pthread_create for the runtime's own threads (guest and helpers), so a finished
 * session can tell them from the frontend's */
int x2_spawn(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg);
/* boot.c: load <dir>/game.exe for `game` and start its main thread */
X2Thread *x2_boot(const X2GameTables *game, const char *dir);
void x2_load_env_file(const char *dir);
/* set by a front end whose own settings override x2.env (the libretro core options); x2_boot calls it after x2.env */
extern void (*x2_env_override)(void);
/* the game draws a 16:9 picture with bars into a 4:3 backbuffer (KOF XIII
 * Climax): front ends show only the 16:9 band */
bool x2_game_letterboxed(void);
u32 x2_peb(void);
void x2_set_last_error(u32 e);

/* ---- save states (savestate.c) ------------------------------------------ *
 * Every guest thread stops at its next import call (or in a wait), holding no critical
 * section; then guest memory, the threads' registers and the runtime's state are saved
 * or restored. Each thread's host stack holds its guest call chain, which a state cannot
 * carry: a state loads only where every thread is in the same chain (same screen). */
extern int x2_ss_pending; /* a state is being taken or restored: threads stop at x2_ss_point */
/* stop here while a state is taken or restored; `held` is released meanwhile */
void x2_ss_point(const char *at, pthread_mutex_t *held);
static inline void x2_ss_check(const char *at, pthread_mutex_t *held) {
    if (__atomic_load_n(&x2_ss_pending, __ATOMIC_ACQUIRE)) x2_ss_point(at, held);
}
/* take (load = false) or restore a state in the background; done() reports the outcome */
void x2_ss_request(bool load, const char *path, void (*done)(bool ok, const char *msg));
typedef struct {
    u8 *p;
    size_t n, cap, pos;
    bool bad;
    bool fixed; /* p is the caller's: no growing */
} X2SsBuf;
/* In memory, on the calling thread (libretro): an upper bound of a state's size now,
 * and taking or restoring one; msg gets the outcome either way. */
size_t x2_ss_size(void);
bool x2_ss_save_into(void *data, size_t size, char *msg, size_t nmsg);
bool x2_ss_load_from(const void *data, size_t size, char *msg, size_t nmsg);
size_t x2_mem_ss_bound(void); /* the memory part's largest size now */
void x2_ss_put(X2SsBuf *b, const void *d, size_t n);
bool x2_ss_get(X2SsBuf *b, void *d, size_t n);
#define X2_SS_PUT(b, v) x2_ss_put((b), &(v), sizeof(v))
#define X2_SS_GET(b, v) x2_ss_get((b), &(v), sizeof(v))
void x2_ss_thread_start(X2Thread *t);    /* before a thread first runs guest code */
void x2_ss_cs_blocking(u32 cs);          /* EnterCriticalSection is about to block */
void x2_ss_cs_acquired(void);            /* ... and has the section now */
void x2_ss_frame(pthread_mutex_t *held); /* in Present: where the presenting thread stops */
/* the parts; loads run twice, first with apply = false to check that the state fits
 * (false, with the reason in x2_ss_error, if not) */
extern char x2_ss_error[256];
void x2_mem_ss_save(X2SsBuf *b);
bool x2_mem_ss_load(X2SsBuf *b);
void x2_sync_ss_save(X2SsBuf *b);
bool x2_sync_ss_load(X2SsBuf *b, bool apply);
void x2_sync_wake_all(void); /* every blocked wait re-checks (and sees a pending state) */
void x2_com_ss_save(X2SsBuf *b);
bool x2_com_ss_load(X2SsBuf *b, bool apply);
void x2_d3d9_ss_save(X2SsBuf *b);
bool x2_d3d9_ss_load(X2SsBuf *b);
void x2_dsound_ss_save(X2SsBuf *b);
bool x2_dsound_ss_load(X2SsBuf *b);
void x2_clock_ss_save(X2SsBuf *b);
void x2_clock_ss_load(X2SsBuf *b);
int x2_threads(X2Thread **out, int max); /* registered threads, finished ones included */
void x2_threads_wake(void);              /* self-suspended threads re-check */

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
#define X2_MAX_HANDLES 8192
/* save states: an open file, and putting it back into a handle slot (k32_file.c) */
typedef struct {
    char path[1024];
    int64_t offset;
    int32_t flags;
} X2SsFile;
void x2_file_ss_info(X2Handle *h, X2SsFile *out);
bool x2_file_ss_open(X2Handle *h, const X2SsFile *s);
X2Handle *x2_handle_slot(u32 i, u32 *value); /* slot i (1 .. X2_MAX_HANDLES-1) and its handle value */

/* ---- registration ----------------------------------------------------- */
typedef struct {
    const char *dll;
    const X2Hle *funcs;
} X2HleModule;
extern const X2Hle x2_hle_kernel32[], x2_hle_k32_sync[], x2_hle_k32_file[], x2_hle_user32[], x2_hle_gdi32[],
    x2_hle_advapi32[], x2_hle_ole32[], x2_hle_winmm[], x2_hle_shlwapi[], x2_hle_d3d9[], x2_hle_d3dx9[],
    x2_hle_shell32[], x2_hle_imm32[], x2_hle_dsound[], x2_hle_oleaut32[], x2_hle_ws2_32[], x2_hle_iphlpapi[],
    x2_hle_dinput8[], x2_hle_ddraw[], x2_hle_msvcrt[], x2_hle_mmio[], x2_hle_d3d8[], x2_hle_idmac[];
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
/* prof.c: a guest frame ended after guest_ms of work (X2_PROF_SLOW reports the slow ones) */
void x2_prof_frame(double guest_ms);
/* the guest's clock: real time, advancing at least 1/60 s from one Present to the next */
u64 x2_guest_ns(void);
void x2_guest_clock_frame(void); /* called as Present returns */
