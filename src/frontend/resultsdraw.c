/*
 * resultsdraw.c - the results screen's drawing (see results.c for its logic):
 * the USA map unfolding in four panels with a marker per track driven, each
 * player's car on a turntable with the place, finish time and average speed,
 * the course record, and PRESS START. In a 2-player race the loser's car may
 * be knocked off its turntable.
 * Original code: front-end overlay, RAM 0x80027AAC and 0x800282DC-0x80029804.
 */
#include "common.h"
#include "frontend.h"
#include "race.h"

typedef struct ViewerCam { s32 pos[3], angles[3], target[3], useAngles; } ViewerCam;
typedef struct Vp16 { s16 v[8]; } Vp16;
typedef struct MapMarker {               /* 0x24 bytes */
    /* 0x00 */ Xform xf;
    /* 0x20 */ s32 anim;
} MapMarker;
typedef struct MapPos { s32 x, y, x2, y2; } MapPos;  /* a marker slides from (x, y) to (x2, y2) */

extern ViewerCam gViewerCams[2];         /* 0x8014EF90 */
extern HiscoreEntry gHiscores[][15][10]; /* 0x801625C0 - per save device */
extern f32 gRaceTime;                    /* 0x80150948 */
extern u32 *gDlCursor;                   /* 0x80147684 */
extern s32 *gMtxCursor;                  /* 0x80147688 */

extern s32 gResultsPhase;                /* 0x80046798 ovl3 */
extern s32 gResultsUnk7AC;               /* 0x800467AC ovl3 - pulse of the award badge */
extern s32 gResultsAnimA;                /* 0x800467B4 ovl3 */
extern s32 gResultsAnimB;                /* 0x800467B8 ovl3 */
extern TextObj *gResultsAvg[2][2];       /* 0x800467C0 ovl3 - per player: "AVG MPH:", the speed */
extern TextObj *gResultsTimes[2][2];     /* 0x800467D0 ovl3 - per player: caption, time */
extern TextObj *gResultsRecord[2];       /* 0x800467E0 ovl3 - the record's time and initials */
extern TextObj *gResultsPressStart;      /* 0x800467E8 ovl3 */
extern RenderObj gResultsCars[2];        /* 0x800467F0 ovl3 */
extern void *gResultsDLs[];              /* 0x80046740 ovl3 */
extern s32 gResultsCarSpin;              /* 0x80046700 ovl3 */
extern s32 gResultsCarTilt;              /* 0x80046704 ovl3 */
extern s32 gMapMarkerT[14];              /* 0x80046708 ovl3 - 0..1000: a marker's slide */
extern s32 gResultsCrashPlayer;          /* 0x80046790 ovl3 - whose car comes off its turntable */
extern s32 gResultsTurntable;            /* 0x80046794 ovl3 - a car is coming off */
extern s32 gResultsUnk6E0;               /* 0x800466E0 ovl3 - the panels' slide */
extern s32 gResultsUnk6E4;               /* 0x800466E4 ovl3 - the map's brightness (0..100) */
extern s32 gMapFold;                     /* 0x800466EC ovl3 - the map panels' fold angle */
extern s32 gMapYaw;                      /* 0x800466F0 ovl3 */
extern s32 gMapZ;                        /* 0x800466F4 ovl3 */
extern s32 gResultsUnk6F8;               /* 0x800466F8 ovl3 */
extern s32 gResultsUnk6FC;               /* 0x800466FC ovl3 */
extern MapMarker gMapMarkers[14];        /* 0x80046DD0 ovl3 */
extern MapPos gMapPos[14];               /* 0x800401C4 ovl3 */
extern s32 gResultsCarPoses[2];          /* 0x800401BC ovl3 */
extern Vp16 gResultsVp;                  /* 0x800402B8 ovl3 */
extern AnimCmd gResultsCrashScript[];    /* 0x800400CC ovl3 */
extern AnimCmd gResultsCrashScript2[];   /* 0x80040108 ovl3 */

