/*
 * gfx.c - draws the game's display lists: the part of Fast3D (the RSP
 * microcode) and of the RDP that the game uses, turned into screen-space
 * triangles for the platform's backend (gb_* in port.h).
 *
 * Matrices are the game's own s32 16.16 ones (the port skips the conversion
 * to the RSP's split format). Addresses in the lists are port pointers
 * (KSEG0), port physical addresses (pointer + 0x80000000) or segmented
 * (segment table set by G_MOVEWORD).
 *
 * Colour combiner: any setting that is linear in TEXEL0 (all the game uses)
 * is evaluated per vertex twice, with TEXEL0 = 0 and = 1, giving
 *     out = texel * (f1 - f0) + f0
 * which the backend draws as texture * vertex colour + offset.
 */
#include "port.h"
void *memcpy(void *d, const void *s, unsigned int n);

#define GFX_MAX_VERTS 4096               /* triangle vertices per draw call batch */

typedef struct Mtx4 { f32 m[4][4]; } Mtx4;

typedef struct Tile {
    u8 fmt, siz;
    u16 line;                            /* in 64-bit words */
    u16 tmem;                            /* in 64-bit words */
    u8 palette;
    u8 cms, cmt;                         /* clamp/mirror bits */
    u8 masks, maskt, shifts, shiftt;
    u16 uls, ult, lrs, lrt;              /* 10.2 */
} Tile;

typedef struct Vert {                    /* transformed vertex */
    f32 x, y, z, w;                      /* clip space */
    f32 s, t;                            /* texture coordinates (texels, before tile offset) */
    f32 r, g, b, a;                      /* shade, 0..1 */
    f32 nx, ny, nz, q;                   /* after the perspective divide (q = 1/w) */
    u8 cr, cg, cb, ca;                   /* combined colour, valid while stamp == gColStamp */
    u32 stamp;
    s32 code;                            /* clipping planes it is outside (see out_code) */
    u32 ostamp;                          /* out: the finished vertex, valid while ostamp == gColStamp */
    GfxVert out;                         /* (a vertex is shared by several triangles) */
} Vert;
static u32 gColStamp = 1;

static void vert_project(Vert *d) {
    f32 q = (d->w != 0.0f) ? 1.0f / d->w : 0.0f;
    d->q = q;
    d->nx = d->x * q;
    d->ny = d->y * q;
    d->nz = d->z * q;
    d->stamp = 0;
    d->ostamp = 0;
    {   /* outside: near plane, then the guard band x <= 2w, -x <= 2w, y <= 2w, -y <= 2w */
        f32 gw = 2.0f * d->w;
        d->code = (d->w + d->z <= 0.0f) | ((gw - d->x <= 0.0f) << 1) | ((gw + d->x <= 0.0f) << 2) |
                  ((gw - d->y <= 0.0f) << 3) | ((gw + d->y <= 0.0f) << 4);
    }
}

static struct {
    u32 seg[16];
    Mtx4 mv[16];
    s32 mvTop;
    Mtx4 proj;
    Mtx4 mvp;
    s32 mvpDirty;
    Vert v[32];
    u32 geom;
    u32 omL, omH;
    u32 combineHi, combineLo;
    f32 prim[4], env[4], fogc[4], blendc[4];
    u32 primRaw, envRaw;
    u32 fillColor;
    f32 texS, texT;
    s32 texOn, texTile;
    u32 timgAddr, timgFmt, timgSiz, timgWidth;
    Tile tile[8];
    u8 tmem[4096];
    struct TmemLoad {                    /* loads into TMEM, replayed only when a texture must be decoded */
        const u8 *src;
        u32 srcPitch, rowBytes;
        u16 dst, line, rows;
        const u8 *rowBase;               /* loadtile: the image's row at column 0, and the first column */
        u16 x0, imgW;
        u32 end;                         /* the TMEM word after the load */
        u32 serial;                      /* when it was (last) issued: see the scenery replay */
    } loads[64];
    s32 numLoads;
    f32 vpScale[3], vpTrans[3];
    s32 scissor[4];
    u32 rdpHalf1, rdpHalf2;
    s32 numLights;
} G;

s32 gStTriNormal;
s32 gStHashB, gStHashCalls, gStDecodes, gStMatB, gStTris;
/* the timing bars' split of the frame: measured only while they're shown (the
 * clock is read a few thousand times a frame, which costs on the Dreamcast) */
s32 gGfxTiming = 0;                      /* (timing the frame's parts: off) */
#define TNOW() (gGfxTiming ? plat_time_us() : 0)
u32 gGfxProf[4];                         /* this frame, microseconds: textures, backend, vertices, triangles */
s32 gGfxFrameNo;
static GfxVert gOut[GFX_MAX_VERTS];
static s32 gOutN;
static GfxTex gCurTex;
static s32 gCurBlend, gCurZcmp, gCurZupd, gCurTextured;
static s32 gDirty = 1;
static s32 gFirstPrim = 1, gFirstEnv = 1;
/* the scenery replay (further down): recording what a display list draws */
typedef struct RSrc { f32 x, y, z, s, t, r, g, b, a; } RSrc;   /* a loaded vertex as the game gave it */
static s32 gRecOn;                       /* recording */
static u32 gRecSegMask;                  /* segments the addresses used */
static u32 gLoadSerial, gRecSerial0;
static RSrc gSlotSrc[32];
static s16 gSlotRec[32];                 /* the recorded vertex a slot is (-1: none yet) */
static u32 gSlotLoaded;                  /* slots loaded while recording */
static void rec_abort(void);
static void rec_seg(s32 textured, s32 blend, s32 zcmp, s32 zupd, const GfxTex *t);
static void rec_tri(s32 i0, s32 i1, s32 i2);
static void rec_range(const void *p, u32 n);
/* tex_key's sources, for checking a recorded texture is still the same */
static struct { const u8 *src; u32 rows, pitch, bytes, hash; const u8 *psrc; u32 plen, phash; s32 ok; } gTk;  /* the colours' converted copies not set yet */                   /* render state changed since the last triangle */

/* ------------------------------------------------------------------ */
/* Addresses                                                          */
/* ------------------------------------------------------------------ */
void *port_n64_to_ptr(u32 a);

/* Port memory (code, data, gPortRam) is all in 0x8C000000-0x8CFFFFFF (the
 * Dreamcast's RAM; the PC build is linked there too), so physical addresses
 * are 0x0C000000-0x0CFFFFFF and the game's "segment s + physical address"
 * (e.g. palette banks through segment 3) are 0x0D000000-0x0FFFFFFF. */
static void *gfx_addr(u32 a) {
    u32 s, n64;

    if (a >= 0x80000000u) {
        return (void *)a;                            /* port pointer */
    }
    if (a >= 0x0C000000u && a < 0x0D000000u) {
        return (void *)(a + 0x80000000u);            /* port physical address */
    }
    if (a >= 0x0D000000u && a < 0x10000000u) {
        s = (a >> 24) - 0x0C;
        gRecSegMask |= 1u << s;
        return (void *)(a - (s << 24) + 0x80000000u + (G.seg[s] < 0x01000000u ? G.seg[s] : 0));
    }
    /* N64 segmented or physical: static display lists in the game's data */
    s = (a >> 24) & 0xF;
    gRecSegMask |= 1u << s;
    n64 = (a & 0xFFFFFF) + (s != 0 && G.seg[s] < 0x01000000u ? G.seg[s] : 0);
    {
        void *p = port_n64_to_ptr(0x80000000u + n64);
        if (p != NULL) {
            return p;
        }
    }
    if (n64 < PORT_RAM_SIZE) {
        return gPortRam + n64;
    }
    port_log("gfx: address %08x?\n", a);
    return gPortRam;
}

/* ------------------------------------------------------------------ */
/* Matrices                                                           */
/* ------------------------------------------------------------------ */
static void mtx_load(Mtx4 *d, const s32 *s) {
    s32 i, j;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            d->m[i][j] = (f32)s[i * 4 + j] * (1.0f / 65536.0f);
        }
    }
}

static void mtx_mul(Mtx4 *d, const Mtx4 *a, const Mtx4 *b) {
    Mtx4 r;
    s32 i, j;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            r.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j] + a->m[i][3] * b->m[3][j];
        }
    }
    *d = r;
}

static void cmd_mtx(u32 w0, u32 w1) {
    u32 p = (w0 >> 16) & 0xFF;
    Mtx4 m;

    mtx_load(&m, (const s32 *)gfx_addr(w1));
    if (p & 1) {                                     /* projection */
        if (p & 2) {
            G.proj = m;
        } else {
            mtx_mul(&G.proj, &m, &G.proj);
        }
    } else {
        if ((p & 4) && G.mvTop < 15) {               /* push */
            G.mv[G.mvTop + 1] = G.mv[G.mvTop];
            G.mvTop++;
        }
        if (p & 2) {
            G.mv[G.mvTop] = m;
        } else {
            mtx_mul(&G.mv[G.mvTop], &m, &G.mv[G.mvTop]);
        }
    }
    G.mvpDirty = 1;
}

/* ------------------------------------------------------------------ */
/* Vertices                                                           */
/* ------------------------------------------------------------------ */
typedef struct N64Vtx {
    s16 x, y, z;
    u16 flag;
    s16 s, t;
    u8 c[4];
} N64Vtx;

