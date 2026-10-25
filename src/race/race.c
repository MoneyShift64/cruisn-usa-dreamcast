/*
 * race/race.c - the race game mode: start-up, the per-frame race function,
 * finishing, positions, road-section tracking and per-track extras.
 * Original code: race overlay, ROM 0x48070-0x4AF70 (RAM 0x8002F600-0x80032500).
 *
 * Drawing / streaming functions in this file (0x8003004C, 0x80030714,
 * 0x80030EFC, 0x80031308, 0x80031754, 0x80031CE8) are not decompiled yet.
 */
#include "common.h"
#include "race.h"
#include "road.h"

typedef void (*ModeFunc)(s32 arg);
extern ModeFunc gModeFunc;               /* 0x8014C690 */
extern s32 gModeArg;                     /* 0x8014C694 */
extern s32 gOverlayRequest;              /* 0x8014C698 */
extern u32 *gDlCursor;                   /* 0x80147684 */
extern u32 gSegment3Base;                /* 0x80146594 */
extern Gfx gRaceBaseDL[];                /* 0x80133710 */

/* HUD / front-end data per racer (0x39C bytes, 0x80150210) */
typedef struct RacerHud {
    /* 0x000 */ s32 manual;
    /* 0x004 */ u8  pad004[8];
    /* 0x00C */ f32 speedo;                   /* shown in mph or km/h */
    /* 0x010 */ f32 odometer;
    /* 0x014 */ f32 distance;
    /* 0x018 */ u8  tacho;                    /* needle position, 255 = off the scale */
    /* 0x019 */ u8  gear;
    /* 0x01A */ u8  pad01A;
    /* 0x01B */ u8  place;                    /* 1st, 2nd ... */
    /* 0x01C */ u8  pad01C[0x324 - 0x1C];
    /* 0x324 */ s8   initials[4];
    /* 0x328 */ s32 unk328;
    /* 0x32C */ u8  pad32C[0x39C - 0x32C];
} RacerHud;
extern RacerHud gRacerHud[];             /* 0x80150210 */
extern f32 gRaceTimeLeft;                /* 0x8015094C */
extern s32 gAttractMode;                 /* 0x8015096C */
extern s32 gUnitsKph;                    /* 0x80150974 */
extern s32 gDifficulty;                  /* 0x80150988 */

extern s32 gCountdownPhase;              /* 0x800468A8 ovl2: 1-3 = 3,2,1 ; 4-5 = go ; 6+ = finished */
extern s32 gFinishCounter;               /* 0x80046E58 ovl2 - 0 until the race has ended */
extern f32 gDrawDistance[2];             /* 0x80046E68 ovl2 - per player */
extern f32 gCarDrawDistance;             /* 0x80046E70 ovl2 */
extern s32 gRaceUnk74;                   /* 0x80046E74 ovl2 */
extern f32 gStartDrawDistance;           /* 0x801460D8 */
extern s32 gAiSkillMax[8];               /* 0x80042A70 ovl2 (indexed by difficulty) */
extern f32 gAiSkill[2];                  /* 0x801460D0 - per player: AI gets tougher after a win */

typedef struct SortEntry {               /* 0x80046DB8 ovl2: 20 vehicles sorted by distance */
    /* 0x00 */ f32 dist2;
    /* 0x04 */ s32 vehicle;
} SortEntry;
extern SortEntry gVehicleSort[20];       /* 0x80046DB8 ovl2 */



typedef struct DrawObj DrawObj;
struct DrawObj {                         /* scenery object, linked per player */
    /* 0x00 */ u8  pad00[0x1C];
    /* 0x1C */ f32 dist2[2];                  /* squared distance to each player's camera */
    /* 0x24 */ u8  pad24[8];
    /* 0x2C */ DrawObj *next[2];              /* per-player draw list link */
    /* 0x34 */ s32 section;
};
typedef struct DrawList {                /* 0x10 bytes */
    /* 0x00 */ DrawObj *head[2];
    /* 0x08 */ s32 count[2];
} DrawList;
extern DrawList gDrawListA;              /* 0x8004A080 ovl2 */
extern DrawList gDrawListB;              /* 0x8004A090 ovl2 */
extern DrawList gDrawListC;              /* 0x8004A0A0 ovl2 */
extern DrawList gDrawListD;              /* 0x8004A0B0 ovl2 */
extern DrawList gDrawListE;              /* 0x8004A0C0 ovl2 */

