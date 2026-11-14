/* Guest address space, VirtualAlloc-style regions and the guest heap. */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "core.h"

u8 *x2_mem;

/* ---- region allocator (64 KiB granules) -------------------------------- */
#define NGRAN (X2_GUEST_SPACE / X2_GRANULE)
/* gran_owner[g] = first granule of the region covering g, or 0 if free.
 * granule 0 is never allocatable, so 0 is a safe "free" marker. */
static u32 gran_owner[NGRAN];
static u32 gran_count[NGRAN];  /* number of granules, valid at a region's first granule */
static pthread_mutex_t vm_lock = PTHREAD_MUTEX_INITIALIZER;

void x2_mem_init(void) {
    /* X2_MEM_BASE=<hex>: place guest memory at a fixed host address (debugging) */
    void *hint = getenv("X2_MEM_BASE") ? (void *)(uintptr_t)strtoull(getenv("X2_MEM_BASE"), NULL, 16) : NULL;
#ifdef _WIN32
    /* committed but untouched pages cost no memory: Windows zero-fills them on first use;
     * the commit only counts against the commit limit (RAM + page file) */
    void *p = VirtualAlloc(hint, (SIZE_T)X2_GUEST_SPACE + X2_GUEST_SLACK, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) x2_fatal("cannot reserve 4 GiB guest address space: error %lu", (unsigned long)GetLastError());
    x2_mem = p;
    DWORD old;
    VirtualProtect(x2_mem, X2_VM_LOW, PAGE_NOACCESS, &old); /* null page guard */
#else
    void *p = mmap(hint, X2_GUEST_SPACE + X2_GUEST_SLACK, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | (hint ? MAP_FIXED : 0), -1, 0);
    if (p == MAP_FAILED) x2_fatal("cannot reserve 4 GiB guest address space: %s", strerror(errno));
    x2_mem = p;
    /* null page guard: catch guest null dereferences as host faults */
    mprotect(x2_mem, X2_VM_LOW, PROT_NONE);
#endif
}

static bool range_free(u32 g, u32 n) {
    if (g == 0 || (u64)g + n > X2_VM_HIGH / X2_GRANULE) return false;
    for (u32 i = 0; i < n; i++)
        if (gran_owner[g + i]) return false;
    return true;
}

u32 x2_vm_alloc(u32 addr, u32 size, const char *tag) {
    if (!size) return 0;
    u32 n = (u32)(((u64)size + X2_GRANULE - 1) / X2_GRANULE);
    pthread_mutex_lock(&vm_lock);
    u32 g = 0;
    if (addr) {
        g = addr / X2_GRANULE;
        if (!range_free(g, n)) g = 0;
    } else {
        /* first fit, starting above the low 16 MiB to keep small addresses rare */
        for (u32 s = 0x01000000u / X2_GRANULE; s + n <= X2_VM_HIGH / X2_GRANULE; s++) {
            if (gran_owner[s]) {  /* skip to the last granule of this region */
                s = gran_owner[s] + gran_count[gran_owner[s]] - 1;
                continue;
            }
            if (range_free(s, n)) { g = s; break; }
        }
    }
    if (g) {
        for (u32 i = 0; i < n; i++) gran_owner[g + i] = g;
        gran_count[g] = n;
    }
    pthread_mutex_unlock(&vm_lock);
    if (!g) {
        x2_log("vm_alloc(%08x, %x, %s) failed", addr, size, tag ? tag : "");
        return 0;
    }
    X2_TRACE("vm_alloc %08x+%x %s", g * X2_GRANULE, n * X2_GRANULE, tag ? tag : "");
    return g * X2_GRANULE;
}

bool x2_vm_free(u32 addr) {
    u32 g = addr / X2_GRANULE;
    pthread_mutex_lock(&vm_lock);
    bool ok = g && gran_owner[g] == g && addr % X2_GRANULE == 0;
    if (ok) {
        u32 n = gran_count[g];
        for (u32 i = 0; i < n; i++) gran_owner[g + i] = 0;
        gran_count[g] = 0;
        /* return pages to the host and make them read back as zero */
        madvise(X2P(g * X2_GRANULE), (size_t)n * X2_GRANULE, MADV_FREE);
        memset(X2P(g * X2_GRANULE), 0, (size_t)n * X2_GRANULE);
    }
    pthread_mutex_unlock(&vm_lock);
    return ok;
}

bool x2_vm_query(u32 addr, u32 *base, u32 *size) {
    u32 g = addr / X2_GRANULE;
    pthread_mutex_lock(&vm_lock);
    u32 o = gran_owner[g];
    if (o) {
        *base = o * X2_GRANULE;
        *size = gran_count[o] * X2_GRANULE;
    }
    pthread_mutex_unlock(&vm_lock);
    return o != 0;
}