void mtxFromAngles(s32 *m, s32 ax, s32 ay, s32 az);  /* 0x80114824 */
s32  sinLookup(s32 a);                   /* 0x80113F38 */
s32  cosLookup(s32 a);                   /* 0x80113FDC */
void viewportSet(Vp16 *vp, s32 dx, s32 dy); /* 0x80115C10 */
void viewBegin(s32 racer);               /* 0x80116C10 */
void bgDrawSetupFull(void);              /* 0x80116FB8 */
void xformsUpdate(Xform *xf);            /* 0x8011508C */
TextObj *textCreateNum(s32 font, s32 value, s32 width, s32 pad, s32 style);  /* 0x8011A1A4 */
TextObj *textCreateTime(s32 font, s32 value, s32 min, s32 max, s32 style);   /* 0x8011A1E8 */
void hudDrawPlace(s32 place, s32 x, s32 y);   /* 0x8011A69C */
s32  randChance(f32 p);                  /* 0x801031DC */
void drawDLSimple(void *dl, s32 x, s32 y); /* 0x80026534 ovl3 */

void resultsMapShade(s32 *out, s32 level);
s32  resultsAvgSpeed(s32 p, f32 time);
void resultsCarDraw(s32 p, s32 x, s32 y);
s32  resultsCrashDone(s32 slot, s32 b, f32 c);
void resultsPressStartDraw(s32 x, s32 y);
void resultsRecordDraw(s32 x, s32 y, s32 slide);
void resultsTimeBar1P(s32 x, s32 y);
void resultsTimeBars2P(s32 x0, s32 y0, s32 x1, s32 y1);
void resultsAvgCreate(s32 p);
void resultsAvgFree(s32 p);
void resultsStats1P(s32 x, s32 y);
void resultsStats2P(s32 p, s32 x, s32 y);

#define DL_PUSH(w0, w1) { u32 *_g = gDlCursor; gDlCursor = _g + 2; _g[0] = (w0); _g[1] = (u32)(w1); }
#define MTX_NEW(m) { m = gMtxCursor; gMtxCursor = m + 16; }
#define MTX_PUSH(m) DL_PUSH(0x01040040, (u32)((m) - 16))
#define PS_FLAGS(ps) (((u8 *)&(ps)->unk01C)[0])     /* 2/4 award badges, 8 = won (see race/hud.c) */

/* 0x80027AAC ovl3 - (gDrawHook) the turntable under the car coming off it: tilt about the wheels */
void resultsTurntableDraw(void) {
    s32 h = gCarModelInfo[gPlayerSetup[gResultsCrashPlayer].car].wheelY[0];
    f32 s = (f32)((f32)sinLookup((gResultsCarSpin + 8) & 0xFFFF) / -16384.0);
    f32 c = (f32)((f32)cosLookup((gResultsCarSpin + 8) & 0xFFFF) / 16384.0);
    f32 fh = (f32)h;
    s32 *m;

    MTX_NEW(m);
    mtxFromAngles(m, gResultsCarTilt, 0, gResultsCarSpin);
    m[12] = (s32)(fh * s);
    m[14] = 0;
    m[13] = (s32)(fh * c - fh);
    MTX_PUSH(m);
}

/* 0x800282DC ovl3 - the USA map (four panels folding out) and the track markers */
void resultsDraw(void) {
    s32 shade[2];
    s32 *m;
    s32 i, c;
    f32 fold, t;
    MapMarker *mk;
    MapPos *pos;

    bgDrawSetupFull();
    fold = (f32)((f32)tblCos(gMapFold) / 16384.0);
    MTX_NEW(m);
    mtxFromAngles(m, 0, gMapYaw, 0);
    m[12] = gResultsUnk6F8 + 0x17CE6;
    m[13] = gResultsUnk6FC + 0x493E;
    m[14] = gMapZ;
    MTX_PUSH(m);
    resultsMapShade(shade, gResultsUnk6E4);
    for (i = 0; i < 4; i++) {
        DL_PUSH(0xE7000000, 0);
        c = shade[i & 1] & 0xFF;
        DL_PUSH(0xFA000000, (c << 24) | (c << 16) | (c << 8) | 0xFF);
        MTX_NEW(m);
        mtxFromAngles(m, (i & 1) ? -gMapFold : gMapFold, 0, 0);
        m[13] = 0;
        m[14] = 0;
        m[12] = (s32)((f32)(i * 2 - 3) * (f32)(fold * 2621440.0));
        MTX_PUSH(m);
        DL_PUSH(0x06000000, gResultsDLs[i]);
        DL_PUSH(0x06000000, gResultsDLs[i + 4]);
        DL_PUSH(0xBD000000, 0);
    }
    DL_PUSH(0xBD000000, 0);
    if (gResultsPhase >= 3 && gResultsPhase < 8) {
        DL_PUSH(0xE7000000, 0);
        c = (s32)(f32)((f32)gResultsUnk6E4 * 2.55) & 0xFF;
        DL_PUSH(0xFA000000, (c << 24) | (c << 16) | (c << 8) | 0xFF);
        bgDrawSetup(-1, 1);
        for (i = 0; i <= gTrackChoice; i++) {
            mk = &gMapMarkers[i];
            if (mk->xf.visible != 0) {
                pos = &gMapPos[i];
                t = (f32)((f32)gMapMarkerT[i] / 1000.0);
                mk->xf.x = (s32)((f32)pos->x + (f32)(pos->x2 - pos->x) * t);
                mk->xf.y = (s32)((f32)pos->y + (f32)(pos->y2 - pos->y) * t);
                mk->xf.unk14 = (s32)(-42000.0f * t + 42000.0f);
                mk->xf.scale = (s32)(-163840.0f * t + 196608.0f);
                xformsUpdate(&mk->xf);
                DL_PUSH(0x06000000, gResultsDLs[8]);
                DL_PUSH(0xBD000000, 0);
            }
        }
    }
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xFA000000, 0xFFFFFFFF);
}

