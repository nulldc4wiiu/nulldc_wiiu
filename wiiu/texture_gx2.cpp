// PVR texture decoding for the GX2 renderer: every format the TA can reference, decoded from VRAM to RGBA8
// GX2 textures, cached by TCW and size. Nothing tracks VRAM writes (the CPU writes texture memory directly), so
// once per new 3D frame each texture in use is checksummed (its VRAM bytes, plus the palette for palettised
// ones) and decoded again only if that changed. Large textures are sampled on most frames and fully hashed on
// every 16th. The checks and decodes run as a scene is built (wiiu_texture_scene, on the intake thread for screen
// frames), which writes and flushes the plain-memory surfaces; the GX2 thread invalidates the GPU's copies before
// drawing. A changed texture gets a new image (four in rotation), and dropped surfaces are freed late, so a
// scene still in flight never sees its textures change or vanish.
#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/core.h>
#include <coreinit/time.h>
#include <gx2/mem.h>
#include <gx2/sampler.h>
#include <gx2/texture.h>
#include <gx2/utils.h>
#include <string.h>
#include <vector>
#include "types.h"
#include "plugs/drkPvr/drkPvr.h"
#include "plugs/drkPvr/regs.h"
#include "plugs/drkPvr/ta_structs.h"
#include "wiiu_frame.h"

// offsets from the TCW address to the full-size level of a mipmapped texture (levels are stored small to
// large), indexed by log2(width): in texels for the plain formats, in index bytes after the codebook for VQ
static const u32 other_mip_point[11] = { 0x3, 0x4, 0x8, 0x18, 0x58, 0x158, 0x558, 0x1558, 0x5558, 0x15558, 0x55558 };
static const u32 vq_mip_point[11] = { 0x0, 0x1, 0x2, 0x6, 0x16, 0x56, 0x156, 0x556, 0x1556, 0x5556, 0x15556 };

static inline u32 rgba(u32 r, u32 g, u32 b, u32 a) { return (r << 24) | (g << 16) | (b << 8) | a; }

static inline u32 from1555(u32 p) { return rgba(((p >> 10) & 31) << 3, ((p >> 5) & 31) << 3, (p & 31) << 3, (p & 0x8000) ? 255 : 0); }
static inline u32 from565(u32 p) { return rgba((p >> 11) << 3, ((p >> 5) & 63) << 2, (p & 31) << 3, 255); }
static inline u32 from4444(u32 p) { return rgba(((p >> 8) & 15) * 17, ((p >> 4) & 15) * 17, (p & 15) * 17, (p >> 12) * 17); }
static inline u32 from8888(u32 p) { return rgba((p >> 16) & 255, (p >> 8) & 255, p & 255, p >> 24); }

static inline u32 from16(u32 fmt, u32 p)
{
    switch (fmt)
    {
    case 1: return from565(p);
    case 2: return from4444(p);
    case 4: return rgba(255, 255, 255, 255);   // bump map: not shaded, keep the polygon colour
    default: return from1555(p);
    }
}

static inline u32 palette_entry(u32 i)
{
    u32 p = PALETTE_RAM[i & 1023];
    switch (PAL_RAM_CTRL & 3)
    {
    case 0: return from1555(p);
    case 1: return from565(p);
    case 2: return from4444(p);
    default: return from8888(p);
    }
}

// texel order inside a twiddled texture: bits of y and x interleaved (y first), the larger axis keeps its rest.
// The x and y bits land in separate positions, so twiddle(x, y) = twiddle(x, 0) | twiddle(0, y): the decoders
// use per-axis tables made from this.
static u32 twiddle(u32 x, u32 y, u32 w, u32 h)
{
    u32 rv = 0, sh = 0;
    w >>= 1;
    h >>= 1;
    while (w || h)
    {
        if (h) { rv |= (y & 1) << sh; h >>= 1; y >>= 1; sh++; }
        if (w) { rv |= (x & 1) << sh; w >>= 1; x >>= 1; sh++; }
    }
    return rv;
}

static u32 tw_x[1024], tw_y[1024];
static void twiddle_tables(u32 w, u32 h)
{
    for (u32 x = 0; x < w; x++)
        tw_x[x] = twiddle(x, 0, w, h);
    for (u32 y = 0; y < h; y++)
        tw_y[y] = twiddle(0, y, w, h);
}

