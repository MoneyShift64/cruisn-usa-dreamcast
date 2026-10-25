/*
 * carselect.c - CHOOSE CAR: each player picks one of four cars on turntables
 * (left/right), its faster-car award level (up/down: the paint and engine
 * earned in Cruise the USA), and the transmission; holding the secret
 * C-button combination swaps the first three for the Jeep, School Bus and
 * Police Car. The chosen car then drives off its turntable and the camera
 * swoops down behind it onto the start line of the race.
 * Original code: front-end overlay, ROM 0x64420-0x65420 (RAM 0x8002B690-0x8002E750).
 *
 * The screen is run by a chain of animation steps (gCsSteps): each step plays
 * an animation script and names the step after it; gCsStepKind tells the
 * frame function what to do meanwhile:
 *   0 intro            1 choose the transmission  2 (sound) cars come in
 *   3 move on to the cars (picks the next step)   4 player 2 joined
 *   5,6,7 cars appear  8 choose the car           9 (unused)
 * Per player (gCsPlayerState): 0 transmission, 1 done with it, 2 choosing a
 * car, 3 car chosen (turntable turning), 4 driving off, 5 camera swooping,
 * 6 at the start line.
 */
#include "common.h"
#include "frontend.h"
#include "race.h"

/* A step of the screen's animation chain (12 bytes, table 0x80042124) */
typedef struct CsStep {
    /* 0x00 */ u8  kind;                     /* gCsStepKind while it plays */
    /* 0x01 */ u8  nextKind;                 /* ... once it has finished */
    /* 0x02 */ s8  next;                     /* step played next, -1 = none */
    /* 0x03 */ u8  pad03;
    /* 0x04 */ f32 seconds;
    /* 0x08 */ AnimCmd *script;
} CsStep;

/* The camera of each player's view while their car drives off */
typedef struct CsSwoop {
    /* 0x00 */ s32 angle;                    /* around the car (binary angle) */
    /* 0x04 */ s32 dist;
    /* 0x08 */ s32 t;                        /* 0..10000000: from the turntable to the start line */
} CsSwoop;

/* A turntable's turn when the player moves on (eases back to straight) */
typedef struct CsTurn {
    /* 0x00 */ s32 angle;
    /* 0x04 */ s32 y;                        /* the turntable's height then */
} CsTurn;

/* data */
extern XformCmd gCsXfScriptPlayer[];        /* 0x800414C0 ovl3 - each player's half */
extern XformCmd gCsXfScript[];              /* 0x80041528 ovl3 */
extern u8      gCsDrawCars[];               /* 0x80041588 ovl3 - drawDLScript: the turntables */
extern u8      gCsDrawTitle[];              /* 0x800415AC ovl3 */
extern u8      gCsDrawFrame[];              /* 0x800415B4 ovl3 */
extern AnimCmd gCsCamDrop[];                /* 0x80041C7C ovl3 - the player's camera into the race view */
extern AnimCmd gCsSwoop1P[];                /* 0x80041CCC ovl3 */
extern AnimCmd gCsSwoop2P[];                /* 0x80041D08 ovl3 */
extern AnimCmd gCsShakeA[];                 /* 0x80041D44 ovl3 */
extern AnimCmd gCsShakeB[];                 /* 0x80041D6C ovl3 */
extern AnimCmd gCsShakeC[];                 /* 0x80041D94 ovl3 */
extern AnimCmd gCsShakeD[];                 /* 0x80041DBC ovl3 */
extern AnimCmd *gCsDriveOff[2][4];          /* 0x80042104 ovl3 - per player, per car */
extern CsStep  gCsSteps[10];                /* 0x80042124 ovl3 */
extern s32     gCsCamZ[2];                  /* 0x800421A4 ovl3 - per player count */
extern s32     gCsCarX[4];                  /* 0x800421AC ovl3 - turntable positions */
extern u8      gCsModels[2][4];             /* 0x800421BC ovl3 - [secret held][turntable] */

/* bss */
extern Xform    gCsXf[35];                  /* 0x80047270 ovl3 - 13 per player, then shared */
extern CsSwoop  gCsSwoop[2];                /* 0x800476D0 ovl3 */
extern s32      gCsShake[2];                /* 0x800476E8 ovl3 - start-line rumble */
extern u32      gCsHeapMark;                /* 0x800476F0 ovl3 */
extern s32      gCsStepKind;                /* 0x800476F4 ovl3 */
extern s32      gCsPlayerState[2];          /* 0x800476F8 ovl3 */
extern s32      gCsCar[2];                  /* 0x80047700 ovl3 - turntable 0-3 */
extern s32      gCsSecret[2];               /* 0x80047708 ovl3 - the secret combination is held */
extern s32      gCsLevel[2][7];             /* 0x80047710 ovl3 - award level chosen per car */
extern TextObj *gCsTitle;                   /* 0x80047748 ovl3 - "CHOOSE CAR" */
extern RenderObj gCsCarObjs[2][4];          /* 0x80047750 ovl3 */
extern RenderObj *gCsCars[2][4];            /* 0x80048ED0 ovl3 */
extern s32      gCsTrackReady;              /* 0x80048EF0 ovl3 */
extern s32      gCsStartZ0[2];              /* 0x80048EF8 ovl3 - the race car's grid place */
extern s32      gCsStartZ[2];               /* 0x80048F00 ovl3 */
extern s32      gCsStartAY[2];              /* 0x80048F08 ovl3 */
extern s32      gCsStartAX[2];              /* 0x80048F10 ovl3 */
extern s32      gCsSkyOn[2];                /* 0x80048F18 ovl3 */
extern f32      gCsSlope;                   /* 0x80048F20 ovl3 */
extern CsTurn   gCsTurn[2][4];              /* 0x80048F28 ovl3 */
extern void    *gCsDLs[64];                 /* 0x80048F68 ovl3 - group 0x2BFA */

