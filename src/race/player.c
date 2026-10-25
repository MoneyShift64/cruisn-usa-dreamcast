/*
 * race/player.c - per-frame update of the racing cars driven by players:
 * controls, gearbox, crash flips, respawns, 2-player catch-up, track events,
 * kerbs, and the engine/tyre sound effects.
 * Original code: race overlay, ROM 0x4AF70-0x4D050 (RAM 0x80032500-0x800345E0).
 */
#include "common.h"
#include "race.h"
#include "road.h"

#define CLAMP01(x) (((x) < 0.0f) ? 0.0f : (((x) > 1.0f) ? 1.0f : (x)))

/* Track events: when a car reaches road section 'section' on 'track', call func(car, arg). */
typedef struct TrackEvent {
    /* 0x00 */ s32 track;
    /* 0x04 */ s32 section;
    /* 0x08 */ void (*func)(s32 car, s32 arg);
    /* 0x0C */ s32 arg;
} TrackEvent;
extern TrackEvent gTrackEvents[];        /* 0x80042A90 ovl2 - sorted by track, ends with track 999 */
extern s32 gTrackEventFirst;             /* 0x80046E80 ovl2 */
extern s32 gTrackEventCount;             /* 0x80046E84 ovl2 */

typedef struct ScriptCmd {
    /* 0x00 */ void (*func)(s32 car, s32 arg, s32 index);
    /* 0x04 */ s32 arg;
} ScriptCmd;
extern ScriptCmd gScriptCmds[];          /* 0x80043720 ovl2 */


/* Front-end choices per racer (0x39C bytes, 0x80150210) */
typedef struct RacerSetup {
    /* 0x000 */ s32 manual;
    /* 0x004 */ u8  pad004[0x10];
    /* 0x014 */ f32 distance;
    /* 0x018 */ u8  pad018[0x37C - 0x18];
    /* 0x37C */ u8  pad37C[0x10];
    /* 0x38C */ s32 cameraFollowsCar;
    /* 0x390 */ u8  pad390[0x0C];
} RacerSetup;
extern RacerSetup gRacerSetup[];         /* 0x80150210 */

extern Mtx3 gTmpMtxA;                    /* 0x8004A030 ovl2 */
extern Mtx3 gTmpMtxB;                    /* 0x8004A058 ovl2 */
extern u8 *gMtxCursor;                   /* 0x80147688 */
extern s32 gSfxGear[2];                  /* 0x800438A0 ovl2 */
extern s32 gSfxHorn[3];                  /* 0x800438A8 ovl2 */
extern s32 gSfxSkid[2];                  /* 0x800438B4 ovl2 */