// 16-bit texel -> RGBA for 1555, 565 and 4444, made on first use (256 KB each)
static u32 *lut16[3];
static const u32 *texel_table(u32 fmt)
{
    const u32 k = fmt == 1 ? 1 : fmt == 2 ? 2 : 0;
    if (!lut16[k])
    {
        lut16[k] = (u32 *)MEMAllocFromDefaultHeapEx(65536 * 4, 64);
        for (u32 p = 0; p < 65536; p++)
            lut16[k][p] = k == 1 ? from565(p) : k == 2 ? from4444(p) : from1555(p);
    }
    return lut16[k];
}
static const u32 bump_texel[1] = { 0xFFFFFFFF };   // bump maps: not shaded, keep the polygon colour

static inline u32 vram16(u32 a) { return *host_ptr_xor((u16 *)&PVRPARAMS.vram[a & VRAM_MASK]); }
static inline u32 vram8(u32 a) { return *host_ptr_xor((u8 *)&PVRPARAMS.vram[a & VRAM_MASK]); }

static inline u32 yuv_to_rgba(int y, int u, int v)
{
    int r = y + ((v - 128) * 1436 >> 10), g = y - (((u - 128) * 352 + (v - 128) * 731) >> 10), b = y + ((u - 128) * 1815 >> 10);
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return rgba(r, g, b, 255);
}

// decode one level, w x h texels: the full-size texture, or for a mipmapped one (square) any level of its chain
static void decode(TCW tcw, u32 *out, u32 pitch, u32 w, u32 h)
{
    const u32 fmt = tcw.NO_PAL.PixelFmt;
    const bool vq = tcw.NO_PAL.VQ_Comp, mip = tcw.NO_PAL.MipMapped, twiddled = !tcw.NO_PAL.ScanOrder || fmt >= 5;
    u32 addr = (tcw.NO_PAL.TexAddr << 3) & VRAM_MASK;
    const u32 log2w = 31 - __builtin_clz(w);
    const bool bump = fmt == 4;
    const u32 *lut = bump ? bump_texel : texel_table(fmt);
    auto texel = [&](u32 p) { return lut[bump ? 0 : p]; };

    if (vq)
    {
        // 256-entry codebook of 2x2 blocks (16-bit texels in twiddled order), then one index byte per block
        const u32 book = addr;
        const u32 index = addr + 2048 + (mip ? vq_mip_point[log2w] : 0);
        if (w == 1)
        {
            // the 1x1 level of a VQ mip chain is a whole index byte; its codebook entry's first texel
            const u32 p = vram16(book + vram8(index) * 8);
            out[0] = fmt == 3 ? yuv_to_rgba(p >> 8, p & 0xFF, 128) : texel(p);
            return;
        }
        twiddle_tables(w >> 1, h >> 1);
        if (fmt == 3)
        {
            // YUV422 entries: the 2x2 block's texels are (0,0) (0,1) (1,0) (1,1); each row's pair shares U and V
            for (u32 y = 0; y < h; y += 2)
                for (u32 x = 0; x < w; x += 2)
                {
                    const u32 e = book + vram8(index + (tw_x[x >> 1] | tw_y[y >> 1])) * 8;
                    for (u32 r = 0; r < 2; r++)
                    {
                        const u32 w0 = vram16(e + r * 2), w1 = vram16(e + 4 + r * 2);
                        out[(y + r) * pitch + x] = yuv_to_rgba(w0 >> 8, w0 & 0xFF, w1 & 0xFF);
                        out[(y + r) * pitch + x + 1] = yuv_to_rgba(w1 >> 8, w0 & 0xFF, w1 & 0xFF);
                    }
                }
            return;
        }
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++)
            {
                u32 k = vram8(index + (tw_x[x >> 1] | tw_y[y >> 1]));
                u32 t = ((x & 1) << 1) | (y & 1);
                out[y * pitch + x] = texel(vram16(book + (k * 4 + t) * 2));
            }
        return;
    }
    if (fmt == 5 || fmt == 6)
    {
        // palettised, always twiddled
        const u32 bits = fmt == 5 ? 4 : 8;
        if (mip)
            addr += other_mip_point[log2w] * bits / 8;
        const u32 pal = fmt == 5 ? tcw.PAL.PalSelect << 4 : (tcw.PAL.PalSelect >> 4) << 8;
        u32 colors[256];
        for (u32 i = 0; i < (bits == 4 ? 16u : 256u); i++)
            colors[i] = palette_entry(pal | i);
        twiddle_tables(w, h);
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++)
            {
                u32 t = tw_x[x] | tw_y[y], i;
                if (bits == 4)
                {
                    u32 b = vram8(addr + (t >> 1));
                    i = (t & 1) ? b >> 4 : b & 15;
                }
                else
                    i = vram8(addr + t);
                out[y * pitch + x] = colors[i];
            }
        return;
    }
    if (mip)
        addr += other_mip_point[log2w] * 2;
    if (fmt == 3)
    {
        // YUV422: U Y0 V Y1 over each horizontal pair of texels. Twiddled (Soulcalibur's fighters), the pair's
        // second texel is two along in twiddled order (x's bit 0 is twiddle bit 1), as Flycast decodes it.
        if (twiddled)
        {
            if (w == 1)
            {
                const u32 p = vram16(addr);
                out[0] = yuv_to_rgba(p >> 8, p & 0xFF, 128);
                return;
            }
            twiddle_tables(w, h);
            for (u32 y = 0; y < h; y++)
                for (u32 x = 0; x < w; x += 2)
                {
                    const u32 t = tw_x[x] | tw_y[y];
                    const u32 w0 = vram16(addr + t * 2), w1 = vram16(addr + (t + 2) * 2);
                    out[y * pitch + x] = yuv_to_rgba(w0 >> 8, w0 & 0xFF, w1 & 0xFF);
                    out[y * pitch + x + 1] = yuv_to_rgba(w1 >> 8, w0 & 0xFF, w1 & 0xFF);
                }
            return;
        }
        const u32 stride = tcw.NO_PAL.StrideSel ? (TEXT_CONTROL & 31) * 32 : w;
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x += 2)
            {
                u32 a = addr + (y * stride + x) * 2;
                u32 w0 = vram16(a), w1 = vram16(a + 2);
                int u = w0 & 0xFF, y0 = w0 >> 8, v = w1 & 0xFF, y1 = w1 >> 8;
                out[y * pitch + x] = yuv_to_rgba(y0, u, v);
                out[y * pitch + x + 1] = yuv_to_rgba(y1, u, v);
            }
        return;
    }
    if (twiddled)
    {
        twiddle_tables(w, h);
        for (u32 y = 0; y < h; y++)
        {
            const u32 ty = tw_y[y];
            u32 *row = &out[y * pitch];
            for (u32 x = 0; x < w; x++)
                row[x] = texel(vram16(addr + (tw_x[x] | ty) * 2));
        }
    }
    else
    {
        const u32 stride = tcw.NO_PAL.StrideSel ? (TEXT_CONTROL & 31) * 32 : w;
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++)
                out[y * pitch + x] = texel(vram16(addr + (y * stride + x) * 2));
    }
}

