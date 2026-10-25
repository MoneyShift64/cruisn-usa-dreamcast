/*
 * ending.c - coast to coast: after the last race of Cruise the USA,
 * "C O N G R A T U L A T I O N S" with the total time(s) over the White House;
 * START joins the victory celebration (crowd, fireworks, the winning car on a
 * turntable, which then gets its faster-car award and paint), then the
 * HOT TIMES table for the whole trip, then the credits roll.
 * Original code: front-end overlay, ROM 0x68830-0x69B70 (RAM 0x8002FAA0-0x80030DE0).
 * (Declared as congratsInit in frontend.h.)
 *
 * gEndState: 0 waiting for START, 1-2 starting, 3 the scene fades in, 4-5 the
 * celebration, 6-7 the camera moves off, 8 the table, 9 table showing, 10 done.
 */
#include "common.h"
#include "frontend.h"
#include "race.h"

typedef struct SpriteFrame { s32 dl; f32 seconds; } SpriteFrame;

typedef struct EndSprite {               /* 0x18 bytes, table 0x80042B04 (7) */
    /* 0x00 */ SpriteFrame *frames;
    /* 0x04 */ s32 x, y, z;
    /* 0x10 */ f32 speed;
    /* 0x14 */ s32 anim;                 /* spriteAnimStart's */
} EndSprite;

typedef struct ViewerCam { s32 pos[3], angles[3], target[3], useAngles; } ViewerCam;

extern AnimCmd   gEndScriptA[];          /* 0x80042A28 ovl3 */
extern AnimCmd   gEndScriptB[];          /* 0x80042A50 ovl3 */
extern AnimCmd   gEndScriptC[];          /* 0x80042A78 ovl3 */
extern AnimCmd   gEndAwardIn[];          /* 0x80042AB4 ovl3 */
extern AnimCmd   gEndAwardOut[];         /* 0x80042ADC ovl3 */
extern EndSprite gEndSprites[7];         /* 0x80042B04 ovl3 */
extern f32       gEndBackfireChance;     /* 0x800458B0 ovl3 - 0.05 */
extern f32       gEndAwardDelay;         /* 0x800458B4 ovl3 - 1.2 */
extern f32       gEndPollDelay;          /* 0x800458B8 ovl3 - 0.1 */
extern f32       gEndPoll1;              /* 0x800458E0 ovl3 - 0.05 */
extern f32       gEndPoll2;              /* 0x800458E4 ovl3 - 0.05 */

extern s32       gEndFade;               /* 0x80049100 ovl3 */
extern s32       gEndAwardX;             /* 0x80049104 ovl3 - "FASTER CAR AWARDED!" */
extern s32       gEndNextX;              /* 0x80049108 ovl3 - "NEXT AWARD AT DIFFICULTY" */
extern void     *gEndDLs[28];            /* 0x80049110 ovl3 - group 0x2E9B */
extern s32       gEndState;              /* 0x80049180 ovl3 */
extern u32       gEndHeapMark;           /* 0x80049184 ovl3 */
extern s32       gEndWinner;             /* 0x80049188 ovl3 */
extern s32       gEndAwardDone;          /* 0x8004918C ovl3 */
extern RenderObj gEndCar;                /* 0x80049190 ovl3 */
extern TextObj  *gEndCongrats;           /* 0x80049480 ovl3 */
extern TextObj  *gEndPressStart;         /* 0x80049484 ovl3 */
extern TextObj  *gEndTitle;              /* 0x80049488 ovl3 */
extern TextObj  *gEndPlayerText[2];      /* 0x80049490 ovl3 */
extern TextObj  *gEndTimeText[2];        /* 0x80049498 ovl3 */
extern TextObj  *gEndWinnerText;         /* 0x800494A0 ovl3 */
extern TextObj  *gEndAwardText;          /* 0x800494A4 ovl3 */
extern TextObj  *gEndNextText;           /* 0x800494A8 ovl3 */

