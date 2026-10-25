/*
 * continue.c - the "CONTINUE?  NO  YES" box (Cruise the USA after a lost race,
 * and resuming a saved Cruise the USA): left/right picks, the choice grows and
 * glows, A/START/Z confirms.
 * Original code: front-end overlay, RAM 0x8002EFC0-0x8002FAA0.
 */
#include "common.h"
#include "frontend.h"

extern void *gContDLs[];                 /* 0x800490C0 ovl3 - group 0x2E46 */
extern u32 gContHeapMark;                /* 0x800490C8 ovl3 */
extern s32 gContPulse;                   /* 0x800490CC ovl3 */
extern TextObj *gContTitle;              /* 0x800490D0 ovl3 */
extern TextObj *gContYes;                /* 0x800490D4 ovl3 */
extern TextObj *gContNo;                 /* 0x800490D8 ovl3 */
extern TextObj *gContTitleShadow;        /* 0x800490DC ovl3 */
extern TextObj *gContYesShadow;          /* 0x800490E0 ovl3 */
extern TextObj *gContNoShadow;           /* 0x800490E4 ovl3 */
extern s32 gContY;                       /* 0x800490E8 ovl3 - 16.16 */
extern s32 gContChoice;                  /* 0x800490EC ovl3 - 1 YES, 0 NO */
extern f32 gContSlide;                   /* 0x800490F0 ovl3 - 0 (NO) .. 1 (YES) */
extern u32 *gDlCursor;                   /* 0x80147684 */

void drawDL(void *dl, s32 x, s32 y, s32 z, s32 scale);
s32  tblCos(s32 angle);                    /* 0x80113EDC */
void continueDraw(void);
s32  continuePulse(void);
void continueFree(void);

#define DL_PUSH(w0, w1) { u32 *_g = gDlCursor; gDlCursor = _g + 2; _g[0] = (w0); _g[1] = (u32)(w1); }

/* 0x8002EFC0 ovl3 - show the box, y pixels up from its usual place */
void continueInit(s32 y) {
    gContY = y << 16;
    gContHeapMark = lowMemOverlayLoad();
    assetLoadGroup(0x2E46, gContDLs);
    lowMemOverlayRelease();
    gContTitle = textCreate(2, "CONTINUE?", 2);
    textPlace(&gContTitle->xf, 0x8B0000, gContY + 0x820000, 0, 1);
    gContYes = textCreate(0, "YES", 2);
    textPlace(&gContYes->xf, 0xA90000, gContY + 0x500000, 0, 1);
    gContNo = textCreate(0, "NO", 2);
    textPlace(&gContNo->xf, 0x6D0000, gContY + 0x500000, 0, 1);
    gContTitleShadow = textCreate(2, "CONTINUE?", 2);
    textPlace(&gContTitleShadow->xf, 0x8F0000, gContY + 0x7E0000, 0, 1);
    gContYesShadow = textCreate(0, "YES", 2);
    textPlace(&gContYesShadow->xf, 0xA90000, 0x500000, gContY, 1);   /* (z, not y: placed each frame anyway) */
    gContNoShadow = textCreate(0, "NO", 2);
    textPlace(&gContNoShadow->xf, 0x6D0000, 0x500000, gContY, 1);
    gContTitle->flags |= TEXTF_COLOR;
    gContTitle->color = 0xFFFFFFFF;
    gContTitleShadow->flags |= TEXTF_COLOR | TEXTF_HIDDEN;
    gContYes->flags |= TEXTF_COLOR | TEXTF_HIDDEN;
    gContNo->flags |= TEXTF_COLOR | TEXTF_HIDDEN;
    gContYesShadow->flags |= TEXTF_COLOR | TEXTF_HIDDEN;
    gContNoShadow->flags |= TEXTF_COLOR | TEXTF_HIDDEN;
    gContTitleShadow->color = 0x96;
    gContYesShadow->color = 0x96;
    gContNoShadow->color = 0x96;
    gContChoice = 1;
    gContSlide = 1.0f;
    sfxPlay(0x4E, -1);
    gContPulse = 0;
}