/* 0x800287C8 ovl3 - the two panel shades as they fold (light facing the viewer) */
void resultsMapShade(s32 *out, s32 level) {
    f32 l = (f32)((f32)level / 100.0);
    f32 v;
    s32 k;

    for (k = 0; k < 2; k++) {
        v = (f32)((f32)tblSin((k * 2 - 1) * gMapFold + 10000) / 16384.0);
        if (v >= 0.0f) {
            out[k] = (s32)((205.0f * v + 50.0f) * l);
        } else {
            out[k] = (s32)(50.0f * l);
        }
    }
}

/* 0x80028904 ovl3 - a player's average speed over the race (mph or km/h) */
s32 resultsAvgSpeed(s32 p, f32 time) {
    f32 v = (f32)(((f64)gPlayerSetup[p].odometer / 5280.0) / ((f64)time / 3600.0));

    if (gUnitsKph != 0) {
        return (s32)((f64)v * 1.609);
    }
    return (s32)v;
}

/* 0x80028998 ovl3 - the cars, times, record and PRESS START */
void resultsDrawStats(void) {
    s32 p;

    for (p = 0; p <= gNumPlayersM1; p++) {
        if (gResultsTurntable == 0 || p != gResultsCrashPlayer) {
            gResultsCars[p].steerAngle = (s32)((1.0 - 2.0 * (f64)gPads[p].xRaw) * 6000.0);
        }
    }
    if (gResultsPhase < 7) {
        gResultsUnk7AC += 5000;
    } else {
        gResultsUnk7AC = 0;
    }
    if (gNumPlayersM1 != 0) {
        resultsStats2P(0, 0, gResultsUnk6E0 + 0xC10000);
        resultsStats2P(1, 0, 0x6A0000 - gResultsUnk6E0);
        resultsTimeBars2P(0, gResultsUnk6E0 + 0xC10000, 0, 0x6A0000 - gResultsUnk6E0);
        resultsRecordDraw(0x910000, 0x5F0000, gResultsUnk6E0);
    } else {
        resultsStats1P(0, gResultsUnk6E0 + 0xAD0000);
        resultsTimeBar1P(0, gResultsUnk6E0 + 0xC10000);
        resultsRecordDraw(0x910000, 0x170000, gResultsUnk6E0);
    }
    drawDLSimple(gResultsDLs[18], 0x200000, 0x60000 - gResultsUnk6E0);
    resultsPressStartDraw(0x8B0000, 0xF0000 - gResultsUnk6E0);
}

