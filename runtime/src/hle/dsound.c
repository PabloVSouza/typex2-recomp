/* DirectSound 8 (created through CoCreateInstance(CLSID_DirectSound8)).
 *
 * Sound buffers live in guest memory so Lock() can hand out guest pointers.
 * A host ticker advances every playing buffer's play cursor in real time and
 * fires position notifications; the audio backend mixes from the same buffers
 * (x2_dsound_mix) and, when present, drives the cursors instead of the clock. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../com.h"
#include "dsound.h"
#include "../video.h"

#define DS_OK 0u
#define DSERR_INVALIDPARAM 0x80070057u
#define DSERR_NOINTERFACE 0x80004002u

#define DSBCAPS_PRIMARYBUFFER 0x1u
#define DSBPLAY_LOOPING 0x1u
#define DSBSTATUS_PLAYING 0x1u
#define DSBSTATUS_LOOPING 0x4u

typedef struct {
    u32 offset, event;
} Notify;

typedef struct Buf {
    X2ComObj com;
    bool primary;
    u32 mem, size;
    u16 channels, bits, block;
    u32 rate, freq;     /* format rate, current playback frequency */
    s32 volume, pan;    /* hundredths of dB */
    bool playing, looping;
    double pos;         /* play cursor in bytes */
    Notify notify[64];
    u32 nnotify;
    u32 notify_obj;
    struct Buf *next;
} Buf;

typedef struct {
    X2ComObj com;
} DSound;

static u32 vt_ds, vt_buf, vt_notify;
static pthread_mutex_t ds_lock = PTHREAD_MUTEX_INITIALIZER;
static Buf *buffers;
static bool backend_drives;  /* an audio device consumes and advances cursors */

/* ---- cursor movement + notifications (call with ds_lock held) ------------ */
static void advance(Buf *b, double bytes) {
    if (!b->playing || b->primary || !b->size) return;
    double old = b->pos, np = b->pos + bytes;
    bool wrapped = np >= b->size;
    if (wrapped && !b->looping) {
        np = b->size;
        b->playing = false;
    }
    for (u32 i = 0; i < b->nnotify; i++) {
        Notify *n = &b->notify[i];
        bool hit;
        if (n->offset == 0xFFFFFFFFu) hit = !b->playing; /* DSBPN_OFFSETSTOP */
        else if (!wrapped) hit = n->offset >= old && n->offset < np;
        else hit = n->offset >= old || n->offset < np - b->size;
        if (hit) x2_event_set(n->event);
    }
    if (wrapped && b->looping) np = fmod(np, (double)b->size);
    b->pos = np;
}

static void *ticker(void *arg) {
    (void)arg;
    u64 last = x2_time_ns();
    for (;;) {
        struct timespec ts = {0, 2000000};
        nanosleep(&ts, NULL);
        u64 now = x2_time_ns();
        double dt = (double)(now - last) / 1e9;
        last = now;
        if (backend_drives) continue;
        pthread_mutex_lock(&ds_lock);
        for (Buf *b = buffers; b; b = b->next) advance(b, dt * b->freq * b->block);
        pthread_mutex_unlock(&ds_lock);
    }
    return NULL;
}

/* Mix all playing buffers into interleaved stereo float at `rate` Hz.
 * Called by the audio backend; it also becomes the cursor clock. */
void x2_dsound_mix(float *out, u32 frames, u32 rate) {
    memset(out, 0, frames * 2 * sizeof *out);
    backend_drives = true;
    pthread_mutex_lock(&ds_lock);
    static u32 diag;
    if (getenv("X2_AUDIOLOG") && (diag += frames) >= 96000) {
        diag = 0;
        int nb = 0;
        for (Buf *b = buffers; b; b = b->next) {
            nb++;
            if (b->primary || !b->size) continue;
            s16 pk = 0;
            for (u32 i = 0; i + 1 < b->size; i += 2) { s16 s; memcpy(&s, X2P(b->mem + i), 2); if (s > pk) pk = s; }
            x2_log("dsound: buf %u bytes %u Hz playing %d loop %d vol %d pos %.0f peak %d", b->size, b->freq, b->playing, b->looping, b->volume, b->pos, pk);
        }
        x2_log("dsound: %d buffers", nb);
    }
    for (Buf *b = buffers; b; b = b->next) {
        if (!b->playing || b->primary || !b->size) continue;
        float gain = b->volume <= -10000 ? 0.f : powf(10.f, (float)b->volume / 2000.f);
        float pl = b->pan > 0 ? powf(10.f, (float)-b->pan / 2000.f) : 1.f;
        float pr = b->pan < 0 ? powf(10.f, (float)b->pan / 2000.f) : 1.f;
        double step = (double)b->freq / rate;
        const u8 *base = X2P(b->mem);
        for (u32 f = 0; f < frames && b->playing; f++) {
            u32 frame = (u32)(b->pos / b->block);
            u32 off = frame * b->block;
            if (off + b->block > b->size) off = 0;
            float l, r;
            if (b->bits == 16) {
                s16 s0, s1;
                memcpy(&s0, base + off, 2);
                s1 = s0;
                if (b->channels > 1) memcpy(&s1, base + off + 2, 2);
                l = s0 / 32768.f;
                r = s1 / 32768.f;
            } else {
                l = (base[off] - 128) / 128.f;
                r = b->channels > 1 ? (base[off + 1] - 128) / 128.f : l;
            }
            out[2 * f] += l * gain * pl;
            out[2 * f + 1] += r * gain * pr;
            advance(b, step * b->block);
        }
    }
    pthread_mutex_unlock(&ds_lock);
    x2_video_mix(out, frames, rate); /* movie soundtracks */
}

