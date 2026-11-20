// Video output on the Wii U. Threads (main_wiiu.cpp has the whole map): the SH4 JIT runs on the codegen core (1 on a
// console) and everything here keeps off it. The GX2 thread owns GX2 (wiiu_gx2_*; on core 0 with the "graphics core"
// option, else the main thread on the codegen core). The intake thread on core 2 turns each published frame into a
// scene (TA decode, vertex conversion with a helper on core 0, textures); the GX2 thread only draws scenes. In Cemu the
// codegen core is 0, so cores 0 and 2 swap.
// The GX2 thread renders the latest 3D frame the emulator thread published (wiiu_frame_publish, built from the TA by
// render_gx2.cpp) once, into a 960x720 off-screen
// target, and shows that, 4:3 pillarboxed, on the TV and the GamePad; when the game hasn't rendered in 3D for a
// second it shows the scanned-out framebuffer (wiiu_fb_capture) instead. Textures come from texture_gx2.cpp.
// Modifier volumes from the opaque modifier list shade the opaque and punch-through polygons that ask for it
// (through the stencil buffer); translucent modifier volumes and two-parameter-set polygons are not done.
#include <coreinit/cache.h>
#include <coreinit/codegen.h>
#include <coreinit/event.h>
#include <coreinit/fastmutex.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <coreinit/memdefaultheap.h>
#include <gx2/clear.h>
#include <gx2/context.h>
#include <gx2/mem.h>
#include <gx2/surface.h>
#include <gx2/enum.h>
#include <gx2/draw.h>
#include <gx2/event.h>
#include <gx2/state.h>
#include <gx2/registers.h>
#include <gx2/sampler.h>
#include <gx2/shaders.h>
#include <gx2/texture.h>
#include <gx2/utils.h>
#include <gx2r/buffer.h>
#include <gx2r/draw.h>
#include <gx2/displaylist.h>
#include <gx2r/surface.h>
#include <whb/gfx.h>
#include <string.h>
#include <algorithm>
#include <vector>
#include "types.h"
#include "plugs/drkPvr/drkPvr.h"
#include "plugs/drkPvr/regs.h"
#include "plugs/drkPvr/ta_structs.h"
#include "wiiu_frame.h"

extern const u8 blit_gsh[], geo_gsh[];
GX2Sampler *wiiu_sampler(u32 tsp);
extern u32 texture_decodes;
extern OSTime texture_decode_ticks;
extern u32 wiiu_vblanks, wiiu_renders, wiiu_dropped_vertices, wiiu_dropped_strips, wiiu_dropped_mv_tris;

static const u32 FB_MAX_W = 640, FB_MAX_H = 480;
static u32 shared_rgba[FB_MAX_W * FB_MAX_H];
static u32 shared_w, shared_h;
static volatile u32 shared_seq;
static OSFastMutex shared_lock;
static bool lock_ready;

// ---- emulator thread ----------------------------------------------------------------------------------

static inline u32 rgba(u32 r, u32 g, u32 b) { return (r << 24) | (g << 16) | (b << 8) | 0xFF; }

// The framebuffer as of the last vblank. Converting the scanned-out picture there cost the emulator thread
// 640x480 pixels 60 times a second (most of a fight's 40% "timeslice" time on a console), though it's only shown
// when the game hasn't rendered in 3D for a second; converting it later from live VRAM tore the title screens.
// So while the GX2 thread shows the framebuffer (fb_wanted), the emulator copies the bytes it covers at each
// vblank into a VRAM-shaped snapshot (a memcpy, ~1 ms), and the worker converts from that.
struct FbRegs
{
    u32 size, depth, sof1, sof2, interlace, line_double, pixel_double;
};
static FbRegs fb_regs;
static volatile u32 fb_regs_seq;   // 0 until the emulator shows a framebuffer (the picker's image stays up)
static volatile bool fb_wanted;
static u8 *vram_snap;              // VRAM_SIZE bytes, same layout as PVRPARAMS.vram
static OSFastMutex snap_lock;      // the emulator skips a vblank's copy while the worker reads the snapshot

static void fb_geometry(const FbRegs &r, u32 &bpp, u32 &w, u32 &h, u32 &stride)
{
    const u32 x_words = (r.size & 0x3FF) + 1, modulus = (r.size >> 20) & 0x3FF;
    h = ((r.size >> 10) & 0x3FF) + 1;
    bpp = r.depth == 2 ? 3 : r.depth == 3 ? 4 : 2;   // 0 RGB555, 1 RGB565, 2 RGB888 packed, 3 RGB0888
    w = x_words * 4 / bpp;
    stride = (x_words + (modulus ? modulus - 1 : 0)) * 4;
}

// copy the 32-bit-addressed VRAM bytes [a, e) into the snapshot: within each 4 MB half the 64-bit layout keeps
// them in order, two to every eight bytes, so each half is one span
static void snap_range(u32 a, u32 e)
{
    while (a < e)
    {
        const u32 half_end = (a & ~0x3FFFFFu) + 0x400000, stop = e < half_end ? e : half_end;
        const u32 lo = vram_offset64(a) & ~7u, hi = (vram_offset64(stop - 1) & ~7u) + 8;
        memcpy(vram_snap + lo, PVRPARAMS.vram + lo, hi - lo);
        a = stop;
    }
}

void wiiu_fb_capture()
{
    if (!lock_ready || !FB_R_CTRL.fb_enable)
        return;
    FbRegs r = { FB_R_SIZE, FB_R_CTRL.fb_depth, FB_R_SOF1, FB_R_SOF2, SPG_CONTROL.interlace, FB_R_CTRL.fb_line_double,
                 (VO_CONTROL >> 8) & 1 };
    if (fb_wanted && vram_snap && OSFastMutex_TryLock(&snap_lock))
    {
        u32 bpp, w, h, stride;
        fb_geometry(r, bpp, w, h, stride);
        const u32 len = stride * (h < 480 ? h : 480);
        for (u32 sof : { r.sof1, r.interlace ? r.sof2 : r.sof1 })
            if ((sof & VRAM_MASK) + len <= VRAM_SIZE)
                snap_range(sof & VRAM_MASK, (sof & VRAM_MASK) + len);
        fb_regs = r;
        fb_regs_seq = fb_regs_seq + 1;
        OSFastMutex_Unlock(&snap_lock);
    }
    else if (!fb_regs_seq)
    {
        fb_regs = r;
        fb_regs_seq = 1;   // the GX2 thread then asks for copies
    }
}

// worker: the snapshotted framebuffer into shared_rgba
static void convert_fb(void *)
{
    OSFastMutex_Lock(&snap_lock);
    const FbRegs r = fb_regs;
    u32 bpp, w, h, stride;
    fb_geometry(r, bpp, w, h, stride);
    const u32 depth = r.depth;
    const u8 *vram = vram_snap ? vram_snap : PVRPARAMS.vram;
    // FB_R_SIZE gives one field: interlaced modes weave the second (FB_R_SOF2) between its lines, and the
    // line- and pixel-doubled modes (240p, 320 wide) show each line or pixel twice
    // (judged by size: fbtest sets pixel double on a 640-wide framebuffer)
    const u32 ys = r.interlace || h <= 288 ? 2 : 1, xs = w <= 320 ? 2 : 1;
    const u32 cw = w * xs < FB_MAX_W ? w * xs : FB_MAX_W, ch = h * ys < FB_MAX_H ? h * ys : FB_MAX_H;
    static u32 logged;
    if (logged != (w | h << 10 | ys << 20 | xs << 22 | r.interlace << 24 | depth << 25))
    {
        logged = w | h << 10 | ys << 20 | xs << 22 | r.interlace << 24 | depth << 25;
        printf("[fb] %ux%u depth %u, interlace %u, line double %u, pixel double %u: shown %ux%u\n", w, h, depth,
               r.interlace, r.line_double, r.pixel_double, cw, ch);
    }

    OSFastMutex_Lock(&shared_lock);
    for (u32 y = 0; y < ch; y++)
    {
        const u32 line = ((r.interlace && (y & 1) ? r.sof2 : r.sof1) & 0x00FFFFFF) + y / ys * stride;
        u32 *out = &shared_rgba[y * FB_MAX_W];
        for (u32 x = 0; x < cw; x++)
        {
            u32 a = line + x / xs * bpp;
            if (bpp == 2)
            {
                u32 px = *host_ptr_xor((u16 *)&vram[vram_offset64(a)]);
                out[x] = depth == 1 ? rgba((px >> 11) << 3, ((px >> 5) & 0x3F) << 2, (px & 0x1F) << 3)
                                    : rgba(((px >> 10) & 0x1F) << 3, ((px >> 5) & 0x1F) << 3, (px & 0x1F) << 3);
            }
            else
            {
                u32 b = *host_ptr_xor((u8 *)&vram[vram_offset64(a)]);
                u32 g = *host_ptr_xor((u8 *)&vram[vram_offset64(a + 1)]);
                u32 r = *host_ptr_xor((u8 *)&vram[vram_offset64(a + 2)]);
                out[x] = rgba(r, g, b);
            }
        }
    }
    // the whole texture is shown: blank what this picture doesn't cover, not the last one's leftovers
    for (u32 y = 0; y < FB_MAX_H; y++)
        if (y >= ch)
            memset(&shared_rgba[y * FB_MAX_W], 0, FB_MAX_W * 4);
        else if (cw < FB_MAX_W)
            memset(&shared_rgba[y * FB_MAX_W + cw], 0, (FB_MAX_W - cw) * 4);
    shared_w = cw;
    shared_h = ch;
    shared_seq = shared_seq + 1;
    OSFastMutex_Unlock(&shared_lock);
    OSFastMutex_Unlock(&snap_lock);
}

// 3D frames: the emulator publishes a finished WiiuFrame, the GX2 thread copies it out under the same lock. The
// newest screen frame replaces an older one nobody drew; render-to-texture frames must all be drawn, so they
// have their own slot and the emulator waits (up to 50 ms) for the GX2 thread to take the previous one.
static WiiuFrame *pending_frame, *pending_rtt;
static const WiiuFrame *intake_using;   // the frame the intake thread is building a scene from, outside the lock
static volatile u32 frame_seq;
static OSFastMutex frame_lock;
static OSEvent intake_go;             // a screen frame was published

WiiuFrame *wiiu_frame_spare(WiiuFrame *frames, const WiiuFrame *a, const WiiuFrame *b)
{
    if (!lock_ready)
        return nullptr;
    OSFastMutex_Lock(&frame_lock);
    WiiuFrame *next = nullptr;
    for (u32 i = 0; i < WIIU_FRAMES && !next; i++)
        if (&frames[i] != pending_frame && &frames[i] != pending_rtt && &frames[i] != intake_using && &frames[i] != a &&
            &frames[i] != b)
            next = &frames[i];
    OSFastMutex_Unlock(&frame_lock);
    return next;
}

WiiuFrame *wiiu_frame_publish(WiiuFrame *done, WiiuFrame *frames, const WiiuFrame *keep)
{
    if (!lock_ready)
        return done;
    OSFastMutex_Lock(&frame_lock);
    if (done->rtt)
    {
        for (int i = 0; i < 50 && pending_rtt; i++)
        {
            OSFastMutex_Unlock(&frame_lock);
            OSSleepTicks(OSMillisecondsToTicks(1));
            OSFastMutex_Lock(&frame_lock);
        }
        pending_rtt = done;
    }
    else
    {
        pending_frame = done;
        frame_seq = frame_seq + 1;
    }
    WiiuFrame *next = nullptr;
    for (u32 i = 0; i < WIIU_FRAMES && !next; i++)
        if (&frames[i] != pending_frame && &frames[i] != pending_rtt && &frames[i] != intake_using && &frames[i] != keep)
            next = &frames[i];
    OSFastMutex_Unlock(&frame_lock);
    if (!done->rtt)
        OSSignalEvent(&intake_go);   // build its scene now, while its textures are what the game rendered with
    next->nv = next->ns = next->n_mv_tris = next->n_mp = 0;
    next->max_invw = 0.0f;
    return next;
}

// ---- GX2 thread -----------------------------------------------------------------------------------------

// the picker's image, sharper than the game framebuffer path allows; it only exists while the picker is up
static GX2Texture ui_tex;
static bool ui_active;
static bool ui_overlay;   // the HOME menu: ui_tex blended over the game's last picture instead of in its place
void wiiu_gx2_show_ui(const u32 *px, u32 w, u32 h)
{
    if (!ui_tex.surface.image)
    {
        ui_tex.surface.width = w;
        ui_tex.surface.height = h;
        ui_tex.surface.depth = 1;
        ui_tex.surface.mipLevels = 1;
        ui_tex.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
        ui_tex.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
        ui_tex.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
        ui_tex.viewNumSlices = 1;
        ui_tex.compMap = 0x00010203;
        GX2CalcSurfaceSizeAndAlignment(&ui_tex.surface);
        GX2InitTextureRegs(&ui_tex);
        if (!GX2RCreateSurface(&ui_tex.surface, GX2R_RESOURCE_BIND_TEXTURE | GX2R_RESOURCE_USAGE_CPU_WRITE | GX2R_RESOURCE_USAGE_GPU_READ))
            return;
    }
    u32 *dst = (u32 *)GX2RLockSurfaceEx(&ui_tex.surface, 0, (GX2RResourceFlags)0);
    for (u32 y = 0; y < h; y++)
        memcpy(&dst[y * ui_tex.surface.pitch], &px[y * w], w * 4);
    GX2RUnlockSurfaceEx(&ui_tex.surface, 0, (GX2RResourceFlags)0);
    ui_active = true;
}
void wiiu_gx2_show_menu(const u32 *px, u32 w, u32 h)
{
    wiiu_gx2_show_ui(px, w, h);
    ui_overlay = true;
}
void wiiu_gx2_hide_ui()
{
    ui_overlay = false;
    if (!ui_tex.surface.image)
        return;
    ui_active = false;
    GX2DrawDone();
    GX2RDestroySurfaceEx(&ui_tex.surface, (GX2RResourceFlags)0);
    ui_tex = {};
}

