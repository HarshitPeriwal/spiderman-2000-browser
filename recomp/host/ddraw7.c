/* DirectDraw 7 + Direct3D 7 (immediate mode) for Spider-Man (2000), on the shared renderer.
 *
 * What the game uses (DXinit.cpp, DXsound.cpp, PCTex.cpp and PCMovie.cpp in spidey-decomp):
 *   - DirectDrawEnumerateExA, DirectDrawCreateEx, IDirectDraw7 (modes, cooperative level, surfaces,
 *     clipper, device identifier, video memory)
 *   - a primary surface with a flip chain (fullscreen) or a primary plus an offscreen 3D scene surface
 *     that is Blt to the primary (windowed); a 16-bit z-buffer attached to the scene surface
 *   - IDirect3D7: EnumDevices (it prefers T&L HAL > HAL > RGB), EnumZBufferFormats, CreateDevice
 *   - IDirect3DDevice7: every polygon is one DrawPrimitive(D3DPT_TRIANGLEFAN, FVF 0x144 =
 *     XYZRHW | DIFFUSE | TEX1) of pre-transformed vertices, plus Clear, SetTexture, render and texture
 *     stage states, viewport, BeginScene/EndScene
 *   - textures: a lockable staging surface is filled and Blt into a texture surface
 *   - movies: Bink decodes into a locked offscreen surface that is Blt onto the scene
 *
 * The renderer (render_rec.c) is shared with the Direct3D 8 layer and works on d3d8.h's Device,
 * Texture and Surface records. D3D7 render state and texture stage state numbers are the same as
 * Direct3D 8's for everything the game sets, except D3DTSS_ADDRESS (12, D3D7 only), which sets both
 * ADDRESSU and ADDRESSV. Each DirectDraw surface used as a texture gets a Texture "view" object that
 * the renderer reads; the game only ever sees the surface object.
 */
#include "d3d8.h"
#include "render.h"

#include <stdlib.h>
#include <string.h>

#define W(name) static void name(CPU *c)

#define DD_OK 0u
#define DDERR_GENERIC 0x80004005u
#define DDERR_INVALIDPARAMS 0x80070057u
#define DDERR_NOTFOUND 0x887600FFu       /* MAKE_DDHRESULT(255) */
#define DDERR_SURFACELOST 0x887601C2u

/* DDSCAPS */
#define DDSCAPS_BACKBUFFER     0x00000004u
#define DDSCAPS_COMPLEX        0x00000008u
#define DDSCAPS_FLIP           0x00000010u
#define DDSCAPS_FRONTBUFFER    0x00000020u
#define DDSCAPS_OFFSCREENPLAIN 0x00000040u
#define DDSCAPS_PRIMARYSURFACE 0x00000200u
#define DDSCAPS_SYSTEMMEMORY   0x00000800u
#define DDSCAPS_TEXTURE        0x00001000u
#define DDSCAPS_3DDEVICE       0x00002000u
#define DDSCAPS_VIDEOMEMORY    0x00004000u
#define DDSCAPS_ZBUFFER        0x00020000u
#define DDSCAPS_MIPMAP         0x00400000u
#define DDSCAPS_LOCALVIDMEM    0x10000000u
/* DDSURFACEDESC2.dwFlags */
#define DDSD_CAPS            0x00000001u
#define DDSD_HEIGHT          0x00000002u
#define DDSD_WIDTH           0x00000004u
#define DDSD_PITCH           0x00000008u
#define DDSD_BACKBUFFERCOUNT 0x00000020u
#define DDSD_ZBUFFERBITDEPTH 0x00000040u
#define DDSD_LPSURFACE       0x00000800u
#define DDSD_PIXELFORMAT     0x00001000u
#define DDSD_CKSRCBLT        0x00010000u
#define DDSD_REFRESHRATE     0x00040000u
/* DDPIXELFORMAT.dwFlags */
#define DDPF_ALPHAPIXELS 0x00000001u
#define DDPF_RGB         0x00000040u
#define DDPF_ZBUFFER     0x00000400u
/* Blt */
#define DDBLT_COLORFILL 0x00000400u
#define DDBLT_KEYSRC    0x00008000u
#define DDCKEY_SRCBLT   0x00000008u

/* ------------------------------------------------------------------ pixel formats */

typedef struct { uint32_t flags, bits, r, g, b, a; } PixFmt;

static const PixFmt PF_565  = { DDPF_RGB, 16, 0xF800, 0x07E0, 0x001F, 0 };
static const PixFmt PF_X555 = { DDPF_RGB, 16, 0x7C00, 0x03E0, 0x001F, 0 };
static const PixFmt PF_1555 = { DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x7C00, 0x03E0, 0x001F, 0x8000 };
static const PixFmt PF_4444 = { DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x0F00, 0x00F0, 0x000F, 0xF000 };
static const PixFmt PF_X888 = { DDPF_RGB, 32, 0xFF0000, 0x00FF00, 0x0000FF, 0 };
static const PixFmt PF_8888 = { DDPF_RGB | DDPF_ALPHAPIXELS, 32, 0xFF0000, 0x00FF00, 0x0000FF, 0xFF000000u };
static const PixFmt PF_888  = { DDPF_RGB, 24, 0xFF0000, 0x00FF00, 0x0000FF, 0 };

static void write_pixfmt(uint32_t p, const PixFmt *f) {
    W32(p + 0, 32); W32(p + 4, f->flags); W32(p + 8, 0); W32(p + 12, f->bits);
    W32(p + 16, f->r); W32(p + 20, f->g); W32(p + 24, f->b); W32(p + 28, f->a);
}

static PixFmt read_pixfmt(uint32_t p) {
    PixFmt f = { R32(p + 4), R32(p + 12), R32(p + 16), R32(p + 20), R32(p + 24), R32(p + 28) };
    if (!(f.flags & DDPF_ALPHAPIXELS)) f.a = 0;
    return f;
}

/* D3DFORMAT equivalent for the renderer's texture converter */
static uint32_t pixfmt_d3d(const PixFmt *f) {
    if (f->bits == 16 && f->g == 0x07E0) return FMT_R5G6B5;
    if (f->bits == 16 && f->a == 0x8000) return FMT_A1R5G5B5;
    if (f->bits == 16 && f->a == 0xF000) return FMT_A4R4G4B4;
    if (f->bits == 16) return FMT_X1R5G5B5;
    if (f->bits == 32) return f->a ? FMT_A8R8G8B8 : FMT_X8R8G8B8;
    if (f->bits == 24) return FMT_R8G8B8;
    return FMT_UNKNOWN;
}

static int mask_shift(uint32_t m) { int s = 0; if (!m) return 0; while (!(m & 1)) { m >>= 1; s++; } return s; }
static uint32_t mask_bits(uint32_t m) { uint32_t n = 0; for (; m; m &= m - 1) n++; return n; }

