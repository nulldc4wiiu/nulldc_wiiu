// The core's renderer interface (plugs/drkPvr/gxRend.h) for the Wii U. The emulator thread only collects the TA
// input (wiiu_ta_input) and raises its end-of-list interrupts; at StartRender the frame is handed to the GX2
// side (present_gx2.cpp), whose intake thread runs the TA parser (FifoSplitter) over it into FrameDecoder, which
// records each polygon strip with its PVR state words and vertices in screen space.
#include "plugs/drkPvr/gxRend.h"
#include "plugs/drkPvr/regs.h"
#include "plugs/drkPvr/ta.h"
#include "wiiu_frame.h"
#include <math.h>
#include "dc/sh4/ccn.h"               // CCN_QACR0/1 (wiiu_sqw_fast)
#include "dc/mem/sh4_internal_reg.h"  // sq_both
#include "dc/mem/sh4_mem.h"           // mem_b
#include "dc/pvr/pvr_if.h"            // vram
#include "dc/sh4/rec_v2/ccall_census.h"
#include <coreinit/atomic.h>
#include <coreinit/fastmutex.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>

using namespace TASplitter;

u32 wiiu_vblanks, wiiu_renders, wiiu_ta_lists;
// what didn't fit in a frame's buffers (WIIU_MAX_*), for the log
u32 wiiu_dropped_vertices, wiiu_dropped_strips, wiiu_dropped_mv_tris;

// set by _vmem_reserve(); the GX renderer reads VRAM and its texture arena through these
u8 *vram_buffer;
u8 *plus_tex_arena = 0;
u32 plus_tex_arena_size = 0;

static WiiuFrame frames[WIIU_FRAMES];
static bool list_init_since_render;

// SPG.cpp raises the render-done interrupts when these count down; games wait for them between frames
extern s32 render_end_pending_cycles, render_isp_pending_cycles, render_tsp_pending_cycles, render_vd_pending_cycles;
extern "C" int get_render_delay_preset();
extern "C" int get_y_scaler_preset(), get_h_scaler_preset();
static WiiuFrame *build = &frames[0];   // emulator thread: the frame collecting TA input
static WiiuFrame *parked;               // ta_keyed: a finished list waiting for its buffer's render (ListInit)
static WiiuFrame *dec;                  // intake thread (wiiu_ta_decode): the frame FrameDecoder writes
// sq_inline (J1): the JIT writes TA store-queue blocks into the frame itself (shop_pref, wii_driver.cpp), from
// this one cache line: the next free slot, the frame's end, per store queue a mask that is 0x01800000 when its QACR
// area is the TA (dst & mask == 0: TA polygon data) and all ones otherwise, and the scanner's skip count (blocks
// that continue a 64-byte record need no parse). While a frame is built, next is the block count: ta_sync() writes
// it back to build->n_ta at every ordering point (TA events, DMA, the C store-queue path, render start, vblank).
// sq_inline=2 also counts a vertex start inline: vfast is the skip a vertex sets (its size in blocks - 1) while
// the scanner would only count it (a polygon list, not sprites), -1 otherwise. nvert and nstrip are the TA
// scanner's counts of what FrameDecoder will make of the frame: StartRender times the render-done interrupt by
// them (the decoded counts aren't known until the intake thread decodes).
struct WiiuTaFast
{
    u32 *next, *end;
    u32 sq_mask[2];
    s32 skip, vfast;
    u32 nvert, nstrip;
};
extern "C" { WiiuTaFast g_ta_fast __attribute__((aligned(32))) = { nullptr, nullptr, { ~0u, ~0u }, 0, -1, 0, 0 };
             int g_exp_sq_inline = 2; }   // on: 2 (the console A/B r2c: DOA2 fights ~+4%); 0 turns it off
extern int g_exp_sq_fast;
static u32 n_synced;   // build->n_ta as of the last sync
static inline void ta_sync()
{
    if (!g_exp_sq_inline)
        return;
    const u32 n = (u32)(g_ta_fast.next - &build->ta[0][0]) / 8;
    extern u32 wiiu_ta_sq_blocks;
    wiiu_ta_sq_blocks += n - n_synced;   // between syncs only the JIT's inline writes move next
    build->n_ta = n_synced = n;
}
static inline void ta_cursor()
{
    g_ta_fast.next = &build->ta[build->n_ta][0];
    g_ta_fast.end = &build->ta[WIIU_MAX_TA_BLOCKS][0];
    n_synced = build->n_ta;
}
// sq_ram (J1b): per store queue, main RAM's host base when its QACR area is RAM (3), else 0. The JIT's cold
// store-queue fragment copies such a block straight to RAM (the C path's same 32-byte copy, without its call, the
// J1 sync and the address decode: ~4-5K blocks a frame in DOA2 fights, 3.8% of core 1 in r3a's J0 profile)
extern "C" { u32 g_sq_ram[2]; int g_exp_sq_ram = 1; }   // on (console r3b: DOA2 fights +0.5%); 0 turns it off
// QACR0/1 writes (dc/sh4/ccn.cpp) and renderer init: which store queue writes may go inline
extern "C" void wiiu_sq_masks()
{
    const u32 a[2] = { CCN_QACR0.Area, CCN_QACR1.Area };
    for (int i = 0; i < 2; i++)
    {
        g_ta_fast.sq_mask[i] = g_exp_sq_inline && g_exp_sq_fast && a[i] == 4 ? 0x01800000u : 0xFFFFFFFFu;
        g_sq_ram[i] = g_exp_sq_ram && g_exp_sq_inline && g_exp_sq_fast && a[i] == 3 ? (u32)(uintptr_t)mem_b.data : 0;
    }
}

// ---- colour conversion: everything ends up as bytes R, G, B, A in memory --------------------------------

