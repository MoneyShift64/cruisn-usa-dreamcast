/*
 * frontend/title.c - entering the front end, the title screen (spinning car
 * + logo, "PRESS START"), the attract-mode rotation, and the
 * "PLAYER 2 PRESS START" / "2 PLAYER CHALLENGE!" prompt shared by the menus.
 * Original code: overlay 3, ROM 0x5EA10-0x5F400 (RAM 0x80025C80-0x80026670).
 */
#include "common.h"
#include "frontend.h"

/* ---- "PLAYER 2 PRESS START" prompt ---- */
typedef struct BlinkStep {               /* 8 bytes */
    /* 0x00 */ AnimCmd *script;
    /* 0x04 */ f32 seconds;
} BlinkStep;
extern s32 gP2JoinState;                 /* 0x80046370 ovl3 - 0 prompt shown, 1 joining, 2 "2 PLAYER CHALLENGE!", 3 none */
extern TextObj *gP2JoinText;             /* 0x80046374 ovl3 */
extern s32 gP2JoinRamp;                  /* 0x80046378 ovl3 - running animation, -1 = none */
extern s32 gP2Joined;                    /* 0x8004637C ovl3 - player 2 pressed START on this screen */
extern AnimCmd gP2FlashScript[];             /* 0x8003FD40 ovl3 */
extern BlinkStep gP2BlinkSteps[5];       /* 0x8003FE08 ovl3 - flashes faster and faster */
extern AnimCmd gP2JoinScript[];              /* 0x8003FE30 ovl3 */

/* ---- title screen ---- */
typedef struct TitleCar {                /* a 3D model object (0x2F0 bytes, like RenderObj) */
    /* 0x000 */ s32 model;
    /* 0x004 */ u8  pad004[0x14];
    /* 0x018 */ s32 spin;                /* binary angle; +0x100 per frame */
    /* 0x01C */ u8  pad01C[0x18];
    /* 0x034 */ s32 unk34[2];
    /* 0x03C */ s32 tilt;                /* from player 1's stick */
    /* 0x040 */ u8  pad040[0x2F0 - 0x40];
} TitleCar;

typedef struct CameraDef {               /* 0x28 bytes, 0x8014EF90 (two, one per player) */
    /* 0x00 */ s32 x;
    /* 0x04 */ s32 y;
    /* 0x08 */ s32 z;
    /* 0x0C */ s32 unk0C;
    /* 0x10 */ s32 unk10;
    /* 0x14 */ s32 unk14;
    /* 0x18 */ s32 rotX;
    /* 0x1C */ s32 rotY;
    /* 0x20 */ s32 rotZ;
    /* 0x24 */ s32 unk24;
} CameraDef;
extern CameraDef gViewerCams[2];         /* 0x8014EF90 */

typedef struct AttractPair { s32 track[2]; } AttractPair;
extern void *gTitleDLs[4];               /* 0x80046380 ovl3 - logo display lists */
extern Xform gTitleXf[2];                /* 0x80046390 ovl3 */
extern u32  gTitleHeapMark;              /* 0x800463D0 ovl3 */
extern s32  gTitleNoTimeout;             /* 0x800463D4 ovl3 - never set: debug "stay on the title" */
extern TextObj *gPressStartText;         /* 0x800463D8 ovl3 */
extern TitleCar gTitleCar;               /* 0x800463E0 ovl3 */
extern s32  gTitleVariant;               /* 0x800466D0 ovl3 - which car spins on the title (0-3) */
extern s32  gAttractIndex;               /* 0x800466D4 ovl3 - next attract demo (0-7) */
extern AnimCmd gTitleLogoScript[];          /* 0x8003FE98 ovl3 */
extern AttractPair gAttractTracks[8];    /* 0x8003FE9C ovl3 - the attract demo's two tracks */
extern AttractPair gAttractArg;          /* 0x8003FEDC ovl3 - (followed by -1) */
extern s32  gTitleCarModels[4];          /* 0x8003FEE8 ovl3 */
extern XformCmd gTitleXfScript[];            /* 0x8003FE60 ovl3 */

void carPalettesReset(void);             /* 0x80117EBC */
void titleSetCar(s32 n);
void titleFree(s32 keepText);
void drawDLSimple(void *dl, s32 x, s32 y);
s32  p2JoinBlinkDone(s32 slot, s32 b, f32 c);
s32  p2JoinShowChallenge(s32 slot, s32 b, f32 c);

