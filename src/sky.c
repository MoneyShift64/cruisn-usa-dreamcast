/*
 * sky.c - the sky behind the track: a 680-texel-wide RGBA16 panorama strip
 * scrolled with the camera's yaw and tilted with its roll, an optional second
 * layer (a display list, e.g. clouds or far mountains) and, when the horizon
 * is tilted, a triangle in the sky's average colour filling the gap.
 * Original code: main segment, ROM 0x2820-0x3CA0 (RAM 0x80101820-0x80102CA0).
 *
 * Screen coordinates here are centred: x -165..165, y down from the top.
 */
#include "common.h"

typedef struct SkyDef {                  /* per track, at 0x80132140 */
    s32 height;                          /* horizon offset (lines) */
    s32 layerOn;                         /* draw the second layer */
} SkyDef;

typedef struct SkyView {                 /* per view (1P, 2P top, 2P bottom), at 0x801321B0 */
    s32 y;                               /* screen y of the horizon, 16.16 */
    s32 pad;
} SkyView;

typedef struct ViewerCam {               /* 0x28 bytes, 0x8014EF90 */
    s32 x, y, z;
    u8  pad[0x1C];
} ViewerCam;

typedef struct Vtx16 {                   /* RSP vertex */
    s16 x, y, z;
    u16 flag;
    s16 s, t;
    u8  rgba[4];
} Vtx16;

extern u32  gSkyTex;                     /* 0x80146150 - panorama texture file (physical address) */
extern u32  gSkyLayerDL;                 /* 0x80146154 - second layer display list file */
extern u32  gSkyR;                       /* 0x80146158 - average colour of the panorama, 0..255 */
extern u32  gSkyG;                       /* 0x8014615C */
extern u32  gSkyB;                       /* 0x80146160 */
extern s32  gSkyHeight[2];               /* 0x80146168 - per player */
extern s32  gSkyLayerOn[2];              /* 0x80146170 */
extern s32  gSkyOn[2];                   /* 0x80146178 */
extern s32  gFadeToWhite[2];             /* 0x80146140 */
extern s16  gViewTopY;                   /* 0x8014EF82 - < 0: lines above the view to fill */
extern ViewerCam gViewerCams[2];         /* 0x8014EF90 */
extern s32  gNumPlayersM1;               /* 0x8015095C */
extern SkyDef gSkyDefs[14];              /* 0x80132140 */
extern SkyView gSkyViews[3];             /* 0x801321B0 */
extern u32 *gDlCursor;                   /* 0x80147684 */
extern s32 *gMtxCursor;                  /* 0x80147688 */
extern Vtx16 *gVtxCursor;                /* 0x8014768C */

u32  assetLoad(s32 id);                  /* 0x80000480 */
u32  lowMemOverlayLoad(void);            /* 0x80102D74 */
void lowMemOverlayRelease(void);         /* 0x80102E84 */
s32  tblSin(s32 angle);                  /* 0x80113E80 */
s32  tblCos(s32 angle);                  /* 0x80113EDC */
s32  atan2s(f32 y, f32 x);               /* 0x80114100 */
void mtxTranslate(s32 *m, s32 x, s32 y, s32 z); /* 0x80114498 */
void mtxFromAngles(s32 *m, s32 ax, s32 ay, s32 az); /* 0x80114824 */
void screenClear(s32 player, s32 color); /* 0x80115730 */
void rectFill(s32 w, s32 h, s32 x, s32 y, s32 r, s32 g, s32 b, s32 a); /* 0x80115F30 */
void bgDrawSetup(s32 a, s32 b);          /* 0x80116D8C */

void skySetHeight(s32 player, s32 h);    /* 0x801019D4 */
void skyDrawView(s32 player, s32 yaw, s32 roll, s32 height); /* 0x80101B44 */
void skyHorizonFill(s32 roll, s32 x, s32 bottom, s32 y); /* 0x80101EE8 */
void skyStrip(s32 yaw, s32 roll, s32 y); /* 0x801022B0 */
s32  skyEdge(s32 roll, s32 h);           /* 0x801024B8 */
void skyStripPart(s32 x, s32 y, s32 s, s32 t, s32 w, s32 h); /* 0x80102570 */
void skyVtxRow(s32 base, s32 x, s32 xEnd, s32 y, s32 s, s32 sEnd, s32 t, s32 dx, s32 ds, s32 n); /* 0x801029B0 */
void skyVtx(s16 x, s16 y, s16 s, s16 t); /* 0x80102ADC */
void skyTexTile(s32 s0, s32 t0, s32 s1, s32 t1); /* 0x80102B20 */

