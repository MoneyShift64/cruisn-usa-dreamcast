/*
 * hottimes.c - "HOT TIMES": the high-score tables. Ten licence plates per
 * race, hanging on a wall of machinery, with the best times.
 *  - after a race (trophyStart/trophyFrame, called by the results screen):
 *    each player who made the table drops in a plate and enters initials
 *    (left/right or the stick through A-Z, 0-9, back and END; A/Z/START to
 *    pick), then the plate slides into the table. While choosing, the stick
 *    tilts the plate and the camera can be raised and lowered (C-up/C-down
 *    or the stick); a secret: rock the stick left and right ~960 times with
 *    the camera low and someone peeks in from the side.
 *  - in attract mode (attractInit): a few races' tables in turn.
 * The table shown is the two save devices' tables merged (gHtTable).
 * Original code: front-end overlay, ROM 0x74AA0-0x77EC0 (RAM 0x8003BD10-0x8003F130).
 *
 * The scene is run by animation scripts (gHtAnims) chained one after the
 * other; gHtState says what the frame function does meanwhile:
 *   0 entering initials   1 plate dropping into place   3..6 table scrolls
 *   4 attract: next race   8 done (look around; START leaves)   9 leaving
 */
#include "common.h"
#include "frontend.h"
#include "race.h"

typedef struct HtAnim {                  /* 0x1C bytes, table 0x80044E50 (28) */
    /* 0x00 */ AnimCmd *script;
    /* 0x04 */ f32 seconds;
    /* 0x08 */ AnimCmd *after;           /* started when the script 'after' finishes (NULL: by hand) */
    /* 0x0C */ s32 state;                /* gHtState once it finishes, -1 = unchanged */
    /* 0x10 */ s32 ramp;                 /* while it plays, -1 = not */
    /* 0x14 */ s32 sfx;                  /* played as it starts, -1 = none */
    /* 0x18 */ s32 sfxStop;              /* stopped as it starts, -1 = none */
} HtAnim;

typedef struct HtBoltPos { s32 x, y; } HtBoltPos;

typedef struct ViewerCam { s32 pos[3], angles[3], target[3], useAngles; } ViewerCam;

extern HtAnim     gHtAnims[28];          /* 0x80044E50 ovl3 */
extern s32        gHtBoltsDefault[4];    /* 0x80045160 ovl3 */
extern HtBoltPos  gHtBoltPos[4];         /* 0x80045170 ovl3 */
extern char       gHtCruiseName[];       /* 0x80046020 ovl3 - "SF TO DC" */
extern f64        gHtStickDead;          /* 0x80046030 ovl3 - 0.1 */
extern f64        gHtStickDeadNeg;       /* 0x80046038 ovl3 - -0.1 */
extern f32        gHtBlinkChance;        /* 0x80046040 ovl3 - 0.06 */
extern f32        gHtNoTime;             /* 0x80046044 ovl3 - 1e9 */
extern f64        gHtRepeatBase;         /* 0x80046048 ovl3 - 1.2 */
extern f64        gHtScrollMin;          /* 0x80046050 ovl3 - 13107 */
extern f64        gHtScrollMin2;         /* 0x80046058 ovl3 - 13107 */
extern f64        gHtTitleRate;          /* 0x80046060 ovl3 - 0.01 */
extern f64        gHtTitleMin;           /* 0x80046068 ovl3 - 13107 */
extern f64        gHtEmptyTime;          /* 0x80046070 ovl3 - 1e8: no time shown */
extern f64        gHtDimLevel;           /* 0x80046078 ovl3 - 0.6 */

extern s32        gHtBgY;                /* 0x8004FA20 ovl3 */
extern s32        gHtBgZ;                /* 0x8004FA24 ovl3 */
extern s32        gHtSceneZ;             /* 0x8004FA28 ovl3 */
extern s32        gHtTilt;               /* 0x8004FA2C ovl3 - the girl's stand turning */
extern s32        gHtRoll;               /* 0x8004FA30 ovl3 - stick: the plate rolls */
extern s32        gHtPlateX;             /* 0x8004FA34 ovl3 */
extern s32        gHtStandY;             /* 0x8004FA3C ovl3 */
extern s32        gHtPlateZ;             /* 0x8004FA40 ovl3 */
extern s32        gHtPlateYaw;           /* 0x8004FA44 ovl3 */
extern s32        gHtPlateY;             /* 0x8004FA48 ovl3 */
extern s32        gHtBolts[4];           /* 0x8004FA50 ovl3 - the new plate's bolts going in (-1 = none) */
extern void      *gHtDLs[96];            /* 0x8004FA60 ovl3 - group 0x2EE0 */
extern s32        gHtSceneX;             /* 0x8004FBE0 ovl3 */
extern s32        gHtSceneY;             /* 0x8004FBE4 ovl3 */
extern s32        gHtPlayer;             /* 0x8004FBEC ovl3 */
extern s32        gHtTrack;              /* 0x8004FBF0 ovl3 - 0..13, 14 = Cruise the USA */
extern f32        gHtTime;               /* 0x8004FBF4 ovl3 */
extern s32        gHtRank;               /* 0x8004FBF8 ovl3 - the new entry's row */
extern s32        gHtState;              /* 0x8004FBFC ovl3 */
extern u32        gHtHeapMark;           /* 0x8004FC00 ovl3 */
extern s32        gHtCursor;             /* 0x8004FC04 ovl3 - initial being entered (0..2) */
extern s32        gHtChar;               /* 0x8004FC08 ovl3 - 0..25 A-Z, 26 back, 27..36 0-9, 37 END */
extern s32        gHtRepeat;             /* 0x8004FC0C ovl3 - stick auto-repeat countdown */
extern s32        gHtStripX;             /* 0x8004FC10 ovl3 - the letter strip scrolling */
extern s32       *gHtSceneMtx;           /* 0x8004FC14 ovl3 */
extern s32        gHtDrop[10];           /* 0x8004FC18 ovl3 - attract: each plate still to fall */
extern s32       *gHtAttractList;        /* 0x8004FC40 ovl3 - races to show, ends with -1 */
extern s32        gHtTitleX;             /* 0x8004FC44 ovl3 */
extern TextObj   *gHtTrackText;          /* 0x8004FC48 ovl3 */
typedef void (*HtDoneFunc)(void);
extern HtDoneFunc gHtDone;              /* 0x8004FC4C ovl3 */
extern s32        gHtPeekCount;          /* 0x8004FC50 ovl3 */
extern s32        gHtPeekX;              /* 0x8004FC54 ovl3 */
extern s32        gHtPeekBlink;          /* 0x8004FC58 ovl3 */
extern s32        gHtPeek[2];            /* 0x8004FC60 ovl3 - per player: the secret found */
extern s32        gHtSaved;              /* 0x8004FC68 ovl3 */
extern HiscoreEntry gHtTable[15][10];    /* 0x8004FC70 ovl3 */

