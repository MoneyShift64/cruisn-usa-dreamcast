/*
 * race/hud.c - the race HUD and race flow around it: time left, checkpoints and
 * extended time, split times against the records, the announcer's callouts,
 * the countdown, and the end of the race.
 * Original code: race overlay, ROM 0x42D90-0x47DD0 (RAM 0x8002A320-0x8002EB60).
 */
#include "common.h"
#include "race.h"
#include "road.h"
#include "frontend.h"

/* HUD state (race overlay bss) */
extern s32      gCountdownPhase;         /* 0x800468A8 ovl2 */
extern s32      gHudStartArg;            /* 0x800468AC ovl2 */
extern s32      gHudUnk8B0;              /* 0x800468B0 ovl2 */
extern s32      gHudUnk8B4;              /* 0x800468B4 ovl2 */
extern s32      gHudUnk8B8;              /* 0x800468B8 ovl2 */
extern s32      gHudSlideA;              /* 0x80046898 ovl2 - 16.16 x positions of sliding HUD parts */
extern s32      gHudSlideB;              /* 0x8004689C ovl2 */
extern s32      gHudSlideC;              /* 0x800468A0 ovl2 */
extern s32      gHudUnk890[2];           /* 0x80046890 ovl2 */
extern TextObj *gHudTrackName;           /* 0x800468C4 ovl2 */
extern TextObj *gHudTimeLeft;            /* 0x800468CC ovl2 - big "time left" readout */
extern void    *gHudDLs[];               /* 0x800468D0 ovl2 - HUD graphics (group 0x31A, 0x31B in 2-player) */
extern TextObj *gHudTimeNum[2];          /* 0x80046988 ovl2 - per player: seconds left (2 digits) */
extern TextObj *gHudSpeed[2];            /* 0x80046990 ovl2 */
extern TextObj *gHudSplit[2];            /* 0x80046998 ovl2 - split time against the record */
extern TextObj *gHudLapTime[2];          /* 0x800469A0 ovl2 */
extern TextObj *gHudWinner;              /* 0x800469A8 ovl2 - "FIRST PLACE" / "WINNER" */
extern s32      gBoostTimer[2];           /* 0x800469C8 ovl2 - boost frames left (gBoostTimer) */
extern s32      gBoosts[2];              /* 0x800469D0 ovl2 - boosts the player can use (button: pad bit 2) */
extern s32      gCheckpoints[2];         /* 0x800469D8 ovl2 - checkpoints passed, per player */
extern u8      *gHudPalA[2];             /* 0x800469E0 ovl2 */
extern u8      *gHudPalB[2];             /* 0x800469E8 ovl2 */
extern u8      *gHudAssetA;              /* 0x800469F0 ovl2 */
extern u8      *gHudAssetB;              /* 0x800469F4 ovl2 */
extern s32      gHudUnkA00;              /* 0x80046A00 ovl2 */
extern s32      gHudUnkA04;              /* 0x80046A04 ovl2 */
extern s32      gCalloutQuiet;           /* 0x80046A1C ovl2 - frames before the next speed callout */
extern s32      gCalloutTimer[2];        /* 0x80046A28 ovl2 - pending callout wait, -1 = none */
extern s32      gHudAnimA30[2];          /* 0x80046A30 ovl2 */
extern s32      gHudLightX[2][4];        /* 0x80046A38 ovl2 - the 4 indicator lights (anim targets) */
extern s32      gHudLightAnim[2][4];     /* 0x80046A58 ovl2 - their running animations, -1 = none */

/* Speech played at most once per race: {sound, played}, ended by -1 (0x8004283C ovl2) */
typedef struct OnceSfx { s32 sfx; s32 played; } OnceSfx;
extern OnceSfx  gOnceSfx[];              /* 0x8004283C ovl2 */

typedef struct DifficultyDef {           /* 12 bytes, 0x80133B20 */
    /* 0x00 */ f32 startTime;            /* seconds on the clock at the start */
    /* 0x04 */ f32 extendTime;           /* added at every checkpoint */
    /* 0x08 */ s32 ai;
} DifficultyDef;
extern DifficultyDef gDifficultyDefs[];  /* 0x80133B20 */
extern s32 gSplitRecords[][14][10];      /* 0x80150A00 - best checkpoint times (1/100 s), per save device */
extern f32 gRaceTime;                    /* 0x80150948 */
extern char *gRaceNames[];               /* 0x80133D54 */
extern s32 gRaceLoser;                   /* 0x80150958 - player in front at the finish (-1 = out of time); results "LOST BY" is the other one */
extern f32 gLostBy;                      /* 0x80150950 */
extern AnimCmd gLightOnScript[];             /* 0x80042774 ovl2 */
extern AnimCmd gLightsOffScript[];           /* 0x8004279C ovl2 */
extern AnimCmd gTrackNameScript[];           /* 0x80042620 ovl2 */
extern AnimCmd gFinishScript[];              /* 0x800426C0 ovl2 */
extern AnimCmd gFinishPlayerScript[];        /* 0x80042670 ovl2 */
extern s32 gWinnerY[2];                  /* 0x80042898 ovl2 - by player count */
extern f64 gTrackNameY;                  /* 0x800458E0 ovl2 */
extern f32 gLightTime;                   /* 0x800458D4 ovl2 */
extern f32 gLightTime2;                  /* 0x800458D8 ovl2 */
typedef struct PadHold { u8 pad00[0x10]; s32 held; u8 pad14[4]; } PadHold;
extern PadHold gPadHold[];               /* 0x8014E378 - 0x18 bytes per pad */

TextObj *textCreateNum(s32 layer, s32 value, s32 digits, s32 max, s32 style); /* 0x8011A1A4 */
TextObj *textCreateTime(s32 layer, s32 value, s32 a, s32 max, s32 b); /* 0x8011A1E8 */
u8  *assetFindLoaded(s32 id);            /* 0x80000994 - address of an asset already in memory, or 0 */
s32  randRange(s32 lo, s32 hi);          /* 0x80103188 */
s32  randChance(f32 p);                  /* 0x801031DC */
void resetCleanup(void);                 /* 0x80107CF8 */
s32  animAddEvent(s32 slot, AnimDoneFunc func, f32 at, s32 flags); /* 0x80112B74 */

f32  splitTimeRound(f32 t);              /* 0x8002D518 ovl2 */
void hudReset(void);                     /* 0x8002B308 ovl2 */
void hudFree(void);                      /* 0x8002B4F8 ovl2 */
void hudFinish(void);                    /* 0x8002D098 ovl2 */
void hudPopupsInit(void);                /* 0x8002E13C ovl2 */
void hudPopup(s32 player, s32 kind, s32 variant); /* 0x8002E1B4 ovl2 */
void radioOff(void);                /* 0x8002E8A8 ovl2 */
s32  hudFinishDone(s32 slot, s32 b, f32 c); /* 0x8002C888 ovl2 */
s32  hudTrackNameDone(s32 slot, s32 b, f32 c); /* 0x8002C7E8 ovl2 */

s32  hudLightDone(s32 slot, s32 b, f32 c);
f32  hudCallout2(s32 slot, s32 player);
f32  hudCallout3(s32 slot, s32 player);
f32  playerSpeedMph(s32 player);

/* 0x8002A320 ovl2 - race start: create the HUD text objects and load the HUD graphics */
void raceStartCommon(void) {
    PlayerSetup *ps;
    OnceSfx *o;
    s32 p;

    gHudTimeLeft = textCreateTime(3, 0, 0, 0x927BF, 0);
    textPlace(&gHudTimeLeft->xf, 0x3E80000, 0x3E80000, 0, 1);
    gHudTimeLeft->flags |= 1;
    gHudTimeLeft->color = 0x4BFF4BFF;
    for (p = 0, ps = gPlayerSetup; p <= gNumPlayersM1; p++, ps++) {
        ps->speedo = 0.0f;
        ps->tacho = 0;
        ps->gear = 1;
        ps->odometer = 0.0f;
        ps->distance = 0.0f;
        gBoostTimer[p] = 0;
        gBoosts[p] = 0;
        gHudSpeed[p] = textCreateNum(3, 0, 3, 0x20, 0);
        gHudTimeNum[p] = textCreateNum(4, (s32)gDifficultyDefs[gDifficulty].startTime, 2, 0x30, 2);
        gHudSplit[p] = textCreateTime(3, 0, 0, 0x927BF, 2);
        gHudLapTime[p] = textCreateTime(3, 0, 0, 0x927BF, 0);
        gHudSpeed[p]->viewer = p;
        gHudTimeNum[p]->viewer = -1;
        gHudSplit[p]->viewer = p;
        gHudLapTime[p]->viewer = p;
        gHudLightX[p][0] = ((s8)ps->unk01A <= 0) ? 0x550000 : 0;
        gHudLightAnim[p][0] = -1;
        gHudLightX[p][1] = ((s8)ps->unk01A - 1 <= 0) ? 0x550000 : 0;
        gHudLightAnim[p][1] = -1;
        gHudLightX[p][2] = ((s8)ps->unk01A - 2 <= 0) ? 0x550000 : 0;
        gHudLightAnim[p][2] = -1;
        gHudLightX[p][3] = 0;
        gHudLightAnim[p][3] = -1;
    }
    for (o = gOnceSfx; o->sfx != -1; o++) {
        o->played = 0;
    }
    hudReset();
    hudPopupsInit();
    gCountdownPhase = 0;
    lowMemOverlayLoad();
    assetLoadGroup((gNumPlayersM1 != 0) ? 0x31B : 0x31A, gHudDLs);
    {
        u8 *a = assetFindLoaded(0x2EA);
        gHudPalA[0] = a + 0x800001D8;
        gHudPalA[1] = a + 0x800001D8 + 0x200;
        gHudPalB[0] = a + 0x8000000A;
        gHudPalB[1] = a + 0x8000000A + 0x200;
    }
    lowMemOverlayRelease();
}