void hudCheckpoint(s32 player, s32 unused); /* 0x8002A778 ovl2 */
void racerSetRumble(s32 racer, s32 on); /* 0x800307E4 ovl2 */
void skySetEnabled(s32 player, s32 on);  /* 0x80101A3C */
void carProbesFindGround(Car *car, s32 racer); /* 0x8002690C ovl2 */
void carGroundUpdate(Car *car, s32 side); /* 0x80025C90 ovl2 */
void racerResetRoad(Car *car, s32 how);  /* 0x80037D88 ovl2 */
void cameraForceFar(s32 player, s32 on, s32 quiet); /* 0x800349BC ovl2 */
void raceCameraUpdate(s32 player);       /* 0x80034B64 ovl2 */
void viewBegin(s32 racer);        /* 0x80116C10 */
void carInitStats(s32 vehicle);          /* 0x80035BF0 ovl2 */
void playerInputClear(void);               /* 0x800345E0 ovl2 */
void sfxPlay(s32 id, s32 player);                   /* 0x8011F2A4  full volume */
void sfxPlayVolume(s32 id, s32 player, f32 volume); /* 0x8011F2C4 */
void sfxSetVolume(s32 id, s32 player, f32 volume);  /* 0x8011F3E0 */
void sfxEngine(s32 id, s32 player, f32 pitch, f32 volume); /* 0x8011F4E8 */
s32  sfxState(s32 id, s32 player);       /* 0x8011F614  3 = playing */
void sfxStop(s32 id, s32 player);        /* 0x8011F670 */
u32  randInt(s32 n);                     /* 0x80103150 */
void carEngineUpdate(Car *car);          /* 0x80036540 ovl2 */
void carRpmUpdate(s32 player);           /* 0x8003698C ovl2 */
void carSlideUpdate(Car *car);           /* 0x80036B0C ovl2 */
void carSteerUpdate(Car *car);           /* 0x80036D14 ovl2 */
void carCheckProbes(Car *car);           /* 0x800370E0 ovl2 */
void carHitBarrier(Car *car, s32 player, s32 noSteer); /* 0x800371D4 ovl2 */
void carBarrierGlance(Car *car, f32 roadHeading, f32 offset, f32 absOffset); /* 0x80037594 ovl2 */
void carPlaySfx(Car *car, s32 id, f32 volume); /* 0x800378DC ovl2 */
void carStartSpin(Car *car, s32 steps, f32 rate, f32 amount); /* 0x80037C8C ovl2 */
s32  autoGearbox(s32 gear, s32 rpm, f32 throttle); /* 0x800364C0 ovl2 */

void carReadBrake(Car *car, PlayerInput *in);
void carReadThrottle(Car *car, PlayerInput *in);
void carCatchUp(Car *car, Car *other, RacerInfo *info);
void carSteerInput(Car *car, PlayerInput *in);
s32  carTrackContact(Car *car, s32 racer);
void carOnKerb(Car *car, s32 racer);
void carCheckWrongWay(Car *car);
void carSetEffects(s32 racer, s32 flags);
void carSounds(Car *car, RacerInfo *info);
void carHazardSpin(Car *car);
void racerRespawn(s32 racer, s32 how);
void racerCheckStuck(s32 racer);

/* 0x80032500 ovl2 - find this track's events in the event table */
void trackEventsInit(void) {
    gTrackEventFirst = 0;
    while (gTrackEvents[gTrackEventFirst].track < gCurrentTrack) {
        gTrackEventFirst++;
    }
    gTrackEventCount = 0;
    while (gTrackEvents[gTrackEventFirst + gTrackEventCount].track == gCurrentTrack) {
        gTrackEventCount++;
    }
}

/* 0x800325A8 ovl2 */
void scriptCmdRun(s32 car, s32 index) {
    ScriptCmd *cmd = &gScriptCmds[index];

    cmd->func(car, cmd->arg, index);
}

/* 0x800325E4 ovl2 - a car entered road section 'section': fire matching track events */
void trackEventsCheck(s32 car, s32 unused, s32 section) {
    TrackEvent *ev;
    s32 i;

    for (i = 0; i < gTrackEventCount; i++) {
        ev = &gTrackEvents[gTrackEventFirst + i];
        if (section == ev->section) {
            ev->func(car, ev->arg);
        }
    }
}

/* 0x80032684 ovl2 */
void eventNone(s32 a, s32 b) {
}

/* 0x80032694 ovl2 */
void eventBoot(s32 unused) {
    hudCheckpoint(0, 0);
}

/* 0x800326B8 ovl2 */
void eventCameraBehind(s32 racer, s32 unused) {
    skySetEnabled(racer, 0);
    racerSetRumble(racer, 1);
}