typedef struct ViewerCam { s32 pos[3], angles[3], target[3], useAngles; } ViewerCam;
typedef struct ShadowSize { s32 w, l, unk08; } ShadowSize;
extern ViewerCam gViewerCams[2];            /* 0x8014EF90 */
extern ShadowSize gShadowSizes[16];         /* 0x801346C0 */
extern s32 gP2Joined;                       /* 0x8004637C ovl3 */
extern s32 gRacersEnabled;
extern RenderObj gRenderObjs[];             /* 0x8013FA08 */
extern s32 gCarRideHeight[];                /* 0x80131820 */

extern u32 *gDlCursor;                      /* 0x80147684 */
void animCancelAll(void);                   /* 0x80113528 */
void animBlinkInit(void);                   /* 0x80113CA0 */
s32  animAddEvent(s32 ramp, AnimDoneFunc func, f32 at, u8 flags); /* 0x80112B54 */
s32  gfxWaitIdle(void);
void skyDraw(s32 player, s32 yaw, s32 roll); /* 0x80101A68 */
void cameraApply(s32 player, s32 x, s32 y, s32 z, s32 showOwn, s32 showOther); /* 0x801005BC */
void p2JoinPressed(void);                   /* 0x80025F10 ovl3 */
void p2JoinFree(void);                      /* 0x80025FD4 ovl3 */
void drawDLSimple(void *dl, s32 x, s32 y);  /* 0x80026534 ovl3 */
void saveMenuInit(s32 players);             /* 0x80035760 ovl3 */
void trackPrepare(void);
void trackStart(void);
void trackUnload(void);
void playerSetupView(s32 player);
void carSetPalette(s32 slot, s32 car, s32 paint);
void viewBegin(s32 racer);
void bgDrawSetupFull(void);
s32  viewYawTo(s32 player, s32 x, s32 z);
s32  randChance(f32 p);
s32  randRange(s32 lo, s32 hi);
s32  sfxState(s32 id, s32 player);
void textFreeAll(void);
void xformsUpdate(Xform *xf);
void func_8002F600_ovl2(s32 arg);           /* RACE_START_MODE */

void carSelectFrame(s32 arg);
void csFrame(void);
void csCarsReset(void);
void csCarDrop(s32 p, s32 i);
void csPlayerReset(s32 p);
void csStep(s32 i);
s32  csStepDone(s32 slot, s32 b, f32 c);
s32  csDriveOffDone(s32 slot, s32 b, f32 c);
s32  csSwoopDone(s32 slot, s32 b, f32 c);
s32  csHalfway(s32 slot, s32 b, f32 c);
s32  csShake2(s32 slot, s32 b, f32 c);
s32  csShake3(s32 slot, s32 player, f32 c);
s32  csShake4(s32 slot, s32 b, f32 c);
void csTurntable(s32 p, s32 i, s32 angle, s32 y);
void csDraw(void);
s32  csApproach(s32 *v, s32 target, f32 rate, f32 step);

#define CS_XF(p, i) gCsXf[(p) * 13 + (i)]
#define DL_PUSH(w0, w1) { u32 *_g = gDlCursor; gDlCursor = _g + 2; _g[0] = (w0); _g[1] = (u32)(w1); }

/* 0x8002B690 ovl3 - enter CHOOSE CAR */
void carSelectInit(s32 arg) {
    s32 p;

    gCsHeapMark = lowMemOverlayLoad();
    assetLoadGroup(0x2BFA, gCsDLs);
#ifdef PORT
    {   /* the ENTHUSIAST cheat: the four cars' details with their real names - copies of
         * the meshes (gCsDLs[11-14]) the port adds to ASSETS.BIN after the ROM's 12518
         * files: 4 name pictures, then the 4 meshes (tools/port_realnames.py) */
        extern s32 gRealNames;
        s32 i;
        void *a;
        if (gRealNames) {
            for (i = 0; i < 4; i++) {
                a = assetLoad(12518 + 4 + i);
                if (a != NULL) {
                    gCsDLs[11 + i] = a;
                }
            }
        }
    }
#endif
    lowMemOverlayRelease();
    for (p = 0; p < 35; p++) {
        textPlace(&gCsXf[p], 0, 0, 0, 1);
    }
    for (p = 0; p < 2; p++) {
        gCsLevel[p][0] = gPlayerSetup[p].carAwards[0];
        gCsLevel[p][1] = gPlayerSetup[p].carAwards[1];
        gCsLevel[p][2] = gPlayerSetup[p].carAwards[2];
        gCsLevel[p][3] = gPlayerSetup[p].carAwards[3];
        gCsLevel[p][4] = gPlayerSetup[p].carAwards[4];
        gCsLevel[p][5] = gPlayerSetup[p].carAwards[5];
        gCsLevel[p][6] = gPlayerSetup[p].carAwards[6];
        xformScriptRun(gCsXfScriptPlayer, &CS_XF(p, 0));
        contPressed(p, BTN_ALL);
    }
    xformScriptRun(gCsXfScript, gCsXf);
    gCsSlope = ((f32)tblSin(1000) / 16384.0) / ((f32)tblCos(1000) / 16384.0);
    csCarsReset();
    csPlayerReset(0);
    if (gNumPlayersM1 != 0) {
        csPlayerReset(1);
    }
    gCsTitle = textCreate(2, "CHOOSE CAR", 2);
    textPlace(&gCsTitle->xf, 0x8B0000, 0, 0, 0);
    csStep(0);
    gModeFunc = carSelectFrame;
    carSelectFrame(0);
}