/* 0x8002A778 ovl2 - a player passed a checkpoint: extended time (for the leader in
 * 2-player games), and the split time against the best for this checkpoint */
void hudCheckpoint(s32 player, s32 arg1) {
    PlayerSetup *ps = &gPlayerSetup[player];
    s32 n;
    s32 *rec[2];
    s32 *best;
    s32 i;
    s32 t100;
    f32 t;

    if (ps->unk328 != 0 && ps->unk008 == 3) {
        gBoosts[player]++;
    }
    n = gCheckpoints[player]++;
    if (gNumPlayersM1 == 0 || n >= gCheckpoints[player == 0]) {
        gRaceTimeLeft += gDifficultyDefs[gDifficulty].extendTime;
        sfxPlay(0x99, player);
        sfxPlay(0x99, player);
    }
    for (i = 0; i < 2; i++) {
        rec[i] = &gSplitRecords[i][gTrackChoice][n];
    }
    best = rec[0];
    if (*rec[1] != -1 && (*rec[0] == -1 || *rec[1] < *rec[0])) {
        best = rec[1];
    }
    t = splitTimeRound(gRaceTime);
    if (*best != -1) {
        ps->unk020 = *best - (s32)(t * 100.0f);
    }
    t100 = (s32)(t * 100.0f);
    for (i = 0; i < 2; i++) {
        if (*rec[i] == -1 || t100 < *rec[i]) {
            *rec[i] = t100;
        }
    }
}

/* 0x8002A9A4 ovl2 - play an announcer line; lines in gOnceSfx only the first time */
void hudSpeak(s32 player, s32 sfx) {
    OnceSfx *o;

    for (o = gOnceSfx; o->sfx != -1; o++) {
        if (sfx == o->sfx) {
            if (o->played != 0) {
                return;
            }
            o->played++;
            break;
        }
    }
    sfxPlay(sfx, player);
}

/* 0x8002AA18 ovl2 - one of the player's three indicator lights goes out (all relight after the last) */
void hudLightOut(s32 player) {
    PlayerSetup *ps = &gPlayerSetup[player];
    s8 *n = (s8 *)&ps->unk01A;
    s32 *anim;
    s32 i;
    s32 slot;

    (*n)--;
    if (*n < 0) {
        *n = 3;
        anim = gHudLightAnim[player];
        for (i = 0; i < 4; i++) {
            if (anim[i] >= 0) {
                *n = 0;
                break;
            }
        }
        if (i == 4) {
            slot = animStart(gLightTime, gLightsOffScript, gHudLightX[player], hudLightDone, 0);
            anim[*n] = slot;
            gAnimSlots[slot].user = *n * 2 + player;
        }
    } else if (gHudLightAnim[player][*n] < 0) {
        slot = animStart(gLightTime2, gLightOnScript, &gHudLightX[player][*n], hudLightDone, 0);
        gHudLightAnim[player][*n] = slot;
        gAnimSlots[slot].user = *n * 2 + player;
    }
}

/* 0x8002ABC8 ovl2 */
s32 hudLightDone(s32 slot, s32 b, f32 c) {
    s32 u = gAnimSlots[slot].user;

    gHudLightAnim[u & 1][u >> 1] = -1;
    return 2;
}

/* 0x8002AC18 ovl2 - announcer callout for an event (kind 0: going fast) */
void cameraStartEvent(s32 player, s32 kind) {
    s32 *t;
    s32 v;

    if (gPlayerSetup[player].viewAtEdge == 0) {          /* +0x394: announcer on for this player */
        return;
    }
    if (kind == 0) {
        t = &gCalloutTimer[player];
        if (playerSpeedMph(player) < 40.0f) {
            return;
        }
        if (*t < 0 && randChance(0.5f)) {
            *t = animWait((f32)randRange(3, 4), (void *)player, (AnimDoneFunc)hudCallout2);
        }
        v = 1;
    } else {
        v = randRange(2, 5);
    }
    hudPopup(player, kind, v);
}

/* 0x8002AD14 ovl2 - a few seconds later: a second callout if still fast */
f32 hudCallout2(s32 slot, s32 player) {
    if (gPlayerSetup[player].viewAtEdge != 0 && playerSpeedMph(player) >= 40.0f) {
        gCalloutQuiet = 900;
        hudPopup(player, 0, randRange(2, 3));
    }
    if (randChance(0.5f)) {
        gCalloutTimer[player] = animWait((f32)randRange(5, 6), (void *)player, (AnimDoneFunc)hudCallout3);
    } else {
        gCalloutTimer[player] = -1;
    }
    return -1.0f;
}

/* 0x8002AE14 ovl2 - and a third */
f32 hudCallout3(s32 slot, s32 player) {
    if (gPlayerSetup[player].viewAtEdge != 0 && playerSpeedMph(player) >= 40.0f) {
        gCalloutQuiet = 900;
        hudPopup(player, 0, randRange(15, 15));
    }
    gCalloutTimer[player] = -1;
    return -1.0f;
}

/* 0x8002AEC8 ovl2 - the speedometer reading in mph */
f32 playerSpeedMph(s32 player) {
    f32 k = gUnitsKph ? 0.6215f : 1.0f;

    return k * gPlayerSetup[player].speedo;
}

/* 0x8002AF24 ovl2 - the race begins (after the camera fly-in): show the track name */
void raceHudInit(s32 arg) {
    u8 *a;
    s32 p;

    lowMemOverlayLoad();
    gHudAssetB = NULL;
    gHudAssetA = NULL;
    a = assetFindLoaded(0x109C);
    if (a != NULL) {
        gHudAssetA = a + 0x80000008;
    }
    a = assetFindLoaded(0xD9C);
    if (a != NULL) {
        gHudAssetB = a + 0x80000008;
    }
    lowMemOverlayRelease();
    gHudStartArg = arg;
    gHudUnk8B0 = 0;
    gCountdownPhase = 1;
    gCalloutQuiet = 900;
    hudReset();
    for (p = 0; p <= gNumPlayersM1; p++) {
        contPressed(p, BTN_ALL);
        gPadHold[p].held = 0;
    }
    gHudUnk890[1] = 0;
    gHudUnk890[0] = 0;
    gCheckpoints[1] = 0;
    gCheckpoints[0] = 0;
    gHudTrackName = textCreate(0, gRaceNames[gTrackChoice], 2);
    textPlace(&gHudTrackName->xf, 0, (s32)gTrackNameY, 0, 1);
    animAddEvent(animStart(1.0f, gTrackNameScript, &gHudTrackName->xf, NULL, 4), hudTrackNameDone, 0.6f, 1);
}

/* 0x8002B0E4 ovl2 - show or hide the HUD; hiding it at the end of a race shows the result */
void raceSetHudVisible(s32 visible) {
    PlayerSetup *ps;
    s32 p;
    s32 w;

    radioOff();
    if (visible) {
        gHudSlideA = 0x550000;
        hudFree();
        return;
    }
    hudFinish();
    resetCleanup();
    sfxPlay(0x20, -1);
    gCountdownPhase = 6;
    w = gRaceWinner;
    if (w >= 0 || (gNumPlayersM1 != 0 && gRaceLoser >= 0)) {
        gHudWinner = textCreate(2, (w >= 0) ? "FIRST PLACE" : "WINNER", 2);
        textPlace(&gHudWinner->xf, 0x8B0000, gWinnerY[gNumPlayersM1], 0, 1);
        gHudWinner->viewer = gRaceLoser;
        if (gRaceWinner >= 0) {
            gHudWinner->blink = gBlinkSlow;
            gHudWinner->blinkLevel = 0.7f;
        } else {
            gHudWinner->blink = gBlinkMedium;
            gHudWinner->blinkLevel = 0.7f;
        }
        w = gRaceWinner;
    }
    if (w < 0) {
        musicPlay(0xA2);
    }
    animStart(1.5f, gFinishScript, NULL, hudFinishDone, 0);
    for (p = 0, ps = gPlayerSetup; p <= gNumPlayersM1; p++, ps++) {
        if (((u8 *)ps)[0x1C] & 1) {            /* (top byte of unk01C) */
            animStart(1.5f, gFinishPlayerScript, (void *)(p * 4), NULL, 4);
            ps->viewAtEdge = 0;
        }
    }
}