static WHBGfxShaderGroup group, geo;
struct GpuVertex { float x, y, z, w; u32 col, spc; float u, v; };
// four frame slots (vertices, indices, scene): one the GPU draws, up to two ready, one the intake thread fills
static const int SLOTS = 4;
static GX2RBuffer geo_buf[SLOTS], rtt_buf;
// each strip as an indexed triangle list (convert_frame), so draw_scene can send a run of strips with the same
// state as one draw (Soulcalibur has ~3000 strips a frame: one draw each is too many for 60 Hz)
static const u32 IDX_MAX = WIIU_MAX_VERTICES * 3;
static GX2RBuffer idx_buf[SLOTS], rtt_idx_buf;
static struct GpuVertex *slot_v[SLOTS];
static u32 *slot_i[SLOTS];
static u32 strip_idx[WIIU_MAX_STRIPS];   // a strip's first index; it has (count - 2) * 3
static u16 draw_strip_tex[WIIU_MAX_STRIPS];   // the committed scene's strip -> texture table index
static GX2Texture *draw_tex;                  // and its table (inside the scene, which isn't rewritten while drawn)
static GX2Texture white_tex;                  // for a textured strip whose texture didn't fit (drawn white)
static u32 draw_calls, draw_strips;      // for the perf report: how well strips batch
enum { BRK_CLIP, BRK_TEX, BRK_SMP, BRK_DEPTH, BRK_CULL, BRK_BLEND, BRK_MODE, BRK_SHADE, BRK_CLAMP, BRK_GAP, BRK_COUNT };
static u32 run_breaks[BRK_COUNT];        // what ended each run of strips
static u32 strip_idx_owner;              // which index buffer strip_idx[] describes (its address), and how many
static u32 strip_idx_count;              // indices convert_frame wrote there: draws never reach past them
static bool init_rtt_target();
static void start_worker();
static void start_intake();
static void start_conv_helper();
static void init_white_texture();
static int geo_cur;
static WiiuStrip strips[WIIU_MAX_STRIPS];
// modifier volumes of the current frame: their triangles start at mv_base in the vertex buffer, followed by a
// full-screen quad at quad_base for the stencil passes
static WiiuModVol modvols[WIIU_MAX_MV_PARAMS];
static u32 modvol_count, mv_base, quad_base, shad_scale;
// tr_depth (DOA2's character select, 2026-11-18; on, console tr1; tr_depth=0 turns it off): auto-sorted translucent polygons
// (pre-sort mode off, the PVR's default) are drawn with depth >= and no depth writes, as Flycast does
// (rend/gles/gldraw.cpp SetGPState). With their own Z-write bit, a translucent quad's transparent margins wrote depth
// and hid what came after at the same or lesser depth: DOA2's stacked mode tiles, all at one depth, kept only the
// first one drawn, and the fighter's name lost its bottom edge to the portrait tiles' margins. Pre-sorted frames keep
// the polygons' own depth mode and Z-write bit (Flycast does the same).
// 2 (the default since 2026-11-20): depth >= as in 1, but each polygon writes depth by its own Z-write bit, and fully
// transparent pixels of plainly blended polygons (source alpha, inverse source alpha) are discarded first, so their
// margins write no depth. Without depth writes (1), the per-strip sort drew Shenmue's overlapping translucent head
// strips in the wrong order: pieces of brow and mouth over Ryo's face. Sorting per triangle, as Flycast does, wasn't
// enough either (stray shards); depth writes stand in for the PVR's per-pixel sort. 1: no depth writes, 0: the
// polygons' own depth mode and Z-write bit (U1)
int wiiu_exp_tr_depth = 2;
static bool trans_autosort;
// mv_bbox (Review 0, on by default since Review 1; mv_bbox=0 turns it off): each closing volume's screen box in canvas pixels (x0, y0, x1, y1; x0 = -1e30: whole screen), so
// its stencil resolve and clear quads fill only the box instead of the whole target
int wiiu_mv_bbox = 1;
int wiiu_mv_bbox_check;   // mv_bbox_check=1 (instrumented): every 30th frame with volumes, drawn with and without boxes, compared
static float mv_box[WIIU_MAX_MV_PARAMS][4];
static u32 strip_count, drawn_frame_seq, last_3d_vblank;
// translucent strips in drawing order: as submitted when the game pre-sorts them, else far to near
static u32 trans_order[WIIU_MAX_STRIPS], trans_count;
static float trans_key[WIIU_MAX_STRIPS];
static bool have_3d;
static float canvas_w = 640.0f, canvas_h = 480.0f;   // of the screen frame being drawn
// a new screen frame was converted and isn't in the render target yet; without one the target keeps the last
// picture and only the blits run
static bool scene_dirty;
static u32 u_screen_vs_loc = ~0u, u_screen_ps_loc = ~0u;
static float draw_screen[4];        // the committed scene's u_screen (the geo shaders' mode word; 0: clip-space positions)
static inline u32 vertex_bytes() { return sizeof(GpuVertex); }
static u32 u_mode_loc, u_shade_loc, u_fog_col_loc, u_fog_vcol_loc, u_clamp_min_loc, u_clamp_max_loc, u_clamp_on_loc, tex0_loc,
    fog_table_loc;

// lookup-table fog: the frame's FOG_TABLE as a 128x2 texture (see geo.frag), its colours and density
static GX2Texture fog_tex;
static GX2Sampler fog_sampler;
static u16 fog_table[128], next_fog_table[128];   // in the texture, and of the frame converted last
static u32 fog_col_ram, fog_col_vert, fog_clamp_min, fog_clamp_max;
static float fog_density;

// What convert_frame makes of a frame, besides the vertices and indices in the GX2 buffers. The intake thread converts
// the next screen frame into one of these while the GX2 thread draws the last one; commit() copies a finished one
// into the statics above, which draw_scene uses. Render-to-texture frames have their own, so they don't disturb the
// screen frame's.
struct Scene
{
    WiiuStrip strips[WIIU_MAX_STRIPS];
    u32 strip_idx[WIIU_MAX_STRIPS], strip_count, idx_owner, idx_count;
    WiiuModVol modvols[WIIU_MAX_MV_PARAMS];
    u32 modvol_count, mv_base, quad_base, shad_scale;
    float mv_box[WIIU_MAX_MV_PARAMS][4];
    float screen[4];   // the geo shaders' u_screen for this scene
    u32 trans_order[WIIU_MAX_STRIPS], trans_count;
    bool trans_autosort;   // the PVR sorts translucent polygons itself (pre-sort mode off)
    u16 fog_table[128];
    u32 fog_col_ram, fog_col_vert, fog_clamp_min, fog_clamp_max;
    float fog_density;
    // each textured strip's texture: an index into tex, copies of the descriptors as built (0xFFFF: none)
    u16 strip_tex[WIIU_MAX_STRIPS];
    GX2Texture tex[1024];
    u32 n_tex;
    WiiuTexInval inv[1024];   // images decoded for this scene: their GPU copies are invalidated before drawing
    u32 n_inv;
    bool inv_all;             // inv overflowed: invalidate the whole texture cache instead
    u32 seq, frame_seq;       // scene number (texture versions), and the emulator frame it was built from
    float canvas_w, canvas_h;
};
static Scene scenes[SLOTS], rtt_scene;
static const Scene *committed;       // what the statics hold now

static void init_fog_texture()
{
    fog_tex.surface.width = 128;
    fog_tex.surface.height = 2;
    fog_tex.surface.depth = 1;
    fog_tex.surface.mipLevels = 1;
    fog_tex.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    fog_tex.surface.format = GX2_SURFACE_FORMAT_UNORM_R8;
    fog_tex.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    fog_tex.viewNumSlices = 1;
    fog_tex.compMap = 0x00010203;
    GX2CalcSurfaceSizeAndAlignment(&fog_tex.surface);
    GX2InitTextureRegs(&fog_tex);
    GX2RCreateSurface(&fog_tex.surface, (GX2RResourceFlags)(GX2R_RESOURCE_BIND_TEXTURE | GX2R_RESOURCE_USAGE_CPU_WRITE |
                                                            GX2R_RESOURCE_USAGE_GPU_READ));
    GX2InitSampler(&fog_sampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_POINT);
}

static void update_fog_texture(const u16 *table)
{
    if (!memcmp(table, fog_table, sizeof(fog_table)))
        return;
    memcpy(fog_table, table, sizeof(fog_table));
    u8 *px = (u8 *)GX2RLockSurfaceEx(&fog_tex.surface, 0, (GX2RResourceFlags)0);
    for (u32 i = 0; i < 128; i++)
    {
        px[i] = table[i] & 0xFF;
        px[fog_tex.surface.pitch + i] = table[i] >> 8;
    }
    GX2RUnlockSurfaceEx(&fog_tex.surface, 0, (GX2RResourceFlags)0);
}

// off-screen render target for the 3D frame: 1.5x the Dreamcast's 640x480, or rt_width= in wiiu.cfg (640 halves
// the GPU's pixels)
int wiiu_rt_width = 960;
static u32 RT_W = 960, RT_H = 720;
static GX2ColorBuffer rt_color;
static GX2DepthBuffer rt_depth;
static GX2Texture rt_tex;
static GX2ContextState *rt_ctx;

static bool alloc_render_target()
{
    RT_W = wiiu_rt_width < 320 ? 320 : wiiu_rt_width > 1920 ? 1920 : (u32)wiiu_rt_width;
    RT_H = RT_W * 3 / 4;
    printf("[gx2] 3D render target %ux%u\n", RT_W, RT_H);
    rt_color.surface.width = RT_W;
    rt_color.surface.height = RT_H;
    rt_color.surface.depth = 1;
    rt_color.surface.mipLevels = 1;
    rt_color.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    rt_color.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    rt_color.surface.use = (GX2SurfaceUse)(GX2_SURFACE_USE_COLOR_BUFFER | GX2_SURFACE_USE_TEXTURE);
    rt_color.surface.tileMode = GX2_TILE_MODE_DEFAULT;
    rt_color.viewNumSlices = 1;
    GX2CalcSurfaceSizeAndAlignment(&rt_color.surface);
    GX2InitColorBufferRegs(&rt_color);
    rt_color.surface.image = MEMAllocFromDefaultHeapEx(rt_color.surface.imageSize, rt_color.surface.alignment);

    rt_depth.surface.width = RT_W;
    rt_depth.surface.height = RT_H;
    rt_depth.surface.depth = 1;
    rt_depth.surface.mipLevels = 1;
    rt_depth.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    rt_depth.surface.format = GX2_SURFACE_FORMAT_FLOAT_D24_S8;   // stencil for modifier volumes
    rt_depth.surface.use = GX2_SURFACE_USE_DEPTH_BUFFER;
    rt_depth.surface.tileMode = GX2_TILE_MODE_DEFAULT;
    rt_depth.viewNumSlices = 1;
    rt_depth.depthClear = 0.0f;
    rt_depth.stencilClear = 0;
    GX2CalcSurfaceSizeAndAlignment(&rt_depth.surface);
    GX2InitDepthBufferRegs(&rt_depth);
    rt_depth.surface.image = MEMAllocFromDefaultHeapEx(rt_depth.surface.imageSize, rt_depth.surface.alignment);

    rt_tex.surface = rt_color.surface;
    rt_tex.surface.use = GX2_SURFACE_USE_TEXTURE;
    rt_tex.viewNumSlices = 1;
    rt_tex.compMap = 0x00010203;
    GX2InitTextureRegs(&rt_tex);
    return rt_color.surface.image && rt_depth.surface.image;
}

static bool init_render_target()
{
    rt_ctx = (GX2ContextState *)MEMAllocFromDefaultHeapEx(sizeof(GX2ContextState), GX2_CONTEXT_STATE_ALIGNMENT);
    if (!alloc_render_target() || !rt_ctx)
        return false;
    GX2SetupContextStateEx(rt_ctx, TRUE);
    return true;
}

// the picker's "3D resolution" option: a new render target between frames, before the emulator starts drawing
void wiiu_gx2_set_render_width(int width)
{
    if (!rt_ctx || width == (int)RT_W)
        return;
    GX2DrawDone();
    MEMFreeToDefaultHeap(rt_color.surface.image);
    MEMFreeToDefaultHeap(rt_depth.surface.image);
    rt_color = {};
    rt_depth = {};
    rt_tex = {};
    wiiu_rt_width = width;
    if (!alloc_render_target())
    {
        // not enough memory for this size: back to the default
        MEMFreeToDefaultHeap(rt_color.surface.image);
        MEMFreeToDefaultHeap(rt_depth.surface.image);
        rt_color = {};
        rt_depth = {};
        rt_tex = {};
        wiiu_rt_width = 960;
        alloc_render_target();
    }
}
static GX2RBuffer pos_buf, uv_buf;
static GX2Texture tex;
static GX2Sampler sampler;
static u32 drawn_seq;
static bool gx2_ready;