/* 0x80025C80 ovl3 - show "PLAYER 2 PRESS START" (1-player game only) */
void p2JoinInit(void) {
    TextObj *t;

    gP2Joined = 0;
    gP2JoinRamp = -1;
    if (gNumPlayersM1 != 0) {
        gP2JoinText = NULL;
        gP2JoinState = 3;
        return;
    }
    if (!P2_ALLOWED) {                   /* (port: 2-player games off) */
        gP2JoinText = NULL;
        gP2JoinState = 3;
        return;
    }
    gP2JoinState = 0;
    t = textCreate(0, "PLAYER 2 PRESS START", 2);
    gP2JoinText = t;
    textPlace(&t->xf, 0xAC0000, 0xD0000, 0, 1);
    gP2JoinText->blink = gBlinkSlow;
    gP2JoinText->blinkLevel = 0.75f;
    gP2JoinText->color = 0x9632FFFF;
    gP2JoinText->flags = 1;
}

/* 0x80025D5C ovl3 - anim callback: flash "2 PLAYER CHALLENGE!" 4 more times, faster each time */
s32 p2JoinBlinkDone(s32 slot, s32 b, f32 c) {
    BlinkStep *step;
    s32 n = gAnimSlots[slot].user + 1;
    s32 s;

    if (n < 5) {
        step = &gP2BlinkSteps[n];
        s = animStart(step->seconds, step->script, &gP2JoinText->xf, p2JoinBlinkDone, 0);
        gAnimSlots[s].user = n;
        gP2JoinRamp = gAnimSlots[s].ramp;
    } else {
        gP2JoinRamp = -1;
        gP2JoinState = 3;
    }
    return 2;
}

/* 0x80025E24 ovl3 - anim callback: replace the prompt with "2 PLAYER CHALLENGE!" */
s32 p2JoinShowChallenge(s32 slot, s32 b, f32 c) {
    TextObj *t;
    s32 s;

    textFree(gP2JoinText);
    t = textCreate(0, "2 PLAYER CHALLENGE!", 2);
    gP2JoinText = t;
    textPlace(&t->xf, 0xA50000, 0xD0000, 0, 1);
    gP2JoinText->color = 0xC82828FF;
    gP2JoinText->flags = 1;
    gP2JoinState = 2;
    s = animStart(0.15f, gP2FlashScript, &gP2JoinText->xf, p2JoinBlinkDone, 0);
    gAnimSlots[s].user = 0;
    gP2JoinRamp = gAnimSlots[s].ramp;
    return 2;
}

/* 0x80025F10 ovl3 - player 2 pressed START: becomes a 2-player game */
void p2JoinPressed(void) {
    s32 s;

    if (gP2JoinState != 0) {
        return;
    }
    sfxStop(0x14, -1);
    sfxPlay(0x9D, -1);
    sfxPlay(0x9D, -1);
    playerSetupReset(1);
    gP2Joined = 1;
    gP2JoinText->blink = -1;
    gP2JoinState = 1;
    s = animStart(0.15f, gP2JoinScript, &gP2JoinText->xf, p2JoinShowChallenge, 0);
    gP2JoinRamp = gAnimSlots[s].ramp;
}

/* 0x80025FD4 ovl3 - remove the prompt */
void p2JoinFree(void) {
    if (gP2JoinRamp >= 0) {
        animCancel(gP2JoinRamp);
        gP2JoinRamp = -1;
    }
    if (gP2JoinText != NULL) {
        textFree(gP2JoinText);
        gP2JoinText = NULL;
    }
}

/* 0x80026030 ovl3 - enter the front end (from boot, a race, or the high-score table).
 * showIntro: play the intro sequence (creditsInit) before the title. */
void frontEndEnter(s32 showIntro) {
    TextObj *t;

    gAttractIndex = 0;
    gTitleVariant = 0;
    carPalettesReset();
    gameSetupReset();
    t = textCreate(5, "PRESS START", 2);
    gPressStartText = t;
    textPlace(&t->xf, 0x8B0000, 0xE0000, 0, 1);
    gPressStartText->blink = gBlinkMedium;
    gPressStartText->blinkLevel = 0.5f;
    if (showIntro) {
        gModeFunc = creditsInit;
        gModeArg = 0;
        creditsInit(0);
    } else {
        titleInit(0);
    }
}