// the VRAM bytes a texture occupies (whole mip chain and VQ codebook included) and, if palettised, its palette;
// unless full, a texture over 1 KB is sampled: its first and last 32 words and 128 spread in between
static u32 checksum(TCW tcw, TSP tsp, u32 w, u32 h, bool full)
{
    const u32 fmt = tcw.NO_PAL.PixelFmt;
    const bool vq = tcw.NO_PAL.VQ_Comp, mip = tcw.NO_PAL.MipMapped;
    const u32 addr = (tcw.NO_PAL.TexAddr << 3) & VRAM_MASK;
    const u32 bits = vq ? 2 : fmt == 5 ? 4 : fmt == 6 ? 8 : 16;   // VQ: one index byte per 4 texels
    u32 bytes = w * h * bits / 8;
    if (mip)
        bytes = bytes * 4 / 3 + 8;
    if (vq)
        bytes += 2048;
    if (tcw.NO_PAL.ScanOrder && fmt < 5 && tcw.NO_PAL.StrideSel)
        bytes = (TEXT_CONTROL & 31) * 32 * h * 2;   // linear with a stride wider than the texture
    u32 sum = 0x811C9DC5;
    const u32 *p = (const u32 *)&PVRPARAMS.vram[addr];
    const u32 words = bytes / 4, limit = (VRAM_MASK + 1 - addr) / 4;
    const u32 n = words < limit ? words : limit;
    if (full || n <= 256)
    {
        for (u32 i = 0; i < n; i++)
            sum = (sum ^ p[i]) * 0x01000193;
    }
    else
    {
        for (u32 i = 0; i < 32; i++)
            sum = (sum ^ p[i] ^ p[n - 1 - i]) * 0x01000193;
        const u32 step = n / 128;
        for (u32 i = 0; i < 128; i++)
            sum = (sum ^ p[i * step + step / 2]) * 0x01000193;
    }
    if (fmt == 5 || fmt == 6)
    {
        const u32 first = fmt == 5 ? tcw.PAL.PalSelect << 4 : (tcw.PAL.PalSelect >> 4) << 8, count = fmt == 5 ? 16 : 256;
        sum = (sum ^ PAL_RAM_CTRL) * 0x01000193;
        for (u32 i = 0; i < count; i++)
            sum = (sum ^ PALETTE_RAM[(first + i) & 1023]) * 0x01000193;
    }
    return sum;
}