/* ---- IDirectSoundBuffer8 ---------------------------------------------- */
#define BUF Buf *b = COM_THIS(Buf)

static void buf_destroy(X2ComObj *o) {
    Buf *b = (Buf *)o;
    pthread_mutex_lock(&ds_lock);
    for (Buf **p = &buffers; *p; p = &(*p)->next)
        if (*p == b) { *p = b->next; break; }
    pthread_mutex_unlock(&ds_lock);
    if (b->mem) x2_heap_free(0, b->mem);
    free(b);
}

static void B_QueryInterface(X2Cpu *c) {
    BUF;
    u32 iid = RD32(ARG(1));
    if (iid == 0xb0210783u) { /* IID_IDirectSoundNotify {b0210783-89cd-11d0-af08-00a0c925cd16} */
        if (!b->notify_obj) {
            /* the notify object shares the buffer's host struct via a tiny wrapper */
            X2ComObj *n = calloc(1, sizeof(X2ComObj) + sizeof(Buf *));
            *(Buf **)(n + 1) = b;
            b->notify_obj = x2_com_wrap(n, vt_notify, "IDirectSoundNotify");
        } else {
            x2_com_get(b->notify_obj)->refs++;
        }
        WR32(ARG(2), b->notify_obj);
        RET(DS_OK);
        return;
    }
    if (iid == 0x6825a449u || iid == 0x279afa85u) { /* IDirectSoundBuffer8 / IDirectSoundBuffer */
        b->com.refs++;
        WR32(ARG(2), ARG(0));
        RET(DS_OK);
        return;
    }
    x2_log("dsound: QueryInterface for unsupported IID %08x", iid);
    WR32(ARG(2), 0);
    RET(DSERR_NOINTERFACE);
}
static void B_GetCaps(X2Cpu *c) {
    BUF;
    u32 p = ARG(1);
    WR32(p + 4, 0x100E0);
    WR32(p + 8, b->size);
    WR32(p + 12, 0);
    WR32(p + 16, 0);
    RET(DS_OK);
}
static void B_GetCurrentPosition(X2Cpu *c) {
    BUF;
    pthread_mutex_lock(&ds_lock);
    u32 play = b->size ? (u32)b->pos % b->size : 0;
    u32 write = b->size ? (play + b->block * (b->freq / 100 + 1)) % b->size : 0; /* ~10 ms ahead */
    pthread_mutex_unlock(&ds_lock);
    if (ARG(1)) WR32(ARG(1), play);
    if (ARG(2)) WR32(ARG(2), write);
    RET(DS_OK);
}
static void B_GetFormat(X2Cpu *c) {
    BUF;
    u32 p = ARG(1);
    if (p && ARG(2) >= 16) {
        WR16(p, 1);
        WR16(p + 2, b->channels);
        WR32(p + 4, b->rate);
        WR32(p + 8, b->rate * b->block);
        WR16(p + 12, b->block);
        WR16(p + 14, b->bits);
        if (ARG(2) >= 18) WR16(p + 16, 0);
    }
    if (ARG(3)) WR32(ARG(3), 18);
    RET(DS_OK);
}
static void B_GetVolume(X2Cpu *c) { BUF; WR32(ARG(1), (u32)b->volume); RET(DS_OK); }
static void B_GetPan(X2Cpu *c) { BUF; WR32(ARG(1), (u32)b->pan); RET(DS_OK); }
static void B_GetFrequency(X2Cpu *c) { BUF; WR32(ARG(1), b->freq); RET(DS_OK); }
static void B_GetStatus(X2Cpu *c) {
    BUF;
    WR32(ARG(1), (b->playing ? DSBSTATUS_PLAYING : 0) | (b->playing && b->looping ? DSBSTATUS_LOOPING : 0));
    RET(DS_OK);
}
static void B_Lock(X2Cpu *c) {
    BUF;
    u32 off = ARG(1), n = ARG(2), flags = ARG(7);
    if (flags & 2) n = b->size;                    /* DSBLOCK_ENTIREBUFFER */
    if (flags & 1) off = (u32)b->pos % (b->size ? b->size : 1); /* DSBLOCK_FROMWRITECURSOR */
    if (!b->size || n > b->size) { RET(DSERR_INVALIDPARAM); return; }
    off %= b->size;
    u32 n1 = off + n <= b->size ? n : b->size - off;
    WR32(ARG(3), b->mem + off);
    WR32(ARG(4), n1);
    if (ARG(5)) WR32(ARG(5), n1 < n ? b->mem : 0);
    if (ARG(6)) WR32(ARG(6), n - n1);
    RET(DS_OK);
}
/* Unlock(this, p1, n1, p2, n2) */
static void B_Unlock(X2Cpu *c) {
    if (getenv("X2_AUDIOLOG")) {
        static u32 calls;
        if (++calls % 50 == 0) {
            s16 pk = 0;
            for (u32 i = 0; i + 1 < ARG(2); i += 2) { s16 s; memcpy(&s, X2P(ARG(1) + i), 2); if (s > pk) pk = s; }
            x2_log("dsound: Unlock #%u %08x+%u (+%u) peak %d", calls, ARG(1), ARG(2), ARG(4), pk);
        }
    }
    RET(DS_OK);
}
static void B_Play(X2Cpu *c) {
    BUF;
    pthread_mutex_lock(&ds_lock);
    b->playing = true;
    b->looping = (ARG(3) & DSBPLAY_LOOPING) != 0;
    pthread_mutex_unlock(&ds_lock);
    RET(DS_OK);
}
static void B_Stop(X2Cpu *c) {
    BUF;
    pthread_mutex_lock(&ds_lock);
    b->playing = false;
    for (u32 i = 0; i < b->nnotify; i++)
        if (b->notify[i].offset == 0xFFFFFFFFu) x2_event_set(b->notify[i].event);
    pthread_mutex_unlock(&ds_lock);
    RET(DS_OK);
}
static void B_SetCurrentPosition(X2Cpu *c) {
    BUF;
    pthread_mutex_lock(&ds_lock);
    b->pos = b->size ? ARG(1) % b->size : 0;
    pthread_mutex_unlock(&ds_lock);
    RET(DS_OK);
}
static void B_SetFormat(X2Cpu *c) { RET(DS_OK); } /* primary buffer format: we always mix to float stereo */
static void B_SetVolume(X2Cpu *c) { BUF; b->volume = (s32)ARG(1); RET(DS_OK); }
static void B_SetPan(X2Cpu *c) { BUF; b->pan = (s32)ARG(1); RET(DS_OK); }
static void B_SetFrequency(X2Cpu *c) {
    BUF;
    b->freq = ARG(1) ? ARG(1) : b->rate; /* DSBFREQUENCY_ORIGINAL = 0 */
    RET(DS_OK);
}
static void B_Restore(X2Cpu *c) { RET(DS_OK); }