/* 0x8002B308 ovl2 - reset the clock and the per-player HUD */
void hudReset(void) {
    PlayerSetup *ps;
    s32 p;

    gHudSlideC = 0xAE60000;
    gHudSlideB = 0xAE60000;
    gHudSlideA = 0xAE60000;
    gHudUnk8B4 = 0;
    gHudUnk8B8 = 0;
    gRaceTime = 0.0f;
    gRaceTimeLeft = gDifficultyDefs[gDifficulty].startTime;
    gHudUnkA00 = 0;
    gHudUnkA04 = 3;
    for (p = 0, ps = gPlayerSetup; p <= gNumPlayersM1; p++, ps++) {
        ps->unk020 = 0x927C0;
        gHudAnimA30[p] = -1;
        textPlace(&gHudSpeed[p]->xf, 0x3E80000, 0x3E80000, 0, 1);
        textPlace(&gHudTimeNum[p]->xf, 0x3E80000, 0x3E80000, 0, 1);
        textPlace(&gHudSplit[p]->xf, 0x3E80000, 0x3E80000, 0, 0);
        textPlace(&gHudLapTime[p]->xf, 0x3E80000, 0x3E80000, 0, gNumPlayersM1);
        gHudSplit[p]->blinkLevel = 0.7f;
    }
}

/* 0x8002B4F8 ovl2 - free the HUD */
void hudFree(void) {
    s32 p;
    s32 i;

    textFree(gHudTimeLeft);
    for (p = 0; p <= gNumPlayersM1; p++) {
        contPressed(p, BTN_ALL);
        textFree(gHudSpeed[p]);
        textFree(gHudTimeNum[p]);
        textFree(gHudSplit[p]);
        textFree(gHudLapTime[p]);
        for (i = 0; i < 4; i++) {
            if (gHudLightAnim[p][i] >= 0) {
                animCancel(gHudLightAnim[p][i]);
                gHudLightAnim[p][i] = -1;
            }
        }
        if (gHudAnimA30[p] >= 0) {
            animCancel(gAnimSlots[gHudAnimA30[p]].ramp);
            gHudAnimA30[p] = -1;
        }
        if (gCalloutTimer[p] >= 0) {
            animCancel(gAnimSlots[gCalloutTimer[p]].ramp);
        }
    }
    gCountdownPhase = 0;
}

extern s32      gHudSlideD;              /* 0x800468A4 ovl2 */
extern u32      gLightsButton[2];        /* 0x8014E360 - per player: configured button C */
extern u32      gViewButton[2];          /* 0x8014E370 - per player: button that changes the view */
extern u32      gViCount;                /* 0x80147694 - retrace counter */
extern f32      gTimeStep;               /* 0x800458FC ovl2 - 1/30 s */
extern f64      gTimeRoundUp;            /* 0x80045900 ovl2 - 0.99 */
extern f32      gCalloutChance;          /* 0x800458F4 ovl2 - 0.6 */
extern f32      gCalloutSwapChance;      /* 0x800458F8 ovl2 - 0.3 */
extern u8       gCarAnim[];              /* 0x80043A50 ovl2, 0xF8 per car model: f32 base engine power at +0 */
typedef struct RenderObjHdr { u8 pad00[8]; u16 flags; u8 pad0A[0x2F0 - 0xA]; } RenderObjHdr;
extern RenderObjHdr gRenderObjHdr[];     /* 0x8013FA08 */

void hudScreenFade(s32 level);           /* 0x801166BC */
void textSetNumber(TextObj *t, s32 value, s32 digits, s32 max, s32 style); /* 0x8011A368 */
void hudDrawPlace(s32 place, s32 x, s32 y); /* 0x8011A69C */
void cameraViewStep(s32 player);           /* 0x80034A94 ovl2 */
void hudDrawRaceTime(s32 x, s32 y);      /* 0x8002D560 ovl2 */
void hudDrawSpeedo(s32 player, s32 x, s32 y); /* 0x8002D664 ovl2 */
void hudDrawTacho(s32 player, s32 x, s32 y); /* 0x8002D77C ovl2 */
void hudDrawGauges2P(s32 player, s32 x, s32 y); /* 0x8002D8D8 ovl2 */
void hudDrawTimeLeft(s32 player, s32 x, s32 y); /* 0x8002DCB8 ovl2 */
void hudDrawSplit2P(s32 player, s32 x, s32 y); /* 0x8002DD98 ovl2 */
void hudDrawTime2P(s32 player, s32 x, s32 y, s32 slide); /* 0x8002DDE8 ovl2 */
void hudSplitUpdate(s32 player);         /* 0x8002DE80 ovl2 */
void hudDrawMap(s32 player, s32 x, s32 y); /* 0x8002C248 ovl2 */
void hudDrawStartLights(s32 x, s32 y);   /* 0x8002E0BC ovl2 */
void hudDrawFinish(s32 x, s32 y);        /* 0x8002CF88 ovl2 */
void hudRadioButton(void);              /* 0x8002E920 ovl2 */
void hudChangeView(s32 player);          /* 0x8002CFFC ovl2 */
void hudPopupsDraw(void);              /* 0x8002E2F4 ovl2 */
void hudPopupsMove(void);              /* 0x8002E5CC ovl2 */

#define LIGHTS(p, i) gHudLightX[p][i]