struct TexEntry
{
    u32 tcw, size, sampled_sum, full_sum;
    u32 decoded_seq, used_seq;
    bool fresh;   // created but not decoded yet
    s16 next;     // next entry in the same hash bucket, -1 at the end
    u32 bytes;    // of the surface, mip chain included
    GX2Texture tex;
    bool valid;
    // Up to three scenes can be in flight (drawing, ready, being built), and a game may change a texture every
    // frame (MvC2's character sprites): a change is decoded into the next of three images, so a scene in flight
    // keeps the one its descriptor copy points at. Allocated on the first change.
    void *img[4];
    u8 cur;
    u32 scene_mark;   // the scene whose texture table has this entry, at scene_idx
    u16 scene_idx;
    bool linked;      // in a hash bucket
    bool fresh_decode;   // the current image was never decoded: decode into it rather than the next
};

// Surfaces of replaced or evicted entries, freed once no scene in flight can draw with them: a surface dropped
// while scene `tag` was being built may be used by scenes before it (wiiu_textures_collect)
struct DeferredFree
{
    void *p;
    u32 tag;
};
static std::vector<DeferredFree> deferred_frees;
static u32 building_scene;   // the scene being built, for the tags

static const int CACHE_SIZE = 512, BUCKETS = 256;
static s16 buckets[BUCKETS];
static bool buckets_ready;
static inline u32 bucket_of(u32 tcw, u32 size) { return ((tcw * 0x9E3779B1u) ^ size) >> 24; }
u32 texture_decodes;
int wiiu_tex_full_hash;   // tex_full_hash=1: every texture fully checksummed every frame (test: stale tiles?)
OSTime texture_decode_ticks;   // spent decoding and checksumming, for the GX2 thread's time report
static TexEntry cache[CACHE_SIZE];

static void create(TexEntry &e, u32 w, u32 h, u32 levels)
{
    memset(&e.tex, 0, sizeof(e.tex));
    e.tex.surface.width = w;
    e.tex.surface.height = h;
    e.tex.surface.depth = 1;
    e.tex.surface.mipLevels = levels;
    e.tex.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    e.tex.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    e.tex.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    e.tex.viewNumSlices = 1;
    e.tex.viewNumMips = levels;
    e.tex.compMap = 0x00010203;
    GX2CalcSurfaceSizeAndAlignment(&e.tex.surface);
    GX2InitTextureRegs(&e.tex);
    // plain memory, written from either core: the mip chain follows the base level at mipLevelOffset[0]
    GX2Surface &sf = e.tex.surface;
    e.bytes = levels > 1 ? sf.mipLevelOffset[0] + sf.mipmapSize : sf.imageSize;
    sf.image = MEMAllocFromDefaultHeapEx(e.bytes, sf.alignment);
    sf.mipmaps = levels > 1 && sf.image ? (u8 *)sf.image + sf.mipLevelOffset[0] : nullptr;
    e.valid = sf.image != nullptr;
    e.img[0] = sf.image;
    e.img[1] = e.img[2] = e.img[3] = nullptr;
    e.cur = 0;
    e.fresh_decode = true;
    e.scene_mark = ~0u;
    // an entry without memory is drawn untextured (white): say so, a few times
    static u32 failures;
    if (!e.valid && failures++ < 8)
        printf("[tex] no memory for a %ux%u texture (%u levels, %u bytes), %u failures so far\n", w, h, levels, e.bytes,
               failures);
}

static void destroy(TexEntry &e)
{
    for (void *&p : e.img)
        if (p)
        {
            deferred_frees.push_back({ p, building_scene });
            p = nullptr;
        }
    e.tex.surface.image = e.tex.surface.mipmaps = nullptr;
    e.valid = false;
}

