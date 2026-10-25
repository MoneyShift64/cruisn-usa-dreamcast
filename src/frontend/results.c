/*
 * frontend/results.c - after a race: the results sequence, writing the save
 * game, and moving on (next Cruise-the-USA race, Choose Race, CONTINUE?,
 * the congratulations screen).
 * Original code: overlay 3, ROM 0x5F400-0x62B90 (RAM 0x80026670-0x80029920).
 *
 * Sequence (gResultsPhase):
 *   1  the last race frame stays on screen (copied out of the framebuffer)
 *   2  "RACE COMPLETED: <track>" slides in and out
 *   3  USA map: a marker pops up for every track driven so far
 *   4  map finished; when both 2 and 3 are done the save game is written
 *   5  finish times / LOST BY, the cars on turntables
 *   6  trophy girl (8003BD6C) when a player placed and racers were on
 *   7  wait for START
 *   8  CONTINUE? (Cruise the USA, nobody won)
 *   9  fading out to the next race
 *   10 next race's camera preview, then the race overlay starts
 */
#include "common.h"
#include "frontend.h"

typedef f32 (*WaitFunc)(s32 arg);

extern HiscoreEntry gHiscores[][15][10]; /* 0x801625C0 - per save device */
extern s32 gTrackRecords[][14][10];      /* 0x80150A00 - per save device */
extern f32 gAiSkill[2];                  /* 0x801460D0 */
extern s32 gCarAwardMax[5];              /* 0x800402A4 ovl3 - most awards a car can get, per difficulty */

/* main-segment race results */
extern f32 gRaceTime;                    /* 0x80150948 - winner's time */
extern f32 gLostBy;                      /* 0x80150950 */
extern s32 gRaceLoser;                   /* 0x80150958 - 2 players: the player who finished behind, -1 = none */
typedef void (*DrawHookFunc)(void);
extern DrawHookFunc gDrawHook;           /* 0x80150F90 - extra drawing called by the main loop */
extern s32 gCarCamHeight[];              /* 0x80131820 - per car */
extern FrameBuffer gFrameBuffers[3];     /* 0x80147730 */
extern u32 gHeapPtr;                     /* 0x8014658C */
extern char *gTrackNames[];              /* 0x80133C54 */

typedef struct CarObj {                  /* gRenderObjs, 0x2F0 bytes */
    /* 0x000 */ s32 model;
    /* 0x004 */ s32 player;
    /* 0x008 */ u16 flags;
    /* 0x00A */ u8  pad00A[2];
    /* 0x00C */ s32 unk0C;
    /* 0x010 */ s32 unk10;
    /* 0x014 */ s32 unk14;
    /* 0x018 */ s32 spin;
    /* 0x01C */ s32 unk1C;
    /* 0x020 */ u8  pad020[0x2F0 - 0x20];
} CarObj;
extern CarObj gRenderObjs[];             /* 0x8013FA08 */

typedef struct CameraDef {               /* 0x28 bytes, 0x8014EF90 */
    /* 0x00 */ s32 x, y, z;
    /* 0x0C */ s32 unk0C, unk10, unk14;
    /* 0x18 */ s32 rotX, rotY, rotZ;
    /* 0x24 */ s32 unk24;
} CameraDef;
extern CameraDef gViewerCams[2];         /* 0x8014EF90 */

typedef struct MapMarker {               /* 0x24 bytes */
    /* 0x00 */ Xform xf;                 /* xf.visible: shown */
    /* 0x20 */ s32 anim;                 /* running animation slot, -1 = none */
} MapMarker;
typedef struct MapPos { s32 x, y, unk8, unkC; } MapPos;

