/*
 * credits.c - the scrolling credits roll ("COIN-OP TEAM", "EUGENE JARVIS", ...)
 * shown by the title screen every fourth time round (and after the intro).
 * START (from the title) skips straight to Choose Race.
 * Original code: front-end overlay, RAM 0x8002E750-0x8002EFC0.
 * (Declared as hiscoreInit in frontend.h: that was a guess; this is the roll.)
 */
#include "common.h"
#include "frontend.h"

/* One line of the roll (0x18 bytes, table 0x800421D0, ends with kind 0x8000) */
typedef struct CreditLine {
    /* 0x00 */ s32 kind;                     /* text layer (font), 0x100 = a picture, 0x8000 = end */
    /* 0x04 */ char *text;                   /* the text, or (pictures) an index in gRollDLs */
    /* 0x08 */ s32 x;                        /* pixels */
    /* 0x0C */ s32 y;                        /* pixels, from the top of the roll */
    /* 0x10 */ s32 style;                    /* text style, or (pictures) the scale */
    /* 0x14 */ TextObj *t;                   /* while on screen */
} CreditLine;

extern CreditLine gCreditLines[];        /* 0x800421D0 ovl3 */
extern s32 gRollState;                   /* 0x80049050 ovl3 - 0 wait, 1 fade in, 2 wait, 3 scroll, 4 hold, 5 fade out, 6 wait, 7 done */
extern s32 gRollAlpha;                   /* 0x80049054 ovl3 */
extern s32 gRollTimer;                   /* 0x80049058 ovl3 */
extern u32 gRollHeapMark;                /* 0x8004905C ovl3 */
extern s32 gRollScroll;                  /* 0x80049060 ovl3 - 16.16 */
extern s32 gRollShown;                   /* 0x80049064 ovl3 - a line has come on screen */
extern s32 gRollFromTitle;               /* 0x80049068 ovl3 */
extern void *gRollDLs[];                 /* 0x80049070 ovl3 - group 0x2EC0 */
extern u32 *gDlCursor;                   /* 0x80147684 */

void titleInit(s32 skip);
void creditsInit(s32 arg);               /* 0x8003AC10 ovl3 */
void drawDL(void *dl, s32 x, s32 y, s32 z, s32 scale);

void rollFrame(s32 arg);
void rollFade(void);
void rollExit(s32 toChooseRace);
s32  rollFadeIn(s32 y, s32 limit, s32 step);

#define DL_PUSH(w0, w1) { u32 *_g = gDlCursor; gDlCursor = _g + 2; _g[0] = (w0); _g[1] = (u32)(w1); }

/* 0x8002E750 ovl3 - start the roll (fromTitle: START skips it, and it is followed by 0x8003AC10) */
void hiscoreInit(s32 fromTitle) {
    CreditLine *l;

    gRollFromTitle = fromTitle;
    gRollHeapMark = lowMemOverlayLoad();
    assetLoadGroup(0x2EC0, gRollDLs);
    lowMemOverlayRelease();
    for (l = gCreditLines; l->kind != 0x8000; l++) {
        l->t = NULL;
    }
    gRollScroll = -0x1E0000;
    gRollShown = 0;
    gRollState = 0;
    gRollTimer = 20;
    gRollAlpha = 0xFF;
    contPressed(0, BTN_ALL);
    contPressed(1, BTN_ALL);
    gModeFunc = rollFrame;
    rollFrame(0);
}