#define DL_PUSH(w0, w1) { u32 *_g = gDlCursor; gDlCursor = _g + 2; _g[1] = (w1); _g[0] = (w0); }
#define KSEG0(a)        ((u32)(a) + 0x80000000)
#define TEX_PER_X       22.566667556762695   /* texture S units (10.5) per screen x */
#define TEX_PER_Y       22.441558837890625   /* ... T units per line */

/* 0x80101820 - load the sky for a track; work out its average colour */
void trackEnvInit(s32 track) {
    u16 *px;
    s32 i, p;
    SkyDef *d;

    lowMemOverlayLoad();
    gSkyTex = assetLoad(0x546);
    gSkyLayerDL = assetLoad(0x545);
    lowMemOverlayRelease();
    gSkyB = 0;
    gSkyG = 0;
    gSkyR = 0;
    px = (u16 *)KSEG0(gSkyTex);
    for (i = 0; i < 677; i++, px++) {
        gSkyR += (*px >> 11) & 0x1F;
        gSkyG += (*px >> 6) & 0x1F;
        gSkyB += (*px >> 1) & 0x1F;
    }
    gSkyR = gSkyR * 8 / 677;
    gSkyG = gSkyG * 8 / 677;
    gSkyB = gSkyB * 8 / 677;
    d = &gSkyDefs[track];
    for (p = 0; p < 2; p++) {
        gSkyOn[p] = 1;
        gSkyLayerOn[p] = d->layerOn;
        skySetHeight(p, d->height);
    }
}

/* 0x801019D4 */
void skySetHeight(s32 player, s32 h) {
    if (player == -1) {
        player = 0;
    }
    gSkyHeight[player] = h;
}

/* 0x80101A00 */
void skySetLayer(s32 player, s32 on) {
    if (player == -1) {
        player = 0;
    }
    gSkyLayerOn[player] = on;
}

/* 0x80101A2C */
void skyNop(s32 player, s32 v) {
}

/* 0x80101A3C */
void skySetEnabled(s32 player, s32 on) {
    if (player == -1) {
        player = 0;
    }
    gSkyOn[player] = on;
}

/* 0x80101A68 - draw the sky for a view (player -1: the whole screen);
 * in fog mode just clear */
void skyDraw(s32 player, s32 yaw, s32 roll) {
    s32 p = (player == -1) ? 0 : player;

    if (gFadeToWhite[p] != 0) {
        screenClear(player, 1);
    } else if (gSkyOn[p] != 0) {
        skyDrawView(player, -yaw, roll, gSkyHeight[p]);
    }
}

/* 0x80101AE8 - yaw from the player's camera towards (x, z) */
s32 viewYawTo(s32 player, s32 x, s32 z) {
    ViewerCam *c = &gViewerCams[player];
    s32 dz = z - c->z;

    return 0x4000 - atan2s((f32)(x - c->x), (f32)dz);
}

/* 0x80101B44 */
void skyDrawView(s32 player, s32 yaw, s32 roll, s32 height) {
    s32 v = (player == -1) ? 0 : gNumPlayersM1 + player;
    SkyView *sv;
    s32 *m;
    s32 p;

    bgDrawSetup(player, 1);
    if (gNumPlayersM1 == 0) {
        skyHorizonFill(roll, 0x9A, 0xF0, (gSkyViews[v].y >> 16) + height);
    }
    m = gMtxCursor;
    gMtxCursor = m + 16;
    sv = &gSkyViews[v];
    mtxFromAngles(m, 0, 0, roll);
    m[12] = 0x8B0000;
    m[13] = sv->y + (height << 16);
    m[14] = 0;
    DL_PUSH(0x01040040, (u32)(m - 16));                 /* gSPMatrix */
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xB900031D, 0x552048);                      /* render mode */
    p = (player == -1) ? 0 : player;
    if (gSkyLayerOn[p] != 0) {
        m = gMtxCursor;
        gMtxCursor = m + 16;
        mtxTranslate(m, (f32)((f64)(f32)((s32)((f64)yaw * 8.5) % 65536) / 65536.0) * -7307264.0f +
                            (f32)((yaw >= 0) * 0x6F8000) + -3178496.0f,
                     0x10000, 0);
        DL_PUSH(0x01040040, (u32)(m - 16));
        DL_PUSH(0x06000000, gSkyLayerDL);
        DL_PUSH(0xBD000000, 0);                         /* pop matrix */
        DL_PUSH(0xE7000000, 0);
    }
    DL_PUSH(0xFCFFFFFF, 0xFFFCF279);                    /* combine: texture */
    skyStrip(yaw, roll, (sv->y >> 16) + height);
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xFC119623, 0xFF2FFFFF);
    DL_PUSH(0xB900031D, 0x504240);
    DL_PUSH(0xBD000000, 0);
}