/* results state (ovl3 bss) */
extern s32 gResultsPhase;                /* 0x80046798 ovl3 */
extern u32 gResultsHeapSave;             /* 0x8004679C ovl3 */
extern u16 *gResultsFrameCopy;           /* 0x800467A0 ovl3 - copy of the last race frame (320x240x16) */
extern u32 gResultsHeapMark;             /* 0x800467A4 ovl3 */
extern s32 gResultsNextTrack;            /* 0x800467A8 ovl3 - track of the next race (14 = Cruise the USA finished) */
extern s32 gResultsUnk7AC;               /* 0x800467AC ovl3 */
extern s32 gResultsPending;              /* 0x800467B0 ovl3 - steps still running before the save */
extern s32 gResultsAnimA;                /* 0x800467B4 ovl3 */
extern s32 gResultsAnimB;                /* 0x800467B8 ovl3 */
extern s32 gResultsTrophy;               /* 0x800467BC ovl3 - racers and traffic were on: trophy girl possible */
extern TextObj *gResultsTimes[2][2];     /* 0x800467D0 ovl3 - per player: caption, time */
extern TextObj *gResultsPressStart;      /* 0x800467E8 ovl3 */
extern TextObj *gResultsTitle;           /* 0x800467EC ovl3 */
extern CarObj gResultsCars[2];           /* 0x800467F0 ovl3 */
extern void *gResultsDLs[];              /* 0x80046740 ovl3 */
extern s32 gResultsCarSpin;              /* 0x80046700 ovl3 */
extern s32 gResultsCarTilt;              /* 0x80046704 ovl3 */
extern s32 gResultsTurntable;            /* 0x80046794 ovl3 */
extern s32 gResultsUnk6E0;               /* 0x800466E0 ovl3 */
extern s32 gResultsUnk6E4;               /* 0x800466E4 ovl3 */
extern s32 gResultsUnk6E8;               /* 0x800466E8 ovl3 */
extern s32 gResultsUnk6F8;               /* 0x800466F8 ovl3 */
extern s32 gResultsUnk6FC;               /* 0x800466FC ovl3 */
extern MapMarker gMapMarkers[14];        /* 0x80046DD0 ovl3 */
extern MapPos gMapPos[14];               /* 0x800401C4 ovl3 */
extern s32 gResultsCarPoses[2];          /* 0x800401BC ovl3 */
extern AnimCmd gFadeOutScript[];              /* 0x8003FFA0 ovl3 */
extern AnimCmd gFadeOutScript2[];             /* 0x8003FFDC ovl3 */
extern AnimCmd gTimes1PScript[];              /* 0x8003FF00 ovl3 */
extern AnimCmd gTimes2PScript[];              /* 0x8003FF3C ovl3 */
extern AnimCmd gTimesScript2[];               /* 0x8003FF78 ovl3 */
extern AnimCmd gIntroScript[];                /* 0x80040004 ovl3 */
extern AnimCmd gStartRaceScript[];            /* 0x80040054 ovl3 */
extern AnimCmd gMarkerScript[];               /* 0x80040144 ovl3 */
extern AnimCmd gTitleInScript[];              /* 0x8004016C ovl3 */
extern AnimCmd gTitleOutScript[];             /* 0x80040194 ovl3 */

void saveBegin(void);                    /* 0x80035D0C ovl3 */
void saveWrite(void (*done)(s32), s32 devices); /* 0x80035A30 ovl3 */
void trophyInit(s32 track);              /* 0x8003C67C ovl3 */
void trophyStart(s32 track, void (*done)(void)); /* 0x8003BD6C ovl3 */
void trophyFrame(void);                  /* 0x8003BE84 ovl3 */
void continueInit(s32 arg);              /* 0x8002EFC0 ovl3 */
s32  continueFrame(void);                /* 0x8002F27C ovl3 - 0 no, 1 yes, else still asking */
void resultsDraw(void);                  /* 0x800282DC ovl3 */
void resultsDrawStats(void);             /* 0x80028998 ovl3 */
void resultsTurntableDraw(void);         /* 0x80027AAC ovl3 */
s32  animAddEvent(s32 ramp, AnimDoneFunc func, f32 at, s32 flags); /* 0x80112B74 */
s32  gfxWaitIdle(void);                  /* 0x80103D88 */
void memCopy32(void *dst, void *src, s32 words); /* 0x80103054 */
void bgLoadImage(u32 dst, void *src);    /* 0x801163A0 */
char *strCopy(char *dst, const char *src); /* 0x80102F4C */
char *strAppend(char *dst, const char *src); /* 0x80102F80 */
TextObj *textCreateTime(s32 layer, s32 value, s32 a, s32 max, s32 b); /* 0x8011A1E8 - "m:ss.cc" from 1/100 s */
u32  heapAlloc(u32 size, s32 align);     /* 0x80102D18 */
void skyDraw(s32 player, s32 yaw, s32 roll); /* 0x80101A68 */
void cameraApply(s32 p, s32 a, s32 b, s32 c, s32 d, s32 e); /* 0x801005BC */
void raceHudSplitScreen(void);           /* 0x80115AC4 */
void trackUnload(void);                  /* 0x80100A84 */