/* one channel to and from 8 bits, replicating high bits as hardware expands them */
static uint32_t chan_get(uint32_t px, uint32_t m) {
    if (!m) return 255;
    uint32_t n = mask_bits(m), v = (px & m) >> mask_shift(m);
    return n >= 8 ? v >> (n - 8) : (v << (8 - n)) | (v >> (2 * n >= 8 ? 2 * n - 8 : 0));
}
static uint32_t chan_put(uint32_t v8, uint32_t m) {
    if (!m) return 0;
    uint32_t n = mask_bits(m);
    return ((n >= 8 ? v8 << (n - 8) : v8 >> (8 - n)) << mask_shift(m)) & m;
}
static uint32_t px_to_argb(uint32_t px, const PixFmt *f) {
    return chan_get(px, f->a) << 24 | chan_get(px, f->r) << 16 | chan_get(px, f->g) << 8 | chan_get(px, f->b);
}
static uint32_t argb_to_px(uint32_t c, const PixFmt *f) {
    return chan_put(c >> 24, f->a) | chan_put((c >> 16) & 255, f->r) | chan_put((c >> 8) & 255, f->g) | chan_put(c & 255, f->b);
}
static uint32_t px_read(uint32_t a, uint32_t bytes) {
    return bytes == 2 ? R16(a) : bytes == 4 ? R32(a) : bytes == 3 ? (uint32_t)R16(a) | (uint32_t)R8(a + 2) << 16 : R8(a);
}
static void px_write(uint32_t a, uint32_t bytes, uint32_t v) {
    if (bytes == 2) W16(a, (uint16_t)v);
    else if (bytes == 4) W32(a, v);
    else if (bytes == 3) { W16(a, (uint16_t)v); W8(a + 2, (uint8_t)(v >> 16)); }
    else W8(a, (uint8_t)v);
}

/* ------------------------------------------------------------------ objects */

enum { SK_PLAIN, SK_TEXTURE, SK_PRIMARY, SK_BACK, SK_SCENE, SK_ZBUF };

typedef struct DDSurf {
    int kind;
    uint32_t caps, w, h, pitch, bytes;
    PixFmt pf;
    uint32_t mem;              /* guest pixels (render targets keep a CPU copy for Lock) */
    uint32_t attached;         /* back buffer of a flip chain, or z-buffer of a scene surface */
    uint32_t clipper;
    uint32_t ckey_lo, ckey_hi; int has_ckey;
    Texture tex;               /* view the renderer samples when this surface is bound as a texture */
    uint32_t tex_obj;          /* guest object whose data is &tex (created on first use) */
    Surface rt;                /* view the renderer uses when this surface is the device's target */
    uint32_t self;             /* the game's surface object */
} DDSurf;

static ComInterface IDirectDraw7_i, IDirectDrawSurface7_i, IDirectDrawClipper_i, IDirect3D7_i,
    IDirect3DDevice7_i, TextureView_i, SurfaceView_i;

static struct {
    uint32_t mode_w, mode_h, mode_bpp;
    int exclusive;
    uint32_t dd, d3d, device_obj, target;   /* guest objects; target = the device's render surface */
    Device dev;
    int dev_ready;
} g_dd = { 640, 480, 16 };

static DDSurf *surf(uint32_t obj) { ComObj *o = com_get(obj); return o && o->iface == &IDirectDrawSurface7_i ? o->data : NULL; }

static void surface_set_format(DDSurf *s, const PixFmt *f) {
    s->pf = *f;
    s->bytes = f->bits / 8 ? f->bits / 8 : 1;
    s->pitch = (s->w * s->bytes + 3) & ~3u;
}

static uint32_t new_surface(int kind, uint32_t caps, uint32_t w, uint32_t h, const PixFmt *f) {
    DDSurf *s = calloc(1, sizeof *s);
    s->kind = kind; s->caps = caps; s->w = w ? w : 1; s->h = h ? h : 1;
    surface_set_format(s, f);
    if (kind != SK_ZBUF) s->mem = heap_alloc(s->pitch * s->h + 16, 1);
    s->rt.gpu_id = gpu_new_id();
    s->rt.format = pixfmt_d3d(f); s->rt.width = s->w; s->rt.height = s->h; s->rt.pitch = s->pitch;
    s->rt.mem = s->mem; s->rt.is_rt = kind == SK_BACK || kind == SK_SCENE || kind == SK_PRIMARY;
    s->rt.is_depth = kind == SK_ZBUF;
    s->self = com_new(&IDirectDrawSurface7_i, RT_SURFACE, s);
    LOG("[ddraw] surface %08x kind %d %ux%u %u-bit caps %08x\n", s->self, kind, s->w, s->h, f->bits, caps);
    return s->self;
}

/* the renderer's Texture view of a surface; the version changes whenever its pixels do */
static uint32_t texture_view(DDSurf *s) {
    Texture *t = &s->tex;
    if (!s->tex_obj) {
        t->gpu_id = gpu_new_id();
        t->sent_version = 0xFFFFFFFFu;
        t->type = RT_TEXTURE;
        t->levels = 1; t->faces = 1;
        s->tex_obj = com_new(&TextureView_i, RT_TEXTURE, t);
    }
    t->format = pixfmt_d3d(&s->pf);
    t->width = t->lw[0] = s->w; t->height = t->lh[0] = s->h;
    t->pitch[0] = s->pitch; t->size[0] = s->pitch * s->h;
    t->mem[0][0] = s->mem;
    return s->tex_obj;
}
static void touch(DDSurf *s) { s->tex.version++; }

static int is_target(DDSurf *s) { return s && g_dd.target && (s->self == g_dd.target || surf(g_dd.target)->attached == s->self); }

/* ------------------------------------------------------------------ DirectDraw entry points */

static void guid_write(uint32_t p, uint32_t d1, uint16_t d2, uint16_t d3, const uint8_t d4[8]) {
    W32(p, d1); W16(p + 4, d2); W16(p + 6, d3);
    for (int i = 0; i < 8; i++) W8(p + 8 + i, d4[i]);
}
static int guid_is(uint32_t p, uint32_t d1, uint16_t d2, uint16_t d3, const uint8_t d4[8]) {
    if (!p || R32(p) != d1 || R16(p + 4) != d2 || R16(p + 6) != d3) return 0;
    for (int i = 0; i < 8; i++) if (R8(p + 8 + i) != d4[i]) return 0;
    return 1;
}
static const uint8_t IID_D3D7_4[8] = { 0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8 };            /* f5049e77-4861-11d2- */
static const uint8_t IID_HAL_4[8] = { 0x81, 0x6f, 0x00, 0x00, 0xc0, 0x20, 0x15, 0x6e };             /* 84e63de0-46aa-11cf- */
static const uint8_t IID_RGB_4[8] = { 0xa3, 0x1a, 0x00, 0xaa, 0x00, 0xb9, 0x33, 0x56 };             /* a4665c60-2673-11cf- */

/* DirectDrawEnumerateExA(lpCallback, lpContext, dwFlags): one display driver, the primary */
void w_DirectDrawEnumerateExA(CPU *c) {
    uint32_t cb = ARG(0), ctx = ARG(1);
    static uint32_t desc, name;
    if (!desc) { desc = guest_strdup("Primary Display Driver"); name = guest_strdup("display"); }
    uint32_t a[5] = { 0, desc, name, ctx, 0 };
    guest_stdcall(c, cb, 5, a);
    RET(3, DD_OK);
}

/* DirectDrawCreateEx(lpGuid, lplpDD, iid, pUnkOuter) */
void w_DirectDrawCreateEx(CPU *c) {
    if (!g_dd.dd) g_dd.dd = com_new(&IDirectDraw7_i, RT_D3D, &g_dd);
    W32(ARG(1), g_dd.dd);
    fprintf(stderr, "[ddraw] DirectDrawCreateEx -> DirectDraw 7 on the %s renderer\n", g_render->name);
    RET(4, DD_OK);
}

/* ------------------------------------------------------------------ IDirectDraw7 */

W(dd_QueryInterface) {
    if (guid_is(ARG(1), 0xf5049e77, 0x4861, 0x11d2, IID_D3D7_4)) {
        if (!g_dd.d3d) g_dd.d3d = com_new(&IDirect3D7_i, RT_D3D, &g_dd);
        W32(ARG(2), g_dd.d3d);
        RET(3, S_OK);
    }
    W32(ARG(2), THIS);
    RET(3, S_OK);
}