s32  racePauseCheck(ModeFunc self);      /* 0x8002A6B4 ovl2 */
void raceHudSplitScreen(void);           /* 0x80115AC4 */
void racersEngageFirstGear(void);        /* 0x80032850 ovl2 */
void aiRacersStart(void);                 /* 0x8003B7A0 ovl2 */
void trafficSpawner(void);                  /* 0x8003BDA4 ovl2 */
void vehiclesTrackUpdate(void);
void racePositions(void);
void playerInputRead(void);            /* 0x80034634 ovl2 */
void raceSectionsUpdate(s32 player);
void sectionWindowUpdate(s32 player);   /* 0x80031754 ovl2 */
s32  meshCacheCollect(void);             /* 0x8011DA50 - recycle released meshes once off screen */
void playerCarUpdate(s32 racer);         /* 0x80032A34 ovl2 */
void vehicleSyncModel(s32 vehicle);      /* 0x8003702C ovl2 */
void raceCameraUpdate(s32 player);           /* 0x80034B64 ovl2 */
void aiRacerUpdate(s32 vehicle);         /* 0x8003B828 ovl2 */
void trafficUpdate(s32 vehicle);         /* 0x8003BA20 ovl2 */
void deathValleyUpdate(s32 phase);       /* 0x8003975C ovl2 */
void flybyUpdate(s32 phase);             /* 0x8003AF00 ovl2 */
void us101Update(s32 phase);             /* 0x8003A6E0 ovl2 */
void moversUpdate(void);              /* 0x80041024 ovl2 */
void raceObjectsUpdate(void);            /* 0x80025E38 ovl2 */
void racerNoReverse(s32 racer);
void skyDraw(s32 player, s32 yaw, s32 roll); /* 0x80101A68 */
void cameraBuildMatrix(s32 player);           /* 0x800351B4 ovl2 */
void raceUpdateDrawLists(s32 player);
void raceDrawWorld(s32 player);          /* 0x80030EFC ovl2 */
void us101Draw(s32 player);              /* 0x8003A8BC ovl2 */
void particlesDraw(s32 player);          /* 0x80040224 ovl2 */
void raceMeshCollect(s32 player);
void particlesEndFrame(void);            /* 0x80040298 ovl2 */
void raceHudDraw(void);                  /* 0x8002B6B8 ovl2 */
void cameraForceFar(s32 player, s32 on, s32 quiet); /* 0x800349BC ovl2 */
void trackUnload(void);             /* 0x80100A84 */
void trafficCleanup(void);              /* 0x8003BAF4 ovl2 */
void raceFinish(s32 racer, s32 how);
void raceStartCommon(void);              /* 0x8002A320 ovl2 */
void trackModelsLoad(s32 track);         /* 0x8003F470 ovl2 */
void sceneryPoolInit(void);                /* 0x80038CD0 ovl2 */
void moversInit(void);                      /* 0x80040C30 ovl2 */
void us101Init(void);                    /* 0x8003A670 ovl2 */
void trackReload(void);                  /* 0x8010041C */
void trackObjectsLoad(s32 track);        /* 0x8003F648 ovl2 */
void moversStart(void);                     /* 0x80040C84 ovl2 */
void raceObjectsInit(void);              /* 0x80025C80 ovl2 */
void trackEventsInit(void);              /* 0x80032500 ovl2 */
void raceHudInit(s32 arg);               /* 0x8002AF24 ovl2 */
void fadeInit(void);                     /* 0x801012C0 */
void trackSpecialInit(s32 track);
void carsInit(void);                       /* 0x800358D0 ovl2 */
void racersInit(void);                   /* 0x800326F0 ovl2 */
void aiRacersInit(void);                  /* 0x8003B560 ovl2 */
void raceFrame(s32 arg);
void raceSetHudVisible(s32 visible);     /* 0x8002B0E4 ovl2 */
void racersMeasureDistance(void);        /* 0x800328B4 ovl2 */
void trackEventsCheck(s32 car, s32 track, s32 section); /* 0x800325E4 ovl2 */
void scriptCmdRun(s32 car, s32 index);   /* 0x800325A8 ovl2 */
void carModelPose(void *pose, void *anim); /* 0x8002662C ovl2 */
f32  carCameraSideOffset(Car *car, s32 player); /* 0x800370B8 ovl2 */
void deathValleyInit(void);              /* 0x80039710 ovl2 */
void flybyInit(s32 track);               /* 0x8003AEE0 ovl2 */
void us101Setup(s32 track);              /* 0x8003A6C8 ovl2 */
s32  drawListRemove(DrawList *list, s32 player, s32 section); /* 0x80038EB4 ovl2 */
void drawListSort(DrawList *list, s32 player, f32 x, f32 z);    /* 0x80031BD4 ovl2 */
void drawListMeasure(DrawList *list, s32 player, f32 x, f32 z); /* 0x80031C94 ovl2 */