static inline u32 rgba_bytes(u32 r, u32 g, u32 b, u32 a) { return (r << 24) | (g << 16) | (b << 8) | a; }
static inline u32 clamp255(float f) { return f <= 0.0f ? 0 : f >= 1.0f ? 255 : (u32)(f * 255.0f); }
// frame-rate work switches (main_wiiu.cpp keys), on: depth_inline, ta_dcbz (see FrameDecoder)
int wiiu_exp_depth_inline = 1, wiiu_exp_ta_dcbz = 1;
// TA packed colour is ARGB8888
static inline u32 from_argb(u32 c) { return rgba_bytes((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, c >> 24); }
static inline u32 from_float(float a, float r, float g, float b) { return rgba_bytes(clamp255(r), clamp255(g), clamp255(b), clamp255(a)); }
// 16-bit UVs are the top half of a float
static inline float uv16(u16 v) { union { u32 u; float f; } x = { (u32)v << 16 }; return x.f; }

struct FrameDecoder
{
    static u32 list;
    static WiiuStrip cur;
    static float face_a, face_r, face_g, face_b, offs_r, offs_g, offs_b;
    static u32 sprite_col, sprite_spc;
    static u32 clip_mode, clip_rect;
    static bool offset_enabled;

    static u32 intensity(float i) { return from_float(face_a, face_r * i, face_g * i, face_b * i); }
    static u32 offs_intensity(float i) { return offset_enabled ? from_float(1.0f, offs_r * i, offs_g * i, offs_b * i) : 0; }

    static WiiuVertex *vert(const f32 *xyz)
    {
        if (dec->nv >= WIIU_MAX_VERTICES)
        {
            wiiu_dropped_vertices++;
            return nullptr;
        }
        WiiuVertex *v = &dec->v[dec->nv++];
        static_assert(sizeof(WiiuVertex) == 28, "ta_dcbz's line test");
        if (wiiu_exp_ta_dcbz && dec->nv + 2 < WIIU_MAX_VERTICES)
        {
            // ta_dcbz: a vertex that starts a cache line, or runs into the next one, gets that line zeroed rather
            // than read from memory first; the rest of the line is the next vertex's (never past the array: nv + 2)
            const uintptr_t a = (uintptr_t)v;
            if ((a + 27) >> 5 != (a - 1) >> 5)
                __asm__ volatile("dcbz 0,%0" : : "r"(a + 27) : "memory");
        }
        v->x = xyz[0];
        v->y = xyz[1];
        v->z = xyz[2];
        v->col = 0xFFFFFFFF;
        v->spc = 0;
        v->u = v->v = 0;
        if (xyz[2] > dec->max_invw && xyz[2] < 1e30f)
            dec->max_invw = xyz[2];
        return v;
    }

    static void params(const PCW &pcw, const ISP_TSP &isp, const TSP &tsp, const TCW &tcw)
    {
        cur.pcw = pcw.full;
        cur.isp = isp.full;
        cur.tsp = tsp.full;
        cur.tcw = tcw.full;
        cur.list = list;
        cur.clip = clip_mode << 24 | clip_rect;
        offset_enabled = pcw.Offset;
        // intensity mode 2 (colour type 3) reuses the last face colour, whatever headers came between
    }

    static void StartList(u32 list_type) { list = list_type; }   // counted by the scanner (wiiu_ta_lists)
    static void EndList(u32 list_type) {}
    static void ListCont() {}
    // a frame's geometry is emptied once, before its replay (wiiu_ta_decode): like gxRend.cpp, a list init or soft
    // reset in the middle keeps what came before it (the BIOS intro starts several lists per frame, its logo first)
    static void ListInit() { clip_mode = 0; }
    static void SoftReset() { clip_mode = 0; }
    static void SetTileClip(u32 xmin, u32 ymin, u32 xmax, u32 ymax) { clip_rect = xmin << 18 | ymin << 12 | xmax << 6 | ymax; }
    // only polygon headers that start a group change it
    static void TileClipMode(u32 mode) { clip_mode = mode; }
    static void StartPolyStrip() { cur.first = dec->nv; }
    // depth_inline: the depth scale's largest 1/w (wiiu_ta_decode) gathered as each strip is kept, over the same
    // vertices the separate pass reads (strips of 3 or more), while they are still in the cache
    static float strip_max;
    static void strip_kept(const WiiuStrip &st)
    {
        for (u32 k = st.first; k < st.first + st.count && k < dec->nv; k++)
            if (dec->v[k].z > strip_max && dec->v[k].z < 1e30f)
                strip_max = dec->v[k].z;
    }
    static void EndPolyStrip()
    {
        cur.count = dec->nv - cur.first;
        if (cur.count >= 3 && dec->ns < WIIU_MAX_STRIPS)
        {
            dec->s[dec->ns++] = cur;
            if (wiiu_exp_depth_inline)
                strip_kept(cur);
        }
        else if (cur.count >= 3)
            wiiu_dropped_strips++;
    }
    static void EndParam() {}

    static void fastcall AppendPolyParam0(TA_PolyParam0 *pp) { params(pp->pcw, pp->isp, pp->tsp, pp->tcw); }
    static void fastcall AppendPolyParam1(TA_PolyParam1 *pp)
    {
        params(pp->pcw, pp->isp, pp->tsp, pp->tcw);
        face_a = pp->FaceColorA; face_r = pp->FaceColorR; face_g = pp->FaceColorG; face_b = pp->FaceColorB;
    }
    static void fastcall AppendPolyParam2A(TA_PolyParam2A *pp) { params(pp->pcw, pp->isp, pp->tsp, pp->tcw); }
    static void fastcall AppendPolyParam2B(TA_PolyParam2B *pp)
    {
        face_a = pp->FaceColorA; face_r = pp->FaceColorR; face_g = pp->FaceColorG; face_b = pp->FaceColorB;
        offs_r = pp->FaceOffsetR; offs_g = pp->FaceOffsetG; offs_b = pp->FaceOffsetB;
    }
    static void fastcall AppendPolyParam3(TA_PolyParam3 *pp) { params(pp->pcw, pp->isp, pp->tsp, pp->tcw); }
    static void fastcall AppendPolyParam4A(TA_PolyParam4A *pp) { params(pp->pcw, pp->isp, pp->tsp, pp->tcw); }
    static void fastcall AppendPolyParam4B(TA_PolyParam4B *pp)
    {
        face_a = pp->FaceColor0A; face_r = pp->FaceColor0R; face_g = pp->FaceColor0G; face_b = pp->FaceColor0B;
    }
    static void fastcall AppendPolyParam64A(void *pp) {}

    // modifier volumes: only the opaque modifier list (shadows on opaque and punch-through polygons) is kept
    static TA_ModVolA mv_a;
    static void fastcall StartModVol(TA_ModVolParam *p)
    {
        if (list != 1 || dec->n_mp >= WIIU_MAX_MV_PARAMS)
            return;
        dec->mp[dec->n_mp++] = { dec->n_mv_tris, 0, p->isp.full >> 29, (p->isp.full >> 27) & 3, p->pcw.Volume };
    }
    static void fastcall AppendModVolVertexA(TA_ModVolA *a) { mv_a = *a; }
    static void fastcall AppendModVolVertexB(TA_ModVolB *b)
    {
        if (list != 1 || !dec->n_mp)
            return;
        if (dec->n_mv_tris >= WIIU_MAX_MV_TRIS)
        {
            wiiu_dropped_mv_tris++;
            return;
        }
        WiiuMvVertex *t = &dec->mv[dec->n_mv_tris++ * 3];
        t[0] = { mv_a.x0, mv_a.y0, mv_a.z0 };
        t[1] = { mv_a.x1, mv_a.y1, mv_a.z1 };
        t[2] = { mv_a.x2, b->y2, b->z2 };
        dec->mp[dec->n_mp - 1].count++;
        // the depth range must cover the volumes too, or a volume nearer than every polygon clamps onto them
        for (int i = 0; i < 3; i++)
            if (t[i].z > dec->max_invw && t[i].z < 1e30f)
                dec->max_invw = t[i].z;
    }

#define V_OR_RETURN(src) WiiuVertex *v = vert((src)->xyz); if (!v) return
    static void fastcall AppendPolyVertex0(TA_Vertex0 *s) { V_OR_RETURN(s); v->col = from_argb(s->BaseCol); }
    static void fastcall AppendPolyVertex1(TA_Vertex1 *s) { V_OR_RETURN(s); v->col = from_float(s->BaseA, s->BaseR, s->BaseG, s->BaseB); }
    static void fastcall AppendPolyVertex2(TA_Vertex2 *s) { V_OR_RETURN(s); v->col = intensity(s->BaseInt); }
    static void fastcall AppendPolyVertex3(TA_Vertex3 *s)
    {
        V_OR_RETURN(s);
        v->col = from_argb(s->BaseCol);
        v->spc = offset_enabled ? from_argb(s->OffsCol) : 0;
        v->u = s->u; v->v = s->v;
    }
    static void fastcall AppendPolyVertex4(TA_Vertex4 *s)
    {
        V_OR_RETURN(s);
        v->col = from_argb(s->BaseCol);
        v->spc = offset_enabled ? from_argb(s->OffsCol) : 0;
        v->u = uv16(s->u); v->v = uv16(s->v);
    }
    static void fastcall AppendPolyVertex5A(TA_Vertex5A *s) { V_OR_RETURN(s); v->u = s->u; v->v = s->v; }
    static void fastcall AppendPolyVertex5B(TA_Vertex5B *s) { last_float_colours(s->BaseA, s->BaseR, s->BaseG, s->BaseB, s->OffsR, s->OffsG, s->OffsB); }
    static void fastcall AppendPolyVertex6A(TA_Vertex6A *s) { V_OR_RETURN(s); v->u = uv16(s->u); v->v = uv16(s->v); }
    static void fastcall AppendPolyVertex6B(TA_Vertex6B *s) { last_float_colours(s->BaseA, s->BaseR, s->BaseG, s->BaseB, s->OffsR, s->OffsG, s->OffsB); }
    static void fastcall AppendPolyVertex7(TA_Vertex7 *s)
    {
        V_OR_RETURN(s);
        v->u = s->u; v->v = s->v;
        v->col = intensity(s->BaseInt);
        v->spc = offs_intensity(s->OffsInt);
    }
    static void fastcall AppendPolyVertex8(TA_Vertex8 *s)
    {
        V_OR_RETURN(s);
        v->u = uv16(s->u); v->v = uv16(s->v);
        v->col = intensity(s->BaseInt);
        v->spc = offs_intensity(s->OffsInt);
    }
    // two-volume formats: only the first volume is drawn
    static void fastcall AppendPolyVertex9(TA_Vertex9 *s) { V_OR_RETURN(s); v->col = from_argb(s->BaseCol0); }
    static void fastcall AppendPolyVertex10(TA_Vertex10 *s) { V_OR_RETURN(s); v->col = intensity(s->BaseInt0); }
    static void fastcall AppendPolyVertex11A(TA_Vertex11A *s)
    {
        V_OR_RETURN(s);
        v->u = s->u0; v->v = s->v0;
        v->col = from_argb(s->BaseCol0);
        v->spc = offset_enabled ? from_argb(s->OffsCol0) : 0;
    }
    static void fastcall AppendPolyVertex11B(TA_Vertex11B *s) {}
    static void fastcall AppendPolyVertex12A(TA_Vertex12A *s)
    {
        V_OR_RETURN(s);
        v->u = uv16(s->u0); v->v = uv16(s->v0);
        v->col = from_argb(s->BaseCol0);
        v->spc = offset_enabled ? from_argb(s->OffsCol0) : 0;
    }
    static void fastcall AppendPolyVertex12B(TA_Vertex12B *s) {}
    static void fastcall AppendPolyVertex13A(TA_Vertex13A *s)
    {
        V_OR_RETURN(s);
        v->u = s->u0; v->v = s->v0;
        v->col = intensity(s->BaseInt0);
        v->spc = offs_intensity(s->OffsInt0);
    }
    static void fastcall AppendPolyVertex13B(TA_Vertex13B *s) {}
    static void fastcall AppendPolyVertex14A(TA_Vertex14A *s)
    {
        V_OR_RETURN(s);
        v->u = uv16(s->u0); v->v = uv16(s->v0);
        v->col = intensity(s->BaseInt0);
        v->spc = offs_intensity(s->OffsInt0);
    }
    static void fastcall AppendPolyVertex14B(TA_Vertex14B *s) {}
#undef V_OR_RETURN

    // the B half of floating-colour vertices fills in the vertex the A half started
    static void last_float_colours(float a, float r, float g, float b, float orr, float og, float ob)
    {
        if (!dec->nv)
            return;
        WiiuVertex *v = &dec->v[dec->nv - 1];
        v->col = from_float(a, r, g, b);
        v->spc = offset_enabled ? from_float(1.0f, orr, og, ob) : 0;
    }

    // these data-only paths aren't used by the polygon types above
    static void fastcall AppendPolyVertexData(void *p) {}
    static void fastcall AppendPolyVertexData32(void *p) {}
    static void fastcall AppendPolyVertexData32Part(void *p) {}

    // sprites: a quad given as corners A, B, C plus D's x/y; drawn as the strip A, B, D, C
    static void fastcall AppendSpriteParam(TA_SpriteParam *sp)
    {
        params(sp->pcw, sp->isp, sp->tsp, sp->tcw);
        sprite_col = from_argb(sp->BaseCol);
        sprite_spc = sp->pcw.Offset ? from_argb(sp->OffsCol) : 0;
    }
    static TA_Sprite1A sprite_a;
    static void fastcall AppendSpriteVertexA(TA_Sprite1A *s) { sprite_a = *s; }
    static void fastcall AppendSpriteVertexB(TA_Sprite1B *s)
    {
        if (dec->nv + 4 > WIIU_MAX_VERTICES || dec->ns >= WIIU_MAX_STRIPS)
            return;
        const f32 a[3] = { sprite_a.x0, sprite_a.y0, sprite_a.z0 };
        const f32 b[3] = { sprite_a.x1, sprite_a.y1, sprite_a.z1 };
        const f32 c[3] = { sprite_a.x2, s->y2, s->z2 };
        const f32 d[3] = { s->x3, s->y3, sprite_a.z0 + s->z2 - sprite_a.z1 };
        float ua = uv16(s->u0), va = uv16(s->v0), ub = uv16(s->u1), vb = uv16(s->v1), uc = uv16(s->u2), vc = uv16(s->v2);
        cur.first = dec->nv;
        WiiuVertex *v;
        v = vert(a); v->u = ua; v->v = va;
        v = vert(b); v->u = ub; v->v = vb;
        v = vert(d); v->u = ua + uc - ub; v->v = va + vc - vb;
        v = vert(c); v->u = uc; v->v = vc;
        for (u32 i = cur.first; i < dec->nv; i++)
        {
            dec->v[i].col = sprite_col;
            dec->v[i].spc = sprite_spc;
        }
        cur.count = 4;
        dec->s[dec->ns++] = cur;
        if (wiiu_exp_depth_inline)
            strip_kept(cur);
    }
};
float FrameDecoder::strip_max;

u32 FrameDecoder::list;
WiiuStrip FrameDecoder::cur;
float FrameDecoder::face_a = 1.0f, FrameDecoder::face_r = 1.0f, FrameDecoder::face_g = 1.0f, FrameDecoder::face_b = 1.0f;
float FrameDecoder::offs_r, FrameDecoder::offs_g, FrameDecoder::offs_b;
u32 FrameDecoder::sprite_col, FrameDecoder::sprite_spc;
u32 FrameDecoder::clip_mode, FrameDecoder::clip_rect;
bool FrameDecoder::offset_enabled;
TA_Sprite1A FrameDecoder::sprite_a;
TA_ModVolA FrameDecoder::mv_a;

static FifoSplitter<FrameDecoder> TileAccel;

// ---- screen canvas --------------------------------------------------------------------------------------

// set by SPG.cpp from the video timing: 240p modes use half the height, SCALER_CTL.hscale games (with the
// x_scaler preset) submit 1280 wide for 2:1 supersampling
static float fb_scale_x = 1.0f, fb_scale_y = 1.0f;

// the canvas the TA geometry is in: the framebuffer's size, the vertical scaler factor and the pixel-doubling
// 320-wide modes (gxRend.cpp's presets)
static void canvas(float &w, float &h)
{
    // the framebuffer the video output reads (FB_R_SIZE, one field when interlaced) is what the game rendered for:
    // 320 wide (Gunbird 2) or a progressive 240 lines get the canvas they were drawn on, not the top-left of 640x480
    const u32 size = FB_R_SIZE, depth = FB_R_CTRL.fb_depth, bpp = depth == 2 ? 3 : depth == 3 ? 4 : 2;
    const u32 fw = ((size & 0x3FF) + 1) * 4 / bpp, fh = ((size >> 10) & 0x3FF) + 1, inter = SPG_CONTROL.interlace;
    if (FB_R_CTRL.fb_enable)
    {
        fb_scale_x = fw <= 320 ? 0.5f : 1.0f;
        fb_scale_y = !inter && fh <= 288 ? 0.5f : 1.0f;
    }
    static u32 logged;
    if (logged != (fw | fh << 11 | inter << 22 | (VO_CONTROL >> 8 & 1) << 23))
    {
        logged = fw | fh << 11 | inter << 22 | (VO_CONTROL >> 8 & 1) << 23;
        printf("[video] framebuffer %ux%u, interlace %u, pixel double %u, scaler %08x: canvas scale %g x %g\n", fw, fh,
               inter, VO_CONTROL >> 8 & 1, SCALER_CTL.full, fb_scale_x, fb_scale_y);
    }
    w = 640.0f * fb_scale_x;
    h = 480.0f * fb_scale_y;
    if (get_y_scaler_preset() == 1)
    {
        const float vsf = (SCALER_CTL.full & 0xFFFF) / 1024.0f;
        if (vsf >= 0.25f && vsf <= 4.0f && (vsf < 0.999f || vsf > 1.001f))
            h *= vsf;
    }
    if (get_h_scaler_preset() && ((VO_CONTROL >> 8) & 1))
        w *= 0.5f;
}

// ---- background plane ---------------------------------------------------------------------------------

static inline u32 param_u32(u32 a) { return *(u32 *)&PVRPARAMS.vram[vram_offset64(a)]; }
static inline float param_f32(u32 a) { union { u32 u; float f; } x = { param_u32(a) }; return x.f; }

// ISP_BACKGND_T points at a triangle in parameter memory whose plane covers the whole screen behind everything:
// extrapolate its colours and UVs to the screen corners and add it as a strip of its own, drawn first
static void add_background(WiiuFrame *build)
{
    build->has_bg = false;
    const u32 bgt = ISP_BACKGND_T;
    const u32 tag_offset = bgt & 7, tag_address = (bgt >> 3) & 0x1FFFFF, skip = (bgt >> 24) & 7, shadow = (bgt >> 27) & 1;
    const u32 base = (PARAM_BASE & 0xF00000) + tag_address * 4;
    u32 stride = (3 + skip) * 4;
    if (shadow && (FPU_SHAD_SCALE & 0x100))
        stride += skip * 4;   // two-volume vertices
    ISP_TSP isp; isp.full = param_u32(base);
    TSP tsp; tsp.full = param_u32(base + 4);
    TCW tcw; tcw.full = param_u32(base + 8);

    struct { float x, y, z, u, v, c[4], s[4]; } t[3];
    for (u32 i = 0; i < 3; i++)
    {
        u32 a = base + 12 + (tag_offset + i) * stride;
        t[i].x = param_f32(a); t[i].y = param_f32(a + 4); t[i].z = param_f32(a + 8);
        a += 12;
        t[i].u = t[i].v = 0.0f;
        if (isp.Texture)
        {
            if (isp.UV_16b)
            {
                const u32 uv = param_u32(a);
                t[i].u = uv16(uv >> 16);
                t[i].v = uv16(uv & 0xFFFF);
                a += 4;
            }
            else
            {
                t[i].u = param_f32(a);
                t[i].v = param_f32(a + 4);
                a += 8;
            }
        }
        const u32 col = param_u32(a), spc = isp.Offset ? param_u32(a + 4) : 0;
        for (u32 k = 0; k < 4; k++)
        {
            t[i].c[k] = (col >> (24 - 8 * k)) & 0xFF;   // A, R, G, B
            t[i].s[k] = (spc >> (24 - 8 * k)) & 0xFF;
        }
        if (!tsp.UseAlpha)
            t[i].c[0] = 255;
    }

    // barycentric weights of each corner against the triangle, extended over its whole plane
    float den = (t[1].y - t[2].y) * (t[0].x - t[2].x) + (t[2].x - t[1].x) * (t[0].y - t[2].y);
    if (den == 0.0f)
        den = 1e-6f;
    // the corners of the canvas, or of the window a render-to-texture pass writes
    const float cw = build->rtt ? (float)build->rtt_w : build->canvas_w, ch = build->rtt ? (float)build->rtt_h : build->canvas_h;
    const float corner[4][2] = { { 0, 0 }, { 0, ch }, { cw, 0 }, { cw, ch } };
    WiiuStrip st;
    st.first = 0;   // set when it's appended after the decoded geometry
    st.count = 4;
    st.list = WIIU_LIST_BACKGROUND;
    st.clip = 0;
    PCW pcw; pcw.full = 0;
    pcw.Texture = isp.Texture;
    pcw.Offset = isp.Offset;
    st.pcw = pcw.full;
    st.isp = isp.full;
    st.tsp = tsp.full;
    st.tcw = tcw.full;
    for (u32 i = 0; i < 4; i++)
    {
        const float px = corner[i][0], py = corner[i][1];
        const float w0 = ((t[1].y - t[2].y) * (px - t[2].x) + (t[2].x - t[1].x) * (py - t[2].y)) / den;
        const float w1 = ((t[2].y - t[0].y) * (px - t[2].x) + (t[0].x - t[2].x) * (py - t[2].y)) / den;
        const float w2 = 1.0f - w0 - w1;
        auto lerp = [&](float a, float b, float c) { return a * w0 + b * w1 + c * w2; };
        auto byte = [&](float a, float b, float c) { float f = lerp(a, b, c); return f <= 0 ? 0u : f >= 255 ? 255u : (u32)f; };
        WiiuVertex &v = build->bg_v[i];
        v.x = px;
        v.y = py;
        v.z = 0.0f;   // set with the decoded geometry's depth range (wiiu_ta_decode)
        v.u = lerp(t[0].u, t[1].u, t[2].u);
        v.v = lerp(t[0].v, t[1].v, t[2].v);
        v.col = rgba_bytes(byte(t[0].c[1], t[1].c[1], t[2].c[1]), byte(t[0].c[2], t[1].c[2], t[2].c[2]),
                           byte(t[0].c[3], t[1].c[3], t[2].c[3]), byte(t[0].c[0], t[1].c[0], t[2].c[0]));
        v.spc = rgba_bytes(byte(t[0].s[1], t[1].s[1], t[2].s[1]), byte(t[0].s[2], t[1].s[2], t[2].s[2]),
                           byte(t[0].s[3], t[1].s[3], t[2].s[3]), 0);
    }
    build->bg_s = st;
    build->has_bg = true;
}

bool InitRenderer()
{
    printf("[wiiu] GX2 renderer\n");
    ta_cursor();
    wiiu_sq_masks();
    return TileAccel.Init();
}
void TermRenderer()
{
    TileAccel.Term();
}
void ResetRenderer(bool Manual) { TileAccel.Reset(Manual); }
bool ThreadStart() { return true; }
void ThreadEnd() {}
void wiiu_emu_safe_point();
void VBlank()
{
    ta_sync();   // the status line's store queue count stays at most a frame behind
    wiiu_vblanks++;
    wiiu_fb_capture();
    wiiu_emu_safe_point();
#ifdef WIIU_INSTRUMENT
    {
        void wiiu_state_trace_vblank();   // state_wiiu.cpp: state_trace=N
        wiiu_state_trace_vblank();
    }
#endif
}
// ta_keyed (on): a render draws the list in the parameter buffer it renders (PARAM_BASE), not all the TA input
// since the previous render. DOA2 double-buffers its lists and, in the doppelganger intro and the cut scene after
// it, starts the next frame's list in the other buffer before it renders the current one. A list init for another
// buffer than the frame being built sets that frame aside whole (parked) and starts the new list in a frame of its
// own, so no frame holds more than one list (the first version carried the next list's start into the next frame
// by copy, and a frame then held up to two lists: overflow at WIIU_MAX_TA_BLOCKS dropped her leg in the cut scene).
// A render of the parked buffer publishes it while the build goes on. A render with no list init since the
// previous one redraws a list the PVR has: the scene on screen stays. Flycast keys its TA contexts the same way
// (ta_ctx.cpp, by TA_OL_BASE; the render finds its context from the region array).
int wiiu_exp_ta_keyed = 1;   // console fr9: the intro and the cut scene correct, no cost; 0: as before
u32 wiiu_ta_parks, wiiu_ta_parked_renders, wiiu_ta_redraws, wiiu_ta_dropped_lists;   // [ta] keyed line
static inline u32 buffer_key(u32 addr) { return (addr & 0xF00000) | 1; }   // never 0, a frame without a list init
static void frame_clear(WiiuFrame *f)
{
    f->n_ta = f->n_ta_ev = 0;
    f->key = 0;
}
// ListInit, ta_keyed: the new list goes into a frame of its own when the build holds another buffer's list
static void keyed_list_init(u32 key)
{
    if (parked && parked->key == key)
    {
        parked = nullptr;   // the game starts this buffer over before rendering it: the PVR overwrites it too
        wiiu_ta_dropped_lists++;
    }
    if (!build->key || build->key == key)
        return;   // the frame's first list (input before it stays: the BIOS swirl), or this buffer again
    WiiuFrame *next = parked;   // a third buffer: the oldest waiting list goes
    if (next)
        wiiu_ta_dropped_lists++;
    else if (!(next = wiiu_frame_spare(frames, build, nullptr)))
        return;
    build->scan_nvert = g_ta_fast.nvert;
    build->scan_nstrip = g_ta_fast.nstrip;
    parked = build;
    build = next;
    frame_clear(build);
    build->nv = build->ns = build->n_mv_tris = build->n_mp = 0;
    build->max_invw = 0.0f;
    g_ta_fast.nvert = g_ta_fast.nstrip = 0;
    ta_cursor();
    wiiu_ta_parks++;
}
// the game asked the PVR to render what the TA collected: hand the frame to the GX2 thread
void StartRender()
{
    ta_sync();
    wiiu_renders++;
    // ta_keyed: the parked list when this render is for its buffer, else the frame being built
    WiiuFrame *const f = parked && parked->key == buffer_key(PARAM_BASE) ? parked : build;
    const bool redraw = wiiu_exp_ta_keyed && f == build && !list_init_since_render && !((FB_W_SOF1 >> 24) & 1);
    // render-done timing, as gxRend.cpp schedules it for each render_delay preset
    // as the scanner counted them: the geometry itself is decoded later, on the intake thread
    const u32 vertices = f == build ? g_ta_fast.nvert : f->scan_nvert, strips = f == build ? g_ta_fast.nstrip : f->scan_nstrip;
    const int mode = get_render_delay_preset();
    if (mode == 1)
    {
        render_isp_pending_cycles = 800000;
        render_tsp_pending_cycles = 850000;
        render_vd_pending_cycles = 900000;
        render_end_pending_cycles = 0;
    }
    else if (mode == 2)
    {
        // timed by the size of the TA data, like flycast
        s32 rd = 4096;
        if (list_init_since_render)
        {
            const u32 ta_bytes = 32u * (vertices + strips);
            rd = ta_bytes >= (1500000u - 450000u) / 100u ? 1500000 : 450000 + (s32)ta_bytes * 100;
        }
        render_isp_pending_cycles = render_tsp_pending_cycles = render_vd_pending_cycles = 0;
        render_end_pending_cycles = rd;
    }
    else
    {
        render_end_pending_cycles = vertices * 15 < 50000 ? 50000 : vertices * 15;
    }
    // the parked list went to its render: the build already holds the next list, with its list init
    list_init_since_render = f != build;
    if (redraw)
    {
        wiiu_ta_redraws++;   // nothing new for the GX2 side; the input so far stays for the next render
        return;
    }
    for (u32 i = 0; i < 128; i++)
        f->fog_table[i] = FOG_TABLE[i] & 0xFFFF;
    f->fog_col_ram = FOG_COL_RAM;
    f->isp_feed_cfg = ISP_FEED_CFG;
    f->fpu_shad_scale = FPU_SHAD_SCALE;
    f->rtt = (FB_W_SOF1 >> 24) & 1;
    f->rtt_addr = FB_W_SOF1 & VRAM_MASK;
    f->rtt_ctrl = FB_W_CTRL;
    f->rtt_stride = (FB_W_LINESTRIDE & 0x1FF) * 8;
    f->rtt_w = ((FB_X_CLIP.full >> 16) & 0x7FF) + 1;
    f->rtt_h = ((FB_Y_CLIP.full >> 16) & 0x3FF) + 1;
    canvas(f->canvas_w, f->canvas_h);
    add_background(f);   // after the canvas and window sizes it covers
    f->fog_col_vert = FOG_COL_VERT;
    f->fog_clamp_min = FOG_CLAMP_MIN;
    f->fog_clamp_max = FOG_CLAMP_MAX;
    // 1.7 fixed-point mantissa in bits 15..8, signed power-of-two exponent in bits 7..0
    f->fog_density = ((FOG_DENSITY >> 8) & 0xFF) / 128.0f * __builtin_ldexpf(1.0f, (s8)(FOG_DENSITY & 0xFF));
    f->decoded = false;
    if (f != build)
    {
        parked = nullptr;
        wiiu_frame_publish(f, frames, build);   // the build goes on in its own frame
        wiiu_ta_parked_renders++;
        return;
    }
    build = wiiu_frame_publish(build, frames, parked);
    frame_clear(build);   // the next render's input starts here
    g_ta_fast.nvert = g_ta_fast.nstrip = 0;
    ta_cursor();
}
void EndRender() {}
// ---- emulator thread: TA input ----------------------------------------------------------------------------
//
// Each 32-byte block is copied into the frame being built, and a scanner follows the parameter stream just far
// enough to raise each list's end-of-list interrupt at the block the real TA would (games wait for them): it
// skips the second half of 64-byte records (headers of polygon types 2 and 4, vertices of polygon types 5, 6 and
// 11-14, sprite and modifier volume vertices) so their data can't be mistaken for a control word, and starts and
// ends lists as ta_main_ctrl does (plugs/drkPvr/ta.h).
static u32 scan_list = ListType_None, scan_vertex = 1;   // scan_vertex: a vertex's size in blocks (skip: g_ta_fast)
static bool scan_sprite;               // vertices are sprites (4 corners each)
u32 wiiu_dropped_ta_blocks;   // TA input that didn't fit a frame (WIIU_MAX_TA_BLOCKS), for the log
u32 wiiu_ta_sq_blocks, wiiu_ta_dma_blocks;   // how the game feeds the TA (status line): store queue vs DMA

int g_exp_sq_fast;
static void scan_parse(const u32 *b);
// sq_inline: the JIT's cold path for a block that needs parsing (skip is 0; the block is already in the frame)
extern "C" void wiiu_ta_scan(const u32 *b) { scan_parse(b); }
// sq_inline=2: whether the JIT may count the next vertex start itself (see WiiuTaFast)
static inline void scan_vfast()
{
    g_ta_fast.vfast = scan_list != ListType_None && !IsModVolList(scan_list) && !scan_sprite ? (s32)scan_vertex - 1 : -1;
}

static void scan_reset()
{
    scan_list = ListType_None;
    g_ta_fast.skip = 0;
    scan_vertex = 1;
    scan_vfast();
}

static inline void scan_block(const u32 *b)
{
    if (g_ta_fast.skip)
        g_ta_fast.skip--;
    else
        scan_parse(b);
}

static void scan_parse(const u32 *b)
{
    PCW pcw; pcw.full = b[0];
    switch (pcw.ParaType)
    {
    case ParamType_End_Of_List:
        if (scan_list == ListType_None)
            scan_list = pcw.ListType;
        spg_SchedListEndIrq(scan_list);
        scan_list = ListType_None;
        scan_vertex = 1;
        break;
    case ParamType_Polygon_or_Modifier_Volume:
        if (scan_list == ListType_None)
        {
            scan_list = pcw.ListType;
            wiiu_ta_lists++;
        }
        scan_sprite = false;
        if (IsModVolList(scan_list))
            scan_vertex = 2;
        else
        {
            const u32 uid = FifoSplitter<FrameDecoder>::ta_type_lut[pcw.obj_ctrl];
            if (uid == 0xFFFFFFFF)
                break;   // no polygon type: the parser skips it (TA_GUARD)
            const u32 pdid = (u8)uid;
            g_ta_fast.skip = (uid >> 30) - 1;
            scan_vertex = pdid == 5 || pdid == 6 || pdid >= 11 ? 2 : 1;
        }
        break;
    case ParamType_Sprite:
        if (scan_list == ListType_None)
        {
            scan_list = pcw.ListType;
            wiiu_ta_lists++;
        }
        scan_vertex = 2;
        scan_sprite = true;
        break;
    case ParamType_Vertex_Parameter:
        g_ta_fast.skip = scan_vertex - 1;
        if (scan_list != ListType_None && !IsModVolList(scan_list))
        {
            if (scan_sprite)
            {
                g_ta_fast.nvert += 4;
                g_ta_fast.nstrip++;
            }
            else
            {
                g_ta_fast.nvert++;
                g_ta_fast.nstrip += pcw.EndOfStrip;
            }
        }
        return;   // a vertex changes nothing scan_vfast reads
    default:
        break;   // user tile clip, object list set, reserved: 32 bytes
    }
    scan_vfast();
}

// ta.cpp's store queue and DMA entry points (instead of the parser)
void wiiu_ta_input(const u32 *data, u32 blocks)
{
    ta_sync();
    wiiu_ta_dma_blocks += blocks;
    for (u32 i = 0; i < blocks; i++, data += 8)
    {
        scan_block(data);
        if (build->n_ta < WIIU_MAX_TA_BLOCKS)
        {
            // each block fills a whole cache line: dcbz claims it without reading memory first
            u32 *dst = build->ta[build->n_ta];
            __asm__ volatile("dcbz 0,%0" : : "r"(dst) : "memory");
            for (int k = 0; k < 8; k++)
                dst[k] = data[k];
            build->n_ta++;
        }
        else
            wiiu_dropped_ta_blocks++;
    }
    if (g_exp_sq_inline)
        ta_cursor();
}

// The JIT's store-queue flush without the MMU (pref @Rn into 0xE0000000-0xE3FFFFFF), instead of do_sqw_nommu,
// TAWriteSQ, libPvr_TaSQ and wiiu_ta_input in turn: TA polygon data, the common case by far (Soulcalibur sends
// ~19,000 blocks a frame this way), goes straight into the frame being built; anything else takes the full path.
void FASTCALL do_sqw_nommu(u32 dst);
static inline __attribute__((always_inline)) void sqw_c(u32 dst)
{
    const u32 qacr = (dst & 0x20) == 0 ? CCN_QACR0.Area : CCN_QACR1.Area;
    const u32 address = (dst & 0x03FFFFE0) | (qacr << 26);
    if (((address >> 26) & 7) != 4 || (address & 0x1FFFFFF) >= 0x800000)
    {
        // exp_sq_fast (the "faster code" option): 32 bytes to RAM or 64-bit VRAM needn't go through the generic block write and memmove
        const u32 area = (address >> 26) & 7;
        u8 *d = !g_exp_sq_fast ? nullptr : area == 3 ? &mem_b[address & RAM_MASK] :
                area == 1 && !(address & 0x01000000) ? &vram[address & VRAM_MASK] : nullptr;
        if (d)
        {
            const u32 *data = (const u32 *)&sq_both[dst & 0x20];
            __asm__ volatile("lfd 0,0(%1)\n\tlfd 1,8(%1)\n\tlfd 2,16(%1)\n\tlfd 3,24(%1)\n\t"
                             "stfd 0,0(%0)\n\tstfd 1,8(%0)\n\tstfd 2,16(%0)\n\tstfd 3,24(%0)"
                             : : "b"(d), "b"(data) : "memory", "fr0", "fr1", "fr2", "fr3");
            return;
        }
        do_sqw_nommu(dst);
        return;
    }
    CCALL(CC_SQW);
    const u32 *data = (const u32 *)&sq_both[dst & 0x20];
    wiiu_ta_sq_blocks++;
    if (g_exp_sq_fast && build->n_ta < WIIU_MAX_TA_BLOCKS)
    {
        // exp_sq_fast: a block continuing a vertex needs no parsing, and four doubles move it in half the instructions
        if (g_ta_fast.skip)
            g_ta_fast.skip--;
        else
            scan_block(data);
        u32 *d = build->ta[build->n_ta++];
        __asm__ volatile("dcbz 0,%0\n\tlfd 0,0(%1)\n\tlfd 1,8(%1)\n\tlfd 2,16(%1)\n\tlfd 3,24(%1)\n\t"
                         "stfd 0,0(%0)\n\tstfd 1,8(%0)\n\tstfd 2,16(%0)\n\tstfd 3,24(%0)"
                         : : "b"(d), "b"(data) : "memory", "fr0", "fr1", "fr2", "fr3");
        return;
    }
    scan_block(data);
    if (build->n_ta < WIIU_MAX_TA_BLOCKS)
    {
        u32 *d = build->ta[build->n_ta];
        __asm__ volatile("dcbz 0,%0" : : "r"(d) : "memory");
        for (int k = 0; k < 8; k++)
            d[k] = data[k];
        build->n_ta++;
    }
    else
        wiiu_dropped_ta_blocks++;
}

// the JIT's call (shop_pref) without sq_inline
void FASTCALL wiiu_sqw_fast(u32 dst)
{
    sqw_c(dst);
}
// sq_inline: shop_pref's cold path (not TA, or the frame is full), with the JIT's block count around it
extern "C" void FASTCALL wiiu_sqw_cold(u32 dst)
{
    ta_sync();
    sqw_c(dst);
    ta_cursor();
}

static void ta_event(u32 what)
{
    ta_sync();   // the event's index is the block count so far
    if (build->n_ta_ev < WIIU_MAX_TA_EVENTS)
        build->ta_ev[build->n_ta_ev++] = { build->n_ta, what };
}

enum { TA_EV_LIST_CONT = 1, TA_EV_SOFT_RESET = 2, TA_EV_LIST_INIT = 3 };
void ListCont()
{
    ta_event(TA_EV_LIST_CONT);
}
void ListInit()
{
    // like gxRend.cpp, a list init empties nothing: a render draws all the TA input since the previous one. The BIOS
    // intro sends its swirl after one render starts and its grey panels after the next list init, for the same
    // render; emptying here dropped the swirl. The frame is emptied when StartRender hands it over.
    // ta_keyed: a list for another buffer goes into a frame of its own (keyed_list_init).
    scan_reset();
    const u32 key = buffer_key(PvrReg(TA_ISP_BASE_addr, u32));
    if (wiiu_exp_ta_keyed)
    {
        ta_sync();   // the frame set aside ends at the block count so far
        keyed_list_init(key);
    }
    ta_event(TA_EV_LIST_INIT);
    build->key = key;
    list_init_since_render = true;
}
void SoftReset()
{
    scan_reset();
    ta_event(TA_EV_SOFT_RESET);
}

// ---- intake: decode ---------------------------------------------------------------------------------------

extern "C" int get_controller_type();
extern "C" u16 GetLightGunX(int port);
extern "C" u16 GetLightGunY(int port);
extern "C" int wiiu_gun_v2, wiiu_gun_cross;   // drkMapleLightGun.cpp: gun_v2 (touch gun, two crosshairs), gun_cross
extern "C" int wiiu_gun_source(int port);       // what aimed a gun this frame: 1 a Remote, 2 touch
OSTime wiiu_decode_ticks[3];   // inside TA decode: the TA command loop, the depth-scale pass, the rest
u32 wiiu_decode_blocks;
void wiiu_ta_decode(WiiuFrame *f)
{
    if (f->decoded)
        return;
    const OSTime d0 = OSGetSystemTime();
    wiiu_decode_blocks += f->n_ta;
    dec = f;
    f->nv = f->ns = f->n_mv_tris = f->n_mp = 0;
    f->max_invw = 0.0f;
    FrameDecoder::strip_max = 0.0f;
    TileAccel.ListInit();   // TaCmd back to the start
    u32 ev = 0;
    for (u32 i = 0; i <= f->n_ta; )
    {
        for (; ev < f->n_ta_ev && f->ta_ev[ev].at == i; ev++)
        {
            if (f->ta_ev[ev].what == TA_EV_LIST_CONT)
                TileAccel.ListCont();
            else if (f->ta_ev[ev].what == TA_EV_LIST_INIT)
                TileAccel.ListInit();
            else
                TileAccel.SoftReset();
        }
        if (i == f->n_ta)
            break;
        const u32 end = ev < f->n_ta_ev && f->ta_ev[ev].at < f->n_ta ? f->ta_ev[ev].at : f->n_ta;
        Ta_Dma *p = (Ta_Dma *)f->ta[i], *last = (Ta_Dma *)f->ta[end - 1];
        do
            p = TaCmd(p, last);
        while (p <= last);
        i = end;
    }
    const OSTime d1 = OSGetSystemTime();
    wiiu_decode_ticks[0] += d1 - d0;
    // the depth scale (1 / the largest 1/w) from the polygons drawn: strips of 3 or more vertices. A larger 1/w
    // elsewhere (Jet Grind Radio's shadow volumes: 100000) squeezed the whole scene into depths ~1e-5, where the
    // clip-space conversion rounds them together and near surfaces flicker through each other
    {
    float m = 0.0f;
    if (wiiu_exp_depth_inline)
        m = FrameDecoder::strip_max;   // the same maximum, gathered as the strips were kept
    else
    for (u32 i = 0; i < f->ns; i++)
        if (f->s[i].count >= 3)
            for (u32 k = f->s[i].first; k < f->s[i].first + f->s[i].count && k < f->nv; k++)
                if (f->v[k].z > m && f->v[k].z < 1e30f)
                    m = f->v[k].z;
    // modifier volumes left out: Jet Grind Radio's run to 1/w 100000 toward the camera. Their nearer vertices
    // clamp to the nearest depth, still in front of every polygon (the 5% headroom), which is all the volume
    // test asks of them
    m *= 1.05f;
    static u32 logged;
    if (m < f->max_invw && logged < 5)
    {
        logged++;
        printf("[gx2] depth scale from drawn vertices: largest 1/w %g, not %g\n", m, f->max_invw);
    }
    if (m > 0.0f)
        f->max_invw = m;
    }
    const OSTime d2 = OSGetSystemTime();
    wiiu_decode_ticks[1] += d2 - d1;
    if (f->has_bg && f->nv + 4 <= WIIU_MAX_VERTICES && f->ns < WIIU_MAX_STRIPS)
    {
        // drawn without depth; mid-range so it can't be clipped against either depth plane
        const float z = f->max_invw > 0.0f ? f->max_invw * 0.5f : 1.0f;
        WiiuStrip st = f->bg_s;
        st.first = f->nv;
        for (u32 k = 0; k < 4; k++)
        {
            f->v[f->nv] = f->bg_v[k];
            f->v[f->nv++].z = z;
        }
        f->s[f->ns++] = st;
    }
    // light gun: a Dreamcast gun has no on-screen cursor (it was aimed down the barrel at a CRT), so draw a crosshair
    // where the Wii Remote points: two bars, last in the translucent list, without depth
    // (gun_v2: one per Remote gun, red and blue, none for the touch gun; gun_cross=0: none)
    for (int port = 0; port < (wiiu_gun_v2 ? 2 : 1); port++)
    if (get_controller_type() == 1 && GetLightGunX(port) != 0xFFFF && f->nv + 8 <= WIIU_MAX_VERTICES && f->ns + 2 <= WIIU_MAX_STRIPS &&
        (!wiiu_gun_v2 || (wiiu_gun_cross && wiiu_gun_source(port) == 1)))
    {
        const float sx = f->canvas_w / 640.0f, sy = f->canvas_h / 480.0f;
        const float cx = GetLightGunX(port) * sx, cy = GetLightGunY(port) * sy, len = 12.0f * sx, th = 1.5f * sx;
        const float z = f->max_invw > 0.0f ? f->max_invw : 1.0f;
        const float bars[2][4] = { { cx - len, cy - th, cx + len, cy + th }, { cx - th, cy - len, cx + th, cy + len } };
        for (const auto &b : bars)
        {
            WiiuStrip st = {};
            st.first = f->nv;
            st.count = 4;
            st.list = 2;
            st.isp = 7u << 29 | 1u << 26 | 1u << 23;   // depth always, no depth write, Gouraud
            st.pcw = 1u << 1;                            // Gouraud, where the renderer reads it
            st.tsp = 1u << 29 | 2u << 22;               // source one, destination zero, no fog
            const float xy[4][2] = { { b[0], b[1] }, { b[2], b[1] }, { b[0], b[3] }, { b[2], b[3] } };
            for (const auto &c : xy)
                f->v[f->nv++] = { c[0], c[1], z, port ? 0x3070FFFFu : 0xFF3030FFu, 0, 0.0f, 0.0f };
            f->s[f->ns++] = st;
        }
    }
    wiiu_decode_ticks[2] += OSGetSystemTime() - d2;
    f->decoded = true;
    dec = nullptr;
}
void SetFpsText(char *text) {}
void SetFbScale(float x, float y) { fb_scale_x = x; fb_scale_y = y; }
void strip_dedup_dump(double tdiff) {}

// save states (wiiu/state_wiiu.cpp), emulator thread with the CPU stopped: after a load the TA input collected for
// the old state goes, and the next ListInit/StartRender builds from the loaded registers (one frame may be lost)
void wiiu_render_after_load()
{
    parked = nullptr;
    frame_clear(build);
    build->nv = build->ns = build->n_mv_tris = build->n_mp = 0;
    build->max_invw = 0.0f;
    g_ta_fast.nvert = g_ta_fast.nstrip = 0;
    scan_reset();
    ta_cursor();
    list_init_since_render = false;
    wiiu_sq_masks();
}