s32  initialsEqual(u8 *a, u8 *b);
void resultsNextRace(void);
s32  resultsNextRaceDone(s32 slot, s32 b, f32 c);
void resultsGrabFrame(void);
void resultsDrawFrame(void);
void resultsShowTimes(void);
s32  resultsTimesDone(s32 slot, s32 b, f32 c);
f32  resultsTrophyStart(s32 arg);
void resultsTrophyDone(void);
void resultsStartRace(void);
s32  resultsStartRaceDone(s32 slot, s32 b, f32 c);
void resultsIntro(void);
s32  resultsTitleIn(s32 slot, s32 b, f32 c);
f32  resultsMapWait(s32 arg);
s32  resultsTitleHold(s32 slot, s32 b, f32 c);
f32  resultsTitleOut(s32 arg);
s32  resultsTitleGone(s32 slot, s32 b, f32 c);
void resultsStepDone(void);
void resultsSaved(s32 arg);
void resultsMapStart(void);
void resultsMapNext(void);
s32  resultsMapNextEvent(s32 slot, s32 b, f32 c);
s32  resultsMarkerDone(s32 slot, s32 b, f32 c);
f32  resultsMapDone(s32 arg);
void resultsFree(void);

/* 0x80026670 ovl3 - copy everything worth saving into the save images and write the
 * devices that changed; then call done (also when nothing needed writing). */
void saveSync(void (*done)(s32)) {
    PlayerSetup *ps;
    SaveSlot *slot;
    HiscoreEntry *mem;
    HiscoreEntry *sv;
    s32 changed;
    s32 dirty;
    s32 dev;
    s32 p;
    s32 t;
    s32 e;
    s32 i;
    u8 cruise;
    u8 opts;

    if (gContReadState != 0) {
        saveBegin();
        dirty = 0;
        for (p = 0, ps = gPlayerSetup; p <= gNumPlayersM1; p++, ps++) {
            if (ps->saveSlot >= 6) {
                continue;
            }
            dev = ps->savePak;
            slot = &gSaveImage[dev].slots[ps->saveSlot];
            changed = 0;
            if ((ps->unk328 == 0) != ((slot->flags & 0x80) == 0) ||
                slot->tracksDone != ps->tracksDone ||
                !initialsEqual(slot->initials, (u8 *)ps->initials) ||
                (p == 0 && (slot->musicVolume != gVolume[0] || slot->sfxVolume != gVolume[1]))) {
                changed = 1;
            }
            for (i = 0; i < 7; i++) {
                if (slot->carAwards[i] != gPlayerSetup[p].carAwards[i]) {
                    changed = 1;
                    break;
                }
            }
            if (gCruiseMode != 0) {
                cruise = slot->cruise;
                opts = slot->opts;
                if (gResultsNextTrack != (cruise >> 4) || (cruise & 7) != ps->car ||
                    slot->totalTime != ps->totalTime || slot->aiSkill != gAiSkill[p] ||
                    (opts & 3) != ps->unk008 || ((opts & 0xC) >> 2) != ps->cameraFollowsCar ||
                    (ps->manual == 0) != ((opts & 0x10) == 0) || gDifficulty != ((opts & 0xE0) >> 5) ||
                    ((cruise & 8) == 0) != (gCruiseMode == 0)) {
                    changed = 1;
                }
            }
            if (!changed) {
                continue;
            }
            dirty |= 1 << dev;
            if (ps->unk328 != 0) {
                slot->flags |= 0x80;
            } else {
                slot->flags &= 0xFF7F;
            }
            slot->tracksDone = ps->tracksDone;
            for (i = 0; i < 7; i++) {
                slot->carAwards[i] = gPlayerSetup[p].carAwards[i];
            }
            for (i = 0; i < 3; i++) {
                slot->initials[i] = gPlayerSetup[p].initials[i];
            }
            if (p == 0) {
                slot->musicVolume = gVolume[0];
                slot->sfxVolume = gVolume[1];
            }
            if (gCruiseMode != 0) {
                if (gResultsNextTrack >= 14) {
                    /* Cruise the USA finished: nothing to resume */
                    slot->opts = 0;
                    slot->cruise = 0;
                    slot->totalTime = 0.0f;
                    slot->aiSkill = 0.0f;
                } else {
                    slot->opts = (ps->manual << 4) | ps->unk008 | (ps->cameraFollowsCar << 2) | (gDifficulty << 5);
                    slot->cruise = ps->car | (gResultsNextTrack << 4) | (gCruiseMode << 3);
                    slot->totalTime = ps->totalTime;
                    slot->aiSkill = gAiSkill[p];
                }
            }
        }

        /* high-score tables and track records of every device */
        for (dev = 0; dev < gContReadState; dev++) {
            for (t = 0; t < 15; t++) {
                for (e = 0; e < 10; e++) {
                    mem = &gHiscores[dev][t][e];
                    sv = &gSaveImage[dev].hiscores[t][e];
                    if (sv->time != mem->time || !initialsEqual(sv->initials, mem->initials)
#ifdef PORT
                        || sv->pad != mem->pad   /* (port) the player-entry mark (hottimes.c htInsert) */
#endif
                        ) {
                        dirty |= 1 << dev;
                        sv->time = mem->time;
#ifdef PORT
                        sv->pad = mem->pad;
#endif
                        for (i = 0; i < 3; i++) {
                            sv->initials[i] = mem->initials[i];
                        }
                    }
                    if (t < 14 && gSaveImage[dev].records[t][e] != gTrackRecords[dev][t][e]) {
                        dirty |= 1 << dev;
                        gSaveImage[dev].records[t][e] = gTrackRecords[dev][t][e];
                    }
                }
            }
        }
        if (dirty != 0) {
            saveWrite(done, dirty);
            return;
        }
    }
    if (done != NULL) {
        done((s32)done);                 /* (the original passes the pointer itself; unused) */
    }
}

