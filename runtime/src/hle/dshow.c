/* DirectShow filter graph emulation (CLSID_FilterGraph).
 *
 * KOF XII plays its WMV movies through a graph: RenderFile, swap the default
 * "Video Renderer" for its own renderer filter, connect pins, Run, then wait
 * for EC_COMPLETE through IMediaEvent / IMediaPosition.
 *
 * Current behaviour: every graph operation succeeds and Run() completes the
 * movie immediately (EC_COMPLETE), so the game moves on. Real playback needs a
 * WMV/WMA decoder pushing samples into the game's renderer filter; the graph
 * object keeps the file name for that. */
#include <stdlib.h>
#include <string.h>

#include <math.h>
#include <time.h>

#include "../com.h"
#include "../video.h"

#define EC_COMPLETE 0x01
#define E_ABORT 0x80004004u
#define VFW_E_NOT_FOUND 0x80040216u

typedef struct Graph Graph;

/* every tear-off interface is a small COM object pointing back at the graph */
typedef struct {
    X2ComObj com;
    Graph *g;
} Tear;

struct Graph {
    X2ComObj com;
    u32 control, event, position, audio, step, seeking, filter_vr, pin;
    u32 event_handle;
    bool running, complete_pending;
    s32 volume;
    double duration, position_s;
    char file[512];
    X2Video *video;
    bool completed;
};

static u32 vt_graph, vt_control, vt_event, vt_position, vt_audio, vt_step, vt_filter, vt_pin, vt_enum;

static Graph *graph_of(u32 obj) {
    X2ComObj *o = x2_com_get(obj);
    if (!o) return NULL;
    if (!strcmp(o->iface, "IGraphBuilder")) return (Graph *)o;
    return ((Tear *)o)->g;
}

static u32 tear(Graph *g, u32 vt, const char *iface) {
    Tear *t = calloc(1, sizeof *t);
    t->g = g;
    t->com.immortal = true;
    return x2_com_wrap(&t->com, vt, iface);
}

static void ret_iface(X2Cpu *c, u32 obj, u32 out) {
    x2_com_get(obj)->refs++;
    WR32(out, obj);
    RET(S_OK);
}

/* QueryInterface shared by the graph and all of its tear-offs */
static void G_QueryInterface(X2Cpu *c) {
    Graph *g = graph_of(ARG(0));
    u32 iid = RD32(ARG(1)), out = ARG(2);
    switch (iid) {
        case 0x56a868a9u: case 0x56a8689fu: case 0x00000000u: ret_iface(c, g->com.guest, out); return; /* IGraphBuilder, IFilterGraph, IUnknown */
        case 0x56a868b1u: ret_iface(c, g->control, out); return;   /* IMediaControl */
        case 0x56a868b6u: case 0x56a868c0u: ret_iface(c, g->event, out); return; /* IMediaEvent(Ex) */
        case 0x56a868b2u: ret_iface(c, g->position, out); return;  /* IMediaPosition */
        case 0x56a868b3u: ret_iface(c, g->audio, out); return;     /* IBasicAudio */
        case 0xe46a9787u: ret_iface(c, g->step, out); return;      /* IVideoFrameStep */
    }
    x2_log("dshow: QueryInterface for unsupported IID %08x", iid);
    WR32(out, 0);
    RET(E_NOINTERFACE);
}

static void Ok(X2Cpu *c) { RET(S_OK); }
static void NotImpl(X2Cpu *c) { RET(E_NOTIMPL); }