extern void func_80026C98_ovl3(s32);
extern void func_80026030_ovl3(s32);

extern u8 gCarAnim[];                    /* 0x80043A50 ovl2, 0xF8 per car model */
extern u8 gPathInfo[];                   /* 0x8015BF20, 0x14 per section */

/* 0x8002F600 ovl2 - race mode, first frame: build the race */
void raceStart(s32 arg) {
    SortEntry *e;
    s32 i;
    s32 p;
    s32 n;

    gRaceState = 0;
    gRaceStateB = 0;
    if (gAttractMode == 0) {
        gAiSkill[0] = 1.0f;
        gAiSkill[1] = 1.0f;
    }
    raceStartCommon();
    trackModelsLoad(gCurrentTrack);
    sceneryPoolInit();
    moversInit();
    if (gCurrentTrack == 2) {
        us101Init();
    }
    trackReload();
    trackObjectsLoad(gCurrentTrack);
    moversStart();
    raceObjectsInit();
    trackEventsInit();
    raceHudInit(arg);
    fadeInit();
    gDrawDistance[1] = gStartDrawDistance;
    gDrawDistance[0] = gDrawDistance[1];
    gCarDrawDistance = 2000.0f;
    gRaceUnk74 = 0;
    trackSpecialInit(gCurrentTrack);
    carsInit();
    racersInit();
    aiRacersInit();
    gModeFunc = raceFrame;
    for (i = 0, e = gVehicleSort; i < 20; i++, e++) {
        e->vehicle = i;
        e->dist2 = 0.0f;
    }
    n = gNumPlayersM1;
    for (p = 0; p <= gNumPlayersM1; p++) {
        raceSectionsUpdate(p);
    }
    if (n >= 0) {
        for (p = 0; p <= gNumPlayersM1; p++) {
            for (i = 89; i != 0; i--) {   /* settle the scenery draw-lists */
                raceUpdateDrawLists(p);
            }
        }
    }
    raceFrame(0);
}