extern HiscoreEntry gHiscores[][15][10]; /* 0x801625C0 - per save device */
extern char      *gTrackNames[];         /* 0x80133C54 */
extern ViewerCam  gViewerCams[2];        /* 0x8014EF90 */
extern s32        gBlinkMedium;          /* 0x8014EF74 */
extern u32       *gDlCursor;             /* 0x80147684 */
extern s32       *gMtxCursor;            /* 0x80147688 */

char *timeToStr(char *out, s32 value, s32 min, s32 max); /* 0x80119EEC */
void viewSetupFull(void);
void mtxTranslate(s32 *m, s32 x, s32 y, s32 z);
void mtxFromAngles(s32 *m, s32 ax, s32 ay, s32 az);
s32  randChance(f32 p);
void saveSync(void (*done)(s32));        /* 0x80026670 ovl3 */
void titleInit(s32 skip);

void trophyInit(s32 track);
void trophyStart(s32 track, void (*done)(void));
void trophyFrame(void);
s32  htRank(s32 player, s32 track);
void htMerge(void);
s32  htSameEntry(s32 track, s32 a, s32 b);
void htInsert(void);
void htAnimsReset(void);
void htPlayerBegin(void);
void htAttractFrame(s32 arg);
void htLoad(void);
void htAnimPlay(s32 i);
s32  htAnimDone(s32 slot, s32 ramp, f32 t);
void htInitials(void);
void htInitialsDone(void);
void htStripDraw(void);
void htTitleDraw(void);
void htTableDraw(s32 skipRow, s32 *drops);
void htTimeDraw(f32 time);
void htStandDraw(void);
void htFloorDraw(void);
void htPlateDraw(s32 x, s32 y, s32 z, s32 yaw, s32 row, s32 blink, s32 *bolts);
void htInitialsDraw(s32 row, s32 blink, s32 cursor);
s32  htCharIndex(u8 c);
void htFree(void);
void htAnimsCancel(void);

#define DL_PUSH(w0, w1) { u32 *_g = gDlCursor; gDlCursor = _g + 2; _g[0] = (w0); _g[1] = (u32)(w1); }
#define MTX_NEW(m) { m = gMtxCursor; gMtxCursor = m + 16; }
#define PS_FLAGS(ps) (((u8 *)&(ps)->unk01C)[0])   /* 2: placed in the table, 4: new best (see race/hud.c) */

/* 0x8003BD10 ovl3 - attract mode: show the tables of a list of races (ends with a negative number) */
void attractInit(s32 *tracks) {
    gHtAttractList = tracks;
    htMerge();
    htAnimsReset();
    gHtState = 4;
    gHtTrackText = NULL;
    htLoad();
    gModeFunc = htAttractFrame;
    htAttractFrame(0);
}

/* 0x8003BD6C ovl3 - after a race: the first player who made the table enters initials
 * (none did: done() straight away) */
void trophyStart(s32 track, void (*done)(void)) {
    s32 p;
    char *name;

    gHtTrack = track;
    gHtDone = done;
    gHtSaved = 0;
    gHtPlayer = 0;
    p = 0;
    while (p <= gNumPlayersM1 && gPlayerSetup[p].unk02C >= 10) {
        gHtPlayer = ++p;
    }
    if (p == gNumPlayersM1 + 1) {
        gHtDone();
        return;
    }
    gHtPeek[1] = 0;
    gHtPeek[0] = 0;
    sfxPlay(0x8F, -1);
    musicPlay(0xA5);
    htLoad();
    name = (gHtTrack == 14) ? gHtCruiseName : gTrackNames[gHtTrack];
    gHtTrackText = textCreate(0, name, 2);
    htPlayerBegin();
}