static void cmd_vtx(u32 w0, u32 w1) {
    s32 n = ((w0 >> 20) & 0xF) + 1;
    s32 v0 = (w0 >> 16) & 0xF;
    N64Vtx *src = (N64Vtx *)gfx_addr(w1);
    s32 i;
    u32 t0 = TNOW();

    if (G.mvpDirty) {
        mtx_mul(&G.mvp, &G.mv[G.mvTop], &G.proj);
        G.mvpDirty = 0;
    }
    for (i = 0; i < n && v0 + i < 32; i++, src++) {
        Vert *d = &G.v[v0 + i];
        f32 x = src->x, y = src->y, z = src->z;
        const f32 (*m)[4] = G.mvp.m;

        d->x = x * m[0][0] + y * m[1][0] + z * m[2][0] + m[3][0];
        d->y = x * m[0][1] + y * m[1][1] + z * m[2][1] + m[3][1];
        d->z = x * m[0][2] + y * m[1][2] + z * m[2][2] + m[3][2];
        d->w = x * m[0][3] + y * m[1][3] + z * m[2][3] + m[3][3];
        d->s = src->s * G.texS * (1.0f / 32.0f);
        d->t = src->t * G.texT * (1.0f / 32.0f);
        if (G.geom & 0x20000) {                      /* G_LIGHTING: no lights set up: full brightness */
            d->r = d->g = d->b = 1.0f;
        } else {
            d->r = src->c[0] * (1.0f / 255.0f);
            d->g = src->c[1] * (1.0f / 255.0f);
            d->b = src->c[2] * (1.0f / 255.0f);
        }
        d->a = src->c[3] * (1.0f / 255.0f);
        vert_project(d);
        if (gRecOn) {
            RSrc *r = &gSlotSrc[v0 + i];
            r->x = x; r->y = y; r->z = z;
            r->s = d->s; r->t = d->t;
            r->r = d->r; r->g = d->g; r->b = d->b; r->a = d->a;
            gSlotRec[v0 + i] = -1;
            gSlotLoaded |= 1u << (v0 + i);
            rec_range(src, sizeof(N64Vtx));
        }
    }
    gGfxProf[2] += TNOW() - t0;
}

/* ------------------------------------------------------------------ */
/* Colour combiner                                                    */
/* ------------------------------------------------------------------ */
/* the combiner's inputs for one channel group: a = 0..3 rgb or alpha */
typedef struct CcIn { f32 comb[4], tex[4], shade[4]; } CcIn;

static f32 cc_rgb(s32 sel, s32 slot, const CcIn *in, s32 ch) {
    /* slot 0: a (subtract a), 1: b, 2: c (multiply), 3: d (add) */
    switch (sel) {
    case 0: return in->comb[ch];
    case 1: return in->tex[ch];
    case 2: return in->tex[ch];                      /* TEXEL1 = TEXEL0 */
    case 3: return G.prim[ch];
    case 4: return in->shade[ch];
    case 5: return G.env[ch];
    }
    if (slot == 0) {
        return sel == 6 ? 1.0f : 0.0f;               /* 1, noise -> 0 */
    }
    if (slot == 1) {
        return sel == 6 ? 0.5f : 0.0f;               /* key centre ~, k4 ~ */
    }
    if (slot == 2) {
        switch (sel) {
        case 7: return 0.0f;                         /* key scale */
        case 8: return in->comb[3];
        case 9: return in->tex[3];
        case 10: return in->tex[3];
        case 11: return G.prim[3];
        case 12: return in->shade[3];
        case 13: return G.env[3];
        case 14: return 0.0f;                        /* LOD */
        case 15: return 0.0f;                        /* prim LOD */
        }
        return 0.0f;
    }
    return sel == 6 ? 1.0f : 0.0f;
}

static f32 cc_alpha(s32 sel, s32 slot, const CcIn *in) {
    switch (sel) {
    case 0: return slot == 2 ? 0.0f : in->comb[3];   /* LOD fraction for c */
    case 1: return in->tex[3];
    case 2: return in->tex[3];
    case 3: return G.prim[3];
    case 4: return in->shade[3];
    case 5: return G.env[3];
    case 6: return slot == 2 ? 0.0f : 1.0f;          /* prim LOD for c */
    }
    return 0.0f;
}

/* one cycle of the combiner; cyc 0 or 1 */
static void cc_cycle(s32 cyc, CcIn *in, f32 out[4]) {
    u32 hi = G.combineHi, lo = G.combineLo;
    s32 a, b, c, d, ch;
    s32 aa, ab, ac, ad;

    if (cyc == 0) {
        a = (hi >> 20) & 0xF; c = (hi >> 15) & 0x1F; aa = (hi >> 12) & 7; ac = (hi >> 9) & 7;
        b = (lo >> 28) & 0xF; d = (lo >> 15) & 7; ab = (lo >> 12) & 7; ad = (lo >> 9) & 7;
    } else {
        a = (hi >> 5) & 0xF; c = hi & 0x1F; aa = (lo >> 21) & 7; ac = (lo >> 18) & 7;
        b = (lo >> 24) & 0xF; d = (lo >> 6) & 7; ab = (lo >> 3) & 7; ad = lo & 7;
    }
    for (ch = 0; ch < 3; ch++) {
        out[ch] = (cc_rgb(a, 0, in, ch) - cc_rgb(b, 1, in, ch)) * cc_rgb(c, 2, in, ch) + cc_rgb(d, 3, in, ch);
    }
    out[3] = (cc_alpha(aa, 0, in) - cc_alpha(ab, 1, in)) * cc_alpha(ac, 2, in) + cc_alpha(ad, 3, in);
}

s32 gStCcSlow, gStCcPrep;
static void cc_eval(const Vert *v, f32 tex, f32 out[4]) {
    gStCcSlow++;
    CcIn in;
    s32 i;
    s32 twoCycle = ((G.omH >> 20) & 3) == 1;

    for (i = 0; i < 4; i++) {
        in.tex[i] = tex;
        in.comb[i] = 0.0f;
    }
    in.shade[0] = v->r; in.shade[1] = v->g; in.shade[2] = v->b; in.shade[3] = v->a;
    cc_cycle(0, &in, out);
    if (twoCycle) {
        for (i = 0; i < 4; i++) {
            in.comb[i] = out[i];
        }
        cc_cycle(1, &in, out);
    }
}

/* the combiner as out = base + M * shade (for TEXEL0 = 0 and = 1), worked out
 * once per render state by evaluating it at a few shade values */
static struct { f32 base[2][4], m[2][4][4]; f32 tb[4], tm[4][4]; s32 linear; s32 texRgb, texAlpha; } gCc;

static void cc_tex_coeffs(void);
/* 1024 entries: scenery fading in sets a new primitive alpha almost every object */
static struct { u32 in[5]; s32 valid; __typeof__(gCc) cc; } gCcMemo[1024];
static u32 gCcSlot;
static void cc_prepare_(void);
static void cc_prepare(void) {
    gStCcPrep++;
    cc_prepare_();
}
static void cc_prepare_(void) {
    Vert v;
    f32 o[4], h[4];
    s32 t, j, i;
    static u32 last[5] = { 1, 1, 1, 1, 1 };
    u32 now[5];

    /* only when the inputs changed: combiner, cycle type, primitive and environment colours */
    now[0] = G.combineHi; now[1] = G.combineLo; now[2] = (G.omH >> 20) & 3;
    now[3] = G.primRaw;
    now[4] = G.envRaw;
    if (now[0] == last[0] && now[1] == last[1] && now[2] == last[2] && now[3] == last[3] && now[4] == last[4]) {
        return;
    }
    for (i = 0; i < 5; i++) {
        last[i] = now[i];
    }
    /* seen before (most frames use the same few dozen combinations)? */
    {
        u32 hsh = (now[0] * 31 + now[1]) * 31 + now[2];
        hsh = ((hsh * 31 + now[3]) * 31 + now[4]) * 2654435761u >> 22;   /* 1024 entries */
        if (gCcMemo[hsh].valid && gCcMemo[hsh].in[0] == now[0] && gCcMemo[hsh].in[1] == now[1] &&
            gCcMemo[hsh].in[2] == now[2] && gCcMemo[hsh].in[3] == now[3] && gCcMemo[hsh].in[4] == now[4]) {
            gCc = gCcMemo[hsh].cc;
            return;
        }
        gCcSlot = hsh;
    }
    gCc.linear = 1;
    for (t = 0; t < 2; t++) {
        v.r = v.g = v.b = v.a = 0.0f;
        cc_eval(&v, (f32)t, gCc.base[t]);
        for (j = 0; j < 4; j++) {
            v.r = (j == 0); v.g = (j == 1); v.b = (j == 2); v.a = (j == 3);
            cc_eval(&v, (f32)t, o);
            for (i = 0; i < 4; i++) {
                gCc.m[t][i][j] = o[i] - gCc.base[t][i];
            }
        }
        v.r = v.g = v.b = v.a = 0.5f;
        cc_eval(&v, (f32)t, h);
        for (i = 0; i < 4; i++) {
            f32 p = gCc.base[t][i] + 0.5f * (gCc.m[t][i][0] + gCc.m[t][i][1] + gCc.m[t][i][2] + gCc.m[t][i][3]);
            if (p - h[i] > 0.002f || h[i] - p > 0.002f) {
                gCc.linear = 0;
            }
        }
    }
    cc_tex_coeffs();
    for (i = 0; i < 5; i++) {
        gCcMemo[gCcSlot].in[i] = now[i];
    }
    gCcMemo[gCcSlot].cc = gCc;
    gCcMemo[gCcSlot].valid = 1;
}

/* textured: the backend multiplies by the texel, so it gets TEXEL0=1 minus
 * TEXEL0=0 for rgb (the offset is ignored) and TEXEL0=1 for alpha */
static s32 differs(f32 a, f32 b) {
    return a - b > 0.002f || b - a > 0.002f;
}
static void cc_tex_coeffs(void) {
    s32 i, j;
    /* does the result use the texel's colour, its alpha? (the backend
     * always multiplies by both, so unused ones get a white / opaque copy
     * of the texture: car windows, the RPM gauge) */
    gCc.texRgb = 0;
    gCc.texAlpha = differs(gCc.base[1][3], gCc.base[0][3]);
    for (i = 0; i < 3; i++) {
        gCc.texRgb |= differs(gCc.base[1][i], gCc.base[0][i]);
    }
    for (j = 0; j < 4; j++) {
        for (i = 0; i < 3; i++) {
            gCc.texRgb |= differs(gCc.m[1][i][j], gCc.m[0][i][j]);
        }
        gCc.texAlpha |= differs(gCc.m[1][3][j], gCc.m[0][3][j]);
    }
    if (!gCc.linear) {
        gCc.texRgb = gCc.texAlpha = 1;
    }
    for (i = 0; i < 4; i++) {
        gCc.tb[i] = (i < 3) ? gCc.base[1][i] - gCc.base[0][i] : gCc.base[1][i];
        for (j = 0; j < 4; j++) {
            gCc.tm[i][j] = (i < 3) ? gCc.m[1][i][j] - gCc.m[0][i][j] : gCc.m[1][i][j];
        }
    }
}