/* 0x8002F7DC ovl2 - race mode, every frame */
void raceFrame(s32 arg) {
    Car *car;
    RacerInfo *info;
    RacerHud *hud;
    RaceCamera *cam;
    PlayerInput *in;
    s32 tacho;
    f32 k;
    s32 i;
    s32 j;
    s32 v;
    u32 *dl;

    if (racePauseCheck(raceFrame) != 0) {
        return;
    }
    if (gNumPlayersM1 != 0) {
        raceHudSplitScreen();
    }
    if (gCountdownPhase == 1 || gCountdownPhase == 2 || gCountdownPhase == 3) {
        gRaceState = 0;
    } else if (gCountdownPhase == 4 || gCountdownPhase == 5) {
        if (gRaceState == 0) {
            racersEngageFirstGear();      /* GO! */
            aiRacersStart();
        }
        gRaceState = 1;
    } else {
        gRaceState = 2;
    }
    trafficSpawner();
    if (gRaceState != 2) {
        vehiclesTrackUpdate();
        racePositions();
    }
    playerInputRead();
    for (i = 0; i <= gNumPlayersM1; i++) {
        raceSectionsUpdate(i);
    }

    /* players */
    for (i = 0, car = gCars, info = gRacers, hud = gRacerHud; i <= gNumPlayersM1; i++, car++, info++, hud++) {
        if (gRaceState != 2) {
            playerCarUpdate(i);
            vehicleSyncModel(i);
        }
        tacho = (car->rpm * 215) / 45 + 20;
        hud->gear = car->gear;
        hud->manual = car->manual;
        if (tacho >= 256) {
            tacho = -1;
        }
        hud->tacho = tacho;
        k = gUnitsKph ? 0.78841f : 0.49f;
        hud->place = info->position + 1;
        hud->speedo = k * car->speed;
        hud->odometer = car->odometer * 0.0078012f;
        if (car->flipping) {
            car->pos[1] -= info->flipHeight;
            raceCameraUpdate(i);
            car->pos[1] += info->flipHeight;
        } else {
            raceCameraUpdate(i);
        }
    }

    /* computer racers, then traffic */
    for (i = 0; i < gNumAiRacers; i++) {
        if (gRaceState != 2) {
            v = gNumPlayers + i;
            aiRacerUpdate(v);
            vehicleSyncModel(v);
        }
    }
    for (v = gNumPlayers + gNumAiRacers; v < gNumVehicles; v++) {
        if (gRaceState != 2) {
            trafficUpdate(v);
            vehicleSyncModel(v);
        }
    }

    /* track-specific moving scenery */
    if (gRaceState == 1) {
        switch (gCurrentTrack) {
            case 6:
                deathValleyUpdate(gRaceState);
                break;
            case 9:
            case 11:
                flybyUpdate(gRaceState);
                break;
            case 2:
                us101Update(gRaceState);
                break;
        }
    }
    moversUpdate();
    raceObjectsUpdate();

    /* draw each player's view */
    for (i = 0, cam = gRaceCameras; i <= gNumPlayersM1; i++, cam++) {
        racerNoReverse(i);
        if (gCurrentTrack == 0) {
            gSegment3Base = gRacers[i].unk1B;
        }
        dl = gDlCursor; gDlCursor = dl + 2;
        dl[0] = 0xBC000C06;                     /* gsSPSegment(3, ...) */
        dl[1] = gSegment3Base << 9;
        skyDraw(i, (s32)((cam->yaw * 32768.0f) / 3.1415927f) + 0xC000,
                       (s32)((cam->pitch * 32768.0f) / 3.1415927f));
        cameraBuildMatrix(i);
        raceUpdateDrawLists(i);
        raceDrawWorld(i);
        dl = gDlCursor; gDlCursor = dl + 2; dl[1] = 0; dl[0] = 0xE7000000;        /* gDPPipeSync */
        dl = gDlCursor; gDlCursor = dl + 2; dl[1] = 1; dl[0] = 0xB7000000;        /* set geometry mode */
        dl = gDlCursor; gDlCursor = dl + 2; dl[0] = 0xB900031D; dl[1] = 0x504870; /* render mode */
        dl = gDlCursor; gDlCursor = dl + 2; dl[0] = 0x01020040; dl[1] = (u32)gRaceBaseDL; /* gSPMatrix */
        if (gCurrentTrack == 2) {
            us101Draw(i);
        }
        particlesDraw(i);
#ifdef PORT
        {
            void port_snow_draw(s32 player);
            void port_rain_draw(s32 player);
            port_snow_draw(i);               /* the snow cheat */
            port_rain_draw(i);               /* the rain cheat */
        }
#endif
        dl = gDlCursor; gDlCursor = dl + 2; dl[1] = 0; dl[0] = 0xE7000000;
        dl = gDlCursor; gDlCursor = dl + 2; dl[0] = 0xB900031D; dl[1] = 0x504240;
        dl = gDlCursor; gDlCursor = dl + 2; dl[1] = 1; dl[0] = 0xB6000000;        /* clear geometry mode */
        raceMeshCollect(i);
    }
    particlesEndFrame();
    raceHudDraw();

    /* QUIT from the pause menu */
    for (i = 0, in = gPlayerInput; i <= gNumPlayersM1; ) {
        i++;
        if (in->buttons & RACEBTN_QUIT) {
            for (j = 0; j <= gNumPlayersM1; j++) {
                cameraForceFar(j, 0, 1);
            }
            trackUnload();
            gModeArg = 0;
            gOverlayRequest = 0x8000 | OVL_FRONTEND;
            gModeFunc = func_80026C98_ovl3;
            break;
        }
        in++;
    }

    trafficCleanup();
    /* out of time */
    if (gRaceTimeLeft == 0.0f && gRaceState == 1) {
        for (i = 0; i <= gNumPlayersM1; i++) {
            raceFinish(i, -1);
        }
    }
}

/* 0x8002FE98 ovl2 - leave the race and go back to the front end */
void raceQuit(void) {
    raceSetHudVisible(1);
    trackUnload();
    gModeArg = 0;
    gModeFunc = func_80026030_ovl3;
    gOverlayRequest = 0x8000 | OVL_FRONTEND;
}

/* 0x8002FEE4 ovl2 - end the race (a player crossed the line or time ran out).
 * Only the first call does anything: adjusts each player's AI skill for the next race. */