/* 0x8003BE84 ovl3 - every frame of the table after a race (called by the results screen) */
void trophyFrame(void) {
    PadState *pad;
    ViewerCam *cam = &gViewerCams[0];
    s32 *m;
    s32 row, blink, pose, v;
    f32 sx = 0.0f;
    f64 d;

    if (gHtState == 8 && gHtSaved == 0) {
        gHtSaved = 1;
        if (gContReadState != 0) {
            saveSync(NULL);
            return;
        }
    }
    if (gHtState > 0 && (contPressed(gHtPlayer, BTN_START) & BTN_START)) {
        gHtState = 9;
        if (gContReadState != 0) {
            gHtSaved = 1;
            saveSync(NULL);
            return;
        }
    }
    if (gHtState == 9) {
        sfxPlay(0x36, -1);
        gHtPlayer++;
        if (gHtPlayer <= gNumPlayersM1 && gPlayerSetup[gHtPlayer].unk02C < 10) {
            htAnimsCancel();
            htPlayerBegin();                 /* the next player's turn */
        } else {
            htFree();
            gHtDone();
            return;
        }
    }
    if (gHtState == 8) {
        /* look around: raise/lower the camera, roll the plate */
        pad = &gPads[gHtPlayer];
        if ((pad->held & 0x800) || (pad->held & 0x400)) {
            cam->pos[1] = (s32)((f64)cam->pos[1] + 1310720.0 / ((pad->held & 0x800) ? 2.0 : -2.0));
        } else {
            cam->pos[1] = (s32)((f32)cam->pos[1] + 1310720.0f * (f32)(2.0 * (f64)pad->y - 1.0));
        }
        if (cam->pos[1] < 0) {
            cam->pos[1] = 0;
        }
        if (cam->pos[1] >= 0x1CF6666) {
            cam->pos[1] = 0x1CF6665;
        }
        sx = (f32)(2.0 * (f64)pad->x - 1.0);
        gHtRoll = (s32)((f32)gHtRoll - 4000.0f * sx);
        if (gHtPeek[gHtPlayer] == 0) {
            if (cam->pos[1] < 0x140000) {
                d = (f64)sx;
                if (d < -0.5) {
                    gHtPeekCount++;
                } else if (0.5 < d) {
                    gHtPeekCount--;
                }
                if (gHtPeekCount >= 0x3C1) {
                    gHtPeek[gHtPlayer] = 1;
                    gPlayerSetup[gHtPlayer].unk328 = 1;
                    sfxPlay(0x17, -1);
                    sfxPlay(0x17, -1);
                }
            }
        } else {
            v = (s32)((f32)gHtPeekX + 327680.0f * sx);
            gHtPeekX = v;
            if (v < (s32)0x8AD00000) {
                gHtPeekX = v = (s32)0x8AD00000;
            }
            if (v >= 0x75300001) {
                gHtPeekX = 0x75300000;
            }
        }
    }
    if (gHtState == 0) {
        htInitials();
    }
    if (gHtState == 5) {
        gHtState = 6;
        htAnimPlay(gHtRank + 1);
    }

    screenClear(-1, 1);
    viewSetupFull();
    MTX_NEW(m);
    gHtSceneMtx = m;
    mtxTranslate(m, gHtSceneX, gHtSceneY, gHtSceneZ);
    DL_PUSH(0x01040040, gHtSceneMtx - 16);
    if (gHtState >= 4) {
        htTableDraw(gHtRank, NULL);
    }
    if (gHtState > 0) {
        htFloorDraw();
        blink = (gHtState == 8 && (f64)gAnimRamps[gBlinkMedium].t < 0.75) ? 1 : 0;
        row = (gHtState >= 3) ? gHtRank : -1;
        htPlateDraw(gHtPlateX, gHtPlateY, gHtPlateZ, gHtPlateYaw, row, blink, gHtBolts);
    }
    if (gHtState < 4) {
        htStandDraw();
    }
    if (gHtState < 2) {
        htTitleDraw();
    }
    if (gHtState == 8 && gHtPeek[gHtPlayer] != 0 && gHtPeekX >= (s32)0xFC540000 && gHtPeekX < (s32)0xFEAC0001) {
        pose = 0x5D;
        if (gHtPeekBlink != 0) {
            gHtPeekBlink--;
        } else if (gHtStickDead < (f64)sx) {
            pose = 0x5E;
        } else {
            pose = 0x5C;
            if ((f64)sx < gHtStickDeadNeg && randChance(gHtBlinkChance)) {
                gHtPeekBlink = 6;
            }
        }
        DL_PUSH(0xE7000000, 0);
        DL_PUSH(0xFCFFFFFF, 0xFFFCF279);
        drawDL(gHtDLs[pose], gHtPeekX, -0x320000, -0x140000, 0x20000);
        DL_PUSH(0xFC119623, 0xFF2FFFFF);
        DL_PUSH(0xE7000000, 0);
    }
    DL_PUSH(0xBD000000, 0);
}

/* 0x8003C67C ovl3 - after a race: where each finisher's time goes in the table
 * (PlayerSetup.unk02C: row, 10 = not in it) and the badges it earns */