/* 0x8002B6B8 ovl2 - once per frame: boosts, the clock, player buttons, and drawing the HUD */
void raceHudDraw(void) {
    PlayerSetup *ps;
    Car *car;
    s32 phase;
    s32 p;
    s32 a;
    s32 b;
    s32 m;
    u32 r;
    s32 secs;

    if (gCountdownPhase == 0) {
        return;
    }
    /* flashing parts of two HUD textures (palette entries) */
    if (gHudAssetB != NULL) {
        a = 4;
        b = 2;
        if (gViCount & 0x10) {
            a = 2;
            b = 4;
        }
        ((u16 *)gHudAssetB)[a] = 0xF6C5;
        ((u16 *)gHudAssetB)[b] = 1;
    }
    if (gHudAssetA != NULL) {
        *(u16 *)(gHudAssetA + 0x188) = 0x5801;
        *(u16 *)(gHudAssetA + 0x1C6) = 0x7001;
        *(u16 *)(gHudAssetA + 0x1D2) = 0x7AC1;
        *(u16 *)(gHudAssetA + 0x1D6) = 0x8301;
        *(u16 *)(gHudAssetA + 0x6) = 0x397;
        *(u16 *)(gHudAssetA + 0xA) = 0x3D9;
        r = gViCount % 105;
        if (r < 60) {
            *(u16 *)(gHudAssetA + 0x6) = 0x2FA9;
            *(u16 *)(gHudAssetA + 0xA) = 0x4D75;
        } else if (r < 75) {
            *(u16 *)(gHudAssetA + 0x1D2) = 0xF503;
            *(u16 *)(gHudAssetA + 0x1D6) = 0xFE4F;
        } else {
            *(u16 *)(gHudAssetA + 0x188) = 0xE001;
            *(u16 *)(gHudAssetA + 0x1C6) = 0xF085;
        }
    }

    phase = gCountdownPhase;
    if (phase == 5) {
        /* racing: a boost doubles engine power for 2 seconds */
        for (p = 0; p <= gNumPlayersM1; p++) {
            if (gBoosts[p] != 0) {
                if (gPadHold[p].held & 2) {
                    car = &gCars[p];
                    car->enginePower = 2.0f * *(f32 *)&gCarAnim[car->model * 0xF8];
                    gRenderObjHdr[p].flags |= 0x2000;
                    gBoostTimer[p] = 60;
                    gBoosts[p]--;
                    sfxPlay(0x54, p);
                    sfxPlay(0x54, p);
                    gPadHold[p].held &= ~2;
                }
            } else {
                gPadHold[p].held &= ~2;
            }
        }
        for (p = 0; p <= gNumPlayersM1; p++) {
            if (gBoostTimer[p] > 0 && --gBoostTimer[p] == 0) {
                car = &gCars[p];
                gRenderObjHdr[p].flags &= ~0x2000;
                car->enginePower = *(f32 *)&gCarAnim[car->model * 0xF8];
            }
        }
    } else {
        for (p = 0; p <= gNumPlayersM1; p++) {
            gBoostTimer[p] = 0;
            gBoosts[p] = 0;
            gPadHold[p].held &= ~2;
            gRenderObjHdr[p].flags &= ~0x2000;
        }
    }
    if (phase == 4 || phase == 5) {
        for (p = 0; p <= gNumPlayersM1; p++) {
            if (gLightsButton[p] & contPressed(p, gLightsButton[p])) {
                hudLightOut(p);
            }
        }
        phase = gCountdownPhase;
    }
    if (phase > 0 && phase < 6) {
        for (p = 0; p <= gNumPlayersM1; p++) {
            if ((gViewButton[p] & contPressed(p, gViewButton[p])) && gPlayerSetup[p].viewAtEdge == 0) {
                hudChangeView(p);
            }
        }
        phase = gCountdownPhase;
    }
    if (phase == 5 && (gTrackChoice == 9 || gTrackChoice == 11)) {
        /* Iowa and Indiana: now and then the announcer comments */
        if (--gCalloutQuiet == 0) {
            gCalloutQuiet = 900;
            if (randChance(gCalloutChance)) {
                p = 0;
                if (gNumPlayersM1 != 0) {
                    p = gPlayerSetup[1].place < gPlayerSetup[0].place;
                    if (randChance(gCalloutSwapChance)) {
                        p = p == 0;
                    }
                }
                cameraStartEvent(p, 0);
            }
        }
        hudPopupsDraw();
        hudPopupsMove();
    }
    if (gHudStartArg != 0) {
        hudScreenFade(gHudUnk8B0);
        gHudUnk8B0 += 1000;
        if (gHudUnk8B0 >= 0x10000) {
            gHudStartArg = 0;
        }
    }
    if (gCountdownPhase == 4 || gCountdownPhase == 5) {
        /* the clock: 1/30 s per frame; "10 seconds" warning and 3-2-1 beeps */
        gRaceTimeLeft -= gTimeStep;
        gRaceTime += gTimeStep;
        secs = (s32)((f64)gRaceTimeLeft + gTimeRoundUp);
        if (secs >= 11) {
            gHudUnkA00 = 0;
        } else if (secs == 10 && gHudUnkA00 == 0) {
            gHudUnkA00 = 1;
            sfxPlay(0x40, -1);
        }
        if (secs == gHudUnkA04) {
            gHudUnkA04--;
            if (secs > 0) {
                sfxPlay(0, -1);
            }
        } else if (secs >= 4) {
            gHudUnkA04 = 3;
        }
        if (gRaceTimeLeft <= 0.0f) {
            gRaceTimeLeft = 0.0f;
        }
    }
    textSetNumber(gHudSpeed[0], (s32)gPlayerSetup[0].speedo, 3, 0x20, 0);
    if (gNumPlayersM1 != 0) {
        textSetNumber(gHudSpeed[1], (s32)gPlayerSetup[1].speedo, 3, 0x20, 0);
        bgDrawSetup(-1, 1);
        m = (LIGHTS(0, 1) < LIGHTS(1, 1)) ? LIGHTS(0, 1) : LIGHTS(1, 1);
        hudDrawRaceTime((-gHudSlideB - m) + 0x30000, 0x6D0000);
        m = (LIGHTS(0, 1) < LIGHTS(1, 1)) ? LIGHTS(0, 1) : LIGHTS(1, 1);
        hudDrawTime2P(0, m + gHudSlideB + 0xD30000, 0x6D0000, m);
        cameraViewStep(0);
        hudSplitUpdate(0);
        bgDrawSetup(0, 1);
        hudDrawGauges2P(0, LIGHTS(0, 1) + gHudSlideA + 0x1170000, 0);
        if (gPlayerSetup[0].place > 0 && gPlayerSetup[0].place < 11) {
            hudDrawPlace(gPlayerSetup[0].place, LIGHTS(0, 0) + gHudSlideA + 0xF40000, 0x3F0000);
        }
        drawDL(gHudDLs[41], gHudSlideA * gHudStartArg + 0x10B0000, 0x580000, 0, 0x10000);
        hudDrawSplit2P(0, (-gHudSlideA - LIGHTS(0, 1)) + 0x230000, 0x210000);
        hudDrawMap(0, (-gHudSlideA - LIGHTS(0, 2)) + 0x50000, 0x610000);
        cameraViewStep(1);
        hudSplitUpdate(1);
        bgDrawSetup(1, 1);
        hudDrawGauges2P(1, LIGHTS(1, 1) + gHudSlideA + 0x1170000, 0);
        if (gPlayerSetup[1].place > 0 && gPlayerSetup[1].place < 11) {
            hudDrawPlace(gPlayerSetup[1].place, LIGHTS(1, 0) + gHudSlideA + 0xF40000, 0x3F0000);
        }
        drawDL(gHudDLs[42], gHudSlideA * gHudStartArg + 0x10B0000, 0x580000, 0, 0x10000);
        hudDrawSplit2P(1, (-gHudSlideA - LIGHTS(1, 1)) + 0x230000, 0x530000);
        hudDrawMap(1, (-gHudSlideA - LIGHTS(1, 2)) + 0x50000, 0x2F0000);
    } else {
        cameraViewStep(0);
        bgDrawSetup(0, 1);
        hudDrawSpeedo(0, -gHudSlideA - LIGHTS(0, 2), 0);
        hudDrawTacho(0, LIGHTS(0, 0) + gHudSlideA + 0x1170000, 0);
        hudDrawTimeLeft(0, 0x8B0000, LIGHTS(0, 1) + gHudSlideA + 0xC20000);
        hudDrawRaceTime(0x50000, LIGHTS(0, 1) + gHudSlideA + 0xC20000);
        if (gRacersEnabled != 0 && gPlayerSetup[0].place > 0 && gPlayerSetup[0].place < 11) {
            hudDrawPlace(gPlayerSetup[0].place, 0xF40000, LIGHTS(0, 0) + gHudSlideA + 0xB80000);
        }
        hudDrawMap(0, LIGHTS(0, 2) + gHudSlideA + 0xFD0000, 0x9F0000);
    }
    phase = gCountdownPhase;
    if (phase < 5) {
        hudDrawStartLights(0xFFEC0000 - gHudSlideC, 0x970000);
    }
    if (phase == 7) {
        hudDrawFinish(0x640000, 0xFFF30000 - gHudSlideD);
    }
    if (phase == 5) {
        hudRadioButton();
    }
}

extern TextObj *gCountdownText;          /* 0x800468C8 ovl2 */
extern s32      gStartLightFrame;        /* 0x800468B4 ovl2 - start-light animation frame 0..6 */
extern s32      gStartLightTicks;        /* 0x800468B8 ovl2 */
extern s32      gFinishUnk8BC;           /* 0x800468BC ovl2 */
extern s32      gFinishUnk8C0;           /* 0x800468C0 ovl2 */
extern char    *gCountdownWords[3];      /* 0x8004288C ovl2 - "READY", "SET", "GO" */
extern u32      gButtonsA[2];            /* 0x8014E350 - the player's configured buttons (5 sets) */
extern u32      gButtonsB[2];            /* 0x8014E358 */
extern u32      gButtonsD[2];            /* 0x8014E368 */
extern AnimCmd gTrackNameHideScript[];  /* 0x80042698 ovl2 */
extern AnimCmd gTrackNameScript2[];     /* 0x80042724 ovl2 */
extern AnimCmd gGoScript[];             /* 0x8004274C ovl2 */
extern AnimCmd gGo2PScript[];           /* 0x800426FC ovl2 */
extern AnimCmd gTrackNameOutScript[];   /* 0x80042648 ovl2 */
extern AnimCmd gFinishFlagScript[];     /* 0x800427EC ovl2 */
extern f32      gSetDelay;               /* 0x80045940 ovl2 - 0.8 */
extern f32      gLightsDoneDelay;        /* 0x80045944 ovl2 - 0.3 */
extern f32      gLightsTick;             /* 0x80045948 ovl2 - 0.1 */

void drawBox(s32 w, s32 h, s32 x, s32 y); /* 0x80116164 */
s32  tblSin(s32 angle);                  /* 0x80113E80 */
void raceGo(void);                       /* 0x8002E858 ovl2 */
void hudFinishText(void);                /* 0x8002CDCC ovl2 */
void radarOffset(f32 px, f32 pz, f32 heading, s32 vehicle, f32 *dx, f32 *dy);
void radarDot(s32 icon, s32 x, s32 y, s32 r, s32 g, s32 b);
s32  hudCountdownStep(s32 slot, s32 b, f32 c);
void hudCountdownNext(void);
f32  hudLightsTickCb(s32 a, s32 b);
f32  hudLightsDone(s32 a, s32 b);
s32  hudGoDone(s32 slot, s32 b, f32 c);
void hudLightsTick(void);
s32  hudTrackNameFree(s32 slot, s32 b, f32 c);
f32  hudCountdownCb(s32 a, s32 b);
f32  hudSetCb(s32 a, s32 b);
void hudFinishFlag(void);

/* 0x8002C248 ovl2 - the radar: nearby cars as dots around the player (2-player: the other
 * player is always shown, pinned to the edge) */