// free what no scene in flight can use: oldest is the oldest scene still ready or drawing (~0u: none)
void wiiu_textures_collect(u32 oldest)
{
    size_t k = 0;
    for (const DeferredFree &d : deferred_frees)
        if (d.tag <= oldest)
            MEMFreeToDefaultHeap(d.p);
        else
            deferred_frees[k++] = d;
    deferred_frees.resize(k);
}

// the cache entry for a TCW/TSP, created (not decoded) if there is none
static TexEntry *resolve(u32 tcw_full, u32 tsp_full, u32 frame_seq)
{
    TCW tcw; tcw.full = tcw_full;
    TSP tsp; tsp.full = tsp_full;
    const u32 w = 8 << tsp.TexU, h = 8 << tsp.TexV, size = (tsp.TexU << 3) | tsp.TexV;
    // a mipmapped texture is square and carries every level down to 1x1
    const u32 levels = tcw.NO_PAL.MipMapped ? tsp.TexU + 4 : 1;
    // consecutive strips often use the same texture
    static TexEntry *last;
    if (last && last->valid && last->tcw == tcw_full && last->size == size)
    {
        last->used_seq = frame_seq;
        return last;
    }
    if (!buckets_ready)
    {
        for (int i = 0; i < BUCKETS; i++)
            buckets[i] = -1;
        buckets_ready = true;
    }
    const u32 b = bucket_of(tcw_full, size);
    TexEntry *e = nullptr;
    for (s16 i = buckets[b]; i >= 0; i = cache[i].next)
        if (cache[i].tcw == tcw_full && cache[i].size == size)
        {
            e = &cache[i];
            break;
        }
    if (!e)
    {
        // a free entry, else the least recently used one not used by this frame (its descriptor may already be in
        // the scene's table), unlinked from its bucket; none: the strip goes untextured
        e = nullptr;
        for (int i = 0; i < CACHE_SIZE; i++)
        {
            if (!cache[i].valid && !cache[i].linked)
            {
                e = &cache[i];
                break;
            }
            if (cache[i].used_seq != frame_seq && (!e || cache[i].used_seq < e->used_seq))
                e = &cache[i];
        }
        if (!e)
            return nullptr;
        if (e->linked)
        {
            for (s16 *link = &buckets[bucket_of(e->tcw, e->size)]; *link >= 0; link = &cache[*link].next)
                if (&cache[*link] == e)
                {
                    *link = e->next;
                    break;
                }
            e->linked = false;
        }
        // always a new surface: one a scene in flight still draws with is freed later (deferred_frees)
        if (e->valid)
            destroy(*e);
        create(*e, w, h, levels);
        e->linked = true;
        e->tcw = tcw_full;
        e->size = size;
        e->next = buckets[b];
        buckets[b] = (s16)(e - cache);
        e->decoded_seq = frame_seq - 1;
        e->fresh = true;
    }
    e->used_seq = frame_seq;
    last = e;
    return e;
}

// check the entry against VRAM once per frame and decode it if it changed; true if decoded (the CPU's copy is then
// flushed to memory, and the GPU's texture cache must be invalidated on the GX2 thread before it's drawn)
static bool update(TexEntry *e, u32 frame_seq)
{
    if (e->decoded_seq == frame_seq || !e->valid)
        return false;
    const OSTime t0 = OSGetSystemTime();
    TCW tcw; tcw.full = e->tcw;
    const u32 w = e->tex.surface.width, h = e->tex.surface.height, levels = e->tex.surface.mipLevels;
    TSP tsp; tsp.full = 0;
    tsp.TexU = e->size >> 3;
    tsp.TexV = e->size & 7;
    // the sampled sum every frame; the full one when that changed, on fresh entries, and on every 16th frame
    // (staggered across entries) to catch writes the samples miss
    const u32 sampled = checksum(tcw, tsp, w, h, wiiu_tex_full_hash != 0);
    bool changed = e->fresh || sampled != e->sampled_sum;
    u32 full = e->full_sum;
    if (changed || ((frame_seq + (u32)(e - cache)) & 15) == 0)
    {
        full = checksum(tcw, tsp, w, h, true);
        changed |= full != e->full_sum;
    }
    e->sampled_sum = sampled;
    e->full_sum = full;
    e->fresh = false;
    if (changed)
    {
        GX2Surface &sf = e->tex.surface;
        if (!e->fresh_decode)
        {
            // a scene in flight may draw with the current image: decode into the next one
            const u8 next = (e->cur + 1) % 4;   // a scene building, two waiting, one drawn (exp_frame_queue)
            if (!e->img[next])
                e->img[next] = MEMAllocFromDefaultHeapEx(e->bytes, sf.alignment);
            if (e->img[next])
            {
                e->cur = next;
                sf.image = e->img[next];
                sf.mipmaps = levels > 1 ? (u8 *)sf.image + sf.mipLevelOffset[0] : nullptr;
            }
        }
        e->fresh_decode = false;
        decode(tcw, (u32 *)sf.image, sf.pitch, w, h);
        for (u32 l = 1; l < levels; l++)
        {
            // level 1 starts at mipmaps, later ones at mipLevelOffset[l - 1] from it; linear rows are 64-texel aligned
            u8 *base = (u8 *)sf.mipmaps + (l == 1 ? 0 : sf.mipLevelOffset[l - 1]);
            const u32 lw = w >> l;
            decode(tcw, (u32 *)base, (lw + 63) & ~63u, lw, lw);
        }
        DCFlushRange(sf.image, e->bytes);
        texture_decodes++;
    }
    e->decoded_seq = frame_seq;
    texture_decode_ticks += OSGetSystemTime() - t0;
    {
        static u32 cores;   // which threads decode, once each, for the fight profile
        if (!(cores >> OSGetCoreId() & 1))
        {
            cores |= 1u << OSGetCoreId();
            printf("[tex] decoding on core %u\n", OSGetCoreId());
        }
    }
    return changed;
}