/* 0x80026108 ovl3 - title screen: load the logo, set up the spinning car */
void titleInit(s32 skip) {
    Xform *xf;

    gTitleHeapMark = lowMemOverlayLoad();
    assetLoadGroup(0x2D18, gTitleDLs);
    lowMemOverlayRelease();
    for (xf = gTitleXf; xf != &gTitleXf[2]; xf++) {
        textPlace(xf, 0, 0, 0, 0);
    }
    renderObjInit(&gTitleCar, 0, 0, 0x100, 0, 0, 0, 0);
    titleSetCar(gTitleVariant);
    VI_SET_DEFAULT_MODE();
    gModeFunc = titleFrame;
    gModeArg = skip;
    titleFrame(skip);
}

/* 0x8002621C ovl3 - title screen, every frame.
 * START (or skip != 0) -> save-game screen if there is a save device, else Choose Race.
 * After ~8 s: every 4th time the high-score table, otherwise an attract demo. */
void titleFrame(s32 skip) {
    AttractPair *pair;
    f64 stick;
    s32 n;

    if (skip != 0 || (contPressed(0, BTN_START) & BTN_START)) {
        VI_SET_DEFAULT_MODE();
        titleFree(0);
        playerSetupReset(0);
        musicPlay(0x9F);
        p2JoinInit();
        if (gContReadState != 0) {
            saveMenuInit(1);
        } else {
            gModeFunc = raceSelectInit;
            gModeArg = 0;
            raceSelectInit(0);
        }
        return;
    }
    if (optionsCheck(titleFrame, 1)) {
        return;
    }
    if (gTitleNoTimeout == 0) {
        gTitleCar.spin += 0x100;
        if (gTitleCar.spin > 0xFA00) {
            titleFree(1);
            contPressed(0, BTN_ALL);
            n = gTitleVariant++;
            if (n == 3) {
                VI_SET_DEFAULT_MODE();
                gModeFunc = hiscoreInit;
                gModeArg = 1;
                hiscoreInit(1);
            } else {
                VI_SET_DEFAULT_MODE();
                pair = &gAttractTracks[gAttractIndex++];
                gAttractArg = *pair;
                if (gAttractIndex >= 8) {
                    gAttractIndex = 0;
                }
                gModeFunc = (ModeFunc)attractInit;
                gModeArg = (s32)&gAttractArg;
                attractInit((s32 *)&gAttractArg);
            }
            return;
        }
    }

    /* draw: logo, the spinning car (stick left/right tilts it), PRESS START */
    screenClear(-1, 1);
    bgDrawSetup(-1, 1);
    drawDLScript(gTitleLogoScript, gTitleXf, gTitleDLs, 0x7FFFFFFF);
    viewBegin(0);
    stick = gPads[0].xRaw;
    gTitleCar.tilt = (1.0 - (stick + stick)) * 6000.0;
    renderObjDraw(&gTitleCar);
    bgDrawSetup(-1, 1);
    drawDLSimple(gTitleDLs[2], 0, 0);
    gTitleCar.unk34[0] = gTitleCar.unk34[0];   /* (no-op copy in the original) */
    gTitleCar.unk34[1] = gTitleCar.unk34[1];
}

/* 0x80026534 ovl3 - draw a display list at (x, y), or at (32, 6) when both are 0 */
void drawDLSimple(void *dl, s32 x, s32 y) {
    if (x == 0 && y == 0) {
        drawDL(dl, 0x200000, 0x60000, 0, 0x10000);
    } else {
        drawDL(dl, x, y, 0, 0x10000);
    }
}

/* 0x80026584 ovl3 - choose the title car (0-3) and place logo + camera */
void titleSetCar(s32 n) {
    CameraDef *cam;

    if (n >= 4) {
        gTitleVariant = 0;
    } else {
        gTitleVariant = n;
    }
    gTitleCar.spin = 0x100;
    gTitleCar.model = gTitleCarModels[gTitleVariant];
    xformScriptRun(gTitleXfScript, gTitleXf);
    cam = &gViewerCams[0];
    cam->unk24 = 0;
    cam->x = 0;
    cam->y = 0x820000;
    cam->z = 0x12C0000;
    cam->rotX = 0;
    cam->rotY = 0x240000;
    cam->rotZ = 0;
}

/* 0x8002662C ovl3 - leave the title: free its assets (and "PRESS START" unless keepText) */
void titleFree(s32 keepText) {
    if (keepText == 0) {
        textFree(gPressStartText);
    }
    lowMemOverlayLoad();
    highHeapRestore(gTitleHeapMark);
    lowMemOverlayRelease();
}