void hudDrawMap(s32 player, s32 x, s32 y) {
    Car *car;
    f32 px;
    f32 pz;
    f32 heading;
    f32 dx;
    f32 dy;
    s32 v;
    s32 r;
    s32 g;
    s32 b;

    drawBox(21, 43, x, y);
    car = &gCars[player];
    px = car->pos[0] * 0.2;
    heading = car->heading;
    pz = car->pos[2] * -0.2;
    for (v = gNumVehicles - 1; v >= 0; v--) {
        if (v == player) {
            continue;
        }
        radarOffset(px, pz, heading, v, &dx, &dy);
        if (gNumPlayersM1 != 0 && v == (player == 0)) {
            if (dx < -9.0f - 3.0f) {
                dx = -9.0f - 3.0f;
            }
            if (9.0f + 3.0f < dx) {
                dx = 9.0f + 3.0f;
            }
            if (dy < -20.0f - 3.0f) {
                dy = -20.0f - 3.0f;
            }
            if (20.0f + 3.0f < dy) {
                dy = 20.0f + 3.0f;
            }
            radarDot(v, x + (s32)((f64)(s32)dx * 65536.0) + 0x90000,
                     y + (s32)((f64)(s32)dy * 65536.0) + 0xFFEC0000, 0xFF, 0, 0x20);
        } else if (-9.0f <= dx && dx <= 9.0f && -20.0f <= dy && dy <= 20.0f) {
            if (v >= gNumPlayers + gNumAiRacers) {
                r = 0x80;                        /* traffic: grey */
                g = 0x80;
                b = 0x80;
            } else {
                r = 0xFF;                        /* racers: red */
                g = 0;
                b = 0x20;
            }
            radarDot(-1, (s32)((f32)(x + 0x90000) + dx * 65536.0f),
                     (s32)((f32)(y + 0xFFEC0000) + dy * 65536.0f), r, g, b);
        }
    }
    radarDot(-1, x + 0x90000, y + 0xFFEC0000, 0xFF, 0xFF, 0xFF);   /* the player: white */
}

/* 0x8002C5E4 ovl2 - where vehicle v appears on the radar of a car at (px, pz) facing 'heading' */
void radarOffset(f32 px, f32 pz, f32 heading, s32 vehicle, f32 *dx, f32 *dy) {
    Car *c = &gCars[vehicle];
    f32 ox;
    f32 oz;
    f32 d;
    f64 dd;
    s32 a;

    ox = (f64)px - (f64)c->pos[0] * 0.2;
    oz = (f64)pz - (f64)c->pos[2] * -0.2;
    d = sqrtf(ox * ox + oz * oz);
    a = (s32)((f64)heading * 10430.37835) - atan2s(oz, ox);
    dd = d;
    *dx = ((f64)(f32)tblSin(a) / 16384.0) * dd * (1.0 / 70.0);
    *dy = ((f64)(f32)tblCos(a) / 16384.0) * dd * (1.0 / 330.0);
}

/* 0x8002C750 ovl2 - one radar dot (with the player number for the other player in 2-player games) */
void radarDot(s32 icon, s32 x, s32 y, s32 r, s32 g, s32 b) {
    rectFill(3, 3, x, y, r, g, b, 0xFF);
    if (icon >= 0) {
        drawDL(gHudDLs[43 + icon], x + 0x20000, y + 0x10000, 0, 0x10000);
    }
}

/* 0x8002C7E8 ovl2 - the track name has been shown: start the countdown */
s32 hudTrackNameDone(s32 slot, s32 b, f32 c) {
    animStart(1.0f, gTrackNameHideScript, NULL, hudCountdownStep, 0);
    animStart(2.0f, gTrackNameScript2, NULL, NULL, 4);
    return 1;
}

/* 0x8002C858 ovl2 */
s32 hudCountdownStep(s32 slot, s32 b, f32 c) {
    hudCountdownNext();
    return 2;
}

/* 0x8002C888 ovl2 - the finish animation is over */
s32 hudFinishDone(s32 slot, s32 b, f32 c) {
    if (gRaceWinner >= 0) {
        hudFinishFlag();
    } else {
        if (gNumPlayersM1 != 0 && gRaceLoser >= 0) {
            textFree(gHudWinner);
        }
        hudFree();
    }
    return 2;
}

/* 0x8002C904 ovl2 - next step of READY, SET, GO */
void hudCountdownNext(void) {
    s32 v = gCountdownPhase;

    if (v >= 2) {
        textFree(gCountdownText);
        v = gCountdownPhase;
    }
    if (v >= 4) {
        return;
    }
    gCountdownPhase = v + 1;
    gCountdownText = textCreate(2, gCountdownWords[v + 1 - 2], 2);
    textPlace(&gCountdownText->xf, 0x8B0000, 0x6E0000, 0, 1);
    animWait(1.0f, NULL, (AnimDoneFunc)hudCountdownCb);
    v = gCountdownPhase;
    if (v == 2) {
        sfxPlay(0xF, -1);                    /* READY */
        v = gCountdownPhase;
    } else if (v == 3) {
        sfxPlay(0x10, -1);                   /* SET */
        v = gCountdownPhase;
    } else if (v == 4) {
        sfxStop(0x7D, -1);                   /* GO */
        sfxPlay(0xE, -1);
        if (gRacersEnabled != 0) {
            sfxPlay(0x54, -1);
        }
        raceGo();
        v = gCountdownPhase;
    }
    if (v == 3) {
        animWait(gSetDelay, NULL, (AnimDoneFunc)hudSetCb);
        v = gCountdownPhase;
    }
    if (v == 4) {
        animStart(1.0f, gTrackNameOutScript, &gHudTrackName->xf, hudTrackNameFree, 0);
    }
}

/* 0x8002CAAC ovl2 */
f32 hudLightsTickCb(s32 a, s32 b) {
    hudLightsTick();
    return -1.0f;
}

/* 0x8002CAD8 ovl2 - start lights finished */
f32 hudLightsDone(s32 a, s32 b) {
    animStart(1.5f, gGoScript, NULL, hudGoDone, 0);
    return -1.0f;
}

/* 0x8002CB20 ovl2 - GO has gone: racing (phase 5); swallow the buttons pressed during the countdown */
s32 hudGoDone(s32 slot, s32 b, f32 c) {
    s32 n = gNumPlayersM1;
    s32 p;

    for (p = 0; p <= gNumPlayersM1; p++) {
        contPressed(p, gViewButton[p] | gButtonsA[p] | gButtonsB[p] | gLightsButton[p] | gButtonsD[p]);
    }
    gCountdownPhase = 5;
    if (n != 0) {
        animStart(0.5f, gGo2PScript, NULL, NULL, 4);
    }
    return 2;
}

/* 0x8002CC3C ovl2 - the start lights: 17 steps of 0.1 s, cycling 7 frames */
void hudLightsTick(void) {
    if (gStartLightTicks == 17) {
        animWait(gLightsDoneDelay, NULL, (AnimDoneFunc)hudLightsDone);
        return;
    }
    if (++gStartLightFrame == 7) {
        gStartLightFrame = 0;
    }
    gStartLightTicks++;
    animWait(gLightsTick, NULL, (AnimDoneFunc)hudLightsTickCb);
}

/* 0x8002CCC8 ovl2 */
s32 hudTrackNameFree(s32 slot, s32 b, f32 c) {
    textFree(gHudTrackName);
    return 2;
}

/* 0x8002CCFC ovl2 */
f32 hudCountdownCb(s32 a, s32 b) {
    hudCountdownNext();
    return -1.0f;
}

/* 0x8002CD28 ovl2 */
f32 hudSetCb(s32 a, s32 b) {
    hudLightsTick();
    return -1.0f;
}

/* 0x8002CD54 ovl2 - a player won: the chequered flag */
void hudFinishFlag(void) {
    gCountdownPhase = 7;
    musicPlay(0xAC);
    animStart(2.0f, gFinishFlagScript, NULL, NULL, 4);
    gHudSlideD = 0x6E0000;
    gFinishUnk8BC = -1;
    gFinishUnk8C0 = 9;
    hudFinishText();
}

extern s32 *gMtxCursor;                  /* 0x80147688 */
extern u32 *gDlCursor;                   /* 0x80147684 */
extern s32  gSegment3Base;               /* main segment */
extern u16  gHudPalSave[28];             /* 0x801509C0 - original colours of the tacho (24) and gear lights (4) */
extern s32  gBlinkFast;                  /* 0x8014EF78 - looping anim ramp for flashing text (0.25 s) */
extern TextObj *gHudUnk9AC;              /* 0x800469AC ovl2 */
extern f32  gFlagFrameTime;              /* 0x80045950 ovl2 - 0.1 */
extern f32  gFlagSkipTime;               /* 0x8004594C ovl2 - 0.1 */
extern f32  gFinishOutTime;              /* 0x80045954 ovl2 - 0.6 */
extern AnimCmd gFinishOutScript[];          /* 0x80042814 ovl2 */
extern f64  gTimeRoundUp2;               /* 0x80045968 ovl2 - 0.99 */
extern f64  gTimeRoundUp3;               /* 0x80045970 ovl2 */
/* A cartoon pop-up that flies in when the announcer comments (0x10 bytes) */
typedef struct Popup {
    /* 0x00 */ s8  id;                   /* kind, -1 = free */
    /* 0x01 */ s8  state;                /* 0 flying in, 1 animating, 2 flying away */
    /* 0x02 */ s8  frame;
    /* 0x03 */ u8  timer;
    /* 0x04 */ s32 vel;
    /* 0x08 */ s32 x;                    /* 16.16, counts down to 0 as it flies in */
    /* 0x0C */ s16 angle;
    /* 0x0E */ s16 dist;
} Popup;
extern Popup gPopups[2][25];   /* 0x80046A78 ovl2 - 0x190 bytes per player */
extern s32  gPopupHead[2];            /* 0x80046A20 ovl2 */