bool wiiu_gx2_init()
{
    OSFastMutex_Init(&shared_lock, "nulldc fb");
    OSFastMutex_Init(&snap_lock, "nulldc fb snapshot");
    vram_snap = (u8 *)MEMAllocFromDefaultHeapEx(VRAM_SIZE, 64);   // without it, convert from live VRAM
    OSFastMutex_Init(&frame_lock, "nulldc frame");
    lock_ready = true;
    if (!WHBGfxInit())
        return false;
    if (!WHBGfxLoadGFDShaderGroup(&group, 0, blit_gsh))
        return false;
    WHBGfxInitShaderAttribute(&group, "a_pos", 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32);
    WHBGfxInitShaderAttribute(&group, "a_uv", 1, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32);
    WHBGfxInitFetchShader(&group);

    // 4:3 inside 16:9 (both outputs are 16:9)
    static const float pos[] = { -0.75f, -1.0f, 0.75f, -1.0f, 0.75f, 1.0f, -0.75f, 1.0f };
    static const float uv[] = { 0, 1, 1, 1, 1, 0, 0, 0 };
    pos_buf.flags = uv_buf.flags = GX2R_RESOURCE_BIND_VERTEX_BUFFER | GX2R_RESOURCE_USAGE_CPU_READ |
                                   GX2R_RESOURCE_USAGE_CPU_WRITE | GX2R_RESOURCE_USAGE_GPU_READ;
    pos_buf.elemSize = uv_buf.elemSize = 8;
    pos_buf.elemCount = uv_buf.elemCount = 4;
    GX2RCreateBuffer(&pos_buf);
    GX2RCreateBuffer(&uv_buf);
    memcpy(GX2RLockBufferEx(&pos_buf, (GX2RResourceFlags)0), pos, sizeof(pos));
    GX2RUnlockBufferEx(&pos_buf, (GX2RResourceFlags)0);
    memcpy(GX2RLockBufferEx(&uv_buf, (GX2RResourceFlags)0), uv, sizeof(uv));
    GX2RUnlockBufferEx(&uv_buf, (GX2RResourceFlags)0);

    if (!WHBGfxLoadGFDShaderGroup(&geo, 0, geo_gsh))
        return false;
    WHBGfxInitShaderAttribute(&geo, "a_pos", 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32);
    WHBGfxInitShaderAttribute(&geo, "a_col", 0, 16, GX2_ATTRIB_FORMAT_UNORM_8_8_8_8);
    WHBGfxInitShaderAttribute(&geo, "a_spc", 0, 20, GX2_ATTRIB_FORMAT_UNORM_8_8_8_8);
    WHBGfxInitShaderAttribute(&geo, "a_uv", 0, 24, GX2_ATTRIB_FORMAT_FLOAT_32_32);
    WHBGfxInitFetchShader(&geo);
    for (u32 i = 0; i < geo.pixelShader->uniformVarCount; i++)
    {
        const GX2UniformVar &u = geo.pixelShader->uniformVars[i];
        if (!strcmp(u.name, "u_mode"))
            u_mode_loc = u.offset;
        else if (!strcmp(u.name, "u_shade"))
            u_shade_loc = u.offset;
        else if (!strcmp(u.name, "u_fog_col"))
            u_fog_col_loc = u.offset;
        else if (!strcmp(u.name, "u_fog_vcol"))
            u_fog_vcol_loc = u.offset;
        else if (!strcmp(u.name, "u_clamp_min"))
            u_clamp_min_loc = u.offset;
        else if (!strcmp(u.name, "u_clamp_max"))
            u_clamp_max_loc = u.offset;
        else if (!strcmp(u.name, "u_clamp_on"))
            u_clamp_on_loc = u.offset;
        else if (!strcmp(u.name, "u_screen"))
            u_screen_ps_loc = u.offset;
    }
    for (u32 i = 0; i < geo.vertexShader->uniformVarCount; i++)
        if (!strcmp(geo.vertexShader->uniformVars[i].name, "u_screen"))
            u_screen_vs_loc = geo.vertexShader->uniformVars[i].offset;
    for (u32 i = 0; i < geo.pixelShader->samplerVarCount; i++)
    {
        const GX2SamplerVar &v = geo.pixelShader->samplerVars[i];
        if (!strcmp(v.name, "tex0"))
            tex0_loc = v.location;
        else if (!strcmp(v.name, "fog_table"))
            fog_table_loc = v.location;
    }
    init_fog_texture();
    for (int i = 0; i < SLOTS; i++)
    {
        geo_buf[i].flags = GX2R_RESOURCE_BIND_VERTEX_BUFFER | GX2R_RESOURCE_USAGE_CPU_WRITE | GX2R_RESOURCE_USAGE_GPU_READ;
        geo_buf[i].elemSize = sizeof(GpuVertex);
        geo_buf[i].elemCount = WIIU_MAX_VERTICES + WIIU_MAX_MV_TRIS * 3 + 4;
        GX2RCreateBuffer(&geo_buf[i]);
        idx_buf[i].flags = GX2R_RESOURCE_BIND_INDEX_BUFFER | GX2R_RESOURCE_USAGE_CPU_WRITE | GX2R_RESOURCE_USAGE_GPU_READ;
        idx_buf[i].elemSize = 4;
        idx_buf[i].elemCount = IDX_MAX;
        GX2RCreateBuffer(&idx_buf[i]);
        if (void *p = GX2RLockBufferEx(&idx_buf[i], (GX2RResourceFlags)0))
        {
            memset(p, 0, IDX_MAX * 4);
            GX2RUnlockBufferEx(&idx_buf[i], (GX2RResourceFlags)0);
        }
        // the intake thread writes these directly (and flushes); the GX2 thread invalidates the GPU's copies
        slot_v[i] = (GpuVertex *)geo_buf[i].buffer;
        slot_i[i] = (u32 *)idx_buf[i].buffer;
    }

    if (!init_render_target() || !init_rtt_target())
        return false;
    init_white_texture();
    wiiu_sampler(0);   // the sampler table, built here before two threads look samplers up
    start_worker();
    start_intake();
    start_conv_helper();

    tex.surface.width = FB_MAX_W;
    tex.surface.height = FB_MAX_H;
    tex.surface.depth = 1;
    tex.surface.mipLevels = 1;
    tex.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    tex.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    tex.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    tex.viewNumSlices = 1;
    tex.compMap = 0x00010203;
    GX2CalcSurfaceSizeAndAlignment(&tex.surface);
    GX2InitTextureRegs(&tex);
    GX2RCreateSurface(&tex.surface, GX2R_RESOURCE_BIND_TEXTURE | GX2R_RESOURCE_USAGE_CPU_WRITE | GX2R_RESOURCE_USAGE_GPU_READ);
    u32 *px = (u32 *)GX2RLockSurfaceEx(&tex.surface, 0, (GX2RResourceFlags)0);
    memset(px, 0, tex.surface.imageSize);
    GX2RUnlockSurfaceEx(&tex.surface, 0, (GX2RResourceFlags)0);
    GX2InitSampler(&sampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_LINEAR);
    gx2_ready = true;
    return true;
}

// the PVR's blend instructions; "other colour" is the destination for the source factor and the reverse
static GX2BlendMode blend_mode(u32 instr, bool src)
{
    static const GX2BlendMode modes[8] = { GX2_BLEND_MODE_ZERO, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_DST_COLOR,
                                           GX2_BLEND_MODE_INV_DST_COLOR, GX2_BLEND_MODE_SRC_ALPHA,
                                           GX2_BLEND_MODE_INV_SRC_ALPHA, GX2_BLEND_MODE_DST_ALPHA,
                                           GX2_BLEND_MODE_INV_DST_ALPHA };
    if (!src && instr == 2)
        return GX2_BLEND_MODE_SRC_COLOR;
    if (!src && instr == 3)
        return GX2_BLEND_MODE_INV_SRC_COLOR;
    return modes[instr & 7];
}

// copy the published frame into a GX2 vertex buffer, in clip space: x, y from Dreamcast pixels, w from 1/w, and
// depth = 1/w scaled by the frame's largest 1/w (see depth_func)
static void convert_frame(const WiiuFrame *f, struct GpuVertex *out, u32 *idx, float width, float height, Scene *sc);
static void log_frame(const WiiuFrame *f, u32 frames_taken);

// ---- worker core ---------------------------------------------------------------------------------------
// The worker (core 2) converts the scanned-out framebuffer for the 2D path (convert_fb), off the GX2 thread.
static OSThread worker;
static OSEvent worker_go, worker_done;
static void (*worker_job)(void *);
static void *worker_arg;

static int worker_main(int, const char **)
{
    for (;;)
    {
        OSWaitEvent(&worker_go);
        worker_job(worker_arg);
        OSSignalEvent(&worker_done);
    }
    return 0;
}

static void start_worker()
{
    static u8 stack[64 * 1024] __attribute__((aligned(16)));
    OSInitEvent(&worker_go, FALSE, OS_EVENT_MODE_AUTO);
    OSInitEvent(&worker_done, FALSE, OS_EVENT_MODE_AUTO);
    const u32 core = OSGetCodegenCore() == 2 ? 0 : 2;
    if (OSCreateThread(&worker, worker_main, 0, nullptr, stack + sizeof(stack), sizeof(stack), 15,
                       core == 0 ? OS_THREAD_ATTRIB_AFFINITY_CPU0 : OS_THREAD_ATTRIB_AFFINITY_CPU2))
    {
        OSSetThreadName(&worker, "nullDC GX2 worker");
        OSResumeThread(&worker);
    }
}

// start job on the worker core; worker_wait() then waits for it (a no-op when nothing runs)
static bool worker_busy;
static void worker_wait()
{
    if (worker_busy)
        OSWaitEvent(&worker_done);
    worker_busy = false;
}
static void worker_start(void (*job)(void *), void *arg)
{
    worker_wait();
    worker_job = job;
    worker_arg = arg;
    worker_busy = true;
    OSSignalEvent(&worker_go);
}

// run job on the worker core and wait for it
static void on_worker(void (*job)(void *), void *arg)
{
    worker_start(job, arg);
    worker_wait();
}

void wiiu_textures_begin(u32 scene_seq);
int wiiu_texture_scene(u32 tcw, u32 tsp, u32 scene_seq, GX2Texture *table, u32 *n, u32 max, WiiuTexInval *inv,
                       u32 *n_inv, u32 inv_max);
void wiiu_textures_collect(u32 oldest);

// the GX2 thread's side of the fog table: refresh its texture from what convert_frame copied
static void apply_fog_table() { update_fog_texture(next_fog_table); }

// ---- intake: frames become scenes as soon as they're published --------------------------------------------
//
// The game writes a frame's textures, starts the render, and soon reuses texture memory for the next frame (a
// PVR has read them within a few ms; MvC2 streams its character sprites that way), so a scene built at the GX2
// thread's next vsync would decode sprites from the next frame's data. The intake thread (core 2) wakes on every
// published screen frame and builds its scene right away: TA decode, vertex conversion, and each textured strip's
// texture decoded and its descriptor copied into the scene. The GX2 thread adopts the oldest ready scene and draws it.
//
// decode_lock covers everything one builder uses at a time (the TA parser, the texture cache, convert_frame's
// scratch): the intake thread, and the GX2 thread for render-to-texture frames, which it builds and draws itself.
static OSFastMutex decode_lock, slot_lock;
// Smooth frames (exp_frame_queue): the emulator renders in bursts (it paces on the audio queue), so two scenes can
// finish within one vsync; with one waiting slot the first would be thrown away. Two can wait instead
int wiiu_exp_frame_queue;
static int ready_q[2], n_ready;
static int slot_drawing = -1;   // under slot_lock; the intake thread fills any other slot
static u32 scene_counter;                         // under decode_lock
static u32 intake_taken_seq;
static OSThread intake;

static OSTime intake_ticks[3];   // where the intake core goes: TA decode, convert, texture lookup
extern OSTime wiiu_decode_ticks[3];   // render_gx2.cpp
extern u32 wiiu_decode_blocks;
static u32 intake_frames;
static void build_scene(const WiiuFrame *f, GpuVertex *out, u32 *idx, float w, float h, Scene *sc)
{
    sc->seq = ++scene_counter;
    wiiu_textures_begin(sc->seq);
    const OSTime s0 = OSGetSystemTime();
    wiiu_ta_decode((WiiuFrame *)f);
    const OSTime s1 = OSGetSystemTime();
    convert_frame(f, out, idx, w, h, sc);
    const OSTime s2 = OSGetSystemTime();
    sc->n_tex = sc->n_inv = 0;
    for (u32 i = 0; i < sc->strip_count; i++)
    {
        const WiiuStrip &st = sc->strips[i];
        PCW pcw; pcw.full = st.pcw;
        const int t = pcw.Texture ? wiiu_texture_scene(st.tcw, st.tsp, sc->seq, sc->tex, &sc->n_tex, 1024, sc->inv,
                                                        &sc->n_inv, 1024) : -1;
        sc->strip_tex[i] = t < 0 ? 0xFFFF : (u16)t;
    }
    sc->inv_all = sc->n_inv == 1024;   // a full list may have missed images
    const OSTime s3 = OSGetSystemTime();
    intake_ticks[0] += s1 - s0;
    intake_ticks[1] += s2 - s1;
    intake_ticks[2] += s3 - s2;
    intake_frames++;
    sc->canvas_w = f->canvas_w;
    sc->canvas_h = f->canvas_h;
}