W(dd_SetCooperativeLevel) {
    g_dd.exclusive = (ARG(2) & 0x10) != 0;   /* DDSCL_EXCLUSIVE */
    LOG("[ddraw] SetCooperativeLevel(flags %#x)\n", ARG(2));
    RET(3, DD_OK);
}

W(dd_SetDisplayMode) {
    g_dd.mode_w = ARG(1); g_dd.mode_h = ARG(2); g_dd.mode_bpp = ARG(3);
    fprintf(stderr, "[ddraw] SetDisplayMode(%ux%u, %u bpp)\n", ARG(1), ARG(2), ARG(3));
    RET(6, DD_OK);
}
W(dd_RestoreDisplayMode) { RET(1, DD_OK); }

static const PixFmt *mode_format(uint32_t bpp) { return bpp == 32 ? &PF_X888 : bpp == 24 ? &PF_888 : &PF_565; }

static void write_mode_desc(uint32_t d, uint32_t w, uint32_t h, uint32_t bpp) {
    for (int i = 0; i < 124; i += 4) W32(d + i, 0);
    W32(d + 0, 124);
    W32(d + 4, DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE);
    W32(d + 8, h); W32(d + 12, w); W32(d + 16, w * (bpp / 8)); W32(d + 24, 60);
    write_pixfmt(d + 72, mode_format(bpp));
}

/* EnumDisplayModes(dwFlags, lpDDSD2, lpContext, lpCallback): the game keeps the sizes of its own table */
W(dd_EnumDisplayModes) {
    static const uint32_t modes[][2] = { { 512, 384 }, { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 1024 } };
    static const uint32_t bpps[] = { 16, 32 };
    uint32_t ctx = ARG(3), cb = ARG(4), d = heap_alloc(124, 1);
    for (unsigned b = 0; b < 2; b++)
        for (unsigned m = 0; m < sizeof modes / sizeof modes[0]; m++) {
            write_mode_desc(d, modes[m][0], modes[m][1], bpps[b]);
            uint32_t a[2] = { d, ctx };
            if (!guest_stdcall(c, cb, 2, a)) goto done;   /* DDENUMRET_CANCEL */
        }
done:
    heap_free(d);
    RET(5, DD_OK);
}

W(dd_GetDisplayMode) { write_mode_desc(ARG(1), g_dd.mode_w, g_dd.mode_h, g_dd.mode_bpp); RET(2, DD_OK); }

W(dd_GetAvailableVidMem) {
    if (ARG(2)) W32(ARG(2), 64u << 20);
    if (ARG(3)) W32(ARG(3), 60u << 20);
    RET(4, DD_OK);
}

/* GetDeviceIdentifier(lpdddi, dwFlags): DDDEVICEIDENTIFIER2, 1068 bytes */
W(dd_GetDeviceIdentifier) {
    uint32_t p = ARG(1);
    for (int i = 0; i < 1068; i += 4) W32(p + i, 0);
    strcpy((char *)HP(p), "browser.drv");
    strcpy((char *)HP(p + 512), "Browser WebGPU renderer");
    RET(3, DD_OK);
}

W(dd_CreateClipper) { W32(ARG(2), com_new(&IDirectDrawClipper_i, RT_SURFACE, NULL)); RET(4, DD_OK); }
W(dd_WaitForVerticalBlank) { RET(3, DD_OK); }
W(dd_TestCooperativeLevel) { RET(1, DD_OK); }
W(dd_RestoreAllSurfaces) { RET(1, DD_OK); }

/* CreateSurface(lpDDSD2, lplpDDS, pUnkOuter) */
W(dd_CreateSurface) {
    uint32_t d = ARG(1), flags = R32(d + 4), caps = R32(d + 104);
    uint32_t w = (flags & DDSD_WIDTH) ? R32(d + 12) : g_dd.mode_w;
    uint32_t h = (flags & DDSD_HEIGHT) ? R32(d + 8) : g_dd.mode_h;
    PixFmt f = (flags & DDSD_PIXELFORMAT) ? read_pixfmt(d + 72) : *mode_format(g_dd.mode_bpp);
    uint32_t obj;
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        obj = new_surface(SK_PRIMARY, caps, g_dd.mode_w, g_dd.mode_h, mode_format(g_dd.mode_bpp));
        if ((caps & DDSCAPS_FLIP) && (flags & DDSD_BACKBUFFERCOUNT)) {
            uint32_t back = new_surface(SK_BACK, DDSCAPS_BACKBUFFER | DDSCAPS_3DDEVICE, g_dd.mode_w, g_dd.mode_h,
                                        mode_format(g_dd.mode_bpp));
            surf(obj)->attached = back;
        }
    } else if (caps & DDSCAPS_ZBUFFER) {
        PixFmt z = { DDPF_ZBUFFER, (flags & DDSD_PIXELFORMAT) ? R32(d + 72 + 12) : 16, 0, 0, 0, 0 };
        obj = new_surface(SK_ZBUF, caps, w, h, &z);
    } else if (caps & DDSCAPS_TEXTURE) {
        obj = new_surface(SK_TEXTURE, caps, w, h, &f);
    } else if (caps & DDSCAPS_3DDEVICE) {
        obj = new_surface(SK_SCENE, caps, w, h, &f);
    } else {
        obj = new_surface(SK_PLAIN, caps, w, h, &f);
    }
    if (flags & DDSD_CKSRCBLT) {
        DDSurf *s = surf(obj);
        s->has_ckey = 1; s->ckey_lo = R32(d + 64); s->ckey_hi = R32(d + 68);
    }
    W32(ARG(2), obj);
    RET(4, DD_OK);
}

static const ComMethod dd_methods[] = {
    { "QueryInterface", 2, dd_QueryInterface }, { "AddRef", 0, com_AddRef }, { "Release", 0, com_Release },
    { "Compact", 0, NULL }, { "CreateClipper", 3, dd_CreateClipper }, { "CreatePalette", 4, NULL },
    { "CreateSurface", 3, dd_CreateSurface }, { "DuplicateSurface", 2, NULL },
    { "EnumDisplayModes", 4, dd_EnumDisplayModes }, { "EnumSurfaces", 4, NULL }, { "FlipToGDISurface", 0, NULL },
    { "GetCaps", 2, NULL }, { "GetDisplayMode", 1, dd_GetDisplayMode }, { "GetFourCCCodes", 2, NULL },
    { "GetGDISurface", 1, NULL }, { "GetMonitorFrequency", 1, NULL }, { "GetScanLine", 1, NULL },
    { "GetVerticalBlankStatus", 1, NULL }, { "Initialize", 1, NULL }, { "RestoreDisplayMode", 0, dd_RestoreDisplayMode },
    { "SetCooperativeLevel", 2, dd_SetCooperativeLevel }, { "SetDisplayMode", 5, dd_SetDisplayMode },
    { "WaitForVerticalBlank", 2, dd_WaitForVerticalBlank }, { "GetAvailableVidMem", 3, dd_GetAvailableVidMem },
    { "GetSurfaceFromDC", 2, NULL }, { "RestoreAllSurfaces", 0, dd_RestoreAllSurfaces },
    { "TestCooperativeLevel", 0, dd_TestCooperativeLevel }, { "GetDeviceIdentifier", 2, dd_GetDeviceIdentifier },
    { "StartModeTest", 3, NULL }, { "EvaluateMode", 2, NULL },
};
static ComInterface IDirectDraw7_i = { "IDirectDraw7", dd_methods, 30, 0 };

