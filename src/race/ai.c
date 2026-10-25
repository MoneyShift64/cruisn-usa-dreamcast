/*
 * race/ai.c - computer opponents ("AI racers"), traffic, and the fly-by object.
 * Original code: race overlay, ROM 0x53950-0x57B00 (RAM 0x8003AEE0-0x8003F470).
 *
 * gCars layout: [players][AI racers][traffic]. AI racers run the same engine /
 * steering code as the players (carphys.c) when a player can see them, and a
 * cheap "slide along the route" movement when nobody can.
 */
#include "common.h"
#include "race.h"
#include "road.h"

#define CLAMP(x, lo, hi) (((x) < (lo)) ? (lo) : (((x) > (hi)) ? (hi) : (x)))

extern s32 gFrameCounter;                /* 0x80147698 */
extern s32 gNumAiRacers;                 /* 0x8004A014 ovl2 */
extern s32 gNumVehicles;                 /* 0x8004A01C ovl2 */
extern s32 gMaxVehicles;                 /* 0x8004A020 ovl2 */
extern s32 gRaceUnk74;                   /* 0x80046E74 ovl2 - finishing order counter */
extern s32 gTrafficEnabled;              /* 0x80150978 */
extern f32 gRaceTime;                    /* 0x80150948 - seconds since the start */
extern s32 gDifficulty;                  /* 0x80150988 */
extern f32 gAiSkill[2];                  /* 0x801460D0 */
extern Mtx3 gTmpMtxA;                    /* 0x8004A030 ovl2 */
extern Mtx3 gTmpMtxB;                    /* 0x8004A058 ovl2 */





typedef struct RacerHud {                /* 0x39C bytes, 0x80150210 */
    /* 0x000 */ u8  pad000[0x388];
    /* 0x388 */ s32 unk388;
    /* 0x38C */ s32 cameraFollowsCar;
    /* 0x390 */ u8  pad390[0x0C];
} RacerHud;
extern RacerHud gRacerHud[];             /* 0x80150210 */

typedef struct Particle {                /* 0x3C bytes, 0x80066460 ovl2 */
    /* 0x00 */ u8  pad00[8];
    /* 0x08 */ f32 x;
    /* 0x0C */ f32 y;
    /* 0x10 */ f32 z;
    /* 0x14 */ f32 yaw;
    /* 0x18 */ u8  pad18[0x24];
} Particle;
extern Particle gParticles[];            /* 0x80066460 ovl2 */

typedef struct FlybyConfig {             /* 12 bytes, 0x80043A20 ovl2 */
    /* 0x00 */ f32 sideDist;
    /* 0x04 */ f32 unk04;
    /* 0x08 */ s32 flip;
} FlybyConfig;
extern FlybyConfig gFlybyConfigs[4];     /* 0x80043A20 ovl2 */
extern FlybyConfig *gFlybyCfg;           /* 0x800657F0 ovl2 */
extern RoadPos gFlybyRoad;               /* 0x800657F8 ovl2 */
extern s32 gFlybyParticle;               /* 0x80065814 ovl2 */
extern u32 gFlybyNextTime;               /* 0x80065818 ovl2 - 0 while one is active */
extern u32 gFlybyEventTime;              /* 0x8006581C ovl2 */
extern s32 gFlybyPlayer;                 /* 0x80065820 ovl2 */
extern s32 gFlybySfxDone;                /* 0x80065824 ovl2 */

typedef struct AiDef {                   /* 12 bytes, 0x80044B14 ovl2, indexed by vehicle */
    /* 0x00 */ s32 type;
    /* 0x04 */ f32 power;
    /* 0x08 */ f32 gain;
} AiDef;
extern AiDef gAiDefs[];                  /* 0x80044B14 ovl2 */
extern s32 gAiDebugBrake;                /* 0x80044B10 ovl2 - always 0 */

typedef struct TrafficDef {              /* 16 bytes, 0x80044B8C ovl2, per track */
    /* 0x00 */ u8  kind;
    /* 0x01 */ u8  pad01[3];
    /* 0x04 */ s32 minDelay;
    /* 0x08 */ s32 randDelay;
    /* 0x0C */ s32 unk0C;
} TrafficDef;
extern TrafficDef gTrafficDefs[];        /* 0x80044B8C ovl2 */

typedef struct TrafficTimer {            /* 0x80066438 ovl2: same-direction traffic */
    /* 0x00 */ u32 next;
    /* 0x04 */ s32 unk04;
    /* 0x08 */ s32 count;
} TrafficTimer;
extern TrafficTimer gTrafficSame;        /* 0x80066438 ovl2 */
typedef struct TrafficTimer2 {           /* 0x80066448 ovl2: oncoming traffic, one timer per player */
    /* 0x00 */ u32 next[2];
    /* 0x08 */ s32 count;
} TrafficTimer2;
extern TrafficTimer2 gTrafficOncoming;   /* 0x80066448 ovl2 */

extern s32 gCrashLandSfx[3];             /* 0x80044C6C ovl2 */
extern f32 gTrackAiBonus[14];            /* 0x80044C78 ovl2 */
extern s32 gDifficultyAi[];              /* 0x80133B28, stride 12 */
extern f32 gDifficultyAiScale;           /* 0x80045E38 ovl2 */

f32  randFloat(void);                    /* 0x80103104 */
u32  randInt(s32 n);                     /* 0x80103150 */
void sfxPlay(s32 id, s32 player);        /* 0x8011F2A4 */
void sfxPlayVolume(s32 id, s32 player, f32 volume); /* 0x8011F2C4 */
s32  particleSpawn(s32 type, f32 x, f32 y, f32 z, s32 kind); /* 0x80040070 ovl2 */
void particleFree(s8 index);             /* 0x800401E0 ovl2 */
void cameraStartEvent(s32 player, s32 kind); /* 0x8002AC18 ovl2 */
s32  raceLeadGap(s32 player);            /* 0x80030844 ovl2 */
s32  carCompareAhead(Car *a, Car *b);    /* 0x80037BB4 ovl2 */
void carInitStats(s32 vehicle);          /* 0x80035BF0 ovl2 */
void carGroundUpdate(Car *car, s32 side);  /* 0x80025C90 ovl2 */
void racerResetRoad(Car *car, s32 how);  /* 0x80037D88 ovl2 */
void carProbesFindGround(Car *car, s32 side); /* 0x8002690C ovl2 */
s32  carGroundDelta(Car *car, f32 *out); /* 0x800266D8 ovl2 */
void carPlaySfx(Car *car, s32 id, f32 volume); /* 0x800378DC ovl2 */
void carPlayRandomSfx(Car *car, s32 *table, s32 count, f32 volume); /* 0x80037944 ovl2 */
void carHitBarrier(Car *car, s32 player, s32 noSteer); /* 0x800371D4 ovl2 */
void carCheckProbes(Car *car);           /* 0x800370E0 ovl2 */
s32  autoGearbox(s32 gear, s32 rpm, f32 throttle); /* 0x800364C0 ovl2 */
void carEngineUpdate(Car *car);          /* 0x80036540 ovl2 */
void carSlideUpdate(Car *car);           /* 0x80036B0C ovl2 */
void carRpmUpdate(s32 vehicle);          /* 0x8003698C ovl2 */
void carSteerUpdate(Car *car);           /* 0x80036D14 ovl2 */
s32  cameraDistance2(s32 player, s32 vehicle); /* 0x80100BC8 */
extern f32 gDrawDistance[2];             /* 0x80046E68 ovl2 */
extern s32 gChicagoPillars;              /* 0x80066454 ovl2 - Chicago: lanes blocked by the elevated railway */
extern s32 gLaneMap[50];                 /* 0x80066370 ovl2 - obstacles across the road, 100 units per slot */