static int intake_main(int, const char **)
{
    for (;;)
    {
        OSWaitEvent(&intake_go);
        for (;;)
        {
            OSFastMutex_Lock(&frame_lock);
            const WiiuFrame *f = frame_seq != intake_taken_seq ? pending_frame : nullptr;
            const u32 seq = frame_seq;
            if (f)
            {
                intake_using = f;   // the emulator builds in the other buffers meanwhile
                intake_taken_seq = seq;
            }
            OSFastMutex_Unlock(&frame_lock);
            if (!f)
                break;

            OSFastMutex_Lock(&slot_lock);
            int s = 0;
            while (s == slot_drawing || (n_ready > 0 && s == ready_q[0]) || (n_ready > 1 && s == ready_q[1]))
                s++;
            // the oldest scene that may still be drawn: surfaces dropped after it was built are kept until then
            u32 oldest = ~0u;
            for (int i = 0; i < n_ready; i++)
                if (scenes[ready_q[i]].seq < oldest)
                    oldest = scenes[ready_q[i]].seq;
            if (slot_drawing >= 0 && scenes[slot_drawing].seq < oldest)
                oldest = scenes[slot_drawing].seq;
            OSFastMutex_Unlock(&slot_lock);

            Scene *sc = &scenes[s];
            OSFastMutex_Lock(&decode_lock);
            wiiu_textures_collect(oldest);
            build_scene(f, slot_v[s], slot_i[s], f->canvas_w, f->canvas_h, sc);
            OSFastMutex_Unlock(&decode_lock);
            sc->frame_seq = seq;
            // flushed by this core (the GX2 thread's flush may not reach lines still in this core's cache)
            DCFlushRange(slot_v[s], (sc->quad_base + 4) * vertex_bytes());
            DCFlushRange(slot_i[s], sc->idx_count * sizeof(u32));
            static u32 built;
            if (++built % 300 == 0 || built == 30)
                log_frame(f, built);

            OSFastMutex_Lock(&frame_lock);
            intake_using = nullptr;
            OSFastMutex_Unlock(&frame_lock);
            OSFastMutex_Lock(&slot_lock);
            // a full queue loses its oldest scene to the newer one, which takes over its texture invalidations:
            // images decoded for the dropped scene are unchanged for this one, so it didn't list them
            if (n_ready == (wiiu_exp_frame_queue ? 2 : 1))
            {
                const Scene &dropped = scenes[ready_q[0]];
                for (u32 i = 0; i < dropped.n_inv && !sc->inv_all; i++)
                {
                    if (sc->n_inv < 1024)
                        sc->inv[sc->n_inv++] = dropped.inv[i];
                    else
                        sc->inv_all = true;
                }
                sc->inv_all |= dropped.inv_all;
                ready_q[0] = ready_q[1];
                n_ready--;
            }
            ready_q[n_ready++] = s;
            OSFastMutex_Unlock(&slot_lock);
        }
    }
    return 0;
}

// 8x8 white, for a textured strip whose texture couldn't be had (cache or table full, no memory)
static void init_white_texture()
{
    GX2Surface &sf = white_tex.surface;
    sf.width = sf.height = 8;
    sf.depth = 1;
    sf.mipLevels = 1;
    sf.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    sf.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    sf.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    white_tex.viewNumSlices = 1;
    white_tex.viewNumMips = 1;
    white_tex.compMap = 0x00010203;
    GX2CalcSurfaceSizeAndAlignment(&sf);
    GX2InitTextureRegs(&white_tex);
    sf.image = MEMAllocFromDefaultHeapEx(sf.imageSize, sf.alignment);
    if (sf.image)
    {
        memset(sf.image, 0xFF, sf.imageSize);
        DCFlushRange(sf.image, sf.imageSize);
        GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, sf.image, sf.imageSize);
    }
}

static void start_intake()
{
    static u8 stack[64 * 1024] __attribute__((aligned(16)));
    OSFastMutex_Init(&decode_lock, "nulldc decode");
    OSFastMutex_Init(&slot_lock, "nulldc slots");
    OSInitEvent(&intake_go, FALSE, OS_EVENT_MODE_AUTO);
    const u32 core = OSGetCodegenCore() == 2 ? 0 : 2;
    if (OSCreateThread(&intake, intake_main, 0, nullptr, stack + sizeof(stack), sizeof(stack), 14,
                       core == 0 ? OS_THREAD_ATTRIB_AFFINITY_CPU0 : OS_THREAD_ATTRIB_AFFINITY_CPU2))
    {
        OSSetThreadName(&intake, "nullDC intake");
        OSResumeThread(&intake);
    }
}

// the GPU's copies of the textures a scene's build decoded (CPU-flushed at decode) are dropped before it's drawn
static void invalidate_textures(const Scene &sc)
{
    if (sc.inv_all)
        GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, nullptr, 0xFFFFFFFF);
    else
        for (u32 i = 0; i < sc.n_inv; i++)
            GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, sc.inv[i].image, sc.inv[i].bytes);
}

// GX2 thread, each vsync: the oldest ready scene becomes the one drawn, so with Smooth frames both queued scenes are
// shown (the GPU finished the previous one in the last vsync's WHBGfxFinishRender, so its slot is the intake
// thread's again)
static void adopt_scene()
{
    OSFastMutex_Lock(&slot_lock);
    const int r = n_ready ? ready_q[0] : -1;
    if (r >= 0)
    {
        slot_drawing = r;
        ready_q[0] = ready_q[1];
        n_ready--;
    }
    OSFastMutex_Unlock(&slot_lock);
    if (r < 0)
        return;
    const Scene &sc = scenes[r];
    GX2Invalidate(GX2_INVALIDATE_MODE_ATTRIBUTE_BUFFER, slot_v[r], (sc.quad_base + 4) * vertex_bytes());
    GX2Invalidate(GX2_INVALIDATE_MODE_ATTRIBUTE_BUFFER, slot_i[r], sc.idx_count * sizeof(u32));
    invalidate_textures(sc);
    geo_cur = r;
    committed = nullptr;
    scene_dirty = true;
    canvas_w = sc.canvas_w;
    canvas_h = sc.canvas_h;
    drawn_frame_seq = sc.frame_seq;
    have_3d = true;
    last_3d_vblank = wiiu_vblanks;
}

// ---- convert (intake thread, a helper on the other core) ----------------------------------------------------

// the frame's vertices into clip space (from w x h Dreamcast pixels) at out; convert_frame puts its strips, volumes,
// translucent order and fog values into the scene. CPU work only; the fog table texture is the GX2 thread's
// (apply_fog_table)
// frame-rate work switches (main_wiiu.cpp keys):
//   conv_split2 (on): the helper also converts the modifier volume vertices (DOA2's fights: ~10,000 a frame, as
//     many as the polygons), and the intake thread waits for it only after the strips, boxes and index lists
//   conv_dcbz (2, on): each output vertex is one 32-byte cache line, written whole: dcbz it rather than read it
//     first; 2: the index list too
//   conv_prio (16, on): the helper's priority on core 0. 16 is below the AICA mixer (15) and level with the GX2
//     thread, so neither waits for the helper (console fr2, fr3, fr5: DOA2 fights +0.6 to +1.3 frames shown a
//     second and fewer underruns, MvC2 +1.8, no game slower); 0 = as created (14)
int wiiu_exp_conv_split2 = 1, wiiu_exp_conv_dcbz = 2, wiiu_exp_conv_prio = 16;
static inline void conv_line(GpuVertex *o)
{
    if (wiiu_exp_conv_dcbz && !((uintptr_t)o & 31))
        __asm__ volatile("dcbz 0,%0" : : "r"(o) : "memory");
}
static void convert_vertices(const WiiuFrame *f, GpuVertex *out, u32 from, u32 to, float hw, float hh, float scale)
{
    for (u32 i = from; i < to; i++)
    {
        const WiiuVertex &v = f->v[i];
        float invw = v.z > 1e-7f ? v.z : 1e-7f;
        float w = 1.0f / invw;
        float d = invw * scale;
        if (d > 1.0f)
            d = 1.0f;
        conv_line(&out[i]);
        out[i].x = (v.x / hw - 1.0f) * w;
        out[i].y = (1.0f - v.y / hh) * w;
        out[i].z = (2.0f * d - 1.0f) * w;
        out[i].w = w;
        out[i].col = v.col;
        out[i].spc = v.spc;
        out[i].u = v.u;
        out[i].v = v.v;
    }
}
int wiiu_exp_split_convert;
static struct { const WiiuFrame *f; GpuVertex *out; u32 from, to; float hw, hh, scale; u32 mv_base, mv_n; } conv_job;
// modifier volume vertices [0, n) of f->mv into out[base...]: clip space, no colour or UV
static void convert_mv(const WiiuFrame *f, GpuVertex *out, u32 base, u32 n, float hw, float hh, float scale)
{
    for (u32 i = 0; i < n; i++)
    {
        const WiiuMvVertex &v = f->mv[i];
        float invw = v.z > 1e-7f ? v.z : 1e-7f;
        float w = 1.0f / invw;
        float d = invw * scale;
        if (d > 1.0f)
            d = 1.0f;
        GpuVertex &o = out[base + i];
        conv_line(&o);
        o.x = (v.x / hw - 1.0f) * w;
        o.y = (1.0f - v.y / hh) * w;
        o.z = (2.0f * d - 1.0f) * w;
        o.w = w;
        o.col = o.spc = 0;
        o.u = o.v = 0.0f;
    }
}
static OSEvent conv_go, conv_done;
static bool conv_helper_up;
static OSThread conv_thread;
static int conv_main(int, const char **)
{
    for (;;)
    {
        OSWaitEvent(&conv_go);
        convert_vertices(conv_job.f, conv_job.out, conv_job.from, conv_job.to, conv_job.hw, conv_job.hh, conv_job.scale);
        if (conv_job.mv_n)
        {
            convert_mv(conv_job.f, conv_job.out, conv_job.mv_base, conv_job.mv_n, conv_job.hw, conv_job.hh, conv_job.scale);
            DCFlushRange(&conv_job.out[conv_job.mv_base], conv_job.mv_n * sizeof(GpuVertex));
        }
        // flushed by the core that wrote them, as the intake thread does its own
        DCFlushRange(&conv_job.out[conv_job.from], (conv_job.to - conv_job.from) * sizeof(GpuVertex));
        OSSignalEvent(&conv_done);
    }
    return 0;
}
static void start_conv_helper()
{
    static u8 stack[32 * 1024] __attribute__((aligned(16)));
    OSInitEvent(&conv_go, FALSE, OS_EVENT_MODE_AUTO);
    OSInitEvent(&conv_done, FALSE, OS_EVENT_MODE_AUTO);
    if (OSCreateThread(&conv_thread, conv_main, 0, nullptr, stack + sizeof(stack), sizeof(stack), 14,
                       OSGetCodegenCore() == 0 ? OS_THREAD_ATTRIB_AFFINITY_CPU2 : OS_THREAD_ATTRIB_AFFINITY_CPU0))
    {
        OSSetThreadName(&conv_thread, "nullDC convert");
        OSResumeThread(&conv_thread);
        conv_helper_up = true;
    }
}
// inside convert_frame: infinite-vertex fixes, this core's vertex half, waiting for the helper's half, modifier volumes
// and copies, the index lists, the translucent sort; and the vertices, strips and indices it handled
static OSTime conv_ticks[6];
static u32 conv_vertices, conv_strips, conv_indices;
static void convert_frame(const WiiuFrame *f, GpuVertex *out, u32 *idx, float width, float height, Scene *sc)
{
    const float hw = width * 0.5f, hh = height * 0.5f;
    const float scale = f->max_invw > 0.0f ? 1.0f / f->max_invw : 1.0f;
    OSTime c0 = OSGetSystemTime(), c1;
    const auto lap = [&](int k) { c1 = OSGetSystemTime(); conv_ticks[k] += c1 - c0; c0 = c1; };
    conv_vertices += f->nv;
    conv_strips += f->ns;
    lap(0);
    // exp_split_convert (the "graphics core" option): this is the intake core's biggest stage, and each vertex stands
    // alone, so the helper core takes the second half
    sc->screen[0] = 0.0f;
    sc->screen[1] = 1.0f / hw;
    sc->screen[2] = 1.0f / hh;
    sc->screen[3] = scale;
    if (wiiu_exp_conv_prio && conv_helper_up && OSGetThreadPriority(&conv_thread) != wiiu_exp_conv_prio)
        OSSetThreadPriority(&conv_thread, wiiu_exp_conv_prio);
    u32 first_half = f->nv;
    const u32 mv_n = f->n_mv_tris * 3;
    sc->mv_base = f->nv;   // modifier volume triangles, then the full-screen quad, after the polygon vertices
    // split2: the helper takes the second half of the polygons and all the volume vertices; split by work, so the
    // intake thread's half grows by half the volume vertices (the boxes, copies and index lists come on top of it)
    const bool split2 = wiiu_exp_conv_split2 && wiiu_exp_split_convert && conv_helper_up && f->nv + mv_n >= 2048;
    bool helper = false;
    if (split2)
    {
        first_half = (f->nv + mv_n) / 2 & ~31u;
        if (first_half > f->nv)
            first_half = f->nv;
        conv_job = { f, out, first_half, f->nv, hw, hh, scale, sc->mv_base, mv_n };
        OSSignalEvent(&conv_go);
        helper = true;
    }
    else if (wiiu_exp_split_convert && f->nv >= 2048 && conv_helper_up)
    {
        first_half = f->nv / 2 & ~31u;
        conv_job = { f, out, first_half, f->nv, hw, hh, scale, 0, 0 };
        OSSignalEvent(&conv_go);
        helper = true;
    }
    convert_vertices(f, out, 0, first_half, hw, hh, scale);
    lap(1);
    if (helper && !split2)
        OSWaitEvent(&conv_done);
    lap(2);
    if (!split2)
        convert_mv(f, out, sc->mv_base, mv_n, hw, hh, scale);
    sc->quad_base = sc->mv_base + f->n_mv_tris * 3;
    if (wiiu_mv_bbox || wiiu_mv_bbox_check)
    {
        // the box of every triangle since the previous closing header: those are what set bit 0x01 for this volume
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        bool whole = false;
        for (u32 i = 0; i < f->n_mp; i++)
        {
            const WiiuModVol &m = f->mp[i];
            for (u32 k = m.first * 3; k < (m.first + m.count) * 3; k++)
            {
                const float x = f->mv[k].x, y = f->mv[k].y;
                if (!(x > -1e9f && x < 1e9f && y > -1e9f && y < 1e9f))   // NaN or huge: no trustworthy box
                    whole = true;
                x0 = x < x0 ? x : x0; x1 = x > x1 ? x : x1;
                y0 = y < y0 ? y : y0; y1 = y > y1 ? y : y1;
            }
            if (m.mode != 1 && m.mode != 2)
                continue;
            float *b = sc->mv_box[i];
            b[0] = whole ? -1e30f : x0; b[1] = y0; b[2] = x1; b[3] = y1;
            x0 = y0 = 1e30f;
            x1 = y1 = -1e30f;
            whole = false;
        }
    }
    // shading inside volumes multiplies by the intensity: black drawn over with alpha 1 - intensity
    sc->shad_scale = f->fpu_shad_scale;
    static const float corners[4][2] = { { -1, 1 }, { -1, -1 }, { 1, 1 }, { 1, -1 } };
    for (u32 i = 0; i < 4; i++)
        out[sc->quad_base + i] = { corners[i][0], corners[i][1], -1.0f, 1.0f, 255u - (sc->shad_scale & 0xFF), 0, 0.0f, 0.0f };
    sc->modvol_count = f->n_mp;
    memcpy(sc->modvols, f->mp, f->n_mp * sizeof(WiiuModVol));
    sc->strip_count = f->ns;
    memcpy(sc->strips, f->s, f->ns * sizeof(WiiuStrip));
    // every other triangle of a strip has its first two vertices swapped, as the GPU does, so the winding (and
    // the culling) stays what the strip had
    lap(3);
    u32 ni = 0;
    sc->idx_owner = (u32)(uintptr_t)idx;
    for (u32 i = 0; i < f->ns; i++)
    {
        const u32 first = f->s[i].first, tris = f->s[i].count - 2;
        sc->strip_idx[i] = ni;
        for (u32 k = 0; k < tris; k++)
        {
            // conv_dcbz=2: a triangle that runs into a new cache line zeroes it rather than read it first
            // (the line starts after idx[ni - 1]; never past the buffer: ni + 11 < IDX_MAX)
            if (wiiu_exp_conv_dcbz >= 2 && ((uintptr_t)&idx[ni + 2] >> 5) != ((uintptr_t)&idx[ni] - 1) >> 5 &&
                ni + 11 < IDX_MAX)
                __asm__ volatile("dcbz 0,%0" : : "r"(&idx[ni + 2]) : "memory");
            idx[ni] = first + k + (k & 1);
            idx[ni + 1] = first + k + 1 - (k & 1);
            idx[ni + 2] = first + k + 2;
            ni += 3;
        }
    }
    sc->idx_count = ni;
    conv_indices += ni;
    lap(4);
    // the PVR sorts translucent polygons by depth itself unless pre-sort mode is on; approximate that per strip,
    // smallest (farthest) first, keeping submission order for ties. The key is the strip's smallest 1/w, its
    // farthest point, as Flycast keys each triangle: a mean let MvC2's cloud layer, which runs from the horizon to
    // right in front of the camera, sort as nearest and draw over the character tiles
    sc->trans_count = 0;
    for (u32 i = 0; i < f->ns; i++)
    {
        const WiiuStrip &st = f->s[i];
        if (st.list != 2)
            continue;
        float lo = 1e30f;
        for (u32 k = 0; k < st.count; k++)
        {
            const float z = f->v[st.first + k].z;
            lo = z < lo ? z : lo;
        }
        trans_key[i] = lo;
        sc->trans_order[sc->trans_count++] = i;
    }
    sc->trans_autosort = !(f->isp_feed_cfg & 1);
    if (sc->trans_autosort)
        std::stable_sort(sc->trans_order, sc->trans_order + sc->trans_count, [](u32 a, u32 b) { return trans_key[a] < trans_key[b]; });
    lap(5);
    if (split2)
        OSWaitEvent(&conv_done);   // the helper's polygons and volume vertices (counted in lap 2, "helper wait")
    {
        const OSTime w = OSGetSystemTime();
        conv_ticks[2] += w - c0;
        c0 = w;
    }
    memcpy(sc->fog_table, f->fog_table, sizeof(sc->fog_table));
    sc->fog_col_ram = f->fog_col_ram;
    sc->fog_col_vert = f->fog_col_vert;
    sc->fog_clamp_min = f->fog_clamp_min;
    sc->fog_clamp_max = f->fog_clamp_max;
    sc->fog_density = f->fog_density;
}