extern ViewerCam gViewerCams[2];         /* 0x8014EF90 */
extern s32       gFadeToWhite[2];        /* 0x80146140 */
extern s32       gBlinkFast;             /* 0x8014EF78 */
extern s32       gBlinkMedium;           /* 0x8014EF74 */
extern u32       gViCount;               /* 0x80147694 */
extern u32      *gDlCursor;              /* 0x80147684 */

TextObj *textCreateTime(s32 font, s32 value, s32 min, s32 max, s32 style);
void trackEnvInit(s32 track);
void skyDraw(s32 player, s32 yaw, s32 roll);
void viewBegin(s32 racer);
void hudScreenFade(s32 level);
s32  randChance(f32 p);
void carSetPalette(s32 slot, s32 car, s32 paint);
s32  spriteAnimStart(SpriteFrame *frames, void **dls, s32 start, f32 speed, s32 x, s32 y, s32 z);
void spriteAnimStop(s32 i);
void spriteAnimDraw(s32 i);
void optionsMeter(s32 x, s32 y, s32 selected, s32 value, s32 alpha, s32 unused, void *box, void *dl1,
                  void *dl2, void *tick, void *marker);
void trophyInit(s32 track);
void trophyStart(s32 track, void (*done)(void));
void trophyFrame(void);

void endFrame(s32 arg);
void endSceneDraw(void);
f32  endAwardIn(s32 ramp, s32 user);
f32  endAwardOut(s32 ramp, s32 user);
void endBegin(void);
void endCelebrate(void);
void endCameraOff(void);
s32  endAnimDone(s32 slot, s32 ramp, f32 t);
f32  endStep(s32 ramp, s32 user);
void endTableDone(void);
void endFree(void);

#define DL_PUSH(w0, w1) { u32 *_g = gDlCursor; gDlCursor = _g + 2; _g[0] = (w0); _g[1] = (u32)(w1); }
#define PS_FLAGS(ps) (((u8 *)&(ps)->unk01C)[0])

