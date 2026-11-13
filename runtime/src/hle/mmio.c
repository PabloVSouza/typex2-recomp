/* winmm multimedia file I/O (mmio*): RIFF files such as WAVs, read the way the
 * DirectX SDK's CWaveFile does it (KOF Maximum Impact Regulation A): descend
 * into chunks, read, or walk the MMIOINFO buffer with mmioGetInfo /
 * mmioAdvance / mmioSetInfo. Writing covers mmioCreateChunk + mmioWrite +
 * mmioAscend (the chunk size is patched on the way out). */
#include <stdio.h>
#include <string.h>

#include "../core.h"

#define MMIO_FINDCHUNK 0x0010
#define MMIO_FINDRIFF 0x0020
#define MMIO_FINDLIST 0x0040
#define MMIO_CREATERIFF 0x0020
#define MMIO_CREATELIST 0x0040
#define MMIO_DIRTY 0x10000000u
#define FOURCC_RIFF 0x46464952u
#define FOURCC_LIST 0x5453494Cu
#define MMIOERR_CHUNKNOTFOUND 265
#define MMIOERR_CANNOTOPEN 259
#define MMIO_BUF 0x2000

typedef struct {
    FILE *f;
    u32 buf;  /* guest buffer for mmioGetInfo / mmioAdvance */
    u32 pos;  /* logical position: what mmioRead / mmioDescend continue from */
} Mmio;
#define MAX_MMIO 32
static Mmio mmios[MAX_MMIO];
static pthread_mutex_t mmio_lock = PTHREAD_MUTEX_INITIALIZER;
#define HMMIO_BASE 0x4D000u

static Mmio *get(u32 h) {
    u32 i = h - HMMIO_BASE;
    return i < MAX_MMIO && mmios[i].f ? &mmios[i] : NULL;
}
static u32 rd(Mmio *m, void *dst, u32 n) {
    fseek(m->f, (long)m->pos, SEEK_SET);
    u32 got = (u32)fread(dst, 1, n, m->f);
    m->pos += got;
    return got;
}

/* mmioOpenA(path, info, flags) */
static void mmioOpenA(X2Cpu *c) {
    u32 flags = ARG(2);
    char hp[1100];
    x2_host_path(GSTR(ARG(0)), hp, sizeof hp);
    const char *mode = flags & 0x1000 /* MMIO_CREATE */ ? "w+b" : (flags & 3) ? "r+b" : "rb";
    FILE *f = fopen(hp, mode);
    if (!f) {
        if (ARG(1)) WR16(ARG(1) + 12, MMIOERR_CANNOTOPEN);
        X2_TRACE("mmioOpenA(\"%s\") failed", GSTR(ARG(0)));
        RET(0);
        return;
    }
    pthread_mutex_lock(&mmio_lock);
    for (u32 i = 0; i < MAX_MMIO; i++)
        if (!mmios[i].f) {
            mmios[i] = (Mmio){f, 0, 0};
            pthread_mutex_unlock(&mmio_lock);
            RET(HMMIO_BASE + i);
            return;
        }
    pthread_mutex_unlock(&mmio_lock);
    fclose(f);
    RET(0);
}
static void mmioClose(X2Cpu *c) {
    Mmio *m = get(ARG(0));
    if (!m) { RET(MMIOERR_CANNOTOPEN); return; }
    fclose(m->f);
    pthread_mutex_lock(&mmio_lock);
    m->f = NULL; /* the guest buffer stays allocated: slots are few */
    pthread_mutex_unlock(&mmio_lock);
    RET(0);
}
static void mmioRead(X2Cpu *c) {
    Mmio *m = get(ARG(0));
    RET(m ? rd(m, X2P(ARG(1)), ARG(2)) : (u32)-1);
}
static void mmioWrite(X2Cpu *c) {
    Mmio *m = get(ARG(0));
    if (!m) { RET((u32)-1); return; }
    fseek(m->f, (long)m->pos, SEEK_SET);
    u32 n = (u32)fwrite(X2P(ARG(1)), 1, ARG(2), m->f);
    m->pos += n;
    RET(n);
}
static void mmioSeek(X2Cpu *c) { /* (h, offset, origin) */
    Mmio *m = get(ARG(0));
    if (!m) { RET((u32)-1); return; }
    s32 off = (s32)ARG(1);
    if (ARG(2) == SEEK_SET) m->pos = (u32)off;
    else if (ARG(2) == SEEK_CUR) m->pos += (u32)off;
    else {
        fseek(m->f, 0, SEEK_END);
        m->pos = (u32)(ftell(m->f) + off);
    }
    RET(m->pos);
}