/* 0x8002E824 ovl3 */
void rollFrame(s32 arg) {
    CreditLine *l;
    s32 y, x, top, wasOn, isOn, create, remove, allOff;
    s32 state = gRollState;

    if (state == 7) {
        rollExit(0);
        return;
    }
    if (gRollFromTitle != 0 && (contPressed(0, BTN_START) & BTN_START)) {
        rollExit(1);
        return;
    }
    if (gRollFromTitle != 0 && optionsCheck(rollFrame, 1) != 0) {
        return;
    }
    screenClear(-1, 1);
    bgDrawSetup(-1, 1);
    top = 0x03E80000;
    if (gRollState >= 3) {
        allOff = 1;
        for (l = gCreditLines; l->kind != 0x8000; l++) {
            y = (l->y << 16) + gRollScroll;
            create = 0;
            remove = 0;
            x = l->x << 16;
            if (y < top) {
                top = y;
            }
            wasOn = y < 0x010D0001 && y >= -0x140000;
            y += 0x10000;
            isOn = y < 0x010D0001 && y >= -0x140000;
            if (isOn) {
                create = !wasOn;
                allOff = 0;
                gRollShown = 1;
            } else {
                remove = wasOn;
            }
            if (l->kind == 0x100) {
                if (wasOn) {
                    DL_PUSH(0xE7000000, 0);
                    DL_PUSH(0xFA000000, (rollFadeIn(y, 0x3C0000, 5) & 0xFF) | ~0xFF);
                    drawDL(gRollDLs[(s32)l->text], x, y, 0, l->style);
                    DL_PUSH(0xE7000000, 0);
                    DL_PUSH(0xFA000000, 0xFFFFFFFF);
                }
            } else if (create) {
                l->t = textCreate(l->kind, l->text, l->style);
                textPlace(&l->t->xf, x, y, 0, 1);
                l->t->flags |= TEXTF_COLOR;
            } else if (remove) {
                textFree(l->t);
                l->t = NULL;
            } else if (wasOn) {
                l->t->color = rollFadeIn(y, 0x500000, 5) - 0x100;
                l->t->xf.y = y;
            }
        }
        /* the logo at the end of the roll */
        y = top - 0x960000;
        if (y > -0x460000) {
            DL_PUSH(0xE7000000, 0);
            DL_PUSH(0xFA000000, (rollFadeIn(y, 0x5A0000, 5) & 0xFF) | ~0xFF);
            drawDL(gRollDLs[1], 0x8B0000, y, 0, 0x10000);
            DL_PUSH(0xE7000000, 0);
            DL_PUSH(0xFA000000, 0xFFFFFFFF);
            rollFade();
        }
        if (gRollState == 3) {
            gRollScroll += 0x10000;
        }
    } else {
        allOff = 1;
    }
    /* the title picture at the start of the roll */
    y = gRollScroll + 0x960000;
    if (y < 0x010D0000) {
        drawDL(gRollDLs[0], 0x8B0000, y, 0, 0x10000);
        rollFade();
    }
    state = gRollState;
    if (state == 0 && --gRollTimer == 0) {
        gRollState = state = 1;
    }
    if (state == 2 && --gRollTimer == 0) {
        gRollState = state = 3;
    }
    if (state == 4 && --gRollTimer == 0) {
        gRollState = 5;
        gRollAlpha = 0;
        state = gRollState;
    }
    if (state == 6 && --gRollTimer == 0) {
        gRollState = 7;
    }
    if (gRollShown != 0 && allOff != 0 && gRollState == 3) {
        gRollState = 4;
        gRollTimer = 90;
    }
}

/* 0x8002ED60 ovl3 - the black fade over everything at the start and the end */
void rollFade(void) {
    if (gRollState == 1 || gRollState == 5 || gRollState == 6 || gRollState == 0) {
        rectFill(0x118, 0xC8, 0, 0xC70000, 0, 0, 0, gRollAlpha);
        if (gRollState == 1) {
            gRollAlpha -= 8;
            if (gRollAlpha <= 0) {
                gRollState = 2;
            }
            gRollTimer = 30;
            return;
        }
        if (gRollState == 5) {
            gRollAlpha += 8;
            if (gRollAlpha >= 0xFF) {
                gRollState = 6;
                gRollTimer = 20;
                gRollAlpha = 0xFF;
            }
        }
    }
}

/* 0x8002EE58 ovl3 - leave: START -> the title (which goes on to Choose Race), else the next screen */
void rollExit(s32 toChooseRace) {
    CreditLine *l;

    contPressed(0, BTN_ALL);
    lowMemOverlayLoad();
    highHeapRestore(gRollHeapMark);
    lowMemOverlayRelease();
    for (l = gCreditLines; l->kind != 0x8000; l++) {
        if (l->t != NULL) {
            textFree(l->t);
        }
    }
    if (toChooseRace != 0) {
        gModeFunc = titleInit;
        gModeArg = 1;
    } else if (gRollFromTitle != 0) {
        gModeFunc = creditsInit;
        gModeArg = 0;
    } else {
        gModeFunc = frontEndEnter;
        gModeArg = 1;
    }
    gModeFunc(gModeArg);
}

/* 0x8002EF78 ovl3 - 255 below 'limit', fading by 'step' per pixel above it */
s32 rollFadeIn(s32 y, s32 limit, s32 step) {
    s32 a = 0xFF;

    if (y < limit) {
        a = 0xFF - ((s32)(limit - y) >> 16) * step;
        if (a < 0) {
            a = 0;
        }
    }
    return a;
}