/* 0x80101EE8 - fill above the view (if it starts off screen) and, with the
 * horizon tilted, the triangle the tilted strip leaves at the top corner */
void skyHorizonFill(s32 roll, s32 x, s32 bottom, s32 y) {
    Vtx16 *v[3];
    s32 a, top, i;
    f32 sn, cs;

    if (gViewTopY < 0) {
        rectFill(0x154, -gViewTopY, 0xFFE20000, (f64)(bottom - gViewTopY - 0x14) * 65536.0, gSkyR, gSkyG, gSkyB, 0xFF);
    }
    if (roll == 0) {
        return;
    }
    a = (roll < 0) ? -roll : roll;
    sn = (f32)tblSin(a) / 16384.0f;
    cs = (f32)tblCos(a) / 16384.0f;
    if ((f64)sn == 0.0 || (f64)cs == 0.0) {
        return;
    }
    top = ((f32)y - 160.0f * (sn / cs)) + (f32)x / cs;
    if (top >= bottom) {
        return;
    }
    for (i = 0; i < 3; i++) {
        v[i] = gVtxCursor++;
        v[i]->z = 0;
    }
    v[0]->y = 0xDC;
    v[1]->y = 0xDC;
    v[2]->y = top - 0x15;
    if (roll > 0) {
        v[0]->x = (s32)((f32)(bottom - top) * (cs / sn)) - 0x13;
        v[1]->x = -0x1E;
        v[2]->x = -0x1E;
    } else {
        v[0]->x = 0x136;
        v[1]->x = 0x12B - (s32)((f32)(bottom - top) * (cs / sn));
        v[2]->x = 0x136;
    }
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xB900031D, 0x0F0A4000);
    DL_PUSH(0xFCFFFFFF, 0xFFFDF6FB);                    /* combine: primitive colour */
    DL_PUSH(0xFA000000, (gSkyR << 24) | ((gSkyG & 0xFF) << 16) | ((gSkyB & 0xFF) << 8) | 0xFF);
    DL_PUSH(0x04200030, (u32)v[0]);                     /* G_VTX 3 */
    DL_PUSH(0xBF000000, 0xA14);                         /* G_TRI1 0 1 2 */
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xB900031D, 0x504240);
    DL_PUSH(0xFC119623, 0xFF2FFFFF);
    DL_PUSH(0xFA000000, 0xFFFFFFFF);
}

/* 0x801022B0 - the panorama: S follows the yaw (wrapping at 0x54A0, the
 * texture width), drawn in one or two parts */
void skyStrip(s32 yaw, s32 roll, s32 y) {
    s32 e = skyEdge(roll, 0xFA - y);
    s32 x0, x1, n, s, over, k, m;

    if (roll < 0) {
        x0 = -0xA5 - e;
        x1 = 0xA5;
    } else {
        x0 = -0xA5;
        x1 = e + 0xA5;
    }
    n = x1 - x0;
    s = (f64)(21664.0f * (f32)((f64)(f32)(u16)yaw / 65536.0)) + (f64)x0 * TEX_PER_X;
    if (s < 0) {
        s += 0x54A0;
    }
    DL_PUSH(0xBB000001, 0xFFFFFFFF);                    /* texture on */
    over = s + (s32)((f64)n * TEX_PER_X) - 0x54A0;
    if (over > 0) {
        k = (f64)(f32)over / TEX_PER_X;
        m = n - k;
        skyStripPart(x0, 0, s, 0xD60, m + 1, 0x9A);
        skyStripPart(x0 + m, 0, (s + (s32)((f64)m * TEX_PER_X)) & 0x1F, 0xD60, k, 0x9A);
    } else {
        skyStripPart(x0, 0, s, 0xD60, n, 0x9A);
    }
    DL_PUSH(0xBB000001, 0x80008000);
}

/* 0x801024B8 - how far the tilted strip reaches past the screen side */
s32 skyEdge(s32 roll, s32 h) {
    s32 a = (roll < 0) ? -roll : roll;
    f32 sn = (f64)(f32)tblSin(a) / 16384.0;

    return ((f32)h * sn + 165.0f * (f32)((f64)(f32)tblCos(a) / 16384.0)) - 165.0f;
}

