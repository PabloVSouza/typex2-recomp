/* Vulkan backend (1.3+): replays the frames gfx_record.c queues, as gfx_gl.c and
 * gfx_metal.m do. Selected with X2_RENDERER=vulkan (platform_sdl.c).
 *
 * - One command buffer per recorded frame (INFLIGHT in flight) with a host-visible
 *   ring for uniforms, streamed vertices and indices, and staged texture data.
 *   A ring that fills up mid-frame submits what is recorded and waits.
 * - Dynamic rendering; a rendering scope per render-target set. Clears are
 *   vkCmdClearAttachments over D3D's rectangle; StretchRect is vkCmdBlitImage.
 * - Pipelines are cached by shaders, vertex layout, topology, blend state and
 *   target formats; the rest (cull, depth, stencil, viewport) is dynamic state.
 * - Descriptors are pushed per draw (one push-descriptor set: sm3_glsl.h).
 * - Image layouts are tracked per texture; transitions happen outside rendering.
 * - Memory comes from 64 MB blocks per memory type (games create thousands of
 *   textures: more than the driver's allocation limit).
 * - Shaders: sm3_translate / hlsl_translate with SH_VKGLSL, compiled to SPIR-V
 *   by glslang on first use. Clip space is flipped to Vulkan's y-down in the
 *   shader, so render targets keep row 0 at the top as in D3D. */
#ifdef X2_VK_SDL /* the standalone app's window; the libretro core uses RetroArch's context */
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#endif
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "gfx_cmd.h"
#include "sm3_glsl.h"

/* Vulkan structs are initialised as {sType} by convention; the rest is zero. */
#pragma clang diagnostic ignored "-Wmissing-field-initializers"

#define MAX_IDS   (1u << 20)
#define INFLIGHT  3
#define RING_SIZE (48u << 20)
#define VK_CHECK(x)                                                                                                    \
    do {                                                                                                               \
        VkResult r_ = (x);                                                                                             \
        if (r_ != VK_SUCCESS) x2_fatal("vulkan: %s failed: %d", #x, (int)r_);                                          \
    } while (0)

/* Vulkan 1.3 / extension entry points, loaded at init */
#define VK_FUNCS(X)                                                                                                    \
    X(vkCmdBeginRendering)                                                                                             \
    X(vkCmdEndRendering)                                                                                               \
    X(vkCmdPipelineBarrier2)                                                                                           \
    X(vkCmdSetCullMode)                                                                                                \
    X(vkCmdSetFrontFace)                                                                                               \
    X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) X(vkCmdSetStencilTestEnable)      \
        X(vkCmdSetStencilOp) X(vkCmdPushDescriptorSetKHR)
#define X(f) static PFN_##f p##f;
VK_FUNCS(X)
#undef X

static VkInstance inst;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static u32 qfam;
static VkPhysicalDeviceMemoryProperties memprops;
static VkPhysicalDeviceLimits limits;
static VkCommandPool cpool;
static VkPipelineLayout playout;
static VkDescriptorSetLayout dslayout;
static VkFormat depth24; /* D24S8: D24_UNORM_S8_UINT or D32_SFLOAT_S8_UINT */
/* a frontend that shares its queue (RetroArch) locks it around submissions */
static void (*qlock)(void *), (*qunlock)(void *);
static void *qlock_handle;
static VkResult qsubmit(const VkSubmitInfo *si, VkFence f) {
    if (qlock) qlock(qlock_handle);
    VkResult r = vkQueueSubmit(queue, 1, si, f);
    if (qunlock) qunlock(qlock_handle);
    return r;
}

/* ---- memory: 64 MB blocks, first-fit free lists --------------------------------- */
typedef struct {
    VkDeviceSize off, size;
} Range;
typedef struct {
    VkDeviceMemory mem;
    u32 type;
    VkDeviceSize size;
    u8 *map;
    Range *free;
    u32 nfree, cap;
} Block;
static Block *blocks;
static u32 nblocks;
typedef struct {
    u32 block;
    VkDeviceSize off, size;
} Alloc;
#define BLOCK_SIZE (64ull << 20)

static u32 mem_type(u32 bits, VkMemoryPropertyFlags want) {
    for (u32 i = 0; i < memprops.memoryTypeCount; i++)
        if (bits & (1u << i) && (memprops.memoryTypes[i].propertyFlags & want) == want) return i;
    return ~0u;
}
static bool block_take(Block *b, VkDeviceSize size, VkDeviceSize align, VkDeviceSize *off) {
    for (u32 i = 0; i < b->nfree; i++) {
        Range *r = &b->free[i];
        VkDeviceSize o = (r->off + align - 1) / align * align;
        if (o + size > r->off + r->size) continue;
        VkDeviceSize end = r->off + r->size;
        if (o > r->off) { /* keep the gap before the aligned start */
            r->size = o - r->off;
            if (o + size < end) { /* and the rest after it */
                if (b->nfree == b->cap) b->free = realloc(b->free, (b->cap = b->cap * 2 + 8) * sizeof *b->free);
                memmove(&b->free[i + 2], &b->free[i + 1], (b->nfree - i - 1) * sizeof *b->free);
                b->free[i + 1] = (Range){o + size, end - o - size};
                b->nfree++;
            }
        } else if (o + size < end) {
            *r = (Range){o + size, end - o - size};
        } else {
            memmove(&b->free[i], &b->free[i + 1], (b->nfree - i - 1) * sizeof *b->free);
            b->nfree--;
        }
        *off = o;
        return true;
    }
    return false;
}
static Alloc mem_alloc(VkMemoryRequirements req, VkMemoryPropertyFlags want) {
    u32 type = mem_type(req.memoryTypeBits, want);
    if (type == ~0u) type = mem_type(req.memoryTypeBits, want & ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == ~0u) x2_fatal("vulkan: no memory type for %#x", want);
    /* buffers and optimal images may not share a page (bufferImageGranularity): align to it */
    VkDeviceSize align = req.alignment > limits.bufferImageGranularity ? req.alignment : limits.bufferImageGranularity;
    for (u32 i = 0; i < nblocks; i++) {
        VkDeviceSize off;
        if (blocks[i].type == type && block_take(&blocks[i], req.size, align, &off)) return (Alloc){i, off, req.size};
    }
    VkDeviceSize bs = req.size > BLOCK_SIZE ? req.size : BLOCK_SIZE;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, bs, type};
    blocks = realloc(blocks, (nblocks + 1) * sizeof *blocks);
    Block *b = &blocks[nblocks];
    memset(b, 0, sizeof *b);
    VK_CHECK(vkAllocateMemory(dev, &ai, NULL, &b->mem));
    b->type = type;
    b->size = bs;
    if (memprops.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        VK_CHECK(vkMapMemory(dev, b->mem, 0, bs, 0, (void **)&b->map));
    b->free = malloc(8 * sizeof *b->free);
    b->cap = 8;
    b->free[0] = (Range){0, bs};
    b->nfree = 1;
    VkDeviceSize off;
    block_take(b, req.size, align, &off);
    return (Alloc){nblocks++, off, req.size};
}
static void mem_free(Alloc a) {
    Block *b = &blocks[a.block];
    u32 i = 0;
    while (i < b->nfree && b->free[i].off < a.off) i++;
    if (b->nfree == b->cap) b->free = realloc(b->free, (b->cap = b->cap * 2 + 8) * sizeof *b->free);
    memmove(&b->free[i + 1], &b->free[i], (b->nfree - i) * sizeof *b->free);
    b->free[i] = (Range){a.off, a.size};
    b->nfree++;
    /* merge with the neighbours */
    if (i + 1 < b->nfree && b->free[i].off + b->free[i].size == b->free[i + 1].off) {
        b->free[i].size += b->free[i + 1].size;
        memmove(&b->free[i + 1], &b->free[i + 2], (b->nfree - i - 2) * sizeof *b->free);
        b->nfree--;
    }
    if (i > 0 && b->free[i - 1].off + b->free[i - 1].size == b->free[i].off) {
        b->free[i - 1].size += b->free[i].size;
        memmove(&b->free[i], &b->free[i + 1], (b->nfree - i - 1) * sizeof *b->free);
        b->nfree--;
    }
}