void flybyMove(void);
void flybyCheckDone(void);
void flybySpawn(void);
void flybyResetTimer(void);
void aiPhysics(s32 vehicle);
s32  aiFinishCheck(Car *car, VehicleState *vs);
void aiPassingHorn(Car *car, VehicleState *vs);
void aiSteer(Car *car, VehicleState *vs);
void aiOffscreen(Car *car, VehicleState *vs, s32 behind);
void aiRubberBand(Car *car, VehicleState *vs);
f32  aiSkillFactor(void);
void aiBarrierCheck(Car *car);
void vehicleIdle(s32 vehicle);
void trafficCrash(Car *car, VehicleState *vs, s32 vehicle);
void trafficDriveSame(Car *car, VehicleState *vs, s32 vehicle);     /* 0x8003E8E8 ovl2 */
void trafficDriveOncoming(Car *car, VehicleState *vs, s32 vehicle); /* 0x8003EBB4 ovl2 */
void trafficSpawnSame(u8 kind);          /* 0x8003E4DC ovl2 */
void trafficSpawnOncoming(s32 kind, s32 player); /* 0x8003E64C ovl2 */
void aiBuildLaneMap(VehicleState *vs);   /* 0x8003D7BC ovl2 */
void aiMarkObstacles(Car *car);          /* 0x8003D92C ovl2 */
void aiChooseLane(Car *car, VehicleState *vs); /* 0x8003E260 ovl2 */
f32  aiPathAhead(Car *car, VehicleState *vs, s32 backwards); /* 0x8003E0F4 ovl2 */
void aiLeaveOffscreen(Car *car, VehicleState *vs); /* 0x8003DF2C ovl2 */
void aiOffscreenEngine(Car *car, VehicleState *vs); /* 0x8003DFA8 ovl2 */
s32  aiMarkVehicle(Car *car, Car *other, s32 flag); /* 0x8003DA34 ovl2 */
f32  aiSideSpeed(Car *car);              /* 0x8003DE34 ovl2 */
f32  aiCarWidth(Car *car);               /* 0x8003DE7C ovl2 */

/* ------------------------------------------------------------------ */
/* Fly-by object (Iowa and Indiana): something flies along the road  */
/* just ahead of a player every so often.                             */
/* ------------------------------------------------------------------ */

/* 0x8003AEE0 ovl2 */
void flybyInit(s32 track) {
    flybyResetTimer();
}

/* 0x8003AF00 ovl2 */
void flybyUpdate(s32 phase) {
    if (gFlybyNextTime == 0) {
        flybyMove();
        flybyCheckDone();
        return;
    }
    if ((u32)gFrameCounter >= gFlybyNextTime && gRaceState == 1) {
        flybySpawn();
    }
}

/* 0x8003AF70 ovl2 - next fly-by in 2 to 11 seconds */
void flybyResetTimer(void) {
    gFlybyNextTime = randInt(540) + gFrameCounter + 120;
}

/* 0x8003AFA4 ovl2 - keep the fly-by just ahead of its player */
void flybyMove(void) {
    Particle *obj;
    Car *car;
    RacerHud *hud;
    f32 dist;
    f32 ang;
    f32 x;
    s32 section;
    s32 i;

    dist = gCars[gFlybyPlayer].speed * 0.8f;
    obj = &gParticles[gFlybyParticle];
    ang = gFlybyRoad.heading;
    if (gFlybyCfg->flip) {
        dist = -dist;
        ang += 3.1415927f;
    }
    dist = dist + gFlybyCfg->sideDist;
    x = gFlybyRoad.pos[0] - sinf_tbl(ang) * dist;
    roadLocate(&gFlybyRoad, x, cosf_tbl(ang) * dist + gFlybyRoad.pos[2], gFlybyCfg->flip);
    obj->x = gFlybyRoad.pos[0] * 0.2f;
    obj->y = ((gNumPlayersM1 != 0) ? 240.0f : 400.0f) + gFlybyRoad.pos[1] * 0.2f;
    obj->z = gFlybyRoad.pos[2] * -0.2f;
    obj->yaw = ang + 3.1415927f;
    section = gFlybyRoad.point;
    for (i = 0, car = gCars; i < gNumPlayers; i++, car++) {
        if (car->road.point + 1 < section && section < car->road.point + 4) {
            if (gFlybySfxDone == 0) {
                sfxPlay(0x3B, gFlybyPlayer);
                gFlybySfxDone = 1;
            }
            if (gFlybyEventTime < (u32)gFrameCounter && randInt(20) == 0) {
                if (car->speed > 50.0f) {
                    hud = &gRacerHud[i];
                    if (hud->unk388 == 0 && hud->cameraFollowsCar == 0) {
                        cameraStartEvent(i, 1);
                    }
                }
                gFlybyEventTime = randInt(120) + gFrameCounter + 60;
            }
        }
    }
}

/* 0x8003B288 ovl2 - remove the fly-by when it leaves the loaded road */
void flybyCheckDone(void) {
    u8 sec = gPathPoints[gFlybyRoad.point].section;

    if (sec < 2 || !(sec < gPathPoints[gPathLength - 2].section - 2) ||
        (!(gRoadSections[sec].flags & 3) && !(gRoadSections[sec - 2].flags & 3))) {
        particleFree(gFlybyParticle);
        flybyResetTimer();
    }
}