void trophyInit(s32 track) {
    PlayerSetup *ps;
    s32 rank[2];
    s32 p;

    htMerge();
    for (p = 0, ps = gPlayerSetup; p <= gNumPlayersM1; p++, ps++) {
        if (PS_FLAGS(ps) & 1) {
            rank[p] = htRank(p, track);
            if (rank[p] == 0) {
                PS_FLAGS(ps) |= 4;
            } else if (rank[p] < 10) {
                PS_FLAGS(ps) |= 2;
            }
        } else {
            rank[p] = 10;
        }
    }
    if (gNumPlayersM1 != 0) {
        /* both new bests / both 10th: only the faster keeps it */
        if (rank[0] == 0 && rank[1] == 0) {
            if (gPlayerSetup[0].raceTime <= gPlayerSetup[1].raceTime) {
                PS_FLAGS(&gPlayerSetup[1]) = (PS_FLAGS(&gPlayerSetup[1]) & 0xFB) | 2;
            } else {
                PS_FLAGS(&gPlayerSetup[0]) = (PS_FLAGS(&gPlayerSetup[0]) & 0xFB) | 2;
            }
        }
        if (rank[0] == 9 || rank[1] == 9) {
            if (gPlayerSetup[0].raceTime <= gPlayerSetup[1].raceTime) {
                PS_FLAGS(&gPlayerSetup[1]) &= 0xFD;
            } else {
                PS_FLAGS(&gPlayerSetup[0]) &= 0xFD;
            }
        }
    }
    for (p = 0; p <= gNumPlayersM1; p++) {
        gPlayerSetup[p].unk02C = rank[p];
    }
}

/* 0x8003C83C ovl3 - the row a player's race time would take (10: not in the table) */
s32 htRank(s32 player, s32 track) {
    HiscoreEntry *e = gHtTable[track];
    s32 i;

    for (i = 0; i < 10; i++, e++) {
        if (gPlayerSetup[player].raceTime < e->time) {
            break;
        }
    }
    return i;
}

/* 0x8003C8B0 ovl3 - the shown tables: the best ten of both devices' tables, each race */
void htMerge(void) {
    s32 used[2];
    s32 t, k, dev, i, j, other, bit;
    s32 bestI = 0, bestDev = 0;
    f32 best;
    HiscoreEntry *src;

    for (t = 0; t < 15; t++) {
        used[0] = 0;
        used[1] = 0;
        for (k = 0; k < 10; k++) {
            best = gHtNoTime;
            for (dev = 0; dev < 2; dev++) {
                for (i = 0; i < 10; i++) {
                    if (!(used[dev] & (1 << i)) && gHiscores[dev][t][i].time < best) {
                        best = gHiscores[dev][t][i].time;
                        bestI = i;
                        bestDev = dev;
                    }
                }
            }
            bit = 1 << bestI;
            used[bestDev] |= bit;
            /* the same entry on the other device is used up too (as the original:
             * tests the chosen row's bit, and compares device 0's row with device 1's) */
            other = (bestDev == 0);
            for (j = 0; j < 10; j++) {
                if (!(used[other] & bit) && htSameEntry(t, bestI, j)) {
                    used[other] |= 1 << j;
                    break;
                }
            }
            src = &gHiscores[bestDev][t][bestI];
            gHtTable[t][k].time = best;
            for (i = 0; i < 3; i++) {
                gHtTable[t][k].initials[i] = src->initials[i];
            }
        }
    }
}

/* 0x8003CD1C ovl3 - device 0's row a and device 1's row b hold the same entry */
s32 htSameEntry(s32 track, s32 a, s32 b) {
    HiscoreEntry *x = &gHiscores[0][track][a];
    HiscoreEntry *y = &gHiscores[1][track][b];
    s32 i;

    if (x->time != y->time) {
        return 0;
    }
    if (*(u32 *)x->initials == 0xFFFFFFFF && *(u32 *)y->initials == 0xFFFFFFFF) {
        return 1;
    }
    for (i = 0; i < 3; i++) {
        if (x->initials[i] == 0 && y->initials[i] == 0) {
            return 1;
        }
        if (x->initials[i] != y->initials[i]) {
            return 0;
        }
    }
    return 1;
}

/* 0x8003CDDC ovl3 - the new time and initials into both devices' tables */
void htInsert(void) {
    PlayerSetup *ps = &gPlayerSetup[gHtPlayer];
    f32 t = ps->raceTime;
    HiscoreEntry *e;
    s32 dev, i, j;

    for (dev = 0; dev < 2; dev++) {
        e = gHiscores[dev][gHtTrack];
        for (i = 0; i < 10; i++) {
            if (t < e[i].time) {
                break;
            }
        }
        if (i < 10) {
            for (j = 8; j >= i; j--) {
                e[j + 1] = e[j];
            }
            e[i].time = t;
            for (j = 0; j < 3; j++) {
                e[i].initials[j] = ps->initials[j];
            }
#ifdef PORT
            e[i].pad = 1;                /* (port) a player's time, not a built-in one: for the website's leaderboard */
#endif
        }
    }
}

/* 0x8003CFD4 ovl3 */
void htAnimsReset(void) {
    HtAnim *a;

    for (a = gHtAnims; a != &gHtAnims[28]; a++) {
        a->ramp = -1;
    }
    gHtSceneX = 0;
    gHtSceneY = -0x100000;
    gHtSceneZ = 0x73EFBA30;
}