void mtxTranslate(s32 *m, s32 x, s32 y, s32 z); /* 0x80114498 - fixed-point 4x4 */
void textSetTime(TextObj *t, s32 value, s32 min, s32 max); /* 0x8011A3AC */
void cameraSetView(s32 player, s32 quiet); /* 0x80034830 ovl2 */
f32  hudFinishTextCb(s32 a, s32 b);
f32  hudFinishOut(s32 a, s32 b);
s32  hudFinishOutDone(s32 slot, s32 b, f32 c);
void hudTachoColours(s32 player);
void hudGearColours(s32 player);
void hudDrawSplitTime(s32 player, s32 x, s32 y);
f32  hudSplitHide(s32 a, s32 player);

#define GSP_MATRIX(m) { u32 *g_ = gDlCursor; gDlCursor += 2; g_[0] = 0x01040040; g_[1] = (u32)(m) - 0x40; }
#define GSP_DL(dl)    { u32 *g_ = gDlCursor; gDlCursor += 2; g_[0] = 0x06000000; g_[1] = (u32)(dl); }
#define GSP_SEG3(v)   { u32 *g_ = gDlCursor; gDlCursor += 2; g_[0] = 0xBC000C06; g_[1] = (v); }
#define GSP_POP()     { u32 *g_ = gDlCursor; gDlCursor += 2; g_[1] = 0; g_[0] = 0xBD000000; }

/* 0x8002CDCC ovl2 - the winner's caption flashes 9 times (START skips it) */
void hudFinishText(void) {
    if ((contPressed(gRaceWinner, BTN_START) & BTN_START) && gHudSlideD == 0) {
        gHudWinner->blink = -1;
        animWait(gFlagSkipTime, NULL, (AnimDoneFunc)hudFinishOut);
        return;
    }
    if (++gFinishUnk8BC >= 6) {
        if (gFinishUnk8C0 == 1) {
            gHudWinner->blink = -1;
        }
        if (gFinishUnk8C0-- == 0) {
            gFinishUnk8BC = 5;
            animWait(1.5f, NULL, (AnimDoneFunc)hudFinishOut);
            return;
        }
        gFinishUnk8BC = 0;
    }
    animWait(gFlagFrameTime, NULL, (AnimDoneFunc)hudFinishTextCb);
}

/* 0x8002CED0 ovl2 */
f32 hudFinishTextCb(s32 a, s32 b) {
    hudFinishText();
    return -1.0f;
}

/* 0x8002CEFC ovl2 */
f32 hudFinishOut(s32 a, s32 b) {
    musicPlay(0xA2);
    textFree(gHudWinner);
    animStart(gFinishOutTime, gFinishOutScript, NULL, hudFinishOutDone, 0);
    return -1.0f;
}

/* 0x8002CF58 ovl2 */
s32 hudFinishOutDone(s32 slot, s32 b, f32 c) {
    hudFree();
    return 2;
}

/* 0x8002CF88 ovl2 - the trophy girl at the finish (6 frames: group 0x31A/0x31B entries 7-12, meshes 0x364-0x369) */
#ifdef PORT
/* The port draws the arcade's trophy girl (the user's frames, 107x179) instead
 * of the N64's: one quad per frame, as tall as the N64's (y -11 to 120) and
 * wider to keep her shape, centred on the same spot. Its texture is a small
 * stand-in that port/gfx.c swaps for the frame's picture (found by its
 * address). gTrophyOriginal (a cheat, to come) brings back the N64's. */
s32 gTrophyOriginal;
u16 gTrophyDummy[6][16 * 32] __attribute__((aligned(8)));
static Vtx gTrophyVtx[4] __attribute__((aligned(8)));
static u32 gTrophyDL[6][32] __attribute__((aligned(8)));

#define TROPHY_W   107                   /* the frames, in pixels (of the 128x256 textures) */
#define TROPHY_H   179
#define TROPHY_TOP 120                   /* the N64 frames' top and bottom */
#define TROPHY_BOT (-11)

static void trophyBuild(void) {
    s32 halfW = (TROPHY_W * (TROPHY_TOP - TROPHY_BOT) / TROPHY_H + 1) / 2;
    s32 S = TROPHY_W * 64 / 8, T = TROPHY_H * 64 / 8;   /* texels of the 16x32 stand-in (x8 = the picture), at the texture scale 1/2 */
    s32 f, i;

    for (i = 0; i < 4; i++) {
        Vtx_t *v = &gTrophyVtx[i].v;
        v->ob[0] = 32 + ((i >= 2) ? halfW : -halfW);
        v->ob[1] = (i == 1 || i == 2) ? TROPHY_BOT : TROPHY_TOP;
        v->ob[2] = 0;
        v->flag = 0;
        v->tc[0] = (i >= 2) ? S : 0;
        v->tc[1] = (i == 1 || i == 2) ? T : 0;
        v->cn[0] = v->cn[1] = v->cn[2] = v->cn[3] = 0;
    }
    for (f = 0; f < 6; f++) {
        u32 *d = gTrophyDL[f];
        for (i = 0; i < 16 * 32; i++) {
            gTrophyDummy[f][i] = f + 1;           /* (each its own contents) */
        }
        *d++ = 0xE7000000; *d++ = 0;
        *d++ = 0xFD100000; *d++ = (u32)gTrophyDummy[f];   /* RGBA16 */
        *d++ = 0xF5100000; *d++ = 0x07000000;
        *d++ = 0xE6000000; *d++ = 0;
        *d++ = 0xF3000000; *d++ = 0x071FF200;   /* 512 texels */
        *d++ = 0xE7000000; *d++ = 0;
        *d++ = 0xF5100800; *d++ = 0x00080200;   /* tile 0: 4 words a line, clamped */
        *d++ = 0xF2000000; *d++ = 0x0003C07C;   /* 16x32 */
        *d++ = 0xBA000E02; *d++ = 0;            /* no palette */
        *d++ = 0x04300040; *d++ = (u32)gTrophyVtx;
        *d++ = 0xBF000000; *d++ = 0x00000A14;
        *d++ = 0xBF000000; *d++ = 0x0000141E;
        *d++ = 0xB8000000; *d++ = 0;
    }
}
#endif

void hudDrawFinish(s32 x, s32 y) {
    bgDrawSetup(gRaceWinner, 1);
#ifdef PORT
    if (!gTrophyOriginal) {
        static s32 built;
        if (!built) {
            trophyBuild();
            built = 1;
        }
        drawDL(gTrophyDL[gFinishUnk8BC], x, y, 0, (gNumPlayersM1 != 0) ? 0xC000 : 0x10000);
        return;
    }
#endif
    drawDL(gHudDLs[7 + gFinishUnk8BC], x, y, 0, (gNumPlayersM1 != 0) ? 0xC000 : 0x10000);
}

/* 0x8002CFFC ovl2 - the view button: cycle the camera (0-2; 2-player games skip view 1) */
void hudChangeView(s32 player) {
    PlayerSetup *ps = &gPlayerSetup[player];
    s32 step = ps->unk390;
    s32 v;

    v = ps->cameraFollowsCar + step;
    ps->cameraFollowsCar = v;
    if (gNumPlayersM1 != 0 && v == 1) {
        ps->cameraFollowsCar = v + step;
    }
    v = ps->cameraFollowsCar;
    if (v < 0) {
        ps->cameraFollowsCar = 2;
    } else if (v >= 3) {
        ps->cameraFollowsCar = 0;
    }
    cameraSetView(player, 0);
}

#define PS_FLAGS(ps) (((u8 *)&(ps)->unk01C)[0])  /* 1 = finished in front, 8 = won */