// ---- draw (GX2 thread) ---------------------------------------------------------------------------------

// the statics draw_scene reads, from a converted scene (a few hundred KB of copying at most)
static void commit(const Scene *sc)
{
    if (committed == sc)
        return;
    committed = sc;
    memcpy(draw_screen, sc->screen, sizeof(draw_screen));
    memcpy(strips, sc->strips, sc->strip_count * sizeof(WiiuStrip));
    memcpy(strip_idx, sc->strip_idx, sc->strip_count * sizeof(u32));
    memcpy(draw_strip_tex, sc->strip_tex, sc->strip_count * sizeof(u16));
    draw_tex = (GX2Texture *)sc->tex;
    strip_count = sc->strip_count;
    strip_idx_owner = sc->idx_owner;
    strip_idx_count = sc->idx_count;
    memcpy(modvols, sc->modvols, sc->modvol_count * sizeof(WiiuModVol));
    if (wiiu_mv_bbox || wiiu_mv_bbox_check)
        memcpy(mv_box, sc->mv_box, sc->modvol_count * sizeof(mv_box[0]));
    modvol_count = sc->modvol_count;
    mv_base = sc->mv_base;
    quad_base = sc->quad_base;
    shad_scale = sc->shad_scale;
    memcpy(trans_order, sc->trans_order, sc->trans_count * sizeof(u32));
    trans_count = sc->trans_count;
    trans_autosort = sc->trans_autosort;
    memcpy(next_fog_table, sc->fog_table, sizeof(next_fog_table));
    fog_col_ram = sc->fog_col_ram;
    fog_col_vert = sc->fog_col_vert;
    fog_clamp_min = sc->fog_clamp_min;
    fog_clamp_max = sc->fog_clamp_max;
    fog_density = sc->fog_density;
    apply_fog_table();
}

static void log_frame(const WiiuFrame *f, u32 frames_taken)
{
    {
        u32 per_list[8] = {}, textured = 0, distinct = 0, shadowed = 0;
        u32 seen_tsp[16], seen_tcw[16];
        for (u32 i = 0; i < f->ns; i++)
        {
            const WiiuStrip &st = f->s[i];
            per_list[st.list & 7]++;
            textured += (st.pcw >> 3) & 1;
            shadowed += (st.pcw >> 7) & 1;
            bool known = false;
            for (u32 k = 0; k < distinct; k++)
                known |= seen_tsp[k] == st.tsp && seen_tcw[k] == st.tcw;
            if (!known && distinct < 16)
            {
                seen_tsp[distinct] = st.tsp;
                seen_tcw[distinct++] = st.tcw;
                const WiiuVertex &v = f->v[st.first];
                printf("[gx2]   state: list %u pcw %08x isp %08x tsp %08x tcw %08x clip %08x v0 %.0f,%.0f col %08x uv %.2f,%.2f\n",
                       st.list, st.pcw, st.isp, st.tsp, st.tcw, st.clip, v.x, v.y, v.col, v.u, v.v);
            }
        }
        printf("[gx2] frame %u: %u vertices, %u strips (opaque %u, punch %u, trans %u), %u textured, %u texture decodes so far\n",
               frames_taken, f->nv, f->ns, per_list[0], per_list[4], per_list[2], textured, texture_decodes);
        printf("[gx2]   %u modifier volume headers, %u triangles, %u strips take shadows, FPU_SHAD_SCALE %x\n", f->n_mp,
               f->n_mv_tris, shadowed, f->fpu_shad_scale);
        extern u32 wiiu_dropped_ta_blocks;
        if (wiiu_dropped_vertices || wiiu_dropped_strips || wiiu_dropped_mv_tris || wiiu_dropped_ta_blocks)
            printf("[gx2]   frames overflowed so far: dropped %u vertices, %u strips, %u volume triangles, %u TA blocks\n",
                   wiiu_dropped_vertices, wiiu_dropped_strips, wiiu_dropped_mv_tris, wiiu_dropped_ta_blocks);
    }
}

static void set_uniform(u32 loc, float a, float b, float c, float d)
{
    float v[4] = { a, b, c, d };
    GX2SetPixelUniformReg(loc, 4, (u32 *)v);
}

// depth is 1/w scaled by the frame's largest 1/w, so nearer is greater and far geometry sits near 0, where float
// depth is most precise (the buffer starts at 0, see draw_scene); the PVR's compare modes on 1/w carry over
static const GX2CompareFunction depth_func[8] = { GX2_COMPARE_FUNC_NEVER, GX2_COMPARE_FUNC_LESS, GX2_COMPARE_FUNC_EQUAL,
                                                  GX2_COMPARE_FUNC_LEQUAL, GX2_COMPARE_FUNC_GREATER, GX2_COMPARE_FUNC_NOT_EQUAL,
                                                  GX2_COMPARE_FUNC_GEQUAL, GX2_COMPARE_FUNC_ALWAYS };

static void stencil_pass(GX2CompareFunction func, GX2StencilFunction op, u8 test_mask, u8 write_mask, u8 ref)
{
    GX2SetDepthStencilControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS, TRUE, TRUE, func, op, op, GX2_STENCIL_FUNCTION_KEEP,
                              func, op, op, GX2_STENCIL_FUNCTION_KEEP);
    GX2SetStencilMask(test_mask, write_mask, ref, test_mask, write_mask, ref);
}