// Building a scene (the intake thread, or the GX2 thread for render to texture, one at a time): each textured
// strip gets an index into the scene's own table of texture descriptors, copied in on first use and decoded or
// refreshed for this scene, so drawing never reads the cache another thread may be changing. The images that
// were decoded go into inv: the GX2 thread invalidates the GPU's copies before drawing the scene.
void wiiu_textures_begin(u32 scene_seq) { building_scene = scene_seq; }

int wiiu_texture_scene(u32 tcw_full, u32 tsp_full, u32 scene_seq, GX2Texture *table, u32 *n, u32 max,
                       WiiuTexInval *inv, u32 *n_inv, u32 inv_max)
{
    TexEntry *e = resolve(tcw_full, tsp_full, scene_seq);
    if (!e || !e->valid)
        return -1;
    if (update(e, scene_seq) && *n_inv < inv_max)
        inv[(*n_inv)++] = { e->tex.surface.image, e->bytes };
    if (e->scene_mark != scene_seq)
    {
        if (*n >= max)
            return -1;
        table[*n] = e->tex;
        e->scene_mark = scene_seq;
        e->scene_idx = (u16)*n;
        (*n)++;
    }
    return e->scene_idx;
}

// samplers for every clamp/flip/filter combination of a TSP word. Point and bilinear filtering use the nearest
// mip level, trilinear blends two, as on the PVR
GX2Sampler *wiiu_sampler(u32 tsp_full)
{
    static GX2Sampler samplers[64];
    static bool ready;
    if (!ready)
    {
        for (int i = 0; i < 64; i++)
        {
            GX2TexClampMode cu = (i & 1) ? GX2_TEX_CLAMP_MODE_CLAMP : (i & 4) ? GX2_TEX_CLAMP_MODE_MIRROR : GX2_TEX_CLAMP_MODE_WRAP;
            GX2TexClampMode cv = (i & 2) ? GX2_TEX_CLAMP_MODE_CLAMP : (i & 8) ? GX2_TEX_CLAMP_MODE_MIRROR : GX2_TEX_CLAMP_MODE_WRAP;
            GX2InitSampler(&samplers[i], GX2_TEX_CLAMP_MODE_WRAP, (i & 16) ? GX2_TEX_XY_FILTER_MODE_LINEAR : GX2_TEX_XY_FILTER_MODE_POINT);
            GX2InitSamplerClamping(&samplers[i], cu, cv, GX2_TEX_CLAMP_MODE_WRAP);
            GX2InitSamplerZMFilter(&samplers[i], GX2_TEX_Z_FILTER_MODE_POINT,
                                   (i & 32) ? GX2_TEX_MIP_FILTER_MODE_LINEAR : GX2_TEX_MIP_FILTER_MODE_POINT);
        }
        ready = true;
    }
    TSP tsp; tsp.full = tsp_full;
    return &samplers[tsp.ClampU | (tsp.ClampV << 1) | (tsp.FlipU << 2) | (tsp.FlipV << 3) | ((tsp.FilterMode != 0) << 4) | ((tsp.FilterMode >= 2) << 5)];
}