static void cc_fast(const Vert *v, s32 t, f32 out[4]) {
    s32 i;
    if (!gCc.linear) {
        cc_eval(v, (f32)t, out);
        return;
    }
    for (i = 0; i < 4; i++) {
        out[i] = gCc.base[t][i] + gCc.m[t][i][0] * v->r + gCc.m[t][i][1] * v->g + gCc.m[t][i][2] * v->b + gCc.m[t][i][3] * v->a;
    }
}

static u8 clamp8(f32 x) {
    if (x <= 0.0f) {
        return 0;
    }
    if (x >= 1.0f) {
        return 255;
    }
    return (u8)(x * 255.0f + 0.5f);
}

/* ------------------------------------------------------------------ */
/* Textures (a small TMEM model)                                      */
/* ------------------------------------------------------------------ */
static void cmd_settile(u32 w0, u32 w1) {
    Tile *t = &G.tile[(w1 >> 24) & 7];

    t->fmt = (w0 >> 21) & 7;
    t->siz = (w0 >> 19) & 3;
    t->line = (w0 >> 9) & 0x1FF;
    t->tmem = w0 & 0x1FF;
    t->palette = (w1 >> 20) & 0xF;
    t->cmt = (w1 >> 18) & 3;
    t->maskt = (w1 >> 14) & 0xF;
    t->shiftt = (w1 >> 10) & 0xF;
    t->cms = (w1 >> 8) & 3;
    t->masks = (w1 >> 4) & 0xF;
    t->shifts = w1 & 0xF;
}

static void cmd_settilesize(u32 w0, u32 w1) {
    Tile *t = &G.tile[(w1 >> 24) & 7];

    t->uls = (w0 >> 12) & 0xFFF;
    t->ult = w0 & 0xFFF;
    t->lrs = (w1 >> 12) & 0xFFF;
    t->lrt = w1 & 0xFFF;
}

/* a load into TMEM: only recorded (most textures are already in the backend's cache) */
static s32 gLoadChanged;                 /* the last load put something new in TMEM */
static struct TmemLoad *tmem_load(u32 dstWord, u32 lineWords, u32 rows, const u8 *src, u32 srcPitch, u32 rowBytes) {
    struct TmemLoad *l;
    u32 end = dstWord + (rows > 1 ? rows * lineWords : (rowBytes + 7) / 8);
    s32 i, k;

    /* the same load again (the game reloads a texture for each object that uses
     * it), with nothing loaded over it since: nothing changes */
    for (i = G.numLoads - 1; i >= 0; i--) {
        struct TmemLoad *o = &G.loads[i];
        u32 oend = o->end;
        if (o->dst == dstWord && o->line == lineWords && o->rows == rows && o->src == src &&
            o->srcPitch == srcPitch && o->rowBytes == rowBytes) {
            gLoadChanged = 0;
            o->serial = ++gLoadSerial;
            return o;
        }
        if (o->dst < end && oend > dstWord) {
            break;                               /* a newer load overlaps it */
        }
    }
    gLoadChanged = 1;

    /* forget earlier loads this one covers completely */
    for (i = k = 0; i < G.numLoads; i++) {
        struct TmemLoad *o = &G.loads[i];
        u32 oend = o->end;
        if (!(o->dst >= dstWord && oend <= end)) {
            if (k != i) {
                G.loads[k] = *o;
            }
            k++;
        }
    }
    G.numLoads = k;
    if (G.numLoads == 64) {
        for (i = 1; i < 64; i++) G.loads[i - 1] = G.loads[i];
        G.numLoads = 63;
    }
    l = &G.loads[G.numLoads++];
    l->src = src;
    l->srcPitch = srcPitch;
    l->rowBytes = rowBytes;
    l->dst = dstWord;
    l->line = lineWords;
    l->rows = rows;
    l->rowBase = NULL;
    l->end = end;
    l->serial = ++gLoadSerial;
    return l;
}

/* the newest load covering a TMEM word (NULL if none) */
static struct TmemLoad *tmem_find(u32 word) {
    s32 i;
    for (i = G.numLoads - 1; i >= 0; i--) {
        struct TmemLoad *l = &G.loads[i];
        u32 end = l->end;
        if (word >= l->dst && word < end) {
            return l;
        }
    }
    return NULL;
}

/* replay the loads into G.tmem (before decoding a texture) */
/* replay the loads that touch TMEM words [lo, hi) or [plo, phi) into G.tmem
 * (before decoding a texture) */
static void tmem_materialize(u32 lo, u32 hi, u32 plo, u32 phi) {
    s32 i;
    u32 r, b;
    for (i = 0; i < G.numLoads; i++) {
        struct TmemLoad *l = &G.loads[i];
        u32 end = l->end;
        if (!((l->dst < hi && end > lo) || (l->dst < phi && end > plo))) {
            continue;
        }
        for (r = 0; r < l->rows; r++) {
            u32 o = (l->dst + r * l->line) * 8;
            const u8 *s = l->src + r * l->srcPitch;
            gStMatB += l->rowBytes;
            b = l->rowBytes;
            if (o >= 4096) {
                continue;
            }
            if (o + b > 4096) {
                b = 4096 - o;
            }
            memcpy(&G.tmem[o], s, b);
        }
    }
}

static void cmd_loadblock(u32 w0, u32 w1) {
    Tile *t = &G.tile[(w1 >> 24) & 7];
    u32 uls = (w0 >> 12) & 0xFFF;
    u32 lrs = (w1 >> 12) & 0xFFF;
    u32 texels = lrs - uls + 1;
    u32 bytes = (G.timgSiz == 0) ? (texels + 1) / 2 : texels << (G.timgSiz - 1);

    tmem_load(t->tmem, 0, 1, (const u8 *)gfx_addr(G.timgAddr) + (uls << G.timgSiz >> 1), 0, bytes);
}

static void cmd_loadtile(u32 w0, u32 w1) {
    Tile *t = &G.tile[(w1 >> 24) & 7];
    u32 uls = ((w0 >> 12) & 0xFFF) >> 2, ult = (w0 & 0xFFF) >> 2;
    u32 lrs = ((w1 >> 12) & 0xFFF) >> 2, lrt = (w1 & 0xFFF) >> 2;
    u32 bpp = (G.timgSiz == 0) ? 0 : (1 << (G.timgSiz - 1));   /* bytes per texel (4-bit: 0) */
    u32 rowBytes = (lrs - uls + 1) * bpp;
    u32 srcPitch = G.timgWidth * bpp;
    const u8 *src = (const u8 *)gfx_addr(G.timgAddr);
    struct TmemLoad *l;
    u32 y;

    if (bpp == 0) {
        rowBytes = (lrs - uls + 2) / 2;
        srcPitch = G.timgWidth / 2;
    }
    y = ult;
    l = tmem_load(t->tmem, t->line, lrt - ult + 1, src + y * srcPitch + uls * bpp, srcPitch, rowBytes);
    if (bpp == 2) {
        l->rowBase = src + y * srcPitch;
        l->x0 = uls;
        l->imgW = G.timgWidth;
    }
}

static void cmd_loadtlut(u32 w0, u32 w1) {
    Tile *t = &G.tile[(w1 >> 24) & 7];
    u32 count = (((w1 >> 14) & 0x3FF) - ((w0 >> 14) & 0x3FF)) + 1;

    tmem_load(t->tmem, 0, 1, (const u8 *)gfx_addr(G.timgAddr), 0, count * 2);
}

static u16 rgba5551_to_argb1555(u16 c) {
    return (u16)(((c & 1) << 15) | (c >> 1));
}

/* decode the render tile into ARGB1555 */
static u16 gTexBuf[256 * 256];

static u16 ia_to_argb(u32 i, u32 a) {
    return (u16)(((a >= 128) << 15) | ((i >> 3) << 10) | ((i >> 3) << 5) | (i >> 3));
}

s32 gGfxLogNew;
extern s32 gGfxTrace;
s32 gGfxDebugStack = 0;
void (*gGfxTexHook)(const u16 *argb, s32 w, s32 h, const void *src, const void *pal);

/* a hash of a block of memory, remembered for the rest of the frame (the same
 * texture is usually bound many times a frame). Textures are identified by
 * their contents, not their address: the game streams the track's textures
 * through the same memory, and changes palettes and some texels as it runs. */
#define MEMO_N 1024
static struct { const u8 *p; u32 n; s32 frame; u32 h; u32 end; u32 wn; s32 born; } gMemo[MEMO_N];

/* Big blocks (1 KB and more: the textures themselves) keep their hash from
 * frame to frame: reading them again is what costs (every sampled word is a
 * cache miss on the Dreamcast). The game only puts new texels there by loading
 * files, and every load is noted here (port_mem_written), so a block is hashed
 * again when a load since then overlapped it - and, in case something wrote
 * there another way, at least every couple of seconds anyway. */
#define WRITES_N 256
static struct { u32 lo, hi; } gWrites[WRITES_N];
static u32 gWriteN;

void port_mem_written(const void *p, u32 len) {
    gWrites[gWriteN & (WRITES_N - 1)].lo = (u32)p;
    gWrites[gWriteN & (WRITES_N - 1)].hi = (u32)p + len;
    gWriteN++;
}

static s32 memo_still_good(u32 slot) {
    u32 i, lo = (u32)gMemo[slot].p, hi = gMemo[slot].end;

    if (gGfxFrameNo - gMemo[slot].born >= 48 + (s32)(slot & 31) || gWriteN - gMemo[slot].wn > WRITES_N) {
        return 0;
    }
    for (i = gMemo[slot].wn; i != gWriteN; i++) {
        if (gWrites[i & (WRITES_N - 1)].lo < hi && gWrites[i & (WRITES_N - 1)].hi > lo) {
            return 0;
        }
    }
    gMemo[slot].frame = gGfxFrameNo;
    gMemo[slot].wn = gWriteN;           /* checked up to here */
    return 1;
}