// shade what lies inside the opaque modifier volumes. Each volume's triangles flip stencil bit 0x01 where they
// are in front of the scene, so it ends up set where an odd number of surfaces is in front: inside a closed
// volume. A header with mode 1 (inside) or 2 (outside) closes the volume: bit 0x02 collects the pixels it
// covers and 0x01 is cleared. Pixels with 0x02 whose polygon takes shadows (0x80) are then darkened.
#ifdef WIIU_INSTRUMENT
static inline void scene_scissor(u32 x, u32 y, u32 w, u32 h);
// the volumes' boxes go through the bottleneck knob's scissor too (knob=1: 1x1)
static inline void box_scissor(u32 x, u32 y, u32 w, u32 h) { scene_scissor(x, y, w, h); }
#else
static inline void box_scissor(u32 x, u32 y, u32 w, u32 h) { GX2SetScissor(x, y, w, h); }
#endif
static void draw_modvols(u32 width, u32 height, float tile_x, float tile_y)
{
    if (!modvol_count || !(shad_scale & 0x100))
        return;
    GX2SetTargetChannelMasks((GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0,
                             (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0);
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 0x00, FALSE, TRUE);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
    set_uniform(u_mode_loc, 0.0f, -1.0f, 1.0f, 0.0f);
    set_uniform(u_shade_loc, 0.0f, 2.0f, 0.0f, 1.0f);
    set_uniform(u_clamp_on_loc, 0.0f, 0.0f, 0.0f, 0.0f);
    for (u32 i = 0; i < modvol_count; i++)
    {
        const WiiuModVol &m = modvols[i];
        if (m.count)
        {
            // volumes are drawn unculled: they may leave out the faces culling would drop
            GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
            // GREATER passes where the volume surface is in front of the scene. A closed volume flips bit 0x01 (an odd
            // count of faces in front: inside); an open one or a quad (PCW Volume bit clear on a header with a mode) sets
            // it wherever any face is in front, as Flycast does. Parity on Soulcalibur's open shadow volumes tore them
            const bool open = !m.last && (m.mode == 1 || m.mode == 2);
            const GX2StencilFunction op = open ? GX2_STENCIL_FUNCTION_REPLACE : GX2_STENCIL_FUNCTION_INV;
            GX2SetDepthStencilControl(TRUE, FALSE, GX2_COMPARE_FUNC_GREATER, TRUE, TRUE, GX2_COMPARE_FUNC_ALWAYS,
                                      op, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP,
                                      GX2_COMPARE_FUNC_ALWAYS, op, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP);
            GX2SetStencilMask(0xFF, 0x01, 0x01, 0xFF, 0x01, 0x01);
            GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, m.count * 3, mv_base + m.first * 3, 1);
        }
        if (m.mode == 1 || m.mode == 2)
        {
            GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
            // mv_bbox: bit 0x01 can only be set inside the box of this volume's triangles, so the clear, and the
            // inside resolve, need only cover it (canvas pixels to target pixels, a pixel of margin). Outside
            // volumes still resolve over the whole target: everything beyond the box is outside too
            u32 bx = 0, by = 0, bw = width, bh = height;
            const float *b = mv_box[i];
            const bool boxed = wiiu_mv_bbox && b[0] > -1e29f;
            if (boxed)
            {
                const float sx = tile_x / 32.0f, sy = tile_y / 32.0f;
                const float fx0 = b[0] * sx - 1.0f, fy0 = b[1] * sy - 1.0f, fx1 = b[2] * sx + 2.0f, fy1 = b[3] * sy + 2.0f;
                const u32 x0 = fx0 <= 0.0f ? 0 : fx0 >= width ? width : (u32)fx0;
                const u32 y0 = fy0 <= 0.0f ? 0 : fy0 >= height ? height : (u32)fy0;
                const u32 x1 = fx1 <= 0.0f ? 0 : fx1 >= width ? width : (u32)fx1;
                const u32 y1 = fy1 <= 0.0f ? 0 : fy1 >= height ? height : (u32)fy1;
                bx = x0; by = y0;
                bw = x1 > x0 ? x1 - x0 : 0;
                bh = y1 > y0 ? y1 - y0 : 0;
            }
            const bool empty = boxed && (!bw || !bh);   // nothing of it on the target: no bit 0x01 to resolve or clear
            // inside: where 0x01 is set; outside: where it is clear. The reference doubles as the value written
            if (!(empty && m.mode == 1))
            {
                if (boxed && m.mode == 1)
                    box_scissor(bx, by, bw, bh);
                stencil_pass(GX2_COMPARE_FUNC_EQUAL, GX2_STENCIL_FUNCTION_REPLACE, 0x01, 0x02, m.mode == 1 ? 0x03 : 0x02);
                GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLE_STRIP, 4, quad_base, 1);
            }
            if (!empty)
            {
                if (boxed)
                    box_scissor(bx, by, bw, bh);
                stencil_pass(GX2_COMPARE_FUNC_ALWAYS, GX2_STENCIL_FUNCTION_REPLACE, 0xFF, 0x01, 0x00);
                GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLE_STRIP, 4, quad_base, 1);
            }
            if (boxed)
                box_scissor(0, 0, width, height);
        }
    }
    GX2SetTargetChannelMasks(GX2_CHANNEL_MASK_RGBA, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0,
                             (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0);
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 0xFF, FALSE, TRUE);
    GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA, GX2_BLEND_COMBINE_MODE_ADD,
                       FALSE, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ZERO, GX2_BLEND_COMBINE_MODE_ADD);
    stencil_pass(GX2_COMPARE_FUNC_EQUAL, GX2_STENCIL_FUNCTION_KEEP, 0x82, 0x00, 0x82);
    GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLE_STRIP, 4, quad_base, 1);
    GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
}

// target and clears
static void scene_setup(GX2ColorBuffer *color, GX2DepthBuffer *depth, u32 width, u32 height)
{
    GX2SetContextState(rt_ctx);
    GX2SetColorBuffer(color, GX2_RENDER_TARGET_0);
    GX2SetDepthBuffer(depth);
    GX2SetViewport(0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f);
    GX2SetScissor(0, 0, width, height);
    GX2ClearColor(color, 0.0f, 0.0f, 0.0f, 1.0f);
    GX2ClearDepthStencilEx(depth, 0.0f, 0, GX2_CLEAR_FLAGS_BOTH);
    GX2SetContextState(rt_ctx);   // the clears change state
    GX2SetColorBuffer(color, GX2_RENDER_TARGET_0);
    GX2SetDepthBuffer(depth);
    GX2SetViewport(0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f);
    GX2SetScissor(0, 0, width, height);
}

// all of a scene's state and draws
static void scene_body(u32 width, u32 height, GX2RBuffer *vbuf, GX2RBuffer *ibuf, float tile_x, float tile_y)
{
    GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_REGISTER);
    GX2SetFetchShader(&geo.fetchShader);
    GX2SetVertexShader(geo.vertexShader);
    GX2SetPixelShader(geo.pixelShader);
    if (u_screen_vs_loc != ~0u)
        GX2SetVertexUniformReg(u_screen_vs_loc, 4, (u32 *)draw_screen);
    if (u_screen_ps_loc != ~0u)
        GX2SetPixelUniformReg(u_screen_ps_loc, 4, (u32 *)draw_screen);
    GX2RSetAttributeBuffer(vbuf, 0, sizeof(GpuVertex), 0);
    GX2SetPixelTexture(&tex, tex0_loc);
    GX2SetPixelSampler(&sampler, tex0_loc);
    GX2SetPixelTexture(&fog_tex, fog_table_loc);
    GX2SetPixelSampler(&fog_sampler, fog_table_loc);
    auto rgb = [](u32 c, float *v) { v[0] = ((c >> 16) & 0xFF) / 255.0f; v[1] = ((c >> 8) & 0xFF) / 255.0f; v[2] = (c & 0xFF) / 255.0f; };
    float fc[3], fv[3];
    rgb(fog_col_ram, fc);
    rgb(fog_col_vert, fv);
    set_uniform(u_fog_col_loc, fc[0], fc[1], fc[2], 1.0f);
    set_uniform(u_fog_vcol_loc, fv[0], fv[1], fv[2], 1.0f);
    auto argb = [](u32 c, float *v) { v[0] = ((c >> 16) & 0xFF) / 255.0f; v[1] = ((c >> 8) & 0xFF) / 255.0f; v[2] = (c & 0xFF) / 255.0f; v[3] = (c >> 24) / 255.0f; };
    float cmin[4], cmax[4];
    argb(fog_clamp_min, cmin);
    argb(fog_clamp_max, cmax);
    set_uniform(u_clamp_min_loc, cmin[0], cmin[1], cmin[2], cmin[3]);
    set_uniform(u_clamp_max_loc, cmax[0], cmax[1], cmax[2], cmax[3]);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
    // depth starts at 0 (far): written by the full-screen quad (depth 0), since a depth clear to anything but 1.0
    // doesn't take in Cemu
    GX2SetTargetChannelMasks((GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0,
                             (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0);
    GX2SetDepthOnlyControl(TRUE, TRUE, GX2_COMPARE_FUNC_ALWAYS);
    GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLE_STRIP, 4, quad_base, 1);
    GX2SetTargetChannelMasks(GX2_CHANNEL_MASK_RGBA, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0,
                             (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0);
    const float pt_ref = (PT_ALPHA_REF & 0xFF) / 255.0f;
    // the PVR draws opaque, then punch-through, then translucent, whatever order they arrived in
    u32 last_clip = ~0u;
    static const u32 passes[4] = { WIIU_LIST_BACKGROUND, 0 /* opaque */, 4 /* punch-through */, 2 /* translucent */ };
    for (u32 pass : passes)
    {
        if (pass == 2)
        {
            GX2SetScissor(0, 0, width, height);   // the volume passes cover the whole target
            last_clip = 0;
            draw_modvols(width, height, tile_x, tile_y);
        }
        // consecutive strips mostly share state: only send what changed (keys reset per pass, as the volume
        // passes above change state behind them)
        GX2Texture *last_tex = nullptr;
        GX2Sampler *last_smp = nullptr;
        u32 last_depth = ~0u, last_cull = ~0u, last_blend = ~0u, last_mode = ~0u, last_shade = ~0u, last_clamp = ~0u;
        const bool blend = pass == 2;
        GX2SetColorControl(GX2_LOGIC_OP_COPY, blend ? 0xFF : 0x00, FALSE, TRUE);
        // strips are drawn in runs: a strip whose indices follow the pending run's and that changes no state
        // joins it; anything that sets state sends the run first
        u32 run_first = 0, run_count = 0, run_strips = 0, run_vfirst = 0, run_vcount = 0;
        auto flush = [&](u32 why) {
            if (run_count && why < BRK_COUNT)
                run_breaks[why]++;
            if (run_count && (strip_idx_owner != (u32)(uintptr_t)ibuf->buffer || run_first + run_count > strip_idx_count))
            {
                // the strips aren't the ones this index buffer was built for (Cemu crashed on such a draw)
                static u32 bad;
                if (bad++ < 5)
                    printf("[gx2] skipped a draw outside the frame's indices (%u + %u of %u, buffer %s)\n", run_first,
                           run_count, strip_idx_count, strip_idx_owner == (u32)(uintptr_t)ibuf->buffer ? "ok" : "other");
                run_count = 0;
            }
            // a run of one strip is drawn as the strip, without indices: each indexed draw takes one of Cemu's
            // fixed pool of index cache entries, and Soulcalibur's translucent pass (thousands of lone strips)
            // emptied it ("virtualBufferHeap_createEntry: Pool empty", then a crash)
            if (run_count && run_strips == 1)
            {
                GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLE_STRIP, run_vcount, run_vfirst, 1);
                draw_calls++;
                run_count = 0;
            }
            // in pieces of at most 65,535 indices (whole triangles): Cemu's index handling crashed on longer draws,
            // and one long strip can be that long on its own
            for (u32 at = run_first, left = run_count; left; draw_calls++)
            {
                const u32 n = left < 65535 ? left : 65535;
                GX2RDrawIndexed(GX2_PRIMITIVE_MODE_TRIANGLES, ibuf, GX2_INDEX_TYPE_U32, n, at, 0, 1);
                at += n;
                left -= n;
            }
            run_count = 0;
        };
        const u32 count = pass == 2 ? trans_count : strip_count;
        for (u32 n = 0; n < count; n++)
        {
            const u32 si = pass == 2 ? trans_order[n] : n;
            const WiiuStrip &st = strips[si];
            if (st.list != pass)
                continue;
            // user tile clip, mode 2: draw inside the rectangle only; mode 3 (outside) is drawn below, once per band
            // around the rectangle
            const u32 clip_mode = st.clip >> 24;
            u32 rect[4] = { 0, 0, width, height };   // x0, y0, x1, y1 in target pixels
            if (clip_mode == 2 || clip_mode == 3)
            {
                const u32 c = st.clip;
                rect[0] = std::min((u32)((c >> 18 & 63) * tile_x), width);
                rect[1] = std::min((u32)((c >> 12 & 63) * tile_y), height);
                rect[2] = std::min((u32)(((c >> 6 & 63) + 1) * tile_x), width);
                rect[3] = std::min((u32)(((c & 63) + 1) * tile_y), height);
            }
            const u32 clip = clip_mode == 2 ? st.clip : 0;
            if (clip_mode != 3 && clip != last_clip)
            {
                flush(BRK_CLIP);
                if (clip)
                    GX2SetScissor(rect[0], rect[1], rect[2] - rect[0], rect[3] - rect[1]);
                else
                    GX2SetScissor(0, 0, width, height);
                last_clip = clip;
            }
            ISP_TSP isp; isp.full = st.isp;
            TSP tsp; tsp.full = st.tsp;
            PCW pcw; pcw.full = st.pcw;
            const u32 sampler_loc = tex0_loc;
            if (pcw.Texture)
            {
                // the committed scene's copy of the descriptor (the cache may be changing on the intake thread)
                const u16 ti = draw_strip_tex[si];
                GX2Texture *t = ti != 0xFFFF ? &draw_tex[ti] : &white_tex;
                GX2Sampler *smp = wiiu_sampler(st.tsp);
                if (t != last_tex)
                {
                    flush(BRK_TEX);
                    GX2SetPixelTexture(last_tex = t, sampler_loc);
                }
                if (smp != last_smp)
                {
                    flush(BRK_SMP);
                    GX2SetPixelSampler(last_smp = smp, sampler_loc);
                }
            }
            const bool tr_auto = pass == 2 && trans_autosort && wiiu_exp_tr_depth;
            // tr_depth=2: the polygon's own depth write; a plainly blended one's fully transparent pixels discarded
            const bool tr_write = tr_auto && wiiu_exp_tr_depth == 2 && !isp.ZWriteDis;
            const bool tr_discard = tr_write && tsp.SrcInstr == 4 && tsp.DstInstr == 5;
            const u32 depth_key = tr_auto ? 0x100u | tr_write << 9 : isp.DepthMode | isp.ZWriteDis << 3 | pcw.Shadow << 4;
            if (depth_key != last_depth)
                flush(BRK_DEPTH);
            if (depth_key == last_depth)
                ;
            else if (pass == WIIU_LIST_BACKGROUND)
                GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
            else if (tr_auto)
                GX2SetDepthOnlyControl(TRUE, tr_write, depth_func[6]);   // tr_depth: >=; no depth writes (1), own bit (2)
            else if (pass == 2)
                GX2SetDepthOnlyControl(isp.DepthMode != 7, !isp.ZWriteDis, depth_func[isp.DepthMode]);
            else
            {
                // opaque and punch-through: stencil bit 0x80 records whether the visible polygon takes shadows
                GX2SetDepthStencilControl(isp.DepthMode != 7, !isp.ZWriteDis, depth_func[isp.DepthMode], TRUE, FALSE,
                                          GX2_COMPARE_FUNC_ALWAYS, GX2_STENCIL_FUNCTION_REPLACE, GX2_STENCIL_FUNCTION_KEEP,
                                          GX2_STENCIL_FUNCTION_KEEP, GX2_COMPARE_FUNC_ALWAYS, GX2_STENCIL_FUNCTION_REPLACE,
                                          GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP);
                const u8 ref = pcw.Shadow ? 0x80 : 0x00;
                GX2SetStencilMask(0xFF, 0x80, ref, 0xFF, 0x80, ref);
            }
            last_depth = depth_key;
            // culling 2 drops polygons that are counter-clockwise on screen, 3 clockwise ones (1, small ones, isn't
            // done); the y flip into clip space keeps the on-screen winding, so CCW is GX2's front face
            const u32 cull = pass == WIIU_LIST_BACKGROUND ? 0 : isp.CullMode;
            if (cull != last_cull)
                flush(BRK_CULL);
            if (cull != last_cull)
                GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, cull == 2, cull == 3);
            last_cull = cull;
            const u32 blend_key = tsp.SrcInstr | tsp.DstInstr << 3;
            if (blend && blend_key != last_blend)
                flush(BRK_BLEND);
            if (blend && blend_key != last_blend)
                GX2SetBlendControl(GX2_RENDER_TARGET_0, blend_mode(tsp.SrcInstr, true), blend_mode(tsp.DstInstr, false),
                                   GX2_BLEND_COMBINE_MODE_ADD, FALSE, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ZERO,
                                   GX2_BLEND_COMBINE_MODE_ADD);
            last_blend = blend_key;
            const u32 mode_key = pcw.Texture | tsp.UseAlpha << 1 | tsp.IgnoreTexA << 2 | tr_discard << 3;
            if (mode_key != last_mode)
                flush(BRK_MODE);
            if (mode_key != last_mode)
                set_uniform(u_mode_loc, pcw.Texture ? 1.0f : 0.0f, pass == 4 ? pt_ref : tr_discard ? 0.5f / 255.0f : -1.0f, tsp.UseAlpha ? 1.0f : 0.0f,
                            tsp.IgnoreTexA ? 1.0f : 0.0f);
            last_mode = mode_key;
            // Gouraud from the parameter control word, as the TA does (it copies PCW's shading bits into the ISP word);
            // the ISP word's own bit, which games may leave clear, flat-shaded Shenmue's faces and MSR's buildings
            const u32 fog = pass == WIIU_LIST_BACKGROUND ? 2 : tsp.FogCtrl,
                      gouraud = pass == WIIU_LIST_BACKGROUND || pcw.Gouraud;
            const u32 shade_key = tsp.ShadInstr | fog << 2 | gouraud << 4;
            if (shade_key != last_shade)
                flush(BRK_SHADE);
            if (shade_key != last_shade)
                set_uniform(u_shade_loc, (float)tsp.ShadInstr, (float)fog, fog_density, gouraud ? 1.0f : 0.0f);
            last_shade = shade_key;
            const u32 clamp = pass != WIIU_LIST_BACKGROUND && tsp.ColorClamp;
            if (clamp != last_clamp)
                flush(BRK_CLAMP);
            if (clamp != last_clamp)
                set_uniform(u_clamp_on_loc, clamp ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
            last_clamp = clamp;
            const u32 first = strip_idx[si], n_idx = (st.count - 2) * 3;
            draw_strips++;
            if (clip_mode != 3)
            {
                // runs stay under 64K indices: longer single draws crashed Cemu's index handling at Soulcalibur's
                // attract sequence (LatteDrawGL_prepareIndicesWithGPUCache)
                if (run_count && run_first + run_count == first && run_count + n_idx <= 0xFFFF)
                {
                    run_count += n_idx;
                    run_strips++;
                }
                else
                {
                    flush(BRK_GAP);
                    run_first = first;
                    run_count = n_idx;
                    run_strips = 1;
                    run_vfirst = st.first;
                    run_vcount = st.count;
                }
            }
            else
            {
                flush(BRK_COUNT);
                const u32 bands[4][4] = { { 0, 0, width, rect[1] }, { 0, rect[3], width, height },
                                          { 0, rect[1], rect[0], rect[3] }, { rect[2], rect[1], width, rect[3] } };
                for (const auto &b : bands)
                    if (b[2] > b[0] && b[3] > b[1])
                    {
                        GX2SetScissor(b[0], b[1], b[2] - b[0], b[3] - b[1]);
                        run_first = first;
                        run_count = n_idx;
                        run_strips = 1;
                        run_vfirst = st.first;
                        run_vcount = st.count;
                        flush(BRK_COUNT);
                    }
                last_clip = ~0u;
            }
        }
        flush(BRK_COUNT);
    }
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 0x00, FALSE, TRUE);
    GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
}

