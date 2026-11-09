// The core's renderer interface (plugs/drkPvr/gxRend.h) for the Wii U. The emulator thread only collects the TA
// input (wiiu_ta_input) and raises its end-of-list interrupts; at StartRender the frame is handed to the GX2
// thread (present_gx2.cpp), whose worker core runs the TA parser (FifoSplitter) over it into FrameDecoder, which
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
#include "prof_wiiu.h"
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

static WiiuFrame frames[4];   // building, latest screen frame, render-to-texture frame, the one the GX2 thread reads
static bool list_init_since_render;

// SPG.cpp raises the render-done interrupts when these count down; games wait for them between frames
extern s32 render_end_pending_cycles, render_isp_pending_cycles, render_tsp_pending_cycles, render_vd_pending_cycles;
extern "C" int get_render_delay_preset();
extern "C" int get_canvas_width_preset(), get_y_scaler_preset(), get_h_scaler_preset();
static WiiuFrame *build = &frames[0];   // emulator thread: the frame collecting TA input
static WiiuFrame *dec;                  // worker (wiiu_ta_decode): the frame FrameDecoder writes
// the TA scanner (below): what FrameDecoder will make of the frame, counted as it arrives: StartRender times the render-done interrupt
// by them (the decoded counts aren't known until the worker runs)
static u32 scan_vertices, scan_strips;
static u32 scan_pos;   // exp_scan_thread: blocks of the frame being built already scanned
static void scan_sync_lock();
static void scan_unlock();

// ---- colour conversion: everything ends up as bytes R, G, B, A in memory --------------------------------