/* 0x8002FAA0 ovl3 */
void congratsInit(s32 arg) {
    char buf[16];
    EndSprite *e;
    s32 p, w, y;

    gFadeToWhite[1] = 0;
    gFadeToWhite[0] = 0;
    gEndHeapMark = lowMemOverlayLoad();
    assetLoadGroup(0x2E9B, gEndDLs);
    lowMemOverlayRelease();
    trackEnvInit(3);
    gEndCongrats = textCreate(1, "C O N G R A T U L A T I O N S", 2);
    for (p = 0; p <= gNumPlayersM1; p++) {
        gEndTimeText[p] = textCreateTime(3, (s32)(gPlayerSetup[p].totalTime * 100.0f), 0, 0x927BF, 0);
        if (gNumPlayersM1 != 0) {
            y = p * -0x110000;
            sprintf(buf, "PLAYER %c", p + '1');
            gEndPlayerText[p] = textCreate(0, buf, 0);
            textPlace(&gEndPlayerText[p]->xf, 0x190000, y + 0x6D0000, 0, 0x10000);
            textPlace(&gEndTimeText[p]->xf, 0x730000, y + 0x6C0000, 0, 0x10000);
        } else {
            textPlace(&gEndTimeText[p]->xf, 0xBE0000, 0x850000, 0, 0x10000);
        }
    }
    if (gNumPlayersM1 != 0) {
        gEndTitle = textCreate(0, "COAST-TO-COAST TIMES:", 0);
        gEndWinnerText = textCreate(1, "WINNER", 0);
        w = (gPlayerSetup[1].totalTime < gPlayerSetup[0].totalTime) ? 1 : 0;
        textPlace(&gEndWinnerText->xf, 0xB90000, w * -0x110000 + 0x6E0000, 0, 0x10000);
        gEndWinnerText->blink = gBlinkFast;
        gEndWinnerText->blinkLevel = 0.75f;
    } else {
        gEndTitle = textCreate(0, "COAST-TO-COAST IN", 0);
        gEndTitle->blink = gBlinkMedium;
        gEndTitle->blinkLevel = 0.75f;
        gEndTimeText[0]->blink = gBlinkMedium;
        gEndTimeText[0]->blinkLevel = 0.75f;
    }
    textPlace(&gEndTitle->xf, 0x140000, 0x860000, 0, 0x10000);
    gEndPressStart = textCreate(0, "PRESS START TO JOIN THE\nVICTORY CELEBRATION...", 2);
    textPlace(&gEndCongrats->xf, 0x8B0000, 0xB30000, 0, 0);
    textPlace(&gEndPressStart->xf, 0x8B0000, 0x2D0000, 0, 0);
    gEndPressStart->flags |= TEXTF_COLOR;
    gEndPressStart->color = 0x327832FF;
    gEndAwardX = 0x8B0000;
    gEndNextX = 0x3E80000;
    gEndAwardText = textCreate(0, "FASTER CAR AWARDED!", 2);
    textPlace(&gEndAwardText->xf, gEndAwardX, 0xAE0000, 0, 0);
    gEndAwardText->blink = gBlinkMedium;
    gEndAwardText->blinkLevel = 0.75f;
    gEndNextText = textCreate(0, "NEXT AWARD AT DIFFICULTY", 2);
    textPlace(&gEndNextText->xf, gEndNextX, 0xAE0000, 0, 0);
    for (e = gEndSprites; e != &gEndSprites[7]; e++) {
        e->anim = spriteAnimStart(e->frames, gEndDLs, 0, e->speed, e->x, e->y, e->z);
    }
    gViewerCams[0].pos[0] = 0x280000;
    gViewerCams[0].pos[1] = 0xC80000;
    gViewerCams[0].pos[2] = 0x0A280000;
    gViewerCams[0].angles[0] = 0;
    gViewerCams[0].angles[1] = 0;
    gViewerCams[0].angles[2] = 0;
    gViewerCams[0].useAngles = 1;
    gEndWinner = 0;
    if (gNumPlayersM1 != 0 && gPlayerSetup[1].totalTime < gPlayerSetup[0].totalTime) {
        gEndWinner = 1;
    }
    renderObjInit(&gEndCar, gPlayerSetup[gEndWinner].car, gEndWinner, 0x199, 0, 0x1E0000, 0x02BC0000, -0xA00000);
    gEndCar.steerAngle = 0xFA0;
    gEndAwardDone = 0;
    gEndFade = 0;
    endBegin();
    gModeFunc = endFrame;
    endFrame(0);
}

/* 0x8003004C ovl3 */
void endFrame(s32 arg) {
    s32 p, s;

    if (gEndState == 0) {
        /* the winner (or player 1) starts the celebration */
        p = 0;
        if (gNumPlayersM1 != 0 && gPlayerSetup[1].totalTime < gPlayerSetup[0].totalTime) {
            p = 1;
        }
        if (contPressed(p, BTN_ALL) & BTN_START) {
            gEndState = 1;
        }
    }
    if (gEndState == 9) {
        trophyFrame();
        return;
    }
    if (gEndState == 10) {
        if (gPlayerSetup[0].unk02C >= 10 && (gNumPlayersM1 == 0 || gPlayerSetup[1].unk02C >= 10)) {
            musicPlay(0xA5);
        }
        contPressed(0, BTN_ALL);
        endFree();
        gModeFunc = hiscoreInit;
        gModeArg = 0;
        hiscoreInit(0);
        return;
    }
    screenClear(-1, 1);
    s = gEndState;
    if (s >= 3 && s < 10) {
        endSceneDraw();
    }
    if (s == 3) {
        hudScreenFade(gEndFade);
        gEndFade += 0x5DC;
        if (gEndFade >= 0x10000) {
            gEndState = 4;
            animWait(1.0f, NULL, (AnimDoneFunc)endStep);
        }
    }
    if ((gEndState == 7 || gEndState == 8) && gEndFade < 0x10000) {
        hudScreenFade(gEndFade);
    }
}