/* 0x800326F0 ovl2 - set up every racing car for the start of a race */
void racersInit(void) {
    RacerInfo *info = gRacers;
    Car *car = gCars;
    RacerSetup *setup;
    Car *car2;
    s32 i;

    for (i = 0, setup = gRacerSetup, car2 = gCars; i < gNumPlayers; i++, setup++, car2++, info++, car++) {
        carInitStats(i);
        car->manual = setup->manual;
        if (setup->manual & 0xFF) {
            car->gear = 1;
        } else {
            car->gear = 0;
        }
        car->shiftTimer = 0;
        car->gearTarget = car2->gear;
        car->unk84 = 0;
        car->boosting = gNumPlayersM1;
        info->engineVolume = 0.0f;
        info->hornTimer = 0;
        info->wasAirborne = 0;
        info->rpmShown = 0;
        info->section = 0;
        info->revSfx = 0;
        info->rumbleSfx = 0;
        info->gravelSfx = 0;
        info->unk27 = 0;
        info->brakeSfx = 0;
        info->lastX = car->pos[0];
        info->unk60 = 0.0f;
        info->unk64 = 0.0f;
        info->lastZ = car->pos[2];
        sfxPlayVolume(0x1F, i, 0.0f);          /* start the engine sound silent */
    }
    playerInputClear();
}

/* 0x80032850 ovl2 - race start: automatic cars go into first gear */
void racersEngageFirstGear(void) {
    Car *car;

    for (car = gCars; car < &gCars[gNumPlayers]; car++) {
        if (car->manual == 0) {
            car->gear = 1;
            car->gearTarget = 1;
        }
    }
}

/* 0x800328B4 ovl2 - distance along the route for each racer (for the HUD) */
void racersMeasureDistance(void) {
    Car *car = gCars;
    RacerInfo *info;
    RacerSetup *setup;
    PathPoint *pt;
    f32 total;
    f32 seg;
    f32 x;
    f32 z;
    f32 dx;
    f32 dz;
    s32 n;
    s32 i;

    for (i = 0, setup = gRacerSetup, info = gRacers; i < gNumPlayers; i++, car++, info++, setup++) {
        n = car->road.point;
        total = 0.0f;
        z = 0.0f;
        x = 0.0f;
        seg = 0.0f;
        pt = &gPathPoints[1];
        while (n-- != 0) {
            total += seg;
            dx = pt->x - x;
            dz = pt->z - z;
            x += dx;
            z += dz;
            seg = sqrtf(dx * dx + dz * dz) * 0.039006f;
            pt++;
        }
        setup->distance = total + car->road.t * seg;
        if (info->rumbleSfx) {
            sfxStop(0x87, i);
            info->rumbleSfx = 0;
        }
    }
}