/* 0x80026C1C ovl3 - compare 3 initials (a 0 ends both early) */
s32 initialsEqual(u8 *a, u8 *b) {
    s32 i;

    /* original bug: meant to treat two empty (-1) entries as equal, but compares a
     * byte with -1, which is never true */
    if ((s32)a[0] == -1 && (s32)b[0] == -1) {
        return TRUE;
    }
    for (i = 0; i < 3; i++) {
        if (a[i] == 0 && b[i] == 0) {
            return TRUE;
        }
        if (a[i] != b[i]) {
            return FALSE;
        }
    }
    return TRUE;
}

/* 0x80026C98 ovl3 - results mode, first frame (the race overlay switches here after a race).
 * Cruise the USA: unlock the track just driven, load the next one; after the 14th race
 * award a faster car to the winner. */
void resultsInit(s32 arg) {
    PlayerSetup *ps;
    s32 *award;
    s32 next;
    s32 w;
    s32 i;

    gCruiseContinue = 1;
    gResultsAnimB = -1;
    gResultsAnimA = -1;
    gResultsTrophy = (gRacersEnabled != 0) ? (gTrafficEnabled != 0) : 0;
    if (gResultsTrophy) {
        trophyInit(gTrackChoice);
    }
    if (gCruiseMode != 0) {
        next = gTrackChoice + (gRaceWinner >= 0);    /* a player won: on to the next track */
        gResultsNextTrack = next;
        if (next < 14) {
            for (ps = gPlayerSetup; ps <= &gPlayerSetup[gNumPlayersM1]; ps++) {
                ps->tracksDone |= 1 << next;
            }
            trackLoad(next);
            trackPrepare();
        } else {
            /* coast to coast: the faster total time wins an award for its car */
            gPlayerSetup[1].awarded = 0;
            gPlayerSetup[0].awarded = 0;
            gPlayerSetup[1].nextAward = -1;
            gPlayerSetup[0].nextAward = -1;
            w = 0;
            if (gNumPlayersM1 != 0 && gPlayerSetup[1].totalTime < gPlayerSetup[0].totalTime) {
                w = 1;
            }
            ps = &gPlayerSetup[w];
            award = &ps->carAwards[ps->car];
            if (*award < gCarAwardMax[gDifficulty]) {
                (*award)++;
                ps->awarded = 1;
            }
            for (ps = gPlayerSetup; ps <= &gPlayerSetup[gNumPlayersM1]; ps++) {
                for (i = 0; i < 5; i++) {
                    if (ps->carAwards[ps->car] < gCarAwardMax[i]) {
                        ps->nextAward = i;
                        break;
                    }
                }
            }
        }
    } else {
        gResultsNextTrack = gTrackChoice;
    }
    gResultsHeapSave = gHeapPtr;
    gResultsFrameCopy = (u16 *)heapAlloc(320 * 240 * 2, 0x40);
    resultsGrabFrame();
    gResultsHeapMark = lowMemOverlayLoad();
    assetLoadGroup(0x2DCF, gResultsDLs);
    lowMemOverlayRelease();
    resultsIntro();
    gModeFunc = resultsFrame;
    resultsFrame(0);
}