/* ---- IGraphBuilder ------------------------------------------------------------- */
static void GB_AddFilter(X2Cpu *c) {
    X2ComObj *f = x2_com_get(ARG(1));
    if (f) f->refs++; /* the graph holds a reference to every filter */
    X2_TRACE("dshow: AddFilter(%08x)", ARG(1));
    RET(S_OK);
}
static void GB_FindFilterByName(X2Cpu *c) {
    Graph *g = graph_of(ARG(0));
    ret_iface(c, g->filter_vr, ARG(2));
}
static void GB_RenderFile(X2Cpu *c) {
    Graph *g = graph_of(ARG(0));
    u16 *w = GWSTR(ARG(1));
    size_t k = 0;
    for (; w && w[k] && k + 1 < sizeof g->file; k++) g->file[k] = w[k] < 0x80 ? (char)w[k] : '?';
    g->file[k] = 0;
    char hp[1024];
    x2_host_path(g->file, hp, sizeof hp);
    if (g->video) x2_video_close(g->video);
    g->video = x2_video_open(hp);
    g->completed = false;
    if (g->video) {
        int vw, vh;
        x2_video_info(g->video, &vw, &vh, &g->duration);
        X2_TRACE("dshow: RenderFile(\"%s\") %.1fs", g->file, g->duration);
    } else {
        x2_log("dshow: RenderFile(\"%s\") - no decoder, completing immediately", g->file);
    }
    RET(S_OK);
}
static void GB_AddSourceFilter(X2Cpu *c) {
    Graph *g = graph_of(ARG(0));
    ret_iface(c, g->filter_vr, ARG(3));
}

/* ---- playback-end monitor ------------------------------------------------------- */
static Graph *active;  /* graph whose frames the SampleGrabber hands out */
static pthread_mutex_t graphs_lock = PTHREAD_MUTEX_INITIALIZER;
static Graph *graphs[64];
static int ngraphs;

static void signal_complete(Graph *g) {
    if (g->completed) return;
    g->completed = true;
    g->complete_pending = true;
    x2_event_set(g->event_handle);
}

static void *monitor(void *arg) {
    (void)arg;
    for (;;) {
        struct timespec ts = {0, 10000000};
        nanosleep(&ts, NULL);
        pthread_mutex_lock(&graphs_lock);
        for (int i = 0; i < ngraphs; i++) {
            Graph *g = graphs[i];
            if (g->running && g->video && x2_video_finished(g->video)) signal_complete(g);
        }
        pthread_mutex_unlock(&graphs_lock);
    }
    return NULL;
}

/* ---- IMediaControl ----------------------------------------------------------------- */
static void MC_Run(X2Cpu *c) {
    Graph *g = graph_of(ARG(0));
    g->running = true;
    active = g;
    if (g->video) {
        x2_video_set_volume(g->video, g->volume <= -10000 ? 0.f : powf(10.f, (float)g->volume / 2000.f));
        x2_video_run(g->video);
    } else {
        g->position_s = g->duration;
        pthread_mutex_lock(&graphs_lock);
        signal_complete(g);
        pthread_mutex_unlock(&graphs_lock);
    }
    RET(S_OK);
}
static void MC_Pause(X2Cpu *c) {
    Graph *g = graph_of(ARG(0));
    if (g->video) x2_video_pause(g->video);
    RET(S_OK);
}
static void MC_Stop(X2Cpu *c) {
    Graph *g = graph_of(ARG(0));
    g->running = false;
    if (g->video) x2_video_pause(g->video);
    RET(S_OK);
}
static void MC_GetState(X2Cpu *c) {
    WR32(ARG(2), graph_of(ARG(0))->running ? 2 : 0); /* State_Running / State_Stopped */
    RET(S_OK);
}

/* ---- IMediaEvent -------------------------------------------------------------------- */
static void ME_GetEventHandle(X2Cpu *c) {
    WR32(ARG(1), graph_of(ARG(0))->event_handle);
    RET(S_OK);
}
static void ME_GetEvent(X2Cpu *c) {
    Graph *g = graph_of(ARG(0));
    pthread_mutex_lock(&graphs_lock);
    bool pending = g->complete_pending;
    g->complete_pending = false;
    pthread_mutex_unlock(&graphs_lock);
    if (!pending) { RET(E_ABORT); return; }
    extern bool x2_event_reset(u32 handle);
    x2_event_reset(g->event_handle);
    WR32(ARG(1), EC_COMPLETE);
    WR32(ARG(2), 0);
    WR32(ARG(3), 0);
    RET(S_OK);
}
static void ME_WaitForCompletion(X2Cpu *c) {
    WR32(ARG(2), EC_COMPLETE);
    RET(S_OK);
}