extern s32 gStHashB, gStHashCalls, gStDecodes, gStMatB;
/* rows of rowBytes bytes, pitch apart */
static u32 mem_hash(const u8 *p, u32 rows, u32 pitch, u32 rowBytes) {
    u32 n = (rows << 16) ^ rowBytes ^ (pitch << 20);
    /* two entries a set (512 sets: the same memory as before): one entry a slot let
     * blocks used every frame evict each other (~38 big texture blocks read again
     * a frame on San Francisco) */
    u32 slot = ((((u32)p >> 2) * 2654435761u + n * 40503u) >> 23) * 2;
    u32 h = (2166136261u ^ (u32)p) * 16777619u, i, r, step;   /* where it is too: textures that differ in a few texels */

    if (gMemo[slot].p != p || gMemo[slot].n != n) {
        if (gMemo[slot + 1].p == p && gMemo[slot + 1].n == n) {
            slot++;
        } else if (gMemo[slot + 1].frame < gMemo[slot].frame) {
            slot++;                              /* not there: replace the one used longer ago */
        }
    }
    if (gMemo[slot].p == p && gMemo[slot].n == n &&
        (gMemo[slot].frame == gGfxFrameNo || (rows * rowBytes >= 1024 && memo_still_good(slot)))) {
        return gMemo[slot].h;
    }
    gStHashCalls++;
    gStHashB += rows * rowBytes;
    /* big blocks: every few words is enough to tell when the game has put a
     * different texture there (palettes and small textures: every word) */
    step = (rows * rowBytes) / 1024 + 1;
    for (r = 0; r < rows; r++, p += pitch) {
        if (((u32)p & 3) == 0) {
            const u32 *w = (const u32 *)p;
            for (i = 0; i < rowBytes / 4; i += step) {
                h = (h ^ w[i]) * 16777619u;
            }
            for (i = rowBytes & ~3u; i < rowBytes; i++) {
                h = (h ^ p[i]) * 16777619u;
            }
        } else {
            for (i = 0; i < rowBytes; i += step) {
                h = (h ^ p[i]) * 16777619u;
            }
        }
    }
    p -= rows * pitch;
    gMemo[slot].p = p;
    gMemo[slot].n = n;
    gMemo[slot].frame = gGfxFrameNo;
    gMemo[slot].h = h;
    gMemo[slot].end = (u32)p + (rows - 1) * pitch + rowBytes;
    gMemo[slot].wn = gWriteN;
    gMemo[slot].born = gGfxFrameNo;
    return h;
}

static u32 load_hash(const struct TmemLoad *l) {
    return l->rows > 1 ? mem_hash(l->src, l->rows, l->srcPitch, l->rowBytes) : mem_hash(l->src, 1, 0, l->rowBytes);
}

/* the render tile's key, size and wrap modes (0: nothing usable) */
/* A strip of a wide RGBA16 image (the sky: 680 texels wide, drawn as pieces
 * whose position in the image moves as the camera turns) becomes the whole
 * width of the image, so turning does not make new textures every frame. */
typedef struct Band { const u8 *base; u32 pitch; s32 x0; } Band;
static Band gCurBand;                    /* gCurTex is a band (base != NULL) */
static f32 gUOff;                        /* added to u: where the tile starts in the band */

static s32 pow2up(s32 v) {
    s32 p = 8;
    while (p < v) p <<= 1;
    return p;
}

/* textures the demo replaces with its own art (port/repl_tex.c): found by
 * size and the full contents of the texels and palette, drawn at a higher
 * resolution (the coordinates are scaled to it) */
extern const u16 gReplCheckpoint[512 * 128];
extern const u16 gReplOptions[256 * 64];
extern const u16 gReplPeekTop[64 * 32];
extern const u16 gReplPeekBottom[64 * 32];
extern const u16 gReplDcA[128 * 128], gReplDcB[128 * 128], gReplDcX[128 * 128], gReplDcY[128 * 128];
extern const u16 gReplPresets[256 * 64];
extern const int gReplTrophyW, gReplTrophyH;
extern const unsigned char gReplTrophyIdx1[], gReplTrophyIdx2[], gReplTrophyIdx3[],
                           gReplTrophyIdx4[], gReplTrophyIdx5[], gReplTrophyIdx6[];
extern const u16 gReplTrophyPal1[256], gReplTrophyPal2[256], gReplTrophyPal3[256],
                 gReplTrophyPal4[256], gReplTrophyPal5[256], gReplTrophyPal6[256];
extern u16 gTrophyDummy[6][16 * 32];     /* src/race/hud.c: stand-ins the arcade frames replace */
extern s32 gTrophyOriginal;              /* src/race/hud.c: the TAKEMEBACK cheat (the N64's own sprites) */
static const struct Repl {
    s32 w, h, fmt;                       /* the game's texture */
    u32 hash;                            /* its texels' and palette's contents */
    const u16 *argb;
    s32 rw, rh;                          /* the replacement's size */
    const void *src;                     /* or (not NULL): found by where the texels are */
    const unsigned char *idx;            /* or (argb NULL): colour numbers, gReplTrophyW x H, */
    const u16 *pal;                      /* and their colours, at the top left of rw x rh */
} gRepl[] = {
    { 96, 21, 2, 0x9a71bed5u, gReplCheckpoint, 512, 128 },   /* the CHECKPOINT banner */
    { 96, 21, 2, 0x2c185675u, gReplCheckpoint, 512, 128 },   /* ... its other colours (it flashes) */
    { 72, 13, 2, 0x50e5f414u, gReplOptions, 256, 64 },       /* "B FOR OPTIONS" -> "X FOR OPTIONS" (file 2BC8) */
    /* the HOT TIMES secret's head, its three poses (bottom and top halves): the user's picture */
    { 64, 30, 2, 0x4c06be47u, gReplPeekBottom, 64, 32 },     /* file 2FA2 */
    { 64, 30, 2, 0xfd821f8eu, gReplPeekTop, 64, 32 },        /* file 2FA3 */
    { 64, 30, 2, 0xaeca144du, gReplPeekBottom, 64, 32 },     /* file 2FA4 */
    { 64, 30, 2, 0x4a514387u, gReplPeekTop, 64, 32 },        /* file 2FA5 */
    { 64, 30, 2, 0x3ed20873u, gReplPeekBottom, 64, 32 },     /* file 2FA6 */
    { 64, 30, 2, 0x3097ea79u, gReplPeekTop, 64, 32 },        /* file 2FA7 */
    /* the options screens' N64 buttons -> the Dreamcast's (tools/port_dcbuttons.py) */
    { 24, 20, 2, 0xf4528cd1u, gReplDcA, 128, 128 },          /* A (file 2BBC) -> A */
    { 24, 20, 2, 0x672ce024u, gReplDcX, 128, 128 },          /* B (2BBD) -> X (the same bit, 0x4000) */
    { 24, 20, 2, 0x2ef88d72u, gReplDcB, 128, 128 },          /* C-up (2BBA) -> B (bit 0x0040) */
    { 24, 20, 2, 0xb07b6527u, gReplDcY, 128, 128 },          /* C-down (2BB7) -> Y (bit 0x0080) */
    { 72, 13, 2, 0x4c0d458eu, gReplPresets, 256, 64 },       /* "B FOR PRESETS" -> "X FOR PRESETS" (2BC9) */
    /* the arcade trophy girl (tools/port_trophy.py), drawn over the port's own stand-in textures */
    { 16, 32, 0, 0, NULL, 128, 256, gTrophyDummy[0], gReplTrophyIdx1, gReplTrophyPal1 },
    { 16, 32, 0, 0, NULL, 128, 256, gTrophyDummy[1], gReplTrophyIdx2, gReplTrophyPal2 },
    { 16, 32, 0, 0, NULL, 128, 256, gTrophyDummy[2], gReplTrophyIdx3, gReplTrophyPal3 },
    { 16, 32, 0, 0, NULL, 128, 256, gTrophyDummy[3], gReplTrophyIdx4, gReplTrophyPal4 },
    { 16, 32, 0, 0, NULL, 128, 256, gTrophyDummy[4], gReplTrophyIdx5, gReplTrophyPal5 },
    { 16, 32, 0, 0, NULL, 128, 256, gTrophyDummy[5], gReplTrophyIdx6, gReplTrophyPal6 },
};

static u32 full_hash(const u8 *p, u32 rows, u32 pitch, u32 rowBytes, u32 h) {
    u32 r, i;
    for (r = 0; r < rows; r++, p += pitch) {
        for (i = 0; i < rowBytes; i++) {
            h = (h ^ p[i]) * 16777619u;
        }
    }
    return h;
}

