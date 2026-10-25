/*
 * port.h - the layer that runs the decompiled game on a 32-bit little-endian
 * machine (the Dreamcast, or a PC test build).
 *
 * The game keeps its N64 habits:
 *   - "physical" addresses are KSEG0 pointers + 0x80000000 (32-bit wrap), so
 *     any port pointer p has the physical address p + 0x80000000;
 *   - its globals live at their N64 offsets inside one array per segment
 *     (gSeg_main, gSeg_ovl1, gSeg_ovl2; tools/gen_data.py);
 *   - it builds RSP display lists, which port/gfx.c draws (Fast3D subset);
 *   - its 4 MB of RDRAM (heaps, frame buffers, display-list buffers) is
 *     gPortRam, at the same offsets as on the N64.
 * Platform code (port/host, dc/) provides the plat_* functions below.
 */
#ifndef PORT_H
#define PORT_H

#ifndef _ULTRATYPES_H_                   /* (same types as the game's ultratypes.h) */
#define _ULTRATYPES_H_
typedef signed char s8;
typedef unsigned char u8;
typedef short s16;
typedef unsigned short u16;
typedef long s32;
typedef unsigned long u32;
typedef long long s64;
typedef unsigned long long u64;
typedef float f32;
typedef double f64;
#ifndef NULL
#define NULL 0
#endif
#endif

#define PORT_RAM_SIZE 0x400000
extern u8 gPortRam[PORT_RAM_SIZE];
/* N64 physical address (0..4 MB) -> the port's physical address */
#define PORT_PHYS(n64) ((u32)(gPortRam + (n64)) + 0x80000000u)
#define PORT_PTR(phys) ((void *)((u32)(phys) + 0x80000000u))

/* segments (port/data.S + generated relocations) */
extern u8 gSeg_main[], gSeg_ovl1[], gSeg_ovl2[];
void port_segments_init(void);
void port_overlay_reset(s32 id);         /* fresh copy of an overlay's data and bss */

/* assets (tools/port_assets.py) */
extern u8 *gPortAssets;
extern u32 gPortAssetsSize;

/* logging */
void port_log(const char *fmt, ...);
void port_fatal(const char *fmt, ...);
void port_stub(const char *name);        /* first call of an unimplemented function */

/* sound (port/audio.c): 16-bit stereo at 22050 Hz, the engine ticking as it goes */
void port_audio_mix(s16 *out, u32 frames);
/* the sound chip plays the voices (audio.c, gHwVoices): the platform's side */
extern s32 gHwVoices;
void port_audio_hw_run(u32 nowMs);      /* the engine's ticks up to the chip's clock (ms) + a little */
u32  hw_snd_alloc(u32 bytes);           /* sound memory for samples (0: none left) */
void hw_snd_free(u32 addr);
void hw_snd_write(u32 addr, const s16 *pcm, u32 bytes);
void hw_snd_start(s32 ch, u32 addr, s32 loop, u32 loopStart, u32 end, u32 freq, s32 vol, s32 pan, u32 ms);
void hw_snd_set(s32 ch, s32 what, u32 freq, s32 vol, s32 pan, u32 ms);   /* what: 1 pitch, 2 volume, 4 pan */
void hw_snd_stop(s32 ch, u32 ms);
void port_mem_written(const void *p, u32 len);   /* port/gfx.c: new contents there (a file was loaded) */

/* graphics: draw one frame's display list */
void gfx_run(u32 *dl);

/* ---- provided by the platform ---- */
void *plat_load_file(const char *name, u32 *size);
void plat_write(const char *s, u32 n);
void plat_exit(int code);
/* input: buttons in N64 layout (A 0x8000, B 0x4000, Z 0x2000, START 0x1000,
 * D-pad 0x0800 up..0x0100 right, L 0x20, R 0x10, C 0x08 up..0x01 right),
 * stick -80..80 */
int  plat_pad(int i, u16 *buttons, s8 *x, s8 *y);   /* 0 = not connected */
int  plat_date(int *year, int *month, int *day);   /* the console's clock: 1, or 0 if not known */
void plat_vmu_lcd(const u8 *bits);     /* player 1's VMU screen: 48x32, rows top down, 6 bytes a row, bit 7 = left */
int  plat_key(void);                    /* a key typed on a keyboard (ASCII, Enter '\n'), 0 = none */
void plat_frame_end(void);               /* after a frame is drawn (present, wait) */
u32  plat_time_us(void);                 /* a microsecond clock, for timing (may be 0) */

/* graphics backend: screen-space triangles (x, y in 320x240 N64 pixels,
 * z 0 near .. 1 far, w = 1/clip w for perspective, u, v in texels) */
typedef struct GfxVert {
    f32 x, y, z, q;                      /* q = 1/w */
    f32 u, v;
    u8 r, g, b, a;
} GfxVert;
typedef struct GfxTex {
    u32 key;                             /* identifies the texture (address, size, format) */
    u16 *argb;                           /* ARGB1555 texels, width x height (filled once per key) */
    s32 width, height;                   /* size of the loaded texels */
    s32 wrapS, wrapT;                    /* 0 clamp, 1 wrap, 2 mirror */
    s32 repl;                            /* port/gfx.c: replaced by the port's own art (index + 1) */
} GfxTex;
enum { GB_OPAQUE = 0, GB_CUTOUT = 1, GB_BLEND = 2 };
/* the scenery replay's direct path: triangles straight into the backend's
 * format. gb_prim_begin sets the render state (0: draw nothing) and says how
 * to make a GbPVert from a GfxVert: x, y times xs, ys; u, v times us, vs;
 * z, q as they are; argb packed */
typedef struct GbPVert { f32 x, y, z, q, u, v; u32 argb; } GbPVert;
typedef struct GbPrim { f32 xs, ys, us, vs; } GbPrim;
s32  gb_prim_begin(const GfxTex *tex, s32 blend, s32 zcmp, s32 zwrite, GbPrim *p);
void gb_prim_tri(const GbPVert *a, const GbPVert *b, const GbPVert *c);
f32  gb_prim_xyscale(void);              /* gb_prim_begin's xs (= ys): the same for every run */
void *gb_alloc(u32 n);                  /* memory for the port's own use (NULL: none) */
void gb_free(void *p);
void port_dl_compiled(const void *dl);  /* gfx.c: a scenery mesh was compiled at dl */
void port_dl_flush(void);               /* gfx.c: the mesh cache was emptied */
s32  gb_tex_known(u32 key);               /* 1: the backend has this texture (argb not filled) */
s32  gb_tex_room(void);                  /* 1: another new texture may be loaded this frame */
void gb_frame_begin(void);
void gb_draw(GfxVert *v, s32 n, GfxTex *tex, s32 blend, s32 zcmp, s32 zwrite); /* n/3 triangles */
void gb_fill(s32 x0, s32 y0, s32 x1, s32 y1, u8 r, u8 g, u8 b, u8 a);
void gb_frame_end(void);

#endif