static void N_SetNotificationPositions(X2Cpu *c) {
    Buf *b = *(Buf **)(x2_com_get(ARG(0)) + 1);
    u32 n = ARG(1), p = ARG(2);
    if (n > 64) { RET(DSERR_INVALIDPARAM); return; }
    pthread_mutex_lock(&ds_lock);
    for (u32 i = 0; i < n; i++) b->notify[i] = (Notify){RD32(p + 8 * i), RD32(p + 8 * i + 4)};
    b->nnotify = n;
    pthread_mutex_unlock(&ds_lock);
    RET(DS_OK);
}

/* ---- IDirectSound8 ----------------------------------------------------- */
static void DS_CreateSoundBuffer(X2Cpu *c) {
    u32 desc = ARG(1), out = ARG(2);
    u32 flags = RD32(desc + 4), bytes = RD32(desc + 8), wfx = RD32(desc + 16);
    Buf *b = calloc(1, sizeof *b);
    b->primary = flags & DSBCAPS_PRIMARYBUFFER;
    b->channels = 2;
    b->bits = 16;
    b->rate = 44100;
    if (wfx) {
        b->channels = RD16(wfx + 2);
        b->rate = RD32(wfx + 4);
        b->bits = RD16(wfx + 14);
    }
    b->block = (u16)(b->channels * b->bits / 8);
    if (!b->block) b->block = 4;
    b->freq = b->rate;
    b->size = b->primary ? 0 : bytes;
    if (b->size) b->mem = x2_galloc(b->size);
    b->com.destroy = buf_destroy;
    pthread_mutex_lock(&ds_lock);
    b->next = buffers;
    buffers = b;
    pthread_mutex_unlock(&ds_lock);
    X2_TRACE("dsound: CreateSoundBuffer %s %u bytes, %u ch %u Hz %u bit, flags %x", b->primary ? "primary" : "secondary",
             b->size, b->channels, b->rate, b->bits, flags);
    WR32(out, x2_com_wrap(&b->com, vt_buf, "IDirectSoundBuffer8"));
    RET(DS_OK);
}
static void DS_GetCaps(X2Cpu *c) {
    u32 p = ARG(1);
    u32 sz = RD32(p);
    memset(X2P(p + 4), 0, sz > 4 ? sz - 4 : 0);
    WR32(p + 4, 0x00000F5F);
    WR32(p + 8, 100);
    WR32(p + 12, 200000);
    RET(DS_OK);
}
static void DS_DuplicateSoundBuffer(X2Cpu *c) {
    Buf *src = (Buf *)x2_com_get(ARG(1));
    if (!src) { RET(DSERR_INVALIDPARAM); return; }
    Buf *b = calloc(1, sizeof *b);
    *b = *src;
    b->com = (X2ComObj){0};
    b->com.destroy = buf_destroy;
    b->playing = false;
    b->pos = 0;
    b->nnotify = 0;
    b->notify_obj = 0;
    b->mem = x2_galloc(b->size); /* shares contents at duplication time */
    memcpy(X2P(b->mem), X2P(src->mem), b->size);
    pthread_mutex_lock(&ds_lock);
    b->next = buffers;
    buffers = b;
    pthread_mutex_unlock(&ds_lock);
    WR32(ARG(2), x2_com_wrap(&b->com, vt_buf, "IDirectSoundBuffer8"));
    RET(DS_OK);
}
static void DS_Ok(X2Cpu *c) { RET(DS_OK); }
static void DS_GetSpeakerConfig(X2Cpu *c) { WR32(ARG(1), 4); RET(DS_OK); } /* DSSPEAKER_STEREO */