/* 0x80028B74 ovl3 - a player's car on its turntable; 2 players: the loser's may come off */
void resultsCarDraw(s32 p, s32 x, s32 y) {
    ViewerCam *cam = &gViewerCams[p];
    RenderObj *obj;
    u8 flags;

    cam->useAngles = 0;
    cam->pos[0] = 0;
    cam->pos[1] = 0x01900000;
    cam->pos[2] = 0;
    cam->target[0] = 0;
    cam->target[1] = 0;
    cam->target[2] = gResultsCarPoses[gNumPlayersM1];
    viewBegin(-1);
    viewportSet(&gResultsVp, (s16)((x - 0x8C0000) >> 14), (s16)((y - 0x640000) >> 14));
    obj = &gResultsCars[p];
    renderObjDraw(obj);
    if (gResultsPhase != 7) {
        return;
    }
    flags = PS_FLAGS(&gPlayerSetup[p]);
    if (!(flags & 8) && gResultsTurntable != 0) {
        return;
    }
    obj->angleZ -= 0x100;
    if (gNumPlayersM1 == 0) {
        return;
    }
    if (gRaceWinner == (p == 0) && obj->angleZ < 0xB060 && !(flags & 4)) {
        if (randChance(0.25f) != 0) {
            gResultsCrashPlayer = p;
            gResultsTurntable = 1;
            gResultsAnimA = animStart(0.1f, gResultsCrashScript, &gResultsCars[p], NULL, 4);
            gResultsAnimB = animStart(2.0f, gResultsCrashScript2, NULL, resultsCrashDone, 0);
            sfxPlay(0x26, -1);
            return;
        }
        obj->angleZ += 0x10000;
    }
}

/* 0x80028DB8 ovl3 */
s32 resultsCrashDone(s32 slot, s32 b, f32 c) {
    sfxPlay(0x6A, -1);
    gResultsAnimB = -1;
    gResultsAnimA = -1;
    return 2;
}

/* 0x80028E04 ovl3 */
void resultsPressStartDraw(s32 x, s32 y) {
    gResultsPressStart->xf.x = x;
    gResultsPressStart->xf.y = y;
    textDraw(gResultsPressStart, x);
}

/* 0x80028E40 ovl3 - the course record (best of both save devices' tables) */
void resultsRecordDraw(s32 x, s32 y, s32 slide) {
    f32 best, other;
    s32 dev = 0;
    TextObj **t;

    drawDL(gResultsDLs[12], x - slide, y, 0, 0x10000);
    best = gHiscores[0][gTrackChoice][0].time;
    other = gHiscores[1][gTrackChoice][0].time;
    if (other < best) {
        best = other;
        dev = 1;
    }
    if ((f64)best != 100000000.0) {
        gResultsRecord[0] = textCreateTime(3, (s32)(best * 100.0f), 0, 599999, 0);
#ifdef PORT
        {   /* the port marks players' entries in the byte after the initials (1), which
             * on the N64 was always 0 and ended the string: copy the 3 letters out */
            static char rec[4];
            s32 i;
            for (i = 0; i < 3; i++) {
                rec[i] = gHiscores[dev][gTrackChoice][0].initials[i];
            }
            rec[3] = 0;
            gResultsRecord[1] = textCreate(0, rec, 0);
        }
#else
        gResultsRecord[1] = textCreate(0, (char *)gHiscores[dev][gTrackChoice][0].initials, 0);
#endif
        for (t = gResultsRecord; t < &gResultsRecord[2]; t++) {
            (*t)->flags |= TEXTF_HIDDEN;
        }
        textSetColor(gResultsRecord[0], 0xFF, 0xFF, 0xFF, 0xFF);
        textSetColor(gResultsRecord[1], 0xC8, 0xC8, 0xC8, 0xFF);
        textPlace(&gResultsRecord[0]->xf, x + slide + 0x30000, y + 0xE0000, 0, 1);
        textPlace(&gResultsRecord[1]->xf, x + slide + 0x470000, y + 0xF0000, 0, 1);
        for (t = gResultsRecord; t != &gResultsRecord[2]; t++) {
            textDraw(*t, 0);
            textFree(*t);
        }
    }
}

/* 0x80029078 ovl3 */
void resultsTimeBar1P(s32 x, s32 y) {
    rectFill(0xF8, 0x16, x + 0x100000, y - 0x940000, 0x14, 0x14, 0x14, 0xC8);
    drawDL(gResultsDLs[11], x, y, 0, 0x10000);
}

/* 0x800290FC ovl3 */
void resultsTimeBars2P(s32 x0, s32 y0, s32 x1, s32 y1) {
    rectFill(0xF8, 0xB, x0 + 0x100000, y0 - 0x4E0000, 0x14, 0x14, 0x14, 0xC8);
    rectFill(0xF8, 0xB, x1 + 0x100000, y1 + 0x10000, 0x14, 0x14, 0x14, 0xC8);
    drawDL(gResultsDLs[9], x0, y0, 0, 0x10000);
    drawDL(gResultsDLs[10], x1, y1, 0, 0x10000);
    drawDL(gResultsDLs[16], x0 + 0x280000, y0 - 0x60000, 0, 0x10000);
    drawDL(gResultsDLs[17], x1 + 0x280000, y1 - 0x510000, 0, 0x10000);
}