/* 0x80026F7C ovl3 - results mode, every frame */
void resultsFrame(s32 arg) {
    CameraDef *cam;
    PlayerSetup *ps;
    CarObj *obj;
    s32 p;

    if (gResultsPhase == 6) {
        trophyFrame();
        return;
    }
    if (gResultsPhase >= 2 && gResultsPhase < 9 && optionsCheck(resultsFrame, gCruiseMode == 0)) {
        return;
    }
    if (gResultsPhase == 1) {
        resultsDrawFrame();
    } else if (gResultsPhase > 0 && gResultsPhase < 10) {
        screenClear(-1, 1);
    } else if (gResultsPhase == 10) {
        /* camera previews for the next race */
        for (p = 0, cam = gViewerCams, ps = gPlayerSetup, obj = gRenderObjs; p <= gNumPlayersM1;
             p++, cam++, obj++, ps++) {
            skyDraw(p, 0xC000, 0);
            cam->x = 0;
            cam->unk0C = 0;
            cam->unk14 = 0;
            cam->y = (obj->unk10 + gCarCamHeight[ps->car]) - (s32)((f64)ps->camHeight * 65536.0 * 0.2);
            cam->z = (s32)((f64)ps->camDist * 65536.0 * 0.2) + 0xFD500000;
            cam->unk24 = 1;
            cam->unk10 = (f32)obj->unk1C * ((f32)ps->viewAtEdge * 0.7f) + (f32)(gNumPlayersM1 * 1000);
            viewBegin(p);
            cameraApply(p, -obj->unk0C, 0, 0, 1, 1);
        }
    }
    if (gResultsPhase == 10 && gNumPlayersM1 != 0) {
        raceHudSplitScreen();
        bgDrawSetup(0, 1);
        drawDL(gResultsDLs[16], 0x10B0000, 0x580000, 0, 0x10000);
        bgDrawSetup(1, 1);
        drawDL(gResultsDLs[17], 0x10B0000, 0x580000, 0, 0x10000);
    }
    resultsDraw();
    if (gResultsPhase >= 5) {
        resultsDrawStats();
    }
    if (gResultsPhase == 7 &&
        ((contPressed(0, BTN_START) & BTN_START) ||
         (gNumPlayersM1 != 0 && (contPressed(1, BTN_START) & BTN_START)))) {
        gTrackChoice = gResultsNextTrack;
        gResultsPressStart->xf.visible = 0;
        for (p = 0; p <= gNumPlayersM1; p++) {
            playerSetupView(p);
        }
        if (gCruiseMode != 0) {
            if (gRaceWinner >= 0) {
                if (gResultsNextTrack >= 14) {
                    resultsFree();
                    gModeFunc = congratsInit;      /* coast to coast! */
                    gModeArg = 0;
                } else {
                    resultsNextRace();
                }
            } else {
                gResultsPhase = 8;                 /* lost: CONTINUE? */
                continueInit(0);
            }
        } else {
            resultsFree();
            gModeArg = 1;
            gModeFunc = raceSelectInit;            /* single race: back to Choose Race */
        }
    }
    if (gResultsPhase == 8) {
        switch (continueFrame()) {
            case 0:                                /* no: back to the title */
                musicStop();
                resultsFree();
                trackUnload();
                gModeArg = 0;
                gModeFunc = frontEndEnter;
                break;
            case 1:                                /* yes: race this track again */
                resultsNextRace();
                break;
        }
    }
}