/* ---- frames, ring, deferred destruction --------------------------------------- */
typedef struct {
    VkCommandBuffer cb;
    VkFence fence;
    VkBuffer ring;
    Alloc ring_mem;
    u8 *ring_map;
    u64 serial; /* serial of the work last submitted with this slot */
} FrameSlot;
static FrameSlot slots[INFLIGHT];
static FrameSlot *fs; /* recording into */
static VkCommandBuffer cmd;
static u32 ring_pos;
static const u32 CONST_OFF = 0; /* (0,0,0,1): inputs the declaration lacks */
static void ring_start(void) {
    static const float one[4] = {0, 0, 0, 1};
    memcpy(fs->ring_map, one, 16);
    ring_pos = 16;
}
static u64 cur_serial, completed_serial;
/* X2_GPUTIME=1: GPU time per frame from timestamps at the start and end of each command buffer */
static VkQueryPool tsq;
static bool gputime;
/* X2_GPUTIME=2: GPU time of each rendering pass too, summed by render target (as gfx_gl.c) */
#define PASSQ 64
static VkQueryPool passq;
static GfxId pass_rt[INFLIGHT][PASSQ];
static u32 pass_n[INFLIGHT];
static bool pass_valid[INFLIGHT];
static bool gputime2;
static struct {
    GfxId rt;
    u32 w, h;
    double ms;
    u32 passes;
} pass_sum[64];
static u32 pass_frames;
static u32 pass_w[INFLIGHT][PASSQ], pass_h[INFLIGHT][PASSQ];
static bool pass_open;
static void pass_harvest(u32 slot) {
    if (!pass_valid[slot] || !pass_n[slot]) return;
    u64 ts[2 * PASSQ];
    if (vkGetQueryPoolResults(dev, passq, slot * 2 * PASSQ, 2 * pass_n[slot], sizeof ts, ts, 8,
                              VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    for (u32 p = 0; p < pass_n[slot]; p++) {
        GfxId rt = pass_rt[slot][p];
        int i = 0;
        while (i < 64 && pass_sum[i].passes && pass_sum[i].rt != rt) i++;
        if (i == 64) continue;
        pass_sum[i].rt = rt;
        pass_sum[i].w = pass_w[slot][p];
        pass_sum[i].h = pass_h[slot][p];
        pass_sum[i].ms += (double)(ts[2 * p + 1] - ts[2 * p]) * limits.timestampPeriod / 1e6;
        pass_sum[i].passes++;
    }
    if (++pass_frames < 120) return;
    char line[1024];
    int len = snprintf(line, sizeof line, "gpu passes (ms/frame):");
    for (int k = 0; k < 10; k++) { /* biggest first */
        int best = -1;
        for (int i = 0; i < 64; i++)
            if (pass_sum[i].passes && (best < 0 || pass_sum[i].ms > pass_sum[best].ms)) best = i;
        if (best < 0) break;
        len += snprintf(line + len, sizeof line - (size_t)len, " rt%u %ux%u %.2f (%u);", pass_sum[best].rt,
                        pass_sum[best].w, pass_sum[best].h, pass_sum[best].ms / 120, pass_sum[best].passes / 120);
        pass_sum[best].passes = 0;
    }
    x2_log("%s", line);
    memset(pass_sum, 0, sizeof pass_sum);
    pass_frames = 0;
}
static double gpu_ms_sum, gpu_ms_max;
static u32 gpu_n;

typedef struct {
    u64 serial;
    VkImage img;
    VkImageView v1, v2;
    VkBuffer buf;
    Alloc mem;
    bool has_mem;
} Garbage;
static Garbage *garbage;
static u32 ngarbage, garbage_cap;
static void trash(Garbage g) {
    g.serial = cur_serial;
    if (ngarbage == garbage_cap) garbage = realloc(garbage, (garbage_cap = garbage_cap * 2 + 64) * sizeof *garbage);
    garbage[ngarbage++] = g;
}
static void collect(void) {
    u32 k = 0;
    for (u32 i = 0; i < ngarbage; i++) {
        Garbage *g = &garbage[i];
        if (g->serial > completed_serial) {
            garbage[k++] = *g;
            continue;
        }
        if (g->v1) vkDestroyImageView(dev, g->v1, NULL);
        if (g->v2) vkDestroyImageView(dev, g->v2, NULL);
        if (g->img) vkDestroyImage(dev, g->img, NULL);
        if (g->buf) vkDestroyBuffer(dev, g->buf, NULL);
        if (g->has_mem) mem_free(g->mem);
    }
    ngarbage = k;
}

static void end_rendering(void);
static void cmd_begin(void) {
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                   VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    VK_CHECK(vkResetCommandBuffer(fs->cb, 0));
    VK_CHECK(vkBeginCommandBuffer(fs->cb, &bi));
    cmd = fs->cb;
}
static void frame_begin(void) {
    fs = &slots[(cur_serial + 1) % INFLIGHT];
    VK_CHECK(vkWaitForFences(dev, 1, &fs->fence, VK_TRUE, UINT64_MAX));
    if (fs->serial > completed_serial) completed_serial = fs->serial;
    if (gputime && fs->serial) {
        u64 ts[2];
        u32 q = (u32)(fs - slots) * 2;
        if (vkGetQueryPoolResults(dev, tsq, q, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            double ms = (double)(ts[1] - ts[0]) * limits.timestampPeriod / 1e6;
            gpu_ms_sum += ms;
            if (ms > gpu_ms_max) gpu_ms_max = ms;
            gpu_n++;
        }
    }
    if (gputime2) pass_harvest((u32)(fs - slots));
    VK_CHECK(vkResetFences(dev, 1, &fs->fence));
    cur_serial++;
    ring_start();
    collect();
    cmd_begin();
    if (gputime) {
        u32 q = (u32)(fs - slots) * 2;
        vkCmdResetQueryPool(cmd, tsq, q, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, tsq, q);
    }
    if (gputime2) {
        u32 sl = (u32)(fs - slots);
        vkCmdResetQueryPool(cmd, passq, sl * 2 * PASSQ, 2 * PASSQ);
        pass_n[sl] = 0;
        pass_valid[sl] = true;
    }
}
static void submit(void) {
    end_rendering();
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VK_CHECK(qsubmit(&si, fs->fence));
    fs->serial = cur_serial;
    cmd = VK_NULL_HANDLE;
}
static void frame_end(void) {
    if (gputime) {
        end_rendering();
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, tsq, (u32)(fs - slots) * 2 + 1);
    }
    submit();
}

/* a ring that is full: submit, wait, and continue the frame in an empty ring */
static void ring_flush(void) {
    submit();
    pass_valid[fs - slots] = false; /* its queries cannot be written again without a reset */
    VK_CHECK(vkWaitForFences(dev, 1, &fs->fence, VK_TRUE, UINT64_MAX));
    completed_serial = cur_serial;
    VK_CHECK(vkResetFences(dev, 1, &fs->fence));
    cur_serial++;
    ring_start();
    collect();
    cmd_begin();
}
/* room for `len` bytes now, so that the allocations of one draw never flush midway */
static void ring_reserve(u64 len) {
    if ((u64)ring_pos + len > RING_SIZE && len <= RING_SIZE) ring_flush();
}
static u32 ring_alloc(u32 len, u32 align) {
    if (len > RING_SIZE) {
        x2_log("vulkan: %u bytes do not fit the frame ring", len);
        return ~0u;
    }
    u32 pos = (ring_pos + align - 1) / align * align;
    if (pos + len > RING_SIZE) {
        ring_flush();
        pos = (ring_pos + align - 1) / align * align;
    }
    ring_pos = pos + len;
    return pos;
}
static void *ring_ptr(u32 off) { return fs->ring_map + off; }
static u32 ring_put(const void *data, u32 len, u32 align) {
    u32 off = ring_alloc(len, align);
    if (off != ~0u) memcpy(ring_ptr(off), data, len);
    return off;
}

/* ---- textures --------------------------------------------------------------------- */
typedef struct {
    VkImage img;
    VkImageView view; /* sampling (with D3D's swizzle) */
    VkImageView att;  /* rendering (identity), render targets only */
    Alloc mem;
    VkFormat vf;
    VkImageLayout layout;
    u32 fmt, w, h, levels, scale;
    bool rt, depth, cube, vol, dxt, expand16;
    bool rgb565;     /* opaque DXT1, stored as RGB565: half the size and bandwidth (as gfx_gl.c) */
    bool swap_rb;    /* a BGRA render target stored as RGBA8: uploads swap red and blue */
    bool uploaded;   /* any upload yet: a DXT1 texture picks its storage at the first */
    GfxId id;        /* the game's id (X2_GPUTIME=2 names passes by target) */
    bool vs_sampled; /* read by a vertex shader once: its barriers include the vertex stage */
    u64 used;
} VTex;
static VTex *texs[MAX_IDS];
static VTex fallback_bb;
static VTex *black[3]; /* 2D, cube, 3D: samplers without a texture */

static bool is_dxt(u32 f) { return f == GFX_FMT_DXT1 || f == GFX_FMT_DXT3 || f == GFX_FMT_DXT5; }
static bool is16(u32 f) { return f == GFX_FMT_B5G6R5 || f == GFX_FMT_BGR5A1 || f == GFX_FMT_BGRA4; }
static VkFormat vk_format(u32 f) {
    switch (f) {
    case GFX_FMT_BGRA8:
    case GFX_FMT_BGRX8:
    case GFX_FMT_B5G6R5:
    case GFX_FMT_BGR5A1:
    case GFX_FMT_BGRA4: return VK_FORMAT_B8G8R8A8_UNORM;
    case GFX_FMT_A8:
    case GFX_FMT_L8: return VK_FORMAT_R8_UNORM;
    case GFX_FMT_A8L8: return VK_FORMAT_R8G8_UNORM;
    case GFX_FMT_DXT1:
    case GFX_FMT_DXT3:
    case GFX_FMT_DXT5: return VK_FORMAT_R8G8B8A8_UNORM; /* decoded on the CPU */
    case GFX_FMT_R16F: return VK_FORMAT_R16_SFLOAT;
    case GFX_FMT_RG16F: return VK_FORMAT_R16G16_SFLOAT;
    case GFX_FMT_RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case GFX_FMT_R32F: return VK_FORMAT_R32_SFLOAT;
    case GFX_FMT_RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case GFX_FMT_D24S8: return depth24;
    case GFX_FMT_D16: return VK_FORMAT_D16_UNORM;
    default: return VK_FORMAT_B8G8R8A8_UNORM;
    }
}
static VkImageAspectFlags aspect_of(const VTex *t) {
    if (!t->depth) return VK_IMAGE_ASPECT_COLOR_BIT;
    return t->vf == VK_FORMAT_D16_UNORM ? VK_IMAGE_ASPECT_DEPTH_BIT
                                        : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
}

/* the stages and accesses that use an image in a layout: barriers wait for exactly
 * those (an all-commands barrier keeps a tile GPU from overlapping passes) */
static void layout_use(VkImageLayout l, VkPipelineStageFlags2 *stage, VkAccessFlags2 *access) {
    switch (l) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        *access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        return;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        *access = VK_ACCESS_2_TRANSFER_READ_BIT;
        return;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
        *access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        return;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        *access = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        return;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        *access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        return;
    default: /* UNDEFINED: nothing to wait for */
        *stage = VK_PIPELINE_STAGE_2_NONE;
        *access = VK_ACCESS_2_NONE;
        return;
    }
}
static VkImageMemoryBarrier2 layout_barrier(VTex *t, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    layout_use(t->layout, &b.srcStageMask, &b.srcAccessMask);
    layout_use(to, &b.dstStageMask, &b.dstAccessMask);
    if (t->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || t->layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
        b.srcAccessMask = VK_ACCESS_2_NONE; /* reads: an execution dependency is enough */
    /* textures are read by pixel shaders: a pass sampling one need not hold back its vertex
     * work (a tile GPU overlaps it with the previous pass's pixels) */
    if (!t->vs_sampled) {
        if (t->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            b.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        if (to == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    }
    b.oldLayout = t->layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t->img;
    b.subresourceRange =
        (VkImageSubresourceRange){aspect_of(t), 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    return b;
}
/* layout transition, recorded outside rendering */
static void transition(VTex *t, VkImageLayout to) {
    if (t->layout == to) return;
    end_rendering();
    VkImageMemoryBarrier2 b = layout_barrier(t, to);
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    pvkCmdPipelineBarrier2(cmd, &di);
    t->layout = to;
}

static VkImageView make_view(VTex *t, VkComponentMapping sw, VkImageAspectFlags aspect) {
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t->img;
    vi.viewType = t->cube ? VK_IMAGE_VIEW_TYPE_CUBE : t->vol ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = t->vf;
    vi.components = sw;
    vi.subresourceRange = (VkImageSubresourceRange){aspect, 0, t->levels, 0, t->cube ? 6u : 1u};
    VkImageView v;
    VK_CHECK(vkCreateImageView(dev, &vi, NULL, &v));
    return v;
}

static VTex *tex_new(u32 fmt, u32 w, u32 h, u32 levels, bool rt, bool cube, u32 scale, bool vol, bool rgb565) {
    VTex *t = calloc(1, sizeof *t);
    t->scale = scale ? scale : 1;
    t->fmt = fmt;
    t->w = w * t->scale;
    t->h = h * t->scale;
    t->levels = levels ? levels : 1;
    t->rt = rt;
    t->depth = fmt == GFX_FMT_D24S8 || fmt == GFX_FMT_D16;
    t->cube = cube;
    t->vol = vol;
    t->dxt = is_dxt(fmt);
    t->expand16 = is16(fmt);
    t->rgb565 = rgb565;
    t->vf = rgb565 ? VK_FORMAT_R5G6B5_UNORM_PACK16 : vk_format(fmt);
    /* render targets as RGBA8, as GL stores them: BGRA attachments cost tile GPUs (Mali) */
    if (rt && t->vf == VK_FORMAT_B8G8R8A8_UNORM && !getenv("X2_VK_BGRA_RT")) {
        t->vf = VK_FORMAT_R8G8B8A8_UNORM;
        t->swap_rb = true;
    }
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ci.imageType = vol ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = t->vf;
    ci.extent = (VkExtent3D){t->w, t->h, 1};
    ci.mipLevels = t->levels;
    ci.arrayLayers = cube ? 6 : 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        (rt ? (t->depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) : 0);
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(dev, &ci, NULL, &t->img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, t->img, &req);
    t->mem = mem_alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkBindImageMemory(dev, t->img, blocks[t->mem.block].mem, t->mem.off));
    t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    /* D3D samples A8 as (0,0,0,a), L8 as (l,l,l,1), A8L8 as (l,l,l,a), X8R8G8B8 with alpha 1 */
    VkComponentMapping sw = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                             VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    if (!rt) {
        if (fmt == GFX_FMT_A8)
            sw = (VkComponentMapping){VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO,
                                      VK_COMPONENT_SWIZZLE_R};
        else if (fmt == GFX_FMT_L8)
            sw = (VkComponentMapping){VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
                                      VK_COMPONENT_SWIZZLE_ONE};
        else if (fmt == GFX_FMT_A8L8)
            sw = (VkComponentMapping){VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
                                      VK_COMPONENT_SWIZZLE_G};
        else if (fmt == GFX_FMT_BGRX8)
            sw.a = VK_COMPONENT_SWIZZLE_ONE;
    }
    t->view = make_view(t, sw, aspect_of(t) & ~VK_IMAGE_ASPECT_STENCIL_BIT);
    if (rt) {
        VkComponentMapping id = {0};
        t->att = make_view(t, id, aspect_of(t)); /* attachments: depth and stencil */
    }
    return t;
}
static void tex_free(VTex *t) {
    trash((Garbage){0, t->img, t->view, t->att, VK_NULL_HANDLE, t->mem, true});
    free(t);
}
static VTex *tex_get(GfxId id) { return id && id < MAX_IDS ? texs[id] : NULL; }

static void do_tex_create(const TexCreate *k) {
    if (!k->id || k->id >= MAX_IDS) return;
    if (texs[k->id]) tex_free(texs[k->id]);
    texs[k->id] = tex_new(k->fmt, k->w, k->h, k->levels, k->rt != 0, k->cube != 0, k->scale, false, false);
    texs[k->id]->id = k->id;
    static int ul = -1;
    if (ul < 0) ul = getenv("X2_UPLOADLOG") != NULL;
    if (ul)
        x2_log("vulkan: texture %u fmt %u %ux%u levels %u%s", k->id, k->fmt, texs[k->id]->w, texs[k->id]->h,
               texs[k->id]->levels, k->rt ? " rt" : "");
}
static void end_if_bound(VTex *t);
static void do_tex_destroy(GfxId id) {
    VTex *t = tex_get(id);
    if (!t) return;
    end_if_bound(t);
    tex_free(t);
    texs[id] = NULL;
}

/* uploads go through the ring and a copy in command order (D3D orders an upload
 * between the draws around it) */
static void upload(VTex *t, u32 level, u32 face, u32 x, u32 y, u32 w, u32 h, const u8 *data, u32 pitch) {
    u32 bw, bpp = gfx_fmt_block_bytes(t->fmt, &bw);
    u8 *tmp = NULL;
    if (t->dxt) {
        data = tmp = gfx_dxt_decode(t->fmt, data, w, h);
        pitch = w * 4;
        if (t->rgb565) {
            gfx_rgba_to_565(tmp, w * h);
            pitch = w * 2;
        }
    } else if (t->expand16) {
        tmp = malloc((size_t)w * h * 4);
        gfx_expand16(t->fmt, data, pitch, w, h, tmp);
        data = tmp;
        pitch = w * 4;
    } else if (bpp != 4 || t->fmt == GFX_FMT_RG16F) {
        (void)0; /* rows of bpp bytes */
    }
    u32 px = t->rgb565 ? 2 : t->dxt || t->expand16 ? 4 : bpp;
    u32 row = w * px;
    u32 off = ring_alloc(row * h, 16 > px ? 16 : px);
    if (off == ~0u) {
        free(tmp);
        return;
    }
    if (t->swap_rb && px == 4) { /* BGRA data, RGBA storage (KOF MI's movie overlay); the ring is
                                    uncached: write it only, never read it back */
        for (u32 r = 0; r < h; r++) {
            const u8 *s = data + (size_t)r * pitch; /* guest rows need not be 4-byte aligned */
            u32 *d = (u32 *)((u8 *)ring_ptr(off) + (size_t)r * row);
            for (u32 i = 0; i < w; i++) {
                u32 v;
                memcpy(&v, s + i * 4, 4);
                d[i] = (v & 0xff00ff00u) | (v >> 16 & 0xffu) | (v & 0xffu) << 16;
            }
        }
    } else {
        for (u32 r = 0; r < h; r++) memcpy((u8 *)ring_ptr(off) + (size_t)r * row, data + (size_t)r * pitch, row);
    }
    free(tmp);
    end_rendering(); /* copies happen outside rendering */
    transition(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c = {0};
    c.bufferOffset = off;
    c.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, level, face, 1};
    c.imageOffset = (VkOffset3D){(s32)x, (s32)y, 0};
    c.imageExtent = (VkExtent3D){w, h, 1};
    vkCmdCopyBufferToImage(cmd, fs->ring, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    t->used = cur_serial;
}
static void do_tex_upload(const TexUpload *k) {
    VTex *t = tex_get(k->id);
    if (!t) return;
    {
        static int ul = -1;
        if (ul < 0) ul = getenv("X2_UPLOADLOG") != NULL;
        if (ul)
            x2_log("vulkan: upload tex %u fmt %u level %u %ux%u at %u,%u pitch %u (%u bytes)", k->id, t->fmt, k->level,
                   k->w, k->h, k->x, k->y, k->pitch, k->bytes);
    }
    if (t->fmt == GFX_FMT_DXT1 && !t->uploaded && !t->cube) {
        static int on = -1;
        if (on < 0) on = !getenv("X2_DXT565") || atoi(getenv("X2_DXT565")) != 0;
        if (on && !gfx_dxt1_has_alpha((const u8 *)(k + 1), k->bytes)) { /* never used yet: new storage */
            VTex *n = tex_new(t->fmt, t->w / t->scale, t->h / t->scale, t->levels, false, false, t->scale, false, true);
            tex_free(t);
            texs[k->id] = t = n;
        }
    }
    t->uploaded = true;
    u32 level = t->cube ? (k->level & 15) : k->level, face = t->cube ? k->level >> 4 : 0;
    if (level >= t->levels || face >= 6) return;
    /* DXT rectangles cover whole blocks: decode at the block-rounded size, copy what fits */
    u32 w = k->w, h = k->h;
    if (t->dxt) {
        u32 lw = t->w >> level ? t->w >> level : 1, lh = t->h >> level ? t->h >> level : 1;
        if (k->x + w > lw) w = lw - k->x;
        if (k->y + h > lh) h = lh - k->y;
        if (w != k->w || h != k->h) { /* partial blocks at the edge: decode all, copy the visible part */
            u8 *full = gfx_dxt_decode(t->fmt, (const u8 *)(k + 1), k->w, k->h);
            u32 px = 4;
            if (t->rgb565) {
                gfx_rgba_to_565(full, k->w * k->h);
                px = 2;
            }
            u32 off = ring_alloc(w * h * px, 16);
            if (off != ~0u) {
                for (u32 r = 0; r < h; r++)
                    memcpy((u8 *)ring_ptr(off) + (size_t)r * w * px, full + (size_t)r * k->w * px, w * px);
                end_rendering();
                transition(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkBufferImageCopy c = {0};
                c.bufferOffset = off;
                c.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, level, face, 1};
                c.imageOffset = (VkOffset3D){(s32)k->x, (s32)k->y, 0};
                c.imageExtent = (VkExtent3D){w, h, 1};
                vkCmdCopyBufferToImage(cmd, fs->ring, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            }
            free(full);
            return;
        }
    }
    upload(t, level, face, k->x, k->y, w, h, (const u8 *)(k + 1), k->pitch);
}

/* ---- buffers ------------------------------------------------------------------- */
typedef struct {
    VkBuffer buf;
    Alloc mem;
    u8 *map;
    u32 size, uploads;
    bool dynamic;
    u8 *shadow;
    u64 used;
} VBuf;
static VBuf *bufs[MAX_IDS];
static bool gpu_idle_for(u64 used) { return used < cur_serial && used <= completed_serial; }

static void do_buf_create(const BufCreate *k) {
    if (k->id >= MAX_IDS) return;
    VBuf *b = calloc(1, sizeof *b);
    b->size = k->size;
    b->shadow = calloc(1, k->size ? k->size : 1);
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = k->size ? k->size : 4;
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    VK_CHECK(vkCreateBuffer(dev, &bi, NULL, &b->buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, b->buf, &req);
    b->mem = mem_alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkBindBufferMemory(dev, b->buf, blocks[b->mem.block].mem, b->mem.off));
    b->map = blocks[b->mem.block].map + b->mem.off;
    if (bufs[k->id]) {
        trash((Garbage){0, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, bufs[k->id]->buf, bufs[k->id]->mem, true});
        free(bufs[k->id]->shadow);
        free(bufs[k->id]);
    }
    bufs[k->id] = b;
}
static void do_buf_upload(const BufUpload *k) {
    VBuf *b = k->id < MAX_IDS ? bufs[k->id] : NULL;
    if (!b || k->offset + k->size > b->size) return;
    memcpy(b->shadow + k->offset, k + 1, k->size);
    /* as gfx_gl.c: after its first rewrite a buffer is streamed from the shadow */
    if (++b->uploads >= 2 || !gpu_idle_for(b->used)) b->dynamic = true;
    if (!b->dynamic) memcpy(b->map + k->offset, k + 1, k->size);
}
static void do_buf_destroy(GfxId id) {
    VBuf *b = id < MAX_IDS ? bufs[id] : NULL;
    if (!b) return;
    trash((Garbage){0, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, b->buf, b->mem, true});
    free(b->shadow);
    free(b);
    bufs[id] = NULL;
}

/* ---- shaders --------------------------------------------------------------------- */
typedef struct {
    u32 key;
    VkShaderModule m;
} Variant;
typedef struct {
    Sm3Info info;
    char *src;
    bool pixel, ok;
    u32 serial;
    Variant *var;
    u32 nvar;
} VShader;
static VShader *shaders[MAX_IDS];
static u32 shader_serial;

static void do_shader_create(GfxId id, bool pixel, const u32 *tok, u32 n) {
    if (id >= MAX_IDS) return;
    VShader *s = calloc(1, sizeof *s);
    s->serial = ++shader_serial;
    s->pixel = pixel;
    char err[256] = "";
    bool ok;
    if (n > 3 && (tok[1] & 0xFFFF) == 0xFFFE && tok[2] == 0x4C484258u) /* x2 HLSL blob */
        ok = hlsl_translate((const char *)&tok[11], (const char *)&tok[3], pixel, SH_VKGLSL, &s->src, &s->info, err,
                            sizeof err);
    else
        ok = sm3_translate(tok, n, SH_VKGLSL, &s->src, &s->info, err, sizeof err);
    if (!ok) x2_log("vulkan: shader %u translation incomplete: %s", id, err);
    if (s->src && getenv("X2_SHADERLOG") && (u32)atoi(getenv("X2_SHADERLOG")) == id)
        x2_log("vulkan: shader %u:\n%s", id, s->src);
    s->ok = s->src != NULL;
    shaders[id] = s;
}
static void do_shader_destroy(GfxId id) {
    VShader *s = id < MAX_IDS ? shaders[id] : NULL;
    if (!s) return;
    /* pipelines made from its modules stay valid; modules are small: keep them */
    free(s->src);
    free(s);
    shaders[id] = NULL;
}
/* GLSL -> SPIR-V with defines inserted after #version */
/* compiled shaders and pipelines are kept in the GL backend's cache folder (gfx_gl_set_cache_dir):
 * glslang on a phone costs milliseconds per shader, and a stall during a movie made its
 * decoder drop frames */
extern char gfx_gl_cache_dir[512];
static u64 fnv64(const void *p, size_t n) {
    u64 h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ ((const u8 *)p)[i]) * 0x100000001b3ull;
    return h;
}
static bool write_file(const char *path, const void *data, size_t n) {
    char tmp[700];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    ok = fclose(f) == 0 && ok;
    if (ok)
        rename(tmp, path);
    else
        remove(tmp);
    return ok;
}
static void *read_file(const char *path, size_t *n) {
    *n = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *d = len > 0 ? malloc((size_t)len) : NULL;
    if (d && fread(d, 1, (size_t)len, f) != (size_t)len) {
        free(d);
        d = NULL;
    }
    fclose(f);
    *n = d ? (size_t)len : 0;
    return d;
}

static VkShaderModule compile(VShader *s, const char *defines) {
    size_t n = strlen(s->src), dl = strlen(defines);
    const char *nl = strchr(s->src, '\n');
    size_t at = nl ? (size_t)(nl - s->src) + 1 : 0;
    char *src = malloc(n + dl + 1);
    memcpy(src, s->src, at);
    memcpy(src + at, defines, dl);
    memcpy(src + at + dl, s->src + at, n - at + 1);
    char spv_path[700] = "";
    if (gfx_gl_cache_dir[0]) {
        snprintf(spv_path, sizeof spv_path, "%s/vk-%016llx-%c.spv", gfx_gl_cache_dir,
                 (unsigned long long)fnv64(src, strlen(src)), s->pixel ? 'p' : 'v');
        size_t sz;
        void *code = read_file(spv_path, &sz);
        if (code && sz >= 20 && sz % 4 == 0) {
            VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            mi.codeSize = sz;
            mi.pCode = code;
            VkShaderModule m = VK_NULL_HANDLE;
            VkResult r = vkCreateShaderModule(dev, &mi, NULL, &m);
            free(code);
            if (r == VK_SUCCESS) {
                free(src);
                return m;
            }
        }
        free(code);
    }
    glslang_stage_t stage = s->pixel ? GLSLANG_STAGE_FRAGMENT : GLSLANG_STAGE_VERTEX;
    glslang_input_t in = {0};
    in.language = GLSLANG_SOURCE_GLSL;
    in.stage = stage;
    in.client = GLSLANG_CLIENT_VULKAN;
    in.client_version = GLSLANG_TARGET_VULKAN_1_1;
    in.target_language = GLSLANG_TARGET_SPV;
    in.target_language_version = GLSLANG_TARGET_SPV_1_3;
    in.code = src;
    in.default_version = 450;
    in.default_profile = GLSLANG_NO_PROFILE;
    in.messages = GLSLANG_MSG_DEFAULT_BIT;
    in.resource = glslang_default_resource();
    glslang_shader_t *sh = glslang_shader_create(&in);
    VkShaderModule m = VK_NULL_HANDLE;
    if (!glslang_shader_preprocess(sh, &in) || !glslang_shader_parse(sh, &in)) {
        x2_log("vulkan: shader failed to compile: %s\n--- source ---\n%s", glslang_shader_get_info_log(sh), src);
    } else {
        glslang_program_t *p = glslang_program_create();
        glslang_program_add_shader(p, sh);
        if (!glslang_program_link(p, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT)) {
            x2_log("vulkan: shader failed to link: %s", glslang_program_get_info_log(p));
        } else {
            glslang_program_SPIRV_generate(p, stage);
            VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            mi.codeSize = glslang_program_SPIRV_get_size(p) * 4;
            mi.pCode = glslang_program_SPIRV_get_ptr(p);
            VK_CHECK(vkCreateShaderModule(dev, &mi, NULL, &m));
            if (spv_path[0]) write_file(spv_path, mi.pCode, mi.codeSize);
        }
        glslang_program_delete(p);
    }
    glslang_shader_delete(sh);
    free(src);
    if (!m) s->ok = false;
    return m;
}
/* key: pixel: alpha test; vertex: 2 bits per input register (1 int, 2 uint vertex data) */
static VkShaderModule module_for(VShader *s, u32 key) {
    for (u32 i = 0; i < s->nvar; i++)
        if (s->var[i].key == key) return s->var[i].m;
    char defs[512] = "";
    size_t l = 0;
    if (s->pixel && key) l += (size_t)snprintf(defs + l, sizeof defs - l, "#define X2_ATEST 1\n");
    /* fp16 pixel maths as on GLES (gfx_gl_draw.c); X2_PS_MEDIUMP=0 turns it off */
    static int mp = -1;
    if (mp < 0) mp = !getenv("X2_PS_MEDIUMP") || atoi(getenv("X2_PS_MEDIUMP")) != 0;
    if (s->pixel && mp) l += (size_t)snprintf(defs + l, sizeof defs - l, "#define X2_MEDIUMP 1\n");
    for (int r = 0; !s->pixel && r < 16; r++) {
        u32 t = key >> (2 * r) & 3;
        if (t) l += (size_t)snprintf(defs + l, sizeof defs - l, "#define X2_VT%d %s\n", r, t == 1 ? "ivec4" : "uvec4");
    }
    VkShaderModule m = compile(s, defs);
    s->var = realloc(s->var, (s->nvar + 1) * sizeof *s->var);
    s->var[s->nvar++] = (Variant){key, m};
    return m;
}

/* ---- vertex declarations ---------------------------------------------------------- */
typedef struct {
    u16 stream, offset;
    u8 type, usage, index;
} Elem;
typedef struct {
    Elem e[64];
    u32 n;
} Decl;
static Decl *decls[MAX_IDS];
static void do_decl_create(GfxId id, const u8 *el, u32 count) {
    if (id >= MAX_IDS) return;
    Decl *d = calloc(1, sizeof *d);
    for (u32 i = 0; i < count && i < 64; i++) {
        Elem *e = &d->e[d->n];
        memcpy(&e->stream, el + 8 * i, 2);
        memcpy(&e->offset, el + 8 * i + 2, 2);
        e->type = el[8 * i + 4];
        e->usage = el[8 * i + 6];
        e->index = el[8 * i + 7];
        if (e->stream == 0xFF) break;
        d->n++;
    }
    free(decls[id]);
    decls[id] = d;
}
static VkFormat vertex_format(u8 type, u32 *vt, bool *bgra) {
    *vt = 0;
    *bgra = false;
    switch (type) {
    case 0: return VK_FORMAT_R32_SFLOAT;
    case 1: return VK_FORMAT_R32G32_SFLOAT;
    case 2: return VK_FORMAT_R32G32B32_SFLOAT;
    case 3: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 4: *bgra = true; return VK_FORMAT_R8G8B8A8_UNORM;
    case 5: *vt = 2; return VK_FORMAT_R8G8B8A8_UINT;
    case 6: *vt = 1; return VK_FORMAT_R16G16_SINT;
    case 7: *vt = 1; return VK_FORMAT_R16G16B16A16_SINT;
    case 8: return VK_FORMAT_R8G8B8A8_UNORM;
    case 9: return VK_FORMAT_R16G16_SNORM;
    case 10: return VK_FORMAT_R16G16B16A16_SNORM;
    case 11: return VK_FORMAT_R16G16_UNORM;
    case 12: return VK_FORMAT_R16G16B16A16_UNORM;
    case 15: return VK_FORMAT_R16G16_SFLOAT;
    case 16: return VK_FORMAT_R16G16B16A16_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

/* ---- constants and uniform blocks (std140, sm3_glsl.h) ---------------------------- */
static float vsc[GFX_VS_CONSTS][4], psc[GFX_PS_CONSTS][4];
static s32 vsi[16][4], psi[16][4], vsb[16], psb[16];
typedef struct {
    float c[256][4];
    s32 i[16][4];
    s32 b[16][4];
    float vp_fix[4];
    s32 bgra_mask, pad[3];
} VsU;
typedef struct {
    float c[224][4];
    s32 i[16][4];
    s32 b[16][4];
    s32 alpha_func;
    float alpha_ref;
    s32 pad[2];
} PsU;

/* ---- pipelines ------------------------------------------------------------------- */
typedef struct {
    u32 vs, ps, atest, vtkey, topo;
    u8 afmt[16], abuf[16];
    u16 aoff[16];
    u32 stride[10];
    u32 cfmt[4], dfmt, ncolor;
    u8 blend, src, dst, op, srca, dsta, opa, wmask;
} PipeKey;
typedef struct PipeEnt {
    PipeKey k;
    VkPipeline p;
    struct PipeEnt *next;
} PipeEnt;
#define PIPE_BUCKETS 4096
static PipeEnt *pipes[PIPE_BUCKETS];
static VkPipelineCache pcache;

static u32 fnv(const void *p, size_t n) {
    u32 h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ ((const u8 *)p)[i]) * 16777619u;
    return h;
}
static VkBlendFactor blend_factor(u32 f) {
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 14: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
static VkBlendOp blend_op(u32 o) {
    switch (o) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}
/* the pipeline cache, saved every 300 frames while new pipelines appear */
static bool pcache_dirty;
static void pcache_save(void) {
    pcache_dirty = false;
    size_t sz = 0;
    if (!gfx_gl_cache_dir[0] || vkGetPipelineCacheData(dev, pcache, &sz, NULL) != VK_SUCCESS || !sz) return;
    void *d = malloc(sz);
    if (vkGetPipelineCacheData(dev, pcache, &sz, d) == VK_SUCCESS) {
        char path[700];
        snprintf(path, sizeof path, "%s/vk-pipelines.bin", gfx_gl_cache_dir);
        write_file(path, d, sz);
    }
    free(d);
}
static VkPipeline get_pipeline(const PipeKey *k, VShader *vs, VShader *ps) {
    u32 h = fnv(k, sizeof *k) % PIPE_BUCKETS;
    for (PipeEnt *e = pipes[h]; e; e = e->next)
        if (!memcmp(&e->k, k, sizeof *k)) return e->p;
    PipeEnt *e = calloc(1, sizeof *e);
    e->k = *k;
    e->next = pipes[h];
    pipes[h] = e;
    VkShaderModule vm = module_for(vs, k->vtkey), fm = module_for(ps, k->atest);
    if (!vm || !fm) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vm, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fm, "main", NULL}};
    VkVertexInputBindingDescription vb[10];
    VkVertexInputAttributeDescription va[16];
    u32 nvb = 0, nva = 0;
    for (u32 b = 1; b < 10; b++)
        if (k->stride[b] || b == 9) {
            bool used = false;
            for (int a = 0; a < 16; a++) used |= k->afmt[a] && k->abuf[a] == b;
            if (used)
                vb[nvb++] =
                    (VkVertexInputBindingDescription){b, b == 9 ? 0 : k->stride[b], VK_VERTEX_INPUT_RATE_VERTEX};
        }
    for (u32 a = 0; a < 16; a++)
        if (k->afmt[a]) {
            VkFormat f = k->abuf[a] == 9 ? VK_FORMAT_R32G32B32A32_SFLOAT : (VkFormat)0;
            if (!f) { /* stored compactly: index into the format table */
                u32 vt;
                bool bgra;
                f = vertex_format(k->afmt[a] - 1, &vt, &bgra);
            }
            va[nva++] = (VkVertexInputAttributeDescription){a, k->abuf[a], f, k->aoff[a]};
        }
    VkPipelineVertexInputStateCreateInfo vi = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, nvb, vb, nva, va};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
                                                 (VkPrimitiveTopology)k->topo, VK_FALSE};
    VkPipelineViewportStateCreateInfo vp = {
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState ba[4];
    for (u32 i = 0; i < 4; i++) {
        ba[i] = (VkPipelineColorBlendAttachmentState){k->blend,
                                                      blend_factor(k->src),
                                                      blend_factor(k->dst),
                                                      blend_op(k->op),
                                                      blend_factor(k->srca),
                                                      blend_factor(k->dsta),
                                                      blend_op(k->opa),
                                                      k->wmask};
    }
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = k->ncolor;
    cb.pAttachments = ba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT,
                            VK_DYNAMIC_STATE_SCISSOR,
                            VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                            VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                            VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                            VK_DYNAMIC_STATE_CULL_MODE,
                            VK_DYNAMIC_STATE_FRONT_FACE,
                            VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
                            VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                            VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                            VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
                            VK_DYNAMIC_STATE_STENCIL_OP};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0,
                                           sizeof dyn / sizeof *dyn, dyn};
    VkFormat cf[4] = {(VkFormat)k->cfmt[0], (VkFormat)k->cfmt[1], (VkFormat)k->cfmt[2], (VkFormat)k->cfmt[3]};
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = k->ncolor;
    ri.pColorAttachmentFormats = cf;
    ri.depthAttachmentFormat = (VkFormat)k->dfmt;
    ri.stencilAttachmentFormat = k->dfmt && k->dfmt != VK_FORMAT_D16_UNORM ? (VkFormat)k->dfmt : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ri;
    gi.stageCount = 2;
    gi.pStages = st;
    gi.pVertexInputState = &vi;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &dy;
    gi.layout = playout;
    if (vkCreateGraphicsPipelines(dev, pcache, 1, &gi, NULL, &e->p) != VK_SUCCESS) {
        x2_log("vulkan: pipeline creation failed");
        e->p = VK_NULL_HANDLE;
    }
    pcache_dirty = true;
    return e->p;
}

typedef struct SampEnt {
    u32 key, border;
    VkSampler s;
    struct SampEnt *next;
} SampEnt;
static SampEnt *samps;
static VkSamplerAddressMode address(u32 a) {
    switch (a) {
    case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 3:
    case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}
static VkSampler get_sampler(const u32 *ss) {
    u32 mag = ss[5], min = ss[6], mip = ss[7];
    u32 key = (ss[1] & 7) | (ss[2] & 7) << 3 | (ss[3] & 7) << 6 | (mag & 3) << 9 | (min & 3) << 11 | (mip & 3) << 13;
    u32 border = ss[4];
    for (SampEnt *e = samps; e; e = e->next)
        if (e->key == key && e->border == border) return e->s;
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = mag >= 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.minFilter = min >= 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = mip >= 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = address(ss[1]);
    si.addressModeV = address(ss[2]);
    si.addressModeW = address(ss[3]);
    si.maxLod = mip == 0 ? 0.0f : VK_LOD_CLAMP_NONE; /* no mip filter: level 0 only */
    /* three border colours: the nearest one */
    si.borderColor = border >> 24 < 128               ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
                     : (border & 0xFFFFFF) > 0x7F7F7F ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                                      : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    SampEnt *e = calloc(1, sizeof *e);
    e->key = key;
    e->border = border;
    VK_CHECK(vkCreateSampler(dev, &si, NULL, &e->s));
    e->next = samps;
    samps = e;
    return e->s;
}

/* ---- rendering scopes ------------------------------------------------------------- *
 * A scope is chosen by select_target() and begun lazily (ensure_rendering) at its first
 * draw: a clear of the whole target before that becomes the attachments' load op, so
 * a tile GPU clears tiles instead of loading them from memory and clearing again. */
static bool rendering, pending;
static VTex *cur_color[4], *cur_depth;
static u32 cur_w, cur_h, cur_ncolor;
static VkRenderingAttachmentInfo pend_ca[4], pend_da, pend_sa;
static u32 st_passes;
static bool drawlog;

static void ensure_rendering(void) {
    if (!pending) return;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = (VkRect2D){{0, 0}, {cur_w, cur_h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = cur_ncolor;
    ri.pColorAttachments = pend_ca;
    ri.pDepthAttachment = cur_depth ? &pend_da : NULL;
    ri.pStencilAttachment = cur_depth && cur_depth->vf != VK_FORMAT_D16_UNORM ? &pend_sa : NULL;
    if (drawlog)
        x2_log("pass %ux%u color %u (%s) depth %s", cur_w, cur_h, cur_ncolor,
               pend_ca[0].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? "clear" : "load",
               !cur_depth                                      ? "none"
               : pend_da.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? "clear"
                                                               : "load");
    if (gputime2 && pass_valid[fs - slots] && pass_n[fs - slots] < PASSQ) {
        u32 sl = (u32)(fs - slots), p = pass_n[sl];
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, passq, sl * 2 * PASSQ + 2 * p);
        pass_rt[sl][p] = cur_color[0] ? cur_color[0]->id : 0;
        pass_w[sl][p] = cur_w;
        pass_h[sl][p] = cur_h;
        pass_open = true;
    }
    pvkCmdBeginRendering(cmd, &ri);
    pending = false;
    rendering = true;
    st_passes++;
}
static void end_rendering(void) {
    if (pending) { /* never drawn into: only clears in its load ops matter */
        bool clears = pend_da.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR || pend_sa.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR;
        for (u32 i = 0; i < cur_ncolor; i++) clears |= pend_ca[i].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR;
        if (clears) ensure_rendering();
        pending = false;
    }
    if (rendering) {
        pvkCmdEndRendering(cmd);
        rendering = false;
        if (pass_open) {
            u32 sl = (u32)(fs - slots);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, passq, sl * 2 * PASSQ + 2 * pass_n[sl] + 1);
            pass_n[sl]++;
            pass_open = false;
        }
    }
}
static void end_if_bound(VTex *t) {
    if (!rendering && !pending) return;
    for (int i = 0; i < 4; i++)
        if (cur_color[i] == t) end_rendering();
    if (cur_depth == t) end_rendering();
}

static VTex *target_of(const GfxState *st) {
    VTex *t = tex_get(st->rt[0]);
    if (t && t->rt) return t;
    t = tex_get(gfx_backbuffer);
    return t && t->rt ? t : &fallback_bb;
}

/* barriers batched into one vkCmdPipelineBarrier2 */
static VkImageMemoryBarrier2 batch[32];
static u32 nbatch;
static void batch_flush(void) {
    if (!nbatch) return;
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = nbatch;
    di.pImageMemoryBarriers = batch;
    pvkCmdPipelineBarrier2(cmd, &di);
    nbatch = 0;
}
static void batch_add(VTex *t, VkImageLayout to) {
    if (t->layout == to) return;
    if (nbatch == 32) batch_flush();
    batch[nbatch++] = layout_barrier(t, to);
    t->layout = to;
}

/* the state's targets chosen, the textures it samples readable */
/* need_depth: the draw or clear uses depth or stencil (a pass without them leaves the depth
 * buffer alone: no load and store of it on a tile GPU) */
static void select_target(const GfxState *st, VTex *const *sampled, u32 nsampled, bool need_depth) {
    VTex *t = target_of(st);
    VTex *color[4] = {t};
    for (int i = 1; i < 4; i++) {
        VTex *c = tex_get(st->rt[i]);
        if (c && c->rt && !c->depth && c->w == t->w && c->h == t->h) color[i] = c;
    }
    VTex *d = tex_get(st->ds);
    if (!(d && d->depth && d->w >= t->w && d->h >= t->h)) d = NULL;
    bool same = (rendering || pending) && !memcmp(cur_color, color, sizeof color) &&
                (cur_depth == (need_depth ? d : NULL) || (cur_depth == d && !need_depth));
    if (!need_depth) d = same ? cur_depth : NULL;
    bool need = false;
    for (u32 i = 0; i < nsampled; i++) {
        bool target = sampled[i] == d;
        for (int c = 0; c < 4; c++) target |= sampled[i] == color[c];
        if (!target && sampled[i]->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) need = true;
    }
    if (same && !need) return;
    end_rendering(); /* barriers happen outside rendering */
    /* the targets left behind become readable now: a draw sampling one later would
     * otherwise end its pass for the transition (and reload the tiles after it) */
    for (int i = 0; i < 4 && !same; i++) {
        bool kept = false;
        for (int c = 0; c < 4; c++) kept |= cur_color[i] == color[c];
        if (cur_color[i] && !kept && cur_color[i]->layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
            batch_add(cur_color[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    for (u32 i = 0; i < nsampled; i++) {
        bool target = sampled[i] == d;
        for (int c = 0; c < 4; c++) target |= sampled[i] == color[c];
        if (!target) batch_add(sampled[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    u32 n = 0;
    for (int i = 0; i < 4; i++) {
        memset(&pend_ca[i], 0, sizeof pend_ca[i]);
        pend_ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        if (!color[i]) continue;
        n = (u32)i + 1;
        bool fresh = color[i]->layout == VK_IMAGE_LAYOUT_UNDEFINED; /* GL hands out zeroed storage */
        batch_add(color[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        pend_ca[i].imageView = color[i]->att;
        pend_ca[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        pend_ca[i].loadOp = fresh ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        pend_ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color[i]->used = cur_serial;
    }
    memset(&pend_da, 0, sizeof pend_da);
    pend_da.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    if (d) {
        bool fresh = d->layout == VK_IMAGE_LAYOUT_UNDEFINED;
        batch_add(d, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        pend_da.imageView = d->att;
        pend_da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        pend_da.loadOp = fresh ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        pend_da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        d->used = cur_serial;
    }
    pend_sa = pend_da; /* depth and stencil can be cleared separately */
    batch_flush();
    pending = true;
    memcpy(cur_color, color, sizeof color);
    cur_depth = d;
    cur_w = t->w;
    cur_h = t->h;
    cur_ncolor = n;
}

static VkRect2D clamp_rect(s64 x, s64 y, s64 w, s64 h) {
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x > cur_w) x = cur_w;
    if (y > cur_h) y = cur_h;
    if (x + w > cur_w) w = cur_w - x;
    if (y + h > cur_h) h = cur_h - y;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    return (VkRect2D){{(s32)x, (s32)y}, {(u32)w, (u32)h}};
}

static u32 st_draws, st_clears, st_frames;
static u32 drawlog_at;

static void do_clear(const Clear *k) {
    select_target(&k->st, NULL, 0, (k->flags & 6) != 0);
    const GfxViewport *vp = &k->st.vp;
    float s = (float)target_of(&k->st)->scale;
    VkRect2D r = clamp_rect((s64)(vp->x * s), (s64)(vp->y * s), (s64)(vp->w * s), (s64)(vp->h * s));
    if (!r.extent.width || !r.extent.height) return;
    VkClearColorValue col = {{(k->color >> 16 & 0xFF) / 255.f, (k->color >> 8 & 0xFF) / 255.f,
                              (k->color & 0xFF) / 255.f, (k->color >> 24) / 255.f}};
    bool cdepth = (k->flags & 2) && cur_depth,
         cstencil = (k->flags & 4) && cur_depth && cur_depth->vf != VK_FORMAT_D16_UNORM;
    st_clears++;
    if (pending && !r.offset.x && !r.offset.y && r.extent.width == cur_w && r.extent.height == cur_h) {
        /* nothing drawn yet and the whole target: the load ops clear */
        if (k->flags & 1)
            for (u32 i = 0; i < cur_ncolor; i++)
                if (cur_color[i]) {
                    pend_ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                    pend_ca[i].clearValue.color = col;
                }
        if (cdepth) {
            pend_da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            pend_da.clearValue.depthStencil.depth = k->z;
        }
        if (cstencil) {
            pend_sa.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            pend_sa.clearValue.depthStencil.stencil = k->stencil & 0xFF;
        }
        return;
    }
    ensure_rendering();
    VkClearAttachment a[5];
    u32 n = 0;
    if (k->flags & 1)
        for (u32 i = 0; i < cur_ncolor; i++) {
            if (!cur_color[i]) continue;
            a[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            a[n].colorAttachment = i;
            a[n].clearValue.color = col;
            n++;
        }
    VkImageAspectFlags da = (cdepth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (cstencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    if (da) {
        a[n].aspectMask = da;
        a[n].colorAttachment = 0;
        a[n].clearValue.depthStencil = (VkClearDepthStencilValue){k->z, k->stencil & 0xFF};
        n++;
    }
    if (!n) return;
    VkClearRect cr = {r, 0, 1};
    vkCmdClearAttachments(cmd, n, a, 1, &cr);
}

/* StretchRect */
static void do_blit(const Blit *k) {
    VTex *s = tex_get(k->src), *d = tex_get(k->dst);
    if (!s && k->src == gfx_backbuffer) s = &fallback_bb;
    if (!d && k->dst == gfx_backbuffer) d = &fallback_bb;
    if (!s || !d || s == d || !d->rt) return;
    end_rendering();
    transition(s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    u32 ss = s->scale, ds = d->scale;
    VkImageBlit b = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     {{(s32)(k->s[0] * ss), (s32)(k->s[1] * ss), 0}, {(s32)(k->s[2] * ss), (s32)(k->s[3] * ss), 1}},
                     {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     {{(s32)(k->d[0] * ds), (s32)(k->d[1] * ds), 0}, {(s32)(k->d[2] * ds), (s32)(k->d[3] * ds), 1}}};
    vkCmdBlitImage(cmd, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &b, k->linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    s->used = d->used = cur_serial;
}

/* ---- draws ---------------------------------------------------------------------- */
static u32 prim_count_to_verts(u32 prim, u32 n) {
    switch (prim) {
    case 1: return n;
    case 2: return n * 2;
    case 3: return n + 1;
    case 4: return n * 3;
    case 5:
    case 6: return n + 2;
    default: return 0;
    }
}

static void draw(const GfxState *st, u32 prim, u32 start, u32 count, bool indexed, s32 base, const u8 *up_verts,
                 u32 up_vbytes, u32 up_stride, const u8 *up_idx, u32 up_isz) {
    VShader *vs = st->vs < MAX_IDS ? shaders[st->vs] : NULL, *ps = st->ps < MAX_IDS ? shaders[st->ps] : NULL;
    if (drawlog)
        x2_log("draw prim %u start %u count %u idx %d up %d / vs %u ps %u decl %u rt0 %u ds %u", prim, start, count,
               indexed, up_verts != NULL, st->vs, st->ps, st->decl, st->rt[0], st->ds);
    if (!vs || !ps || !vs->ok || !ps->ok) return;
    Decl *dc = st->decl < MAX_IDS ? decls[st->decl] : NULL;
    if (!dc) return;
    u32 n = prim_count_to_verts(prim, count);
    if (!n) return;
    const u32 *rs = st->rs;

    /* indices (always from the ring: fans become lists) and the vertex range */
    u32 isz = up_verts ? up_isz : (st->ib_index_size ? st->ib_index_size : 2);
    const u8 *isrc = NULL;
    if (indexed) {
        if (up_verts)
            isrc = up_idx;
        else {
            VBuf *ib = st->ib < MAX_IDS ? bufs[st->ib] : NULL;
            if (!ib || (u64)(start + n) * isz > ib->size) return;
            isrc = ib->shadow + (size_t)start * isz;
        }
    }
    VkPrimitiveTopology topo = prim == 1   ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                               : prim == 2 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                               : prim == 3 ? VK_PRIMITIVE_TOPOLOGY_LINE_STRIP
                               : prim == 5 ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
                                           : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    u32 nidx = prim == 6 ? count * 3 : n;
    bool use_idx = indexed || prim == 6;
    s64 vlo, vhi;
    if (indexed) {
        u32 mn = ~0u, mx = 0;
        for (u32 i = 0; i < n; i++) {
            u32 v = isz == 4 ? ((const u32 *)isrc)[i] : ((const u16 *)isrc)[i];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        vlo = (s64)base + mn;
        vhi = (s64)base + mx + 1;
    } else {
        vlo = start;
        vhi = (s64)start + n;
    }
    if (vlo < 0) return;
    bool any_stream = up_verts != NULL;
    for (u32 k2 = 0; k2 < dc->n && !any_stream; k2++) {
        GfxId b = st->vb[dc->e[k2].stream];
        if (b < MAX_IDS && bufs[b] && bufs[b]->dynamic) any_stream = true;
    }
    /* everything this draw puts in the ring, reserved at once: no flush midway */
    u64 need = 4 * 256 + sizeof(VsU) + sizeof(PsU) + (u64)nidx * 4 + 64;
    if (up_verts)
        need += (u64)(vhi - vlo) * up_stride + 16;
    else if (any_stream)
        for (int s = 0; s < GFX_MAX_STREAMS; s++) need += (u64)(vhi - vlo) * st->vb_stride[s] + 16;
    ring_reserve(need);

    /* textures this draw samples */
    VTex *samp_tex[20];
    VkDescriptorImageInfo img[20];
    u32 binding[20], nsamp = 0;
    for (int pass_i = 0; pass_i < 2; pass_i++) {
        VShader *sh = pass_i ? ps : vs;
        for (int i = 0; i < 16; i++) {
            if (!(sh->info.samplers_used & (1u << i)) || (!pass_i && i >= 4)) continue;
            int ty = sh->info.samp_type[i];
            VTex *tx = tex_get(st->tex[i]);
            /* a texture of another type than the sampler reads black, as in GL (no 3D textures exist) */
            VTex *use = tx && ty != 4 && tx->cube == (ty == 3) ? tx : black[ty == 3 ? 1 : ty == 4 ? 2 : 0];
            if (drawlog)
                x2_log("  %s s%d: tex %u (%ux%u fmt %u)%s", pass_i ? "ps" : "vs", i, st->tex[i], tx ? tx->w : 0,
                       tx ? tx->h : 0, tx ? tx->fmt : 0, use == tx ? "" : " black");
            if (!pass_i && !use->vs_sampled) {
                /* a vertex shader reads it: from now on its barriers include the vertex stage, and
                 * one now covers the readable layout it may already be in (made for pixel shaders) */
                use->vs_sampled = true;
                if (use->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                    end_rendering();
                    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
                    mb.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
                    mb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                    di.memoryBarrierCount = 1;
                    di.pMemoryBarriers = &mb;
                    pvkCmdPipelineBarrier2(cmd, &di);
                }
            }
            use->used = cur_serial;
            samp_tex[nsamp] = use;
            img[nsamp] =
                (VkDescriptorImageInfo){get_sampler(st->samp[i]), use->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            binding[nsamp++] = pass_i ? 2 + (u32)i : 18 + (u32)i;
        }
    }

    /* vertex inputs */
    PipeKey pk;
    memset(&pk, 0, sizeof pk);
    pk.vs = vs->serial;
    pk.ps = ps->serial;
    pk.atest = rs[15] && rs[25] != 8 ? 1 : 0;
    pk.topo = (u32)topo;
    u32 bgra_mask = 0;
    VkBuffer sbuf[GFX_MAX_STREAMS] = {0};
    VkDeviceSize soff[GFX_MAX_STREAMS] = {0};
    for (int loc = 0; loc < 16; loc++) {
        if (!(vs->info.vs_in_mask & (1u << loc))) continue;
        int usage = vs->info.vs_in_usage[loc], index = vs->info.vs_in_index[loc];
        const Elem *e = NULL;
        for (u32 k2 = 0; k2 < dc->n; k2++)
            if (dc->e[k2].usage == usage && dc->e[k2].index == index) {
                e = &dc->e[k2];
                break;
            }
        u32 vt = 0;
        bool bgra = false;
        VkFormat vf = e ? vertex_format(e->type, &vt, &bgra) : VK_FORMAT_UNDEFINED;
        u32 s = e ? e->stream : 0;
        if (vf && !up_verts && !sbuf[s]) {
            VBuf *b = st->vb[s] < MAX_IDS ? bufs[st->vb[s]] : NULL;
            if (!b)
                vf = VK_FORMAT_UNDEFINED;
            else if (any_stream) {
                u32 stride = st->vb_stride[s];
                u64 from = st->vb_offset[s] + (u64)vlo * stride, len = (u64)(vhi - vlo) * stride;
                if (from >= b->size)
                    vf = VK_FORMAT_UNDEFINED;
                else {
                    if (from + len > b->size) len = b->size - from;
                    u32 off = ring_put(b->shadow + from, (u32)len, 16);
                    if (off == ~0u) return;
                    sbuf[s] = fs->ring;
                    soff[s] = off;
                }
            } else {
                sbuf[s] = b->buf;
                soff[s] = st->vb_offset[s];
                b->used = cur_serial;
            }
        }
        if (!vf) { /* the constant (0,0,0,1) */
            pk.afmt[loc] = 1;
            pk.abuf[loc] = 9;
            pk.stride[9] = 16;
            continue;
        }
        if (bgra) bgra_mask |= 1u << loc;
        pk.vtkey |= vt << (2 * loc);
        pk.afmt[loc] = (u8)(e->type + 1); /* stored compactly, mapped again at pipeline creation */
        pk.aoff[loc] = e->offset;
        pk.abuf[loc] = (u8)(1 + s);
        pk.stride[1 + s] = up_verts ? up_stride : st->vb_stride[s];
    }
    if (up_verts) {
        if ((u64)vhi * up_stride > up_vbytes) return;
        u32 off = ring_put(up_verts + (size_t)vlo * up_stride, (u32)((vhi - vlo) * up_stride), 16);
        if (off == ~0u) return;
        for (int s = 0; s < GFX_MAX_STREAMS; s++) {
            sbuf[s] = fs->ring;
            soff[s] = off;
        }
    }
    u32 ioff = 0, out_isz = indexed ? isz : 4;
    if (use_idx) {
        ioff = ring_alloc(nidx * out_isz, 4);
        if (ioff == ~0u) return;
        u8 *ip = ring_ptr(ioff);
        for (u32 i = 0; i < nidx; i++) {
            u32 src_i = prim == 6 ? (i % 3 == 0 ? 0 : i / 3 + i % 3) : i;
            u32 v = indexed ? (isz == 4 ? ((const u32 *)isrc)[src_i] : ((const u16 *)isrc)[src_i]) : start + src_i;
            if (out_isz == 4)
                ((u32 *)ip)[i] = v;
            else
                ((u16 *)ip)[i] = (u16)v;
        }
    }

    /* uniforms */
    u32 align = (u32)limits.minUniformBufferOffsetAlignment;
    u32 uoff = ring_alloc(sizeof(VsU), align), poff = ring_alloc(sizeof(PsU), align);
    if (uoff == ~0u || poff == ~0u) return;
    VsU *vu = ring_ptr(uoff);
    u32 nvc = vs->info.const_count < GFX_VS_CONSTS ? (u32)vs->info.const_count : GFX_VS_CONSTS;
    memcpy(vu->c, vsc, nvc * 16);
    memcpy(vu->i, vsi, sizeof vsi);
    for (int i = 0; i < 16; i++) vu->b[i][0] = vu->b[i][1] = vu->b[i][2] = vu->b[i][3] = vsb[i];
    vu->vp_fix[0] = 1.0f / st->vp.w; /* D3D9 pixel centres: +0.5 px */
    vu->vp_fix[1] = -1.0f / st->vp.h;
    vu->vp_fix[2] = vu->vp_fix[3] = 0;
    vu->bgra_mask = (s32)bgra_mask;
    PsU *pu = ring_ptr(poff);
    u32 npc = ps->info.const_count < GFX_PS_CONSTS ? (u32)ps->info.const_count : GFX_PS_CONSTS;
    memcpy(pu->c, psc, npc * 16);
    memcpy(pu->i, psi, sizeof psi);
    for (int i = 0; i < 16; i++) pu->b[i][0] = pu->b[i][1] = pu->b[i][2] = pu->b[i][3] = psb[i];
    pu->alpha_func = rs[15] ? (s32)rs[25] : 0;
    pu->alpha_ref = (float)(rs[24] & 0xFF);

    /* targets (sampled textures made readable first), then the pipeline for their formats */
    select_target(st, samp_tex, nsamp, rs[7] || rs[52]);
    for (u32 i = 0; i < cur_ncolor; i++) pk.cfmt[i] = cur_color[i] ? (u32)cur_color[i]->vf : 0;
    pk.ncolor = cur_ncolor;
    pk.dfmt = cur_depth ? (u32)cur_depth->vf : 0;
    if (rs[27]) {
        pk.blend = 1;
        pk.src = (u8)rs[19];
        pk.dst = (u8)rs[20];
        pk.op = (u8)rs[171];
        pk.srca = (u8)(rs[206] ? rs[207] : rs[19]);
        pk.dsta = (u8)(rs[206] ? rs[208] : rs[20]);
        pk.opa = (u8)(rs[206] ? rs[209] : rs[171]);
    }
    pk.wmask = (u8)(rs[168] & 0xF); /* D3D's RGBA bits are Vulkan's */
    VkPipeline pipe = get_pipeline(&pk, vs, ps);
    if (!pipe) return;

    ensure_rendering();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkDescriptorBufferInfo ub[2] = {{fs->ring, uoff, sizeof(VsU)}, {fs->ring, poff, sizeof(PsU)}};
    VkWriteDescriptorSet w[22];
    u32 nw = 0;
    for (u32 i = 0; i < 2; i++) {
        w[nw] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[nw].dstBinding = i;
        w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[nw++].pBufferInfo = &ub[i];
    }
    for (u32 i = 0; i < nsamp; i++) {
        w[nw] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[nw].dstBinding = binding[i];
        w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[nw++].pImageInfo = &img[i];
    }
    pvkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 0, nw, w);

    /* dynamic state */
    float sc = (float)target_of(st)->scale;
    VkViewport v = {st->vp.x * sc, st->vp.y * sc, st->vp.w * sc, st->vp.h * sc, st->vp.minz, st->vp.maxz};
    vkCmdSetViewport(cmd, 0, 1, &v);
    VkRect2D sr = rs[174] ? clamp_rect((s64)st->scissor[0] * (s64)sc, (s64)st->scissor[1] * (s64)sc,
                                       (s64)(st->scissor[2] - st->scissor[0]) * (s64)sc,
                                       (s64)(st->scissor[3] - st->scissor[1]) * (s64)sc)
                          : (VkRect2D){{0, 0}, {cur_w, cur_h}};
    vkCmdSetScissor(cmd, 0, 1, &sr);
    u32 bf = rs[193];
    float bc[4] = {(bf >> 16 & 0xFF) / 255.f, (bf >> 8 & 0xFF) / 255.f, (bf & 0xFF) / 255.f, (bf >> 24) / 255.f};
    vkCmdSetBlendConstants(cmd, bc);
    /* D3D's front faces are clockwise on screen, as Vulkan sees them after the y flip */
    pvkCmdSetFrontFace(cmd, VK_FRONT_FACE_CLOCKWISE);
    pvkCmdSetCullMode(cmd, rs[22] == 1   ? VK_CULL_MODE_NONE
                           : rs[22] == 3 ? VK_CULL_MODE_BACK_BIT
                                         : VK_CULL_MODE_FRONT_BIT);
    bool ztest = rs[7] && cur_depth;
    pvkCmdSetDepthTestEnable(cmd, ztest);
    pvkCmdSetDepthWriteEnable(cmd, ztest && rs[14]);
    pvkCmdSetDepthCompareOp(cmd, rs[23] >= 1 && rs[23] <= 8 ? (VkCompareOp)(rs[23] - 1) : VK_COMPARE_OP_ALWAYS);
    bool stencil = rs[52] && cur_depth && cur_depth->vf != VK_FORMAT_D16_UNORM;
    pvkCmdSetStencilTestEnable(cmd, stencil);
    static const VkStencilOp sop[9] = {VK_STENCIL_OP_KEEP,
                                       VK_STENCIL_OP_KEEP,
                                       VK_STENCIL_OP_ZERO,
                                       VK_STENCIL_OP_REPLACE,
                                       VK_STENCIL_OP_INCREMENT_AND_CLAMP,
                                       VK_STENCIL_OP_DECREMENT_AND_CLAMP,
                                       VK_STENCIL_OP_INVERT,
                                       VK_STENCIL_OP_INCREMENT_AND_WRAP,
                                       VK_STENCIL_OP_DECREMENT_AND_WRAP};
    pvkCmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, sop[rs[53] < 9 ? rs[53] : 0], sop[rs[55] < 9 ? rs[55] : 0],
                       sop[rs[54] < 9 ? rs[54] : 0],
                       rs[56] >= 1 && rs[56] <= 8 ? (VkCompareOp)(rs[56] - 1) : VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, rs[57] & 0xFF);
    vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, rs[58] & 0xFF);
    vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, rs[59] & 0xFF);

    for (int s = 0; s < GFX_MAX_STREAMS; s++)
        if (sbuf[s]) vkCmdBindVertexBuffers(cmd, 1 + (u32)s, 1, &sbuf[s], &soff[s]);
    if (pk.stride[9]) {
        VkDeviceSize o = CONST_OFF;
        vkCmdBindVertexBuffers(cmd, 9, 1, &fs->ring, &o);
    }
    s32 base_vertex = any_stream ? (indexed ? (s32)(base - vlo) : -(s32)vlo) : (indexed ? base : 0);
    if (use_idx) {
        vkCmdBindIndexBuffer(cmd, fs->ring, ioff, out_isz == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cmd, nidx, 1, 0, base_vertex, 0);
    } else {
        vkCmdDraw(cmd, n, 1, any_stream ? 0 : start, 0);
    }
    st_draws++;
}

/* ---- replay -------------------------------------------------------------------- */
static void run_frame(Frame *f) {
    frame_begin();
    for (size_t off = 0; off < f->len;) {
        u32 type = ((u32 *)(f->data + off))[0], size = ((u32 *)(f->data + off))[1];
        const void *p = f->data + off + 8;
        switch (type) {
        case C_TEX_CREATE: do_tex_create(p); break;
        case C_TEX_UPLOAD: do_tex_upload(p); break;
        case C_TEX_DESTROY: do_tex_destroy(*(const GfxId *)p); break;
        case C_BUF_CREATE: do_buf_create(p); break;
        case C_BUF_UPLOAD: do_buf_upload(p); break;
        case C_BUF_DESTROY: do_buf_destroy(*(const GfxId *)p); break;
        case C_SHADER_CREATE: {
            const ShaderCreate *s = p;
            do_shader_create(s->id, s->pixel, (const u32 *)(s + 1), s->ntokens);
            break;
        }
        case C_SHADER_DESTROY: do_shader_destroy(*(const GfxId *)p); break;
        case C_DECL_CREATE: {
            const DeclCreate *d = p;
            do_decl_create(d->id, (const u8 *)(d + 1), d->count);
            break;
        }
        case C_CONSTS: {
            const Consts *k = p;
            bool pixel = k->pixel & 1;
            if (k->pixel & 6) {
                u32 st0 = k->start, cnt = k->count;
                if (st0 >= 16) break;
                if (st0 + cnt > 16) cnt = 16 - st0;
                if (k->pixel & 4)
                    memcpy(pixel ? &psb[st0] : &vsb[st0], k + 1, cnt * 4);
                else
                    memcpy(pixel ? psi[st0] : vsi[st0], k + 1, cnt * 16);
            } else {
                u32 max = pixel ? GFX_PS_CONSTS : GFX_VS_CONSTS, st0 = k->start, cnt = k->count;
                if (st0 >= max) break;
                if (st0 + cnt > max) cnt = max - st0;
                memcpy(pixel ? psc[st0] : vsc[st0], k + 1, cnt * 16);
            }
            break;
        }
        case C_CLEAR: do_clear(p); break;
        case C_DRAW: {
            const Draw *k = p;
            draw(&k->st, k->prim, k->start, k->count, k->indexed, k->base, NULL, 0, 0, NULL, 0);
            break;
        }
        case C_DRAW_UP: {
            const DrawUp *k = p;
            const u8 *v = (const u8 *)(k + 1);
            draw(&k->st, k->prim, 0, k->count, k->ibytes != 0, 0, v, k->vbytes, k->stride,
                 k->ibytes ? v + k->vbytes : NULL, k->isz);
            break;
        }
        case C_BLIT: do_blit(p); break;
        case C_PRESENT:
            drawlog = drawlog_at && st_frames + 2 == drawlog_at;
            if (drawlog) x2_log("---- frame %u (vulkan) ----", st_frames + 2);
            gfx_frame_replayed();
            if (pcache_dirty && st_frames % 300 == 299) pcache_save();
            if (++st_frames % 120 == 0) {
                x2_log("gfx: frame %u: %u draws, %u clears, %u passes in the last 120 frames (vulkan)", st_frames,
                       st_draws, st_clears, st_passes);
                st_passes = 0;
                if (gpu_n) x2_log("gputime: %.2f ms avg, %.2f max", gpu_ms_sum / gpu_n, gpu_ms_max);
                gpu_ms_sum = gpu_ms_max = 0;
                gpu_n = 0;
                st_draws = st_clears = 0;
            }
            break;
        }
        off += size;
    }
    frame_end();
}

/* ---- presentation ---------------------------------------------------------------- */
static VkSurfaceKHR surface;
static VkSwapchainKHR swapchain;
static VkFormat sc_format;
static VkExtent2D sc_extent;
static VkImage sc_img[8];
static VkImageLayout sc_layout[8];
static VkSemaphore sc_release[8];
static VkImageView sc_view[8]; /* overlays render into the swapchain image */
static u32 sc_n;
static VkSemaphore acq_sem[INFLIGHT];
static VkCommandBuffer pr_cb[INFLIGHT];
static VkFence pr_fence[INFLIGHT];
static u32 pr_i;

static void swapchain_create(u32 w, u32 h) {
    vkDeviceWaitIdle(dev);
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps));
    u32 nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &nf, NULL);
    VkSurfaceFormatKHR fm[32];
    if (nf > 32) nf = 32;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &nf, fm);
    sc_format = fm[0].format;
    VkColorSpaceKHR cs = fm[0].colorSpace;
    for (u32 i = 0; i < nf; i++)
        if (fm[i].format == VK_FORMAT_B8G8R8A8_UNORM || fm[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
            sc_format = fm[i].format;
            cs = fm[i].colorSpace;
            break;
        }
    sc_extent = caps.currentExtent.width != 0xFFFFFFFFu ? caps.currentExtent : (VkExtent2D){w, h};
    u32 np = 0;
    VkPresentModeKHR pm[8], mode = VK_PRESENT_MODE_FIFO_KHR;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &np, NULL);
    if (np > 8) np = 8;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &np, pm);
    /* the guest paces itself: present without waiting for vblank unless X2_VSYNC */
    if (!getenv("X2_VSYNC"))
        for (u32 i = 0; i < np; i++)
            if (pm[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = pm[i];
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface;
    ci.minImageCount = caps.minImageCount + 1 > 3 ? caps.minImageCount + 1 : 3;
    if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount) ci.minImageCount = caps.maxImageCount;
    ci.imageFormat = sc_format;
    ci.imageColorSpace = cs;
    ci.imageExtent = sc_extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR; /* the compositor rotates */
    ci.compositeAlpha = caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                            : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swapchain;
    VkSwapchainKHR sc;
    VK_CHECK(vkCreateSwapchainKHR(dev, &ci, NULL, &sc));
    if (swapchain) vkDestroySwapchainKHR(dev, swapchain, NULL);
    swapchain = sc;
    sc_n = 8;
    VK_CHECK(vkGetSwapchainImagesKHR(dev, swapchain, &sc_n, sc_img));
    for (u32 i = 0; i < 8; i++) {
        if (sc_view[i]) vkDestroyImageView(dev, sc_view[i], NULL);
        sc_view[i] = VK_NULL_HANDLE;
    }
    for (u32 i = 0; i < sc_n; i++) {
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = sc_img[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = sc_format;
        vi.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(dev, &vi, NULL, &sc_view[i]));
        sc_layout[i] = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!sc_release[i]) {
            VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VK_CHECK(vkCreateSemaphore(dev, &si, NULL, &sc_release[i]));
        }
    }
    x2_log("vulkan: swapchain %ux%u, %u images, format %d, %s", sc_extent.width, sc_extent.height, sc_n, sc_format,
           mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo");
}

static VTex *shown(void) {
    VTex *t = tex_get(gfx_backbuffer);
    return t && t->rt ? t : &fallback_bb;
}

static void barrier_raw(VkCommandBuffer c, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    pvkCmdPipelineBarrier2(c, &di);
}

/* ---- window overlays (FPS counter, touch controls) --------------------------------
 * The front end queues quads in window pixels (top-left origin) before each present;
 * they are drawn over the game image with the modes of platform_sdl.c's GL overlay:
 * 0 ring (procedural), 1 label (texture alpha x colour), 2 opaque texture. */
typedef struct {
    VkImage img;
    VkImageView view;
    Alloc mem;
    VkBuffer stage;
    Alloc smem;
    u32 w, h;
    bool dirty, ready;
} OvTex;
typedef struct {
    float x, y, w, h, col[4];
    u32 tex;
    int mode;
} OvQuad;
static OvTex ovt[64];
static u32 ovt_n = 1; /* 0 = none */
static OvQuad ovq[64];
static u32 ovq_n;
static VkPipeline ov_pipe;
static VkFormat ov_pipe_fmt;
static VkPipelineLayout ov_layout;
static VkDescriptorSetLayout ov_dsl;
static VkSampler ov_samp;
static u32 ov_white;

/* a new overlay texture (id 0), or new pixels for one of the same size; returns its id */
u32 gfx_vk_overlay_tex(u32 id, const u8 *rgba, u32 w, u32 h) {
    if (id >= ovt_n) id = 0;
    if (id && (ovt[id].w != w || ovt[id].h != h)) { /* a new size (the FPS text): rebuild in place */
        OvTex *t = &ovt[id];
        vkDeviceWaitIdle(dev);
        vkDestroyImageView(dev, t->view, NULL);
        vkDestroyImage(dev, t->img, NULL);
        vkDestroyBuffer(dev, t->stage, NULL);
        mem_free(t->mem);
        mem_free(t->smem);
        memset(t, 0, sizeof *t);
    }
    if (!id || !ovt[id].img) {
        if (!id && ovt_n >= 64) return 0;
        if (!id) id = ovt_n++;
        OvTex *t = &ovt[id];
        t->w = w;
        t->h = h;
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R8G8B8A8_UNORM;
        ci.extent = (VkExtent3D){w, h, 1};
        ci.mipLevels = ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VK_CHECK(vkCreateImage(dev, &ci, NULL, &t->img));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev, t->img, &req);
        t->mem = mem_alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkBindImageMemory(dev, t->img, blocks[t->mem.block].mem, t->mem.off));
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = t->img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = ci.format;
        vi.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(dev, &vi, NULL, &t->view));
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = (VkDeviceSize)w * h * 4;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VK_CHECK(vkCreateBuffer(dev, &bi, NULL, &t->stage));
        vkGetBufferMemoryRequirements(dev, t->stage, &req);
        t->smem = mem_alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkBindBufferMemory(dev, t->stage, blocks[t->smem.block].mem, t->smem.off));
    }
    /* the copy of the previous pixels finished presents ago (labels: once; FPS: twice a second) */
    memcpy(blocks[ovt[id].smem.block].map + ovt[id].smem.off, rgba, (size_t)w * h * 4);
    ovt[id].dirty = true;
    return id;
}

void gfx_vk_overlay_quad(float x, float y, float w, float h, u32 tex, int mode, float r, float g, float b, float a) {
    if (ovq_n < 64) ovq[ovq_n++] = (OvQuad){x, y, w, h, {r, g, b, a}, tex, mode};
}

static void ov_init(VkFormat fmt) {
    if (!ov_layout) {
        VkDescriptorSetLayoutBinding lb = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                           VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL,
                                              VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, 1, &lb};
        VK_CHECK(vkCreateDescriptorSetLayout(dev, &dl, NULL, &ov_dsl));
        VkPushConstantRange pr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &ov_dsl, 1, &pr};
        VK_CHECK(vkCreatePipelineLayout(dev, &pl, NULL, &ov_layout));
        VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 0.25f;
        VK_CHECK(vkCreateSampler(dev, &si, NULL, &ov_samp));
        static const u8 white[4] = {255, 255, 255, 255};
        ov_white = gfx_vk_overlay_tex(0, white, 1, 1);
    }
    if (ov_pipe && ov_pipe_fmt == fmt) return;
    if (ov_pipe) {
        vkDeviceWaitIdle(dev);
        vkDestroyPipeline(dev, ov_pipe, NULL);
    }
    /* a triangle covering the viewport (the quad), uv (0,0) at its top-left */
    VShader vs = {.pixel = false,
                  .ok = true,
                  .src = "#version 450\n"
                         "layout(location=0) out vec2 uv;\n"
                         "void main() {\n"
                         "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
                         "  uv = p;\n"
                         "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
                         "}\n"};
    VShader fs = {.pixel = true,
                  .ok = true,
                  .src = "#version 450\n"
                         "layout(location=0) in vec2 uv;\n"
                         "layout(set=0, binding=0) uniform sampler2D img;\n"
                         "layout(push_constant) uniform P { vec4 col; int mode; } pc;\n"
                         "layout(location=0) out vec4 o;\n"
                         "void main() {\n"
                         "  vec4 t = texture(img, uv);\n"
                         "  if (pc.mode == 2) { o = vec4(t.rgb, 1.0); return; }\n"
                         "  if (pc.mode == 1) { o = vec4(pc.col.rgb, t.a * pc.col.a); return; }\n"
                         "  float d = length(uv * 2.0 - 1.0);\n"
                         "  float a = (1.0 - smoothstep(0.92, 1.0, d)) * (d > 0.86 ? 1.0 : 0.35);\n"
                         "  o = vec4(pc.col.rgb, a * pc.col.a);\n"
                         "}\n"};
    VkShaderModule mv = compile(&vs, ""), mf = compile(&fs, "");
    if (!mv || !mf) x2_fatal("vulkan: overlay shaders");
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, mv, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, mf, "main", NULL}};
    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba = {VK_TRUE,
                                              VK_BLEND_FACTOR_SRC_ALPHA,
                                              VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                              VK_BLEND_OP_ADD,
                                              VK_BLEND_FACTOR_ZERO,
                                              VK_BLEND_FACTOR_ONE,
                                              VK_BLEND_OP_ADD,
                                              0xf};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &fmt;
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ri;
    gi.stageCount = 2;
    gi.pStages = st;
    gi.pVertexInputState = &vin;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &ds;
    gi.layout = ov_layout;
    VK_CHECK(vkCreateGraphicsPipelines(dev, pcache, 1, &gi, NULL, &ov_pipe));
    vkDestroyShaderModule(dev, mv, NULL);
    vkDestroyShaderModule(dev, mf, NULL);
    ov_pipe_fmt = fmt;
}

/* the queued quads into the swapchain image (in TRANSFER_DST layout; left in PRESENT_SRC) */
static void ov_draw(VkCommandBuffer c, VkImage img, VkImageView view) {
    for (u32 i = 1; i < ovt_n; i++) {
        OvTex *t = &ovt[i];
        if (!t->dirty) continue;
        barrier_raw(c, t->img, t->ready ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy cp = {0};
        cp.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        cp.imageExtent = (VkExtent3D){t->w, t->h, 1};
        vkCmdCopyBufferToImage(c, t->stage, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
        barrier_raw(c, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        t->dirty = false;
        t->ready = true;
    }
    barrier_raw(c, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea.extent = sc_extent;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    pvkCmdBeginRendering(c, &ri);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, ov_pipe);
    VkRect2D sc = {{0, 0}, sc_extent};
    vkCmdSetScissor(c, 0, 1, &sc);
    for (u32 i = 0; i < ovq_n; i++) {
        const OvQuad *q = &ovq[i];
        if (q->w <= 0 || q->h <= 0) continue;
        u32 id = q->tex && q->tex < ovt_n && ovt[q->tex].ready ? q->tex : ov_white;
        VkDescriptorImageInfo ii = {ov_samp, ovt[id].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        pvkCmdPushDescriptorSetKHR(c, VK_PIPELINE_BIND_POINT_GRAPHICS, ov_layout, 0, 1, &w);
        struct {
            float col[4];
            s32 mode, pad[3];
        } pc = {{q->col[0], q->col[1], q->col[2], q->col[3]}, q->mode, {0}};
        vkCmdPushConstants(c, ov_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        VkViewport v = {q->x, q->y, q->w, q->h, 0, 1};
        vkCmdSetViewport(c, 0, 1, &v);
        vkCmdDraw(c, 3, 1, 0, 0);
    }
    pvkCmdEndRendering(c);
    barrier_raw(c, img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    ovq_n = 0;
}

/* the game's image into the window, letterboxed */
void gfx_vk_present(u32 ww, u32 wh) {
    if (!surface || !ww || !wh) return;
    static bool stale;
    if (!swapchain || stale || ww != sc_extent.width || wh != sc_extent.height) {
        swapchain_create(ww, wh);
        stale = false;
    }
    u32 i = pr_i++ % INFLIGHT;
    VK_CHECK(vkWaitForFences(dev, 1, &pr_fence[i], VK_TRUE, UINT64_MAX));
    u32 idx;
    VkResult r = vkAcquireNextImageKHR(dev, swapchain, 100000000ull, acq_sem[i], VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchain_create(ww, wh);
        return;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return;
    VK_CHECK(vkResetFences(dev, 1, &pr_fence[i]));
    VkCommandBuffer c = pr_cb[i];
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                   VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    VK_CHECK(vkResetCommandBuffer(c, 0));
    VK_CHECK(vkBeginCommandBuffer(c, &bi));
    VTex *t = shown();
    VkCommandBuffer saved = cmd;
    cmd = c; /* transition() records into cmd */
    transition(t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    cmd = saved;
    barrier_raw(c, sc_img[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black_c = {{0, 0, 0, 1}};
    VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(c, sc_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black_c, 1, &rr);
    /* KOF XIII Climax's 4:3 image carries a 16:9 picture with bars: show that band */
    float y0 = 0, y1 = (float)t->h, ih = (float)t->h;
    if (t->w * 3 == t->h * 4 && x2_game_letterboxed()) {
        ih = (float)(t->w * 9 / 16);
        y0 = ((float)t->h - ih) / 2;
        y1 = y0 + ih;
    }
    float sa = (float)sc_extent.width / (float)sc_extent.height, ga = (float)t->w / ih;
    s32 vw = (s32)sc_extent.width, vh = (s32)sc_extent.height;
    if (sa > ga)
        vw = (s32)(sc_extent.height * ga);
    else
        vh = (s32)(sc_extent.width / ga);
    s32 x0 = ((s32)sc_extent.width - vw) / 2, yy0 = ((s32)sc_extent.height - vh) / 2;
    VkImageBlit b = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     {{0, (s32)y0, 0}, {(s32)t->w, (s32)y1, 1}},
                     {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     {{x0, yy0, 0}, {x0 + vw, yy0 + vh, 1}}};
    vkCmdBlitImage(c, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &b, VK_FILTER_LINEAR);
    if (ovq_n) {
        ov_init(sc_format);
        ov_draw(c, sc_img[idx], sc_view[idx]);
    } else {
        barrier_raw(c, sc_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
    VK_CHECK(vkEndCommandBuffer(c));
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acq_sem[i];
    si.pWaitDstStageMask = &ws;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &c;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &sc_release[idx];
    VK_CHECK(qsubmit(&si, pr_fence[i]));
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &sc_release[idx];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain;
    pi.pImageIndices = &idx;
    r = vkQueuePresentKHR(queue, &pi);
    /* SUBOPTIMAL: the display is rotated and the compositor turns the image; that is fine */
    if (r == VK_ERROR_OUT_OF_DATE_KHR) stale = true;
}

extern bool x2_write_png(const char *path, const u8 *rgba, u32 w, u32 h); /* png.c */

/* the shown image as a PNG (waits for the GPU) */
void gfx_vk_screenshot(const char *path) {
    VTex *t = shown();
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = (VkDeviceSize)t->w * t->h * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf;
    VK_CHECK(vkCreateBuffer(dev, &bi, NULL, &buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, buf, &req);
    Alloc a = mem_alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkBindBufferMemory(dev, buf, blocks[a.block].mem, a.off));
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cpool,
                                      VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer c;
    VK_CHECK(vkAllocateCommandBuffers(dev, &ai, &c));
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    VK_CHECK(vkBeginCommandBuffer(c, &cbi));
    VkCommandBuffer saved = cmd;
    cmd = c;
    transition(t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    cmd = saved;
    VkBufferImageCopy cp = {0};
    cp.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.imageExtent = (VkExtent3D){t->w, t->h, 1};
    vkCmdCopyImageToBuffer(c, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &cp);
    VK_CHECK(vkEndCommandBuffer(c));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &c;
    VK_CHECK(qsubmit(&si, VK_NULL_HANDLE));
    if (qlock) qlock(qlock_handle);
    VK_CHECK(vkQueueWaitIdle(queue));
    if (qunlock) qunlock(qlock_handle);
    u8 *px = blocks[a.block].map + a.off;
    size_t np = (size_t)t->w * t->h;
    bool bgra = t->vf == VK_FORMAT_B8G8R8A8_UNORM;
    for (size_t i = 0; i < np; i++) { /* to RGBA, opaque */
        if (bgra) {
            u8 tb = px[i * 4];
            px[i * 4] = px[i * 4 + 2];
            px[i * 4 + 2] = tb;
        }
        px[i * 4 + 3] = 255;
    }
    if (x2_write_png(path, px, t->w, t->h)) x2_log("screenshot saved to %s", path);
    vkFreeCommandBuffers(dev, cpool, 1, &c);
    vkDestroyBuffer(dev, buf, NULL);
    mem_free(a);
}

bool gfx_vk_run_frames(u32 timeout_ms) {
    Frame *list = gfx_take_frames(timeout_ms);
    bool any = list != NULL;
    u64 t0 = x2_time_ns();
    while (list) {
        Frame *n = list->next;
        run_frame(list);
        gfx_frame_free(list);
        list = n;
    }
    if (any) gfx_replay_time(x2_time_ns() - t0);
    return any;
}

/* ---- init ------------------------------------------------------------------------- */
static VTex *black_tex(bool cube, bool vol) {
    VTex *t = tex_new(GFX_FMT_BGRA8, 1, 1, 1, false, cube, 1, vol, false);
    static const u32 px[1] = {0xFF000000u};
    for (u32 f = 0; f < (cube ? 6u : 1u); f++) upload(t, 0, f, 0, 0, 1, 1, (const u8 *)px, 4);
    return t;
}

/* the device with what this backend needs (Vulkan 1.3 dynamic rendering and
 * synchronization2, push descriptors), on a graphics queue family that presents to
 * `surf` if given. `wrap`: a frontend's device creation (RetroArch adds the extensions
 * it needs itself); NULL creates it here, with the swapchain extension for `surf`. */
typedef VkDevice (*VkDeviceWrap)(VkPhysicalDevice gpu, void *opaque, const VkDeviceCreateInfo *ci);
VkDevice gfx_vk_make_device(VkPhysicalDevice gpu, VkSurfaceKHR surf, VkDeviceWrap wrap, void *opaque, u32 *out_qfam) {
    VkPhysicalDeviceProperties dp;
    vkGetPhysicalDeviceProperties(gpu, &dp);
    if (dp.apiVersion < VK_API_VERSION_1_3) {
        x2_log("vulkan: %s has only Vulkan %u.%u (1.3 needed)", dp.deviceName, VK_API_VERSION_MAJOR(dp.apiVersion),
               VK_API_VERSION_MINOR(dp.apiVersion));
        return VK_NULL_HANDLE;
    }
    u32 nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &nq, NULL);
    VkQueueFamilyProperties qp[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &nq, qp);
    u32 fam = ~0u;
    for (u32 i = 0; i < nq && fam == ~0u; i++) {
        VkBool32 present = VK_TRUE;
        if (surf) vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surf, &present);
        if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT && present) fam = i;
    }
    if (fam == ~0u) {
        x2_log("vulkan: no graphics queue");
        return VK_NULL_HANDLE;
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, fam, 1, &prio};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    const char *dext[] = {"VK_KHR_push_descriptor", "VK_KHR_swapchain"};
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.pNext = &f13;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = surf && !wrap ? 2 : 1;
    di.ppEnabledExtensionNames = dext;
    VkDevice d = VK_NULL_HANDLE;
    if (wrap)
        d = wrap(gpu, opaque, &di);
    else if (vkCreateDevice(gpu, &di, NULL, &d) != VK_SUCCESS)
        d = VK_NULL_HANDLE;
    *out_qfam = fam;
    return d;
}

/* everything after the device: entry points, layouts, pools, frames, built-in textures */
static void setup(u32 w, u32 h) {
    if (!glslang_initialize_process()) x2_fatal("vulkan: glslang");
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(phys, &pp);
    limits = pp.limits;
    vkGetPhysicalDeviceMemoryProperties(phys, &memprops);
#define X(f)                                                                                                           \
    p##f = (PFN_##f)vkGetDeviceProcAddr(dev, #f);                                                                      \
    if (!p##f) x2_fatal("vulkan: no %s", #f);
    VK_FUNCS(X)
#undef X
    /* D24S8 */
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(phys, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
    depth24 = fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT ? VK_FORMAT_D24_UNORM_S8_UINT
                                                                                        : VK_FORMAT_D32_SFLOAT_S8_UINT;
    /* one push-descriptor set: the two uniform blocks, 16 pixel and 4 vertex samplers */
    VkDescriptorSetLayoutBinding lb[22];
    for (u32 i = 0; i < 22; i++) {
        lb[i] = (VkDescriptorSetLayoutBinding){
            i < 2 ? i : i, i < 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
            i == 0 || i >= 18 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    }
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL,
                                          VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, 22, lb};
    VK_CHECK(vkCreateDescriptorSetLayout(dev, &dl, NULL, &dslayout));
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dslayout, 0, NULL};
    VK_CHECK(vkCreatePipelineLayout(dev, &pl, NULL, &playout));
    VkPipelineCacheCreateInfo pci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    void *pdata = NULL;
    if (gfx_gl_cache_dir[0]) { /* the driver checks it belongs to this GPU and driver */
        char path[700];
        snprintf(path, sizeof path, "%s/vk-pipelines.bin", gfx_gl_cache_dir);
        size_t sz;
        pdata = read_file(path, &sz);
        pci.initialDataSize = sz;
        pci.pInitialData = pdata;
    }
    if (vkCreatePipelineCache(dev, &pci, NULL, &pcache) != VK_SUCCESS) {
        pci.initialDataSize = 0;
        pci.pInitialData = NULL;
        VK_CHECK(vkCreatePipelineCache(dev, &pci, NULL, &pcache));
    }
    free(pdata);
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                   VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfam};
    VK_CHECK(vkCreateCommandPool(dev, &cpi, NULL, &cpool));
    for (int i = 0; i < INFLIGHT; i++) {
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cpool,
                                          VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        VK_CHECK(vkAllocateCommandBuffers(dev, &ai, &slots[i].cb));
        VK_CHECK(vkAllocateCommandBuffers(dev, &ai, &pr_cb[i]));
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT};
        VK_CHECK(vkCreateFence(dev, &fi, NULL, &slots[i].fence));
        VK_CHECK(vkCreateFence(dev, &fi, NULL, &pr_fence[i]));
        VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(dev, &si, NULL, &acq_sem[i]));
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = RING_SIZE;
        bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VK_CHECK(vkCreateBuffer(dev, &bi, NULL, &slots[i].ring));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, slots[i].ring, &req);
        slots[i].ring_mem = mem_alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkBindBufferMemory(dev, slots[i].ring, blocks[slots[i].ring_mem.block].mem, slots[i].ring_mem.off));
        slots[i].ring_map = blocks[slots[i].ring_mem.block].map + slots[i].ring_mem.off;
    }
    drawlog_at = getenv("X2_DRAWLOG") ? (u32)atoi(getenv("X2_DRAWLOG")) : 0;
    gputime = getenv("X2_GPUTIME") != NULL;
    gputime2 = getenv("X2_GPUTIME") && atoi(getenv("X2_GPUTIME")) == 2;
    if (gputime2) {
        VkQueryPoolCreateInfo pq = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        pq.queryType = VK_QUERY_TYPE_TIMESTAMP;
        pq.queryCount = INFLIGHT * 2 * PASSQ;
        VK_CHECK(vkCreateQueryPool(dev, &pq, NULL, &passq));
    }
    if (gputime) {
        VkQueryPoolCreateInfo qi2 = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi2.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi2.queryCount = INFLIGHT * 2;
        VK_CHECK(vkCreateQueryPool(dev, &qi2, NULL, &tsq));
    }
    /* the black textures and the fallback target, created in a first command buffer */
    frame_begin();
    black[0] = black_tex(false, false);
    black[1] = black_tex(true, false);
    black[2] = black_tex(false, true);
    fallback_bb = *tex_new(GFX_FMT_BGRA8, w, h, 1, true, false, 1, false, false);
    frame_end();
    x2_log("vulkan: %s, API %u.%u, D24S8 as %s", pp.deviceName, VK_API_VERSION_MAJOR(pp.apiVersion),
           VK_API_VERSION_MINOR(pp.apiVersion), depth24 == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8");
}

#ifdef X2_VK_SDL
/* false when there is no usable Vulkan 1.3 device (the caller falls back to GL) */
bool gfx_vk_init(SDL_Window *win, u32 w, u32 h) {
    Uint32 next = 0;
    const char *const *ext = SDL_Vulkan_GetInstanceExtensions(&next);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "typex2-recomp", 1, "x2", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ii.pApplicationInfo = &app;
    ii.enabledExtensionCount = next;
    ii.ppEnabledExtensionNames = ext;
    if (!ext || vkCreateInstance(&ii, NULL, &inst) != VK_SUCCESS) {
        x2_log("vulkan: no instance");
        return false;
    }
    const char *why = NULL;
    u32 np = 1;
    VkResult er;
    if (win && !SDL_Vulkan_CreateSurface(win, inst, NULL, &surface))
        why = SDL_GetError();
    else if (er = vkEnumeratePhysicalDevices(inst, &np, &phys), (er != VK_SUCCESS && er != VK_INCOMPLETE) || !np)
        why = "no device";
    else if (!(dev = gfx_vk_make_device(phys, surface, NULL, NULL, &qfam)))
        why = "cannot create the device";
    if (why) {
        x2_log("vulkan: %s", why);
        if (surface) SDL_Vulkan_DestroySurface(inst, surface, NULL);
        surface = VK_NULL_HANDLE;
        vkDestroyInstance(inst, NULL);
        inst = VK_NULL_HANDLE;
        return false;
    }
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    setup(w, h);
    return true;
}
#endif

/* size of the image Present shows */
void gfx_vk_shown_size(u32 *w, u32 *h) {
    VTex *t = shown();
    *w = t->w;
    *h = t->h;
}

/* RetroArch's context (made with gfx_vk_make_device through its wrapper) */
void gfx_vk_init_external(VkInstance i, VkPhysicalDevice g, VkDevice d, VkQueue q, u32 fam, void (*lock)(void *),
                          void (*unlock)(void *), void *handle, u32 w, u32 h) {
    inst = i;
    phys = g;
    dev = d;
    queue = q;
    qfam = fam;
    qlock = lock;
    qunlock = unlock;
    qlock_handle = handle;
    setup(w, h);
}

/* libretro: rows [v0, v1) of the shown image copied into an output image (three in
 * turn, so the frontend never samples one being written) and left readable by
 * shaders. Its handles are what the frontend's set_image takes. */
typedef struct {
    VTex *t;
    VkCommandBuffer cb;
    VkFence fence;
} OutSlot;
static OutSlot outs[INFLIGHT];
static u32 out_i;
bool gfx_vk_output(float v0, float v1, VkImage *img, VkImageView *view, VkImageViewCreateInfo *ci, u32 *ow, u32 *oh) {
    VTex *src = shown();
    u32 w = src->w, y0 = (u32)(v0 * src->h), y1 = (u32)(v1 * src->h), h = y1 - y0;
    if (!w || !h) return false;
    OutSlot *o = &outs[out_i++ % INFLIGHT];
    if (!o->cb) {
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cpool,
                                          VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        VK_CHECK(vkAllocateCommandBuffers(dev, &ai, &o->cb));
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT};
        VK_CHECK(vkCreateFence(dev, &fi, NULL, &o->fence));
    }
    VK_CHECK(vkWaitForFences(dev, 1, &o->fence, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(dev, 1, &o->fence));
    if (!o->t || o->t->w != w || o->t->h != h) {
        if (o->t) tex_free(o->t); /* destroyed once its last use is done */
        o->t = tex_new(GFX_FMT_BGRA8, w, h, 1, true, false, 1, false, false);
    }
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                   VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    VK_CHECK(vkResetCommandBuffer(o->cb, 0));
    VK_CHECK(vkBeginCommandBuffer(o->cb, &bi));
    VkCommandBuffer saved = cmd;
    cmd = o->cb; /* transition() records into cmd */
    transition(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(o->t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy c = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     {0, (s32)y0, 0},
                     {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     {0, 0, 0},
                     {w, h, 1}};
    vkCmdCopyImage(o->cb, src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, o->t->img,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    transition(o->t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    cmd = saved;
    VK_CHECK(vkEndCommandBuffer(o->cb));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &o->cb;
    VK_CHECK(qsubmit(&si, o->fence));
    o->t->used = cur_serial;
    *img = o->t->img;
    *view = o->t->view;
    memset(ci, 0, sizeof *ci);
    ci->sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ci->image = o->t->img;
    ci->viewType = VK_IMAGE_VIEW_TYPE_2D;
    ci->format = o->t->vf;
    ci->subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    *ow = w;
    *oh = h;
    return true;
}