/* 0x8002924C ovl3 - "AVG MPH:" and the player's average speed */
void resultsAvgCreate(s32 p) {
    TextObj **t = gResultsAvg[p];
    s32 i;

    t[0] = textCreate(0, gUnitsKph != 0 ? "AVG KPH:" : "AVG MPH:", 1);
    t[1] = textCreateNum(3, resultsAvgSpeed(p, gRaceTime), 0, 0x20, 0);
    for (i = 0; i < 2; i++) {
        t[i]->flags |= TEXTF_HIDDEN;
        textPlace(&t[i]->xf, 0x03E80000, 0x03E80000, 0, 1);
    }
}

/* 0x80029348 ovl3 */
void resultsAvgFree(s32 p) {
    s32 i;

    for (i = 0; i < 2; i++) {
        textFree(gResultsAvg[p][i]);
    }
}

/* 0x800293A0 ovl3 - 1 player: car, place, time, average speed, award badge */
void resultsStats1P(s32 x, s32 y) {
    s32 i, k;
    u8 flags;

    resultsCarDraw(0, x + 0x960000, y - 0x6E0000);
    bgDrawSetup(-1, 1);
    resultsAvgCreate(0);
    gResultsAvg[0][0]->xf.x = x + 0xA70000;
    gResultsAvg[0][0]->xf.y = y + 0x60000;
    gResultsAvg[0][1]->xf.x = x + 0xAD0000;
    gResultsAvg[0][1]->xf.y = y + 0x50000;
    gResultsTimes[0][0]->xf.x = x + 0xA70000;
    gResultsTimes[0][0]->xf.y = y - 0x90000;
    gResultsTimes[0][1]->xf.x = x + 0xA50000;
    gResultsTimes[0][1]->xf.y = y - 0xA0000;
    for (i = 0; i < 2; i++) {
        textDraw(gResultsAvg[0][i], 0);
        textDraw(gResultsTimes[0][i], 0);
    }
    resultsAvgFree(0);
    if (gNumPlayersM1 != 0 || gRacersEnabled != 0) {
        hudDrawPlace(gPlayerSetup[0].place, x + 0x890000, y - 0x250000);
    }
    flags = PS_FLAGS(&gPlayerSetup[0]);
    if (flags & 6) {
        k = (flags & 4) ? 14 : 13;
        drawDL(gResultsDLs[k], x + 0x910000, y - 0x3A0000, 0, (s32)((f64)tblSin(gResultsUnk7AC) * 0.5 + 49152.0));
    }
}

/* 0x800295A0 ovl3 - 2 players: the same, half height */
void resultsStats2P(s32 p, s32 x, s32 y) {
    s32 ty = y - 0x370000;
    s32 i, k;
    u8 flags;

    resultsCarDraw(p, x + 0x410000, ty);
    bgDrawSetup(-1, 1);
    resultsAvgCreate(p);
    gResultsAvg[p][0]->xf.x = x + 0xCF0000;
    gResultsAvg[p][0]->xf.y = y - 0xF0000;
    gResultsAvg[p][1]->xf.x = x + 0xD50000;
    gResultsAvg[p][1]->xf.y = y - 0x100000;
    gResultsTimes[p][0]->xf.x = x + 0xCF0000;
    gResultsTimes[p][0]->xf.y = y - 0x1E0000;
    gResultsTimes[p][1]->xf.x = x + 0xCD0000;
    gResultsTimes[p][1]->xf.y = y - 0x1F0000;
    for (i = 0; i < 2; i++) {
        textDraw(gResultsAvg[p][i], 0);
        textDraw(gResultsTimes[p][i], 0);
    }
    resultsAvgFree(p);
    if (gNumPlayersM1 != 0 || gRacersEnabled != 0) {
        hudDrawPlace(gPlayerSetup[p].place, x + 0xED0000, y - 0x380000);
    }
    flags = PS_FLAGS(&gPlayerSetup[p]);
    if (flags & 6) {
        k = (flags & 4) ? 14 : 13;
        drawDL(gResultsDLs[k], x + 0x9B0000, ty, 0, (s32)((f64)tblSin(gResultsUnk7AC) * 0.5 + 49152.0));
    }
}