static const ComMethod clip_methods[] = {
    { "QueryInterface", 2, com_QueryInterface }, { "AddRef", 0, com_AddRef }, { "Release", 0, com_Release },
    { "GetClipList", 3, NULL }, { "GetHWnd", 1, NULL }, { "Initialize", 2, NULL }, { "IsClipListChanged", 1, NULL },
    { "SetClipList", 2, NULL }, { "SetHWnd", 2, NULL },
};
static ComInterface IDirectDrawClipper_i = { "IDirectDrawClipper", clip_methods, 9, 0 };

static const ComMethod view_methods[] = {
    { "QueryInterface", 2, com_QueryInterface }, { "AddRef", 0, com_AddRef }, { "Release", 0, com_Release },
};
static ComInterface TextureView_i = { "TextureView", view_methods, 3, 0 };
static ComInterface SurfaceView_i = { "SurfaceView", view_methods, 3, 0 };

/* ------------------------------------------------------------------ IDirectDrawSurface7 */

static DDSurf *me(CPU *c) { return com_data(THIS); }

/* RC_LOG_FPS: every 2 s, how many of each operation ran and how long present() took */
static struct { uint32_t present, blit, lock, readback, mem_blt; double present_s, t0; int on; } g_stats = { .on = -1 };
static void stats_tick(void) {
    if (g_stats.on < 0) { g_stats.on = getenv("RC_LOG_FPS") != NULL; g_stats.t0 = host_seconds(); }
    if (!g_stats.on) return;
    double t = host_seconds();
    if (t - g_stats.t0 < 2.0) return;
    fprintf(stderr, "[d3d7] %.1f s: %u presents (%.1f ms each), %u scene blits, %u mem blts, %u locks, %u readbacks\n",
            t - g_stats.t0, g_stats.present, g_stats.present ? 1000.0 * g_stats.present_s / g_stats.present : 0.0,
            g_stats.blit, g_stats.mem_blt, g_stats.lock, g_stats.readback);
    memset(&g_stats, 0, sizeof g_stats);
    g_stats.on = 1; g_stats.t0 = t;
}

static void present(void) {
    if (!g_dd.dev_ready) return;
    Device *d = &g_dd.dev;
    double tp = host_seconds();
    if (g_render->present) g_render->present(d);
    g_stats.present++; g_stats.present_s += host_seconds() - tp;
    stats_tick();
    static int log_fps = -1;
    static uint32_t draws;
    static double t0;
    if (log_fps < 0) { log_fps = getenv("RC_LOG_FPS") != NULL; t0 = host_seconds(); }
    draws += d->draws_this_frame;
    if (log_fps && d->frame % 60 == 59) {
        double t = host_seconds();
        fprintf(stderr, "[d3d7] frame %u: %.1f fps, %.0f draws/frame\n", d->frame + 1, 60.0 / (t - t0), draws / 60.0);
        t0 = t;
        draws = 0;
    }
    d->frame++;
    d->draws_this_frame = 0;
    extern void input_script_tick(uint32_t frame), dump_tick(uint32_t frame);
    input_script_tick(d->frame);   /* RC_INPUT scripted test input, RC_DUMP memory dumps */
    dump_tick(d->frame);
}

/* CPU access to a render target: fetch what the GPU drew first, push the CPU's pixels back after */
static void target_readback(DDSurf *s) { g_stats.readback++; if (g_dd.dev_ready && g_render->readback) g_render->readback(&g_dd.dev, com_data(g_dd.dev.backbuffer)); (void)s; }
static void target_upload(DDSurf *s) { if (g_dd.dev_ready && g_render->upload_surface) g_render->upload_surface(&g_dd.dev, com_data(g_dd.dev.backbuffer)); (void)s; }

W(s_GetAttachedSurface) {
    DDSurf *s = me(c);
    uint32_t want = R32(ARG(1));
    DDSurf *a = s->attached ? surf(s->attached) : NULL;
    if (a && ((want & DDSCAPS_BACKBUFFER) ? a->kind == SK_BACK : (want & DDSCAPS_ZBUFFER) ? a->kind == SK_ZBUF : 1)) {
        com_get(s->attached)->refs++;
        W32(ARG(2), s->attached);
        RET(3, DD_OK);
    }
    W32(ARG(2), 0);
    RET(3, DDERR_NOTFOUND);
}

W(s_AddAttachedSurface) { me(c)->attached = ARG(1); RET(2, DD_OK); }
W(s_DeleteAttachedSurface) { me(c)->attached = 0; RET(3, DD_OK); }
W(s_SetClipper) { me(c)->clipper = ARG(1); RET(2, DD_OK); }
W(s_IsLost) { RET(1, DD_OK); }
W(s_Restore) { RET(1, DD_OK); }

static void write_desc(DDSurf *s, uint32_t d) {
    for (int i = 4; i < 124; i += 4) W32(d + i, 0);
    W32(d + 0, 124);
    W32(d + 4, DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT);
    W32(d + 8, s->h); W32(d + 12, s->w); W32(d + 16, s->pitch);
    write_pixfmt(d + 72, &s->pf);
    if (s->kind == SK_ZBUF) { W32(d + 72 + 4, DDPF_ZBUFFER); W32(d + 72 + 12, s->pf.bits); W32(d + 72 + 20, 0xFFFF); }
    W32(d + 104, s->caps);
}

W(s_GetSurfaceDesc) { write_desc(me(c), ARG(1)); RET(2, DD_OK); }
W(s_GetPixelFormat) { write_pixfmt(ARG(1), &me(c)->pf); RET(2, DD_OK); }

W(s_GetCaps) {
    uint32_t p = ARG(1);
    W32(p, me(c)->caps); W32(p + 4, 0); W32(p + 8, 0); W32(p + 12, 0);
    RET(2, DD_OK);
}

/* Lock(lpDestRect, lpDDSD2, dwFlags, hEvent) */
W(s_Lock) {
    DDSurf *s = me(c);
    uint32_t rect = ARG(1), d = ARG(2);
    g_stats.lock++;
    if (!s->mem) RET(5, DDERR_GENERIC);
    if (s->kind == SK_PRIMARY || is_target(s)) target_readback(s);
    write_desc(s, d);
    W32(d + 4, R32(d + 4) | DDSD_LPSURFACE);
    uint32_t p = s->mem;
    if (rect) p += R32(rect + 4) * s->pitch + R32(rect) * s->bytes;   /* RECT: left, top, right, bottom */
    W32(d + 36, p);
    RET(5, DD_OK);
}

W(s_Unlock) {
    DDSurf *s = me(c);
    touch(s);
    if (is_target(s)) target_upload(s);
    RET(2, DD_OK);
}

W(s_SetColorKey) {
    DDSurf *s = me(c);
    if (ARG(1) & DDCKEY_SRCBLT) {
        s->has_ckey = ARG(2) != 0;
        if (ARG(2)) { s->ckey_lo = R32(ARG(2)); s->ckey_hi = R32(ARG(2) + 4); }
    }
    RET(3, DD_OK);
}

W(s_GetColorKey) {
    DDSurf *s = me(c);
    if (!s->has_ckey) RET(3, 0x887600D7u /* DDERR_NOCOLORKEY */);
    W32(ARG(2), s->ckey_lo); W32(ARG(2) + 4, s->ckey_hi);
    RET(3, DD_OK);
}

/* Draws `src` stretched onto the device's target (dest rect in target pixels), with blending, depth and
   culling off and the previous device state restored afterwards. */