/* ---- IMediaPosition (doubles are passed by value as two dwords) ---------------- */
static void MP_get_Duration(X2Cpu *c) { WRF64(ARG(1), graph_of(ARG(0))->duration); RET(S_OK); }
static void MP_put_CurrentPosition(X2Cpu *c) {
    Graph *g = graph_of(ARG(0));
    u64 bits = (u64)ARG(2) << 32 | ARG(1);
    double v;
    memcpy(&v, &bits, 8);
    g->position_s = v;
    if (g->video) {
        x2_video_seek(g->video, v);
        g->completed = false;
    }
    RET(S_OK);
}
static void MP_get_CurrentPosition(X2Cpu *c) {
    Graph *g = graph_of(ARG(0));
    WRF64(ARG(1), g->video ? x2_video_position(g->video) : g->position_s);
    RET(S_OK);
}
static void MP_get_Double(X2Cpu *c) { WRF64(ARG(1), 0.0); RET(S_OK); }
static void MP_get_Rate(X2Cpu *c) { WRF64(ARG(1), 1.0); RET(S_OK); }
static void MP_Can(X2Cpu *c) { WR32(ARG(1), 0xFFFFFFFFu); RET(S_OK); }

/* ---- IBasicAudio ----------------------------------------------------------------------- */
static void BA_put_Volume(X2Cpu *c) {
    Graph *g = graph_of(ARG(0));
    g->volume = (s32)ARG(1);
    if (g->video) x2_video_set_volume(g->video, g->volume <= -10000 ? 0.f : powf(10.f, (float)g->volume / 2000.f));
    RET(S_OK);
}
static void BA_get_Volume(X2Cpu *c) { WR32(ARG(1), (u32)graph_of(ARG(0))->volume); RET(S_OK); }
static void BA_get_Balance(X2Cpu *c) { WR32(ARG(1), 0); RET(S_OK); }

/* ---- IDispatch stubs --------------------------------------------------------------- */
static void D_GetTypeInfoCount(X2Cpu *c) { WR32(ARG(1), 0); RET(S_OK); }

/* ---- stand-in filter / pin (what FindFilterByName/FindPin hand out) ------------- */
static void F_GetClassID(X2Cpu *c) { memset(X2P(ARG(1)), 0, 16); RET(S_OK); }
static void F_GetState(X2Cpu *c) { WR32(ARG(2), graph_of(ARG(0))->running ? 2 : 0); RET(S_OK); }
static void F_FindPin(X2Cpu *c) { ret_iface(c, graph_of(ARG(0))->pin, ARG(2)); }
static void F_EnumPins(X2Cpu *c) {
    Graph *g = graph_of(ARG(0));
    WR32(ARG(1), tear(g, vt_enum, "IEnumPins"));
    RET(S_OK);
}
static void F_QueryFilterInfo(X2Cpu *c) {
    memset(X2P(ARG(1)), 0, 128 * 2 + 4);
    const char *n = "Video Renderer";
    for (u32 i = 0; n[i]; i++) WR16(ARG(1) + 2 * i, (u8)n[i]);
    RET(S_OK);
}
static void EP_Next(X2Cpu *c) {
    /* one pin, then done */
    Tear *t = (Tear *)x2_com_get(ARG(0));
    static u32 served_for;
    if (served_for == t->com.guest || ARG(1) == 0) {
        if (ARG(3)) WR32(ARG(3), 0);
        RET(1); /* S_FALSE */
        return;
    }
    served_for = t->com.guest;
    x2_com_get(t->g->pin)->refs++;
    WR32(ARG(2), t->g->pin);
    if (ARG(3)) WR32(ARG(3), 1);
    RET(S_OK);
}
static void P_ConnectedTo(X2Cpu *c) { WR32(ARG(1), 0); RET(0x80040209u); /* VFW_E_NOT_CONNECTED */ }
static void P_QueryDirection(X2Cpu *c) { WR32(ARG(1), 1); RET(S_OK); /* PINDIR_OUTPUT */ }
static void P_QueryPinInfo(X2Cpu *c) {
    Graph *g = graph_of(ARG(0));
    memset(X2P(ARG(1)), 0, 4 + 4 + 128 * 2);
    x2_com_get(g->filter_vr)->refs++;
    WR32(ARG(1), g->filter_vr);
    WR32(ARG(1) + 4, 1);
    RET(S_OK);
}