/* 0x8002D098 ovl2 - race over: work out the winner, LOST BY and the race/total times */
void hudFinish(void) {
    PlayerSetup *ps;
    PlayerSetup *me;
    PlayerSetup *other;
    s32 p;
    s32 f;
    f32 d, frac, avg, v, sp;

    gRaceTime = splitTimeRound(gRaceTime);
    gRaceWinner = -1;
    gRaceLoser = -1;
    gLostBy = 0.0f;
    for (p = 0, ps = gPlayerSetup; p <= gNumPlayersM1; p++, ps++) {
        PS_FLAGS(ps) = 0;
    }

    /* the player in front (only if the race wasn't lost on time) */
    if (gRaceTimeLeft != 0.0f) {
        if (gNumPlayersM1 != 0) {
            gRaceLoser = gPlayerSetup[1].place < gPlayerSetup[0].place;
        } else {
            gRaceLoser = 0;
        }
    }
    f = gRaceLoser;

    /* 2 players: estimate how long the player behind needs to reach the line */
    if (f >= 0 && gNumPlayersM1 != 0) {
        other = &gPlayerSetup[f == 0];
        me = &gPlayerSetup[f];
        if (me->distance <= other->distance) {
            other->distance = me->distance;
        }
        d = me->distance - other->distance;
        if (me->distance != 0.0f) {
            frac = other->distance / me->distance;
            avg = other->odometer / gRaceTime;
            v = (gUnitsKph ? 0.6215040684f : 1.0f) * other->speedo;
            if (v < avg) {
                v = avg;
            }
            sp = (v - avg) * frac + avg;
            if (sp == 0.0f) {
                gLostBy = gRaceTime;
            } else {
                gLostBy = d / sp;
                if (gRaceTime < gLostBy) {
                    gLostBy = gRaceTime;
                }
                if (gLostBy == 0.0f) {
                    gLostBy = 0.01f;
                }
                gLostBy = splitTimeRound(gLostBy);
            }
        }
        f = gRaceLoser;
    }

    if (f >= 0) {
        if (gNumPlayersM1 != 0) {
            if (gPlayerSetup[0].place == 1) {
                gRaceWinner = 0;
            } else if (gPlayerSetup[1].place == 1) {
                gRaceWinner = 1;
            }
            PS_FLAGS(&gPlayerSetup[f]) |= 8;
            ps = &gPlayerSetup[f];
        } else {
            if (gPlayerSetup[0].place == 1 && gRacersEnabled != 0) {
                gRaceWinner = 0;
                PS_FLAGS(&gPlayerSetup[0]) |= 8;
            }
            ps = &gPlayerSetup[0];
        }
        PS_FLAGS(ps) |= 1;
    }

    /* original bug: out of time in 1 player (f = -1) writes gPlayerSetup[-1].raceTime */
    if (gNumPlayersM1 != 0) {
        gPlayerSetup[f].raceTime = gRaceTime;
        gPlayerSetup[f == 0].raceTime = splitTimeRound(gRaceTime + gLostBy);
    } else {
        gPlayerSetup[f].raceTime = gRaceTime;
    }

    if (gRaceWinner >= 0) {
        f = gRaceLoser;
        if (gNumPlayersM1 != 0) {
            gPlayerSetup[f].totalTime += gPlayerSetup[f].raceTime;
            gPlayerSetup[f == 0].totalTime += gPlayerSetup[f == 0].raceTime;
            gPlayerSetup[f].totalTime = splitTimeRound(gPlayerSetup[f].totalTime);
            gPlayerSetup[gRaceLoser == 0].totalTime = splitTimeRound(gPlayerSetup[gRaceLoser == 0].totalTime);
        } else {
            gPlayerSetup[0].totalTime += gPlayerSetup[f].raceTime;
            gPlayerSetup[0].totalTime = splitTimeRound(gPlayerSetup[0].totalTime);
        }
    }
}

/* 0x8002D518 ovl2 - round a time to 1/100 s */
f32 splitTimeRound(f32 t) {
    return (f32)(s32)((t + 0.005f) * 100.0f) / 100.0f + 0.0001f;
}

/* 0x8002D560 ovl2 - the race time (2-player: hidden while a result is shown) */
void hudDrawRaceTime(s32 x, s32 y) {
    textSetTime(gHudTimeLeft, (s32)(splitTimeRound(gRaceTime) * 100.0f), 0, 0x927BF);
    gHudTimeLeft->xf.x = x + 0xFFFC0000;
    gHudTimeLeft->xf.y = y + 0xFFF60000;
    if (gNumPlayersM1 == 0 || gHudUnk9AC == NULL) {
        gHudTimeLeft->xf.visible = 1;
        drawBox(0x3F, 0x15, x, y);
        drawDL(gHudDLs[28], x + 0x80000, y + 0xFFFF0000, 0, 0x10000);
        return;
    }
    gHudTimeLeft->xf.visible = 0;
}

/* 0x8002D664 ovl2 - 1-player speedometer (mph or km/h dial) */
void hudDrawSpeedo(s32 player, s32 x, s32 y) {
    s32 *m = gMtxCursor;

    gMtxCursor += 16;
    mtxTranslate(m, x, y, 0);
    GSP_MATRIX(m);
    GSP_DL(gHudDLs[39]);
    if (gUnitsKph != 0) {
        GSP_DL(gHudDLs[40]);
    }
    GSP_POP();
    gHudSpeed[player]->xf.x = x + 0x60000;
    gHudSpeed[player]->xf.y = y + 0x1B0000;
}

/* 0x8002D77C ovl2 - 1-player rev counter and gear */
void hudDrawTacho(s32 player, s32 x, s32 y) {
    PlayerSetup *ps;

    hudTachoColours(player);
    hudGearColours(player);
    GSP_SEG3(player << 9);
    drawDL(gHudDLs[31], x, y, 0, 0x10000);
    GSP_SEG3(gSegment3Base << 9);
    ps = &gPlayerSetup[player];
    drawDL(gHudDLs[32 + ps->gear], x + 0xFFE30000, y + 0xE0000, 0, 0x10000);
    drawDL(gHudDLs[37 + ps->manual], x + 0xFFF50000, y + 0x40000, 0, 0x10000);
}

/* 0x8002D8D8 ovl2 - 2-player rev counter, gear and speed */
void hudDrawGauges2P(s32 player, s32 x, s32 y) {
    PlayerSetup *ps;
    s32 *m;

    hudTachoColours(player);
    m = gMtxCursor;
    gMtxCursor += 16;
    mtxTranslate(m, x, y, 0);
    GSP_MATRIX(m);
    GSP_SEG3(player << 9);
    GSP_DL(gHudDLs[31]);
    GSP_SEG3(gSegment3Base << 9);
    GSP_DL(gHudDLs[39 + gUnitsKph]);
    ps = &gPlayerSetup[player];
    drawDL(gHudDLs[32 + ps->gear], 0xFFEA0000, 0x30000, 0, 0x10000);
    drawDL(gHudDLs[37 + ps->manual], 0xFFF20000, 0x30000, 0, 0x10000);
    GSP_POP();
    gHudSpeed[player]->xf.x = x + 0xFFDE0000;
    gHudSpeed[player]->xf.y = y + 0x180000;
}

/* 0x8002DADC ovl2 - light the first (rpm * 24 / 255) segments of the rev counter */
void hudTachoColours(s32 player) {
    s32 n = gPlayerSetup[player].tacho * 24 / 255;
    u16 *pal = (u16 *)gHudPalA[player];
    s32 i;

    for (i = 0; i < 24; i++) {
        pal[i] = (i < n) ? gHudPalSave[i] : 1;
    }
}

/* 0x8002DBF8 ovl2 - light the current gear (1-4) */
void hudGearColours(s32 player) {
    s32 g = gPlayerSetup[player].gear;
    u16 *pal = (u16 *)gHudPalB[player];

    pal[0] = (g == 1) ? gHudPalSave[24] : 1;
    pal[1] = (g == 2) ? gHudPalSave[25] : 1;
    pal[2] = (g == 3) ? gHudPalSave[26] : 1;
    pal[3] = (g == 4) ? gHudPalSave[27] : 1;
}

/* 0x8002DCB8 ovl2 - 1-player: seconds left (or the split time while it is shown) */
void hudDrawTimeLeft(s32 player, s32 x, s32 y) {
    hudSplitUpdate(player);
    if (gHudAnimA30[player] >= 0) {
        hudDrawSplitTime(player, x, y);
        return;
    }
    textSetNumber(gHudTimeNum[player], (s32)((f64)gRaceTimeLeft + gTimeRoundUp2), 2, 0x30, 0);
    drawDL(gHudDLs[29], x, y, 0, 0x10000);
    gHudTimeNum[player]->xf.x = x;
    gHudTimeNum[player]->xf.y = y + 0xFFF60000;
}

/* 0x8002DD98 ovl2 - 2-player: the split time while it is shown */
void hudDrawSplit2P(s32 player, s32 x, s32 y) {
    hudSplitUpdate(player);
    if (gHudAnimA30[player] >= 0) {
        hudDrawSplitTime(player, x, y);
    }
}

/* 0x8002DDE8 ovl2 - 2-player: seconds left */
void hudDrawTime2P(s32 player, s32 x, s32 y, s32 slide) {
    textSetNumber(gHudTimeNum[player], (s32)((f64)gRaceTimeLeft + gTimeRoundUp3), 2, 0x30, 0);
    gHudTimeNum[player]->xf.x = x + 0x50000;
    gHudTimeNum[player]->xf.y = y + 0xFFFC0000;
}

/* 0x8002DE80 ovl2 - a new split time (difference to the record) is shown for 6 seconds */
void hudSplitUpdate(s32 player) {
    PlayerSetup *ps = &gPlayerSetup[player];
    s32 d = ps->unk020;

    if (d == 600000) {
        return;
    }
    if (gHudAnimA30[player] == -1) {
        textSetTime(gHudSplit[player], d, -599999, 599999);
        if (ps->unk020 > 0) {
            gHudSplit[player]->blink = gBlinkFast;
        } else {
            gHudSplit[player]->blink = -1;
        }
        gHudSplit[player]->xf.visible = 1;
        if (gNumPlayersM1 == 0) {
            gHudTimeNum[player]->xf.visible = 0;
        }
        gHudAnimA30[player] = animWait(6.0f, (void *)player, (AnimDoneFunc)hudSplitHide);
    }
    ps->unk020 = 600000;
}

/* 0x8002DFC0 ovl2 */
void hudDrawSplitTime(s32 player, s32 x, s32 y) {
    drawDL(gHudDLs[30], x, y, 0, 0x10000);
    gHudSplit[player]->xf.x = ((gPlayerSetup[player].unk020 < 0) ? 0 : 0xFFFB0000) + x;
    gHudSplit[player]->xf.y = y + 0xFFF60000;
}