/* 0x80030254 ovl3 - the White House, the crowd and fireworks, the winning car turning */
void endSceneDraw(void) {
    PlayerSetup *ps;
    EndSprite *e;
    s32 v;

    gEndAwardText->xf.x = gEndAwardX;
    gEndNextText->xf.x = gEndNextX;
    skyDraw(-1, 0x8000, 0);
    viewBegin(-1);
    drawDL(gEndDLs[1], 0, 0, 0, 0x10000);
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xBA000C02, 0);
    drawDL(gEndDLs[2], 0, 0, 0, 0x10000);
    drawDL(gEndDLs[3], 0, 0, 0, 0x10000);
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xBA000C02, 0x2000);
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xBA000C02, 0);
    drawDL(gEndDLs[4], 0, 0, 0, 0x10000);
    drawDL(gEndDLs[5], 0, 0, 0, 0x10000);
    drawDL(gEndDLs[6], 0, 0, 0, 0x10000);
    drawDL(gEndDLs[7], 0, 0, 0, 0x10000);
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xBA000C02, 0x2000);
    drawDL(gEndDLs[9], 0x1E0000, 0, -0xA00000, 0x10000);
    renderObjDraw(&gEndCar);
    if (randChance(gEndBackfireChance)) {
        gEndCar.flags |= RO_BACKFIRE;
    }
    gEndCar.angleZ -= 0x140;
    if (gEndCar.angleZ < -100000 && gEndAwardDone == 0) {
        /* round once: the award's paint */
        gEndAwardDone = 1;
        ps = &gPlayerSetup[gEndWinner];
        if (ps->unk008 != ps->carAwards[ps->car]) {
            sfxPlay(0x9A, -1);
            sfxPlay(0x9A, -1);
            ps = &gPlayerSetup[gEndWinner];
            carSetPalette(gEndWinner, ps->car, ps->carAwards[ps->car]);
            ps = &gPlayerSetup[gEndWinner];
            if (ps->awarded != 0) {
                gEndAwardText->xf.visible = 1;
                ps = &gPlayerSetup[gEndWinner];
            }
        }
        if (ps->nextAward != -1) {
            gEndNextText->xf.visible = 1;
            animWait(5.0f, NULL, (AnimDoneFunc)endAwardIn);
        }
    }
    for (e = gEndSprites; e != &gEndSprites[7]; e++) {
        spriteAnimDraw(e->anim);
    }
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xBA000C02, 0);
    drawDL(gEndDLs[0], 0, 0, 0, 0x10000);
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xBA000C02, 0x2000);
    drawDL(gEndDLs[8], 0, 0, 0, 0x10000);
    if (gEndNextText->xf.visible != 0) {
        bgDrawSetup(-1, 1);
        v = (gViCount & 8) ? gPlayerSetup[gEndWinner].nextAward : -1;
        optionsMeter(gEndNextX - 0x10000, 0x9A0000, 1, v, 0xFF, 1,
                     gEndDLs[22], gEndDLs[23], gEndDLs[24], gEndDLs[25], gEndDLs[26]);
    }
}

/* 0x800307C8 ovl3 - (animWait) the award texts slide */
f32 endAwardIn(s32 ramp, s32 user) {
    gEndAwardText->blink = -1;
    animStart(1.0f, gEndAwardIn, NULL, NULL, ANIM_ONCE);
    animWait(gEndAwardDelay, NULL, (AnimDoneFunc)endAwardOut);
    return -1.0f;
}

/* 0x80030838 ovl3 */
f32 endAwardOut(s32 ramp, s32 user) {
    animStart(1.0f, gEndAwardOut, NULL, NULL, ANIM_ONCE);
    return -1.0f;
}

/* 0x80030880 ovl3 */
void endBegin(void) {
    musicPlay(0xA1);
    gEndState = 0;
    gEndCongrats->xf.visible = 1;
    gEndPressStart->xf.visible = 1;
    animWait(gEndPollDelay, NULL, (AnimDoneFunc)endStep);
}