static s32 tex_key(s32 tileIdx, GfxTex *o, Band *band) {
    Tile *t = &G.tile[tileIdx];
    s32 w = ((t->lrs - t->uls) >> 2) + 1;
    s32 h = ((t->lrt - t->ult) >> 2) + 1;
    s32 tlut = (G.omH >> 14) & 3;                    /* 2 = RGBA16 palette, 3 = IA16 */
    struct TmemLoad *l;
    u32 key;

    if (t->masks && (1 << t->masks) < w) w = 1 << t->masks;
    if (t->maskt && (1 << t->maskt) < h) h = 1 << t->maskt;
    if (w <= 0 || h <= 0 || w > 256 || h > 256) {
        return 0;
    }
    /* the texels' contents, where in the load the tile starts, the layout */
    l = tmem_find(t->tmem);
    gTk.ok = (l != NULL && l->serial > gRecSerial0);
    gTk.psrc = NULL;
    if (band != NULL) {
        band->base = NULL;
        if (l != NULL && l->rowBase != NULL && l->dst == t->tmem && l->rows >= h && t->fmt == 0 && t->siz == 2 &&
            !t->masks && !t->maskt && (t->cms & 2) && (t->cmt & 2) && l->imgW > w && l->imgW <= 1024 &&
            pow2up(l->imgW) * pow2up(h) <= 65536) {
            band->base = l->rowBase;
            band->pitch = l->srcPitch;
            band->x0 = l->x0;
            o->key = mem_hash(l->rowBase, h, l->srcPitch, l->imgW * 2) * 31 + (l->imgW << 20) + (h << 10) + 0xB;
            o->width = l->imgW;
            o->height = h;
            o->wrapS = o->wrapT = 0;
            o->argb = gTexBuf;
            o->repl = 0;
            gTk.ok = 0;
            return 1;
        }
    }
    if (l != NULL) {
        gTk.src = l->src;
        gTk.rows = l->rows > 1 ? l->rows : 1;
        gTk.pitch = l->rows > 1 ? l->srcPitch : 0;
        gTk.bytes = l->rowBytes;
        gTk.hash = load_hash(l);
    }
    key = (l ? gTk.hash + (t->tmem - l->dst) * 0x9E3779B1u : 0);
    key = key * 31 + (w << 20) + (h << 10) + (t->fmt << 6) + (t->siz << 4) + (t->line << 24);
    if (t->fmt == 2 && tlut >= 2) {                  /* CI: and the palette's contents */
        u32 pw = 256 + t->palette * 4;
        l = tmem_find(pw);
        if (l != NULL) {
            gTk.psrc = l->src + (pw - l->dst) * 8;
            gTk.plen = (t->siz == 0) ? 32 : 512;
            gTk.phash = mem_hash(gTk.psrc, 1, 0, gTk.plen);
            gTk.ok &= l->serial > gRecSerial0;
            key = (key ^ gTk.phash) * 16777619u;
        }
        key += tlut;
    }
    o->key = key;
    o->width = w;
    o->height = h;
    o->wrapS = (t->cms & 2) ? 0 : (t->cms & 1) ? 2 : 1;   /* bit 1: clamp, bit 0: mirror */
    o->wrapT = (t->cmt & 2) ? 0 : (t->cmt & 1) ? 2 : 1;
    o->argb = gTexBuf;
    o->repl = 0;
    {
        u32 k;
        for (k = 0; k < sizeof(gRepl) / sizeof(gRepl[0]); k++) {
            const struct Repl *r = &gRepl[k];
            struct TmemLoad *lt, *lp;
            u32 hh;
            if (r->w != w || r->h != h || r->fmt != t->fmt || (lt = tmem_find(t->tmem)) == NULL) continue;
            if (k < 2 && gTrophyOriginal) continue;      /* the TAKEMEBACK cheat: the N64's CHECKPOINT banner */
            if (r->src != NULL) {
                if ((const void *)lt->src != r->src) continue;
                goto found;
            }
            hh = full_hash(lt->src, lt->rows, lt->srcPitch, lt->rowBytes, 2166136261u);
            if (t->fmt == 2 && (lp = tmem_find(256 + t->palette * 4)) != NULL) {
                hh = full_hash(lp->src + (256 + t->palette * 4 - lp->dst) * 8, 1, 0, (t->siz == 0) ? 32 : 512, hh);
            }
            if (gGfxTrace) port_log("repl check %d: hash %08x\n", (int)k, hh);
            if (hh != r->hash) continue;
        found:
            o->key = 0x5EB1AC00u + k;
            o->width = r->rw;
            o->height = r->rh;
            o->repl = k + 1;
            gTk.ok = 0;
            break;
        }
    }
    return 1;
}

/* texture variants: bit 0 alpha forced opaque, bit 1 colour forced white */
static s32 gCurVariant;
static u32 tex_variant(void) {
    return (gCc.texAlpha ? 0 : 1) | (gCc.texRgb ? 0 : 2);
}
static void tex_decode_raw(s32 tileIdx, s32 w, s32 h);
static void tex_decode(s32 tileIdx, s32 w, s32 h) {
    tex_decode_raw(tileIdx, w, h);
    if (gCurVariant != 0) {
        u16 set = ((gCurVariant & 1) ? 0x8000 : 0) | ((gCurVariant & 2) ? 0x7FFF : 0);
        s32 i;
        for (i = 0; i < w * h; i++) {
            gTexBuf[i] |= set;
        }
    }
}

/* decode the render tile into gTexBuf */
static void tex_decode_raw(s32 tileIdx, s32 w, s32 h) {
    if (gCurTex.repl) {                              /* the port's own art */
        const struct Repl *r = &gRepl[gCurTex.repl - 1];
        s32 i;
        if (r->argb != NULL) {
            for (i = 0; i < w * h; i++) gTexBuf[i] = r->argb[i];
        } else {                                 /* colour numbers: padded with see-through */
            s32 x, y;
            for (y = 0; y < h; y++) {
                for (x = 0; x < w; x++) {
                    gTexBuf[y * w + x] = (x < gReplTrophyW && y < gReplTrophyH) ? r->pal[r->idx[y * gReplTrophyW + x]] : 0;
                }
            }
        }
        return;
    }
    if (gCurBand.base != NULL) {                     /* a band: straight from the image */
        s32 x, y;
        for (y = 0; y < h; y++) {
            const u16 *src = (const u16 *)(gCurBand.base + y * gCurBand.pitch);
            u16 *out = gTexBuf + y * w;
            for (x = 0; x < w; x++) out[x] = rgba5551_to_argb1555(src[x]);
        }
        return;
    }
    Tile *t = &G.tile[tileIdx];
    s32 x, y;
    u32 line = t->line * 8;
    u8 *base = G.tmem + t->tmem * 8;
    u16 *pal = (u16 *)(G.tmem + 2048 + t->palette * 32);
    s32 tlut = (G.omH >> 14) & 3;
    static u16 palConv[256];
    s32 palReady = 0;

    if (gGfxLogNew) {
        struct TmemLoad *l = tmem_find(t->tmem);
        port_log("NEWTEX src=%p w=%d h=%d fmt=%d siz=%d pal=%d tlut=%d\n", l ? (void *)l->src : NULL, (int)w, (int)h, (int)t->fmt, (int)t->siz, (int)t->palette, (int)tlut);
    }
    gStDecodes++;
    {
        u32 lo = t->tmem, hi = t->line ? t->tmem + h * t->line : 512;
        u32 plo = 0, phi = 0;
        if (t->fmt == 2) {
            plo = 256 + t->palette * 4;
            phi = plo + (t->siz == 0 ? 4 : 64);
        }
        tmem_materialize(lo, hi, plo, phi);
    }
    if (t->fmt == 2 && t->siz == 1 && tlut >= 2 && w * h > 256) {
        for (x = 0; x < 256; x++) {
            u32 v = pal[x];
            palConv[x] = (tlut == 3) ? ia_to_argb(v >> 8, v & 0xFF) : rgba5551_to_argb1555((u16)v);
        }
        palReady = 1;
    }
    for (y = 0; y < h; y++) {
        u8 *row = base + y * line;
        u16 *out = gTexBuf + y * w;
        switch ((t->fmt << 2) | t->siz) {
        case (0 << 2) | 2:                           /* RGBA16 */
            for (x = 0; x < w; x++) out[x] = rgba5551_to_argb1555(((u16 *)row)[x]);
            break;
        case (2 << 2) | 1:                           /* CI8 */
            if (tlut < 2) {                          /* no palette: the index is the intensity */
                for (x = 0; x < w; x++) out[x] = ia_to_argb(row[x], row[x]);
            } else if (palReady) {
                for (x = 0; x < w; x++) out[x] = palConv[row[x]];
            } else if (tlut == 3) {
                for (x = 0; x < w; x++) { u32 v = pal[row[x]]; out[x] = ia_to_argb(v >> 8, v & 0xFF); }
            } else {
                for (x = 0; x < w; x++) out[x] = rgba5551_to_argb1555(pal[row[x]]);
            }
            break;
        case (2 << 2) | 0:                           /* CI4 */
            for (x = 0; x < w; x++) {
                u32 i4 = (row[x >> 1] >> ((x & 1) ? 0 : 4)) & 0xF, v = pal[i4];
                out[x] = (tlut < 2) ? ia_to_argb(i4 * 17, i4 * 17) :
                         (tlut == 3) ? ia_to_argb(v >> 8, v & 0xFF) : rgba5551_to_argb1555((u16)v);
            }
            break;
        case (3 << 2) | 2:                           /* IA16 */
            for (x = 0; x < w; x++) { u32 v = ((u16 *)row)[x]; out[x] = ia_to_argb(v >> 8, v & 0xFF); }
            break;
        case (3 << 2) | 1:                           /* IA8 */
            for (x = 0; x < w; x++) { u32 v = row[x]; out[x] = ia_to_argb((v >> 4) * 17, (v & 0xF) * 17); }
            break;
        case (3 << 2) | 0:                           /* IA4 */
            for (x = 0; x < w; x++) {
                u32 v = (row[x >> 1] >> ((x & 1) ? 0 : 4)) & 0xF;
                out[x] = ia_to_argb((v >> 1) * 36, (v & 1) ? 255 : 0);
            }
            break;
        case (4 << 2) | 1:                           /* I8 */
            for (x = 0; x < w; x++) out[x] = ia_to_argb(row[x], row[x]);
            break;
        case (4 << 2) | 0:                           /* I4 */
            for (x = 0; x < w; x++) {
                u32 v = (row[x >> 1] >> ((x & 1) ? 0 : 4)) & 0xF;
                out[x] = ia_to_argb(v * 17, v * 17);
            }
            break;
        default:
            for (x = 0; x < w; x++) out[x] = 0xFFFF;
            break;
        }
    }
    if (gGfxTexHook != NULL && gGfxTrace) {
        struct TmemLoad *l = tmem_find(t->tmem), *lp = tmem_find(256 + t->palette * 4);
        gGfxTexHook(gTexBuf, w, h, l ? l->src : NULL, (t->fmt == 2 && lp) ? lp->src : NULL);
    }
}

/* the render tile as gCurTex, decoded unless the backend already has it */
static s32 tex_prepare(s32 tileIdx, s32 copy) {
    u32 t0 = TNOW();
    s32 ok;

    cc_prepare();
    ok = tex_key(tileIdx, &gCurTex, NULL);
    gCurVariant = copy ? 0 : tex_variant();      /* copy mode: no combiner */
    if (gCurVariant != 0) {
        gCurTex.key ^= gCurVariant * 0x632BE5ABu;
    }
    gCurBand.base = NULL;
    gUOff = 0.0f;

    if (ok && !gb_tex_known(gCurTex.key)) {
        tex_decode(tileIdx, gCurTex.width, gCurTex.height);
    }
    gGfxProf[0] += TNOW() - t0;
    return ok;
}