/* 0x80027430 ovl3 - fade out towards the next race */
void resultsNextRace(void) {
    sfxPlay(0x94, -1);
    gResultsPhase = 9;
    animStart(0.8f, gFadeOutScript, NULL, resultsNextRaceDone, 0);
    animStart(0.7f, gFadeOutScript2, NULL, NULL, 4);
}

/* 0x800274A8 ovl3 */
s32 resultsNextRaceDone(s32 slot, s32 b, f32 c) {
    resultsStartRace();
    return 2;
}

/* 0x800274D8 ovl3 - keep a copy of the last race frame */
void resultsGrabFrame(void) {
#ifdef PORT
    /* the Dreamcast draws straight to the PowerVR: there is no N64 framebuffer to copy
     * (the results screen shows a black background for now) */
    gfxWaitIdle();
    return;
#endif
    s32 fb = gfxWaitIdle();
    void *dst = (u8 *)gResultsFrameCopy + 0x80000000;
    void *src = (u8 *)gFrameBuffers[fb].addr + 0xE0000000;

    osInvalDCache(src, 320 * 240 * 2);
    memCopy32(dst, src, 320 * 240 * 2 / 4);
}

/* 0x80027548 ovl3 */
void resultsDrawFrame(void) {
#ifdef PORT
    screenClear(-1, 1);
    return;
#endif
    bgLoadImage(0x02000000, gResultsFrameCopy);
}

/* 0x80027570 ovl3 - phase 5: finish times and the cars on turntables */
void resultsShowTimes(void) {
    TextObj **pair;
    TextObj **t;
    PlayerSetup *ps;
    CarObj *car;
    u8 flags;
    s32 i;
    s32 p;

    gResultsPhase = 5;
    if (gNumPlayersM1 != 0) {
        gResultsUnk6E0 = 0x820000;
    } else {
        gResultsUnk6E0 = 0xFA0000;
    }
    gResultsUnk6E8 = 0xAA0000;
    gResultsUnk7AC = 0;
    for (p = 0, car = gResultsCars, ps = gPlayerSetup, pair = gResultsTimes[0]; p <= gNumPlayersM1;
         p++, car++, ps++, pair += 2) {
        contPressed(p, BTN_ALL);
        renderObjInit(car, ps->car, 0, 0x110, 0, 0xFFCE0000, 0, gResultsCarPoses[gNumPlayersM1]);
        flags = ((u8 *)ps)[0x1C];
        car->player = p;
        if (!(flags & 8)) {
            car->flags |= 0x20;
        }
        if (flags & 8) {
            car->spin = 0xC000;
        } else {
            car->spin = (gNumPlayersM1 != 0) ? 0xC000 : 0x9C40;
        }
        if (gNumPlayersM1 != 0 && gRaceLoser >= 0) {
            if (flags & 1) {
                pair[0] = textCreate(0, "FINISH TIME:", 1);
                pair[1] = textCreateTime(3, gRaceTime * 100.0f, 0, 599999, 0);
            } else {
                pair[0] = textCreate(0, "LOST BY:", 1);
                pair[1] = textCreateTime(3, gLostBy * 100.0f, 0, 599999, 0);
            }
        } else {
            pair[0] = textCreate(0, "FINISH TIME:", 1);
            if (((u8 *)ps)[0x1C] & 1) {
                pair[1] = textCreateTime(3, gRaceTime * 100.0f, 0, 599999, 0);
            } else {
                pair[1] = textCreate(0, " ---", 0);
            }
        }
        for (i = 0, t = pair; i < 2; i++, t++) {
            (*t)->flags |= 2;
            textPlace(&(*t)->xf, 0x3E80000, 0x3E80000, 0, 1);
        }
    }
    gResultsPressStart = textCreate(1, "PRESS START", 2);
    textPlace(&gResultsPressStart->xf, 0x3E80000, 0x3E80000, 0, 0);
    gResultsPressStart->flags |= 2;
    gDrawHook = resultsTurntableDraw;
    gResultsTurntable = 0;
    gResultsCarSpin = 0;
    gResultsCarTilt = 0;
    animStart(0.8f, (gNumPlayersM1 != 0) ? gTimes2PScript : gTimes1PScript, NULL, resultsTimesDone, 0);
    animStart(0.7f, gTimesScript2, NULL, NULL, 4);
    sfxPlay(0x95, -1);
}