static void blit_to_target(DDSurf *src, uint32_t sx0, uint32_t sy0, uint32_t sx1, uint32_t sy1,
                           float dx0, float dy0, float dx1, float dy1) {
    Device *d = &g_dd.dev;
    if (!g_dd.dev_ready || !g_render->draw) return;
    DevState saved = d->st;
    texture_view(src);
    d->st.texture[0] = src->tex_obj;
    for (int s = 1; s < 8; s++) d->st.texture[s] = 0;
    d->st.vs = 0x144;
    d->st.rs[7] = 0;  d->st.rs[14] = 0;           /* ZENABLE, ZWRITEENABLE */
    d->st.rs[27] = 0; d->st.rs[15] = 0;           /* ALPHABLENDENABLE, ALPHATESTENABLE */
    d->st.rs[22] = 1; d->st.rs[28] = 0;           /* CULLMODE none, FOGENABLE */
    uint32_t *t = d->st.tss[0];
    t[1] = 2; t[2] = 2; t[4] = 2; t[5] = 2;      /* COLOR/ALPHAOP selectarg1 texture */
    t[13] = t[14] = 3;                            /* clamp */
    int scaled = (sx1 - sx0) != (uint32_t)(dx1 - dx0) || (sy1 - sy0) != (uint32_t)(dy1 - dy0);
    t[16] = t[17] = scaled ? 2 : 1;               /* MAG/MINFILTER linear when stretching */
    d->st.tss[1][1] = d->st.tss[1][4] = 1;        /* stage 1 disabled */
    d->st.viewport.x = 0; d->st.viewport.y = 0; d->st.viewport.w = d->width; d->st.viewport.h = d->height;
    d->st.viewport.minz = 0; d->st.viewport.maxz = 1;
    float u0 = (float)sx0 / src->w, v0 = (float)sy0 / src->h, u1 = (float)sx1 / src->w, v1 = (float)sy1 / src->h;
    struct { float x, y, z, rhw; uint32_t col; float u, v; } q[4] = {
        { dx0, dy0, 0, 1, 0xFFFFFFFFu, u0, v0 }, { dx1, dy0, 0, 1, 0xFFFFFFFFu, u1, v0 },
        { dx1, dy1, 0, 1, 0xFFFFFFFFu, u1, v1 }, { dx0, dy1, 0, 1, 0xFFFFFFFFu, u0, v1 },
    };
    g_render->draw(d, 6 /* TRIANGLEFAN */, 2, 0, 4, (const uint8_t *)q, sizeof q[0], NULL, 0, 0);
    d->st = saved;
}

static void rect_or_full(uint32_t r, DDSurf *s, uint32_t *x0, uint32_t *y0, uint32_t *x1, uint32_t *y1) {
    if (r) { *x0 = R32(r); *y0 = R32(r + 4); *x1 = R32(r + 8); *y1 = R32(r + 12); }
    else { *x0 = 0; *y0 = 0; *x1 = s->w; *y1 = s->h; }
}

static int ckey_match(DDSurf *s, uint32_t px) { return s->has_ckey && px >= s->ckey_lo && px <= s->ckey_hi; }

/* Blt(lpDestRect, lpDDSrcSurface, lpSrcRect, dwFlags, lpDDBltFx) */
W(s_Blt) {
    DDSurf *dst = me(c), *src = ARG(2) ? surf(ARG(2)) : NULL;
    uint32_t flags = ARG(4), fx = ARG(5);
    uint32_t dx0, dy0, dx1, dy1;
    rect_or_full(ARG(1), dst, &dx0, &dy0, &dx1, &dy1);

    if (flags & DDBLT_COLORFILL) {
        uint32_t color = fx ? R32(fx + 80) : 0;   /* DDBLTFX.dwFillColor */
        if (is_target(dst) && g_dd.dev_ready && g_render->clear) {
            uint32_t argb = px_to_argb(color, &dst->pf) | 0xFF000000u, r = heap_alloc(16, 0);
            W32(r, dx0); W32(r + 4, dy0); W32(r + 8, dx1); W32(r + 12, dy1);
            g_render->clear(&g_dd.dev, 1, r, 1 /* D3DCLEAR_TARGET */, argb, 1.f, 0);
            heap_free(r);
        } else if (dst->mem) {
            for (uint32_t y = dy0; y < dy1 && y < dst->h; y++)
                for (uint32_t x = dx0; x < dx1 && x < dst->w; x++) px_write(dst->mem + y * dst->pitch + x * dst->bytes, dst->bytes, color);
            touch(dst);
        }
        RET(6, DD_OK);
    }
    if (!src) RET(6, DDERR_INVALIDPARAMS);
    uint32_t sx0, sy0, sx1, sy1;
    rect_or_full(ARG(3), src, &sx0, &sy0, &sx1, &sy1);

    /* the finished scene onto the primary (windowed mode's flip) */
    if (dst->kind == SK_PRIMARY && is_target(src)) { present(); RET(6, DD_OK); }

    /* a CPU surface (movie frame, loading screen) onto the 3D target */
    if (is_target(dst) && !is_target(src)) {
        g_stats.blit++;
        blit_to_target(src, sx0, sy0, sx1, sy1, (float)dx0, (float)dy0, (float)dx1, (float)dy1);
        RET(6, DD_OK);
    }

    /* memory to memory (texture uploads from the staging surface): convert formats, stretch nearest */
    g_stats.mem_blt++;
    if (is_target(src)) target_readback(src);
    if (!dst->mem || !src->mem) RET(6, DD_OK);
    uint32_t dw = dx1 - dx0, dh = dy1 - dy0, sw = sx1 - sx0, sh = sy1 - sy0;
    int same = !memcmp(&dst->pf, &src->pf, sizeof dst->pf);
    for (uint32_t y = 0; y < dh && dy0 + y < dst->h; y++) {
        uint32_t syy = sy0 + (dh ? y * sh / dh : 0);
        for (uint32_t x = 0; x < dw && dx0 + x < dst->w; x++) {
            uint32_t sxx = sx0 + (dw ? x * sw / dw : 0);
            uint32_t px = px_read(src->mem + syy * src->pitch + sxx * src->bytes, src->bytes);
            if ((flags & DDBLT_KEYSRC) && ckey_match(src, px)) continue;
            uint32_t out = same ? px : argb_to_px(px_to_argb(px, &src->pf), &dst->pf);
            px_write(dst->mem + (dy0 + y) * dst->pitch + (dx0 + x) * dst->bytes, dst->bytes, out);
        }
    }
    touch(dst);
    if (is_target(dst)) target_upload(dst);
    RET(6, DD_OK);
}

/* BltFast(dwX, dwY, lpDDSrcSurface, lpSrcRect, dwTrans): same as an unscaled Blt */
W(s_BltFast) {
    DDSurf *dst = me(c), *src = surf(ARG(3));
    if (!src) RET(6, DDERR_INVALIDPARAMS);
    uint32_t sx0, sy0, sx1, sy1;
    rect_or_full(ARG(4), src, &sx0, &sy0, &sx1, &sy1);
    float x = (float)ARG(1), y = (float)ARG(2);
    if (is_target(dst)) { blit_to_target(src, sx0, sy0, sx1, sy1, x, y, x + (sx1 - sx0), y + (sy1 - sy0)); RET(6, DD_OK); }
    for (uint32_t yy = 0; yy < sy1 - sy0 && ARG(2) + yy < dst->h; yy++)
        for (uint32_t xx = 0; xx < sx1 - sx0 && ARG(1) + xx < dst->w; xx++) {
            uint32_t px = px_read(src->mem + (sy0 + yy) * src->pitch + (sx0 + xx) * src->bytes, src->bytes);
            if ((ARG(5) & 1 /* DDBLTFAST_SRCCOLORKEY */) && ckey_match(src, px)) continue;
            px_write(dst->mem + (ARG(2) + yy) * dst->pitch + (ARG(1) + xx) * dst->bytes, dst->bytes,
                     argb_to_px(px_to_argb(px, &src->pf), &dst->pf));
        }
    touch(dst);
    RET(6, DD_OK);
}

