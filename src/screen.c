/*
 * screen.c - screen set-up for drawing: viewports, scissor boxes per player
 * window, clearing, and flat rectangles.
 * Original code: main segment, part of ROM 0x166E0-0x17C10
 * (RAM 0x80115730-0x80116C10).
 */
#include "common.h"

typedef struct Vp16 { s16 scale[4], trans[4]; } Vp16;   /* RSP viewport, units of 1/4 pixel */
typedef struct ScissorBox { u32 ulx, uly, lrx, lry; } ScissorBox;

extern u32 *gDlCursor;                   /* 0x80147684 */
extern s32 *gMtxCursor;                  /* 0x80147688 */
extern u8  *gVpCursor;                   /* 0x80147690 */
extern GfxSlot gGfxSlots[3];             /* 0x801476A0 */
extern s32 gGfxBuildingSlot;             /* 0x80147664 */
extern s16 gViewLeftX;                   /* 0x8014EF80 - screen shift (pixels) */
extern s16 gViewTopY;                    /* 0x8014EF82 */
extern s32 gNumPlayersM1;                /* 0x8015095C */
extern ScissorBox gScissorBoxes[3];      /* 0x80133A78 - full screen, top, bottom */
extern Vtx gUnitSquareVtx[4];            /* 0x801339F8 - (0,0)-(1,1) */

extern s32 gLowMemOverlayState;          /* 0x801466CC: 0 = none, 1 = waiting, 2 = loaded */
extern u32 gLowMemStart;                 /* 0x80146598 - also the z-buffer */
extern Vtx gShadowSquareVtx[4];          /* 0x80133A38 - also the fade's two squares */

void mtxScale(s32 *m, s32 sx, s32 sy, s32 sz); /* 0x801144DC */
void mtxTranslate(s32 *m, s32 x, s32 y, s32 z);        /* 0x80114498 */
void mtxFromAngles(s32 *m, s32 ax, s32 ay, s32 az);    /* 0x80114824 */
void mtxRotYTranslate(s32 *m, s32 angle, s32 x, s32 y, s32 z); /* 0x80114DDC */
void mtxScaleBy(s32 *m, s32 sx, s32 sy, s32 sz);       /* 0x8011545C */
s32  tblCos(s32 angle);                  /* 0x80113EDC */
s32  disableInterrupts(void);            /* 0x801156E0 */
void restoreInterrupts(s32 mask);        /* 0x80115700 */
void scissorSet(s32 viewer);             /* 0x80115DD4 */
void bgDrawSetup(s32 viewer, s32 ortho); /* 0x80116D8C */

#define DL_PUSH(w0, w1) { u32 *_g = gDlCursor; gDlCursor = _g + 2; _g[0] = (w0); _g[1] = (w1); }

/* 0x80115C10 - set the viewport (shifted by dx, dy quarter pixels); the
 * same viewport is shared within a frame */
void viewportSet(Vp16 *vp, s16 dx, s16 dy) {
    Vp16 v;
    u32 *p, *end;
    s32 i;

    v = *vp;
    v.trans[0] = v.trans[0] + gViewLeftX * 4 + dx;
    v.trans[1] = (v.trans[1] - gViewTopY * 4) - dy;
    p = (u32 *)gGfxSlots[gGfxBuildingSlot].vpStart;
    end = (u32 *)gVpCursor;
    for (; p < end; p += 4) {
        for (i = 0; i < 4; i++) {
            if (((u32 *)&v)[i] != p[i]) {
                break;
            }
        }
        if (i == 4) {
            break;
        }
    }
    if (p >= (u32 *)gVpCursor) {
        p = (u32 *)gVpCursor;
        gVpCursor += 16;
        for (i = 0; i < 4; i++) {
            p[i] = ((u32 *)&v)[i];
        }
    }
    DL_PUSH(0x03800010, (u32)p);         /* gSPViewport */
}

/* 0x80115DD4 - scissor to a player's window (-1: the whole screen) */
void scissorSet(s32 viewer) {
    ScissorBox *b;
    s32 i;

    gScissorBoxes[1].lry = 0x77 - gViewTopY;
    gScissorBoxes[2].uly = 0x79 - gViewTopY;
    i = (viewer == -1) ? 0 : gNumPlayersM1 + viewer;
    b = &gScissorBoxes[i];
    DL_PUSH(0xED000000 | (((s32)((f32)b->ulx * 4.0f) & 0xFFF) << 12) | ((s32)((f32)b->uly * 4.0f) & 0xFFF),
            (((s32)((f32)b->lrx * 4.0f) & 0xFFF) << 12) | ((s32)((f32)b->lry * 4.0f) & 0xFFF));
}

/* 0x80115730 - clear a player's window (-1 or one player: the whole screen)
 * to a 16-bit colour */