void raceFinish(s32 racer, s32 how) {
    RenderObj *obj;
    f32 *skill;
    s32 i;

    if (gFinishCounter++ == 0) {          /* only the first call ends the race */
        for (i = 0, obj = gRenderObjs, skill = gAiSkill; i <= gNumPlayersM1; i++, skill++, obj++) {
            if (gRaceTimeLeft != 0.0f && gRacers[i].position == 0) {
                *skill += 0.07f;            /* the winner faces tougher AI next race */
                if ((f32)gAiSkillMax[gDifficulty] < *skill) {
                    *skill = (f32)gAiSkillMax[gDifficulty];
                }
            } else {
                *skill -= 0.035f;
                if (*skill < 0.8f) {
                    *skill = 0.8f;
                }
            }
            obj->flags |= 0x400;
        }
        racePositions();
        racersMeasureDistance();
        raceSetHudVisible(0);
        gRaceState = 2;
    }
}

/* 0x800307E4 ovl2 */
void racerSetRumble(s32 racer, s32 on) {
    gRacers[racer].kerbRumble = on;
}

/* 0x80030804 ovl2 */
void racerSetFlag1A(s32 racer, s32 on) {
    ((u8 *)&gRacers[racer])[0x1A] = on;
}

/* 0x80030824 ovl2 */
void racerSetFlag1B(s32 racer, s32 on) {
    gRacers[racer].unk1B = on;
}


/* 0x80030844 ovl2 - 2 players: how many road sections this player leads (+) or trails (-) by */
s32 raceLeadGap(s32 player) {
    SectionWindow *me;
    SectionWindow *other;
    s32 a;
    s32 b;
    s32 gap;

    if (gNumPlayersM1 == 0) {
        return 0;
    }
    me = &gSectionWindows[player];
    a = me->first;
    other = &gSectionWindows[player == 0];
    b = other->first;
    gap = a - b - other->count;
    if (gap > 0) {
        return gap;
    }
    gap = b - a - me->count;
    if (gap > 0) {
        return -gap;
    }
    return 0;
}

/* 0x800308D0 ovl2 - follow every vehicle along the road: side of road, road section,
 * and fire track events / script commands when a racer enters a new section */
void vehiclesTrackUpdate(void) {
    Car *car = gCars;
    RacerInfo *info;
    VehicleState *vs;
    RoadPos *road;
    s32 section;
    s32 target;
    s32 flags;
    s32 side;
    s32 cmd;
    f32 d0;
    f32 d1;
    s32 i;

    for (i = 0, info = gRacers, vs = gVehicleStates; i < gNumVehicles; i++, car++, info++, vs++) {
        car->unk64 = car->speed;
        car->prevHeading = car->heading;
        carModelPose(&gCarAnim[car->model * 0xF8 + 0x94], (u8 *)car + 0x1B8);
        section = info->section;
        road = &car->road;
        roadLocate(road, car->pos[0], car->pos[2], car->unk84 == 3);
        car->roadOffset = dist2D(car->pos[0], car->pos[2], car->road.pos[0], car->road.pos[2]);
        if (roadIsLeft(road) == 0) {
            car->roadOffset = car->roadOffset * -1.0f;
        }
        side = 0;
        if (car->sparkTimer != 0) {
            car->sparkTimer--;
        }
        if (vs->timer != 0) {
            vs->timer--;
        }
        target = gPathInfo[car->road.point * 0x14 + 0xE];
        flags = gRoadSections[target].flags & 3;
        vs->onSplitRoad = (flags != 0);
        if (flags == 3) {
            d0 = carCameraSideOffset(car, 0);
            d1 = carCameraSideOffset(car, 1);
            if ((d1 > 0.0f && d1 < d0) || (d1 < 0.0f && d0 < d1)) {
                side = 1;
            }
        } else {
            side = 0;
            if (flags & 2) {
                side = 1;
            }
            if (car->unk84 == 3 && car->spinTimer != 0 && gRacers[side].pathBehind >= car->road.point) {
                vs->remove = 1;
            }
        }
        vs->roadSide = side;
        if (i < gNumPlayers) {
            while (section < target) {
                section++;
                trackEventsCheck(i, gCurrentTrack, section);
                cmd = gTrack->sections[section].flags & 0x3F;
                if (cmd != 0x3F) {
                    scriptCmdRun(i, cmd);
                }
                info->section = section;
            }
        }
        car->heading = angleWrap(car->heading);
        car->travelDir = angleWrap(car->travelDir);
    }
}

/* 0x80030C64 ovl2 - race positions: count the racers ahead of each player
 * (AI racers that have already finished count as ahead) */