W(s_Flip) { present(); RET(3, DD_OK); }
W(s_GetFlipStatus) { RET(2, DD_OK); }
W(s_GetBltStatus) { RET(2, DD_OK); }
W(s_GetDC) { RET(2, DDERR_GENERIC); }   /* no GDI drawing onto surfaces */
W(s_ReleaseDC) { RET(2, DD_OK); }
W(s_PageLock) { RET(2, DD_OK); }
W(s_PageUnlock) { RET(2, DD_OK); }
W(s_SetPriority) { RET(2, DD_OK); }
W(s_SetLOD) { RET(2, DD_OK); }

static void destroy_surface(void *data) {
    DDSurf *s = data;
    if (s->tex_obj && g_render->destroy_texture) g_render->destroy_texture(&s->tex);
    if (s->mem) heap_free(s->mem);
    free(s);
}

static const ComMethod surf_methods[] = {
    { "QueryInterface", 2, com_QueryInterface }, { "AddRef", 0, com_AddRef }, { "Release", 0, com_Release },
    { "AddAttachedSurface", 1, s_AddAttachedSurface }, { "AddOverlayDirtyRect", 1, NULL }, { "Blt", 5, s_Blt },
    { "BltBatch", 3, NULL }, { "BltFast", 5, s_BltFast }, { "DeleteAttachedSurface", 2, s_DeleteAttachedSurface },
    { "EnumAttachedSurfaces", 2, NULL }, { "EnumOverlayZOrders", 3, NULL }, { "Flip", 2, s_Flip },
    { "GetAttachedSurface", 2, s_GetAttachedSurface }, { "GetBltStatus", 1, s_GetBltStatus }, { "GetCaps", 1, s_GetCaps },
    { "GetClipper", 1, NULL }, { "GetColorKey", 2, s_GetColorKey }, { "GetDC", 1, s_GetDC },
    { "GetFlipStatus", 1, s_GetFlipStatus }, { "GetOverlayPosition", 2, NULL }, { "GetPalette", 1, NULL },
    { "GetPixelFormat", 1, s_GetPixelFormat }, { "GetSurfaceDesc", 1, s_GetSurfaceDesc }, { "Initialize", 2, NULL },
    { "IsLost", 0, s_IsLost }, { "Lock", 4, s_Lock }, { "ReleaseDC", 1, s_ReleaseDC }, { "Restore", 0, s_Restore },
    { "SetClipper", 1, s_SetClipper }, { "SetColorKey", 2, s_SetColorKey }, { "SetOverlayPosition", 2, NULL },
    { "SetPalette", 1, NULL }, { "Unlock", 1, s_Unlock }, { "UpdateOverlay", 5, NULL },
    { "UpdateOverlayDisplay", 1, NULL }, { "UpdateOverlayZOrder", 2, NULL }, { "GetDDInterface", 1, NULL },
    { "PageLock", 1, s_PageLock }, { "PageUnlock", 1, s_PageUnlock }, { "SetSurfaceDesc", 2, NULL },
    { "SetPrivateData", 4, NULL }, { "GetPrivateData", 3, NULL }, { "FreePrivateData", 1, NULL },
    { "GetUniquenessValue", 1, NULL }, { "ChangeUniquenessValue", 0, NULL }, { "SetPriority", 1, s_SetPriority },
    { "GetPriority", 1, NULL }, { "SetLOD", 1, s_SetLOD }, { "GetLOD", 1, NULL },
};
static ComInterface IDirectDrawSurface7_i = { "IDirectDrawSurface7", surf_methods, 49, 0, destroy_surface };

/* ------------------------------------------------------------------ IDirect3D7 */

/* D3DDEVICEDESC7 (236 bytes) as a 2001 T&L card reports it */
static void write_device_desc(uint32_t p, int kind /* 0 RGB, 1 HAL, 2 T&L HAL */) {
    for (int i = 0; i < 236; i += 4) W32(p + i, 0);
    W32(p + 0, kind == 2 ? 0x000FFFF0u : 0x0000FFF0u);          /* dwDevCaps; 0x10000 = HWTRANSFORMANDLIGHT */
    for (int k = 0; k < 2; k++) {                                /* dpcLineCaps, dpcTriCaps: D3DPRIMCAPS */
        uint32_t q = p + 4 + 56 * k;
        W32(q + 0, 56);
        W32(q + 4, 0x7Fu);           /* MiscCaps: masks, cull */
        W32(q + 8, 0x00FFFFFFu);     /* RasterCaps: dither, z test, fog table/vertex, mip lod bias, ... */
        W32(q + 12, 0xFFu);          /* ZCmpCaps: all */
        W32(q + 16, 0x1FFFu);        /* SrcBlendCaps */
        W32(q + 20, 0x1FFFu);        /* DestBlendCaps */
        W32(q + 24, 0xFFu);          /* AlphaCmpCaps */
        W32(q + 28, 0x001FFFFFu);    /* ShadeCaps */
        W32(q + 32, 0x00001FC5u);    /* TextureCaps: perspective, alpha, transparency, border, pow2 not required */
        W32(q + 36, 0x0303FFFFu);    /* TextureFilterCaps */
        W32(q + 40, 0xFFu);          /* TextureBlendCaps */
        W32(q + 44, 0x1Fu);          /* TextureAddressCaps: wrap mirror clamp border independent */
    }
    W32(p + 116, 0x500u);            /* dwDeviceRenderBitDepth: DDBD_16 | DDBD_32 */
    W32(p + 120, 0x600u);            /* dwDeviceZBufferBitDepth: DDBD_16 | DDBD_24 */
    W32(p + 124, 1); W32(p + 128, 1);
    W32(p + 132, 2048); W32(p + 136, 2048);   /* max texture size */
    W32(p + 140, 8192); W32(p + 144, 2048);   /* max repeat, max aspect ratio */
    W32(p + 148, 2);
    uint32_t big = 0x46000000u;               /* 8192.0f guard band */
    W32(p + 152, big | 0x80000000u); W32(p + 156, big | 0x80000000u); W32(p + 160, big); W32(p + 164, big);
    W32(p + 176, 0x00080008u);                /* FVFCaps: 8 texture coordinates */
    W32(p + 180, 0x00FFFFFFu);                /* TextureOpCaps: all, including MODULATE (4) and MODULATE2X (8) */
    W16(p + 184, 8); W16(p + 186, 2);         /* wMaxTextureBlendStages, wMaxSimultaneousTextures */
    W32(p + 188, 8);                          /* lights */
    W32(p + 192, 0x5D5E0B6Bu);                /* dvMaxVertexW 1e17 */
    if (kind == 0) guid_write(p + 196, 0xa4665c60, 0x2673, 0x11cf, IID_RGB_4);
    else if (kind == 1) guid_write(p + 196, 0x84e63de0, 0x46aa, 0x11cf, IID_HAL_4);
    else guid_write(p + 196, 0xf5049e78, 0x4861, 0x11d2, IID_D3D7_4);
    W16(p + 212, 6); W16(p + 214, 4);
    W32(p + 216, kind == 2 ? 0x3Bu : 0);
}