/* 0x80027970 ovl3 - times are in: trophy girl if a player placed (and racers were on) */
s32 resultsTimesDone(s32 slot, s32 b, f32 c) {
    s32 p1Placed;
    s32 p2Placed;

    sfxPlay(0x4E, -1);
    p1Placed = ((u8 *)&gPlayerSetup[0])[0x1C] & 6;
    p2Placed = gNumPlayersM1 != 0;
    if (p2Placed) {
        p2Placed = (((u8 *)&gPlayerSetup[1])[0x1C] & 6) != 0;
    }
    if (gResultsTrophy && (p1Placed || p2Placed)) {
        animWait(3.0f, NULL, (AnimDoneFunc)resultsTrophyStart);
        return 2;
    }
    gResultsPhase = 7;
    gResultsPressStart->xf.visible = 1;
    return 2;
}

/* 0x80027A2C ovl3 */
f32 resultsTrophyStart(s32 arg) {
    gResultsPhase = 6;
    trophyStart(gTrackChoice, resultsTrophyDone);
    return -1.0f;
}

/* 0x80027A70 ovl3 */
void resultsTrophyDone(void) {
    musicPlay(0xA2);
    gResultsPhase = 7;
    gResultsPressStart->xf.visible = 1;
}

/* 0x80027C34 ovl3 - start the next race after a short fade */
void resultsStartRace(void) {
    gResultsPhase = 10;
    if (gRacersEnabled != 0) {
        sfxPlay(0x7D, -1);
    }
    animStart(2.0f, gStartRaceScript, NULL, resultsStartRaceDone, 0);
}

/* 0x80027C94 ovl3 - -> race overlay */
s32 resultsStartRaceDone(s32 slot, s32 b, f32 c) {
    gResultsPhase = 0;
    resultsFree();
    gModeArg = 0;
    gOverlayRequest = OVERLAY_REQUEST(OVL_RACE);
    gModeFunc = RACE_START_MODE;
    return 2;
}

/* 0x80027CEC ovl3 - phase 1: hold the last race frame for 1.5 s */
void resultsIntro(void) {
    gResultsUnk6E4 = 100;
    gResultsUnk6FC = 0;
    gResultsUnk6F8 = 0;
    gResultsPhase = 1;
    gResultsPending = 2;
    animStart(1.5f, gIntroScript, NULL, resultsTitleIn, 0);
}

/* 0x80027D60 ovl3 - phase 2: "RACE COMPLETED:\n<track>" slides in; the map starts 0.2 s later */
s32 resultsTitleIn(s32 slot, s32 b, f32 c) {
    char buf[60];
    TextObj *t;

    gResultsPhase = 2;
    gHeapPtr = gResultsHeapSave;         /* the frame copy is no longer needed */
    strCopy(buf, "RACE COMPLETED:\n");
    strAppend(buf, gTrackNames[gTrackChoice]);
    t = textCreate(0, buf, 2);
    gResultsTitle = t;
    textPlace(&t->xf, 0x3E80000, 0x1B0000, 0, 1);
    animStart(0.5f, gTitleInScript, &gResultsTitle->xf, resultsTitleHold, 0);
    animWait(0.2f, NULL, (AnimDoneFunc)resultsMapWait);
    return 2;
}

/* 0x80027E50 ovl3 */
f32 resultsMapWait(s32 arg) {
    resultsMapStart();
    return -1.0f;
}