void racePositions(void) {
    Car *car = gCars;
    Car *other;
    RacerInfo *info;
    f32 me;
    f32 them;
    s32 ahead;
    s32 total;
    s32 i;
    s32 j;

    total = gNumPlayers + gNumAiRacers;
    for (i = 0, info = gRacers; i < gNumPlayers; i++, car++, info++) {
        me = (f32)car->road.point + car->road.t;
        ahead = 0;
        for (j = 0; j < total; j++) {
            if (i == j) {
                continue;
            }
            other = &gCars[j];
            them = other->road.t + (f32)other->road.point;
            if ((j >= gNumPlayers && gVehicleStates[j].finishDist != 0.0f) ||
                me < them || (me == them && i < j)) {
                ahead++;
            }
        }
        info->position = ahead;
        if (ahead != 0) {
            info->framesLeading = 0;
        } else {
            info->framesLeading++;
        }
        /* all four wheels off the road: count up, otherwise reset */
        info->offroadTime++;
        if (car->probes[0].offroad == 0) info->offroadTime = 0;
        if (car->probes[1].offroad == 0) info->offroadTime = 0;
        if (car->probes[2].offroad == 0) info->offroadTime = 0;
        if (car->probes[3].offroad == 0) info->offroadTime = 0;
        if (car->probes[4].offroad == 0) info->offroadTime = 0;
    }
}

/* 0x80030E34 ovl2 - re-sort the scenery lists by distance from this player's camera */
void raceUpdateDrawLists(s32 player) {
    RaceCamera *cam = &gRaceCameras[player];
    f32 x = cam->posX;
    f32 z = cam->posZ;

    drawListSort(&gDrawListB, player, x, z);
    drawListSort(&gDrawListC, player, x, z);
    drawListSort(&gDrawListA, player, x, z);
    drawListSort(&gDrawListD, player, x, z);
    drawListMeasure(&gDrawListE, player, x, z);
}

/* 0x80031710 ovl2 */
void raceSectionsUpdate(s32 player) {
    sectionWindowUpdate(player);
}

/* 0x80031730 ovl2 */
void raceMeshCollect(s32 player) {
    meshCacheCollect();
}

/* 0x80031BA4 ovl2 - length of the next road section that has one */
f32 sectionLength(RoadSection *s) {
    s32 len = s->length;

    while (len == 0) {
        len = s[1].length;
        s++;
    }
    return (f32)len;
}

/* 0x80032278 ovl2 - extra set-up for tracks with special scenery */
void trackSpecialInit(s32 track) {
    if (track == 11) {
        gSegment3Base = 1;          /* Indiana: alternate texture bank */
    } else {
        gSegment3Base = 0;
    }
    if (track == 6) {
        deathValleyInit();
    }
    if (track == 9 || track == 11) {
        flybyInit(track);           /* Iowa, Indiana (shared moving scenery, probably trains) */
    }
    if (track == 2) {
        us101Setup(track);
    }
}

/* 0x800322FC ovl2 - stop a car moving backwards along the road (unless allowed) */
void racerNoReverse(s32 racer) {
    RacerInfo *info = &gRacers[racer];
    Car *car = &gCars[racer];
    s32 sec = car->road.point;
    s32 bit;
    f32 dx;
    f32 dz;
    f32 s;
    f32 c;
    f32 along;

    if (info->allowReverse == 0 && sec >= 2) {
        bit = (car != gCars) ? 2 : 1;
        if (!(bit & gRoadSections[gPathInfo[sec * 0x14 - 0x2E]].flags)) {
            goto check;
        }
    } else {
check:
        dx = car->pos[0] - info->lastX;
        dz = car->pos[2] - info->lastZ;
        s = -sinf_tbl(car->road.heading);
        c = cosf_tbl(car->road.heading);
        along = dx * s + dz * c;
        if (along < 0.0f) {
            car->pos[0] -= along * s;
            car->pos[2] -= along * c;
        }
    }
    dz = car->pos[2] - info->lastZ;
    info->unk60 = car->pos[0] - info->lastX;
    info->lastX = car->pos[0];
    info->lastZ = car->pos[2];
    info->unk64 = dz;
}

/* 0x80032484 ovl2 - do this racer's initials match 'name' (3 letters)? */
s32 initialsMatch(s32 racer, char *name) {
    s8 *mine = gRacerHud[racer].initials;
    s32 same = mine[0] == name[0];

    if (same) {
        same = mine[1] == name[1];
        if (same) {
            same = mine[2] == name[2];
        }
    }
    return same;
}