/* ------------------------------------------------------------------ */
/* Triangles                                                          */
/* ------------------------------------------------------------------ */
s32 gGfxTrace;
static s32 gNeedDecode;
static s32 gSkipBatch;                   /* texture not loaded yet (upload budget spent): drop these triangles */
static void flush(void) {
    if (gOutN > 0 && gGfxTrace) {
        s32 i;
        f32 x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (i = 0; i < gOutN; i++) {
            if (gOut[i].x < x0) x0 = gOut[i].x;
            if (gOut[i].x > x1) x1 = gOut[i].x;
            if (gOut[i].y < y0) y0 = gOut[i].y;
            if (gOut[i].y > y1) y1 = gOut[i].y;
        }
 port_log("q=%d/1e9 ", (int)(gOut[0].q * 1e9f));
        port_log("DRAW n=%d tex=%d blend=%d z=%d/%d box %d,%d-%d,%d col %d %d %d %d\n", (int)gOutN, (int)gCurTextured,
                 (int)gCurBlend, (int)gCurZcmp, (int)gCurZupd, (int)x0, (int)y0, (int)x1, (int)y1,
                 gOut[0].r, gOut[0].g, gOut[0].b, gOut[0].a);
        port_log("  cc %08x %08x omH %08x omL %08x prim %08x env %08x key %08x var %d src %p\n", G.combineHi, G.combineLo, G.omH, G.omL, G.primRaw, G.envRaw, gCurTextured ? gCurTex.key : 0, (int)gCurVariant, tmem_find(G.tile[G.texTile].tmem) ? (void *)tmem_find(G.tile[G.texTile].tmem)->src : NULL);
        if (gCurTextured && y1 <= 120 && x1 <= 140 && gOutN == 6) {
            port_log("  tex %dx%d wrap %d/%d key %08x\n", (int)gCurTex.width, (int)gCurTex.height, (int)gCurTex.wrapS, (int)gCurTex.wrapT, gCurTex.key);
            for (i = 0; i < gOutN && i < 24; i++) {
                port_log("    v%d x %d y %d q*1e4 %d u*100 %d v*100 %d\n", (int)i, (int)gOut[i].x, (int)gOut[i].y, (int)(gOut[i].q * 1e4f), (int)(gOut[i].u * 100), (int)(gOut[i].v * 100));
            }
        }
    }
    if (gOutN > 0) {
        u32 t0 = TNOW();
        gb_draw(gOut, gOutN, gCurTextured ? &gCurTex : NULL, gCurBlend, gCurZcmp, gCurZupd);
        gOutN = 0;
        gGfxProf[1] += TNOW() - t0;
    }
}

/* render state for the next triangles */
static s32 state_textured(void) {
    /* does the combiner read TEXEL0/TEXEL1 in an active slot? */
    u32 hi = G.combineHi, lo = G.combineLo;
    s32 sel[8], i;
    sel[0] = (hi >> 20) & 0xF; sel[1] = (lo >> 28) & 0xF; sel[2] = (hi >> 15) & 0x1F; sel[3] = (lo >> 15) & 7;
    sel[4] = (hi >> 5) & 0xF; sel[5] = (lo >> 24) & 0xF; sel[6] = hi & 0x1F; sel[7] = (lo >> 6) & 7;
    for (i = 0; i < 8; i++) {
        if (sel[i] == 1 || sel[i] == 2 || (i % 4 == 2 && (sel[i] == 9 || sel[i] == 10))) {
            return 1;
        }
    }
    sel[0] = (hi >> 12) & 7; sel[1] = (lo >> 12) & 7; sel[2] = (hi >> 9) & 7; sel[3] = (lo >> 9) & 7;
    sel[4] = (lo >> 21) & 7; sel[5] = (lo >> 3) & 7; sel[6] = (lo >> 18) & 7; sel[7] = lo & 7;
    for (i = 0; i < 8; i++) {
        if ((sel[i] == 1 || sel[i] == 2) && !(i % 4 == 2)) {
            return 1;
        }
    }
    return 0;
}

s32 gGfxBatches, gGfxMerged;
static void state_update(void) {
    u32 rm = G.omL, t0 = TNOW();
    s32 textured = G.texOn && state_textured();
    s32 blend, zcmp, zupd;
    GfxTex nt;
    Band nb;

    if (rm & 0x4000) {                               /* FORCE_BL */
        blend = GB_BLEND;
    } else if ((rm & 3) == 1 || (rm & 0x2000)) {     /* alpha compare / coverage x alpha */
        blend = GB_CUTOUT;
    } else {
        blend = GB_OPAQUE;
    }
    zcmp = (G.geom & 1) && (rm & 0x10);
    zupd = (G.geom & 1) && (rm & 0x20);
    nb.base = NULL;
    cc_prepare();
    if (textured && !tex_key(G.texTile, &nt, &nb)) {
        textured = 0;
    }
    if (textured && tex_variant() != 0) {
        nt.key ^= tex_variant() * 0x632BE5ABu;
    }
    gGfxProf[0] += TNOW() - t0;
    /* the triangles so far are drawn with the same state: keep adding to them */
    if (textured == gCurTextured && blend == gCurBlend && zcmp == gCurZcmp && zupd == gCurZupd &&
        (!textured || (nt.key == gCurTex.key && nt.wrapS == gCurTex.wrapS && nt.wrapT == gCurTex.wrapT))) {
        gGfxMerged++;
    } else {
        flush();
        gGfxBatches++;
        gCurTextured = textured;
        gCurBlend = blend;
        gCurZcmp = zcmp;
        gCurZupd = zupd;
        gNeedDecode = textured;                      /* when a triangle is actually drawn with it */
        gSkipBatch = 0;
        gCurBand = nb;
        if (textured) {
            gCurTex = nt;
            gCurVariant = tex_variant();
        }
    }
    gUOff = (textured && nb.base != NULL) ? (f32)nb.x0 : 0.0f;
    cc_prepare();
    gColStamp++;                                     /* vertex colours must be combined again */
    gDirty = 0;
    if (gRecOn) {
        rec_seg(textured, blend, zcmp, zupd, &nt);
    }
}

static void vert_colour(Vert *v);
/* a vertex for the backend: screen position and combined colour */
static void out_vert(GfxVert *o, const Vert *vc) {
    Vert *v = (Vert *)vc;
    Tile *t = &G.tile[G.texTile];

    if (v->ostamp == gColStamp) {                    /* done already for another triangle */
        *o = v->out;
        return;
    }

    o->x = v->nx * G.vpScale[0] + G.vpTrans[0];
    o->y = -v->ny * G.vpScale[1] + G.vpTrans[1];
    o->z = v->nz * 0.5f + 0.5f;
    o->q = v->q;
    o->u = v->s - t->uls * 0.25f + gUOff;
    o->v = v->t - t->ult * 0.25f;
    if (gCurTex.repl) {
        const struct Repl *r = &gRepl[gCurTex.repl - 1];
        o->u *= (f32)r->rw / r->w;
        o->v *= (f32)r->rh / r->h;
    }
    if (v->stamp != gColStamp) {                     /* colour: once per vertex and render state */
        vert_colour(v);
        v->stamp = gColStamp;
    }
    o->r = v->cr;
    o->g = v->cg;
    o->b = v->cb;
    o->a = v->ca;
    v->out = *o;
    v->ostamp = gColStamp;
}

/* the combined colour of a vertex for the current render state */
static void vert_colour(Vert *v) {
    {
        f32 c0[4], c1[4];
        if (gCc.linear) {
            const f32 (*m)[4] = gCurTextured ? gCc.tm : gCc.m[0];
            const f32 *b = gCurTextured ? gCc.tb : gCc.base[0];
            s32 i;
            for (i = 0; i < 4; i++) {
                c0[i] = b[i] + m[i][0] * v->r + m[i][1] * v->g + m[i][2] * v->b + m[i][3] * v->a;
            }
            if (gCurTextured && !gCc.texRgb) {       /* the texture is white: the whole colour here */
                for (i = 0; i < 3; i++) {
                    c0[i] = gCc.base[0][i] + gCc.m[0][i][0] * v->r + gCc.m[0][i][1] * v->g +
                            gCc.m[0][i][2] * v->b + gCc.m[0][i][3] * v->a;
                }
            }
        } else if (gCurTextured) {
            cc_fast(v, 0, c0);
            cc_fast(v, 1, c1);
            c0[0] = c1[0] - c0[0];
            c0[1] = c1[1] - c0[1];
            c0[2] = c1[2] - c0[2];
            c0[3] = c1[3];
        } else {
            cc_fast(v, 0, c0);
        }
        v->cr = clamp8(c0[0]);
        v->cg = clamp8(c0[1]);
        v->cb = clamp8(c0[2]);
        v->ca = clamp8(c0[3]);
    }
}

static void lerp_vert(Vert *d, const Vert *a, const Vert *b, f32 t) {
    d->x = a->x + (b->x - a->x) * t;
    d->y = a->y + (b->y - a->y) * t;
    d->z = a->z + (b->z - a->z) * t;
    d->w = a->w + (b->w - a->w) * t;
    d->s = a->s + (b->s - a->s) * t;
    d->t = a->t + (b->t - a->t) * t;
    d->r = a->r + (b->r - a->r) * t;
    d->g = a->g + (b->g - a->g) * t;
    d->b = a->b + (b->b - a->b) * t;
    d->a = a->a + (b->a - a->a) * t;
    vert_project(d);
}

static void emit_tri(const Vert *a, const Vert *b, const Vert *c) {
    if (gNeedDecode) {
        u32 t0 = TNOW();
        if (!gb_tex_known(gCurTex.key)) {
            /* only depth-tested scenery waits: the sky, road and dashboard
             * are always loaded straight away (gaps there would show) */
            if (!gCurZcmp || gb_tex_room()) {
                tex_decode(G.texTile, gCurTex.width, gCurTex.height);
            } else {
                gSkipBatch = 1;                      /* drawn from a later frame, once it is loaded */
            }
        }
        gNeedDecode = 0;
        gGfxProf[0] += TNOW() - t0;
    }
    if (gSkipBatch) {
        return;
    }
    if (gOutN + 3 > GFX_MAX_VERTS) {
        flush();
    }
    out_vert(&gOut[gOutN++], a);
    out_vert(&gOut[gOutN++], b);
    out_vert(&gOut[gOutN++], c);
}