/* 0x8002B8D8 ovl3 - the four cars back on their turntables, cameras reset */
void csCarsReset(void) {
    s32 p, i;
    ViewerCam *cam;

    for (p = 0; p < 2; p++) {
        gCsSkyOn[p] = 0;
        gCsPlayerState[p] = 2;
        gCsCar[p] = 2;
        gCsSecret[p] = 0;
        gCsShake[p] = 0;
        gPlayerSetup[p].unk008 = gCsLevel[p][2];
        cam = &gViewerCams[p];
        cam->useAngles = 1;
        cam->pos[0] = 0;
        cam->angles[0] = 0;
        cam->angles[1] = 0;
        cam->angles[2] = 0;
        cam->pos[1] = 0x780000;
        gCsSwoop[0].t = 0;                       /* (fixed addresses, not per player, in the original) */
        gCsSwoop[1].t = 0;
        CS_XF(p, 0).x = -gCsCarX[2];
        CS_XF(p, 12).visible = 0;
        cam->pos[2] = gCsCamZ[gNumPlayersM1];
        for (i = 0; i < 4; i++) {
            CS_XF(p, 7 + i).y = 0x100000;
            CS_XF(p, 3 + i).y = 0;
            gCsCars[p][i] = &gCsCarObjs[p][i];
            renderObjInit(&gCsCarObjs[p][i], gCsModels[0][i], p, 0x12, 0, gCsCarX[i] - gCsCarX[2], 0, 0);
            csCarDrop(p, i);
            gCsCars[p][i]->shadowDrop = gCsCars[p][i]->y;
            gCsCars[p][i]->angleZ = 0x8000;
        }
    }
    gCsTrackReady = 0;
}

/* 0x8002BBBC ovl3 - a car sits on its turntable */
void csCarDrop(s32 p, s32 i) {
    RenderObj *obj = gCsCars[p][i];
    s32 y = CS_XF(p, 7 + i).y - gCarModelInfo[obj->model].bodyHeight;

    if (y < 0) {
        y = 0;
    }
    obj->y = y;
}

/* 0x8002BC3C ovl3 - a player starts choosing (the transmission first) */
void csPlayerReset(s32 p) {
    gCsPlayerState[p] = 0;
    gPlayerSetup[p].manual = p;
    gCsXf[31 + p].visible = 1;
    gCsXf[33 + p].visible = 0;
}

/* 0x8002BC98 ovl3 - play a step of the screen's animation chain */
void csStep(s32 i) {
    CsStep *s = &gCsSteps[i];

    gCsStepKind = s->kind;
    if (s->kind == 2 || s->kind == 5) {
        sfxPlay(0x29, -1);
    }
    gAnimSlots[animStart(s->seconds, s->script, NULL, csStepDone, 0)].user = i;
}

/* 0x8002BD3C ovl3 - a step finished: on to the next */
s32 csStepDone(s32 slot, s32 b, f32 c) {
    s32 i = gAnimSlots[slot].user;
    CsStep *s;

    if (gCsStepKind == 0) {
        sfxPlay(0x4E, -1);
        contPressed(0, BTN_ALL);
        contPressed(1, BTN_ALL);
    }
    s = &gCsSteps[i];
    gCsStepKind = s->nextKind;
    if (s->next >= 0) {
        csStep(s->next);
    }
    return 2;
}

/* 0x8002BDE8 ovl3 - CHOOSE CAR, every frame (the options screen can open while choosing) */
void carSelectFrame(s32 arg) {
    if (gCsPlayerState[0] >= 3 || (gNumPlayersM1 != 0 && gCsPlayerState[1] >= 3)
        || optionsCheck(carSelectFrame, 1) == 0) {
        csFrame();
    }
}