/* 0x8002F27C ovl3 - every frame: 1 YES, 0 NO, -1 still choosing (either player can answer) */
s32 continueFrame(void) {
    s32 p, prev;

    continueDraw();
    for (p = 0; p <= gNumPlayersM1; p++) {
        prev = gContChoice;
        if (contPressed(p, BTN_LEFT) & BTN_LEFT) {
            gContChoice = 0;
        }
        if (contPressed(p, BTN_RIGHT) & BTN_RIGHT) {
            gContChoice = 1;
        }
        if (prev != gContChoice) {
            sfxPlay(0x91, -1);
        }
        if (contPressed(p, BTN_A | BTN_START | BTN_Z) & (BTN_A | BTN_START | BTN_Z)) {
            sfxPlay(gContChoice != 0 ? 0x38 : 0x60, -1);
            continueFree();
            return gContChoice;
        }
    }
    if (gContChoice != 0) {
        gContSlide = (f32)((f64)gContSlide + ((1.0 - (f64)gContSlide) / 3.0 + 0.05));
        if ((f64)gContSlide > 1.0) {
            gContSlide = 1.0f;
        }
    } else {
        gContSlide = (f32)((f64)gContSlide + ((0.0 - (f64)gContSlide) / 3.0 - 0.05));
        if ((f64)gContSlide < 0.0) {
            gContSlide = 0.0f;
        }
    }
    gContPulse += 0x800;
    return -1;
}

/* 0x8002F4C0 ovl3 */
void continueDraw(void) {
    f32 f = gContSlide;
    f64 g;
    s32 pulse, c;

    bgDrawSetup(-1, 1);
    rectFill(0x140, 0xF0, -0x140000, 0xDB0000, 0x14, 0x14, 0x14, 0x96);
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xFA000000, 0x4D76A5FF);
    drawDL(gContDLs[0], 0x8B0000, gContY + 0x630000, 0, 0x10000);
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xFA000000, 0xFFFFFFFF);
    if ((f64)f == 0.0) {
        gContYes->xf.scale = 0x10000;
        gContNo->xf.scale = 0x1FFFE;
    } else if ((f64)f == 1.0) {
        gContYes->xf.scale = 0x1FFFE;
        gContNo->xf.scale = 0x10000;
    } else {
        gContYes->xf.scale = (s32)(((f64)f + 1.0) * 65535.0);
        gContNo->xf.scale = (s32)((2.0 - (f64)gContSlide) * 65535.0);
    }
    gContYes->xf.y = (s32)((f64)(6.0f * gContSlide) * 65536.0) + gContY + 0x500000;
    gContNo->xf.y = (s32)((1.0 - (f64)gContSlide) * 6.0 * 65536.0) + gContY + 0x500000;
    pulse = continuePulse();
    g = 1.0 - (f64)gContSlide;
    gContYes->color = ((s32)(g * 55.0 + (f64)(gContSlide * (f32)pulse))) * 0x01010100 + 0xFF;
    c = (s32)((f64)(gContSlide * 55.0f) + g * (f64)pulse);
    gContNo->color = c * 0x01010100 + 0xFF;
    gContYesShadow->xf.scale = gContYes->xf.scale;
    gContNoShadow->xf.scale = gContNo->xf.scale;
    gContYesShadow->xf.x = (s32)((f32)gContYes->xf.x + gContSlide * 524288.0f + 131072.0f);
    gContYesShadow->xf.y = (s32)(((f32)gContYes->xf.y - gContSlide * 524288.0f) - 131072.0f);
    gContNoShadow->xf.x = (s32)((f64)gContNo->xf.x + (1.0 - (f64)gContSlide) * 524288.0 + 131072.0);
    gContNoShadow->xf.y = (s32)(((f64)gContNo->xf.y - (1.0 - (f64)gContSlide) * 524288.0) - 131072.0);
    textDraw(gContYesShadow, 0);
    textDraw(gContNoShadow, 0);
    textDraw(gContTitleShadow, 0);
    DL_PUSH(0xBA000C02, 0);                  /* texture filter: point (sharp letters) */
    textDraw(gContYes, 0);
    textDraw(gContNo, 0);
    DL_PUSH(0xBA000C02, 0x2000);             /* back to bilinear */
}

/* 0x8002F9B4 ovl3 - the chosen word's glow */
s32 continuePulse(void) {
    return (s32)((f64)((f32)((f64)(f32)tblCos(gContPulse) / 32768.0) * 127.0f) + 192.0);
}

/* 0x8002FA20 ovl3 */
void continueFree(void) {
    lowMemOverlayLoad();
    highHeapRestore(gContHeapMark);
    lowMemOverlayRelease();
    textFree(gContTitle);
    textFree(gContYes);
    textFree(gContNo);
    textFree(gContTitleShadow);
    textFree(gContYesShadow);
    textFree(gContNoShadow);
}