/* 0x8002E070 ovl2 */
f32 hudSplitHide(s32 a, s32 player) {
    gHudAnimA30[player] = -1;
    gHudSplit[player]->xf.visible = 0;
    gHudTimeNum[player]->xf.visible = 1;
    return -1.0f;
}

/* 0x8002E0BC ovl2 - the start lights */
void hudDrawStartLights(s32 x, s32 y) {
    bgDrawSetup(-1, 1);
    drawBox(0x61, 0x64, x, y);
    drawDL(gHudDLs[gStartLightFrame], x + 0x2F0000, y + 0xFFA10000, 0, 0x10000);
}

/* 0x8002E13C ovl2 - empty the announcer queues */
void hudPopupsInit(void) {
    s32 p;
    s32 i;

    for (p = 0; p < 2; p++) {
        gPopups[p][0].id = -1;
        for (i = 1; i < 25; i++) {
            gPopups[p][i].id = -1;
        }
        gPopupHead[p] = 0;
        gCalloutTimer[p] = -1;
    }
}

typedef struct RadioStation { char *name; s32 track; } RadioStation;
extern RadioStation gRadioStations[8];   /* 0x800428E8 ovl2 */
extern s32  gTrackStation[14];           /* 0x800428B0 ovl2 - default station per track */
extern s32  gRadioStation;               /* 0x80150970 */
extern s32  gRadioCaptionY[2];           /* 0x80042928 ovl2 - by player count */
extern s32  gRadioAnim;                  /* 0x800469F8 ovl2 */
extern s32  gRadioCaptionX;              /* 0x800469FC ovl2 */
extern s32  gPopupDL[];                  /* 0x800428A0 ovl2 - first HUD graphic of each pop-up kind */
extern s32  gPopupFrames[];              /* 0x800428A8 ovl2 - frames of each pop-up kind */
extern f64  gPopupDistScale;             /* 0x80045978 ovl2 - 1000 */
extern f32  gPopupStopChance;            /* 0x80045980 ovl2 - 0.2 */
extern u32  gMusicButton[2];             /* 0x8014E368 */
void pauseMusicResume(void);                    /* 0x8002EB60 ovl2 */
void pauseMusicStop(void);                    /* 0x8002EB94 ovl2 */
s32  textWidth(char *s, s32 font);       /* 0x8011A03C */
void radioCaption(void);
f32  radioCaptionDone(s32 a, s32 b);

/* 0x8002E1B4 ovl2 - queue 'count' pop-ups of a kind (kind 0 also plays a whoosh) */
void hudPopup(s32 player, s32 kind, s32 count) {
    Popup *q = gPopups[player];
    s32 *head = &gPopupHead[player];
    Popup *e;
    s32 i;

    if ((s8)kind == 0) {
        sfxPlay(4, player);
        sfxPlay(4, player);
    }
    if ((s16)count <= 0) {
        return;
    }
    i = 0;
    do {
        e = &q[*head];
        if (e->id >= 0) {
            return;                          /* queue full */
        }
        e->id = kind;
        e->state = 0;
        e->frame = 0;
        e->x = i * 0x640000 + 0x3E80000;
        q[*head].angle = randRange(0, 0x8000);
        i++;
        q[*head].dist = randRange(100, 1200);
        if (++*head >= 25) {
            *head = 0;
        }
    } while (i != (s16)count);
}

/* 0x8002E2F4 ovl2 - draw the pop-ups (players with the announcer on) */
void hudPopupsDraw(void) {
    PlayerSetup *ps;
    Popup *e;
    s32 p;
    s32 i;
    s32 *head;
    s32 x0;
    s32 ox;
    s32 oy;
    s32 v;
    s32 a;
    f32 r;
    f32 sn;

    for (p = 0, ps = gPlayerSetup; p <= gNumPlayersM1; p++, ps++) {
        if (ps->viewAtEdge == 0) {
            continue;
        }
        head = &gPopupHead[p];
        bgDrawSetup(p, 0);
        i = *head;
        do {
            if (--i < 0) {
                i = 24;
            }
            e = &gPopups[p][i];
            if (e->id < 0) {
                continue;
            }
            a = e->angle;
            r = (f64)(f32)e->dist * gPopupDistScale;
            sn = (f64)(f32)tblSin(a) / 16384.0;
            ox = (s32)(r * (f32)((f64)(f32)tblCos(a) / 16384.0));
            x0 = e->x;
            if (x0 < 0) {
                x0 = 0;
            }
            if (e->id == 1) {
                v = 0;
                oy = x0 / ((gNumPlayersM1 != 0) ? 50 : 5);
            } else {
                a = i * 100000 + x0;
                if (i & 1) {
                    a = -a;
                }
                v = tblSin(a >> 8) * 40;
                oy = tblCos(a >> 9) * 40 + 0xFFF60000;
            }
            drawDL(gHudDLs[gPopupDL[e->id] + e->frame], ox + v, (s32)(r * sn) + oy + 0xFFF80000, 0x74EA0000 - e->x, 0x10000);
        } while (i != *head);
    }
}

/* 0x8002E5CC ovl2 - move the pop-ups */
void hudPopupsMove(void) {
    Popup *e;
    s32 p;
    s32 i;
    s32 v;

    for (p = 0; p <= gNumPlayersM1; p++) {
        i = gPopupHead[p];
        do {
            if (--i < 0) {
                i = 24;
            }
            e = &gPopups[p][i];
            if (e->id < 0) {
                continue;
            }
            switch (e->state) {
                case 0:
                    e->x += 0xFFEC0000;
                    if (e->x < 0) {
                        e->x = 0;
                        e->state = 1;
                        e->frame++;
                        e->timer = 2;
                        if (gPlayerSetup[p].viewAtEdge != 0) {      /* announcer on */
                            if (randChance(0.5f)) {
                                sfxPlay(0x5F, p);
                                sfxPlay(0x5F, p);
                                sfxPlay(0x5F, p);
                            } else {
                                sfxPlay(0x60, p);
                                sfxPlay(0x60, p);
                                sfxPlay(0x60, p);
                            }
                        }
                    }
                    break;
                case 1:
                    if ((u8)--e->timer == 0) {
                        e->timer = 2;
                        e->frame++;
                        if (e->frame == gPopupFrames[e->id] || randChance(gPopupStopChance)) {
                            e->frame--;
                            e->vel = 1;
                            e->state = 2;
                        }
                    }
                    /* fall through */
                case 2:
                    v = e->vel;
                    e->dist += v;
                    if (e->dist >= 2401) {
                        e->id = -1;
                    } else {
                        e->vel = v + 1;
                        if (v + 1 >= 51) {
                            e->vel = 50;
                        }
                    }
                    break;
            }
        } while (i != gPopupHead[p]);
    }
}

/* 0x8002E858 ovl2 - GO: start the track's radio station */
void raceGo(void) {
    gHudUnk9AC = NULL;
    gRadioAnim = -1;
    gRadioStation = gTrackStation[gTrackChoice];
    pauseMusicResume();
}

/* 0x8002E8A8 ovl2 */
void radioOff(void) {
    pauseMusicStop();
    if (gHudUnk9AC != NULL) {
        textFree(gHudUnk9AC);
    }
    if (gRadioAnim != -1) {
        animCancel(gAnimSlots[gRadioAnim].ramp);
    }
    gHudUnk9AC = NULL;
    gRadioAnim = -1;
}

/* 0x8002E920 ovl2 - the music button changes the radio station (8 stations) */
void hudRadioButton(void) {
    s32 pressed = 0;
    s32 p;

    for (p = 0; p <= gNumPlayersM1; p++) {
        if (gMusicButton[p] & contPressed(p, gMusicButton[p])) {
            pressed = 1;
        }
    }
    if (pressed) {
        radioOff();
        if (++gRadioStation >= 8) {
            gRadioStation = 0;
        }
        radioCaption();
        pauseMusicResume();
    }
    if (gHudUnk9AC != NULL) {
        bgDrawSetup(-1, 1);
        drawDL(gHudDLs[24], gRadioCaptionX, gRadioCaptionY[gNumPlayersM1] + 0xFFFB0000, 0, 0x10000);
    }
}

/* 0x8002EA44 ovl2 - show the station name for 1.5 s */
void radioCaption(void) {
    gHudUnk9AC = textCreate(0, gRadioStations[gRadioStation].name, 2);
    textPlace(&gHudUnk9AC->xf, 0x8B0000, gRadioCaptionY[gNumPlayersM1], 0, 1);
    gRadioAnim = animWait(1.5f, NULL, (AnimDoneFunc)radioCaptionDone);
    gRadioCaptionX = ((0x8B - (textWidth(gRadioStations[gRadioStation].name, 0) >> 1)) << 16) + 0xFFF10000;
}

/* 0x8002EB1C ovl2 */
f32 radioCaptionDone(s32 a, s32 b) {
    textFree(gHudUnk9AC);
    gHudUnk9AC = NULL;
    gRadioAnim = -1;
    return -1.0f;
}