/* 0x8002BE5C ovl3 */
void csFrame(void) {
    s32 p, i, prev, sel, held, car, lvl, old, most;
    u32 pressed;
    PlayerSetup *ps;
    RenderObj *obj, *race;
    ViewerCam *cam;
    CsSwoop *sw;
    Xform *xf;
    f32 t, slope;
    s32 dx, dz, y, x;

    /* player 2 presses START: a two-player game (with save devices: the save screen first) */
    if (gNumPlayersM1 == 0 && gCsPlayerState[0] < 3 && gP2Joined == 0 && P2_ALLOWED
        && (contPressed(1, BTN_START) & BTN_START)) {
        p2JoinPressed();
        if (gContReadState != 0) {
            p2JoinFree();
            sfxStop(0x97, 0);                    /* (the original passes a register it never set) */
            sfxStop(0x14, -1);
            sfxStop(0x42, 0);
            sfxStop(0x29, -1);
            textFree(gCsTitle);
            lowMemOverlayLoad();
            highHeapRestore(gCsHeapMark);
            lowMemOverlayRelease();
            trackUnload();
            animCancelAll();
            animBlinkInit();
            textFreeAll();
            gNumPlayersM1 = 1;
            saveMenuInit(2);
            return;
        }
    }
    if (gP2Joined != 0) {
        if (gCsStepKind == 0 || gCsStepKind == 1) {
            gP2Joined = 0;
            csPlayerReset(1);
            gNumPlayersM1 = 1;
        } else if (gCsStepKind == 8 && gCsPlayerState[0] == 2) {
            gP2Joined = 0;
            csPlayerReset(0);
            csPlayerReset(1);
            gCsXf[9].visible = 0;                /* (0x8004740C) */
            csStep(4);
        }
    }

    if (gCsStepKind == 4) {
        sfxStop(0x29, -1);
        sfxPlay(0x4E, -1);
        gNumPlayersM1 = 1;
        gCsStepKind = 1;
        for (p = 0; p < 2; p++) {
            contPressed(p, BTN_ALL);
        }
    }

    /* the transmission */
    if (gCsStepKind == 1) {
        for (p = 0; p <= gNumPlayersM1; p++) {
            ps = &gPlayerSetup[p];
            if (gCsPlayerState[p] == 0) {
                old = ps->manual;
                pressed = contPressed(p, BTN_LEFT | BTN_RIGHT);
                if ((pressed & BTN_RIGHT) && ps->manual == 0) {
                    ps->manual = 1;
                }
                if ((pressed & BTN_LEFT) && ps->manual != 0) {
                    ps->manual = 0;
                }
                if (old != ps->manual) {
                    sfxPlay(0x97, p);
                }
                gCsXf[33 + p].visible = 1;
                if (contPressed(p, BTN_A | BTN_START | BTN_Z) & (BTN_A | BTN_START | BTN_Z)) {
                    sfxPlay(0x81, p);
                    gCsPlayerState[p] = 1;
                    gCsXf[31 + p].visible = 0;
                    if (gNumPlayersM1 == 0 || gCsPlayerState[p == 0] == 1) {
                        contPressed(0, BTN_ALL);
                        if (gNumPlayersM1 != 0) {
                            p2JoinFree();
                            contPressed(1, BTN_ALL);
                        }
                        gCsXf[33].visible = 0;
                        gCsXf[34].visible = 0;
                        csCarsReset();
                        if (gNumPlayersM1 == 0 || gPlayerSetup[0].manual == gPlayerSetup[1].manual) {
                            csStep(ps->manual + 1);
                        } else {
                            csStep(3);
                        }
                    }
                }
            }
            csApproach(&gCsXf[31 + p].x, ps->manual != 0 ? 0x3BA240 : -0x3C61A8, 0.22f, 196608.0f);
            t = (f32)(gCsXf[31 + p].x + 0x3C61A8) / 7865320.0f;
            gCsXf[33 + p].x = (s32)(t * 7.995392e6f + 3538944.0f + (f32)(p * 0x300000));
        }
    }

    /* cars appearing: they follow their turntables down */
    if (gCsStepKind == 6 || gCsStepKind == 7 || gCsStepKind == 5) {
        for (p = 0; p <= gNumPlayersM1; p++) {
            gCsCars[p][0]->z = CS_XF(p, 0).z + 0x140000;
            gCsCars[p][1]->z = CS_XF(p, 0).z + 0x140000;
            gCsCars[p][2]->z = CS_XF(p, 0).z + 0x140000;
            gCsCars[p][3]->z = CS_XF(p, 0).z + 0x140000;
        }
        if (gCsStepKind == 7) {
            contPressed(0, BTN_ALL);
            contPressed(1, BTN_ALL);
            sfxStop(0x29, -1);
            sfxPlay(0x14, -1);
            gCsStepKind = 8;
        }
    }
    if (gCsStepKind == 3) {
        if (gNumPlayersM1 == 0 || gPlayerSetup[0].manual == gPlayerSetup[1].manual) {
            csStep(gPlayerSetup[0].manual + 7);
        } else {
            csStep(9);
        }
    }

    /* choosing the car */
    gCsTitle->xf.visible = 0;
    if (gCsStepKind == 8) {
        if (gCsPlayerState[0] == 2 || (gNumPlayersM1 != 0 && gCsPlayerState[1] == 2)) {
            gCsTitle->xf.y = (gNumPlayersM1 != 0 ? 0x630000 : 0xBD0000) + 0xB0000;
            gCsTitle->xf.visible = 1;
        }
        for (p = 0; p <= gNumPlayersM1; p++) {
            i = 0;
            if (gCsPlayerState[p] == 2 || gCsPlayerState[p] == 3) {
                xf = &CS_XF(p, 0);
                if (gCsPlayerState[p] == 2) {
                    ps = &gPlayerSetup[p];
                    prev = gCsCar[p];
                    pressed = contPressed(p, 0xC30F00);
                    if ((pressed & 0x800100) && gCsCar[p] < 3) {
                        gCsCar[p]++;
                    }
                    sel = gCsCar[p];
                    if ((pressed & 0x400200) && sel > 0) {
                        gCsCar[p] = --sel;
                    }
                    if (prev != sel) {
                        sfxStop(0x42, p);
                        sfxPlay(0x42, p);
                        sel = gCsCar[p];
                    }
                    /* the secret: C-up, C-left and C-down held (on the first three cars) */
                    gCsSecret[p] = 0;
                    if (sel < 3 && (gPads[p].held & 0x403F) == 0xE) {
                        gCsSecret[p] = 1;
                    }
#ifdef PORT
                    {   /* the BARNFIND cheat: the Jeep, School Bus and Police Car instead, without holding anything */
                        extern s32 gSecretCars;
                        if (gSecretCars && sel < 3) {
                            gCsSecret[p] = 1;
                        }
                    }
#endif
                    if (prev != sel) {
                        gCsTurn[p][prev].angle = gCsCars[p][prev]->angleZ;
                        gCsTurn[p][prev].y = CS_XF(p, 7 + prev).y;
                    }
                    car = gCsModels[gCsSecret[p]][sel];
                    lvl = gCsLevel[p][car];
                    old = lvl;
                    most = ps->carAwards[car];
#ifdef PORT
                    {   /* the HOTRODS cheat: every faster car (the most a car can earn), not only those won */
                        extern s32 gHotRods, gCarAwardMax[5];
                        if (gHotRods) {
                            most = gCarAwardMax[4];
                        }
                    }
#endif
                    if ((pressed & 0x10800) && old < most) {
                        gCsLevel[p][car] = lvl = old + 1;
                    }
                    if ((pressed & 0x20400) && old > 0) {
                        gCsLevel[p][car] = lvl = lvl - 1;
                    }
                    if (old != lvl) {
                        sfxPlay(0x2C, p);
                        lvl = gCsLevel[p][car];
                    }
                    ps->unk008 = lvl;
                    if (contPressed(p, BTN_A | BTN_START | BTN_Z) & (BTN_A | BTN_START | BTN_Z)) {
                        playerSetupView(p);
                        sfxPlay(0x3E, p);
                        gCsPlayerState[p] = 3;
                        ps->car = car;
                        gCsSwoop[p].angle = gCsCar[p] < 2 ? 0x8000 : -0x8000;
                        gRenderObjs[p].model = ps->car;
                        p2JoinFree();
                        if (gCsTrackReady == 0) {
                            gCsTrackReady = 1;
                            trackPrepare();
                            trackStart();
                            for (i = 0; i <= gNumPlayersM1; i++) {
                                gCsStartZ0[i] = gRenderObjs[i].z + 0x05BC0000;
                                gCsStartZ[i] = gRenderObjs[i].z;
                                gCsStartAY[i] = gRenderObjs[i].angleY;
                                gCsStartAX[i] = gRenderObjs[i].angleX;
                            }
                            i = 0;
                        }
                    }
                }
                /* the chosen turntable rises; once up (after choosing) the car drives off */
                if (csApproach(&xf->x, -gCsCarX[gCsCar[p]], 0.22f, 196608.0f) != 0) {
                    CS_XF(p, 12).visible = 1;
                    if (gCsPlayerState[p] == 3 && CS_XF(p, 7 + gCsCar[p]).y == 0x3C0000) {
                        ps = &gPlayerSetup[p];
                        cam = &gViewerCams[p];
                        sfxPlay(0x80, p);
                        gCsPlayerState[p] = 4;
                        sel = gCsCar[p];
                        /* the car becomes the player's race car object */
                        {
                            u8 *from = (u8 *)gCsCars[p][sel];
                            u8 *to = (u8 *)ps->pad034;
                            for (i = 0; i < (s32)sizeof(RenderObj); i++) {
                                to[i] = from[i];
                            }
                            i = 0;
                        }
                        x = gCsCarX[sel];
                        gCsCars[p][sel] = (RenderObj *)ps->pad034;
                        CS_XF(p, 12).visible = 0;
                        xf->x = 0x5A0000 - x;
                        xf->z = 0x012C0000;
                        cam->pos[0] = 0x5A0000;
                        cam->target[0] = 0x5A0000;
                        cam->useAngles = 0;
                        cam->pos[2] = gCsCamZ[gNumPlayersM1] + 0x044C0000;
                        gCsCars[p][0]->x = (gCsCarX[0] - x) + 0x5A0000;
                        gCsCars[p][0]->z = 0x01400000;
                        gCsCars[p][1]->x = (gCsCarX[1] - gCsCarX[gCsCar[p]]) + 0x5A0000;
                        gCsCars[p][1]->z = 0x01400000;
                        gCsCars[p][2]->x = (gCsCarX[2] - gCsCarX[gCsCar[p]]) + 0x5A0000;
                        gCsCars[p][2]->z = 0x01400000;
                        gCsCars[p][3]->x = (gCsCarX[3] - gCsCarX[gCsCar[p]]) + 0x5A0000;
                        gCsCars[p][3]->z = 0x01400000;
                        {
                            AnimSlot *a = &gAnimSlots[animStart(3.0f, gCsDriveOff[p][gCsCar[p]], NULL, csDriveOffDone, 0)];
                            a->user = p;
                            gAnimSlots[animAddEvent(a->ramp, csHalfway, 0.5f, 1)].user = p;
                        }
                        sel = gCsCar[p];
                        gCsTurn[p][sel].angle = gCsCars[p][sel]->angleZ;
                        i = 0;
                        gCsTurn[p][sel].y = CS_XF(p, 7 + sel).y;
                    }
                } else {
                    CS_XF(p, 12).visible = 0;
                }
            }

            /* each turntable: the chosen one up and turning, the others down and straightening */
            for (i = 0; i < 4; i++) {
                if (i == gCsCar[p]) {
                    gCsCars[p][i]->model = gCsModels[gCsSecret[p]][i];
                } else {
                    gCsCars[p][i]->model = gCsModels[0][i];
#ifdef PORT
                    {
                        extern s32 gSecretCars;
                        if (gSecretCars && i < 3) {
                            gCsCars[p][i]->model = gCsModels[1][i];   /* (BARNFIND: on every turntable) */
                        }
                    }
#endif
                }
                sel = gCsCar[p];
                if (i != sel || gCsPlayerState[p] == 2 || gCsPlayerState[p] == 3) {
                    csApproach(&CS_XF(p, 7 + i).y, i == sel ? 0x3C0000 : 0x100000, 0.04f, 104857.0f);
                    if (i == gCsCar[p]) {
                        gCsCars[p][i]->angleZ += 500;
                    } else {
                        csTurntable(p, i, 0x8000, 0x100000);
                    }
                }
                if (gCsPlayerState[p] == 2 || gCsPlayerState[p] == 3) {
                    gCsCars[p][i]->x = CS_XF(p, 0).x + gCsCarX[i];
                }
                csCarDrop(p, i);
                gCsCars[p][i]->shadowDrop = gCsCars[p][i]->y;
            }

            /* driving off: the camera swoops round behind the car to the start line */
            if (gCsPlayerState[p] == 4 || gCsPlayerState[p] == 5) {
                sw = &gCsSwoop[p];
                cam = &gViewerCams[p];
                race = &gRenderObjs[p];
                ps = &gPlayerSetup[p];
                obj = gCsCars[p][gCsCar[p]];
                t = (f32)((f32)sw->t / 10000000.0);
                if (gCsPlayerState[p] == 5) {
                    obj->y = (s32)((f32)race->y * t);
                    race->z = (s32)((f32)(gCsStartZ[p] - gCsStartZ0[p]) * t + (f32)gCsStartZ0[p]);
                    race->angleY = (s32)((f32)gCsStartAY[p] * t);
                    obj->angleY = race->angleY;
                    race->angleX = (s32)((f32)gCsStartAX[p] * t);
                    obj->angleX = race->angleX;
                    dx = gCsShake[p];
                    if (dx != 0) {
                        dz = 0;
                    } else if (randChance(0.1f) != 0) {
                        dx = randRange(-200, 200);
                        dz = randRange(-200, 200);
                    } else {
                        dx = 0;
                        dz = 0;
                    }
                    race->angleYAdd = dx;
                    obj->angleYAdd = dx;
                    race->angleXAdd = dz;
                    obj->angleXAdd = dz;
                }
                cam->pos[0] = 0x5A0000 - tblSin(sw->angle) * sw->dist * 4;
                cam->target[2] = obj->z;
                cam->pos[2] = gCsCamZ[gNumPlayersM1] + sw->dist * tblCos(sw->angle) * 4 + 0x01400000;
                y = race->y + (gNumPlayersM1 != 0 ? 0x442041 : 0x500000) + gCarRideHeight[ps->car] - 0x780000;
                cam->pos[1] = (s32)((f32)y * t + (f32)0x780000);
                sel = gCsCar[p];
                obj = gCsCars[p][sel];
                y = gShadowSizes[obj->model].unk08 + obj->y;
                cam->target[1] = (s32)((f32)((s32)((f32)(cam->pos[2] - obj->z) * gCsSlope * (f32)gNumPlayersM1 + (f32)cam->pos[1]) - y) * t + (f32)y);
                if ((sel < 2 && sw->angle < 0x4000) || (sel >= 2 && sw->angle >= -0x3FFF)) {
                    CS_XF(p, 7).visible = 0;
                    CS_XF(p, 8).visible = 0;
                    CS_XF(p, 9).visible = 0;
                    CS_XF(p, 10).visible = 0;
                }
                csTurntable(p, sel, 0, 0x260000);
            }
        }
    }

    /* at the start line: the camera settles behind the car */
    for (p = 0; p <= gNumPlayersM1; p++) {
        if (gCsPlayerState[p] == 6) {
            race = &gRenderObjs[p];
            gCsCars[p][gCsCar[p]]->y = race->y;
            race->z = gCsStartZ[p];
            race->angleY = gCsStartAY[p];
            sw = &gCsSwoop[p];
            cam = &gViewerCams[p];
            ps = &gPlayerSetup[p];
            race->angleX = gCsStartAX[p];
            cam->pos[0] = 0x5A0000 - tblSin(sw->angle) * sw->dist * 4;
            cam->pos[1] = race->y + (gNumPlayersM1 != 0 ? 0x442041 : 0x500000) + gCarRideHeight[ps->car];
            cam->angles[0] = 0;
            cam->angles[2] = 0;
            cam->pos[2] = gCsCamZ[gNumPlayersM1] + sw->dist * tblCos(sw->angle) * 4 + 0x01400000;
            cam->angles[1] = gNumPlayersM1 != 0 ? 1000 : 0;
            cam->useAngles = 1;
        }
    }

    /* everyone at the start line: the race */
    if (gCsPlayerState[0] == 6 && (gNumPlayersM1 == 0 || gCsPlayerState[1] == 6)) {
        if (gRacersEnabled != 0) {
            sfxPlay(0x7D, -1);
        }
        if (gNumPlayersM1 != 0) {
            for (p = 0; p < 2; p++) {
                gPlayerSetup[p].cameraFollowsCar = 2;
                playerSetupView(p);
            }
        }
        gfxWaitIdle();
        for (p = 0; p <= gNumPlayersM1; p++) {
            obj = gCsCars[p][gCsCar[p]];
            gRenderObjs[p].wheelSpin[0] = obj->wheelSpin[0];
            gRenderObjs[p].wheelSpin[1] = obj->wheelSpin[1];
            carSetPalette(p, obj->model, gPlayerSetup[p].unk008);
        }
        textFree(gCsTitle);
        lowMemOverlayLoad();
        highHeapRestore(gCsHeapMark);
        lowMemOverlayRelease();
        gModeArg = 0;
        gOverlayRequest = OVERLAY_REQUEST(OVL_RACE);
        gModeFunc = func_8002F600_ovl2;
    }
    csDraw();
}