/* EnumDevices(lpEnumDevicesCallback, lpUserArg): what DirectX 7 lists on a T&L card */
W(d3d_EnumDevices) {
    static const char *desc[] = { "Microsoft Direct3D RGB Software Emulation", "Microsoft Direct3D Hardware acceleration through Direct3D HAL",
                                  "Microsoft Direct3D Hardware Transform and Lighting acceleration capable device" };
    static const char *name[] = { "RGB Emulation", "Direct3D HAL", "Direct3D T&L HAL" };
    uint32_t cb = ARG(1), ctx = ARG(2), dd = heap_alloc(236, 1);
    for (int k = 0; k < 3; k++) {
        uint32_t s1 = guest_strdup(desc[k]), s2 = guest_strdup(name[k]);
        write_device_desc(dd, k);
        uint32_t a[4] = { s1, s2, dd, ctx };
        uint32_t r = guest_stdcall(c, cb, 4, a);
        heap_free(s1); heap_free(s2);
        if (!r) break;
    }
    heap_free(dd);
    RET(3, DD_OK);
}

/* EnumZBufferFormats(riidDevice, lpEnumCallback, lpContext) */
W(d3d_EnumZBufferFormats) {
    uint32_t cb = ARG(2), ctx = ARG(3), pf = heap_alloc(32, 1);
    static const uint32_t depths[][2] = { { 16, 0xFFFF }, { 32, 0xFFFFFF } };
    for (int k = 0; k < 2; k++) {
        W32(pf, 32); W32(pf + 4, DDPF_ZBUFFER); W32(pf + 8, 0); W32(pf + 12, depths[k][0]);
        W32(pf + 16, 0); W32(pf + 20, depths[k][1]); W32(pf + 24, 0); W32(pf + 28, 0);
        uint32_t a[2] = { pf, ctx };
        if (!guest_stdcall(c, cb, 2, a)) break;
    }
    heap_free(pf);
    RET(4, DD_OK);
}

static void device_defaults(Device *d) {
    uint32_t *rs = d->st.rs;
    memset(&d->st, 0, sizeof d->st);
    rs[4] = 1;                    /* TEXTUREPERSPECTIVE */
    rs[7] = 1;                    /* ZENABLE (a z-buffer is attached) */
    rs[8] = 3; rs[9] = 2;         /* FILLMODE solid, SHADEMODE gouraud */
    rs[14] = 1;                   /* ZWRITEENABLE */
    rs[19] = 2; rs[20] = 1;       /* SRCBLEND one, DESTBLEND zero */
    rs[22] = 3;                   /* CULLMODE ccw */
    rs[23] = 4;                   /* ZFUNC lessequal */
    rs[25] = 8;                   /* ALPHAFUNC always */
    rs[137] = 1; rs[141] = 1;     /* LIGHTING, COLORVERTEX */
    rs[145] = 1; rs[146] = 2;
    rs[168] = 0xF; rs[171] = 1;   /* COLORWRITEENABLE, BLENDOP add (Direct3D 8 states the renderer reads) */
    for (int s = 0; s < 8; s++) {
        uint32_t *t = d->st.tss[s];
        t[1] = s == 0 ? 4 : 1; t[2] = 2; t[3] = 1;
        t[4] = s == 0 ? 2 : 1; t[5] = 2; t[6] = 1;
        t[11] = (uint32_t)s;
        t[13] = t[14] = t[25] = 1;
        t[16] = t[17] = 1; t[18] = 1;
        t[26] = 1;
    }
    for (int i = 0; i < MAX_TS; i++)
        for (int k = 0; k < 4; k++) d->st.transform[i][k * 5] = 1.f;
    d->st.vs = 0x144;
}

/* CreateDevice(rclsid, lpDDS, lplpD3DDevice): the device renders into lpDDS (and its attached z-buffer) */
W(d3d_CreateDevice) {
    DDSurf *t = surf(ARG(2));
    if (!t) RET(4, DDERR_INVALIDPARAMS);
    Device *d = &g_dd.dev;
    memset(d, 0, sizeof *d);
    device_defaults(d);
    d->width = t->w; d->height = t->h;
    d->backbuffer_format = pixfmt_d3d(&t->pf);
    d->depth_format = FMT_D16;
    d->windowed = !g_dd.exclusive;
    d->backbuffer = com_new(&SurfaceView_i, RT_SURFACE, &t->rt);
    d->st.render_target = d->backbuffer;
    d->st.viewport.w = t->w; d->st.viewport.h = t->h; d->st.viewport.maxz = 1.f;
    g_dd.target = ARG(2);
    if (g_render->init) g_render->init(d);
    g_dd.dev_ready = 1;
    if (!g_dd.device_obj) g_dd.device_obj = com_new(&IDirect3DDevice7_i, RT_DEVICE, d);
    fprintf(stderr, "[d3d7] CreateDevice on %ux%u %u-bit surface\n", t->w, t->h, t->pf.bits);
    W32(ARG(3), g_dd.device_obj);
    RET(4, DD_OK);
}

static const ComMethod d3d_methods[] = {
    { "QueryInterface", 2, com_QueryInterface }, { "AddRef", 0, com_AddRef }, { "Release", 0, com_Release },
    { "EnumDevices", 2, d3d_EnumDevices }, { "CreateDevice", 3, d3d_CreateDevice }, { "CreateVertexBuffer", 3, NULL },
    { "EnumZBufferFormats", 3, d3d_EnumZBufferFormats }, { "EvictManagedTextures", 0, NULL },
};
static ComInterface IDirect3D7_i = { "IDirect3D7", d3d_methods, 8, 0 };

/* ------------------------------------------------------------------ IDirect3DDevice7 */

static Device *D(void) { return &g_dd.dev; }

W(dev_GetCaps) { write_device_desc(ARG(1), 2); RET(2, DD_OK); }

/* EnumTextureFormats(lpd3dEnumPixelProc, lpArg) */
W(dev_EnumTextureFormats) {
    static const PixFmt *list[] = { &PF_565, &PF_X555, &PF_1555, &PF_4444, &PF_X888, &PF_8888 };
    uint32_t cb = ARG(1), ctx = ARG(2), pf = heap_alloc(32, 1);
    for (unsigned k = 0; k < sizeof list / sizeof list[0]; k++) {
        write_pixfmt(pf, list[k]);
        uint32_t a[2] = { pf, ctx };
        if (!guest_stdcall(c, cb, 2, a)) break;
    }
    heap_free(pf);
    RET(3, DD_OK);
}

W(dev_BeginScene) { RET(1, DD_OK); }
W(dev_EndScene) { RET(1, DD_OK); }
W(dev_GetDirect3D) { W32(ARG(1), g_dd.d3d); RET(2, DD_OK); }

/* Clear(dwCount, lpRects, dwFlags, dwColor, dvZ, dwStencil) */
W(dev_Clear) {
    uint32_t z = ARG(5);
    float zf;
    memcpy(&zf, &z, 4);
    if (g_render->clear) g_render->clear(D(), ARG(1), ARG(2), ARG(3), ARG(4), zf, ARG(6));
    RET(7, DD_OK);
}

/* SetViewport(lpViewport): D3DVIEWPORT7 { dwX, dwY, dwWidth, dwHeight, dvMinZ, dvMaxZ } */
W(dev_SetViewport) {
    uint32_t p = ARG(1);
    Device *d = D();
    d->st.viewport.x = R32(p); d->st.viewport.y = R32(p + 4);
    d->st.viewport.w = R32(p + 8); d->st.viewport.h = R32(p + 12);
    uint32_t a = R32(p + 16), b = R32(p + 20);
    memcpy(&d->st.viewport.minz, &a, 4); memcpy(&d->st.viewport.maxz, &b, 4);
    RET(2, DD_OK);
}
W(dev_GetViewport) {
    uint32_t p = ARG(1);
    Device *d = D();
    W32(p, d->st.viewport.x); W32(p + 4, d->st.viewport.y); W32(p + 8, d->st.viewport.w); W32(p + 12, d->st.viewport.h);
    uint32_t a, b;
    memcpy(&a, &d->st.viewport.minz, 4); memcpy(&b, &d->st.viewport.maxz, 4);
    W32(p + 16, a); W32(p + 20, b);
    RET(2, DD_OK);
}