/* 0x80102570 - a w x h block of the panorama as a grid of textured quads,
 * loaded into TMEM one tile (up to 2 KB) at a time */
void skyStripPart(s32 x, s32 y, s32 s, s32 t, s32 w, s32 h) {
    s32 cols, cw, rowH, rows, v, xEnd, sEnd, sStep, tH, base, r, c, ty, tcur, sc;
    s32 i0, i1, i2, i3;

    cols = 8;
    do {
        cols--;
        cw = w / cols;
    } while (cw < 0x40 && cols != 1);
    if (cw * cols < w) {
        v = cw * cols;
        do {
            v += cols;
            cw++;
        } while (v < w);
    }
    if (cw < 0xE) {
        cw = 0xE;
    }
    rowH = 0x800 / ((cw + 7) & ~7);
    rows = h / rowH;
    if (rowH * rows < h) {
        v = rowH * rows;
        do {
            v += rowH;
            rows++;
        } while (v < h);
    }
    xEnd = x + w;
    sEnd = (f64)s + (f64)(f32)w * TEX_PER_X;
    sStep = (f64)cw * TEX_PER_X;
    skyVtxRow(0, x, xEnd, y, s, sEnd, t, cw, sStep, cols);
    tH = (f64)rowH * TEX_PER_Y;
    tcur = t - tH;
    base = 8;
    ty = y + rowH;
    for (r = 0; r < rows; r++) {
        skyVtxRow(base, x, xEnd, ty, s, sEnd, tcur, cw, sStep, cols);
        sc = s;
        i0 = base * 10;
        i1 = base * 10 + 10;
        i2 = -base * 10 + 0x50;
        i3 = -base * 10 + 0x5A;
        for (c = 0; c < cols; c++) {
            skyTexTile(sc, tcur, sc + sStep, tcur + tH);
            sc += sStep;
            DL_PUSH(0xBF000000, (i1 & 0xFF) << 16 | (i0 & 0xFF) << 8 | (i2 & 0xFF));
            DL_PUSH(0xBF000000, (i2 & 0xFF) << 16 | (i3 & 0xFF) << 8 | (i1 & 0xFF));
            i0 += 10;
            i1 += 10;
            i2 += 10;
            i3 += 10;
        }
        base = 8 - base;
        tcur -= tH;
        ty += rowH;
        if (tcur < 0) {
            ty = (f64)ty + (f64)tcur / TEX_PER_Y;
            tcur = 0;
        }
    }
}

/* 0x801029B0 - n + 1 vertices along a row, loaded at vertex 'base' */
void skyVtxRow(s32 base, s32 x, s32 xEnd, s32 y, s32 s, s32 sEnd, s32 t, s32 dx, s32 ds, s32 n) {
    Vtx16 *first = gVtxCursor;
    s32 i = n;

    if (n + 1 != 0) {
        do {
            skyVtx(x, y, s, t);
            x += dx;
            s += ds;
            if (xEnd < x) {
                x = xEnd;
            }
            if (sEnd < s) {
                s = sEnd;
            }
        } while (i-- != 0);
    }
    DL_PUSH(0x04000000 | (((n * 16) | base) & 0xFF) << 16 | ((n * 16 + 16) & 0xFFFF), (u32)first);
}

/* 0x80102ADC */
void skyVtx(s16 x, s16 y, s16 s, s16 t) {
    gVtxCursor->x = x;
    gVtxCursor->y = y;
    gVtxCursor->z = 0;
    gVtxCursor->s = s;
    gVtxCursor->t = t;
    gVtxCursor++;
}

/* 0x80102B20 - load the part of the panorama a quad uses */
void skyTexTile(s32 s0, s32 t0, s32 s1, s32 t1) {
    s32 uls = s0 >> 5;
    s32 lrs = s1 >> 5;
    u32 tile = 0xF5100000 | (((((lrs - uls) * 2 + 9) >> 3) & 0x1FF) << 9);
    u32 ul = ((uls * 4) & 0xFFF) << 12 | ((t0 >> 5) * 4 & 0xFFF);
    u32 lr = ((lrs * 4) & 0xFFF) << 12 | ((t1 >> 5) * 4 & 0xFFF);

    DL_PUSH(0xFD1002A7, gSkyTex);                       /* image: RGBA16, 680 wide */
    DL_PUSH(tile, 0x07080200);
    DL_PUSH(0xE6000000, 0);
    DL_PUSH(0xF4000000 | ul, 0x07000000 | lr);          /* load tile */
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(tile, 0x80200);
    DL_PUSH(0xF2000000 | ul, lr);                       /* tile size */
}