/* 0x8002D5E4 ovl3 - the car has driven off: the camera swoop */
s32 csDriveOffDone(s32 slot, s32 b, f32 c) {
    s32 p = gAnimSlots[slot].user;

    gAnimSlots[animStart(2.0f, gNumPlayersM1 != 0 ? gCsSwoop2P : gCsSwoop1P, (void *)(p * sizeof(CsSwoop)),
                         csSwoopDone, 0)].user = p;
    return 2;
}

/* 0x8002D684 ovl3 - the camera is behind the car on the start line */
s32 csSwoopDone(s32 slot, s32 b, f32 c) {
    s32 p = gAnimSlots[slot].user;

    gCsPlayerState[p] = 6;
    gRenderObjs[p].angleYAdd = 0;
    gCsCars[p][gCsCar[p]]->angleYAdd = 0;
    gRenderObjs[p].angleXAdd = 0;
    gCsCars[p][gCsCar[p]]->angleXAdd = 0;
    return 2;
}

/* 0x8002D730 ovl3 - halfway off the turntable: the engine, the race view, the start-line rumble */
s32 csHalfway(s32 slot, s32 b, f32 c) {
    s32 p = gAnimSlots[slot].user;

    animStart(3.5f, gCsCamDrop, (void *)(p * sizeof(PlayerSetup)), NULL, 4);
    sfxPlay(0x56, p);
    if (gRacersEnabled != 0 && sfxState(0x7D, -1) != 3) {
        sfxPlay(0x7D, -1);
    }
    gCsPlayerState[p] = 5;
    gCsSkyOn[p] = 1;
    gAnimSlots[animStart(0.1f, gCsShakeA, (void *)(p * 4), csShake2, 0)].user = p;
    return 1;
}

