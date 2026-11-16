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
 * parked. Medium blocks (4 KB to 256 KB) come from size-class chunks released the same
 * way; blocks under 4 KB come from the system allocator and stay.
 *
 * Every block has a 16-byte trailer just before the pointer; strdup, strndup
 * and vasprintf are wrapped too, so whatever this library frees came from
 * here. */
#define _GNU_SOURCE /* mremap */
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

/* Medium blocks (4 KB up to a big block) come from size classes of powers of two, carved out of
 * 4 MB chunks this instance maps: freed blocks are reused, and the chunks are released with the
 * big blocks. (They were most of what a closed game left behind: 17 of SSF4's 20 MB.) */
enum { MID = 0x58324834, MID_MIN_LOG = 12, MID_CLASSES = 7, CHUNK = 4 << 20, MAX_CHUNKS = 1024 };
typedef struct {
    pthread_mutex_t mu;
    void *free; /* freed blocks, linked through their first word */
    uint8_t *cur, *end;
} MidClass;
static MidClass mids[MID_CLASSES] = {{PTHREAD_MUTEX_INITIALIZER, 0, 0, 0}, {PTHREAD_MUTEX_INITIALIZER, 0, 0, 0},
                                     {PTHREAD_MUTEX_INITIALIZER, 0, 0, 0}, {PTHREAD_MUTEX_INITIALIZER, 0, 0, 0},
                                     {PTHREAD_MUTEX_INITIALIZER, 0, 0, 0}, {PTHREAD_MUTEX_INITIALIZER, 0, 0, 0},
                                     {PTHREAD_MUTEX_INITIALIZER, 0, 0, 0}};
static uint8_t *chunks[MAX_CHUNKS];
static atomic_uint nchunks;
static atomic_size_t mid_live;

static void *mid_alloc(size_t n) {
    size_t total = n + sizeof(Tail);
    uint32_t c = 0;
    while (((size_t)1 << (MID_MIN_LOG + c)) < total) c++;
    if (c >= MID_CLASSES) return NULL;
    size_t sz = (size_t)1 << (MID_MIN_LOG + c);
    MidClass *m = &mids[c];
    pthread_mutex_lock(&m->mu);
    uint8_t *b = m->free;
    if (b) {
        m->free = *(void **)b;
    } else {
        if (!m->cur || m->cur + sz > m->end) {
            uint32_t k = atomic_fetch_add(&nchunks, 1);
            uint8_t *ch = k < MAX_CHUNKS ? mmap(NULL, CHUNK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)
                                         : MAP_FAILED;
            if (ch == MAP_FAILED) { /* out of chunk slots: the system allocator */
                if (k >= MAX_CHUNKS) atomic_fetch_sub(&nchunks, 1);
                pthread_mutex_unlock(&m->mu);
                return NULL;
            }
            chunks[k] = ch;
            m->cur = ch;
            m->end = ch + CHUNK;
        }
        b = m->cur;
        m->cur += sz;
    }
    pthread_mutex_unlock(&m->mu);
    *(Tail *)b = (Tail){MID, c, n};
    mid_live += sz;
    return b + sizeof(Tail);
}
static void mid_free(Tail *t) {
    MidClass *m = &mids[t->align];
    mid_live -= (size_t)1 << (MID_MIN_LOG + t->align);
    t->magic = 0;
    pthread_mutex_lock(&m->mu);
    *(void **)t = m->free;
    m->free = t;
    pthread_mutex_unlock(&m->mu);
}

static void *alloc(size_t n, size_t align) {
    if (align < 16) align = 16;
    static int mids_on = -1; /* X2_HEAP_MID=0: medium blocks from the system allocator (A/B) */
    if (mids_on < 0) mids_on = !getenv("X2_HEAP_MID") || atoi(getenv("X2_HEAP_MID")) != 0;
    if (mids_on && align == 16 && n + sizeof(Tail) > ((size_t)1 << MID_MIN_LOG) && n < BIG_MIN) {
        void *p = mid_alloc(n);
        if (p) return p;
    }
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
    case MID: mid_free(t); return;
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
    if (t->magic == MID) { /* in place while it fits its class, else moved (align holds the class) */
        if (n + sizeof(Tail) <= ((size_t)1 << (MID_MIN_LOG + t->align))) {
            t->size = n;
            return p;
        }
        void *q = alloc(n, 16);
        if (!q) return NULL;
        memcpy(q, p, t->size < n ? t->size : n);
        mid_free(t);
        return q;
    }
    if (t->magic == SMALL && n + sizeof(Tail) <= ((size_t)1 << MID_MIN_LOG)) { /* stays small */
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
#ifdef __linux__
    if (t->magic == BIG && n >= BIG_MIN) {
        /* growing: the kernel moves the pages (a growing command buffer was copied each time it
         * doubled, ~230 ms of BlazBlue CT's fight-start frame on the tablet) */
        Big *b = (Big *)t - 1;
        size_t head = (size_t)((uint8_t *)p - (uint8_t *)b->base);
        size_t map = (head + n + page() - 1) / page() * page();
        pthread_mutex_lock(&big_mu);
        size_t old = b->map;
        uint8_t *base = mremap(b->base, old, map, MREMAP_MAYMOVE);
        if (base != MAP_FAILED) {
            uint8_t *q = base + head;
            b = (Big *)(q - sizeof(Tail)) - 1; /* the header moved with the pages: relink it */
            if (b->prev)
                b->prev->next = b;
            else
                bigs = b;
            if (b->next) b->next->prev = b;
            b->base = base;
            b->map = map;
            tail_of(q)->size = n;
            big_live += map - old;
            pthread_mutex_unlock(&big_mu);
            return q;
        }
        pthread_mutex_unlock(&big_mu);
    }
#endif

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
    uint32_t nc = atomic_load(&nchunks);
    if (nc > MAX_CHUNKS) nc = MAX_CHUNKS;
    for (uint32_t i = 0; i < nc; i++) /* medium blocks: their chunks whole */
        if (chunks[i])
            mmap(chunks[i], CHUNK, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
    fprintf(stderr,
            "[x2] heap: released %zu big blocks (%zu MB) and %u chunks of medium ones (%zu MB in use); %zu MB in small "
            "blocks stays\n",
            n, mb, nc, (size_t)mid_live >> 20, (size_t)small_live >> 20);
}