static inline u32 rgba_bytes(u32 r, u32 g, u32 b, u32 a) { return (r << 24) | (g << 16) | (b << 8) | a; }
static inline u32 clamp255(float f) { return f <= 0.0f ? 0 : f >= 1.0f ? 255 : (u32)(f * 255.0f); }
// TA packed colour is ARGB8888
static inline u32 from_argb(u32 c) { return rgba_bytes((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, c >> 24); }
static inline u32 from_float(float a, float r, float g, float b) { return rgba_bytes(clamp255(r), clamp255(g), clamp255(b), clamp255(a)); }
// 16-bit UVs are the top half of a float
static inline float uv16(u16 v) { union { u32 u; float f; } x = { (u32)v << 16 }; return x.f; }

// exp_single_pass (with raw vertices): where decode streams each closed strip (build_scene sets these)
int wiiu_exp_single_pass;
WiiuVertex *wiiu_stream_v;
u32 *wiiu_stream_i;
extern int wiiu_exp_trans_min;   // present_gx2.cpp
// copy whole vertices to the GPU buffer; destination cache lines wholly inside are zeroed first (dcbz) so they're
// never read from memory just to be overwritten
static inline void stream_copy(void *dst, const void *src, u32 bytes)
{
    u8 *d = (u8 *)dst;
    u8 *line = (u8 *)(((uintptr_t)d + 31) & ~31u), *end = (u8 *)(((uintptr_t)d + bytes) & ~31u);
    for (; line < end; line += 32)
        asm volatile("dcbz 0,%0" : : "r"(line) : "memory");
    memcpy(d, src, bytes);
}
static void stream_strip(WiiuStrip &st)
{
    // the strip's vertices are still in this core's cache: depth range, sort key, GPU copy and indices in one go
    const WiiuVertex *v = &dec->v[st.first];
    float hi = 0.0f, lo = 1e30f, sum = 0.0f;
    for (u32 k = 0; k < st.count; k++)
    {
        const float z = v[k].z;
        if (z > hi && z < 1e30f)
            hi = z;
        lo = z < lo ? z : lo;
        sum += z;
    }
    if (hi > dec->max_drawn)
        dec->max_drawn = hi;
    st.zkey = wiiu_exp_trans_min ? lo : sum / st.count;
    // vertices of dropped short strips between strips are copied too, so the copy stays one contiguous run
    const u32 from = dec->streamed_nv, to = st.first + st.count;
    if (to > from)
        stream_copy(&wiiu_stream_v[from], &dec->v[from], (to - from) * sizeof(WiiuVertex));
    dec->streamed_nv = to;
    const u32 tris = st.count - 2;
    u32 *idx = &wiiu_stream_i[dec->streamed_ni];
    u8 *line = (u8 *)(((uintptr_t)idx + 31) & ~31u), *end = (u8 *)(((uintptr_t)(idx + tris * 3)) & ~31u);
    for (; line < end; line += 32)
        asm volatile("dcbz 0,%0" : : "r"(line) : "memory");
    for (u32 k = 0; k < tris; k++)
    {
        idx[k * 3] = st.first + k + (k & 1);
        idx[k * 3 + 1] = st.first + k + 1 - (k & 1);
        idx[k * 3 + 2] = st.first + k + 2;
    }
    dec->streamed_ni += tris * 3;
    dec->streamed_ns++;
}

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
    static void EndPolyStrip()
    {
        cur.count = dec->nv - cur.first;
        if (cur.count >= 3 && dec->ns < WIIU_MAX_STRIPS)
        {
            dec->s[dec->ns++] = cur;
            if (wiiu_stream_v)
                stream_strip(dec->s[dec->ns - 1]);
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
        if (wiiu_stream_v)
            stream_strip(dec->s[dec->ns - 1]);
    }
};

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

// the canvas the TA geometry is in, as gxRend.cpp sizes it (same presets): the video mode's scale, a forced
// width for 240p games that render narrow, the vertical scaler factor and the pixel-doubling 320-wide modes
int wiiu_exp_canvas_regs = 1;   // exp_canvas_regs=0: always 640x480, as before
static void canvas(float &w, float &h)
{
    // the framebuffer the video output reads (FB_R_SIZE, one field when interlaced) is what the game rendered for:
    // 320 wide (Gunbird 2) or a progressive 240 lines get the canvas they were drawn on, not the top-left of 640x480
    const u32 size = FB_R_SIZE, depth = FB_R_CTRL.fb_depth, bpp = depth == 2 ? 3 : depth == 3 ? 4 : 2;
    const u32 fw = ((size & 0x3FF) + 1) * 4 / bpp, fh = ((size >> 10) & 0x3FF) + 1, inter = SPG_CONTROL.interlace;
    if (wiiu_exp_canvas_regs && FB_R_CTRL.fb_enable)
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
    bool forced = false;
    const int forced_w = get_canvas_width_preset();
    if (!wiiu_exp_canvas_regs && forced_w >= 320 && forced_w <= 1280 && fb_scale_y == 0.5f)
    {
        w = (float)forced_w;
        forced = true;
    }
    if (get_y_scaler_preset() == 1)
    {
        const float vsf = (SCALER_CTL.full & 0xFFFF) / 1024.0f;
        if (vsf >= 0.25f && vsf <= 4.0f && (vsf < 0.999f || vsf > 1.001f))
            h *= vsf;
    }
    if (get_h_scaler_preset() && !forced && ((VO_CONTROL >> 8) & 1))
        w *= 0.5f;
}

// ---- background plane ---------------------------------------------------------------------------------

static inline u32 param_u32(u32 a) { return *(u32 *)&PVRPARAMS.vram[vram_offset64(a)]; }
static inline float param_f32(u32 a) { union { u32 u; float f; } x = { param_u32(a) }; return x.f; }

// ISP_BACKGND_T points at a triangle in parameter memory whose plane covers the whole screen behind everything:
// extrapolate its colours and UVs to the screen corners and add it as a strip of its own, drawn first
static void add_background()
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

bool InitRenderer() { printf("[wiiu] GX2 renderer\n"); return TileAccel.Init(); }
static void scan_stop_thread();
void TermRenderer()
{
    scan_stop_thread();
    TileAccel.Term();
}
void ResetRenderer(bool Manual) { TileAccel.Reset(Manual); }
bool ThreadStart() { return true; }
void ThreadEnd() {}
extern u32 wiiu_bench_last;
void wiiu_bench_vblank();   // main_wiiu.cpp
void wiiu_emu_safe_point();
void VBlank()
{
    wiiu_vblanks++;
    wiiu_fb_capture();
    wiiu_emu_safe_point();
    if (wiiu_bench_last)
        wiiu_bench_vblank();
}
// the game asked the PVR to render what the TA collected: hand the frame to the GX2 thread
void StartRender()
{
    scan_sync_lock();
    wiiu_renders++;
    // render-done timing, as gxRend.cpp schedules it for each render_delay preset
    // as the scanner counted them: the geometry itself is decoded later, on the worker
    const u32 vertices = scan_vertices, strips = scan_strips;
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
    list_init_since_render = false;
    for (u32 i = 0; i < 128; i++)
        build->fog_table[i] = FOG_TABLE[i] & 0xFFFF;
    build->fog_col_ram = FOG_COL_RAM;
    build->isp_feed_cfg = ISP_FEED_CFG;
    build->fpu_shad_scale = FPU_SHAD_SCALE;
    build->rtt = (FB_W_SOF1 >> 24) & 1;
    build->rtt_addr = FB_W_SOF1 & VRAM_MASK;
    build->rtt_ctrl = FB_W_CTRL;
    build->rtt_stride = (FB_W_LINESTRIDE & 0x1FF) * 8;
    build->rtt_w = ((FB_X_CLIP.full >> 16) & 0x7FF) + 1;
    build->rtt_h = ((FB_Y_CLIP.full >> 16) & 0x3FF) + 1;
    canvas(build->canvas_w, build->canvas_h);
    add_background();   // after the canvas and window sizes it covers
    build->fog_col_vert = FOG_COL_VERT;
    build->fog_clamp_min = FOG_CLAMP_MIN;
    build->fog_clamp_max = FOG_CLAMP_MAX;
    // 1.7 fixed-point mantissa in bits 15..8, signed power-of-two exponent in bits 7..0
    build->fog_density = ((FOG_DENSITY >> 8) & 0xFF) / 128.0f * __builtin_ldexpf(1.0f, (s8)(FOG_DENSITY & 0xFF));
    build->decoded = false;
    build = wiiu_frame_publish(build, frames);
    build->n_ta = build->n_ta_ev = 0;   // the next render's input starts here
    scan_vertices = scan_strips = 0;
    scan_pos = 0;
    scan_unlock();
}
void EndRender() {}
// ---- emulator thread: TA input ----------------------------------------------------------------------------
//
// Each 32-byte block is copied into the frame being built, and a scanner follows the parameter stream just far
// enough to raise each list's end-of-list interrupt at the block the real TA would (games wait for them): it
// skips the second half of 64-byte records (headers of polygon types 2 and 4, vertices of polygon types 5, 6 and
// 11-14, sprite and modifier volume vertices) so their data can't be mistaken for a control word, and starts and
// ends lists as ta_main_ctrl does (plugs/drkPvr/ta.h).
static u32 scan_list = ListType_None, scan_skip, scan_vertex = 1;   // scan_vertex: a vertex's size in blocks
static bool scan_sprite;               // vertices are sprites (4 corners each)
u32 wiiu_dropped_ta_blocks;   // TA input that didn't fit a frame (WIIU_MAX_TA_BLOCKS), for the log
u32 wiiu_ta_sq_blocks, wiiu_ta_dma_blocks;   // how the game feeds the TA (status line): store queue vs DMA

// exp_scan_thread (experiment, wiiu.cfg): the scanner runs on core 0; the emulator thread only copies blocks. The thread
// scans what has arrived in batches under scan_lock; at every ordering point (list init/cont, soft reset, render) the
// emulator takes the lock and finishes the scan itself, so nothing is scanned out of order. List-end interrupts go
// through ta_irq_count, which the emulator drains each timeslice (wiiu_ta_poll).
// exp_dma_async (needs exp_scan_thread): a ch2 DMA to the TA only queues its source; the scanner thread copies it too.
// The DMA-done interrupt doesn't wait, so a game reusing the buffer at once would send stale data.
int g_exp_scan_thread, g_exp_dma_async, g_exp_sq_fast;
static const u32 DMA_QUEUE = 256;
static struct { const u32 *data; u32 blocks; } dma_queue[DMA_QUEUE];
static volatile u32 dma_w, dma_r;
static bool scan_threaded;
static volatile bool scan_stop;
static OSFastMutex scan_lock;
static OSThread scan_thread;
// list ends: the scanner counts, the emulator compares (OSOrAtomic does nothing in Cemu)
static volatile u32 ta_irq_count[5];
static u32 ta_irq_seen[5];
static void scan_block(const u32 *b);
void wiiu_ta_poll()
{
    if (!scan_threaded)
        return;
    for (u32 l = 0; l < 5; l++)
        if (ta_irq_seen[l] != ta_irq_count[l])
        {
            ta_irq_seen[l] = ta_irq_count[l];
            spg_SchedListEndIrq(l);
        }
}
static void scan_upto(u32 n)
{
    for (; scan_pos < n; scan_pos++)
        scan_block(build->ta[scan_pos]);
}
// copy up to `max` queued DMA blocks into the frame (under scan_lock)
static void dma_copy(u32 max)
{
    while (dma_r != dma_w && max)
    {
        auto &q = dma_queue[dma_r % DMA_QUEUE];
        const u32 n = q.blocks < max ? q.blocks : max;
        for (u32 i = 0; i < n; i++, q.data += 8)
        {
            if (build->n_ta >= WIIU_MAX_TA_BLOCKS)
            {
                wiiu_dropped_ta_blocks++;
                continue;
            }
            u32 *d = build->ta[build->n_ta++];
            __asm__ volatile("dcbz 0,%0" : : "r"(d) : "memory");
            for (int k = 0; k < 8; k++)
                d[k] = q.data[k];
        }
        q.blocks -= n;
        max -= n;
        if (!q.blocks)
            dma_r = dma_r + 1;
    }
}
static int scan_main(int argc, const char **argv)
{
    while (!scan_stop)
    {
        OSFastMutex_Lock(&scan_lock);
        dma_copy(512);
        const u32 n = build->n_ta;
        scan_upto(n - scan_pos > 512 ? scan_pos + 512 : n);
        OSFastMutex_Unlock(&scan_lock);
        OSSleepTicks(OSMicrosecondsToTicks(50));
    }
    return 0;
}
static void scan_stop_thread()
{
    if (scan_threaded)
    {
        scan_stop = true;
        OSJoinThread(&scan_thread, nullptr);
        scan_threaded = false;
    }
}
static void scan_start()
{
    static u8 stack[64 * 1024] __attribute__((aligned(16)));
    OSFastMutex_Init(&scan_lock, "ta scan");
    if (OSCreateThread(&scan_thread, scan_main, 0, nullptr, stack + sizeof(stack), sizeof(stack), 16,
                       OS_THREAD_ATTRIB_AFFINITY_CPU0))
    {
        OSSetThreadName(&scan_thread, "nullDC TA scan");
        OSResumeThread(&scan_thread);
        scan_threaded = true;
        printf("[ta] scanner thread on core 0\n");
    }
}
// an ordering point: everything received so far scanned, interrupts raised; leaves the lock held for the caller
static void scan_sync_lock()
{
    if (!scan_threaded)
        return;
    OSFastMutex_Lock(&scan_lock);
    dma_copy(~0u);
    scan_upto(build->n_ta);
    wiiu_ta_poll();
}
static void scan_unlock()
{
    if (scan_threaded)
        OSFastMutex_Unlock(&scan_lock);
}

static void scan_reset()
{
    scan_list = ListType_None;
    scan_skip = 0;
    scan_vertex = 1;
}

static void scan_block(const u32 *b)
{
    if (scan_skip)
    {
        scan_skip--;
        return;
    }
    PCW pcw; pcw.full = b[0];
    switch (pcw.ParaType)
    {
    case ParamType_End_Of_List:
        if (scan_list == ListType_None)
            scan_list = pcw.ListType;
        if (scan_threaded)
        {
            if (scan_list < 5)
                ta_irq_count[scan_list] = ta_irq_count[scan_list] + 1;
        }
        else
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
            scan_skip = (uid >> 30) - 1;
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
        scan_skip = scan_vertex - 1;
        if (scan_list != ListType_None && !IsModVolList(scan_list))
        {
            if (scan_sprite)
            {
                scan_vertices += 4;
                scan_strips++;
            }
            else
            {
                scan_vertices++;
                scan_strips += pcw.EndOfStrip;
            }
        }
        break;
    default:
        break;   // user tile clip, object list set, reserved: 32 bytes
    }
}

// ta.cpp's store queue and DMA entry points (instead of the parser)
void wiiu_ta_input(const u32 *data, u32 blocks)
{
    wiiu_ta_dma_blocks += blocks;
    if (scan_threaded && g_exp_dma_async)
    {
        if (dma_w - dma_r >= DMA_QUEUE)
        {
            scan_sync_lock();
            scan_unlock();
        }
        dma_queue[dma_w % DMA_QUEUE] = { data, blocks };
        __asm__ volatile("sync" : : : "memory");
        dma_w = dma_w + 1;
        return;
    }
    for (u32 i = 0; i < blocks; i++, data += 8)
    {
        if (!scan_threaded)
            scan_block(data);
        if (build->n_ta < WIIU_MAX_TA_BLOCKS)
        {
            // each block fills a whole cache line: dcbz claims it without reading memory first (the plain copy
            // missed on every line of a 2 MB buffer: the store queue path stayed at 16-19% after deferring)
            u32 *dst = build->ta[build->n_ta];
            __asm__ volatile("dcbz 0,%0" : : "r"(dst) : "memory");
            for (int k = 0; k < 8; k++)
                dst[k] = data[k];
            if (scan_threaded)
                __asm__ volatile("sync" : : : "memory");   // the scanner must not see the count before the block
            build->n_ta++;
        }
        else
            wiiu_dropped_ta_blocks++;
    }
}

// The JIT's store-queue flush without the MMU (pref @Rn into 0xE0000000-0xE3FFFFFF), instead of do_sqw_nommu,
// TAWriteSQ, libPvr_TaSQ and wiiu_ta_input in turn: TA polygon data, the common case by far (Soulcalibur sends
// ~19,000 blocks a frame this way), goes straight into the frame being built; anything else takes the full path.
#ifdef WIIU_INSTRUMENT
u32 wiiu_sq_other[3];   // store queue blocks not for the TA: RAM, VRAM, anything else
#endif
void FASTCALL do_sqw_nommu(u32 dst);
void FASTCALL wiiu_sqw_fast(u32 dst)
{
    const u32 qacr = (dst & 0x20) == 0 ? CCN_QACR0.Area : CCN_QACR1.Area;
    const u32 address = (dst & 0x03FFFFE0) | (qacr << 26);
    if (((address >> 26) & 7) != 4 || (address & 0x1FFFFFF) >= 0x800000)
    {
        // exp_sq_fast: 32 bytes to RAM or 64-bit VRAM needn't go through the generic block write and memmove
        const u32 area = (address >> 26) & 7;
        u8 *d = !g_exp_sq_fast ? nullptr : area == 3 ? &mem_b[address & RAM_MASK] :
                area == 1 && !(address & 0x01000000) ? &vram[address & VRAM_MASK] : nullptr;
#ifdef WIIU_INSTRUMENT
        wiiu_sq_other[area == 3 ? 0 : area == 1 ? 1 : 2]++;
#endif
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
    WIIU_PROF(PROF_SQW);
    if (dma_r != dma_w)
    {
        // blocks stay in arrival order
        scan_sync_lock();
        scan_unlock();
    }
    const u32 *data = (const u32 *)&sq_both[dst & 0x20];
    wiiu_ta_sq_blocks++;
    if (g_exp_sq_fast && !scan_threaded && build->n_ta < WIIU_MAX_TA_BLOCKS)
    {
        // exp_sq_fast: a block continuing a vertex needs no parsing, and four doubles move it in half the instructions
        if (scan_skip)
            scan_skip--;
        else
            scan_block(data);
        u32 *d = build->ta[build->n_ta++];
        __asm__ volatile("dcbz 0,%0\n\tlfd 0,0(%1)\n\tlfd 1,8(%1)\n\tlfd 2,16(%1)\n\tlfd 3,24(%1)\n\t"
                         "stfd 0,0(%0)\n\tstfd 1,8(%0)\n\tstfd 2,16(%0)\n\tstfd 3,24(%0)"
                         : : "b"(d), "b"(data) : "memory", "fr0", "fr1", "fr2", "fr3");
        return;
    }
    if (!scan_threaded)
        scan_block(data);
    if (build->n_ta < WIIU_MAX_TA_BLOCKS)
    {
        u32 *d = build->ta[build->n_ta];
        __asm__ volatile("dcbz 0,%0" : : "r"(d) : "memory");
        for (int k = 0; k < 8; k++)
            d[k] = data[k];
        if (scan_threaded)
            __asm__ volatile("sync" : : : "memory");
        build->n_ta++;
    }
    else
        wiiu_dropped_ta_blocks++;
}

static void ta_event(u32 what)
{
    if (build->n_ta_ev < WIIU_MAX_TA_EVENTS)
        build->ta_ev[build->n_ta_ev++] = { build->n_ta, what };
}

enum { TA_EV_LIST_CONT = 1, TA_EV_SOFT_RESET = 2, TA_EV_LIST_INIT = 3 };
void ListCont()
{
    scan_sync_lock();
    ta_event(TA_EV_LIST_CONT);
    scan_unlock();
}
void ListInit()
{
    if (g_exp_scan_thread && !scan_threaded)
        scan_start();
    scan_sync_lock();
    // like gxRend.cpp, a list init empties nothing: a render draws all the TA input since the previous one. The BIOS
    // intro sends its swirl after one render starts and its grey panels after the next list init, for the same
    // render; emptying here dropped the swirl. The frame is emptied when StartRender hands it over.
    scan_reset();
    ta_event(TA_EV_LIST_INIT);
    list_init_since_render = true;
    scan_unlock();
}
void SoftReset()
{
    scan_sync_lock();
    scan_reset();
    ta_event(TA_EV_SOFT_RESET);
    scan_unlock();
}

// ---- worker: decode ---------------------------------------------------------------------------------------

extern "C" int get_controller_type();
extern "C" u16 GetLightGunX(int port);
extern "C" u16 GetLightGunY(int port);
float wiiu_depth_cap;   // depth_cap=X (test): the depth scale's largest 1/W at most X
int wiiu_depth_stats;   // depth_stats=1: log the 1/w spread every 60 renders (test setting)
int wiiu_exp_depth_drawn = 1;   // exp_depth_drawn=0: the depth scale from every vertex and volume, as before
int wiiu_dump_render;   // dump_render=N: log the decoded frame at render N
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
    f->streamed_v = wiiu_stream_v;
    f->streamed_i = wiiu_stream_i;
    f->streamed_nv = f->streamed_ns = f->streamed_ni = 0;
    f->max_drawn = 0.0f;
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
    if (wiiu_depth_stats && wiiu_renders % 60 == 0)
    {
        // depth_stats: how far the largest 1/w sits above the bulk (log10 buckets, strips' vertices)
        u32 hist[12] = {};
        for (u32 k = 0; k < f->nv; k++)
        {
            const float z = f->v[k].z;
            int b = z <= 0.0f || z != z ? 0 : z >= 1e30f ? 11 : (int)floorf(log10f(z)) + 5;
            hist[b < 0 ? 0 : b > 11 ? 11 : b]++;
        }
        printf("[depth] render %u max 1/w %g; 1/w by decade 1e-4..1e5: %u %u %u %u %u %u %u %u %u %u, inf %u\n", wiiu_renders,
               f->max_invw, hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7], hist[8], hist[9], hist[10], hist[11]);
    }
    // the depth scale (1 / the largest 1/w) from the polygons drawn: strips of 3 or more vertices. A larger 1/w
    // elsewhere (Jet Grind Radio's shadow volumes: 100000) squeezed the whole scene into depths ~1e-5, where the
    // clip-space conversion rounds them together and near surfaces flicker through each other
    if (wiiu_exp_depth_drawn)
    {
        float m = f->max_drawn;   // streamed: taken strip by strip
        for (u32 i = f->streamed_ns; i < f->ns; i++)
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
        if (wiiu_depth_cap > 0.0f && f->max_invw > wiiu_depth_cap)
            f->max_invw = wiiu_depth_cap;   // test: depth precision for the bulk, nearer vertices clamp
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
    if (get_controller_type() == 1 && GetLightGunX(0) != 0xFFFF && f->nv + 8 <= WIIU_MAX_VERTICES && f->ns + 2 <= WIIU_MAX_STRIPS)
    {
        const float sx = f->canvas_w / 640.0f, sy = f->canvas_h / 480.0f;
        const float cx = GetLightGunX(0) * sx, cy = GetLightGunY(0) * sy, len = 12.0f * sx, th = 1.5f * sx;
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
                f->v[f->nv++] = { c[0], c[1], z, 0xFF3030FFu, 0, 0.0f, 0.0f };
            f->s[f->ns++] = st;
        }
    }
    if (wiiu_dump_render && wiiu_renders >= (u32)wiiu_dump_render)
    {
        wiiu_dump_render = 0;
        printf("[frame] renders %u: %u strips, %u vertices, max 1/w %g\n", wiiu_renders, f->ns, f->nv, f->max_invw);
        for (u32 i = 0; i < f->n_mp; i++)
        {
            const WiiuModVol &m = f->mp[i];
            float zmin = 1e30f, zmax = -1e30f, xmin = 1e30f, xmax = -1e30f, ymin = 1e30f, ymax = -1e30f;
            u32 tiny = 0;
            for (u32 k = m.first * 3; k < (m.first + m.count) * 3 && k < f->n_mv_tris * 3; k++)
            {
                const WiiuMvVertex &v = f->mv[k];
                zmin = v.z < zmin ? v.z : zmin; zmax = v.z > zmax ? v.z : zmax;
                xmin = v.x < xmin ? v.x : xmin; xmax = v.x > xmax ? v.x : xmax;
                ymin = v.y < ymin ? v.y : ymin; ymax = v.y > ymax ? v.y : ymax;
                tiny += !(v.z > 1e-6f);
            }
            printf("[frame] volume %u: last %u, cull %u, first %u, %u triangles, mode %u, 1/w %g..%g (%u <= 1e-6), x %g..%g, y %g..%g\n", i,
                   m.last, m.cull, m.first, m.count, m.mode, zmin, zmax, tiny, xmin, xmax, ymin, ymax);
        }
        for (u32 i = 0; i < f->ns; i++)
        {
            const WiiuStrip &st = f->s[i];
            printf("[frame] strip %u list %u first %u n %u pcw %08x isp %08x tsp %08x tcw %08x\n", i, st.list, st.first,
                   st.count, st.pcw, st.isp, st.tsp, st.tcw);
            {
                // 16-bit textures: how their texels' alpha is spread (ARGB4444's top nibble, ARGB1555's top bit)
                const u32 fmt = (st.tcw >> 27) & 7, addr = (st.tcw << 3) & VRAM_MASK;
                const u32 tw = 8u << ((st.tsp >> 3) & 7), th = 8u << (st.tsp & 7);
                if ((st.pcw >> 3 & 1) && (fmt == 0 || fmt == 2) && !(st.tcw >> 30 & 1))
                {
                    u32 hist[16] = {};
                    for (u32 k = 0; k < tw * th && addr + k * 2 + 1 < VRAM_MASK; k++)
                    {
                        const u32 a = addr + k * 2;
                        const u16 px = *host_ptr_xor((u16 *)&PVRPARAMS.vram[a & VRAM_MASK & ~1u]);
                        hist[fmt == 2 ? px >> 12 : (px >> 15) * 15]++;
                    }
                    printf("[frame]   texture %ux%u fmt %u alpha nibbles 0:%u 1-7:%u 8-14:%u 15:%u\n", tw, th, fmt, hist[0],
                           hist[1] + hist[2] + hist[3] + hist[4] + hist[5] + hist[6] + hist[7],
                           hist[8] + hist[9] + hist[10] + hist[11] + hist[12] + hist[13] + hist[14], hist[15]);
                }
            }
            for (u32 k = 0; k < st.count; k++)
            {
                const WiiuVertex &v = f->v[st.first + k];
                printf("[frame]  %.1f %.1f %g %08x\n", v.x, v.y, v.z, v.col);
            }
        }
    }
    // anything a draw would read out of range (Cemu crashed in its draw handler on a decoded frame)
    static u32 bad;
    for (u32 i = 0; i < f->ns && bad < 8; i++)
    {
        const WiiuStrip &st = f->s[i];
        if (st.count < 3 || st.first + st.count > f->nv || (st.list > 4 && st.list != WIIU_LIST_BACKGROUND))
        {
            bad++;
            printf("[ta] frame of %u blocks: strip %u of %u: first %u count %u list %u, %u vertices\n", f->n_ta, i, f->ns,
                   st.first, st.count, st.list, f->nv);
        }
    }
    wiiu_decode_ticks[2] += OSGetSystemTime() - d2;
    f->decoded = true;
    dec = nullptr;
}
void SetFpsText(char *text) {}
void SetFbScale(float x, float y) { fb_scale_x = x; fb_scale_y = y; }
void strip_dedup_dump(double tdiff) {}