void screenClear(s32 viewer, s32 color) {
    color &= 0xFFFF;
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xBA001402, 0x300000);       /* fill mode */
    DL_PUSH(0xB900031D, 0);
    DL_PUSH(0xF7000000, (color << 16) | color);
    scissorSet(viewer);
    if (gNumPlayersM1 != 0 && viewer >= 0) {
        if (viewer != 0) {
            DL_PUSH(0xF64FC3BC, ((0x79 - gViewTopY) & 0x3FF) * 4);
        } else {
            DL_PUSH(0xF64FC000 | (((0x76 - gViewTopY) & 0x3FF) * 4), 0);
        }
    } else {
        DL_PUSH(0xF64FC3BC, 0);          /* (0,0)-(319,239) */
    }
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xB900031D, 0x504240);
    DL_PUSH(0xBA001402, 0);              /* one cycle */
}

/* 0x80115F30 - a flat rectangle (in the 2D set-up of bgDrawSetup) */
void rectFill(s32 w, s32 h, s32 x, s32 y, s32 r, s32 g, s32 b, s32 a) {
    s32 *m = gMtxCursor;

    gMtxCursor = m + 16;
    mtxScale(m, w << 16, h << 16, 0x10000);
    m[12] = x;
    m[14] = 0;
    m[13] = y;
    DL_PUSH(0x01040040, (u32)(m - 16));  /* push, multiply */
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xB900031D, 0x504240);
    DL_PUSH(0xFCFFFFFF, 0xFFFDF6FB);     /* combine: primitive colour */
    DL_PUSH(0xFA000000, (r << 24) | ((g & 0xFF) << 16) | ((b & 0xFF) << 8) | (a & 0xFF));
    DL_PUSH(0x04300040, (u32)gUnitSquareVtx);
    DL_PUSH(0xBF000000, 0x140A00);
    DL_PUSH(0xBF000000, 0x1E1400);
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xBD000000, 0);              /* pop */
    DL_PUSH(0xB900031D, 0x504240);
    DL_PUSH(0xFC119623, 0xFF2FFFFF);     /* combine: texture x shade */
    DL_PUSH(0xFA000000, 0xFFFFFFFF);
}

/* 0x80116164 - the grey box behind the pause menu */
void drawBox(s32 w, s32 h, s32 x, s32 y) {
    rectFill(w, h, x, y, 0x14, 0x14, 0x14, 0x96);
}

/* 0x801158EC - start of a frame: clear the z-buffer (unless the low-memory
 * overlay is using it) and draw into the frame buffer */
void drawLowMemLayer(void) {
    s32 mask = disableInterrupts();

    if (gLowMemOverlayState == 0) {
        gGfxSlots[gGfxBuildingSlot].unk24 = 1;
        restoreInterrupts(mask);
        DL_PUSH(0xE7000000, 0);
        DL_PUSH(0xFE000000, gLowMemStart);   /* z image */
        DL_PUSH(0xBA001402, 0x300000);       /* fill mode */
        DL_PUSH(0xB900031D, 0);
        DL_PUSH(0xFF10013F, gLowMemStart);   /* draw into the z-buffer */
        DL_PUSH(0xF7000000, 0xFFFCFFFC);     /* furthest depth */
        scissorSet(-1);
        DL_PUSH(0xF64FC3BC, 0);
        DL_PUSH(0xE7000000, 0);
        DL_PUSH(0xB900031D, 0x504240);
        DL_PUSH(0xBA001402, 0);
    } else {
        restoreInterrupts(mask);
    }
    DL_PUSH(0xFF10013F, 0x02000000);         /* colour image: the frame buffer (segment 2) */
}

/* 0x80115AC4 - the black line between the two players' halves */
void raceHudSplitScreen(void) {
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xBA001402, 0x300000);
    DL_PUSH(0xB900031D, 0);
    DL_PUSH(0xF7000000, 0x00010001);
    scissorSet(-1);
    DL_PUSH(0xF64FC000 | (((0x78 - gViewTopY) & 0x3FF) << 2), ((0x77 - gViewTopY) & 0x3FF) << 2);
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xB900031D, 0x504240);
    DL_PUSH(0xBA001402, 0);
}

/* 0x801161A0 - set the colour of n vertices */
void vtxSetColor(Vtx *v, s32 n, u8 r, u8 g, u8 b) {
    s32 i;

    for (i = 0; i < n; i++) {
        v[i].v.cn[0] = r;
        v[i].v.cn[1] = g;
        v[i].v.cn[2] = b;
    }
}

/* 0x80116230 - ... to a grey */
void vtxSetGrey(Vtx *v, s32 n, u8 c) {
    vtxSetColor(v, n, c, c, c);
}

/* 0x80116254 - draw a display list at a 16.16 position */
void drawDL(void *dl, s32 x, s32 y, s32 z, s32 scale) {
    s32 *m = gMtxCursor;

    gMtxCursor = m + 16;
    mtxTranslate(m, x, y, z);
    if (scale != 0x10000) {
        mtxScaleBy(m, scale, scale, scale);
    }
    DL_PUSH(0x01040040, (u32)(m - 16));
    DL_PUSH(0x06000000, (u32)dl);
    DL_PUSH(0xBD000000, 0);
}