/* 0x8003B348 ovl2 - 50%: start a fly-by near one of the players */
void flybySpawn(void) {
    Particle *obj;
    PathPoint *pt;
    s32 gap;

    flybyResetTimer();
    if (randInt(2) != 0) {
        gFlybyPlayer = 0;
        if (gNumPlayersM1 != 0) {
            gap = raceLeadGap(0);
            if (gap < 0) {
                gap = -gap;
            }
            if (gap >= 3) {
                gFlybyPlayer = randInt(2);
            } else if (carCompareAhead(&gCars[0], &gCars[1]) > 0) {
                gFlybyPlayer = 1;
            } else {
                gFlybyPlayer = 0;
            }
        }
        if (gSectionWindows[gFlybyPlayer].first >= 4) {
            gFlybyParticle = particleSpawn(2, 0.0f, 0.0f, 0.0f, 0);
            if (gFlybyParticle >= 0) {
                gFlybyCfg = &gFlybyConfigs[randInt(4)];
                pt = &gPathPoints[roadCurrentPoint(gFlybyPlayer)] - 2;
                obj = &gParticles[gFlybyParticle];
                obj->x = pt->x;
                obj->y = ((gNumPlayersM1 != 0) ? 240.0f : 400.0f) + pt->y;
                obj->z = pt->z;
                gFlybyRoad.point = 0;
                roadLocate(&gFlybyRoad, pt->x * 5.0f, pt->z * -5.0f, 0);
                gFlybyNextTime = 0;
                gFlybyEventTime = 0;
                gFlybySfxDone = 0;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* AI racers                                                          */
/* ------------------------------------------------------------------ */

/* 0x8003B560 ovl2 - set up the computer opponents */
void aiRacersInit(void) {
    Car *car;
    VehicleState *vs;
    AiDef *def;
    s32 i;
    f32 power;

    for (i = 0; i < gNumAiRacers; i++) {
        car = &gCars[gNumPlayers + i];
        vs = &gVehicleStates[gNumPlayers + i];
        def = &gAiDefs[gNumPlayers + i];
        carInitStats(i + gNumPlayers);
        power = aiSkillFactor() * def->power;
        car->shiftTimer = 0;
        car->manual = 0;
        car->speedScale = 1.0f;
        car->boostPower = 1.0f;
        car->boosting = 0;
        car->unk84 = 1;
        car->enginePower = power;
        car->gearTarget = car->gear;
        bytesClear((u8 *)vs, sizeof(VehicleState));
        car->grip = 0.44f;
        car->dragOffroad = car->dragRoad * 1.5f;
        vs->throttle = 1.0f;
        vs->throttleVar = 1.0f;
        vs->varTimer = 120;
        vs->rubberTimer = 120;
        vs->rubberBand = randFloat() * 0.05f + 1.2f;
        vs->rubberGain = def->gain;
        vs->unk08 = vs->laneTarget = (def->type == 2) ? 572.0f : 1724.0f;
        vs->pathTarget = 4;
        vs->type = def->type;
        vs->vehicle = i + gNumPlayers;
    }
    gTrafficSame.next = 0;
    gTrafficSame.count = 0;
    gTrafficOncoming.next[0] = 0;
    gTrafficOncoming.next[1] = 0;
    gTrafficOncoming.count = 0;
}

/* 0x8003B7A0 ovl2 - GO! the AI racers engage first gear */
void aiRacersStart(void) {
    Car *car;
    s32 i;

    for (i = 0; i < gNumAiRacers; i++) {
        car = &gCars[gNumPlayers + i];
        car->gear = 1;
        gVehicleStates[gNumPlayers + i].pathTarget = car->road.point + 4;
    }
}

/* 0x8003B828 ovl2 - one frame of an AI racer */
void aiRacerUpdate(s32 v) {
    Car *car;
    VehicleState *vs;
    u8 sec;
    s32 behind;

    if (gRaceState != 1) {
        vehicleIdle(v);
        return;
    }
    vs = &gVehicleStates[v];
    car = &gCars[v];
    if (aiFinishCheck(car, vs) != 0) {
        return;
    }
    sec = gPathPoints[car->road.point].section;
    if (gAiDebugBrake != 0) {
        gCars[v].brake = 1.0f;
        gCars[v].throttle = 0.0f;
        if (gRoadSections[sec].flags & 3) {
            aiPhysics(v);
        }
    } else if (gRoadSections[sec].flags & 3) {
        /* a player can see it: full simulation */
        aiSteer(car, vs);
        aiPassingHorn(car, vs);
        aiPhysics(v);
        if (car->orient[1][1] < 0.01f || (car->unk1AC >= 3 && car->unk1A8 >= 5)) {
            racerResetRoad(car, 1);
        }
    } else {
        behind = sec < gSectionWindows[0].first;
        if (gNumPlayersM1 != 0) {
            behind |= sec < gSectionWindows[1].first;
        }
        aiOffscreen(car, vs, behind);
    }
}

/* 0x8003BA20 ovl2 - one frame of a traffic car */
void trafficUpdate(s32 v) {
    Car *car;

    if (gRaceState == 1) {
        car = &gCars[v];
        if (car->flipping) {
            trafficCrash(car, &gVehicleStates[v], v);
            return;
        }
        if (car->unk84 == 2) {
            trafficDriveSame(car, &gVehicleStates[v], v);
            return;
        }
        if (car->unk84 == 3) {
            trafficDriveOncoming(car, &gVehicleStates[v], v);
        }
    }
}

/* 0x8003BAF4 ovl2 - remove traffic flagged for removal (moves the last one into the gap) */
void trafficCleanup(void) {
    s32 last = gNumVehicles;
    s32 v = gNumPlayers + gNumAiRacers;
    Car *car = &gCars[v];
    VehicleState *vs = &gVehicleStates[v];

    while (v < last) {
        if (vs->remove) {
            if (car->unk84 == 2) {
                gTrafficSame.count--;
            } else {
                gTrafficOncoming.count--;
            }
            last = --gNumVehicles;
            if (v < last) {
                *vs = gVehicleStates[last];
                vs--;
                *car = gCars[gNumVehicles];
                car--;
                car[1].unk7D = v;
                last = gNumVehicles;
                gRenderObjs[v] = gRenderObjs[last];
                v--;
            }
        }
        v++;
        car++;
        vs++;
    }
}

/* 0x8003BCD0 ovl2 - before the start / after the finish: just sit on the road */
void vehicleIdle(s32 v) {
    s32 side = gVehicleStates[v].roadSide;
    Car *car;

    if (gNumPlayersM1 != 0 && gCars[v].unk84 == 3) {
        side += 2;
    }
    car = &gCars[v];
    carGroundUpdate(car, side);
    mtx3RotY(gTmpMtxA, car->heading);
    mtx3Mul(car->orient, gTmpMtxA, car->orient);
}

/* 0x8003BDA4 ovl2 - spawn traffic: same-direction (1 player only, after 10 s)
 * and oncoming (each player, after 20 s and 11000 units). The race leader gets
 * the most traffic. */
void trafficSpawner(void) {
    TrafficDef *def;
    TrafficTimer *same;
    u32 *slot;
    s32 blocked[2];
    s32 i;
    s32 track3;
    s32 b;
    u32 next;
    f32 t;
    s32 pos;
    s32 r;

    if (gTrafficEnabled == 0 || gRaceState != 1) {
        return;
    }
    track3 = gCurrentTrack == 3;
    for (i = 0; i < gNumPlayers; i++) {
        b = track3;
        if (b) {
            b = gSectionWindows[i].first < 0x80;    /* Redwood Forest: none in the first part */
        }
        blocked[i] = b;
    }
    def = &gTrafficDefs[gCurrentTrack];
    t = gRaceTime;
    next = randInt(def->randDelay) + def->minDelay + gFrameCounter;
    if (t >= 10.0f && gNumPlayersM1 == 0 && gTrafficSame.count == 0 && blocked[0] == 0) {
        same = &gTrafficSame;
        if (same == NULL) {                         /* sic: original tests the address, so never true */
            same->next = next;
        }
        if ((u32)gFrameCounter >= same->next) {
            if (gNumVehicles < gMaxVehicles) {
                pos = gRacers[0].position;
                r = randInt(100);
                if (pos == 0 || (pos < 4 && r < 70) || (pos < 7 && r < 60) || r < 35) {
                    trafficSpawnSame(def->kind);
                }
                same->next = 0;
            } else {
                same->next = next;
            }
        }
    }
    if (t >= 20.0f) {
        for (i = 0, slot = gTrafficOncoming.next; i < gNumPlayers; i++, slot++) {
            if (blocked[i] == 0) {
                if (*slot == 0) {
                    *slot = next;
                }
                if ((u32)gFrameCounter >= *slot && gNumVehicles < gMaxVehicles) {
                    if (gCars[i].odometer > 11000.0f) {
                        pos = gRacers[i].position;
                        r = randInt(100);
                        if (pos < 2 || (pos < 4 && r < 75) || (pos < 7 && r < 60) || r < 35) {
                            trafficSpawnOncoming(def->kind, i);
                        }
                        *slot = 0;
                    } else {
                        *slot = next;
                    }
                }
            }
        }
    }
}

/* 0x8003C0CC ovl2 - traffic car crash: tumble (1), then slide to a stop (2) */
void trafficCrash(Car *car, VehicleState *vs, s32 v) {
    s32 heavy = car->unk84 == 4;
    f32 tmp;
    f32 dir;
    f32 slow;
    f32 a;
    f32 b;
    f32 lim;
    f32 step;
    s32 negNow;
    s32 negBefore;
    s32 n;

    if (car->flipping == 1) {
        vs->crashPitch += 2.0f * vs->crashPitchRate;
        vs->crashYaw += 2.0f * vs->crashYawRate;
        if (car->unk84 == 4) {
            mtx3RotX(car->orient, vs->crashPitch);
            mtx3RotY(gTmpMtxA, vs->crashYaw);
            mtx3Mul(car->orient, car->orient, gTmpMtxA);
        } else {
            mtx3RotX(car->orient, vs->crashPitch);
            mtx3RotY(gTmpMtxA, vs->crashYaw);
            mtx3Mul(car->orient, car->orient, gTmpMtxA);
        }
        dir = car->travelDir;
        car->vel[2] = cosf_tbl(dir) * (2.0 * car->speed);
        car->vel[0] = sinf_tbl(dir) * (-2.0 * car->speed);
        vec3Add(car->pos, car->pos, car->vel);
        car->vel[1] -= 4.0f;
        aiBarrierCheck(car);
        carProbesFindGround(car, vs->roadSide);
        tmp = car->probes[0].groundY;
        car->pos[1] += tmp - car->unk40;
        car->unk40 = tmp;
        if (car->vel[1] < 0.0f && carGroundDelta(car, &tmp) != 0 && tmp < 0.0f) {
            /* hit the ground: bounce */
            car->pos[1] += tmp;
            car->vel[1] *= -0.5f;
            car->speed *= 0.5f;
            vs->crashPitchRate *= 0.5f;
            vs->crashYawRate *= 0.5f;
            if (car->vel[1] > -35.0f) {
                car->flipping = 2;
            } else {
                carPlayRandomSfx(car, gCrashLandSfx, 3, 1.0f);
            }
        }
    }
    if (car->flipping == 2) {
        dir = car->travelDir;
        car->vel[2] = cosf_tbl(dir) * car->speed;
        car->vel[0] = sinf_tbl(dir) * -car->speed;
        vs->crashYawRate = car->yawRate;
        carProbesFindGround(car, vs->roadSide);
        slow = heavy ? 0.99f : 0.9f;
        n = 1;
        do {
            car->vel[0] *= slow;
            car->vel[2] *= slow;
            car->pos[0] += car->vel[0];
            car->pos[1] += car->vel[1];
            car->pos[2] += car->vel[2];
            vs->crashPitchRate *= 0.96f;
            vs->crashPitch += vs->crashPitchRate;
            vs->crashYawRate *= 0.98f;
            vs->crashYaw += vs->crashYawRate;
            vs->crashPitch = angleWrap(vs->crashPitch);
            vs->crashYaw = angleWrap(vs->crashYaw);
            car->yawRate = vs->crashYawRate;
            car->speed *= slow;
            if (car->speed <= 10.0f) {
                a = car->yawRate;
                if (a < 0.0f) {
                    a = -a;
                }
                b = vs->crashPitchRate;
                if (b < 0.0f) {
                    b = -b;
                }
                tmp = (a + b) * 10.0f;
                lim = heavy ? 0.08f : 0.02f;
                if (tmp <= lim) {
                    a = vs->crashPitch;
                    tmp = (a < 0.0f) ? -a : a;
                    if (tmp <= 0.2) {
                        vs->crashPitch = 0.0f;       /* settled on its wheels */
                        break;
                    }
                    if (2.95 <= tmp) {
                        break;                       /* settled on its roof */
                    }
                }
                /* rock towards upright or upside down */
                a = vs->crashPitch;
                if (a < 0.0f) {
                    step = (a < -1.57f) ? -0.01f : 0.01f;
                } else {
                    step = (a < 1.57f) ? -0.01f : 0.01f;
                }
                step *= 0.4f;
                b = vs->crashPitchRate;
                vs->crashPitchRate = b + step;
                negNow = FALSE;
                if (vs->crashPitchRate < 0.0f) {
                    negNow = TRUE;
                }
                negBefore = FALSE;
                if (b < 0.0f) {
                    negBefore = TRUE;
                }
                if (negNow != negBefore) {
                    a = angleWrap(vs->crashPitch);
                    if (a < 0.0f) {
                        a = -a;
                    }
                    if (0.08 <= a && a <= 3.06f) {
                        carPlaySfx(car, 2, 1.0f);
                    }
                }
            }
        } while (n-- != 0);                  /* runs twice: two half-steps per frame */
        aiBarrierCheck(car);
        mtx3RotX(car->orient, vs->crashPitch);
        mtx3RotY(gTmpMtxA, vs->crashYaw);
        mtx3Mul(car->orient, car->orient, gTmpMtxA);
        if (carGroundDelta(car, &tmp) != 0) {
            car->pos[1] -= tmp;
        }
    }
    car->heading = vs->crashYaw;
    if (!(gRoadSections[gPathPoints[car->road.point].section].flags & 3)) {
        vs->remove = 1;                      /* out of sight: remove */
    }
}

/* 0x8003C7B8 ovl2 - engine + steering + movement for an AI racer (as for players) */
void aiPhysics(s32 v) {
    Car *car;

    if (gRaceState == 1) {
        car = &gCars[v];
        car->gear = autoGearbox(car->gear, car->rpm, car->throttle);
        carEngineUpdate(car);
        carSlideUpdate(car);
        carRpmUpdate(v);
        carSteerUpdate(car);
        aiBarrierCheck(car);
        mtx3RotY(gTmpMtxA, car->yawDelta + car->unk50);
        mtx3Mul(car->orient, gTmpMtxA, car->orient);
        mtx3RotY(gTmpMtxB, car->travelDir - car->heading);
        mtx3Mul(gTmpMtxB, gTmpMtxB, car->orient);
        if (car->airborne == 0) {
            vec3Forward(car->vel, car->frameDist * car->speedScale, gTmpMtxB);
        }
        vec3Add(car->pos, car->pos, car->vel);
        vehicleIdle(v);
    }
}

/* 0x8003C90C ovl2 - AI racer crossing the finish: coast a staggered distance, then park.
 * Returns TRUE if the AI was handled here. */
s32 aiFinishCheck(Car *car, VehicleState *vs) {
    s32 sec = gPathPoints[car->road.point].section;
    s32 off;

    if ((gTrack->sections[sec].flags & 0x3F) == gCurrentTrack + 0x20 && vs->finishDist == 0.0f) {
        vs->laneOffset = randFloat() * 1.4f - 0.7f;
        gRaceUnk74++;
        vs->finishDist = (f32)(gNumAiRacers + gNumPlayers - gRaceUnk74) * 2800.0f + 3500.0f;
    }
    if (vs->finishDist == 0.0f) {
        return FALSE;
    }
    vs->finishDist -= 2.0f * car->speed;
    if (vs->finishDist >= 0.0f) {
        aiPhysics(car->unk7D);
        return TRUE;
    }
    /* parked */
    if (vs->offscreen && car->speed != 0.0f) {
        car->pos[0] += randFloat() * 600.0f - 300.0f;
        car->pos[2] += randFloat() * 600.0f - 300.0f;
    }
    car->speed = 0.0f;
    car->enginePower = 0.0f;
    off = vs->offscreen;
    if (off && (gRoadSections[sec].flags & 3)) {
        f32 y = car->road.pos[1];          /* back in view: put it on the road */
        off = 0;
        car->probes[0].y = y;
        car->pos[1] = y;
        vs->offscreen = 0;
    }
    if (off == 0) {
        vs->timer = -1;
        car->heading = car->road.heading + vs->laneOffset;
        aiPhysics(car->unk7D);
        return TRUE;
    }
    return FALSE;
}

/* 0x8003CB74 ovl2 - sound when an AI car overtakes / is overtaken by a player */
void aiPassingHorn(Car *car, VehicleState *vs) {
    Car *p;
    f32 dv;
    f32 side;
    f32 lat;
    f32 vol;
    s32 t;
    s32 id;
    s32 i;

    t = vs->hornCooldown;
    if (t != 0) {
        vs->hornCooldown = t - 1;
        if (t < 0) {
            vs->hornCooldown = 0;
        }
        return;
    }
    for (i = 0, p = gCars; i < gNumPlayers; i++, p++) {
        dv = car->speed - p->speed;
        if (dv <= 40.0f) {
            if (dv < 0.0f) {
                dv = -dv;
            }
            if (dv < 20.0f) {
                continue;
            }
            if (dv > 60.0f) {
                dv = 60.0f;
            }
            side = cameraSideOffset(car->pos[0], car->pos[2], i);
            if (side < 0.0f || 3000.0f < side) {
                continue;
            }
        } else {
            side = cameraSideOffset(car->pos[0], car->pos[2], i);
            if (side < 1500.0f || 5000.0f < side) {
                continue;
            }
        }
        lat = car->roadOffset - p->roadOffset;
        if (lat < 0.0f) {
            lat = -lat;
        }
        if (lat < 1.0f || (vol = 4000.0f / lat) > 1.0f) {
            vol = 1.0f;
        }
        if (dv <= 40.0f) {
            id = 0x78;
        } else {
            id = 0x39;
            if (dv > 60.0f && ((s32)dv & 1)) {
                id = 0x79;
            }
        }
        sfxPlayVolume(id, i, vol);
        vs->hornCooldown = 40;
    }
}

/* 0x8003CDD0 ovl2 - AI driving: follow the route, pick a lane, steer */
void aiSteer(Car *car, VehicleState *vs) {
    PathPoint *pt;
    f32 dist;
    f32 sp;
    f32 frames;
    f32 rh;
    f32 lane;
    f32 d;
    f32 sx;
    f32 ang;
    f32 f;
    f32 s;
    f32 k;

    if (vs->offscreen) {
        aiLeaveOffscreen(car, vs);
    }
    if (gRaceState != 1) {
        return;
    }
    dist = aiPathAhead(car, vs, 0);
    pt = &gPathPoints[vs->pathTarget];
    sp = 2.0f * car->speed;
    if (sp < 60.0f) {
        sp = 60.0f;
    }
    frames = dist / sp;
    aiBuildLaneMap(vs);
    aiMarkObstacles(car);
    aiChooseLane(car, vs);
    rh = roadHeadingAt(vs->pathTarget);
    lane = vs->laneTarget;
    if (gChicagoPillars != 0 &&
        ((d = lane - car->roadOffset, lane < 0.0f && d < 0.0f) || (lane > 0.0f && d > 0.0f))) {
        lane += d * 1.6f;
    }
    sx = cosf_tbl(rh) * lane;
    ang = ((f32)(s16)(0xC000 - atan2s((pt->x + sx * 0.2f) - car->pos[0] * 0.2f,
                                       (pt->z - sinf_tbl(rh) * lane * 0.2f) + car->pos[2] * 0.2f))
           * 3.1415927f) / 32768.0f;
    if (car->odometer == 0.0f) {
        car->heading = ang;
    }
    d = angleWrap(ang - car->heading);
    f = (f32)((s32)frames - 1);
    if (0.001f < f) {
        d = d / f;
    }
    aiRubberBand(car, vs);
    if (gRaceState == 1) {
        car->throttle = vs->throttle;
    }
    sp = car->speed;
    if (sp < 0.01f) {
        car->steerYaw = 0.0f;
    } else {
        s = CLAMP(d * 2.5f, -0.09f, 0.09f);
        k = s / (sp * -0.0010816f);
        if (k < -0.3f) {
            car->steerYaw = 0.3f;
        } else {
            car->steerYaw = -((0.3f < k) ? 0.3f : k);
        }
    }
    if (car->flipping) {
        car->flipping = 0;
    }
}

/* 0x8003D138 ovl2 - nobody can see this AI car: slide it along the route */
void aiOffscreen(Car *car, VehicleState *vs, s32 behind) {
    PathPoint *pt;
    f32 d;
    f32 inv;
    f32 dx;
    f32 dz;
    f32 h;
    f32 step;
    f32 y;
    s32 v;

    vs->offscreen = 1;
    vs->timer = -1;
    v = car->unk7D;
    d = aiPathAhead(car, vs, 0);
    inv = d;
    if (0.01f < d) {
        inv = 1.0f / d;
    }
    pt = &gPathPoints[vs->pathTarget];
    dx = pt->x * 5.0f - car->pos[0];
    dz = -pt->z * 5.0f - car->pos[2];
    carRpmUpdate(v);
    car->gear = autoGearbox(car->gear, car->rpm, car->throttle);
    aiRubberBand(car, vs);
    aiOffscreenEngine(car, vs);
    h = car->road.heading;
    step = inv * (2.0f * car->speed);
    y = car->road.pos[1];
    car->pos[0] += dx * step;
    car->heading = h;
    car->travelDir = h;
    car->pos[1] = y;
    car->probes[0].y = y;
    car->pos[2] += dz * step;
}

/* 0x8003D290 ovl2 - rubber band: AI speeds up behind / slows down ahead of the leading player */
void aiRubberBand(Car *car, VehicleState *vs) {
    Car *target;
    RacerInfo *info;
    f32 d;
    f32 sign;
    f32 r;
    s32 diff;

    if (--vs->rubberTimer < 0) {
        vs->rubberTimer = randInt(35) + 10;
        target = &gCars[0];
        info = &gRacers[0];
        if (gNumPlayersM1 != 0 && gCars[0].road.point < gCars[1].road.point) {
            target = &gCars[1];
            info = &gRacers[1];
        }
        d = dist2D(target->pos[0], target->pos[2], car->pos[0], car->pos[2]) * 7e-06;
        if (0.4f < d) {
            d = 0.4f;
        }
        diff = (car->road.point - target->road.point) >> 2;
        if (diff == 0) {
            sign = 0.0f;
        } else if (diff > 0) {
            sign = -1.0f;                 /* AI ahead: ease off */
            if (d < 0.1f) {
                d = 0.1f;
            }
        } else {
            sign = 1.0f;                  /* AI behind: catch up */
            if (d < 0.2f) {
                d = 0.2f;
            }
        }
        d *= sign;
        if (info->framesLeading != 0) {
            r = randFloat() * ((f32)info->framesLeading * 0.0015f);
            if (0.15f < r) {
                r = 0.15f;
            }
            d += r;                       /* a player leading for a long time makes the AI push */
        }
        vs->rubberBand = vs->rubberGain * d + 1.0f;
    }
    if (--vs->varTimer < 0) {
        vs->varTimer = randInt(90) + 30;
        vs->throttleVar = randFloat() * 0.12f + 1.0f;
    }
    vs->throttle = vs->throttleVar * vs->rubberBand;
}

/* 0x8003D4D0 ovl2 - AI engine power multiplier: track bonus + difficulty + players' skill level */
f32 aiSkillFactor(void) {
    f32 s = gAiSkill[0];
    f32 s2;
    f32 r;

    if (gNumPlayersM1 != 0) {
        s2 = CLAMP(gAiSkill[1], 0.8f, 1.16f);
        s = (s2 + s) * 0.5f;
    }
    s = CLAMP(s, 0.8f, 1.16f);
    r = gTrackAiBonus[gCurrentTrack] + ((f32)gDifficultyAi[gDifficulty * 3] * 0.04f + 0.8f + (s - 1.0f));
    return CLAMP(r, 0.75f, 1.25f);
}

/* 0x8003D644 ovl2 - barrier collisions for AI/traffic, only near a player */
void aiBarrierCheck(Car *car) {
    VehicleState *vs;
    RacerInfo *info;
    s32 side;
    s32 v;
    f32 d;

    if (gRaceState == 1) {
        v = car->unk7D;
        vs = &gVehicleStates[v];
        side = vs->roadSide;
        d = (f32)cameraDistance2(side, v) * 0.000015258789f;
        info = &gRacers[side];
        if (d < 32000.0f && info->pathBehind + 2 < car->road.point && car->road.point < info->pathAhead - 2) {
            carCheckProbes(car);
            if (car->unk80 == 2) {
                if (car->unk84 == 4) {
                    vs->crashPitchRate = 0.0f;
                    vs->crashYawRate = 0.0f;
                    car->yawRate = 0.0f;
                    car->speed = 0.0f;
                    return;
                }
                if (d < gDrawDistance[side] * 0.9f) {
                    carHitBarrier(car, side, 0);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Lane choice. The road ahead of an AI car is a 50-slot map across   */
/* the road (gLaneMap, 100 units per slot, slot 25 = road centre).    */
/* Each slot holds 0 (free), 0x1000 (permanently blocked) or a flag   */
/* (0x2000 AI/traffic, 0x4000 player 1, 0x8000 player 2) plus the     */
/* number of frames until this car would reach that obstacle.         */
/* ------------------------------------------------------------------ */

extern u8 gCarAnim[];                    /* 0x80043A50 ovl2, 0xF8 per car model */
#define MODEL_F32(model, off) (*(f32 *)&gCarAnim[(model) * 0xF8 + (off)])

/* 0x8003D7BC ovl2 - clear the lane map and block lanes that can't be used here */
void aiBuildLaneMap(VehicleState *vs) {
    PathPoint *pt;
    s32 sec;
    s32 v;
    s32 i;
    s32 start;

    bytesClear((u8 *)gLaneMap, sizeof(gLaneMap));
    pt = &gPathPoints[vs->pathTarget];
    sec = pt->section;
    if (gCurrentTrack == 10) {               /* Chicago */
        gChicagoPillars = 0;
        v = vs->vehicle + 1;
        if (sec >= 77) {
            if (sec >= 110) {
                if (v < 3 || !(v & 1)) {
                    for (i = 0; i < 14; i++) {      /* both road edges */
                        gLaneMap[i] = 0x1000;
                        gLaneMap[i + 36] = 0x1000;
                    }
                } else {
                    for (i = 0; i < 5; i++) {
                        gLaneMap[i + 9] = 0x1000;
                        gLaneMap[i + 36] = 0x1000;
                    }
                }
            } else {
                /* under the elevated railway: odd and even cars keep to different halves */
                gChicagoPillars = 1;
                start = (v & 1) ? 0 : 22;
                for (i = 0; i < 28; i++) {
                    gLaneMap[start + i] = 0x1000;
                }
            }
        }
    } else if (pt->lanes == 2) {              /* ordinary 2-lane road: only the middle of the map is usable */
        for (i = 0; i < 12; i++) {
            gLaneMap[i] = 0x1000;
            gLaneMap[i + 38] = 0x1000;
        }
    }
}

/* 0x8003D92C ovl2 - put every other vehicle (and the players) into the lane map */
void aiMarkObstacles(Car *car) {
    s32 i;
    Car *other;

    for (i = gNumPlayers, other = &gCars[i]; i < gNumVehicles; i++, other++) {
        if (car != other && gVehicleStates[i].timer == 0) {
            aiMarkVehicle(car, other, 0x2000);
        }
    }
    aiMarkVehicle(car, &gCars[0], 0x4000);
    if (gNumPlayersM1 != 0) {
        aiMarkVehicle(car, &gCars[1], 0x8000);
    }
}

/* 0x8003DA34 ovl2 - predict when `car` would hit `other`; if it will within
 * 200 frames, mark the lanes `other` will occupy then. Returns the frames
 * until contact, or 0x800 if there is no danger.
 */
s32 aiMarkVehicle(Car *car, Car *other, s32 flag) {
    f32 dx;
    f32 dz;
    f32 d2;
    f32 size;
    f32 lat;
    f32 d0;
    f32 closing;
    f32 rate;
    f32 ax;
    f32 az;
    f32 t;
    f32 lane;
    f32 w;
    s32 n;
    s32 slot;

    if (carCompareAhead(car, other) < 0) {
        return 0x800;                        /* it's behind us */
    }
    dx = other->pos[0] - car->pos[0];
    dz = other->pos[2] - car->pos[2];
    d2 = dx * dx + dz * dz;
    if (900000000.0f <= d2) {
        return 0x800;                        /* more than 30000 away */
    }
    size = MODEL_F32(other->model, 0x58) + MODEL_F32(car->model, 0x58) + 200.0f;
    lat = car->roadOffset - other->roadOffset;
    if (lat < 0.0f) {
        lat = -lat;
    }
    if (d2 <= size * size && 400.0f <= lat) {
        t = 10.0f;                           /* alongside in another lane */
    } else {
        d0 = sqrtf(d2);
        dx += sinf_tbl(car->travelDir) * car->speed;
        dz -= cosf_tbl(car->travelDir) * car->speed;
        dx -= sinf_tbl(other->travelDir) * other->speed;
        dz += cosf_tbl(other->travelDir) * other->speed;
        closing = sqrtf(dx * dx + dz * dz) - d0;
        if (-0.001f <= closing) {
            return 0x800;                    /* not getting closer */
        }
        rate = ((closing < 0.0f) ? -closing : closing) * 1.5f;
        ax = (dx < 0.0f) ? -dx : dx;
        ax = (ax < 400.0f) ? 0.0f : ax - 400.0f;
        az = (dz < 0.0f) ? -dz : dz;
        az = (az < 1000.0f) ? 0.0f : az - 1000.0f;
        t = sqrtf(ax * ax + az * az) / rate;
        if (200.0f < t) {
            return 0x800;
        }
    }
    flag = (s32)((f32)flag + t);
    lane = (other->roadOffset - aiSideSpeed(other) * t) * 0.01f;
    w = aiCarWidth(other) * 0.01f;
    if (w < 0.0f) {
        w = -w;
    }
    if (20.0f < w) {
        w = 20.0f;
    }
    n = (s32)(w + 3.0f);
    slot = (s32)(lane + 25.0f) - (n >> 1);
    for (; n != -1; n--) {
        if (slot >= 0 && slot < 50) {
            gLaneMap[slot] = flag;
        }
        slot++;
    }
    return (s32)t;
}

/* 0x8003DE34 ovl2 - sideways speed across the road */
f32 aiSideSpeed(Car *car) {
    return car->speed * sinf_tbl(car->road.heading - car->travelDir) * 1.5f;
}

/* 0x8003DE7C ovl2 - width of the car across the road (depends on its angle to the road) */
f32 aiCarWidth(Car *car) {
    f32 len;
    f32 wid;
    f32 a;
    f32 s;
    f32 c;

    len = MODEL_F32(car->model, 0x50) - MODEL_F32(car->model, 0x5C);
    wid = MODEL_F32(car->model, 0x58) - MODEL_F32(car->model, 0x70);
    a = car->road.heading - car->travelDir;
    s = sinf_tbl(a);
    c = cosf_tbl(a);
    if (s < 0.0f) {
        s = -s;
    }
    if (c < 0.0f) {
        c = -c;
    }
    return c * len + s * wid;
}

/* 0x8003DF2C ovl2 - a player can see this AI car again: put it back on the road properly */
void aiLeaveOffscreen(Car *car, VehicleState *vs) {
    f32 h;

    vs->timer = 0;
    vs->offscreen = 0;
    h = car->road.heading;
    car->spinTimer = 0;
    car->spinRemaining = 0.0f;
    car->yawRate = 0.0f;
    car->travelDir = h;
    car->heading = h;
    mtx3RotY(car->orient, h);
    car->probes[0].y = car->road.pos[1] - 1000.0f;
    aiPathAhead(car, vs, 0);
}

/* 0x8003DFA8 ovl2 - cheap engine model for off-screen AI (no probes, no slide) */
void aiOffscreenEngine(Car *car, VehicleState *vs) {
    s32 g;
    f32 power;
    f32 rpm;
    f32 over;
    f32 drag;
    f32 v1;
    f32 v2;

    if (gRaceState == 1) {
        car->throttle = vs->throttle;
    } else {
        car->throttle = 0.0f;
    }
    power = tableLerp(gTorqueCurve, (f32)car->rpm * 0.333f, 20);
    g = car->gear;
    power = power * gGearPower[g] * 0.97f * (car->throttle * car->enginePower);
    rpm = (f32)car->rpm;
    over = (rpm <= 42.0f) ? 0.0f : rpm - 42.0f;
    drag = car->dragRoad + gGearRevDrag[g] * (over * 0.04f + 1.0f);
    v1 = (car->speed - car->speed * drag) + power;
    v2 = (v1 - v1 * drag) + power;
    car->speed = v2;
    car->frameDist = 1.5f * (v1 + v2);
}

/* 0x8003E0F4 ovl2 - advance vs->pathTarget to the first route point at least
 * 200 units (1000 in route units) ahead (or behind) of the car. Returns the
 * distance to it. Running off either end of the route marks the car for removal.
 */
f32 aiPathAhead(Car *car, VehicleState *vs, s32 backwards) {
    s32 p;
    s32 step;
    s32 n;
    PathPoint *pt;
    f32 dx;
    f32 dz;
    f32 d2;

    if (backwards) {
        p = vs->pathTarget;
        step = -1;
        if (p >= car->road.point) {
            p = car->road.point - 1;
            vs->pathTarget = p;
        }
        if (p < 3) {
            vs->remove = 1;
            return 1000.0f;
        }
        n = p;
    } else {
        p = vs->pathTarget;
        if (car->road.point >= p) {
            p = car->road.point + 1;
            vs->pathTarget = p;
        }
        step = 1;
        n = gPathLength - p - 1;
        if (n <= 0) {
            vs->remove = 1;
            return 1000.0f;
        }
    }
    while (n-- != 0) {
        pt = &gPathPoints[vs->pathTarget];
        dx = pt->x - car->road.pos[0] * 0.2f;
        dz = pt->z + car->road.pos[2] * 0.2f;
        d2 = dx * dx + dz * dz;
        if (1000000.0f <= d2) {
            break;
        }
        vs->pathTarget += step;
    }
    return sqrtf(d2) * 5.0f;
}

/* 0x8003E260 ovl2 - brake for a player just ahead, and pick the nearest clear lane */
void aiChooseLane(Car *car, VehicleState *vs) {
    s32 slot;
    s32 i;
    s32 end;
    s32 run;
    s32 runL;
    s32 right;
    s32 left;
    s32 flags;
    s32 t;
    f32 ps;
    f32 sp;

    vs->braking = 0;
    slot = (s32)(car->roadOffset * 0.01f) + 25;
    if (slot < 4) {
        slot = 4;
    } else if (slot >= 47) {
        slot = 46;
    }
    flags = 0;
    for (i = -4; i < 5; i++) {
        flags |= gLaneMap[slot + i];
    }
    if (flags == 0) {
        return;                              /* our lane is clear */
    }
    runL = 0;
    if (flags & 0xC000) {
        t = flags & 0xFFF;
        if (t < 21 && car->model != 14) {
            ps = gCars[0].speed;
            if ((flags & 0x8000) && gCars[1].speed < ps) {
                ps = gCars[1].speed;
            }
            sp = car->speed;
            if (ps - sp <= 10.0f && 40.0f < sp) {
                car->speed = sp * (0.8f + (f32)t * 0.01f);   /* don't ram the player */
                vs->braking = 1;
            }
        }
    }
    /* search right for 9 free slots in a row */
    end = slot + 30;
    run = 0;
    if (end >= 51) {
        end = 50;
    }
    for (i = slot - 4; i < end; i++) {
        if (gLaneMap[i] != 0) {
            run = 0;
        }
        if (run++ == 8) {
            break;
        }
    }
    right = (run >= 8) ? i - 3 : 0;
    /* and left */
    end = slot - 30;
    if (end < 0) {
        end = -1;
    }
    for (i = slot + 4; i > end; i--) {
        if (gLaneMap[i] != 0) {
            runL = 0;
        }
        if (runL++ == 8) {
            break;
        }
    }
    left = (runL >= 8) ? i + 3 : 0;
    if ((left | right) != 0) {
        if (right != 0 && (left == 0 || right - slot < slot - left)) {
            i = right;
        } else {
            i = left;
        }
        vs->laneTarget = (f32)(i - 25) * 100.0f;
    }
}

/* ------------------------------------------------------------------ */
/* Traffic                                                            */
/* ------------------------------------------------------------------ */

typedef struct OncomingDef {             /* 4 bytes, 0x80044CC0 ovl2 */
    /* 0x00 */ u8  model;
    /* 0x01 */ u8  weaves;                    /* drifts from side to side */
    /* 0x02 */ s16 passSfx;                   /* sound when it just misses a player */
} OncomingDef;
extern u8 gTrafficSameModels[16];        /* 0x80044CB0 ovl2 */
extern OncomingDef gOncomingDefs[17];    /* 0x80044CC0 ovl2 */
extern s32 gTrafficHornSfx[];            /* 0x80044AC8 ovl2, per car model */
extern s32 gNearMissSfx[6];              /* 0x80044D04 ovl2 */

s32  vehicleSpawn(s32 point, s32 oncoming, f32 lane, s32 model); /* 0x80035D1C ovl2 */
void carSparks(Car *car, Car *other, s32 front, s32 back, s32 side); /* 0x8003798C ovl2 */
void vec3MulMtx(Vec3f out, Vec3f v, Mtx3 m);
void trafficEndCheck(Car *car, VehicleState *vs);
s32  carAllWheelsOffroad(Car *car);
s32  trafficCarInLine(Car *car, Car *other);
void trafficNewWeave(VehicleState *vs);
void trafficNewWeaveCentre(VehicleState *vs);
void trafficSteer(Car *car, VehicleState *vs, s32 oncoming);
f32  trafficPickLane(s32 point);

/* 0x8003E4DC ovl2 - same-direction traffic appears 2 route points behind player 1 */
void trafficSpawnSame(u8 kind) {
    s32 model;
    s32 p;
    f32 lane;
    s32 v;
    Car *car;
    VehicleState *vs;
    s32 sfx;

    model = gTrafficSameModels[randInt(kind ? 14 : 16)];
    gTrafficSame.count++;
    p = roadCurrentPoint(0) - 2;
    if (p > gPathLength - 10) {
        return;
    }
    lane = trafficPickLane(p);
    v = vehicleSpawn(p, 0, lane, model);
    vs = &gVehicleStates[v];
    car = &gCars[v];
    car->unk84 = 2;
    car->gear = 1;
    vs->pathTarget = p + 2;
    vs->offroadTimer = 0;
    vs->weaving = 0;
    vs->passSfx[1] = 0;
    vs->passSfx[0] = 0;
    car->speed = 50.0f;
    vs->lane = lane;
    sfx = gTrafficHornSfx[car->model];
    vs->remove = 0;
    vs->hornSfx[1] = sfx;
    vs->hornSfx[0] = sfx;
    if (randInt(10) == 0) {
        vs->weaving = 1;
        vs->unk7A = randInt(50) + 20;
    }
}

/* 0x8003E64C ovl2 - oncoming traffic appears ahead of `player` */
void trafficSpawnOncoming(s32 kind, s32 player) {
    s32 p;
    s32 r;
    s32 v;
    f32 lane;
    OncomingDef *def;
    Car *car;
    VehicleState *vs;
    s32 sfx;
    s32 bit;
    RacerInfo *other;

    p = roadCurrentPoint(player) - 2;
    if (p > gPathLength - 10) {
        return;
    }
    if (gNumPlayersM1 != 0) {
        other = &gRacers[1 - player];
        if (gRacers[player].position < other->position) {
            /* this player is ahead: not while the other one is still near the start */
            if (gCars[1 - player].road.point < 50 && gCars[player].road.point < gCars[1 - player].road.point + 50) {
                return;
            }
        } else {
            bit = (other == &gRacers[1]) ? 2 : 1;
            if (gRoadSections[gPathPoints[p].section].flags & bit) {
                return;                      /* the other player would see it appear */
            }
        }
    }
    r = randInt(kind ? 14 : 17);
    gTrafficOncoming.count++;
    lane = trafficPickLane(p);
    def = &gOncomingDefs[r];
    v = vehicleSpawn(p, 1, lane, def->model);
    car = &gCars[v];
    vs = &gVehicleStates[v];
    vs->passSfx[1] = def->passSfx;
    vs->passSfx[0] = def->passSfx;
    car->unk84 = 3;
    car->gear = 1;
    vs->pathTarget = p - 2;
    vs->weaving = 0;
    car->speed = 50.0f;
    vs->lane = lane;
    sfx = gTrafficHornSfx[car->model];
    vs->remove = 0;
    vs->hornSfx[1] = sfx;
    vs->hornSfx[0] = sfx;
    if (60.0f < gRaceTime && def->weaves) {
        vs->weaving = 1;
        vs->unk7A = 30;
        trafficNewWeave(vs);
        trafficNewWeaveCentre(vs);
    }
}

/* 0x8003E8E8 ovl2 - same-direction traffic, once per frame */
void trafficDriveSame(Car *car, VehicleState *vs, s32 vehicle) {
    s32 i;
    s32 t;
    Car *pc;
    f32 dx;
    f32 dz;
    f32 d2;
    f32 th;

    if (gPathPoints[car->road.point].lanes == 2) {
        vs->lane = 575.0f;                   /* 2-lane road: the only lane */
    }
    if (carAllWheelsOffroad(car) && vs->offroadTimer == 0) {
        vs->offroadTimer++;
    }
    t = vs->offroadTimer;
    if (t >= 2) {
        /* crashed off the road: sparks once, then just wait to be removed */
        vs->offroadTimer = t + 1;
        if (t >= 12 && t == 12) {
            carSparks(car, NULL, 0, 1, 0);
        }
        trafficEndCheck(car, vs);
        return;
    }
    if (car->hitFlag) {
        car->hitFlag = 0;
        carSparks(car, NULL, 0, 1, 0);
    }
    for (i = 0, pc = &gCars[0]; i < gNumPlayers; i++, pc++) {
        if (vs->hornSfx[i] != 0 && carCompareAhead(car, pc) <= 0) {
            dx = car->pos[0] - pc->pos[0];
            dz = car->pos[2] - pc->pos[2];
            vs->prevPlayerDist2[i] = vs->playerDist2[i];
            d2 = dx * dx + dz * dz;
            vs->playerDist2[i] = d2;
            /* a fast player closing in from behind within 5000: honk once */
            if (d2 < 25000000.0f && d2 < vs->prevPlayerDist2[i] && 127.0f <= pc->speed) {
                carPlaySfx(car, vs->hornSfx[i], 1.0f);
                vs->hornSfx[i] = 0;
            }
        }
    }
    trafficSteer(car, vs, 0);
    if (trafficCarInLine(car, &gCars[0]) || (gNumPlayersM1 != 0 && trafficCarInLine(car, &gCars[1]))) {
        car->throttle *= 0.01f;              /* a player is in our line: back off */
    } else {
        th = vs->throttle * 1.01f;
        if (th < 0.35f) {
            vs->throttle = 0.35f;
        } else {
            vs->throttle = (0.5f < th) ? 0.5f : th;
        }
        car->throttle = vs->throttle;
    }
    trafficEndCheck(car, vs);
}

/* 0x8003EBB4 ovl2 - oncoming traffic, once per frame */
void trafficDriveOncoming(Car *car, VehicleState *vs, s32 vehicle) {
    s32 twoLane;
    s32 calm;
    s32 i;
    Car *pc;
    f32 dx;
    f32 dz;
    f32 d2;
    f32 th;

    twoLane = gPathPoints[car->road.point].lanes == 2;
    if (twoLane) {
        vs->lane = 575.0f;
    }
    if (vs->weaving) {
        calm = FALSE;
        if (gRacers[0].position >= 5 && (gNumPlayersM1 == 0 || gRacers[1].position >= 5)) {
            calm = TRUE;                     /* players are well back: don't weave */
        } else if (twoLane) {
            calm = TRUE;
        }
        if (calm) {
            vs->lane = trafficPickLane(car->road.point);
        } else {
            if (randInt(10) == 0) {
                trafficNewWeave(vs);
            }
            if (randInt(5) == 0) {
                trafficNewWeaveCentre(vs);
            }
            vs->weavePhase = angleWrap(vs->weavePhase + vs->weaveSpeed);
            vs->lane = sinf_tbl(vs->weavePhase) * vs->weaveSize + vs->weaveCentre;
        }
    }
    for (i = 0, pc = &gCars[0]; i < gNumPlayers; i++, pc++) {
        if (carCompareAhead(car, pc) <= 0) {
            dx = car->pos[0] - pc->pos[0];
            dz = car->pos[2] - pc->pos[2];
            d2 = dx * dx + dz * dz;
            vs->prevPlayerDist2[i] = vs->playerDist2[i];
            vs->playerDist2[i] = d2;
            if (vs->passSfx[i] != 0 && !roadIsLeft(&pc->road) && trafficCarInLine(car, pc)) {
                if (randInt(5) < 4) {
                    carPlaySfx(car, vs->passSfx[i], 1.0f);
                } else {
                    carPlayRandomSfx(pc, gNearMissSfx, 6, 1.0f);
                }
                vs->passSfx[i] = 0;
            }
            if (vs->hornSfx[i] != 0 && d2 < 25000000.0f && d2 < vs->prevPlayerDist2[i] && 50.0f <= car->speed) {
                carPlaySfx(car, vs->hornSfx[i], 1.0f);
                vs->hornSfx[i] = 0;
            }
        }
    }
    trafficSteer(car, vs, 1);
    if (trafficCarInLine(car, &gCars[0]) || (gNumPlayersM1 != 0 && trafficCarInLine(car, &gCars[1]))) {
        car->throttle *= 0.01f;
    } else {
        th = vs->throttle * 1.01f;
        if (th < 0.35f) {
            vs->throttle = 0.35f;
        } else {
            vs->throttle = (1.0f < th) ? 1.0f : th;
        }
        car->throttle = vs->throttle;
    }
    trafficEndCheck(car, vs);
}

/* 0x8003EF44 ovl2 - physics, and remove the car once its road section is unloaded */
void trafficEndCheck(Car *car, VehicleState *vs) {
    aiPhysics(car->unk7D);
    if (!(gRoadSections[gPathPoints[car->road.point].section].flags & 3)) {
        vs->remove = 1;
    }
}

/* 0x8003EFC4 ovl2 - TRUE if probes 1-4 (the wheels) are all off the road */
s32 carAllWheelsOffroad(Car *car) {
    if (!car->probes[1].offroad) {
        return FALSE;
    }
    if (!car->probes[2].offroad) {
        return FALSE;
    }
    if (!car->probes[3].offroad) {
        return FALSE;
    }
    if (!car->probes[4].offroad) {
        return FALSE;
    }
    return TRUE;
}

/* 0x8003F024 ovl2 - TRUE if `other` is within 15000 and less than 400 to the side of car's line of travel */
s32 trafficCarInLine(Car *car, Car *other) {
    Vec3f v;
    f32 dx;
    f32 dz;
    f32 nx;
    f32 nz;
    f32 c;
    f32 len;
    f32 d;

    dx = other->pos[0] - car->pos[0];
    dz = other->pos[2] - car->pos[2];
    if (225000000.0f <= dx * dx + dz * dz) {
        return FALSE;
    }
    v[0] = 0.0f;
    v[1] = 0.0f;
    v[2] = car->speed * 10.0f;
    mtx3RotY(gTmpMtxA, car->travelDir);
    vec3MulMtx(v, v, gTmpMtxA);
    nx = -v[2];
    nz = v[0];
    c = -(car->pos[0] * nx + car->pos[2] * nz);
    len = sqrtf(nx * nx + nz * nz);
    if (!(0.01f < len)) {
        return FALSE;
    }
    d = (c + (other->pos[0] * nx + other->pos[2] * nz)) / len;
    if (d < 0.0f) {
        d = -d;
    }
    return (d <= 400.0f) ? TRUE : FALSE;
}

/* 0x8003F19C ovl2 */
void trafficNewWeave(VehicleState *vs) {
    vs->weaveSpeed = randFloat() * 0.1f + 0.25f;
    vs->weaveSize = randFloat() * 520.0f + 2600.0f;
}

/* 0x8003F200 ovl2 */
void trafficNewWeaveCentre(VehicleState *vs) {
    vs->weaveCentre = randFloat() * 1152.0f - 576.0f;
}

/* 0x8003F240 ovl2 - steer towards the next route point, offset sideways by vs->lane */
void trafficSteer(Car *car, VehicleState *vs, s32 oncoming) {
    f32 dist;
    f32 sp;
    f32 frames;
    PathPoint *pt;
    f32 dx;
    f32 dz;
    f32 a;
    f32 ox;
    f32 d;

    /* the original passes its own third argument on (it never touches $a2):
     * oncoming traffic looks for its target behind it on the route */
    dist = aiPathAhead(car, vs, oncoming);
    sp = car->speed + car->speed;
    if (sp < 60.0f) {
        sp = 60.0f;
    }
    frames = dist / sp;
    pt = &gPathPoints[vs->pathTarget];
    dx = pt->x * 5.0f - car->pos[0];
    dz = pt->z * -5.0f - car->pos[2];
    a = ((f32)(s16)(atan2s(dx, dz) + 0xC000) * 3.1415927f) / 32768.0f;
    ox = cosf_tbl(a) * vs->lane;
    a = ((f32)(s16)(atan2s(dx + ox, dz + sinf_tbl(a) * vs->lane) + 0xC000) * 3.1415927f) / 32768.0f;
    d = angleWrap(car->heading - a);
    if (1.001f < frames) {
        d = d / (1.0f - frames);
    }
    car->steerYaw = d * 1.95f;
}

/* 0x8003F400 ovl2 - a lane for new traffic: 575 right of centre, or 575/1725 on 4-lane roads */
f32 trafficPickLane(s32 point) {
    s32 r;

    if (gPathPoints[point].lanes == 2) {
        r = 0;
    } else {
        r = randInt(2);
    }
    return (f32)(r + 2) * 1150.0f - 1725.0f;
}