/* 0x80032A34 ovl2 - one frame of a player's car */
void playerCarUpdate(s32 racer) {
    Car *car = &gCars[racer];
    RacerInfo *info = &gRacers[racer];
    PlayerInput *in;
    u8 gearBefore;
    u8 gear;
    s32 onEdge;
    f32 a;
    f32 d;
    f32 x;
    f32 z;

    info->respawns = 0;
    if (car->manual) {
        in = &gPlayerInput[racer];
        if (in->buttons & RACEBTN_SHIFT_UP) {
            if (car->gearTarget < 4) {
                car->gearTarget++;
                car->shiftTimer = car->gear ? 5 : 1;
                car->gear = 0;
            }
        }
        if (in->buttons & RACEBTN_SHIFT_DOWN) {
            if (car->gearTarget > 0) {
                car->gearTarget--;
                car->shiftTimer = 5;
                car->gear = 0;
            }
        }
    }
    in = &gPlayerInput[racer];
    carReadBrake(car, in);
    carReadThrottle(car, in);

    if (car->flipping) {
        /* tumbling through the air after a big crash */
        info->flipRoll += 2.0f * info->flipRollRate;
        info->flipPitch += 2.0f * info->flipPitchRate;
        mtx3RotX(car->orient, info->flipRoll);
        mtx3RotY(gTmpMtxA, info->flipPitch);
        mtx3Mul(car->orient, car->orient, gTmpMtxA);
        a = car->travelDir;
        car->vel[2] = cosf_tbl(a) * (2.0f * car->speed);
        car->vel[0] = sinf_tbl(a) * (-2.0f * car->speed);
        vec3Add(car->pos, car->pos, car->vel);
        carProbesFindGround(car, racer);
        d = car->probes[0].groundY - car->unk40;   /* follow the ground height */
        car->unk40 = car->probes[0].groundY;
        car->pos[1] += d;
        info->flipHeight += 2.0f * car->vel[1];
        car->vel[1] -= 4.0f;
        if (car->vel[1] <= -60.0f) {
            car->flipping = 0;
            carPlaySfx(car, 2, 1.0f);
            cameraForceFar(racer, 0, 0);
        }
        carCheckProbes(car);
        if (car->unk80 == 2) {
            carHitBarrier(car, racer, 1);
        }
    } else {
        racerCheckStuck(racer);
        gearBefore = car->gear;
        gear = gearBefore;
        if (car->manual) {
            if (car->shiftTimer != 0) {
                if (--car->shiftTimer == 0) {
                    car->gear = car->gearTarget;
                }
            }
            gear = car->gear;
        } else if (gRaceState == 1) {
            car->gear = autoGearbox(gear, car->rpm, car->throttle);
            gear = car->gear;
        }
        if (gearBefore < gear) {
            car->throttle = 2.0f * car->throttle;
            if (car->throttle > 1.4f) {
                sfxPlay(0x88, racer);          /* gear-change chirp */
                carSetEffects(racer, 3);
            }
        }
        carEngineUpdate(car);
        carSlideUpdate(car);
        if (gNumPlayersM1 != 0 && car->spinTimer == 0) {
            car->slide = car->slide * (2.0f - car->speedScale);
        }
        carSteerInput(car, in);
        carSteerUpdate(car);
        onEdge = carTrackContact(car, racer);
        carCheckWrongWay(car);
        mtx3RotY(gTmpMtxA, car->yawDelta + car->unk50);
        mtx3Mul(car->orient, gTmpMtxA, car->orient);
        mtx3RotY(gTmpMtxB, car->travelDir - car->heading);
        if (onEdge && info->kerbRumble && info->respawns == 0) {
            x = car->pos[0];
            z = car->pos[2];
            car->pos[0] = x + (x - info->lastX);
            car->pos[2] = z + (z - info->lastZ);
        }
        mtx3Mul(gTmpMtxB, gTmpMtxB, car->orient);
        if (car->airborne == 0) {
            vec3Forward(car->vel, car->frameDist * car->speedScale, gTmpMtxB);
        }
        vec3Add(car->pos, car->pos, car->vel);
        carGroundUpdate(car, racer);
        mtx3RotY(gTmpMtxA, car->heading);
        mtx3Mul(car->orient, gTmpMtxA, car->orient);
    }
    carRpmUpdate(racer);
    carSounds(car, info);
    if (car->speed <= 0.5f) {
        car->speed = 0.0f;
    }
    if (gNumPlayersM1 != 0) {
        carCatchUp(car, &gCars[1 - racer], info);
    }
    if (in->buttons & RACEBTN_RESET) {
        car->heading = car->travelDir = car->road.heading;
        car->pos[0] = car->road.pos[0];
        car->pos[1] = car->road.pos[1];
        car->pos[2] = car->road.pos[2];
    }
}

/* 0x80032FDC ovl2 - put a crashed / stuck car back on the road ('how': see racerResetRoad) */
void racerRespawn(s32 racer, s32 how) {
    Car *car = &gCars[racer];
    RaceCamera *cam;
    u32 *src;
    u32 *dst;
    s32 i;

    racerResetRoad(car, how);
    if (gRacerSetup[racer].cameraFollowsCar) {
        cam = &gRaceCameras[racer];
        cam->yaw = car->heading;
        cam->pitch = 0.0f;
    }
    gRacers[racer].respawns++;
    cameraForceFar(racer, 0, 0);
    raceCameraUpdate(racer);
    src = (u32 *)gMtxCursor;
    viewBegin(racer);
    dst = (u32 *)((u8 *)&gRaceCameras[racer] + 0x28);
    for (i = 0; i != 4; i++) {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = src[3];
        dst += 4;
        src += 4;
    }
}