/* 0x80116304 - ... turned about y */
void drawDLRotY(void *dl, s32 angle, s32 x, s32 y, s32 z) {
    s32 *m = gMtxCursor;

    gMtxCursor = m + 16;
    mtxRotYTranslate(m, angle, x, y, z);
    DL_PUSH(0x01040040, (u32)(m - 16));
    DL_PUSH(0x06000000, (u32)dl);
    DL_PUSH(0xBD000000, 0);
}

/* 0x801163A0 - copy a 320x240 16-bit picture to another (copy mode, six
 * lines at a time): the paused game's frozen frame */
void bgLoadImage(u32 dst, u32 src) {
    s32 y;

    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xBA001402, 0x200000);           /* copy mode */
    DL_PUSH(0xB900031D, 0);
    DL_PUSH(0xFF10013F, dst);
    DL_PUSH(0xBA001301, 0);
    DL_PUSH(0xB9000002, 0);
    scissorSet(-1);
    for (y = 0; y != 240; y += 6) {
        DL_PUSH(0xFD100000, src);
        DL_PUSH(0xF5100000, 0x07000000);
        DL_PUSH(0xE6000000, 0);
        DL_PUSH(0xF3000000, 0x0777F01A);
        DL_PUSH(0xE7000000, 0);
        DL_PUSH(0xF510A000, 0);
        DL_PUSH(0xF2000000, 0x004FC014);
        DL_PUSH(0xE44FC000 | (((y + 5) << 2) & 0xFFF), (y << 2) & 0xFFF);
        DL_PUSH(0xB3000000, 0);
        DL_PUSH(0xB2000000, 0x10000400);
        src += 0xF00;
    }
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xB900031D, 0x504240);
    DL_PUSH(0xBA001402, 0);
    DL_PUSH(0xFF10013F, 0x02000000);
    DL_PUSH(0xBA001301, 0x80000);
    DL_PUSH(0xB9000002, 1);
}

/* 0x801166BC - the iris that closes over the screen: a black frame around a
 * square hole that turns and shrinks with 'level' */
void hudScreenFade(s32 level) {
    s32 *m;
    s32 s;
    s32 k;
    u32 a, b, c, d;

    bgDrawSetup(-1, 0);
    DL_PUSH(0xFCFFFFFF, 0xFFFDF6FB);         /* combine: primitive colour */
    DL_PUSH(0xFA000000, 0xFF);               /* black */
    DL_PUSH(0xBC000404, 2);                  /* clip ratios */
    DL_PUSH(0xBC000C04, 2);
    DL_PUSH(0xBC001404, 0xFFFE);
    DL_PUSH(0xBC001C04, 0xFFFE);
    /* outer square: vertices 4-7 */
    m = gMtxCursor;
    gMtxCursor = m + 16;
    mtxTranslate(m, 0, 0, 0x74D60000);
    m[0] = 0x320000;
    m[5] = 0x320000;
    DL_PUSH(0x01040040, (u32)(m - 16));
    DL_PUSH(0x04340040, (u32)gShadowSquareVtx);
    DL_PUSH(0xBD000000, 0);
    /* the hole: vertices 0-3 */
    s = (s32)(((f32)((f64)(f32)(0x4000 - tblCos(level >> 2)) / 16384.0) + 0.0082) * 65536.0 * 50.0);
    m = gMtxCursor;
    gMtxCursor = m + 16;
    mtxFromAngles(m, 0, 0, level);
    mtxScaleBy(m, s, s, 0x10000);
    m[12] = 0;
    m[13] = 0;
    m[14] = 0x74D60000;
    DL_PUSH(0x01040040, (u32)(m - 16));
    DL_PUSH(0x04300040, (u32)gShadowSquareVtx);
    DL_PUSH(0xBD000000, 0);
    /* the frame between them, starting from the hole's corner nearest the top */
    k = (level & 0xFFFF) >> 14;
    a = ((k & 3) * 10) & 0xFF;
    b = (((k + 1) & 3) * 10) & 0xFF;
    c = (((k + 2) & 3) * 10) & 0xFF;
    d = (((k + 3) & 3) * 10) & 0xFF;
    DL_PUSH(0xBF000000, 0x280000 | (a << 8) | b);
    DL_PUSH(0xBF000000, 0x280000 | (b << 8) | 0x32);
    DL_PUSH(0xBF000000, 0x320000 | (b << 8) | c);
    DL_PUSH(0xBF000000, 0x320000 | (c << 8) | 0x3C);
    DL_PUSH(0xBF000000, 0x3C0000 | (c << 8) | d);
    DL_PUSH(0xBF000000, 0x3C0000 | (d << 8) | 0x46);
    DL_PUSH(0xBF000000, 0x460000 | (d << 8) | a);
    DL_PUSH(0xBF000000, 0x460000 | (a << 8) | 0x28);
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xFC119623, 0xFF2FFFFF);
    DL_PUSH(0xFA000000, 0xFFFFFFFF);
    DL_PUSH(0xBC000404, 3);
    DL_PUSH(0xBC000C04, 3);
    DL_PUSH(0xBC001404, 0xFFFD);
    DL_PUSH(0xBC001C04, 0xFFFD);
}