W(dev_SetRenderState) {
    uint32_t s = ARG(1);
    if (s < 256) D()->st.rs[s] = ARG(2);
    RET(3, DD_OK);
}
W(dev_GetRenderState) { W32(ARG(2), ARG(1) < 256 ? D()->st.rs[ARG(1)] : 0); RET(3, DD_OK); }

W(dev_SetTextureStageState) {
    uint32_t stage = ARG(1), type = ARG(2), v = ARG(3);
    if (stage < 8 && type < 32) {
        if (type == 12) { D()->st.tss[stage][13] = D()->st.tss[stage][14] = v; }   /* D3DTSS_ADDRESS */
        else D()->st.tss[stage][type] = v;
    }
    RET(4, DD_OK);
}
W(dev_GetTextureStageState) {
    uint32_t stage = ARG(1), type = ARG(2);
    W32(ARG(3), stage < 8 && type < 32 ? D()->st.tss[stage][type == 12 ? 13 : type] : 0);
    RET(4, DD_OK);
}

/* SetTexture(dwStage, lpTexture): a DirectDraw surface, or NULL */
W(dev_SetTexture) {
    uint32_t stage = ARG(1);
    DDSurf *s = ARG(2) ? surf(ARG(2)) : NULL;
    if (stage < 8) D()->st.texture[stage] = s ? texture_view(s) : 0;
    RET(3, DD_OK);
}
W(dev_GetTexture) { W32(ARG(2), 0); RET(3, DD_OK); }

static uint32_t fvf_stride(uint32_t fvf) {
    uint32_t n = 0;
    switch (fvf & 0xE) { case 0x2: n = 12; break; case 0x4: n = 16; break; default: n = 12 + 4 * (((fvf & 0xE) - 4) / 2); }
    if (fvf & 0x10) n += 12;   /* NORMAL */
    if (fvf & 0x40) n += 4;    /* DIFFUSE */
    if (fvf & 0x80) n += 4;    /* SPECULAR */
    n += 8 * ((fvf >> 8) & 0xF);
    return n;
}

static uint32_t prim_count(uint32_t prim, uint32_t n) {
    switch (prim) {
    case 1: return n;                 /* POINTLIST */
    case 2: return n / 2;             /* LINELIST */
    case 3: return n ? n - 1 : 0;     /* LINESTRIP */
    case 4: return n / 3;             /* TRIANGLELIST */
    case 5: case 6: return n >= 2 ? n - 2 : 0;   /* STRIP, FAN */
    }
    return 0;
}

/* DrawPrimitive(dptPrimitiveType, dwVertexTypeDesc, lpvVertices, dwVertexCount, dwFlags) */
W(dev_DrawPrimitive) {
    Device *d = D();
    uint32_t prim = ARG(1), fvf = ARG(2), n = ARG(4);
    d->st.vs = fvf;
    d->draws_this_frame++;
    if (g_render->draw && n) g_render->draw(d, prim, prim_count(prim, n), 0, n, HP(ARG(3)), fvf_stride(fvf), NULL, 0, 0);
    RET(6, DD_OK);
}

/* DrawIndexedPrimitive(type, fvf, lpvVertices, dwVertexCount, lpwIndices, dwIndexCount, dwFlags) */
W(dev_DrawIndexedPrimitive) {
    Device *d = D();
    uint32_t prim = ARG(1), fvf = ARG(2), nv = ARG(4), ni = ARG(6);
    d->st.vs = fvf;
    d->draws_this_frame++;
    if (g_render->draw && ni) g_render->draw(d, prim, prim_count(prim, ni), 0, nv, HP(ARG(3)), fvf_stride(fvf), HP(ARG(5)), 0, 0);
    RET(8, DD_OK);
}

W(dev_SetTransform) {
    uint32_t t = ARG(1);
    if (t < MAX_TS) for (int i = 0; i < 16; i++) { uint32_t v = R32(ARG(2) + 4 * i); memcpy(&D()->st.transform[t][i], &v, 4); }
    RET(3, DD_OK);
}
W(dev_ValidateDevice) { if (ARG(1)) W32(ARG(1), 1); RET(2, DD_OK); }
W(dev_PreLoad) { RET(2, DD_OK); }
W(dev_SetRenderTarget) { RET(3, DD_OK); }

static const ComMethod dev_methods[] = {
    { "QueryInterface", 2, com_QueryInterface }, { "AddRef", 0, com_AddRef }, { "Release", 0, com_Release },
    { "GetCaps", 1, dev_GetCaps }, { "EnumTextureFormats", 2, dev_EnumTextureFormats },
    { "BeginScene", 0, dev_BeginScene }, { "EndScene", 0, dev_EndScene }, { "GetDirect3D", 1, dev_GetDirect3D },
    { "SetRenderTarget", 2, dev_SetRenderTarget }, { "GetRenderTarget", 1, NULL }, { "Clear", 6, dev_Clear },
    { "SetTransform", 2, dev_SetTransform }, { "GetTransform", 2, NULL }, { "SetViewport", 1, dev_SetViewport },
    { "MultiplyTransform", 2, NULL }, { "GetViewport", 1, dev_GetViewport }, { "SetMaterial", 1, NULL },
    { "GetMaterial", 1, NULL }, { "SetLight", 2, NULL }, { "GetLight", 2, NULL },
    { "SetRenderState", 2, dev_SetRenderState }, { "GetRenderState", 2, dev_GetRenderState },
    { "BeginStateBlock", 0, NULL }, { "EndStateBlock", 1, NULL }, { "PreLoad", 1, dev_PreLoad },
    { "DrawPrimitive", 5, dev_DrawPrimitive }, { "DrawIndexedPrimitive", 7, dev_DrawIndexedPrimitive },
    { "SetClipStatus", 1, NULL }, { "GetClipStatus", 1, NULL }, { "DrawPrimitiveStrided", 5, NULL },
    { "DrawIndexedPrimitiveStrided", 7, NULL }, { "DrawPrimitiveVB", 5, NULL }, { "DrawIndexedPrimitiveVB", 7, NULL },
    { "ComputeSphereVisibility", 5, NULL }, { "GetTexture", 2, dev_GetTexture }, { "SetTexture", 2, dev_SetTexture },
    { "GetTextureStageState", 3, dev_GetTextureStageState }, { "SetTextureStageState", 3, dev_SetTextureStageState },
    { "ValidateDevice", 1, dev_ValidateDevice }, { "ApplyStateBlock", 1, NULL }, { "CaptureStateBlock", 1, NULL },
    { "DeleteStateBlock", 1, NULL }, { "CreateStateBlock", 2, NULL }, { "Load", 5, NULL }, { "LightEnable", 2, NULL },
    { "GetLightEnable", 2, NULL }, { "SetClipPlane", 2, NULL }, { "GetClipPlane", 2, NULL }, { "GetInfo", 3, NULL },
};
static ComInterface IDirect3DDevice7_i = { "IDirect3DDevice7", dev_methods, 49, 0 };

static const ImportHandler ddraw7_list[] = {
    { "DDRAW.DLL!DirectDrawCreateEx", w_DirectDrawCreateEx },
    { "DDRAW.DLL!DirectDrawEnumerateExA", w_DirectDrawEnumerateExA },
};
const ImportTable ddraw7_imports = { ddraw7_list, sizeof ddraw7_list / sizeof ddraw7_list[0] };