/* 0x8003D02C ovl3 - a player's turn: a new row in the shown table, ready for initials */
void htPlayerBegin(void) {
    PlayerSetup *ps;
    HiscoreEntry *e;
    s32 j, i;
    s8 c;

    contPressed(gHtPlayer, BTN_ALL);
    gHtPeekCount = 0;
    gHtPeekX = -0x1540000;
    gHtPeekBlink = 0;
    ps = &gPlayerSetup[gHtPlayer];
    gHtRank = ps->unk02C;
    gHtTime = ps->raceTime;
    e = gHtTable[gHtTrack];
    if (gHtRank < 9) {
        for (j = 8; j >= gHtRank; j--) {
            e[j + 1] = e[j];
        }
    }
    e = &gHtTable[gHtTrack][gHtRank];
    e->time = gHtTime;
    if (ps->initials[0] == -1) {
        e->initials[0] = 0;
        e->initials[1] = 0;
        e->initials[2] = 0;
        gHtChar = 0x13;
        gHtCursor = 0;
    } else {
        for (i = 0; i < 3; i++) {
            e->initials[i] = ps->initials[i];
        }
        /* the cursor on the first blank (or the last) */
        for (i = 0; ; ) {
            gHtCursor = i;
            gHtChar = htCharIndex(ps->initials[i]);
            c = gPlayerSetup[gHtPlayer].initials[i];
            i++;
            if (c == 0 || i == 3) {
                break;
            }
        }
    }
    htAnimsReset();
    gHtStandY = 0;
    gHtTilt = -0x4000;
    gHtBgY = 0;
    gHtBgZ = 0;
    gHtPlateY = (s32)0xFFA50000;
    gHtPlateZ = 0x100000;
    gHtPlateYaw = -0x3C8C;
    gHtTitleX = 0x1490000;
    gHtBolts[1] = -1;
    gHtBolts[3] = -1;
    gHtStripX = 0;
    gHtRepeat = 0;
    textPlace(&gHtTrackText->xf, 0x8B0000, 0xAA0000, 0, 1);
    gHtState = 0;
    gViewerCams[0].pos[0] = 0;
    gViewerCams[0].pos[1] = 0;
    gViewerCams[0].pos[2] = 0x75300000;
    gViewerCams[0].angles[0] = 0;
    gViewerCams[0].angles[1] = 0;
    gViewerCams[0].angles[2] = 0;
    gViewerCams[0].useAngles = 1;
}

/* 0x8003D358 ovl3 - attract mode, every frame: the plates fall in, a while, the next race */
void htAttractFrame(s32 arg) {
    s32 *m, *d, v, i;

    if (optionsCheck(htAttractFrame, 1) != 0) {
        return;
    }
    if (contPressed(0, BTN_START) & BTN_START) {
        htFree();
        gModeFunc = titleInit;
        gModeArg = 1;
        titleInit(1);
        return;
    }
    if (gHtState == 4) {
        gHtTrack = *gHtAttractList++;
        if (gHtTrack < 0) {
            htFree();
            gModeFunc = titleInit;
            gModeArg = 0;
            titleInit(0);
            return;
        }
        gHtState = 0;
        gHtDrop[1] = 0x5DC0000;
        gHtDrop[0] = 0x5780000;
        for (i = 2, v = 0x6400000; i < 10; i++, v += 0x640000) {
            gHtDrop[i] = v;
        }
        gViewerCams[0].pos[0] = (s32)0xFD800000;
        gViewerCams[0].pos[1] = 0x1000000;
        gViewerCams[0].pos[2] = 0x78500000;
        gViewerCams[0].angles[0] = 0;
        gViewerCams[0].angles[1] = 0;
        gViewerCams[0].angles[2] = 0;
        gViewerCams[0].useAngles = 1;
    }
    if (gHtState == 0) {
        for (d = gHtDrop; d != &gHtDrop[10]; d++) {
            v = *d - 0x500000;
            *d = v;
            if (v < 0) {
                *d = 0;
            }
        }
        if (gHtDrop[9] == 0) {
            gHtState = 1;
            htAnimPlay(0x18);
        }
    }
    screenClear(-1, 1);
    viewSetupFull();
    MTX_NEW(m);
    gHtSceneMtx = m;
    mtxTranslate(m, gHtSceneX, gHtSceneY, gHtSceneZ);
    DL_PUSH(0x01040040, gHtSceneMtx - 16);
    htTableDraw(-1, gHtDrop);
    DL_PUSH(0xBD000000, 0);
}

/* 0x8003D5E0 ovl3 */
void htLoad(void) {
    gHtHeapMark = lowMemOverlayLoad();
    assetLoadGroup(0x2EE0, gHtDLs);
    lowMemOverlayRelease();
}

/* 0x8003D620 ovl3 - play an entry of the animation chain */
void htAnimPlay(s32 i) {
    HtAnim *a = &gHtAnims[i];
    s32 ramp;

    /* (the original calls these with -1 as well, which reads before the sound
     * table; skipped here) */
    if (a->sfxStop != -1) {
        sfxStop(a->sfxStop, -1);
    }
    if (a->sfx != -1) {
        sfxPlay(a->sfx, -1);
    }
    ramp = gAnimSlots[animStart(a->seconds, a->script, NULL, htAnimDone, 0)].ramp;
    a->ramp = ramp;
    gAnimRamps[ramp].user2 = i;
}

/* 0x8003D6C4 ovl3 - an entry finished: its state, and the entries that follow it */
s32 htAnimDone(s32 slot, s32 ramp, f32 t) {
    HtAnim *a = &gHtAnims[gAnimRamps[ramp].user2];
    AnimCmd *script;
    s32 i;

    a->ramp = -1;
    if (a->state != -1) {
        gHtState = a->state;
    }
    script = a->script;
    for (i = 0; i < 28; i++) {
        if (script == gHtAnims[i].after) {
            htAnimPlay(i);
        }
    }
    return 2;
}

