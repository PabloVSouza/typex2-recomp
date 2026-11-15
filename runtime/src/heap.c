/* The libretro core's heap on ELF platforms (Android, Linux), linked with
 * --wrap=malloc,free,... so that every allocation of this library instance,
 * its statically linked C++ runtime, glslang and FFmpeg included, comes
 * through here.
 *
 * A closed game's instance stays loaded with its threads parked (see
 * platform_libretro.c), so its heap cannot simply be freed: a parked thread
 * that wakes up (a timed wait) would use the memory after someone else got
 * it. Large blocks, which hold most of a game's host memory (texture and
 * buffer copies, decoders, shader caches), are mapped one by one: at unload
 * (x2_heap_release) their pages go back to the system and their addresses
 * stay reserved and inaccessible, so a thread touching them faults and is
 * parked. Small blocks come from the system allocator and stay.
 *
 * Every block has a 16-byte trailer just before the pointer; strdup, strndup
 * and vasprintf are wrapped too, so whatever this library frees came from
 * here. */
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

void *__real_malloc(size_t n);
void __real_free(void *p);
void *__real_realloc(void *p, size_t n);

enum { SMALL = 0x58324831, ALIGNED = 0x58324832, BIG = 0x58324833, BIG_MIN = 256 << 10 };

typedef struct { /* just before every block */
    uint32_t magic, align;
    size_t size;
} Tail;
typedef struct Big { /* before a big block's tail */
    struct Big *prev, *next;
    void *base;
    size_t map;
} Big;

static pthread_mutex_t big_mu = PTHREAD_MUTEX_INITIALIZER;
static Big *bigs;
static atomic_size_t small_live, big_live;

static Tail *tail_of(void *p) { return (Tail *)p - 1; }

static size_t page(void) {
    static size_t ps;
    if (!ps) ps = (size_t)sysconf(_SC_PAGESIZE);
    return ps;
}

static void *alloc(size_t n, size_t align) {
    if (align < 16) align = 16;
    if (n >= BIG_MIN) {
        size_t head = (sizeof(Big) + sizeof(Tail) + align - 1) / align * align;
        size_t map = (head + n + page() - 1) / page() * page();
        uint8_t *base = mmap(NULL, map, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base == MAP_FAILED) return NULL;
        uint8_t *p = base + head;
        Big *b = (Big *)(p - sizeof(Tail)) - 1;
        *tail_of(p) = (Tail){BIG, (uint32_t)align, n};
        b->base = base;
        b->map = map;
        pthread_mutex_lock(&big_mu);
        b->prev = NULL;
        b->next = bigs;
        if (bigs) bigs->prev = b;
        bigs = b;
        pthread_mutex_unlock(&big_mu);
        big_live += map;
        return p;
    }
    if (align == 16) {
        uint8_t *base = __real_malloc(sizeof(Tail) + n);
        if (!base) return NULL;
        *(Tail *)base = (Tail){SMALL, 16, n};
        small_live += n;
        return base + sizeof(Tail);
    }
    /* aligned: the system block's address is stored before the tail */
    uint8_t *base = __real_malloc(n + align + sizeof(Tail) + sizeof(void *));
    if (!base) return NULL;
    uintptr_t p = ((uintptr_t)base + sizeof(Tail) + sizeof(void *) + align - 1) & ~(uintptr_t)(align - 1);
    ((void **)(p - sizeof(Tail)))[-1] = base;
    *tail_of((void *)p) = (Tail){ALIGNED, (uint32_t)align, n};
    small_live += n;
    return (void *)p;
}

void __wrap_free(void *p) {
    if (!p) return;
    Tail *t = tail_of(p);
    switch (t->magic) {
    case SMALL:
        small_live -= t->size;
        t->magic = 0;
        __real_free(t);
        return;
    case ALIGNED:
        small_live -= t->size;
        t->magic = 0;
        __real_free(((void **)t)[-1]);
        return;
    case BIG: {
        Big *b = (Big *)t - 1;
        pthread_mutex_lock(&big_mu);
        if (b->prev)
            b->prev->next = b->next;
        else
            bigs = b->next;
        if (b->next) b->next->prev = b->prev;
        pthread_mutex_unlock(&big_mu);
        big_live -= b->map;
        munmap(b->base, b->map);
        return;
    }
    default: /* not ours: a frontend or system allocation */
        fprintf(stderr, "[x2] heap: free of a block not allocated here (%p)\n", p);
        __real_free(p);
    }
}

void *__wrap_malloc(size_t n) { return alloc(n, 16); }

void *__wrap_calloc(size_t k, size_t n) {
    if (n && k > SIZE_MAX / n) return NULL;
    void *p = alloc(k * n, 16);
    if (p && k * n < BIG_MIN) memset(p, 0, k * n); /* big blocks are fresh pages */
    return p;
}

void *__wrap_realloc(void *p, size_t n) {
    if (!p) return alloc(n, 16);
    if (!n) {
        __wrap_free(p);
        return NULL;
    }
    Tail *t = tail_of(p);
    if (t->magic == SMALL && n < BIG_MIN) {
        size_t old = t->size;
        Tail *r = __real_realloc(t, sizeof(Tail) + n);
        if (!r) return NULL;
        r->size = n;
        small_live += n - old; /* modular: shrinking works too */
        return r + 1;
    }
    if (t->magic == BIG && n <= t->size && n >= BIG_MIN) { /* shrinking within the mapping */
        t->size = n;
        return p;
    }
    void *q = alloc(n, t->align);
    if (!q) return NULL;
    memcpy(q, p, t->size < n ? t->size : n);
    __wrap_free(p);
    return q;
}

int __wrap_posix_memalign(void **out, size_t align, size_t n) {
    if (align < sizeof(void *) || (align & (align - 1))) return EINVAL;
    void *p = alloc(n, align);
    if (!p) return ENOMEM;
    *out = p;
    return 0;
}

char *__wrap_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = alloc(n, 16);
    if (d) memcpy(d, s, n);
    return d;
}

char *__wrap_strndup(const char *s, size_t max) {
    size_t n = strnlen(s, max);
    char *d = alloc(n + 1, 16);
    if (d) {
        memcpy(d, s, n);
        d[n] = 0;
    }
    return d;
}

int __wrap_vasprintf(char **out, const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    *out = NULL;
    if (n < 0) return -1;
    char *s = alloc((size_t)n + 1, 16);
    if (!s) return -1;
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    *out = s;
    return n;
}

/* The game is over: big blocks' pages go back to the system, their addresses stay
 * reserved and inaccessible (a parked thread touching one faults and stays parked). */
void x2_heap_release(void) {
    pthread_mutex_lock(&big_mu);
    size_t n = 0, mb = big_live >> 20;
    for (Big *b = bigs, *next; b; b = next, n++) {
        next = b->next;
        mmap(b->base, b->map, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
    }
    bigs = NULL;
    big_live = 0;
    pthread_mutex_unlock(&big_mu);
    fprintf(stderr, "[x2] heap: released %zu blocks (%zu MB); %zu MB in small blocks stays\n", n, mb,
            (size_t)small_live >> 20);
}