/* 0x80033138 ovl2 - start a big crash: the car flips up into the air */
void racerStartFlip(s32 racer) {
    Car *car = &gCars[racer];
    RacerInfo *info;

    if (car->flipping == 0) {
        cameraForceFar(racer, 1, 0);
        if (roadIsLeft(&car->road)) {
            info = &gRacers[racer];
            info->flipRollRate = 0.104f;
            info->flipPitchRate = 0.104f;
        } else {
            info = &gRacers[racer];
            info->flipPitchRate = 0.104f;
            info->flipRollRate = -0.104f;
        }
        info->flipRoll = 0.0f;
        info->flipPitch = car->heading;
        car->vel[1] = 60.0f;
        car->unk40 = car->probes[0].groundY;
        info->flipHeight = 0.0f;
        car->flipping = 1;
    }
}

/* 0x80033240 ovl2 - respawn cars that are spinning endlessly, off the road too long or upside down */
void racerCheckStuck(s32 racer) {
    Car *car = &gCars[racer];
    RacerInfo *info;
    s32 reasons = 0;
    s32 negNow;
    s32 negBefore;
    s32 t;

    if (car->spinTimer) {
        info = &gRacers[racer];
        info->spinFrames++;
        negNow = FALSE;
        if (car->yawRate < 0.0f) {
            negNow = TRUE;
        }
        negBefore = FALSE;
        if (info->lastYawRate < 0.0f) {
            negBefore = TRUE;
        }
        if (negNow != negBefore) {
            info->spinReversals++;
        }
        if (info->spinReversals >= 7 &&
            (((t = info->spinFrames) >= 60 && info->offroadTime >= t) || t >= 120)) {
            reasons = 1;
        }
    } else {
        info = &gRacers[racer];
        info->spinFrames = 0;
        info->spinReversals = 0;
    }
    info->lastYawRate = car->yawRate;
    if (car->unk1AC >= 3) {
        info->offroadTime += 150;
    }
    if (info->offroadTime >= 600) {
        reasons++;
    }
    if (car->orient[1][1] < 0.01f) {      /* upside down */
        reasons++;
    }
    if (reasons) {
        racerRespawn(racer, 0);
    }
}

/* 0x800333CC ovl2 - brake pedal (squared for finer control) */
void carReadBrake(Car *car, PlayerInput *in) {
    f32 b = in->brake;

    if (b > 0.0f) {
        b *= b;
    }
    car->brake = b;
}

/* 0x800333FC ovl2 - accelerator.
 * NOTE: the original also tried to cut the throttle before the race starts, but
 * then overwrote it unconditionally, so the throttle always follows the pad. */
void carReadThrottle(Car *car, PlayerInput *in) {
    if (gRaceState == 1) {
        car->throttle = in->gas;
    } else {
        car->throttle = 0.0f;
    }
    car->throttle = in->gas;
}