/* 0x80027E7C ovl3 */
s32 resultsTitleHold(s32 slot, s32 b, f32 c) {
    animWait(1.0f, NULL, (AnimDoneFunc)resultsTitleOut);
    return 2;
}

/* 0x80027EBC ovl3 */
f32 resultsTitleOut(s32 arg) {
    animStart(0.5f, gTitleOutScript, &gResultsTitle->xf, resultsTitleGone, 0);
    return -1.0f;
}

/* 0x80027F0C ovl3 */
s32 resultsTitleGone(s32 slot, s32 b, f32 c) {
    resultsStepDone();
    return 2;
}

/* 0x80027F3C ovl3 - title and map both done: write the save game, then show the times */
void resultsStepDone(void) {
    if (--gResultsPending == 0) {
        saveSync(resultsSaved);
    }
}

/* 0x80027F78 ovl3 */
void resultsSaved(s32 arg) {
    resultsShowTimes();
}

/* 0x80027F98 ovl3 - phase 3: the USA map */
void resultsMapStart(void) {
    MapMarker *m;

    gResultsPhase = 3;
    sfxStop(0x20, -1);
    sfxPlay(0xC, -1);
    for (m = gMapMarkers; m != &gMapMarkers[14]; m++) {
        m->xf.visible = 0;
        m->anim = -1;
    }
    resultsMapNext();
}

/* 0x8002803C ovl3 - pop up the next marker (all tracks so far in Cruise the USA, else just this one) */
void resultsMapNext(void) {
    MapMarker *m;
    s32 first = 0;
    s32 last;
    s32 i;
    s32 s;

    if (gCruiseMode != 0) {
        last = gTrackChoice;
    } else {
        last = gTrackChoice;
        first = last;
    }
    for (i = first, m = &gMapMarkers[i]; i <= last && m->xf.visible != 0; i++, m++) {
    }
    if (i <= last) {
        m = &gMapMarkers[i];
        sfxPlay(0x47, -1);
        textPlace(&m->xf, gMapPos[i].x, gMapPos[i].y, 0, 1);
        s = animStart(1.0f, gMarkerScript, (void *)(i * 4), resultsMarkerDone, 0);
        m->anim = s;
        gAnimSlots[s].user = i;
        animAddEvent(gAnimSlots[s].ramp, resultsMapNextEvent, 0.35f, 1);   /* next one starts 0.35 s in */
    }
}

/* 0x80028180 ovl3 */
s32 resultsMapNextEvent(s32 slot, s32 b, f32 c) {
    resultsMapNext();
    return 1;
}

/* 0x800281B0 ovl3 - a marker finished; when all are, phase 4 */
s32 resultsMarkerDone(s32 slot, s32 b, f32 c) {
    MapMarker *m;
    s32 first;
    s32 last;
    s32 i;

    if (gCruiseMode != 0) {
        last = gTrackChoice;
        first = 0;
    } else {
        last = gTrackChoice;
        first = last;
    }
    gMapMarkers[gAnimSlots[slot].user].anim = -1;
    for (i = first, m = &gMapMarkers[first]; i <= last && m->xf.visible != 0 && m->anim < 0; i++, m++) {
    }
    if (i > last) {
        gResultsPhase = 4;
        animWait(0.2f, NULL, (AnimDoneFunc)resultsMapDone);
    }
    return 2;
}

/* 0x800282B0 ovl3 */
f32 resultsMapDone(s32 arg) {
    resultsStepDone();
    return -1.0f;
}

/* 0x80029804 ovl3 - free everything the results screen created */
void resultsFree(void) {
    TextObj **t;
    s32 p;
    s32 i;

    lowMemOverlayLoad();
    highHeapRestore(gResultsHeapMark);
    lowMemOverlayRelease();
    for (p = 0; p <= gNumPlayersM1; p++) {
        for (i = 0, t = gResultsTimes[p]; i < 2; i++, t++) {
            textFree(*t);
        }
    }
    textFree(gResultsPressStart);
    textFree(gResultsTitle);
    if (gResultsAnimA >= 0) {
        animCancel(gAnimSlots[gResultsAnimA].ramp);
    }
    if (gResultsAnimB >= 0) {
        animCancel(gAnimSlots[gResultsAnimB].ramp);
    }
}
