/* FFmpeg movie decoder: WMV7/WMV9 video -> YV12 frames, WMA -> 48 kHz stereo.
 * Per playing movie: a reader thread (demux + audio decode) and a video thread
 * that presents frames by the wall clock started in run(). */
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core.h"
#include "video.h"

#define RING (48000 * 2 * 2) /* 2 s of stereo float */

struct X2Video {
    AVFormatContext *fmt;
    AVCodecContext *vdec, *adec;
    int vstream, astream;
    struct SwsContext *sws;
    SwrContext *swr;
    int w, h;
    double duration;

    pthread_t thread, vthread;
    struct VPkt *vq_head, *vq_tail; /* video packets waiting for the video thread */
    u32 vq_len;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool running, quit, thread_started, eof, audio_done, video_done;
    double pos;              /* playback clock (s) */
    u64 clock_base_ns;       /* wall time corresponding to clock_base_pos */
    double clock_base_pos;
    double seek_to;          /* < 0: none pending */

    u8 *frame;               /* latest YV12 frame */
    bool have_frame;

    float ring[RING];
    u32 ring_r, ring_w;      /* sample indices (floats) */
    float gain;
    struct X2Video *next;
};

static pthread_mutex_t videos_lock = PTHREAD_MUTEX_INITIALIZER;
static X2Video *videos;

static double now_clock(X2Video *v) {
    if (!v->running) return v->clock_base_pos;
    return v->clock_base_pos + (double)(x2_time_ns() - v->clock_base_ns) / 1e9;
}

static void ring_push(X2Video *v, const float *s, u32 n) {
    for (u32 i = 0; i < n; i++) {
        if (v->ring_w - v->ring_r >= RING) break; /* full: drop */
        v->ring[v->ring_w++ % RING] = s[i];
    }
}

static void store_frame(X2Video *v, AVFrame *f) {
    uint8_t *dst[4];
    int ls[4];
    u8 *tmp = malloc((size_t)v->w * v->h * 3 / 2);
    /* YUV420P planes Y, U, V -> pack as YV12 (Y, V, U) */
    dst[0] = tmp;
    dst[2] = tmp + (size_t)v->w * v->h;                 /* U goes last */
    dst[1] = dst[2] + (size_t)(v->w / 2) * (v->h / 2);   /* placeholder, swapped below */
    ls[0] = v->w;
    ls[1] = ls[2] = v->w / 2;
    uint8_t *planes[4] = {tmp, tmp + (size_t)v->w * v->h + (size_t)(v->w / 2) * (v->h / 2), tmp + (size_t)v->w * v->h, NULL};
    (void)dst;
    sws_scale(v->sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, planes, ls);
    pthread_mutex_lock(&v->lock);
    memcpy(v->frame, tmp, (size_t)v->w * v->h * 3 / 2);
    v->have_frame = true;
    pthread_mutex_unlock(&v->lock);
    free(tmp);
}

/* Video packets queued by the reader for the video thread. A node with a NULL
 * packet asks the video thread to flush its decoder (after a seek). */
typedef struct VPkt {
    AVPacket *pkt;
    struct VPkt *next;
} VPkt;

static void vq_push(X2Video *v, AVPacket *p) { /* caller holds v->lock */
    VPkt *n = calloc(1, sizeof *n);
    n->pkt = p;
    if (v->vq_tail) v->vq_tail->next = n;
    else v->vq_head = n;
    v->vq_tail = n;
    v->vq_len++;
    pthread_cond_broadcast(&v->cond);
}

static void vq_clear(X2Video *v) { /* caller holds v->lock */
    while (v->vq_head) {
        VPkt *n = v->vq_head;
        v->vq_head = n->next;
        av_packet_free(&n->pkt);
        free(n);
    }
    v->vq_tail = NULL;
    v->vq_len = 0;
}