/* 0x80033438 ovl2 - 2-player catch-up: the car behind gets more speed and power */
void carCatchUp(Car *car, Car *other, RacerInfo *info) {
    s32 lead = car->road.point - other->road.point;
    f32 rate;
    f32 scale;
    f32 dist;
    f32 lo;
    f32 hi;
    f32 room;
    f32 boost;
    s32 half;
    f32 t;

    if (lead == 0) {
        car->boostPower = 1.0f;
    } else {
        if (lead > 0) {
            rate = -0.0008f;
        } else {
            rate = 0.0001f;
        }
        scale = 2.0f * rate + car->speedScale;
        dist = dist2D(other->pos[0], other->pos[2], car->pos[0], car->pos[2]) * 0.000005f;
        if (dist > 0.1f) {
            dist = 0.1f;
        }
        lo = dist + 1.0f;
        if (lead < 0 && scale < lo) {
            scale = lo;
        }
        hi = lo + 0.09f;
        if (scale < 1.0f) {
            car->speedScale = 1.0f;
        } else {
            car->speedScale = (hi < scale) ? hi : scale;
        }
        room = 300.0f - car->speed;
        if (room < 0.0f) {
            room = 0.0f;
        }
        boost = (hi - 1.1f) * 10.0f * (room * 0.00333f) + 1.0f;
        if (boost > 2.0f) {
            boost = 2.0f;
        }
        if (lead > 0) {
            boost = 1.0f;
        }
        car->boostPower = boost;
    }
    /* fade the effect out over the last half of the route */
    half = gPathLength >> 1;
    if (gPathLength - half < car->road.point) {
        t = (f32)(gPathLength - car->road.point) / (f32)half;
        car->speedScale = (car->speedScale - 1.0f) * t + 1.0f;
        car->boostPower = (car->boostPower - 1.0f) * t + 1.0f;
    }
}

/* 0x80033634 ovl2 - steering wheel input -> steering, more effective at speed */
void carSteerInput(Car *car, PlayerInput *in) {
    f32 sens = in->steerSetting;
    f32 curve = (5.0f - sens) * 0.06f + 0.5f;
    f32 s = (sens * 0.02f + 0.9f) * in->steer;
    f32 abs = (s < 0.0f) ? -s : s;
    f32 speed = car->speed;
    f32 steer;
    f32 k1;
    f32 k2;

    steer = s * (1.0f - curve) + abs * s * curve;
    car->steer = steer * -0.3f;
    k1 = speed * (speed * 0.0125f);
    if (k1 > 5.0f) {
        k1 = 5.0f;
    }
    k2 = speed * 0.0625f;
    if (k2 > 15.0f) {
        k2 = 15.0f;
    }
    car->steerYaw = 2.0f * (steer * (k1 + k2)) * -0.0013f;
}

/* 0x80033750 ovl2 - returns TRUE if the car touched the edge of the track */
s32 carTrackContact(Car *car, s32 racer) {
    if (gRaceState != 0) {
        carHazardSpin(car);
        carCheckProbes(car);
        if (car->unk80 == 2) {
            carHitBarrier(car, racer, 0);
            return TRUE;
        }
        if (car->unk80 == 1) {
            carOnKerb(car, racer);
            return TRUE;
        }
    }
    return FALSE;
}

/* 0x800337DC ovl2 - riding the kerb: ease the car back towards the road */
void carOnKerb(Car *car, s32 racer) {
    s32 left = roadIsLeft(&car->road);
    f32 edge = left ? 0.1f : -0.1f;
    f32 d;
    f32 a;
    f32 step;

    if (car->spinTimer) {
        if (car->spinTimer < 2 || !(car->speed < 80.0f)) {
            d = angleWrap(car->travelDir - car->road.heading);
            a = (d < 0.0f) ? -d : d;
            if (a < 0.1f) {
                d = 0.1f;
            }
            car->travelDir = ((d < 0.0f) ? -1.0f : 1.0f) * d + car->road.aim;
        }
        return;
    }
    d = angleWrap(car->heading - car->road.heading);
    if (left) {
        step = edge - d;
        if (step >= 0.0f) {
            if (step > 0.1f) {
                step = 0.1f;
            }
            car->heading += step;
        }
    } else {
        step = edge - d;
        if (step <= 0.0f) {
            if (step < -0.1f) {
                step = -0.1f;
            }
            car->heading += step;
        }
    }
    if (car->speed < 20.0f) {
        car->speed = 20.0f;
    }
    d = angleWrap(car->travelDir - car->road.heading);
    if (((d < 0.0f) ? -d : d) >= 0.6f) {
        carBarrierGlance(car, car->road.heading, car->road.heading - car->travelDir, (d < 0.0f) ? -d : d);
        return;
    }
    if (left) {
        step = edge - d;
        if (step >= 0.0f) {
            if (step > 0.1f) {
                step = 0.1f;
            }
            car->travelDir += step;
        }
    } else {
        step = edge - d;
        if (step <= 0.0f) {
            if (step < -0.1f) {
                step = -0.1f;
            }
            car->travelDir += step;
        }
    }
}