// make the rendered image visible to the texture unit (and the CPU) afterwards
static void scene_end(GX2ColorBuffer *color)
{
    GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_COLOR_BUFFER | GX2_INVALIDATE_MODE_TEXTURE), color->surface.image,
                  color->surface.imageSize);
}

// render the converted frame (vertices in vbuf) into the top-left w x h of a colour and depth buffer; tile
// clip rectangles are scaled by tile_x, tile_y (the size of a 32-pixel tile in target pixels)
static void draw_scene(GX2ColorBuffer *color, GX2DepthBuffer *depth, u32 width, u32 height, GX2RBuffer *vbuf,
                       GX2RBuffer *ibuf, float tile_x, float tile_y)
{
    scene_setup(color, depth, width, height);
    scene_body(width, height, vbuf, ibuf, tile_x, tile_y);
    scene_end(color);
}

// ---- render to texture --------------------------------------------------------------------------------

static const u32 RTT_MAX = 1024;
static GX2ColorBuffer rtt_color;   // linear, so the CPU can read it back
static GX2DepthBuffer rtt_depth;

static bool init_rtt_target()
{
    rtt_color.surface.width = rtt_color.surface.height = RTT_MAX;
    rtt_color.surface.depth = 1;
    rtt_color.surface.mipLevels = 1;
    rtt_color.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    rtt_color.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    rtt_color.surface.use = GX2_SURFACE_USE_COLOR_BUFFER;
    rtt_color.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    rtt_color.viewNumSlices = 1;
    GX2CalcSurfaceSizeAndAlignment(&rtt_color.surface);
    GX2InitColorBufferRegs(&rtt_color);
    rtt_color.surface.image = MEMAllocFromDefaultHeapEx(rtt_color.surface.imageSize, rtt_color.surface.alignment);

    rtt_depth.surface.width = rtt_depth.surface.height = RTT_MAX;
    rtt_depth.surface.depth = 1;
    rtt_depth.surface.mipLevels = 1;
    rtt_depth.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    rtt_depth.surface.format = GX2_SURFACE_FORMAT_FLOAT_D24_S8;
    rtt_depth.surface.use = GX2_SURFACE_USE_DEPTH_BUFFER;
    rtt_depth.surface.tileMode = GX2_TILE_MODE_DEFAULT;
    rtt_depth.viewNumSlices = 1;
    rtt_depth.depthClear = 0.0f;
    GX2CalcSurfaceSizeAndAlignment(&rtt_depth.surface);
    GX2InitDepthBufferRegs(&rtt_depth);
    rtt_depth.surface.image = MEMAllocFromDefaultHeapEx(rtt_depth.surface.imageSize, rtt_depth.surface.alignment);

    rtt_buf.flags = GX2R_RESOURCE_BIND_VERTEX_BUFFER | GX2R_RESOURCE_USAGE_CPU_WRITE | GX2R_RESOURCE_USAGE_GPU_READ;
    rtt_buf.elemSize = sizeof(GpuVertex);
    rtt_buf.elemCount = WIIU_MAX_VERTICES + WIIU_MAX_MV_TRIS * 3 + 4;
    rtt_idx_buf.flags = GX2R_RESOURCE_BIND_INDEX_BUFFER | GX2R_RESOURCE_USAGE_CPU_WRITE | GX2R_RESOURCE_USAGE_GPU_READ;
    rtt_idx_buf.elemSize = 4;
    rtt_idx_buf.elemCount = IDX_MAX;
    return rtt_color.surface.image && rtt_depth.surface.image && GX2RCreateBuffer(&rtt_buf) &&
           GX2RCreateBuffer(&rtt_idx_buf);
}

// write the rendered w x h pixels to VRAM as the PVR would: FB_W_CTRL bits 2..0 pick the format (0 KRGB0555,
// 1 RGB565, 2 ARGB4444, 3 ARGB1555 with alpha from the threshold in bits 15..8, 4 RGB888 packed, 5 KRGB0888,
// 6 ARGB8888); K is FB_W_CTRL's fb_kval (bits 23..16, top bit for 0555)
struct RttTarget
{
    u32 addr, ctrl, stride, w, h;
};

static void rtt_write_back(const RttTarget *f)
{
    const u32 w = f->w, h = f->h;
    const u32 mode = f->ctrl & 7, threshold = (f->ctrl >> 8) & 0xFF, kval = (f->ctrl >> 16) & 0xFF;
    const u8 *src = (const u8 *)rtt_color.surface.image;
    const u32 pitch = rtt_color.surface.pitch * 4;
    u8 *vram = PVRPARAMS.vram;
    for (u32 y = 0; y < h; y++)
    {
        const u8 *p = src + y * pitch;
        u32 a = f->addr + y * f->stride;
        for (u32 x = 0; x < w; x++, p += 4)
        {
            const u32 r = p[0], g = p[1], b = p[2], al = p[3];
            switch (mode)
            {
            case 0: case 1: case 2: case 3:
            {
                u32 v;
                if (mode == 0)
                    v = (kval & 0x80) << 8 | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3;
                else if (mode == 1)
                    v = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
                else if (mode == 2)
                    v = (al >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4;
                else
                    v = (al >= threshold ? 0x8000 : 0) | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3;
                *host_ptr_xor((u16 *)&vram[a & VRAM_MASK]) = v;
                a += 2;
                break;
            }
            case 4:
                *host_ptr_xor(&vram[a & VRAM_MASK]) = b;
                *host_ptr_xor(&vram[(a + 1) & VRAM_MASK]) = g;
                *host_ptr_xor(&vram[(a + 2) & VRAM_MASK]) = r;
                a += 3;
                break;
            default:
                *(u32 *)&vram[a & VRAM_MASK & ~3u] = (mode == 5 ? kval : al) << 24 | r << 16 | g << 8 | b;
                a += 4;
                break;
            }
        }
    }
}

// draw the waiting render-to-texture frame, if any, and copy its pixels to VRAM (stalls for the GPU)
static void process_rtt()
{
    OSFastMutex_Lock(&frame_lock);
    WiiuFrame *f = pending_rtt;
    if (!f)
    {
        OSFastMutex_Unlock(&frame_lock);
        return;
    }
    const u32 w = f->rtt_w < RTT_MAX ? f->rtt_w : RTT_MAX, h = f->rtt_h < RTT_MAX ? f->rtt_h : RTT_MAX;
    // built and drawn here, holding the builders' lock throughout: the intake thread can't change the texture
    // cache under it, and the GPU is done with the scene before anyone else builds
    OSFastMutex_Lock(&decode_lock);
    build_scene(f, (GpuVertex *)GX2RLockBufferEx(&rtt_buf, (GX2RResourceFlags)0),
                (u32 *)GX2RLockBufferEx(&rtt_idx_buf, (GX2RResourceFlags)0), (float)w, (float)h, &rtt_scene);
    GX2RUnlockBufferEx(&rtt_buf, (GX2RResourceFlags)0);
    GX2RUnlockBufferEx(&rtt_idx_buf, (GX2RResourceFlags)0);
    invalidate_textures(rtt_scene);
    committed = nullptr;
    commit(&rtt_scene);
    // everything the write-back needs, while the frame is certainly still ours
    const RttTarget target = { f->rtt_addr, f->rtt_ctrl, f->rtt_stride, w, h };
    OSFastMutex_Unlock(&frame_lock);

    draw_scene(&rtt_color, &rtt_depth, w, h, &rtt_buf, &rtt_idx_buf, 32.0f, 32.0f);
    GX2DrawDone();
    OSFastMutex_Unlock(&decode_lock);
    DCInvalidateRange(rtt_color.surface.image, rtt_color.surface.imageSize);
    rtt_write_back(&target);
    static u32 rtt_frames;
    if (++rtt_frames == 1 || rtt_frames % 300 == 0)
        printf("[gx2] render to texture %u: %ux%u to %06x, format %u, stride %u\n", rtt_frames, w, h, target.addr,
               target.ctrl & 7, target.stride);

    // free the slot, unless the emulator gave up waiting and already put a newer frame in it
    OSFastMutex_Lock(&frame_lock);
    if (pending_rtt == f)
        pending_rtt = nullptr;
    OSFastMutex_Unlock(&frame_lock);
}

// draw a texture over the 4:3 area of the current output
static void blit(GX2Texture *t)
{
    GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_REGISTER);
    GX2SetFetchShader(&group.fetchShader);
    GX2SetVertexShader(group.vertexShader);
    GX2SetPixelShader(group.pixelShader);
    GX2RSetAttributeBuffer(&pos_buf, 0, pos_buf.elemSize, 0);
    GX2RSetAttributeBuffer(&uv_buf, 1, uv_buf.elemSize, 0);
    GX2SetPixelTexture(t, group.pixelShader->samplerVars[0].location);
    GX2SetPixelSampler(&sampler, group.pixelShader->samplerVars[0].location);
    GX2DrawEx(GX2_PRIMITIVE_MODE_QUADS, 4, 0, 1);
}
// the HOME menu's image over what blit() drew: alpha-blended, with no depth test (the quad is at the same depth as
// the picture under it)
static void blit_over(GX2Texture *t)
{
    GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 0xFF, FALSE, TRUE);
    GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA,
                       GX2_BLEND_COMBINE_MODE_ADD, FALSE, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ZERO, GX2_BLEND_COMBINE_MODE_ADD);
    blit(t);
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 0x00, FALSE, TRUE);
}

