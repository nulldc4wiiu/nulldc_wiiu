// One PVR frame as collected from the TA: vertices in Dreamcast screen space (x, y in pixels, z = 1/w) and
// polygon strips with their state words. Built on the emulator thread, drawn on the GX2 thread.
#pragma once
#include "types.h"
#include "plugs/drkPvr/drkPvr.h"

// Dreamcast VRAM is two interleaved 32-bit banks; framebuffer and parameter addresses use the 32-bit view, the
// emulator stores the 64-bit (texture) layout
static inline u32 vram_offset64(u32 offset32)
{
    offset32 &= VRAM_MASK;
    return ((offset32 & 0x3FFFFC) << 1) | (((offset32 >> 22) & 1) << 2) | (offset32 & 3);
}

// strips in this list are the background plane, drawn first as the clear
static const u32 WIIU_LIST_BACKGROUND = 8;

static const u32 WIIU_MAX_VERTICES = 65536, WIIU_MAX_STRIPS = 16384;
static const u32 WIIU_MAX_MV_TRIS = 8192, WIIU_MAX_MV_PARAMS = 4096;
// the TA input of a frame, kept raw (32-byte blocks) and decoded on the worker core (render_gx2.cpp)
static const u32 WIIU_MAX_TA_BLOCKS = 65536, WIIU_MAX_TA_EVENTS = 64;

struct WiiuVertex
{
    float x, y, z;
    u32 col, spc;   // bytes R, G, B, A
    float u, v;
};

struct WiiuStrip
{
    u32 first, count, list;
    u32 pcw, isp, tsp, tcw;
    // user tile clip: mode (2 draw inside, 3 outside, else off) in bits 31..24, then the rectangle in 32-pixel
    // tiles, inclusive: x min 23..18, y min 17..12, x max 11..6, y max 5..0
    u32 clip;
};

// one modifier volume header from the opaque modifier list and the triangles after it, [first, first + count)
// in WiiuFrame::mv; mode (its ISP word's top bits) 1 or 2 closes a volume, counting pixels inside or outside it
struct WiiuModVol
{
    u32 first, count, mode, cull;   // mode: ISP volume instruction (1 inside, 2 outside: closes it); cull: ISP cull mode
};

struct WiiuMvVertex
{
    float x, y, z;
};

struct WiiuFrame
{
    WiiuVertex v[WIIU_MAX_VERTICES];
    WiiuStrip s[WIIU_MAX_STRIPS];
    u32 nv, ns;
    float max_invw;
    WiiuMvVertex mv[WIIU_MAX_MV_TRIS * 3];
    WiiuModVol mp[WIIU_MAX_MV_PARAMS];
    u32 n_mv_tris, n_mp;
    u32 fpu_shad_scale;   // bit 8: volumes shade (intensity in bits 7..0) rather than select a second parameter set
    u32 isp_feed_cfg;   // bit 0: translucent polygons come pre-sorted
    // render to texture (FB_W_SOF1 bit 24): the frame's pixels go to VRAM at rtt_addr (64-bit texture layout) in
    // FB_W_CTRL's format, rtt_stride bytes per line, instead of to the screen
    u32 rtt, rtt_addr, rtt_ctrl, rtt_stride, rtt_w, rtt_h;
    // the screen-space canvas the geometry was submitted in: 640x480, or as the video mode and scalers make it
    float canvas_w, canvas_h;
    // fog state at StartRender
    u16 fog_table[128];
    u32 fog_col_ram, fog_col_vert, fog_clamp_min, fog_clamp_max;
    float fog_density;
    // The TA blocks as the game sent them since its list init, and the list continue / soft reset register
    // writes among them (before block ta_ev[i].at). The emulator thread only collects these (decoding them there
    // was ~23% of its time in Soulcalibur fights); wiiu_ta_decode turns them into v, s, mv and mp above.
    u32 ta[WIIU_MAX_TA_BLOCKS][8] __attribute__((aligned(32)));   // one cache line each (wiiu_ta_input)
    u32 n_ta;
    struct { u32 at, what; } ta_ev[WIIU_MAX_TA_EVENTS];
    u32 n_ta_ev;
    bool decoded;
    // the background plane (ISP_BACKGND_T), read at StartRender and added after the decoded geometry
    WiiuVertex bg_v[4];
    WiiuStrip bg_s;
    bool has_bg;
};

// worker core (or the GX2 thread with the worker idle): decode a published frame's TA blocks, once
void wiiu_ta_decode(WiiuFrame *f);

// a texture image decoded for a scene, whose GPU copy the GX2 thread invalidates before drawing it
struct WiiuTexInval
{
    void *image;
    u32 bytes;
};

// emulator thread: publish the finished frame, get one of the four buffers to build the next one in
// (present_gx2.cpp). A render-to-texture frame waits for the GX2 thread to take the previous one.
WiiuFrame *wiiu_frame_publish(WiiuFrame *done, WiiuFrame *frames);
// emulator thread, every VBlank: copy the scanned-out framebuffer for the 2D path
void wiiu_fb_capture();