/* 0x80033AE4 ovl2 - pointing more than ~100 degrees away from the road: spin */
void carCheckWrongWay(Car *car) {
    f32 d;

    if (car->spinTimer == 0 && car->speed >= 2.0f) {
        d = angleWrap(car->road.heading - car->travelDir);
        if (((d < 0.0f) ? -d : d) >= 1.75f) {
            carStartSpin(car, 0, (d < 0.0f) ? -0.12f : 0.12f, 1.0f);
        }
    }
}

/* 0x80033BAC ovl2 - tyre smoke / dust on the car's model.
 * flags: 1 = flash, 2 = light smoke, 4 = heavy smoke, 8 = off */
void carSetEffects(s32 racer, s32 flags) {
    RenderObj *obj;
    Car *car;
    s32 left;
    s32 right;

    if (gRaceState != 1) {
        flags &= ~6;
    }
    if (flags & 1) {
        gRenderObjs[racer].flags |= 0x40;
    }
    if (flags & 8) {
        obj = &gRenderObjs[racer];
        obj->smoke[0] = 0;
        obj->smoke[1] = 0;
        return;
    }
    if (flags & 6) {
        car = &gCars[racer];
        obj = &gRenderObjs[racer];
        left = car->probes[3].offroad ? 0 : 0x80;     /* rear wheels: dust instead of smoke off-road */
        right = 0x80;
        if (car->probes[4].offroad) {
            right = 0;
        }
        if (flags & 4) {
            obj->smoke[0] = left | 2;
            obj->smoke[1] = right | 2;
            return;
        }
        obj->smoke[0] = left | 1;
        obj->smoke[1] = right | 1;
    }
}