/* 0x8002D848 ovl3 */
s32 csShake2(s32 slot, s32 b, f32 c) {
    s32 p = gAnimSlots[slot].user;

    animStart(1.0f, gCsShakeB, (void *)(p * 4), NULL, 4);
    animWait(2.3f, (void *)p, csShake3);
    return 2;
}

/* 0x8002D8C4 ovl3 */
s32 csShake3(s32 slot, s32 player, f32 c) {
    gAnimSlots[animStart(0.1f, gCsShakeC, (void *)(player * 4), csShake4, 0)].user = player;
    return (s32)0xBF800000;                      /* (-1.0f's bits, as the original returns) */
}

/* 0x8002D930 ovl3 */
s32 csShake4(s32 slot, s32 b, f32 c) {
    animStart(0.5f, gCsShakeD, (void *)(gAnimSlots[slot].user * 4), NULL, 4);
    return 2;
}

/* 0x8002D990 ovl3 - a turntable eases back to 'angle' as it sinks to 'y' */
void csTurntable(s32 p, s32 i, s32 angle, s32 y) {
    CsTurn *tn = &gCsTurn[p][i];
    s32 cur = CS_XF(p, 7 + i).y;
    s32 from = tn->angle & 0xFFFF;
    s32 a, d;
    f32 f;

    if (y < cur) {
        a = angle & 0xFFFF;
        d = a - from;
        f = (f32)(y - cur) / (f32)(tn->y - y);
        if (d & 0x8000) {
            a = (s32)((f32)a - f * (f32)((from - a) & 0xFFFF));
        } else {
            a = (s32)((f32)(d & 0xFFFF) * f - (f32)a);
        }
    } else {
        a = angle & 0xFFFF;
    }
    gCsCars[p][i]->angleZ = a;
}