/* MMCKINFO: ckid, cksize, fccType, dwDataOffset, dwFlags */
static void mmioDescend(X2Cpu *c) { /* (h, ck, parent, flags) */
    Mmio *m = get(ARG(0));
    u32 ck = ARG(1), parent = ARG(2), flags = ARG(3);
    if (!m) { RET(MMIOERR_CHUNKNOTFOUND); return; }
    u32 end = parent ? RD32(parent + 12) + RD32(parent + 4) : 0xFFFFFFFFu;
    u32 want_id = RD32(ck), want_type = RD32(ck + 8);
    for (;;) {
        if (m->pos + 8 > end) { RET(MMIOERR_CHUNKNOTFOUND); return; }
        u32 hdr[2];
        if (rd(m, hdr, 8) != 8) { RET(MMIOERR_CHUNKNOTFOUND); return; }
        u32 id = hdr[0], size = hdr[1], type = 0, data = m->pos;
        bool container = id == FOURCC_RIFF || id == FOURCC_LIST;
        if (container && rd(m, &type, 4) != 4) { RET(MMIOERR_CHUNKNOTFOUND); return; }
        bool match = flags & MMIO_FINDRIFF   ? id == FOURCC_RIFF && type == want_type
                     : flags & MMIO_FINDLIST ? id == FOURCC_LIST && type == want_type
                     : flags & MMIO_FINDCHUNK ? id == want_id
                                              : true;
        if (match) {
            WR32(ck, id);
            WR32(ck + 4, size);
            WR32(ck + 8, type);
            WR32(ck + 12, data);
            WR32(ck + 16, 0);
            RET(0);
            return;
        }
        m->pos = data + size + (size & 1); /* skip the chunk (and its pad byte) */
    }
}
static void mmioAscend(X2Cpu *c) { /* (h, ck, flags) */
    Mmio *m = get(ARG(0));
    u32 ck = ARG(1);
    if (!m) { RET(MMIOERR_CANNOTOPEN); return; }
    u32 data = RD32(ck + 12);
    if (RD32(ck + 16) & MMIO_DIRTY) { /* written chunk: patch its size */
        u32 size = m->pos - data;
        fseek(m->f, (long)data - 4, SEEK_SET);
        fwrite(&size, 4, 1, m->f);
        WR32(ck + 4, size);
        WR32(ck + 16, RD32(ck + 16) & ~MMIO_DIRTY);
        if (size & 1) { /* pad byte */
            fseek(m->f, (long)m->pos, SEEK_SET);
            fputc(0, m->f);
        }
    }
    u32 size = RD32(ck + 4);
    m->pos = data + size + (size & 1);
    RET(0);
}
static void mmioCreateChunk(X2Cpu *c) { /* (h, ck, flags) */
    Mmio *m = get(ARG(0));
    u32 ck = ARG(1), flags = ARG(2);
    if (!m) { RET(MMIOERR_CANNOTOPEN); return; }
    u32 id = flags & MMIO_CREATERIFF ? FOURCC_RIFF : flags & MMIO_CREATELIST ? FOURCC_LIST : RD32(ck);
    u32 hdr[3] = {id, RD32(ck + 4), RD32(ck + 8)};
    fseek(m->f, (long)m->pos, SEEK_SET);
    bool container = id == FOURCC_RIFF || id == FOURCC_LIST;
    fwrite(hdr, 4, container ? 3 : 2, m->f);
    WR32(ck, id);
    WR32(ck + 12, m->pos + 8);
    WR32(ck + 16, MMIO_DIRTY);
    m->pos += container ? 12 : 8;
    RET(0);
}

/* MMIOINFO buffer fields: cchBuffer +20, pchBuffer +24, pchNext +28,
 * pchEndRead +32, pchEndWrite +36, lBufOffset +40, lDiskOffset +44, hmmio +68 */
static void mmioGetInfo(X2Cpu *c) {
    Mmio *m = get(ARG(0));
    u32 info = ARG(1);
    if (!m) { RET(MMIOERR_CANNOTOPEN); return; }
    if (!m->buf) m->buf = x2_galloc(MMIO_BUF);
    memset(X2P(info), 0, 72);
    WR32(info + 20, MMIO_BUF);
    WR32(info + 24, m->buf);
    WR32(info + 28, m->buf); /* empty: the first mmioAdvance fills it */
    WR32(info + 32, m->buf);
    WR32(info + 36, m->buf + MMIO_BUF);
    WR32(info + 40, m->pos);
    WR32(info + 44, m->pos);
    WR32(info + 68, ARG(0));
    RET(0);
}
static void mmioAdvance(X2Cpu *c) { /* (h, info, flags): refill the buffer for reading */
    Mmio *m = get(ARG(0));
    u32 info = ARG(1);
    if (!m || !info) { RET(MMIOERR_CANNOTOPEN); return; }
    /* continue from where the caller has consumed up to */
    m->pos = RD32(info + 40) + (RD32(info + 28) - RD32(info + 24));
    u32 at = m->pos, n = rd(m, X2P(m->buf), MMIO_BUF);
    WR32(info + 24, m->buf);
    WR32(info + 28, m->buf);
    WR32(info + 32, m->buf + n);
    WR32(info + 40, at);
    WR32(info + 44, m->pos);
    RET(0);
}
static void mmioSetInfo(X2Cpu *c) { /* the caller is done with the buffer: resume after what it read */
    Mmio *m = get(ARG(0));
    u32 info = ARG(1);
    if (!m || !info) { RET(MMIOERR_CANNOTOPEN); return; }
    m->pos = RD32(info + 40) + (RD32(info + 28) - RD32(info + 24));
    RET(0);
}

const X2Hle x2_hle_mmio[] = {
    {"mmioOpenA", mmioOpenA, 3},       {"mmioClose", mmioClose, 2},     {"mmioRead", mmioRead, 3},
    {"mmioWrite", mmioWrite, 3},       {"mmioSeek", mmioSeek, 3},       {"mmioDescend", mmioDescend, 4},
    {"mmioAscend", mmioAscend, 3},     {"mmioCreateChunk", mmioCreateChunk, 3},
    {"mmioGetInfo", mmioGetInfo, 3},   {"mmioAdvance", mmioAdvance, 3}, {"mmioSetInfo", mmioSetInfo, 3},
    {NULL, NULL, 0},
};