/* 0x8003D780 ovl3 - entering initials */
void htInitials(void) {
    s32 left = 0, right = 0;
    s32 rate, old, target, cnt;
    u32 pressed;
    f32 a;
    f64 x, ad;
    u8 ch;
    HiscoreEntry *e;
    s32 i;

    pressed = contPressed(gHtPlayer, BTN_ALL);
    if (pressed & 0x200) {
        left = 1;
    }
    if (pressed & 0x100) {
        right = 1;
    }
    /* the stick: auto-repeat, faster the further it is pushed */
    x = (f64)gPads[gHtPlayer].x;
    a = (f32)(2.0 * x - 1.0);
    ad = (f64)a;
    if (ad < 0.0) {
        a = -a;
        ad = (f64)a;
    }
    if (ad == 0.0) {
        gHtRepeat = 0;
    } else {
        rate = (s32)((gHtRepeatBase - ad) * 8.0);
        if (rate < gHtRepeat) {
            gHtRepeat = rate;
        }
        cnt = gHtRepeat;
        gHtRepeat = cnt - 1;
        if (cnt == 0) {
            gHtRepeat = rate;
            if (x < 0.5) {
                left = 1;
            } else {
                right = 1;
            }
        }
    }
    old = gHtChar;
    if (left) {
        gHtChar = old - 1;
        if (old == 0) {
            gHtChar = 0;
        }
    } else if (right) {
        gHtChar = old + 1;
        if (gHtChar >= 0x26) {
            gHtChar = 0x25;
        }
    }
    if (old != gHtChar) {
        sfxPlay(0x46, -1);
    }
    /* the letter strip slides to the chosen one */
    target = 0x1A50000 - gHtChar * 0xE0000;
    if (gHtStripX < target) {
        gHtStripX = (s32)((f64)gHtStripX + ((f64)(target - gHtStripX) * 0.25 + gHtScrollMin));
        if (target < gHtStripX) {
            gHtStripX = target;
        }
    } else {
        gHtStripX = (s32)((f64)gHtStripX - ((f64)(gHtStripX - target) * 0.25 + gHtScrollMin2));
        if (gHtStripX < target) {
            gHtStripX = target;
        }
    }
    ch = (gHtChar < 27) ? (u8)(gHtChar + 'A') : (u8)(gHtChar + 0x15);
    e = &gHtTable[gHtTrack][gHtRank];
    e->initials[gHtCursor] = ch;
    if (pressed & (BTN_A | BTN_START | BTN_Z)) {
        sfxPlay(0x13, -1);
        if (gHtChar == 0x1A) {               /* back */
            if (gHtCursor != 0) {
                e->initials[gHtCursor] = 0;
                gHtCursor--;
                e->initials[gHtCursor] = 0;
            }
        } else {
            if (gHtChar == 0x25) {           /* END */
                e->initials[gHtCursor] = 0;
            }
            gHtCursor++;
            if (gHtChar == 0x25 || gHtCursor >= 3) {
                for (i = 0; i < 3; i++) {
                    gPlayerSetup[gHtPlayer].initials[i] = e->initials[i];
                }
                htInitialsDone();
                htInsert();
            }
        }
    }
}

/* 0x8003DC04 ovl3 */
void htInitialsDone(void) {
    gHtTrackText->xf.visible = 0;
    gHtState = 1;
    htAnimPlay(0);
}

/* 0x8003DC3C ovl3 - the letters to choose from */
void htStripDraw(void) {
    drawDL(gHtDLs[17], gHtStripX, 0x230000, 0, 0x10000);
    drawDL(gHtDLs[18], gHtStripX, 0x230000, 0, 0x10000);
}

/* 0x8003DC9C ovl3 - the background, and while entering initials HOT TIMES / ENTER INITIALS / PLAYER n */
void htTitleDraw(void) {
    drawDL(gHtDLs[0], 0, gHtBgY, gHtBgZ, 0x10000);
    if (gHtState == 0) {
        bgDrawSetup(-1, 1);
        drawDL(gHtDLs[8], 0x8B0000, 0xB90000, 0, 0x10000);
        drawDL(gHtDLs[7], 0x1D0000, 0xAE0000, 0, 0x10000);
        drawDL(gHtDLs[7], 0xFA0000, 0xAE0000, 0, 0x10000);
        if (gNumPlayersM1 != 0) {
            if ((f64)gAnimRamps[gBlinkMedium].t > 0.25) {
                drawDL(gHtDLs[10 + gHtPlayer], gHtTitleX - 0x2B0000, 0x390000, 0, 0x10000);
            }
            drawDL(gHtDLs[9], gHtTitleX + 0x2B0000, 0x390000, 0, 0x10000);
        } else {
            drawDL(gHtDLs[9], gHtTitleX, 0x390000, 0, 0x10000);
        }
        gHtTitleX = (s32)((f64)gHtTitleX - ((f64)(gHtTitleX - 0x8B0000) * gHtTitleRate + gHtTitleMin));
        if (gHtTitleX < 0x8B0000) {
            gHtTitleX = 0x8B0000;
        }
        viewSetupFull();
        DL_PUSH(0x01040040, gHtSceneMtx - 16);
    }
}

/* 0x8003DEAC ovl3 - the table: ten rows of times, the race's name, the plates
 * (skipRow: the new entry's, drawn by itself; drops: each plate's height still to fall) */