// ---- time report and frame loop ----------------------------------------------------------------------

// where the GX2 thread's time goes, reported every 5 s
static struct
{
    OSTime start, rtt, convert, draw, present, finish, flip;
    u32 vsyncs, new_frames, renders_at_start;
} perf;

static void perf_report()
{
    const OSTime now = OSGetSystemTime();
    if (!perf.start)
    {
        perf.start = now;
        perf.renders_at_start = wiiu_renders;
        texture_decode_ticks = 0;
        return;
    }
    const OSTime span = now - perf.start;
    if (span < OSSecondsToTicks(5))
        return;
    auto ms_per_s = [&](OSTime t) { return (float)OSTicksToMicroseconds(t) / (float)OSTicksToMicroseconds(span) * 1000.0f; };
    printf("[gx2] perf %.1f s: %u vsyncs, %u new 3D frames, emulator %.1f renders/s; GX2 thread CPU %.1f ms/s (rtt %.1f, "
           "convert %.1f, draw %.1f incl. textures %.1f, blits %.1f), waiting for the GPU %.1f ms/s; %u strips in %u draws "
           "(runs ended by clip %u, texture %u, sampler %u, depth %u, cull %u, blend %u, mode %u, shade %u, clamp %u, gap %u); "
           "flip wait %.1f ms/s\n",
           (float)OSTicksToMilliseconds(span) / 1000.0f, perf.vsyncs, perf.new_frames,
           (wiiu_renders - perf.renders_at_start) * 1000.0f / OSTicksToMilliseconds(span),
           ms_per_s(perf.rtt + perf.convert + perf.draw + perf.present), ms_per_s(perf.rtt), ms_per_s(perf.convert),
           ms_per_s(perf.draw), ms_per_s(texture_decode_ticks), ms_per_s(perf.present), ms_per_s(perf.finish), draw_strips,
           draw_calls, run_breaks[0], run_breaks[1], run_breaks[2], run_breaks[3], run_breaks[4], run_breaks[5],
           run_breaks[6], run_breaks[7], run_breaks[8], run_breaks[9], ms_per_s(perf.flip));
    draw_strips = draw_calls = 0;
    memset(run_breaks, 0, sizeof(run_breaks));
    perf = {};
    perf.start = now;
    perf.renders_at_start = wiiu_renders;
    texture_decode_ticks = 0;
    if (intake_frames)
    {
        const double ms = 1.0 / intake_frames / 1000.0;
        printf("[gx2] intake per frame: TA decode %.2f ms, convert %.2f ms, textures %.2f ms, %u frames\n",
               OSTicksToMicroseconds(intake_ticks[0]) * ms, OSTicksToMicroseconds(intake_ticks[1]) * ms,
               OSTicksToMicroseconds(intake_ticks[2]) * ms, intake_frames);
        const auto us = [&](OSTime t) { return OSTicksToMicroseconds(t) * ms; };
        printf("[gx2] intake detail per frame: decode = TA loop %.2f + depth pass %.2f + rest %.2f ms; "
               "convert = setup %.2f + vertices %.2f + helper wait %.2f + volumes/copies %.2f + indices %.2f + "
               "translucent sort %.2f ms; %u blocks, %u vertices, %u strips, %u indices\n",
               us(wiiu_decode_ticks[0]), us(wiiu_decode_ticks[1]), us(wiiu_decode_ticks[2]),
               us(conv_ticks[0]), us(conv_ticks[1]), us(conv_ticks[2]), us(conv_ticks[3]), us(conv_ticks[4]),
               us(conv_ticks[5]), wiiu_decode_blocks / intake_frames, conv_vertices / intake_frames,
               conv_strips / intake_frames, conv_indices / intake_frames);
        memset(wiiu_decode_ticks, 0, sizeof(wiiu_decode_ticks));
        memset(conv_ticks, 0, sizeof(conv_ticks));
        wiiu_decode_blocks = conv_vertices = conv_strips = conv_indices = 0;
        intake_ticks[0] = intake_ticks[1] = intake_ticks[2] = 0;
        intake_frames = 0;
    }
}

// what the GX2 thread is doing, for the watchdog in main_wiiu.cpp (which can still report when this thread hangs)
volatile const char *wiiu_gx2_stage = "init";
volatile u32 wiiu_gx2_frames_done;

static void mv_bbox_check();

// draw the newest 3D frame, or the scanned-out framebuffer when there has been no 3D for a second; waits for VSync
void wiiu_gx2_frame()
{
    if (!gx2_ready)
        return;
    wiiu_gx2_stage = "begin render (vsync wait)";
    const OSTime tb = OSGetSystemTime();
    WHBGfxBeginRender();   // waits for the previous frame's flip
    perf.flip += OSGetSystemTime() - tb;
    perf_report();
    // the intake thread built the newest frame's scene when it was published: draw that one
    OSTime t = OSGetSystemTime(), t1;
    const u32 seq_before = drawn_frame_seq;
    wiiu_gx2_stage = "take 3D frame";
    adopt_scene();
    perf.convert += (t1 = OSGetSystemTime()) - t;
    t = t1;
    wiiu_gx2_stage = "render to texture";
    process_rtt();
    perf.rtt += (t1 = OSGetSystemTime()) - t;
    t = t1;
    perf.vsyncs++;
    if (drawn_frame_seq != seq_before)
        perf.new_frames++;
    const bool show_3d = have_3d && wiiu_vblanks - last_3d_vblank < 60;
    fb_wanted = !show_3d;
    static u32 converted_seq;
    if (!show_3d && fb_regs_seq != converted_seq)
    {
        converted_seq = fb_regs_seq;   // a new vblank's snapshot (the emulator may be slower than the display)
        on_worker(convert_fb, nullptr);
    }
    if (!show_3d && shared_seq != drawn_seq)
    {
        OSFastMutex_Lock(&shared_lock);
        u32 *px = (u32 *)GX2RLockSurfaceEx(&tex.surface, 0, (GX2RResourceFlags)0);
        for (u32 y = 0; y < FB_MAX_H; y++)
            memcpy(&px[y * tex.surface.pitch], &shared_rgba[y * FB_MAX_W], FB_MAX_W * 4);
        GX2RUnlockSurfaceEx(&tex.surface, 0, (GX2RResourceFlags)0);
        drawn_seq = shared_seq;
        OSFastMutex_Unlock(&shared_lock);
    }
    if (show_3d && scene_dirty)
    {
        wiiu_gx2_stage = "draw 3D scene";
        {
            commit(&scenes[geo_cur]);
            draw_scene(&rt_color, &rt_depth, RT_W, RT_H, &geo_buf[geo_cur], &idx_buf[geo_cur], 32.0f * RT_W / canvas_w,
                       32.0f * RT_H / canvas_h);
            mv_bbox_check();
        }
        scene_dirty = false;
    }
    perf.draw += (t1 = OSGetSystemTime()) - t;
    t = t1;
    wiiu_gx2_stage = "blit to TV and GamePad";
    for (int screen = 0; screen < 2; screen++)
    {
        if (screen == 0)
            WHBGfxBeginRenderTV();
        else
            WHBGfxBeginRenderDRC();
        WHBGfxClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        if (ui_active && ui_overlay)
        {
            blit(show_3d ? &rt_tex : &tex);   // the paused game's last picture, the HOME menu over it
            blit_over(&ui_tex);
        }
        else
        blit(ui_active ? &ui_tex : show_3d ? &rt_tex : &tex);
        if (screen == 0)
            WHBGfxFinishRenderTV();
        else
            WHBGfxFinishRenderDRC();
    }
    perf.present += (t1 = OSGetSystemTime()) - t;
    wiiu_gx2_stage = "finish render (GPU wait)";
    WHBGfxFinishRender();   // submits and waits for the GPU (GX2DrawDone)
    perf.finish += OSGetSystemTime() - t1;
    wiiu_gx2_stage = "main loop";
    wiiu_gx2_frames_done++;
}

// mv_bbox_check=1: the current scene rendered twice into the linear target, modifier volume quads full screen and
// bounded, and the pixels compared, at the Dreamcast canvas size (1:1). mv_bbox_check=2: at the 3D render target's
// size and scale (e.g. 960x720 from 640x480, where the boxes' rounding matters). Only frames whose volumes shade (else
// both draws are the same by construction)
static void mv_bbox_check()
{
#ifdef WIIU_INSTRUMENT
    static u32 n, frames, bad;
    if (!wiiu_mv_bbox_check || !modvol_count || !(shad_scale & 0x100) || ++n % 30)
        return;
    const bool full = wiiu_mv_bbox_check == 2 && RT_W <= RTT_MAX && RT_H <= RTT_MAX;
    const u32 w = full ? RT_W : canvas_w < RTT_MAX ? (u32)canvas_w : RTT_MAX;
    const u32 h = full ? RT_H : canvas_h < RTT_MAX ? (u32)canvas_h : RTT_MAX;
    const float tx = full ? 32.0f * RT_W / canvas_w : 32.0f, ty = full ? 32.0f * RT_H / canvas_h : 32.0f;
    static std::vector<u32> a;
    const int saved = wiiu_mv_bbox;
    u32 diff = 0, fx = 0, fy = 0, va = 0, vb = 0, shaded = 0;
    for (int pass = 0; pass < 2; pass++)
    {
        wiiu_mv_bbox = pass;
        draw_scene(&rtt_color, &rtt_depth, w, h, &geo_buf[geo_cur], &idx_buf[geo_cur], tx, ty);
        GX2DrawDone();
        DCInvalidateRange(rtt_color.surface.image, rtt_color.surface.imageSize);
        const u32 *px = (const u32 *)rtt_color.surface.image, pitch = rtt_color.surface.pitch;
        if (pass == 0)
        {
            a.resize(w * h);
            for (u32 y = 0; y < h; y++)
                memcpy(&a[y * w], &px[y * pitch], w * 4);
        }
        else
            for (u32 y = 0; y < h; y++)
                for (u32 x = 0; x < w; x++)
                    if (px[y * pitch + x] != a[y * w + x] && diff++ == 0)
                    {
                        fx = x;
                        fy = y;
                        va = a[y * w + x];
                        vb = px[y * pitch + x];
                    }
    }
    wiiu_mv_bbox = saved;
    for (u32 i = 0; i < modvol_count; i++)
        shaded += modvols[i].mode == 1 || modvols[i].mode == 2;
    frames++;
    bad += diff != 0;
    printf("[mv-check] frame %u (%ux%u, %u closed volumes): %u pixels differ; %u of %u checked frames differ%s",
           drawn_frame_seq, w, h, shaded, diff, bad, frames, diff ? "" : "\n");
    if (diff)
        printf("; first at %u,%u: %08x full, %08x bounded\n", fx, fy, va, vb);
#endif
}

extern bool wiiu_gfx_down;   // main_wiiu.cpp
void wiiu_gx2_shutdown()
{
    if (!gx2_ready || wiiu_gfx_down)
        return;
    gx2_ready = false;
    GX2RDestroySurfaceEx(&tex.surface, (GX2RResourceFlags)0);
    GX2RDestroyBufferEx(&pos_buf, (GX2RResourceFlags)0);
    GX2RDestroyBufferEx(&uv_buf, (GX2RResourceFlags)0);
    WHBGfxFreeShaderGroup(&group);
    WHBGfxFreeShaderGroup(&geo);
    for (int i = 0; i < SLOTS; i++)
    {
        GX2RDestroyBufferEx(&geo_buf[i], (GX2RResourceFlags)0);
        GX2RDestroyBufferEx(&idx_buf[i], (GX2RResourceFlags)0);
    }
    WHBGfxShutdown();
}

// save states (wiiu/state_wiiu.cpp): a render-to-texture result still to be written back into VRAM lands before the
// state is taken or loaded
void wiiu_rtt_wait()
{
    for (int i = 0; i < 500 && *(WiiuFrame *volatile *)&pending_rtt; i++)
        OSSleepTicks(OSMillisecondsToTicks(1));
}
void wiiu_textures_all_fresh();   // texture_gx2.cpp
void wiiu_textures_after_load()
{
    OSFastMutex_Lock(&decode_lock);
    wiiu_textures_all_fresh();
    OSFastMutex_Unlock(&decode_lock);
}