/* 0x8002DA98 ovl3 - draw the screen */
void csDraw(void) {
    s32 kind = gCsStepKind;
    s32 p, i, v, a, shade;
    PlayerSetup *ps;
    Xform *xf;
    f32 f1, f2, n, l1, l2;
    f64 d;
    u32 c;

    if (kind < 9) {
        screenClear(-1, 1);
    }
    if (kind >= 5) {
        for (p = 0; p <= gNumPlayersM1; p++) {
            xf = &CS_XF(p, 0);
            a = gCsSwoop[p].angle;
            if (gCsPlayerState[p] >= 3 && (a < -0x4000 || a >= 0x4001)) {
                xf[1].visible = 1;
            } else {
                xf[1].visible = 0;
            }
            /* the race track behind, once the camera has turned round to it */
            if (gCsPlayerState[p] >= 3) {
                a = gCsSwoop[p].angle;
                if ((gCsCar[p] < 2 && a < -0x3A98) || (gCsCar[p] >= 2 && a >= 0x3A99)) {
                    ps = &gPlayerSetup[p];
                    skyDraw(p, viewYawTo(p, *(s32 *)&ps->pad034[0x40 - 0x34], *(s32 *)&ps->pad034[0x48 - 0x34]) + 0x14000, 0);
                    viewBegin(p);
                    cameraApply(p, 0x5A0000 - gRenderObjs[p].x, 0, 0xFCF40000, 0, gCsSkyOn[p == 0]);
                } else {
                    viewBegin(p);
                }
            } else {
                viewBegin(p);
            }
            a = gCsSwoop[p].angle;
            if (gCsPlayerState[p] >= 3 && a >= -0x3FFF && a < 0x4000) {
                xf[11].visible = 1;
            } else {
                xf[11].visible = 0;
            }
            xformsUpdate(xf);
            drawDLScript(gCsDrawCars, xf, gCsDLs, 0x7FFFFFFF);
            /* the turntables' lights: the chosen one bright */
            for (i = 0; i < 4; i++) {
                DL_PUSH(0xE7000000, 0);
                shade = (3 - i == gCsCar[p]) ? 0xA0 : 0x2A;
                DL_PUSH(0xFA000000, (shade << 24) | (shade << 16) | (shade << 8) | 0xFF);
                DL_PUSH(0x06000000, gCsDLs[5 + i]);
            }
            DL_PUSH(0xE7000000, 0);
            DL_PUSH(0xFA000000, 0xFFFFFFFF);
            DL_PUSH(0xBD000000, 0);
            for (i = 0; i < 4; i++) {
                if (i == gCsCar[p]) {
                    v = gCsLevel[p][gCsSecret[p] * 4 + i];
                } else {
                    v = gCsLevel[p][i];
                }
                carSetPalette(p, gCsCars[p][i]->model, v);
                if (i == gCsCar[p]) {
                    gCsCars[p][i]->flags &= ~RO_DIM;
                } else {
                    gCsCars[p][i]->flags |= RO_DIM;
                }
                renderObjDraw(gCsCars[p][i]);
            }
            if (gNumPlayersM1 != 0) {
                bgDrawSetup(p, 1);
                drawDL(gCsDLs[54 + p], 0x010B0000, 0x580000, 0, 0x10000);
                bgDrawSetup(-1, 1);
            } else {
                bgDrawSetup(0, 1);
                if (xf[12].visible != 0) {
                    /* the car's name, its level and its stats */
                    drawDLXform(gCsDLs[10], &xf[12]);
                    drawDLXform(gCsDLs[11 + gCsSecret[p] * 4 + gCsCar[p]], &xf[12]);
                    drawDLXform(gCsDLs[18 + gPlayerSetup[p].unk008 * 7 + gCsCar[p] + gCsSecret[p] * 4], &xf[12]);
                }
            }
            if (gCsPlayerState[0] < 3 && (gNumPlayersM1 == 0 || gCsPlayerState[1] < 3)) {
                drawDLSimple(gCsDLs[56], 0, 0);
            }
        }
    }

    /* AUTOMATIC / MANUAL */
    if (kind < 7) {
        bgDrawSetup(-1, 0);
        xformsUpdate(&gCsXf[26]);
        if (gCsStepKind == 1 || gCsStepKind == 2 || gCsStepKind == 6) {
            l1 = (f32)(gCsXf[32].x + 0x3C61A8) / 7865320.0f;
            n = (f32)gNumPlayersM1;
            l2 = (f32)(gCsXf[31].x + 0x3C61A8) / 7865320.0f;
            d = (1.0f - l2) + (1.0f - l1) * n;
            f1 = d < 1.0 ? (f32)d : 1.0f;
            d = n * l1 + l2;
            f2 = d < 1.0 ? (f32)d : 1.0f;
        } else {
            f1 = 1.0f;
            v = gNumPlayersM1 != 0;
            if (v == 0) {
                v = gCsStepKind >= 5;
            }
            f2 = (f32)v;
        }
        DL_PUSH(0xE7000000, 0);
        c = (u32)(f1 * 155.0f + 100.0f) & 0xFF;
        DL_PUSH(0xFA000000, (c << 24) | (c << 16) | (((u32)(f1 * 0.0f + 255.0f) & 0xFF) << 8) | 0xFF);
        drawDLXform(gCsDLs[50], &gCsXf[27]);
        DL_PUSH(0xE7000000, 0);
        c = (u32)(f2 * 155.0f + 100.0f) & 0xFF;
        DL_PUSH(0xFA000000, (c << 24) | (c << 16) | (((u32)(f2 * 0.0f + 255.0f) & 0xFF) << 8) | 0xFF);
        drawDLXform(gCsDLs[51], &gCsXf[28]);
        DL_PUSH(0xE7000000, 0);
        DL_PUSH(0xFA000000, 0xFFFFFFFF);
        drawDLScript(gCsDrawTitle, gCsXf, gCsDLs, 0x7FFFFFFF);
        DL_PUSH(0xBD000000, 0);
        bgDrawSetupFull();
        drawDLScript(gCsDrawFrame, gCsXf, gCsDLs, 0x7FFFFFFF);
        bgDrawSetup(-1, 1);
        if (gNumPlayersM1 != 0) {
            for (p = 0; p <= gNumPlayersM1; p++) {
                if (gCsXf[33 + p].visible != 0) {
                    if (gCsPlayerState[p] == 1) {
                        DL_PUSH(0xE7000000, 0);
                        DL_PUSH(0xFA000000, 0x646464C8);
                    }
                    drawDLXform(gCsDLs[54 + p], &gCsXf[33 + p]);
                    if (gCsPlayerState[p] == 1) {
                        DL_PUSH(0xE7000000, 0);
                        DL_PUSH(0xFA000000, 0xFFFFFFFF);
                    }
                }
            }
        }
        drawDLSimple(gCsDLs[56], 0, 0);
    }
}

/* 0x8002E690 ovl3 - move *v towards target by rate of the distance plus step; 1 when there */
s32 csApproach(s32 *v, s32 target, f32 rate, f32 step) {
    s32 d = (s32)((f32)(target - *v) * rate);

    if (d < 0) {
        *v = (s32)((f32)*v + ((f32)d - step));
        if (target >= *v) {
            *v = target;
            return 1;
        }
        return 0;
    }
    *v = (s32)((f32)*v + ((f32)d + step));
    if (*v >= target) {
        *v = target;
        return 1;
    }
    return 0;
}