/* clipping planes: the near plane, and a guard band twice the viewport's size
 * (the PowerVR smears textures across triangles with corners far off screen) */
#define GUARD 2.0f
static f32 plane_d(const Vert *v, s32 p) {
    switch (p) {
    case 0: return v->w + v->z;                      /* inside the near plane when > 0 */
    case 1: return GUARD * v->w - v->x;
    case 2: return GUARD * v->w + v->x;
    case 3: return GUARD * v->w - v->y;
    default: return GUARD * v->w + v->y;
    }
}

static void tri_clip(const Vert *const in[3], s32 c0, s32 c1, s32 c2);
static void draw_tri(s32 i0, s32 i1, s32 i2) {
    const Vert *in[3];
    s32 c0, c1, c2;

    gStTris++;
    gStTriNormal++;

    if (gDirty) {
        state_update();
    }
    if (gRecOn) {
        rec_tri(i0, i1, i2);
    }
    in[0] = &G.v[i0]; in[1] = &G.v[i1]; in[2] = &G.v[i2];
    c0 = in[0]->code;
    c1 = in[1]->code;
    c2 = in[2]->code;
    if (c0 & c1 & c2) {
        return;                                      /* wholly outside one plane */
    }
    /* back-face culling (screen space, before the near clip) */
    if ((G.geom & 0x3000) && !((c0 | c1 | c2) & 1)) {
        f32 x0 = in[0]->nx, y0 = in[0]->ny;
        f32 x1 = in[1]->nx, y1 = in[1]->ny;
        f32 x2 = in[2]->nx, y2 = in[2]->ny;
        f32 area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        if ((G.geom & 0x2000) && area < 0) {
            return;
        }
        if ((G.geom & 0x1000) && area > 0) {
            return;
        }
    }
    if ((c0 | c1 | c2) == 0) {
        /* wholly off one side of the screen: nothing to draw */
        if ((in[0]->nx > 1.0f && in[1]->nx > 1.0f && in[2]->nx > 1.0f) ||
            (in[0]->nx < -1.0f && in[1]->nx < -1.0f && in[2]->nx < -1.0f) ||
            (in[0]->ny > 1.0f && in[1]->ny > 1.0f && in[2]->ny > 1.0f) ||
            (in[0]->ny < -1.0f && in[1]->ny < -1.0f && in[2]->ny < -1.0f)) {
            return;
        }
        emit_tri(in[0], in[1], in[2]);
        return;
    }
    tri_clip(in, c0, c1, c2);
}

/* a triangle crossing the near plane or the guard band: clipped against each
 * plane it crosses */
static void tri_clip(const Vert *const in[3], s32 c0, s32 c1, s32 c2) {
    Vert buf[2][9];
    s32 n, k, p, cur;


    buf[0][0] = *in[0];
    buf[0][1] = *in[1];
    buf[0][2] = *in[2];
    n = 3;
    cur = 0;
    for (p = 0; p < 5; p++) {
        Vert *src = buf[cur], *dst = buf[cur ^ 1];
        s32 m = 0;
        if (!((c0 | c1 | c2) & (1 << p))) {
            continue;
        }
        for (k = 0; k < n; k++) {
            s32 j = (k + 1 == n) ? 0 : k + 1;
            f32 dk = plane_d(&src[k], p), dj = plane_d(&src[j], p);
            if (dk > 0) {
                dst[m++] = src[k];
            }
            if ((dk > 0) != (dj > 0) && m < 9) {
                lerp_vert(&dst[m++], &src[k], &src[j], dk / (dk - dj));
            }
        }
        n = m;
        cur ^= 1;
        if (n < 3) {
            return;
        }
    }
    for (k = 1; k + 1 < n; k++) {
        emit_tri(&buf[cur][0], &buf[cur][k], &buf[cur][k + 1]);
    }
}

/* ------------------------------------------------------------------ */
/* Rectangles                                                         */
/* ------------------------------------------------------------------ */
static void fill_rect(u32 w0, u32 w1) {
    s32 x1 = ((w0 >> 12) & 0xFFF) >> 2, y1 = (w0 & 0xFFF) >> 2;
    s32 x0 = ((w1 >> 12) & 0xFFF) >> 2, y0 = (w1 & 0xFFF) >> 2;
    s32 cyc = (G.omH >> 20) & 3;
    u8 r, g, b, a;

    flush();
    gDirty = 1;
    if (cyc == 3) {                                  /* fill mode: RGBA5551 fill colour, inclusive */
        u16 c = G.fillColor & 0xFFFF;
        r = ((c >> 11) & 31) * 255 / 31;
        g = ((c >> 6) & 31) * 255 / 31;
        b = ((c >> 1) & 31) * 255 / 31;
        a = 255;
        x1++;
        y1++;
    } else {
        Vert v;
        f32 c[4];
        v.r = v.g = v.b = v.a = 1.0f;
        cc_eval(&v, 1.0f, c);
        r = clamp8(c[0]); g = clamp8(c[1]); b = clamp8(c[2]); a = clamp8(c[3]);
        if (!(G.omL & 0x4000)) {
            a = 255;
        }
    }
    gb_fill(x0, y0, x1, y1, r, g, b, a);
}

static void tex_rect(u32 w0, u32 w1, u32 h1, u32 h2, s32 flip) {
    f32 x1 = ((w0 >> 12) & 0xFFF) * 0.25f, y1 = (w0 & 0xFFF) * 0.25f;
    f32 x0 = ((w1 >> 12) & 0xFFF) * 0.25f, y0 = (w1 & 0xFFF) * 0.25f;
    s32 tile = (w1 >> 24) & 7;
    f32 s = (s16)(h1 >> 16) / 32.0f, t = (s16)(h1 & 0xFFFF) / 32.0f;
    f32 dsdx = (s16)(h2 >> 16) / 1024.0f, dtdy = (s16)(h2 & 0xFFFF) / 1024.0f;
    s32 cyc = (G.omH >> 20) & 3;
    GfxVert q[6];
    s32 i;
    f32 c[4];
    Vert v;
    Tile *tl = &G.tile[tile];

    flush();
    gDirty = 1;
    if (cyc == 2) {                                  /* copy mode: 4 texels per pixel step */
        dsdx /= 4.0f;
        x1 += 1.0f;
        y1 += 1.0f;
    }
    if (!tex_prepare(tile, cyc == 2)) {
        return;
    }
    v.r = v.g = v.b = v.a = 1.0f;
    if (cyc == 2) {
        c[0] = c[1] = c[2] = c[3] = 1.0f;
    } else {
        f32 c0[4];
        cc_eval(&v, 1.0f, c);
        cc_eval(&v, 0.0f, c0);
        for (i = 0; i < 3; i++) c[i] = (gCurVariant & 2) ? c0[i] : c[i] - c0[i];
    }
    for (i = 0; i < 6; i++) {
        static const u8 cx[6] = { 0, 1, 1, 0, 1, 0 }, cy[6] = { 0, 0, 1, 0, 1, 1 };
        f32 px = cx[i] ? x1 : x0, py = cy[i] ? y1 : y0;
        f32 du = (px - x0) * dsdx, dv = (py - y0) * dtdy;
        q[i].x = px; q[i].y = py; q[i].z = 0.0f; q[i].q = 1.0f;
        if (flip) {
            q[i].u = s + (py - y0) * dsdx - tl->uls * 0.25f;
            q[i].v = t + (px - x0) * dtdy - tl->ult * 0.25f;
        } else {
            q[i].u = s + du - tl->uls * 0.25f;
            q[i].v = t + dv - tl->ult * 0.25f;
        }
        q[i].r = clamp8(c[0]); q[i].g = clamp8(c[1]); q[i].b = clamp8(c[2]); q[i].a = clamp8(c[3]);
        if (gCurTex.repl) {
            const struct Repl *r = &gRepl[gCurTex.repl - 1];
            q[i].u *= (f32)r->rw / r->w;
            q[i].v *= (f32)r->rh / r->h;
        }
    }
    gb_draw(q, 6, &gCurTex, (G.omL & 0x4000) ? GB_BLEND : GB_CUTOUT, 0, 0);
}

/* ------------------------------------------------------------------ */
/* Display list                                                       */
/* ------------------------------------------------------------------ */
static f32 col_f(u32 w, s32 sh) {
    return ((w >> sh) & 0xFF) * (1.0f / 255.0f);
}

static void set_col(f32 *c, u32 w1) {
    c[0] = col_f(w1, 24); c[1] = col_f(w1, 16); c[2] = col_f(w1, 8); c[3] = col_f(w1, 0);
}

static s32 gStats[256];

static s32 gOffscreen;

#include "gfx_replay.h"