/* 0x80033CDC ovl2 - engine, tyre and road sounds for a car */
void carSounds(Car *car, RacerInfo *info) {
    s32 racer = car->unk7D;
    f32 v;
    f32 vol;
    f32 rpm;
    f32 pitch;
    s32 wasAir;
    s32 inAir;
    f32 skidVol;
    s32 kerbs;
    s32 moving;

    v = (1.0f - car->speed * 0.00333f) * 0.75f + 0.25f;
    v = CLAMP01(v);
    rpm = (f32)car->rpm;
    vol = v * (car->throttle * 0.8f + 0.2f) * (rpm * 0.01f + 0.5f);
    if (gRaceState == 0) {
        vol *= 0.0625f;
    }
    vol = 2.0f * vol;
    vol = CLAMP01(vol);
    pitch = rpm * 0.02f;
    pitch = CLAMP01(pitch);
    info->engineVolume = vol;
    sfxEngine(0x1F, racer, pitch, vol);
    info->rpmShown = car->rpm;

    /* revving */
    if (car->throttle >= 0.75f) {
        if (gRaceState == 0) {
            carSetEffects(racer, 1);
        }
        if (info->revSfx == 0 && sfxState(gSfxGear[0], racer) != 3 && sfxState(gSfxGear[1], racer) != 3) {
            carSetEffects(racer, 1);
            sfxPlay(gSfxGear[randInt(2)], racer);
            info->revSfx = 1;
        }
    } else if (car->throttle < 0.5f) {
        info->revSfx = 0;
    }

    /* occasional horn / engine flourish at high revs */
    if (car->flipping == 0) {
        if (info->hornTimer-- <= 0 && car->probes[0].unk20 > 100.0f) {
            sfxPlay(gSfxHorn[randInt(3)], racer);
            info->hornTimer = 100;
        }
    }

    /* landing thump */
    wasAir = car->airborne;
    if (wasAir == 0 && info->wasAirborne && gRaceState == 1) {
        sfxPlay(2, racer);
        wasAir = car->airborne;
    }
    info->wasAirborne = wasAir;

    inAir = car->airborne != 0;
    if (!inAir) {
        inAir = car->flipping != 0;
    }
    if (car->slide >= 0.5f && !inAir) {
        carSetEffects(racer, 4);
    } else {
        carSetEffects(racer, 8);
    }

    /* tyre squeal */
    info->skidSfx = (car->slide >= 0.25f) ? (inAir == 0) : 0;
    if (info->skidSfx) {
        skidVol = car->slide * 0.45f + 0.55f;
        if (sfxState(gSfxSkid[0], racer) == 3) {
            sfxSetVolume(gSfxSkid[0], racer, skidVol);
        } else if (sfxState(gSfxSkid[1], racer) == 3) {
            sfxSetVolume(gSfxSkid[1], racer, skidVol);
        } else {
            sfxPlayVolume(gSfxSkid[randInt(2)], racer, skidVol);
        }
        if (car->speed < -20.0f) {
            carSetEffects(racer, 4);
        }
    } else {
        sfxStop(0x76, racer);
        sfxStop(0x77, racer);
    }

    /* braking hard */
    if (car->brake > 0.8f && car->speed >= 1.5f && !inAir) {
        carSetEffects(racer, 4);
        if (info->brakeSfx) {
            if (info->skidSfxPrev && info->skidSfx == 0 && sfxState(3, racer) != 3) {
                sfxPlay(3, racer);
            }
        } else {
            sfxPlay(3, racer);
            info->brakeSfx = 1;
        }
    } else if (info->brakeSfx) {
        sfxStop(3, racer);
        info->brakeSfx = 0;
    }

    /* engine braking whine when coasting at high revs */
    if (car->throttle < 0.15f && (f32)car->rpm > 30.0f && gRaceState == 1) {
        if (sfxState(0x7A, racer) != 3) {
            sfxPlay(0x7A, racer);
        }
    } else {
        sfxStop(0x7A, racer);
    }

    /* kerb rumble strip */
    if (info->kerbRumble) {
        v = car->speed * 0.003921569f;
        if (v < 0.4f) {
            v = 0.4f;
        } else if (v > 1.0f) {
            v = 1.0f;
        }
        if (info->rumbleSfx == 0) {
            sfxPlayVolume(0x87, racer, v);
            info->rumbleSfx = 1;
        } else {
            sfxSetVolume(0x87, racer, v);
        }
    } else if (info->rumbleSfx) {
        sfxStop(0x87, racer);
        info->rumbleSfx = 0;
    }

    /* gravel under the rear wheels */
    kerbs = (car->probes[4].offroad != 0) + (car->probes[3].offroad != 0);
    if (kerbs) {
        moving = FALSE;
        if (car->speed >= 1.0f) {
            moving = TRUE;
        }
        v = (f32)kerbs * 0.5f * (f32)moving;
        if (info->gravelSfx) {
            sfxSetVolume(0x3F, racer, v);
        } else {
            info->gravelSfx = 1;
            sfxPlayVolume(0x3F, racer, v);
        }
    } else {
        if (info->gravelSfx) {
            sfxStop(0x3F, racer);
            sfxPlay(2, racer);
        }
        info->gravelSfx = 0;
    }
    info->skidSfxPrev = info->skidSfx;
}

/* 0x800344EC ovl2 - hazard on the road (flagged per racer): forced spin */
void carHazardSpin(Car *car) {
    if (gRacers[car->unk7D].kerbRumble != 0 && car->flipping == 0 && car->orient[1][1] <= 0.5f) {
        car->travelDir = angleWrap((car->road.heading + car->road.aim) * 0.5f);
        carStartSpin(car, 15, angleWrap(car->road.heading - car->heading) * 0.06666667f, 0.0f);
        if (car->speed < 40.0f) {
            car->speed = 40.0f;
        }
    }
}