void htTableDraw(s32 skipRow, s32 *drops) {
    s32 bolts[4];
    s32 *m;
    s32 row, y;

    bolts[0] = gHtBoltsDefault[0];
    bolts[1] = gHtBoltsDefault[1];
    bolts[3] = gHtBoltsDefault[3];
    bolts[2] = gHtBoltsDefault[2];
    y = 0x209FFFE;
    for (row = 0; row < 10; row++) {
        MTX_NEW(m);
        mtxTranslate(m, (s32)0xFD800000, y, (s32)0xFF37CCCD);
        DL_PUSH(0x01040040, m - 16);
        DL_PUSH(0x06000000, gHtDLs[15]);
        if (row != 9) {
            DL_PUSH(0x06000000, gHtDLs[16]);
        }
        DL_PUSH(0x06000000, gHtDLs[34 + row]);
        htTimeDraw(gHtTable[gHtTrack][row].time);
        DL_PUSH(0xBD000000, 0);
        y += (s32)0xFFBDCCCD;
    }
    MTX_NEW(m);
    mtxTranslate(m, 0x12D0000, 0x80000, 0);
    DL_PUSH(0x01040040, m - 16);
    DL_PUSH(0x06000000, gHtDLs[14]);
    DL_PUSH(0x06000000, gHtDLs[19 + gHtTrack]);
    DL_PUSH(0xBD000000, 0);
    y = 0x209FFFE;
    for (row = 0; row < 10; row++) {
        MTX_NEW(m);
        mtxTranslate(m, (s32)0xFD800000, y, (s32)0xFF37CCCD);
        DL_PUSH(0x01040040, m - 16);
        if (row != skipRow) {
            htPlateDraw(-0x3C0000, -0x20000, (drops != NULL) ? drops[row] : 0, 0, row, 0, bolts);
        }
        DL_PUSH(0xBD000000, 0);
        y += (s32)0xFFBDCCCD;
    }
}

/* 0x8003E1CC ovl3 - a row's time in big digits */
void htTimeDraw(f32 time) {
    char buf[9];
    s32 x, d, i;

    if (gHtEmptyTime != (f64)time) {
        timeToStr(buf, (s32)(time * 100.0f), 0, 600000);
        x = 0x410000;
        for (i = 0; i < 9; i++) {
            if (buf[i] == ':') {
                x += 0x140000;
            } else {
                d = (u8)(buf[i] - '0');
                if (d >= 0 && d < 10) {
                    drawDL(gHtDLs[44 + d], x, -0x30000, 0x6666, 0x10000);
                    x += 0x180000;
                }
            }
        }
    }
}

/* 0x8003E2EC ovl3 - the girl on her turning stand with the new plate, the letter strip */
void htStandDraw(void) {
    s32 *mDrop, *mTurn, *mUp, *mPlate;
    s32 y;

    MTX_NEW(mDrop);
    mtxTranslate(mDrop, 0, gHtStandY, 0);
    DL_PUSH(0x01040040, mDrop - 16);
    DL_PUSH(0x06000000, gHtDLs[1]);
    MTX_NEW(mTurn);
    mtxFromAngles(mTurn, 0, gHtTilt, 0);
    mTurn[12] = 0;
    mTurn[13] = 0;
    mTurn[14] = 0;
    DL_PUSH(0x01040040, mTurn - 16);
    y = -0xA0000;
    if (gHtStandY < (s32)0xFFE60000) {
        y = (s32)0xFFDC0000 - gHtStandY;
    }
    MTX_NEW(mUp);
    mtxTranslate(mUp, 0, y, 0);
    MTX_NEW(mPlate);
    mtxFromAngles(mPlate, 0, 0x4000, 0);
    mPlate[12] = 0;
    mPlate[13] = (s32)0xFFBC0000;
    mPlate[14] = 0x120000;
    if (gHtState == 0) {
        DL_PUSH(0x06000000, gHtDLs[3]);
        bgDrawSetup(-1, 1);
        htStripDraw();
        viewSetupFull();
        DL_PUSH(0x01040040, gHtSceneMtx - 16);
        DL_PUSH(0x01040040, mDrop - 16);
        DL_PUSH(0x01040040, mTurn - 16);
        DL_PUSH(0x01040040, mUp - 16);
        DL_PUSH(0x06000000, gHtDLs[4]);
        DL_PUSH(0x01040040, mPlate - 16);
        htInitialsDraw(gHtRank, 0, 1);
        DL_PUSH(0xBD000000, 0);
        DL_PUSH(0xBD000000, 0);
    } else if (gHtTilt < -0x800) {
        DL_PUSH(0x06000000, gHtDLs[3]);
        DL_PUSH(0x01040040, mUp - 16);
        DL_PUSH(0x06000000, gHtDLs[5]);
        DL_PUSH(0x06000000, gHtDLs[4]);
        DL_PUSH(0x01040040, mPlate - 16);
        htInitialsDraw(gHtRank, 0, 1);
        DL_PUSH(0xBD000000, 0);
        DL_PUSH(0xBD000000, 0);
    } else {
        DL_PUSH(0x01040040, mUp - 16);
        DL_PUSH(0x06000000, gHtDLs[4]);
        DL_PUSH(0xBD000000, 0);
        DL_PUSH(0x06000000, gHtDLs[3]);
    }
    DL_PUSH(0xBD000000, 0);
    DL_PUSH(0xBD000000, 0);
    y = (s32)0xFFEC0000;
    if (gHtStandY > 0) {
        y = gHtStandY + (s32)0xFFEC0000;
    }
    drawDL(gHtDLs[2], 0, y, 0, 0x10000);
}