/* Decodes queued video packets and shows each frame when the clock reaches it. */
static void *video_thread(void *arg) {
    X2Video *v = arg;
    AVFrame *f = av_frame_alloc();
    AVStream *st = v->fmt->streams[v->vstream];
    for (;;) {
        pthread_mutex_lock(&v->lock);
        while (!v->quit && (!v->running || !v->vq_head)) pthread_cond_wait(&v->cond, &v->lock);
        if (v->quit) { pthread_mutex_unlock(&v->lock); break; }
        VPkt *n = v->vq_head;
        v->vq_head = n->next;
        if (!v->vq_head) v->vq_tail = NULL;
        v->vq_len--;
        pthread_cond_broadcast(&v->cond); /* room for the reader */
        pthread_mutex_unlock(&v->lock);
        if (!n->pkt) { /* seek marker */
            avcodec_flush_buffers(v->vdec);
            free(n);
            continue;
        }
        int sent = avcodec_send_packet(v->vdec, n->pkt);
        av_packet_free(&n->pkt);
        free(n);
        if (sent < 0) continue;
        while (avcodec_receive_frame(v->vdec, f) == 0) {
            double t = f->best_effort_timestamp == AV_NOPTS_VALUE ? 0 : (double)f->best_effort_timestamp * av_q2d(st->time_base);
            bool drop = false;
            for (;;) {
                pthread_mutex_lock(&v->lock);
                double c = now_clock(v);
                bool stop = v->quit || !v->running;
                drop = v->vq_head && !v->vq_head->pkt; /* a seek is pending */
                pthread_mutex_unlock(&v->lock);
                if (stop || drop || c >= t) break;
                double d = t - c;
                struct timespec ts = {0, (long)((d > 0.01 ? 0.01 : d) * 1e9)};
                nanosleep(&ts, NULL);
            }
            if (!drop) store_frame(v, f);
            av_frame_unref(f);
        }
    }
    av_frame_free(&f);
    return NULL;
}

/* Reader: demuxes ahead of playback. Audio is decoded straight into the ring;
 * video packets go to the video thread. Reading ahead matters because some
 * files store many seconds of video before their first audio packet. */