void gfx_run(u32 *dl) {
    u32 *stack[16];
    s32 sp = 0, i;
    s32 guard = 0;

    G.mvTop = 0;
    G.mvpDirty = 1;
    gDirty = 1;
    gOutN = 0;
    gGfxFrameNo++;
    gGfxProf[0] = gGfxProf[1] = gGfxProf[2] = gGfxProf[3] = 0;
    gGfxBatches = gGfxMerged = 0;
    gCurTextured = -1;                               /* nothing drawn yet: the first state starts a batch */
    gNeedDecode = 0;
    gSkipBatch = 0;
    gOffscreen = 0;
    G.vpScale[0] = 160.0f; G.vpScale[1] = 120.0f; G.vpScale[2] = 0.5f;
    G.vpTrans[0] = 160.0f; G.vpTrans[1] = 120.0f; G.vpTrans[2] = 0.5f;
    gb_frame_begin();
    while (dl != NULL && guard++ < 1000000) {
        u32 w0 = dl[0], w1 = dl[1];
        u32 op = w0 >> 24;
        if (op == 0) {                               /* G_SPNOOP: half the commands; nothing to do */
            dl += 2;
            continue;
        }
        if (gGfxTrace) {
            port_log("%08x %08x %08x\n", (u32)(dl), w0, w1);
        }
        if (gRecOn) {
            if (!rec_op_ok(op, w0)) {
                rec_abort();
            } else {
                rec_range(dl, 8);
            }
        }
        dl += 2;
        switch (op) {
        case 0x01:
            cmd_mtx(w0, w1);
            break;
        case 0x03:                                   /* G_MOVEMEM */
            if (((w0 >> 16) & 0xFF) == 0x80) {       /* viewport */
                s16 *vp = (s16 *)gfx_addr(w1);
                flush();
                for (i = 0; i < 3; i++) {
                    G.vpScale[i] = vp[i] / 4.0f;
                    G.vpTrans[i] = vp[4 + i] / 4.0f;
                }
                gColStamp++;                         /* finished vertices are out of date */
            }
            break;
        case 0x04:
            cmd_vtx(w0, w1);
            break;
        case 0x06:                                   /* G_DL */
            if (((w0 >> 16) & 0xFF) == 0 && sp < 16) {
                stack[sp++] = dl;
            }
            dl = (u32 *)gfx_addr(w1);
            break;
        case 0x02:                                   /* the port's own: keep (w1 0) or put back (w1 1) the drawing
                                                      * state, around something the port adds (the snow) */
            {
                static struct { f32 texS, texT; s32 texTile, texOn; u32 hi, lo, omL, omH, geom; } keep;
                flush();                             /* (what is waiting is drawn as it was set up) */
                if (w1 == 0) {
                    keep.texS = G.texS; keep.texT = G.texT; keep.texTile = G.texTile; keep.texOn = G.texOn;
                    keep.hi = G.combineHi; keep.lo = G.combineLo; keep.omL = G.omL; keep.omH = G.omH;
                    keep.geom = G.geom;
                } else {
                    G.texS = keep.texS; G.texT = keep.texT; G.texTile = keep.texTile; G.texOn = keep.texOn;
                    G.combineHi = keep.hi; G.combineLo = keep.lo; G.omL = keep.omL; G.omH = keep.omH;
                    G.geom = keep.geom;
                    gDirty = 1;
                }
            }
            break;
        case 0x05:                                   /* the port's own: G_DL of a list that doesn't change (replayed) */
        case 0x07:                                   /* the port's own: G_DL of a scenery mesh (replayed) */
            if (sp < 16) {
                u32 *to = (u32 *)gfx_addr(w1);
                u32 t0 = TNOW(), inner = gGfxProf[1] + gGfxProf[2];
                s32 done = rep_draw(to, sp, op == 0x05);
                gGfxProf[3] += (TNOW() - t0) - (gGfxProf[1] + gGfxProf[2] - inner);
                if (!done) {
                    stack[sp++] = dl;
                    dl = to;
                }
            }
            break;
        case 0xBF:                                   /* G_TRI1 */
            if (gOffscreen) break;
            {   /* timed without the texture and backend work inside it (counted on their own) */
                u32 t0 = TNOW(), inner = gGfxProf[0] + gGfxProf[1];
                draw_tri(((w1 >> 16) & 0xFF) / 10, ((w1 >> 8) & 0xFF) / 10, (w1 & 0xFF) / 10);
                {
                    extern u32 gRpUs[5];
                    u32 d = (TNOW() - t0) - (gGfxProf[0] + gGfxProf[1] - inner);
                    gGfxProf[3] += d;
                    gRpUs[4] += d;
                }
            }
            break;
        case 0xBD:                                   /* G_POPMTX */
            if (G.mvTop > 0) {
                G.mvTop--;
            }
            G.mvpDirty = 1;
            break;
        case 0xBC:                                   /* G_MOVEWORD */
            if ((w0 & 0xFF) == 0x06) {
                G.seg[((w0 >> 8) & 0xFFFF) / 4 & 0xF] = w1;
            } else if ((w0 & 0xFF) == 0x0C) {        /* G_MW_POINTS: change a loaded vertex */
                u32 off = (w0 >> 8) & 0xFFFF;
                Vert *v = &G.v[(off / 40) & 31];
                v->stamp = 0;
                v->ostamp = 0;
                if (off % 40 == 0x14) {              /* texture coordinates (already scaled) */
                    v->s = (s16)(w1 >> 16) * (1.0f / 32.0f);
                    v->t = (s16)(w1 & 0xFFFF) * (1.0f / 32.0f);
                } else if (off % 40 == 0x10) {
                    v->r = (w1 >> 24) / 255.0f; v->g = ((w1 >> 16) & 0xFF) / 255.0f;
                    v->b = ((w1 >> 8) & 0xFF) / 255.0f; v->a = (w1 & 0xFF) / 255.0f;
                }
                if (gRecOn) {
                    RSrc *r = &gSlotSrc[(off / 40) & 31];
                    r->s = v->s; r->t = v->t;
                    r->r = v->r; r->g = v->g; r->b = v->b; r->a = v->a;
                    gSlotRec[(off / 40) & 31] = -1;
                }
            } else if ((w0 & 0xFF) == 0x02) {
                G.numLights = ((w1 - 0x80000000u) >> 5) - 1;
            }
            break;
        case 0xBB:                                   /* G_TEXTURE */
            G.texS = (w1 >> 16) / 65536.0f;
            G.texT = (w1 & 0xFFFF) / 65536.0f;
            G.texTile = (w0 >> 8) & 7;
            G.texOn = w0 & 0xFF;
            gDirty = 1;
            break;
        case 0xB6:                                   /* G_CLEARGEOMETRYMODE */
            G.geom &= ~w1;
            gDirty = 1;
            break;
        case 0xB7:                                   /* G_SETGEOMETRYMODE */
            G.geom |= w1;
            gDirty = 1;
            break;
        case 0xB9:                                   /* G_SETOTHERMODE_L */
        case 0xBA: {                                 /* G_SETOTHERMODE_H */
            u32 sh = (w0 >> 8) & 0xFF, len = w0 & 0xFF;
            u32 mask = ((len >= 32) ? 0xFFFFFFFFu : ((1u << len) - 1)) << sh;
            u32 *om = (op == 0xB9) ? &G.omL : &G.omH;
            u32 v = (*om & ~mask) | (w1 & mask);
            if (v != *om) {                          /* the game often sends what is already set */
                *om = v;
                gDirty = 1;
            }
            break;
        }
        case 0xB8:                                   /* G_ENDDL */
            dl = (sp > 0) ? stack[--sp] : NULL;
            if (gRecOn && sp == gRecDepth) {
                rec_end();
            }
            break;
        case 0xB4:                                   /* G_RDPHALF_1 */
            G.rdpHalf1 = w1;
            break;
        case 0xB3:                                   /* G_RDPHALF_2 */
            G.rdpHalf2 = w1;
            break;
        case 0xE4:                                   /* G_TEXRECT (+ RDPHALF_1, RDPHALF_2) */
        case 0xE5: {
            u32 h1 = dl[1], h2 = dl[3];
            dl += 4;
            if (gOffscreen) break;
            tex_rect(w0, w1, h1, h2, op == 0xE5);
            break;
        }
        case 0xE6: case 0xE7: case 0xE8: case 0xE9:  /* syncs */
            break;
        case 0xED:                                   /* G_SETSCISSOR */
            G.scissor[0] = ((w0 >> 12) & 0xFFF) >> 2;
            G.scissor[1] = (w0 & 0xFFF) >> 2;
            G.scissor[2] = ((w1 >> 12) & 0xFFF) >> 2;
            G.scissor[3] = (w1 & 0xFFF) >> 2;
            break;
        case 0xF0:
            cmd_loadtlut(w0, w1);
            gDirty |= gLoadChanged;
            break;
        case 0xF2: case 0xF5: {
            Tile *t = &G.tile[(w1 >> 24) & 7], before = *t;
            const u8 *a = (const u8 *)t, *b = (const u8 *)&before;
            u32 k;
            if (op == 0xF2) cmd_settilesize(w0, w1); else cmd_settile(w0, w1);
            for (k = 0; k < sizeof(Tile); k++) {
                if (a[k] != b[k]) {
                    gDirty = 1;
                    break;
                }
            }
            break;
        }
        case 0xF3:
            cmd_loadblock(w0, w1);
            gDirty |= gLoadChanged;
            break;
        case 0xF4:
            cmd_loadtile(w0, w1);
            gDirty |= gLoadChanged;
            break;
        case 0xF6:
            if (gOffscreen) break;
            fill_rect(w0, w1);
            break;
        case 0xF7:
            G.fillColor = w1;
            break;
        case 0xF8:
            set_col(G.fogc, w1);
            break;
        case 0xF9:
            set_col(G.blendc, w1);
            break;
        case 0xFA:
            if (w1 != G.primRaw || gFirstPrim) {
                set_col(G.prim, w1);
                G.primRaw = w1;
                gFirstPrim = 0;
                gDirty = 1;
            }
            break;
        case 0xFB:
            if (w1 != G.envRaw || gFirstEnv) {
                set_col(G.env, w1);
                G.envRaw = w1;
                gFirstEnv = 0;
                gDirty = 1;
            }
            break;
        case 0xFC:
            if ((w0 & 0xFFFFFF) != G.combineHi || w1 != G.combineLo) {
                G.combineHi = w0 & 0xFFFFFF;
                G.combineLo = w1;
                gDirty = 1;
            }
            break;
        case 0xFD:                                   /* G_SETTIMG */
            G.timgFmt = (w0 >> 21) & 7;
            G.timgSiz = (w0 >> 19) & 3;
            G.timgWidth = (w0 & 0xFFF) + 1;
            G.timgAddr = w1;
            break;
        case 0xFE:                                   /* depth image */
            break;
        case 0xFF:                                   /* colour image: the frame buffer is segment 2;
                                                      * anything else (clearing the z-buffer, copies
                                                      * into pictures) is not drawn */
            flush();
            gOffscreen = (w1 >> 24) != 0x02;
            break;
        default:
            if (++gStats[op] == 1) {
                port_log("gfx: command %02x not handled (%08x %08x)\n", op, w0, w1);
                if (gGfxDebugStack) { s32 k; for (k = 0; k < sp; k++) port_log("   called from %p (%08x %08x)\n", stack[k] - 2, stack[k][-2], stack[k][-1]); gGfxDebugStack = 0; }
            }
            break;
        }
    }
    if (gRecOn) {
        rec_abort();
    }
    flush();
    gb_frame_end();
    gGfxTrace = 0;
}