#define M(name, fn, n) {#name, fn, n}
#define U(name, n) {#name, NULL, n}

u32 x2_dsound_create(void) {
    static bool started;
    if (!started) {
        started = true;
        static const X2Method ds[] = {
            M(QueryInterface, x2_com_QueryInterface_self, 3), M(AddRef, x2_com_AddRef, 1),
            M(Release, x2_com_Release, 1), M(CreateSoundBuffer, DS_CreateSoundBuffer, 4), M(GetCaps, DS_GetCaps, 2),
            M(DuplicateSoundBuffer, DS_DuplicateSoundBuffer, 3), M(SetCooperativeLevel, DS_Ok, 3),
            M(Compact, DS_Ok, 1), M(GetSpeakerConfig, DS_GetSpeakerConfig, 2), M(SetSpeakerConfig, DS_Ok, 2),
            M(Initialize, DS_Ok, 2), M(VerifyCertification, DS_Ok, 2),
        };
        vt_ds = x2_com_vtable("IDirectSound8", ds, sizeof ds / sizeof *ds);
        static const X2Method buf[] = {
            M(QueryInterface, B_QueryInterface, 3), M(AddRef, x2_com_AddRef, 1), M(Release, x2_com_Release, 1),
            M(GetCaps, B_GetCaps, 2), M(GetCurrentPosition, B_GetCurrentPosition, 3), M(GetFormat, B_GetFormat, 4),
            M(GetVolume, B_GetVolume, 2), M(GetPan, B_GetPan, 2), M(GetFrequency, B_GetFrequency, 2),
            M(GetStatus, B_GetStatus, 2), M(Initialize, DS_Ok, 3), M(Lock, B_Lock, 8), M(Play, B_Play, 4),
            M(SetCurrentPosition, B_SetCurrentPosition, 2), M(SetFormat, B_SetFormat, 2),
            M(SetVolume, B_SetVolume, 2), M(SetPan, B_SetPan, 2), M(SetFrequency, B_SetFrequency, 2),
            M(Stop, B_Stop, 1), M(Unlock, B_Unlock, 5), M(Restore, B_Restore, 1), U(SetFX, 4),
            U(AcquireResources, 4), U(GetObjectInPath, 5),
        };
        vt_buf = x2_com_vtable("IDirectSoundBuffer8", buf, sizeof buf / sizeof *buf);
        static const X2Method nt[] = {
            M(QueryInterface, x2_com_QueryInterface_self, 3), M(AddRef, x2_com_AddRef, 1),
            M(Release, x2_com_Release, 1), M(SetNotificationPositions, N_SetNotificationPositions, 3),
        };
        vt_notify = x2_com_vtable("IDirectSoundNotify", nt, 4);
        pthread_t t;
        pthread_create(&t, NULL, ticker, NULL);
        pthread_detach(t);
    }
    DSound *d = calloc(1, sizeof *d);
    return x2_com_wrap(&d->com, vt_ds, "IDirectSound8");
}