static void *decode_thread(void *arg) {
    X2Video *v = arg;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *f = av_frame_alloc();
    float abuf[8192 * 2];
    for (;;) {
        pthread_mutex_lock(&v->lock);
        for (;;) {
            if (v->quit || v->seek_to >= 0) break;
            u32 audio = v->ring_w - v->ring_r;
            bool audio_ok = v->astream < 0 || audio > 48000; /* 0.5 s of stereo */
            bool full = audio > 3 * 48000 ||                   /* 1.5 s: keep the 2 s ring from overflowing */
                        (audio_ok && v->vq_len >= 8) || v->vq_len >= 4096;
            if (v->running && !v->eof && !full) break;
            pthread_cond_wait(&v->cond, &v->lock);
        }
        if (v->quit) { pthread_mutex_unlock(&v->lock); break; }
        if (v->seek_to >= 0) {
            double t = v->seek_to;
            v->seek_to = -1;
            v->eof = v->audio_done = v->video_done = false;
            v->ring_r = v->ring_w = 0;
            vq_clear(v);
            if (v->vdec) vq_push(v, NULL); /* video thread flushes its decoder */
            pthread_mutex_unlock(&v->lock);
            av_seek_frame(v->fmt, -1, (int64_t)(t * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
            if (v->adec) avcodec_flush_buffers(v->adec);
            continue;
        }
        pthread_mutex_unlock(&v->lock);
        if (av_read_frame(v->fmt, pkt) < 0) {
            pthread_mutex_lock(&v->lock);
            v->eof = true;
            pthread_mutex_unlock(&v->lock);
            continue;
        }
        if (pkt->stream_index == v->vstream && v->vdec) {
            pthread_mutex_lock(&v->lock);
            vq_push(v, av_packet_clone(pkt));
            pthread_mutex_unlock(&v->lock);
            av_packet_unref(pkt);
            continue;
        }
        if (pkt->stream_index != v->astream || !v->adec || avcodec_send_packet(v->adec, pkt) < 0) {
            av_packet_unref(pkt);
            continue;
        }
        av_packet_unref(pkt);
        while (avcodec_receive_frame(v->adec, f) == 0) {
            int out = swr_convert(v->swr, (uint8_t *[]){(uint8_t *)abuf}, 8192, (const uint8_t **)f->extended_data,
                                  f->nb_samples);
            if (out > 0) {
                pthread_mutex_lock(&v->lock);
                ring_push(v, abuf, (u32)out * 2);
                pthread_mutex_unlock(&v->lock);
            }
            av_frame_unref(f);
        }
    }
    av_frame_free(&f);
    av_packet_free(&pkt);
    return NULL;
}

static AVCodecContext *open_codec(AVFormatContext *fmt, int idx) {
    const AVCodec *codec = avcodec_find_decoder(fmt->streams[idx]->codecpar->codec_id);
    if (!codec) return NULL;
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, fmt->streams[idx]->codecpar);
    ctx->thread_count = 2;
    if (avcodec_open2(ctx, codec, NULL) < 0) { avcodec_free_context(&ctx); return NULL; }
    return ctx;
}

X2Video *x2_video_open(const char *path) {
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) { x2_log("video: cannot open %s", path); return NULL; }
    avformat_find_stream_info(fmt, NULL);
    X2Video *v = calloc(1, sizeof *v);
    v->fmt = fmt;
    v->vstream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    v->astream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (v->vstream >= 0) v->vdec = open_codec(fmt, v->vstream);
    if (v->astream >= 0) v->adec = open_codec(fmt, v->astream);
    if (!v->vdec) v->vstream = -1;
    if (!v->adec) v->astream = -1;
    v->w = v->vdec ? v->vdec->width : 1280;
    v->h = v->vdec ? v->vdec->height : 720;
    v->duration = fmt->duration > 0 ? (double)fmt->duration / AV_TIME_BASE : 0;
    v->frame = malloc((size_t)v->w * v->h * 3 / 2);
    /* black: Y = 16, U = V = 128 */
    memset(v->frame, 16, (size_t)v->w * v->h);
    memset(v->frame + (size_t)v->w * v->h, 128, (size_t)v->w * v->h / 2);
    if (v->vdec)
        v->sws = sws_getContext(v->vdec->width, v->vdec->height, v->vdec->pix_fmt, v->w, v->h, AV_PIX_FMT_YUV420P,
                                SWS_BILINEAR, NULL, NULL, NULL);
    if (v->adec) {
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 24, 100) /* FFmpeg 5.1: AVChannelLayout */
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        swr_alloc_set_opts2(&v->swr, &stereo, AV_SAMPLE_FMT_FLT, 48000, &v->adec->ch_layout, v->adec->sample_fmt,
                            v->adec->sample_rate, 0, NULL);
#else /* FFmpeg 4 (Ubuntu 22.04, the Linux release's base) */
        int64_t in = v->adec->channel_layout ? (int64_t)v->adec->channel_layout
                                             : av_get_default_channel_layout(v->adec->channels);
        v->swr = swr_alloc_set_opts(NULL, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_FLT, 48000, in, v->adec->sample_fmt,
                                    v->adec->sample_rate, 0, NULL);
#endif
        swr_init(v->swr);
    }
    v->gain = 1.0f;
    v->seek_to = -1;
    pthread_mutex_init(&v->lock, NULL);
    pthread_cond_init(&v->cond, NULL);
    pthread_mutex_lock(&videos_lock);
    v->next = videos;
    videos = v;
    pthread_mutex_unlock(&videos_lock);
    x2_log("video: %s: %dx%d %.1fs video=%s audio=%s", path, v->w, v->h, v->duration,
           v->vdec ? v->vdec->codec->name : "-", v->adec ? v->adec->codec->name : "-");
    return v;
}