/* ---- guest heap -------------------------------------------------------- *
 * Size-class allocator inside guest memory. Each block has an 8-byte header
 * {u32 requested_size; u32 class} directly before the 8-byte-aligned payload.
 * Classes are powers of two from 16 B to 256 KiB; larger blocks get their own
 * VM region (class 0xFFFFFFFF). One global lock: the game is not allocation-bound. */
#define HEAP_MIN_SHIFT 4
#define HEAP_MAX_SHIFT 18
#define HEAP_ARENA (4u << 20)
#define HEAP_LARGE 0xFFFFFFFFu

static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;
static u32 free_list[HEAP_MAX_SHIFT + 1];  /* guest addr of first free payload */
static u32 arena_cur, arena_end;
static u32 process_heap_handle;

static u32 size_class(u32 n) {
    u32 s = HEAP_MIN_SHIFT;
    while (s <= HEAP_MAX_SHIFT && (1u << s) < n + 8) s++;
    return s;
}

u32 x2_heap_alloc(u32 heap, u32 size, bool zero) {
    (void)heap;
    u32 cls = size_class(size ? size : 1);
    u32 p;
    if (cls > HEAP_MAX_SHIFT) {
        u32 r = x2_vm_alloc(0, size + 16, "heap-large");
        if (!r) return 0;
        p = r + 16;
        WR32(p - 8, size);
        WR32(p - 4, HEAP_LARGE);
        return p;  /* fresh VM is already zero */
    }
    pthread_mutex_lock(&heap_lock);
    p = free_list[cls];
    if (p) {
        free_list[cls] = RD32(p);
    } else {
        u32 bsz = 1u << cls;
        if (arena_cur + bsz > arena_end) {
            u32 a = x2_vm_alloc(0, HEAP_ARENA, "heap-arena");
            if (!a) { pthread_mutex_unlock(&heap_lock); return 0; }
            arena_cur = a;
            arena_end = a + HEAP_ARENA;
        }
        p = arena_cur + 8;
        arena_cur += bsz;
    }
    WR32(p - 8, size);
    WR32(p - 4, cls);
    pthread_mutex_unlock(&heap_lock);
    if (zero) memset(X2P(p), 0, size);
    return p;
}

bool x2_heap_free(u32 heap, u32 p) {
    (void)heap;
    if (!p) return true;
    u32 cls = RD32(p - 4);
    if (cls == HEAP_LARGE) return x2_vm_free(p - 16);
    if (cls < HEAP_MIN_SHIFT || cls > HEAP_MAX_SHIFT) {
        x2_log("heap_free: bad block %08x (class %u)", p, cls);
        return false;
    }
    pthread_mutex_lock(&heap_lock);
    WR32(p, free_list[cls]);
    free_list[cls] = p;
    WR32(p - 4, 0xDEAD0000u | cls);  /* catch double frees */
    pthread_mutex_unlock(&heap_lock);
    return true;
}

u32 x2_heap_size(u32 heap, u32 p) {
    (void)heap;
    return p ? RD32(p - 8) : 0xFFFFFFFFu;
}

u32 x2_heap_realloc(u32 heap, u32 p, u32 size, bool zero) {
    if (!p) return x2_heap_alloc(heap, size, zero);
    u32 old = RD32(p - 8), cls = RD32(p - 4);
    u32 cap = cls == HEAP_LARGE ? old : (1u << cls) - 8;
    if (size <= cap && cls != HEAP_LARGE) {
        if (zero && size > old) memset(X2P(p + old), 0, size - old);
        WR32(p - 8, size);
        return p;
    }
    u32 n = x2_heap_alloc(heap, size, zero);
    if (!n) return 0;
    memcpy(X2P(n), X2P(p), old < size ? old : size);
    x2_heap_free(heap, p);
    return n;
}

u32 x2_heap_create(void) {
    static u32 next = 0x00500000u;  /* opaque handle values; not real addresses */
    pthread_mutex_lock(&heap_lock);
    u32 h = next += 0x10;
    pthread_mutex_unlock(&heap_lock);
    return h;
}

u32 x2_process_heap(void) {
    if (!process_heap_handle) process_heap_handle = x2_heap_create();
    return process_heap_handle;
}

u32 x2_galloc(u32 size) { return x2_heap_alloc(x2_process_heap(), size, true); }

u32 x2_gstrdup(const char *s) {
    u32 n = (u32)strlen(s) + 1;
    u32 p = x2_galloc(n);
    memcpy(X2P(p), s, n);
    return p;
}