/* 0x800308DC ovl3 */
void endCelebrate(void) {
    gEndState = 5;
    animStart(5.0f, gEndScriptA, NULL, NULL, ANIM_ONCE);
    animStart(8.0f, gEndScriptB, NULL, endAnimDone, ANIM_ONCE);
}

/* 0x8003094C ovl3 */
void endCameraOff(void) {
    gEndState = 7;
    animStart(2.5f, gEndScriptC, NULL, endAnimDone, ANIM_ONCE);
}

/* 0x80030998 ovl3 */
s32 endAnimDone(s32 slot, s32 ramp, f32 t) {
    if (gEndState == 5) {
        gEndState = 6;
        animWait(13.0f, NULL, (AnimDoneFunc)endStep);
    } else {
        gEndState = 8;
        animWait(0.5f, NULL, (AnimDoneFunc)endStep);
    }
    return 2;
}

/* 0x80030A1C ovl3 - (animWait) the sequence's steps; returns the time to the next call
 * (-1: no more) */
f32 endStep(s32 ramp, s32 user) {
    s32 p, w;

    switch (gEndState) {
        case 0:                              /* still waiting for START */
            return gEndPoll1;
        case 1:
            gEndState = 2;
            return gEndPoll2;
        case 2:
            sfxPlay(0x22, -1);
            sfxPlay(0x20, -1);
            gEndState = 3;
            gEndCongrats->xf.visible = 0;
            gEndPressStart->xf.visible = 0;
            gEndTitle->xf.visible = 0;
            if (gNumPlayersM1 != 0) {
                gEndWinnerText->xf.visible = 0;
            }
            for (p = 0; p <= gNumPlayersM1; p++) {
                if (gNumPlayersM1 != 0) {
                    gEndPlayerText[p]->xf.visible = 0;
                }
                gEndTimeText[p]->xf.visible = 0;
            }
            return -1.0f;
        case 4:
            endCelebrate();
            return -1.0f;
        case 6:
            sfxPlay(0x22, -1);
            sfxStop(0x20, -1);
            gEndAwardText->xf.visible = 0;
            gEndNextText->xf.visible = 0;
            endCameraOff();
            return -1.0f;
        case 8:
            /* the trip's total goes in the Cruise the USA table */
            gEndState = 9;
            for (p = 0; p <= gNumPlayersM1; p++) {
                PS_FLAGS(&gPlayerSetup[p]) = 0;
                gPlayerSetup[p].raceTime = gPlayerSetup[p].totalTime;
            }
            w = 0;
            if (gNumPlayersM1 != 0 && gPlayerSetup[1].totalTime < gPlayerSetup[0].totalTime) {
                w = 1;
            }
            PS_FLAGS(&gPlayerSetup[w]) = 9;
            trophyInit(14);
            trophyStart(14, endTableDone);
            return -1.0f;
    }
    return -1.0f;                            /* (3, 5, 7: the original returns an undefined value) */
}

/* 0x80030C98 ovl3 */
void endTableDone(void) {
    gEndState = 10;
}

/* 0x80030CAC ovl3 */
void endFree(void) {
    EndSprite *e;
    s32 p;

    for (e = gEndSprites; e != &gEndSprites[7]; e++) {
        spriteAnimStop(e->anim);
    }
    textFree(gEndCongrats);
    textFree(gEndPressStart);
    textFree(gEndTitle);
    textFree(gEndAwardText);
    textFree(gEndNextText);
    if (gNumPlayersM1 != 0) {
        textFree(gEndWinnerText);
    }
    for (p = 0; p <= gNumPlayersM1; p++) {
        if (gNumPlayersM1 != 0) {
            textFree(gEndPlayerText[p]);
        }
        textFree(gEndTimeText[p]);
    }
    lowMemOverlayLoad();
    highHeapRestore(gEndHeapMark);
    lowMemOverlayRelease();
}