/* ---- stand-in filters: CLSID_SampleGrabber / CLSID_NullRenderer ------------- */
typedef struct {
    X2ComObj com;
    bool grabber;
    u32 grabber_iface; /* ISampleGrabber tear-off (same host object) */
    u32 callback;
} Filter;
static u32 vt_sfilter, vt_grabber;

static Filter *filter_of(u32 g) {
    X2ComObj *o = x2_com_get(g);
    if (!o) return NULL;
    if (!strcmp(o->iface, "ISampleGrabber")) return *(Filter **)(o + 1);
    return (Filter *)o;
}
static void SF_QueryInterface(X2Cpu *c) {
    Filter *f = filter_of(ARG(0));
    u32 iid = RD32(ARG(1)), out = ARG(2);
    if (iid == 0x6b652fffu && f->grabber) { /* IID_ISampleGrabber */
        if (!f->grabber_iface) {
            X2ComObj *t = calloc(1, sizeof(X2ComObj) + sizeof(Filter *));
            t->immortal = true;
            *(Filter **)(t + 1) = f;
            f->grabber_iface = x2_com_wrap(t, vt_grabber, "ISampleGrabber");
        } else {
            x2_com_get(f->grabber_iface)->refs++;
        }
        WR32(out, f->grabber_iface);
        RET(S_OK);
        return;
    }
    if (iid == 0x56a86895u || iid == 0x56a86899u || iid == 0x0000010cu || iid == 0u) { /* IBaseFilter, IMediaFilter, IPersist, IUnknown */
        f->com.refs++;
        WR32(out, f->com.guest);
        RET(S_OK);
        return;
    }
    x2_log("dshow: filter QueryInterface for unsupported IID %08x", iid);
    WR32(out, 0);
    RET(E_NOINTERFACE);
}
static void SF_GetState(X2Cpu *c) { WR32(ARG(2), 0); RET(S_OK); }
static void SG_SetCallback(X2Cpu *c) {
    filter_of(ARG(0))->callback = ARG(1);
    RET(S_OK);
}
/* AM_MEDIA_TYPE for 1280x720 RGB32 video with a VIDEOINFOHEADER */
static void SG_GetConnectedMediaType(X2Cpu *c) {
    u32 mt = ARG(1);
    memset(X2P(mt), 0, 72);
    static const u8 video[16] = {0x76, 0x69, 0x64, 0x73, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
    /* MEDIASUBTYPE_YV12 {32315659-0000-0010-8000-00aa00389b71}: what KOF XII asks for */
    static const u8 rgb32[16] = {0x59, 0x56, 0x31, 0x32, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
    static const u8 vinfo[16] = {0x80, 0x9f, 0x58, 0x05, 0x56, 0xc3, 0xce, 0x11, 0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a};
    memcpy(X2P(mt), video, 16);
    memcpy(X2P(mt + 16), rgb32, 16);
    WR32(mt + 32, 1);                      /* bFixedSizeSamples */
    WR32(mt + 40, 1280 * 720 * 3 / 2);     /* lSampleSize */
    memcpy(X2P(mt + 44), vinfo, 16);
    u32 vih = x2_galloc(88);
    WR32(vih + 8, 1280); WR32(vih + 12, 720);           /* rcSource right/bottom */
    WR32(vih + 24, 1280); WR32(vih + 28, 720);          /* rcTarget */
    WR64(vih + 40, 333667);                             /* AvgTimePerFrame: 29.97 fps */
    WR32(vih + 48, 40);                                 /* bmiHeader.biSize */
    WR32(vih + 52, 1280);
    WR32(vih + 56, 720);
    WR16(vih + 60, 1);
    WR16(vih + 62, 12);
    WR32(vih + 64, 0x32315659u);           /* biCompression = 'YV12' */
    WR32(vih + 68, 1280 * 720 * 3 / 2);
    WR32(mt + 64, 88);                     /* cbFormat */
    WR32(mt + 68, vih);                    /* pbFormat */
    RET(S_OK);
}

static void SF_Ok(X2Cpu *c) { RET(S_OK); }
static void SG_SetMediaType(X2Cpu *c) {
    u32 mt = ARG(1);
    x2_log("dshow: SampleGrabber::SetMediaType major %08x sub %08x-%04x format %08x cb %u", RD32(mt), RD32(mt + 16), RD16(mt + 20), RD32(mt + 44), RD32(mt + 64));
    RET(S_OK);
}

/* GetCurrentBuffer(this, long *size, long *buffer): size query when buffer is
 * NULL, else copy the latest frame. No decoder yet: frames are black. */
static void SG_GetCurrentBuffer(X2Cpu *c) {
    u32 need = 1280 * 720 * 3 / 2, psize = ARG(1), buf = ARG(2);
    if (!buf) { WR32(psize, need); RET(S_OK); return; }
    u32 have = RD32(psize);
    if (have < need) { RET(0x8007000Eu); return; }
    if (!active || !active->video || !x2_video_frame_yv12(active->video, X2P(buf), have)) {
        memset(X2P(buf), 16, 1280 * 720);              /* black: Y 16, U/V 128 */
        memset(X2P(buf + 1280 * 720), 128, 1280 * 720 / 2);
    }
    WR32(psize, need);
    RET(S_OK);
}

/* pins of stand-in filters: { filter, direction } */
typedef struct {
    X2ComObj com;
    Filter *f;
    u32 dir; /* PINDIR_INPUT = 0, PINDIR_OUTPUT = 1 */
} SPin;
typedef struct {
    X2ComObj com;
    Filter *f;
    u32 pos;
} SEnum;
static u32 vt_spin, vt_senum;

static u32 make_spin(Filter *f, u32 dir) {
    SPin *p = calloc(1, sizeof *p);
    p->com.immortal = true;
    p->f = f;
    p->dir = dir;
    return x2_com_wrap(&p->com, vt_spin, "IPin(standin)");
}
static void SP_QueryDirection(X2Cpu *c) { WR32(ARG(1), ((SPin *)x2_com_get(ARG(0)))->dir); RET(S_OK); }
static void SP_QueryPinInfo(X2Cpu *c) {
    SPin *p = (SPin *)x2_com_get(ARG(0));
    memset(X2P(ARG(1)), 0, 8 + 256);
    p->f->com.refs++;
    WR32(ARG(1), p->f->com.guest);
    WR32(ARG(1) + 4, p->dir);
    const char *n = p->dir ? "Output" : "Input";
    for (u32 i = 0; n[i]; i++) WR16(ARG(1) + 8 + 2 * i, (u8)n[i]);
    RET(S_OK);
}
static void SP_ConnectedTo(X2Cpu *c) { WR32(ARG(1), 0); RET(0x80040209u); }
static void SP_ConnectionMediaType(X2Cpu *c) {
    WR32(c->esp + 4, ARG(0)); /* same layout as ISampleGrabber::GetConnectedMediaType(this, mt) */
    SG_GetConnectedMediaType(c);
}
static void SE_Next(X2Cpu *c) {
    SEnum *e = (SEnum *)x2_com_get(ARG(0));
    u32 want = ARG(1), out = ARG(2), fetched = 0;
    u32 npins = e->f->grabber ? 2 : 1;
    while (fetched < want && e->pos < npins) {
        WR32(out + 4 * fetched, make_spin(e->f, e->pos));
        e->pos++;
        fetched++;
    }
    if (ARG(3)) WR32(ARG(3), fetched);
    RET(fetched == want ? S_OK : 1);
}
static void SE_Reset(X2Cpu *c) { ((SEnum *)x2_com_get(ARG(0)))->pos = 0; RET(S_OK); }
static void SF_EnumPins(X2Cpu *c) {
    SEnum *e = calloc(1, sizeof *e);
    e->com.immortal = true;
    e->f = filter_of(ARG(0));
    WR32(ARG(1), x2_com_wrap(&e->com, vt_senum, "IEnumPins(standin)"));
    RET(S_OK);
}
static void SF_FindPin(X2Cpu *c) {
    Filter *f = filter_of(ARG(0));
    u16 first = RD16(ARG(1));
    WR32(ARG(2), make_spin(f, (first == 'O' || first == 'o') ? 1 : 0));
    RET(S_OK);
}
static void SF_GetClassID(X2Cpu *c) { memset(X2P(ARG(1)), 0, 16); RET(S_OK); }
static void SF_QueryFilterInfo(X2Cpu *c) { memset(X2P(ARG(1)), 0, 128 * 2 + 4); RET(S_OK); }

u32 x2_dshow_create_filter(bool grabber) {
    if (!vt_spin) {
        static const X2Method pin[] = {
            {"QueryInterface", x2_com_QueryInterface_self, 3}, {"AddRef", x2_com_AddRef, 1},
            {"Release", x2_com_Release, 1}, {"Connect", SF_Ok, 3}, {"ReceiveConnection", SF_Ok, 3},
            {"Disconnect", SF_Ok, 1}, {"ConnectedTo", SP_ConnectedTo, 2},
            {"ConnectionMediaType", SP_ConnectionMediaType, 2}, {"QueryPinInfo", SP_QueryPinInfo, 2},
            {"QueryDirection", SP_QueryDirection, 2}, {"QueryId", NULL, 2}, {"QueryAccept", SF_Ok, 2},
            {"EnumMediaTypes", NULL, 2}, {"QueryInternalConnections", NULL, 3}, {"EndOfStream", SF_Ok, 1},
            {"BeginFlush", SF_Ok, 1}, {"EndFlush", SF_Ok, 1}, {"NewSegment", SF_Ok, 6},
        };
        vt_spin = x2_com_vtable("IPin(standin)", pin, sizeof pin / sizeof *pin);
        static const X2Method en[] = {
            {"QueryInterface", x2_com_QueryInterface_self, 3}, {"AddRef", x2_com_AddRef, 1},
            {"Release", x2_com_Release, 1}, {"Next", SE_Next, 4}, {"Skip", SF_Ok, 2}, {"Reset", SE_Reset, 1},
            {"Clone", NULL, 2},
        };
        vt_senum = x2_com_vtable("IEnumPins(standin)", en, sizeof en / sizeof *en);
    }
    if (!vt_sfilter) {
        static const X2Method bf[] = {
            {"QueryInterface", SF_QueryInterface, 3}, {"AddRef", x2_com_AddRef, 1}, {"Release", x2_com_Release, 1},
            {"GetClassID", SF_GetClassID, 2}, {"Stop", SF_Ok, 1}, {"Pause", SF_Ok, 1}, {"Run", SF_Ok, 3},
            {"GetState", SF_GetState, 3}, {"SetSyncSource", SF_Ok, 2}, {"GetSyncSource", NULL, 2},
            {"EnumPins", SF_EnumPins, 2}, {"FindPin", SF_FindPin, 3}, {"QueryFilterInfo", SF_QueryFilterInfo, 2},
            {"JoinFilterGraph", SF_Ok, 3}, {"QueryVendorInfo", NULL, 2},
        };
        vt_sfilter = x2_com_vtable("IBaseFilter(standin)", bf, sizeof bf / sizeof *bf);
        static const X2Method sg[] = {
            {"QueryInterface", SF_QueryInterface, 3}, {"AddRef", x2_com_AddRef, 1}, {"Release", x2_com_Release, 1},
            {"SetOneShot", SF_Ok, 2}, {"SetMediaType", SG_SetMediaType, 2}, {"GetConnectedMediaType", SG_GetConnectedMediaType, 2},
            {"SetBufferSamples", SF_Ok, 2}, {"GetCurrentBuffer", SG_GetCurrentBuffer, 3}, {"GetCurrentSample", NULL, 2},
            {"SetCallback", SG_SetCallback, 3},
        };
        vt_grabber = x2_com_vtable("ISampleGrabber", sg, sizeof sg / sizeof *sg);
    }
    Filter *f = calloc(1, sizeof *f);
    f->grabber = grabber;
    f->com.immortal = true;
    return x2_com_wrap(&f->com, vt_sfilter, grabber ? "SampleGrabber" : "NullRenderer");
}

#define M(name, fn, n) {#name, fn, n}
#define U(name, n) {#name, NULL, n}
#define TEAR_IUNKNOWN M(QueryInterface, G_QueryInterface, 3), M(AddRef, x2_com_AddRef, 1), M(Release, x2_com_Release, 1)
#define IDISPATCH M(GetTypeInfoCount, D_GetTypeInfoCount, 2), M(GetTypeInfo, NotImpl, 4), M(GetIDsOfNames, NotImpl, 6), M(Invoke, NotImpl, 9)

static void init_vtables(void) {
    if (vt_graph) return;
    static const X2Method gb[] = {
        TEAR_IUNKNOWN, M(AddFilter, GB_AddFilter, 3), M(RemoveFilter, Ok, 2), U(EnumFilters, 2),
        M(FindFilterByName, GB_FindFilterByName, 3), M(ConnectDirect, Ok, 4), M(Reconnect, Ok, 2),
        M(Disconnect, Ok, 2), M(SetDefaultSyncSource, Ok, 1), M(Connect, Ok, 3), M(Render, Ok, 2),
        M(RenderFile, GB_RenderFile, 3), M(AddSourceFilter, GB_AddSourceFilter, 4), M(SetLogFile, Ok, 2),
        M(Abort, Ok, 1), M(ShouldOperationContinue, Ok, 1),
    };
    vt_graph = x2_com_vtable("IGraphBuilder", gb, sizeof gb / sizeof *gb);
    static const X2Method mc[] = {
        TEAR_IUNKNOWN, IDISPATCH, M(Run, MC_Run, 1), M(Pause, MC_Pause, 1), M(Stop, MC_Stop, 1),
        M(GetState, MC_GetState, 3), U(RenderFile, 2), U(AddSourceFilter, 3), U(get_FilterCollection, 2),
        U(get_RegFilterCollection, 2), M(StopWhenReady, Ok, 1),
    };
    vt_control = x2_com_vtable("IMediaControl", mc, sizeof mc / sizeof *mc);
    static const X2Method me[] = {
        TEAR_IUNKNOWN, IDISPATCH, M(GetEventHandle, ME_GetEventHandle, 2), M(GetEvent, ME_GetEvent, 5),
        M(WaitForCompletion, ME_WaitForCompletion, 3), M(CancelDefaultHandling, Ok, 2),
        M(RestoreDefaultHandling, Ok, 2), M(FreeEventParams, Ok, 4), M(SetNotifyWindow, Ok, 4),
        M(SetNotifyFlags, Ok, 2), M(GetNotifyFlags, Ok, 2),
    };
    vt_event = x2_com_vtable("IMediaEventEx", me, sizeof me / sizeof *me);
    static const X2Method mp[] = {
        TEAR_IUNKNOWN, IDISPATCH, M(get_Duration, MP_get_Duration, 2),
        M(put_CurrentPosition, MP_put_CurrentPosition, 3), M(get_CurrentPosition, MP_get_CurrentPosition, 2),
        M(get_StopTime, MP_get_Double, 2), M(put_StopTime, Ok, 3), M(get_PrerollTime, MP_get_Double, 2),
        M(put_PrerollTime, Ok, 3), M(put_Rate, Ok, 3), M(get_Rate, MP_get_Rate, 2), M(CanSeekForward, MP_Can, 2),
        M(CanSeekBackward, MP_Can, 2),
    };
    vt_position = x2_com_vtable("IMediaPosition", mp, sizeof mp / sizeof *mp);
    static const X2Method ba[] = {
        TEAR_IUNKNOWN, IDISPATCH, M(put_Volume, BA_put_Volume, 2), M(get_Volume, BA_get_Volume, 2),
        M(put_Balance, Ok, 2), M(get_Balance, BA_get_Balance, 2),
    };
    vt_audio = x2_com_vtable("IBasicAudio", ba, sizeof ba / sizeof *ba);
    static const X2Method vs[] = {TEAR_IUNKNOWN, M(Step, Ok, 3), M(CanStep, Ok, 3), M(CancelStep, Ok, 1)};
    vt_step = x2_com_vtable("IVideoFrameStep", vs, 6);
    static const X2Method bf[] = {
        TEAR_IUNKNOWN, M(GetClassID, F_GetClassID, 2), M(Stop, Ok, 1), M(Pause, Ok, 1), M(Run, Ok, 3),
        M(GetState, F_GetState, 3), M(SetSyncSource, Ok, 2), U(GetSyncSource, 2), M(EnumPins, F_EnumPins, 2),
        M(FindPin, F_FindPin, 3), M(QueryFilterInfo, F_QueryFilterInfo, 2), M(JoinFilterGraph, Ok, 3),
        U(QueryVendorInfo, 2),
    };
    vt_filter = x2_com_vtable("IBaseFilter", bf, sizeof bf / sizeof *bf);
    static const X2Method pin[] = {
        TEAR_IUNKNOWN, M(Connect, Ok, 3), M(ReceiveConnection, Ok, 3), M(Disconnect, Ok, 1),
        M(ConnectedTo, P_ConnectedTo, 2), U(ConnectionMediaType, 2), M(QueryPinInfo, P_QueryPinInfo, 2),
        M(QueryDirection, P_QueryDirection, 2), U(QueryId, 2), M(QueryAccept, Ok, 2), U(EnumMediaTypes, 2),
        U(QueryInternalConnections, 3), M(EndOfStream, Ok, 1), M(BeginFlush, Ok, 1), M(EndFlush, Ok, 1),
        M(NewSegment, Ok, 6),
    };
    vt_pin = x2_com_vtable("IPin", pin, sizeof pin / sizeof *pin);
    static const X2Method en[] = {TEAR_IUNKNOWN, M(Next, EP_Next, 4), M(Skip, Ok, 2), M(Reset, Ok, 1), U(Clone, 2)};
    vt_enum = x2_com_vtable("IEnumPins", en, 7);
}

u32 x2_dshow_create_graph(void) {
    init_vtables();
    Graph *g = calloc(1, sizeof *g);
    g->com.immortal = true;
    g->duration = 0.0;
    u32 obj = x2_com_wrap(&g->com, vt_graph, "IGraphBuilder");
    g->control = tear(g, vt_control, "IMediaControl");
    g->event = tear(g, vt_event, "IMediaEventEx");
    g->position = tear(g, vt_position, "IMediaPosition");
    g->audio = tear(g, vt_audio, "IBasicAudio");
    g->step = tear(g, vt_step, "IVideoFrameStep");
    g->filter_vr = tear(g, vt_filter, "IBaseFilter");
    g->pin = tear(g, vt_pin, "IPin");
    /* manual-reset event signalled whenever an event is pending */
    extern u32 x2_create_event(bool manual, bool initial);
    g->event_handle = x2_create_event(true, false);
    pthread_mutex_lock(&graphs_lock);
    if (ngraphs < 64) graphs[ngraphs++] = g;
    static bool monitor_started;
    if (!monitor_started) {
        monitor_started = true;
        pthread_t t;
        pthread_create(&t, NULL, monitor, NULL);
        pthread_detach(t);
    }
    pthread_mutex_unlock(&graphs_lock);
    return obj;
}