void x2_video_close(X2Video *v) {
    if (!v) return;
    pthread_mutex_lock(&v->lock);
    v->quit = true;
    pthread_cond_broadcast(&v->cond);
    pthread_mutex_unlock(&v->lock);
    if (v->thread_started) {
        pthread_join(v->thread, NULL);
        if (v->vdec) pthread_join(v->vthread, NULL);
    }
    vq_clear(v);
    pthread_mutex_lock(&videos_lock);
    for (X2Video **p = &videos; *p; p = &(*p)->next)
        if (*p == v) { *p = v->next; break; }
    pthread_mutex_unlock(&videos_lock);
    sws_freeContext(v->sws);
    swr_free(&v->swr);
    avcodec_free_context(&v->vdec);
    avcodec_free_context(&v->adec);
    avformat_close_input(&v->fmt);
    free(v->frame);
    free(v);
}

void x2_video_info(X2Video *v, int *w, int *h, double *duration) {
    *w = v->w;
    *h = v->h;
    *duration = v->duration;
}

void x2_video_run(X2Video *v) {
    pthread_mutex_lock(&v->lock);
    if (!v->running) {
        v->clock_base_ns = x2_time_ns();
        v->running = true;
    }
    if (!v->thread_started) {
        v->thread_started = true;
        pthread_create(&v->thread, NULL, decode_thread, v);
        if (v->vdec) pthread_create(&v->vthread, NULL, video_thread, v);
    }
    pthread_cond_broadcast(&v->cond);
    pthread_mutex_unlock(&v->lock);
}

void x2_video_pause(X2Video *v) {
    pthread_mutex_lock(&v->lock);
    if (v->running) {
        v->clock_base_pos = now_clock(v);
        v->running = false;
    }
    pthread_mutex_unlock(&v->lock);
}

void x2_video_seek(X2Video *v, double t) {
    pthread_mutex_lock(&v->lock);
    v->seek_to = t;
    v->clock_base_pos = t;
    v->clock_base_ns = x2_time_ns();
    pthread_cond_broadcast(&v->cond);
    pthread_mutex_unlock(&v->lock);
}

double x2_video_position(X2Video *v) {
    pthread_mutex_lock(&v->lock);
    double p = now_clock(v);
    pthread_mutex_unlock(&v->lock);
    if (v->duration > 0 && p > v->duration) p = v->duration;
    return p;
}

bool x2_video_finished(X2Video *v) {
    pthread_mutex_lock(&v->lock);
    bool done = v->eof && v->ring_w == v->ring_r && (v->duration <= 0 || now_clock(v) >= v->duration - 0.05);
    pthread_mutex_unlock(&v->lock);
    return done;
}

bool x2_video_frame_yv12(X2Video *v, uint8_t *dst, size_t n) {
    size_t need = (size_t)v->w * v->h * 3 / 2;
    if (n < need) return false;
    pthread_mutex_lock(&v->lock);
    memcpy(dst, v->frame, need);
    pthread_mutex_unlock(&v->lock);
    return true;
}

void x2_video_set_volume(X2Video *v, float gain) { v->gain = gain; }

void x2_video_mix(float *out, uint32_t frames, uint32_t rate) {
    if (rate != 48000) return;
    pthread_mutex_lock(&videos_lock);
    for (X2Video *v = videos; v; v = v->next) {
        pthread_mutex_lock(&v->lock);
        if (v->running) {
            for (u32 i = 0; i < frames * 2 && v->ring_r != v->ring_w; i++) out[i] += v->ring[v->ring_r++ % RING] * v->gain;
            pthread_cond_broadcast(&v->cond); /* the reader may be waiting for ring space */
        }
        pthread_mutex_unlock(&v->lock);
    }
    pthread_mutex_unlock(&videos_lock);
}