/* 0x8003E738 ovl3 - the floor tiles in view */
void htFloorDraw(void) {
    s32 camX = gViewerCams[0].pos[0];
    s32 sceneX = gHtSceneX;
    s32 *m;
    s32 x;

    x = 0x1A20000;
    if (camX < sceneX + 0x1A20000) {
        do {
            if (x < camX + 0x12C0000) {
                MTX_NEW(m);
                mtxFromAngles(m, 0, 0, gHtRoll);
                m[12] = x;
                m[13] = (s32)0xFF920000;
                m[14] = (s32)0xFFEFCCCD;
                DL_PUSH(0x01040040, m - 16);
                DL_PUSH(0x06000000, gHtDLs[6]);
                DL_PUSH(0xBD000000, 0);
                sceneX = gHtSceneX;
            }
            x += (s32)0xFFDA0000;
        } while (camX < x + sceneX);
    }
    sceneX = gHtSceneX;
    x = -0x5580000;
    if (camX >= gHtSceneX + (s32)0xFAA80000) {
        do {
            if (camX + (s32)0xFED40000 < x) {
                MTX_NEW(m);
                mtxFromAngles(m, 0, 0, gHtRoll);
                m[12] = x;
                m[13] = (s32)0xFF920000;
                m[14] = (s32)0xFFEFCCCD;
                DL_PUSH(0x01040040, m - 16);
                DL_PUSH(0x06000000, gHtDLs[6]);
                DL_PUSH(0xBD000000, 0);
                sceneX = gHtSceneX;
            }
            x += 0x260000;
        } while (camX >= x + sceneX);
    }
}

/* 0x8003E988 ovl3 - a plate with its initials (and the bolts going in, if any) */
void htPlateDraw(s32 x, s32 y, s32 z, s32 yaw, s32 row, s32 blink, s32 *bolts) {
    s32 *m;
    s32 i;

    MTX_NEW(m);
    mtxFromAngles(m, 0, yaw, 0);
    m[12] = x;
    m[13] = y;
    m[14] = z;
    DL_PUSH(0x01040040, m - 16);
    DL_PUSH(0x06000000, gHtDLs[12]);
    htInitialsDraw(row, blink, 0);
    if (bolts != NULL) {
        for (i = 0; i < 4; i++) {
            if (bolts[i] != -1) {
                drawDL(gHtDLs[13], gHtBoltPos[i].x, gHtBoltPos[i].y, bolts[i] + 0x3333, 0x10000);
            }
        }
    }
    DL_PUSH(0xBD000000, 0);
}

/* 0x8003EAEC ovl3 - a row's initials on its plate (blink: yellow instead of white;
 * cursor: the letters not being entered dimmed) */
void htInitialsDraw(s32 row, s32 blink, s32 cursor) {
    u8 *c;
    s32 i, dl, blue;
    f32 f, b;
    s32 *m;

    if (row < 0) {
        return;
    }
    blue = (blink != 0) ? 0 : 0xFF;
    c = gHtTable[gHtTrack][row].initials;
    for (i = 0; i < 3; i++, c++) {
        if (*c == 0) {
            return;
        }
        if (*c >= 'A' && *c <= 'Z') {
            dl = *c - 0xB;
        } else if (*c >= '0' && *c <= '9') {
            dl = *c + 0x21;
        } else if (*c == '[') {
            dl = 0x50;
        } else if (*c == ':') {
            dl = 0x5B;
        } else {
            continue;
        }
        b = (f32)blue;
        MTX_NEW(m);
        mtxTranslate(m, i * 0x160000 - 0x160000, 0x40000, 0x3333);
        f = (cursor != 0 && i != gHtCursor) ? (f32)gHtDimLevel : 1.0f;
        DL_PUSH(0xE7000000, 0);
        DL_PUSH(0xFA000000, ((u32)((f32)0xFF * f) << 24) | (((u32)(f * (f32)0xFF) & 0xFF) << 16) |
                            (((u32)(f * b) & 0xFF) << 8) | 0xFF);
        DL_PUSH(0x01040040, m - 16);
        DL_PUSH(0x06000000, gHtDLs[dl]);
        DL_PUSH(0xE7000000, 0);
        DL_PUSH(0xFA000000, 0xFFFFFFFF);
        DL_PUSH(0xBD000000, 0);
    }
}

/* 0x8003EFB4 ovl3 - a character's place in the letter strip */
s32 htCharIndex(u8 c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= '0' && c <= '9') {
        return c - 0x15;
    }
    if (c == '[') {
        return 0x1A;
    }
    return 0x25;
}

/* 0x8003F030 ovl3 */
void htFree(void) {
    s32 p;

    if (gHtTrackText != NULL) {
        textFree(gHtTrackText);
    }
    htAnimsCancel();
    lowMemOverlayLoad();
    highHeapRestore(gHtHeapMark);
    lowMemOverlayRelease();
    for (p = 0; p < gNumControllers; p++) {
        contPressed(p, BTN_ALL);
    }
}

/* 0x8003F0C4 ovl3 */
void htAnimsCancel(void) {
    HtAnim *a;

    for (a = gHtAnims; a != &gHtAnims[28]; a++) {
        if (a->ramp != -1) {
            animCancel(a->ramp);
            a->ramp = -1;
        }
    }
}
