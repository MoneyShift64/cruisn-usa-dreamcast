/*
 * race/cars.c - setting up the vehicles (players, AI racers, traffic) and
 * drawing them: model collision boxes, start positions, traffic spawning,
 * putting a car back on the road, and per-frame drawing with wheel spin,
 * steering, brake lights and body lean.
 * Original code: race overlay, inside ROM 0x4E340-0x50DE0 (RAM 0x800358D0-0x80038370),
 * shared with carphys.c.
 */
#include "common.h"
#include "race.h"
#include "road.h"
#include "scenery.h"
#include "ground.h"
#include "frontend.h"

extern s32 gCarRideHeight[17];           /* 0x80131820 - 16.16 drawing units, per model */

/* Traffic paint schemes: a random one of 'count' starting at 'first' (0x80134080) */
typedef struct PaintRange { s32 first, count; } PaintRange;
extern PaintRange gTrafficPaint[];       /* 0x80134080 */

/* Start grid: where each vehicle starts on each track (0x90 bytes, 10 per track, 0x80135570) */
typedef struct StartProbe { f32 x, y, z, fallSpeed; } StartProbe;
typedef struct StartPos {
    /* 0x00 */ u8  model;                     /* AI racers: car model and colour (renderObjsInit) */
    /* 0x01 */ u8  colour;
    /* 0x02 */ u8  pad02[2];
    /* 0x04 */ s32 pos[3];                   /* 16.16 drawing units */
    /* 0x10 */ s32 angleZ, angleY, angleX;
    /* 0x1C */ StartProbe probes[5];
    /* 0x6C */ Mtx3 orient;
} StartPos;
extern StartPos gStartGrid[][10];        /* 0x80135570 */

extern s32 gNumTraffic;                  /* 0x8004A018 ovl2 (cleared here) */
extern s32 gMaxVehicles;                 /* 0x8004A020 ovl2 */
extern u32 *gDlCursor;                   /* 0x80147684 */
extern Gfx gRaceBaseDL[];                /* 0x80133710 - the base matrix */

u32  randInt(s32 n);                     /* 0x80103150 */
s32  cameraDistance2(s32 player, s32 vehicle); /* 0x80100BC8 */
s32  atan2s(f32 y, f32 x);               /* 0x80114100 */
void renderObjInit(void *obj, s32 a, s32 b, s32 c, s32 d, s32 e, s32 f, s32 g); /* 0x8011802C */
void renderObjDraw(void *obj);           /* 0x801180EC */
f32  angleWrap(f32 a);                   /* 0x80038370 ovl2 */
f32  sinf_tbl(f32 a);                    /* 0x80038458 ovl2 */
f32  cosf_tbl(f32 a);                    /* 0x800384AC ovl2 */
void bytesClear(u8 *p, s32 n);           /* 0x80038500 ovl2 */
void mtx3RotY(Mtx3 m, f32 a);            /* 0x80038568 ovl2 */
void vec3MulMtx(Vec3f out, Vec3f v, Mtx3 m); /* 0x8003879C ovl2 */
void mtx3ToAngles(Mtx3 m, s32 *outX, s32 *outY, s32 *outZ); /* 0x800389D4 ovl2 */
void sceneryFadeColor(s32 flags, s32 player); /* 0x80030714 ovl2 */

Car *carReset(s32 vehicle, s32 model);   /* 0x80037ECC ovl2 */
void carInitProbes(s32 vehicle);         /* 0x80038068 ovl2 */
void carBodyLean(Car *car, RenderObj *obj, s32 player); /* 0x80038164 ovl2 */

/* 0x800358D0 ovl2 - count the vehicles for this race and build every car
 * model's collision box (8 corners) and bounding size. */
void carsInit(void) {
    CarModel *m;
    s32 *ride;
    CarModelInfo *info;
    CarBox box;
    Vec3f *c;
    s32 i;
    f32 lift, size, d;

    gNumPlayers = gNumPlayersM1 + 1;
    if (gRacersEnabled != 0) {
        gNumAiRacers = 10 - gNumPlayers;
    } else {
        gNumAiRacers = 0;
    }
    gNumTraffic = 0;
    gNumVehicles = gNumPlayers + gNumAiRacers;
    gMaxVehicles = 20;
    for (m = gCarModels, ride = gCarRideHeight, info = gCarModelInfo; ride != &gCarRideHeight[17];
         m++, ride++, info++) {
        box = m->box;
        lift = (f32)info->bodyHeight * 7.6293945e-05f - (f32)*ride * 7.6293945e-05f;  /* x5 / 65536 */
        m->corners[0][0] = -box.halfWidth; m->corners[0][1] = box.bottom + lift; m->corners[0][2] = box.front;
        m->corners[1][0] = box.halfWidth;  m->corners[1][1] = box.bottom + lift; m->corners[1][2] = box.front;
        m->corners[2][0] = -box.halfWidth; m->corners[2][1] = box.bottom + lift; m->corners[2][2] = box.back;
        m->corners[3][0] = box.halfWidth;  m->corners[3][1] = box.bottom + lift; m->corners[3][2] = box.back;
        m->corners[4][0] = -box.halfWidth; m->corners[4][1] = box.top + lift;    m->corners[4][2] = box.front;
        m->corners[5][0] = box.halfWidth;  m->corners[5][1] = box.top + lift;    m->corners[5][2] = box.front;
        m->corners[6][0] = -box.halfWidth; m->corners[6][1] = box.top + lift;    m->corners[6][2] = box.back;
        m->corners[7][0] = box.halfWidth;  m->corners[7][1] = box.top + lift;    m->corners[7][2] = box.back;
        size = 0.0f;
        for (i = 0, c = m->corners; i < 8; i++, c++) {
            d = (*c)[0] * (*c)[0] + (*c)[1] * (*c)[1] + (*c)[2] * (*c)[2];
            if (size < d) {
                size = d;
            }
        }
        m->bound = size;                     /* squared */
    }
}

/* 0x80035BF0 ovl2 - set up a racer (player or AI) at its start position: the
 * model and position come from its render object, the probes from the grid. */
void carInitStats(s32 vehicle) {
    RenderObj *obj = &gRenderObjs[vehicle];
    Car *car = carReset(vehicle, obj->model);

    car->heading = ((f32)(s16)obj->angleZ * 3.1415927f) / 32768.0f;
    car->travelDir = ((f32)(s16)obj->angleZ * 3.1415927f) / 32768.0f;
    car->pos[0] = (f32)obj->x * 5.0f * (1.0f / 65536.0f);
    car->pos[1] = (f32)(obj->y - 0x28000) * 5.0f * (1.0f / 65536.0f) + 164.0f;
    car->pos[2] = -((f32)obj->z * 5.0f * (1.0f / 65536.0f));
    mtx3RotY(car->orient, car->heading);
    carInitProbes(vehicle);
}

/* 0x80035D1C ovl2 - add a traffic vehicle at route point 'point', 'lane' units
 * to the side, facing along the route (or against it). Returns its number, or 0
 * when all 20 vehicle slots are taken. */
s32 vehicleSpawn(s32 point, s32 oncoming, f32 lane, s32 model) {
    s32 v = gNumVehicles;
    Car *car;
    PaintRange *paint;
    PathPoint *pt;
    RenderObj *obj;
    s32 colour;
    s32 a;
    f32 h, dx;

    if (gMaxVehicles == v) {
        return 0;
    }
    gNumVehicles = v + 1;
    car = carReset(v, model);
    paint = &gTrafficPaint[model];
    colour = randInt(paint->count) + paint->first;
    pt = &gPathPoints[point];
    car->pos[0] = pt->x * 5.0f;
    car->pos[1] = pt->y * 5.0f;
    car->road.point = point - 1;
    car->pos[2] = pt->z * -5.0f;
    roadLocate(&car->road, car->pos[0], car->pos[2], 0);
    a = 0xC000 - atan2s(pt[1].x - pt->x, pt[1].z - pt->z);
    if (oncoming != 0) {
        a += 0x8000;
    }
    h = angleWrap(((f32)(s16)a * 3.1415927f) / 32768.0f);
    dx = cosf_tbl(h) * lane;
    car->pos[0] += dx;
    car->heading = h;
    car->travelDir = h;
    car->pos[2] += sinf_tbl(h) * lane;
    mtx3RotY(car->orient, h);
    obj = &gRenderObjs[v];
    renderObjInit(obj, model, colour, 1, cameraDistance2(0, v), 0, 0, 0);
    obj->angleZ = a;
    return v;
}

/* 0x80035F54 ovl2 - draw vehicle 'vehicle' in 'player's view (if it is on a
 * section loaded for that player and within range of the camera). */
void carDraw(s32 vehicle, s32 player) {
    Car *car = &gCars[vehicle];
    CarModel *m = &gCarModels[car->model];
    SectionWindow *w;
    RenderObj *obj;
    s32 section;
    u16 flags;
    Vec3f v, o;
    RaceCamera *cam;
    f32 x, y, z, roll;
    s32 i;

    if (car->muted != 0) {
        car->muted--;
    }
    w = &gSectionWindows[player];
    section = gPathPoints[car->road.point].section;
    if (section < w->first - 2 || section >= w->first + w->count + 2) {
        return;
    }
    obj = &gRenderObjs[vehicle];
    flags = obj->flags & ~0x1A19;
    if ((car->unk2D == 0 || car->airborne == 0) && car->flipping == 0) {
        flags |= 1;                          /* shadow */
    }
    if (vehicle == player) {
        flags |= 0x10;                       /* the viewer's own car */
        if (gPlayerSetup[player].viewAtEdge != 0) {
            flags |= 0x200;                  /* in-car view */
        }
    }
    if (car->brake > 0.0f && vehicle < gNumPlayers) {
        flags |= 8;                          /* brake lights */
    }
    if (gNumPlayersM1 != 0 && vehicle < 2 && vehicle != player) {
        flags |= (vehicle != 0) ? 0x1000 : 0x800;   /* the other player's car: P1 / P2 marker */
    }
    obj->flags = flags;
    obj->steerAngle = (s32)((car->steer * 32768.0f) / 3.1415927f);
    v[0] = 0.0f;
    v[2] = 0.0f;
    v[1] = m->probes[0][1];
    vec3MulMtx(o, v, car->orient);
    cam = &gRaceCameras[player];
    x = (car->pos[0] + o[0]) * 0.2f - cam->posX;
    y = (car->pos[1] + o[1]) * 0.2f - cam->posY;
    z = (o[2] - car->pos[2]) * 0.2f - cam->posZ;
    if (x < -32000.0f || x > 32000.0f || y < -32000.0f || y > 32000.0f || z < -32000.0f || z > 32000.0f) {
        return;
    }
    obj->x = (s32)(x * 65536.0f);
    obj->y = (s32)(y * 65536.0f + 163840.0f);
    obj->z = (s32)(z * 65536.0f);
    obj->camDist = cameraDistance2(player, vehicle);
    if (gRaceState == 1 && car->enginePower != 0.0f && car->flipping == 0) {
        roll = car->frameDist * 0.2f;
        for (i = 0; i < 2; i++) {
            obj->wheelSpin[i] -= (s32)(((roll / ((f32)gCarModelInfo[car->model].wheelY[i] * (1.0f / 65536.0f)))
                                        * 32768.0f) / 3.1415927f);
        }
    }
    carBodyLean(car, obj, player);
    {
        u32 *g = gDlCursor;
        gDlCursor = g + 2;
        g[0] = 0x01020040;                   /* gSPMatrix(base matrix, load) */
        g[1] = (u32)gRaceBaseDL;
    }
    renderObjDraw(obj);
    sceneryFadeColor(obj->flags, player);
}

/* 0x8003702C ovl2 - copy the car's orientation to its render object's angles */
void vehicleSyncModel(s32 vehicle) {
    RenderObj *obj = &gRenderObjs[vehicle];

    mtx3ToAngles(gCars[vehicle].orient, &obj->angleX, &obj->angleY, &obj->angleZ);
    obj->angleZ -= 0x4000;
}

/* 0x80037BB4 ovl2 - which car is further along the route: > 0 if b is ahead,
 * < 0 if a is ahead (same route point: nearest to the next point wins). */
s32 carCompareAhead(Car *a, Car *b) {
    s32 d = b->road.point - a->road.point;
    PathPoint *next;
    f32 dx, dz, da, db;

    if (d != 0) {
        return d;
    }
    next = &gPathPoints[a->road.point + 1];
    dx = next->x - a->pos[0] * 0.2f;
    dz = next->z + a->pos[2] * 0.2f;
    da = dx * dx + dz * dz;
    dx = next->x - b->pos[0] * 0.2f;
    dz = next->z + b->pos[2] * 0.2f;
    db = dx * dx + dz * dz;
    if (da < db) {
        return -1;
    }
    return (db < da) ? 1 : 0;
}

/* 0x80037D88 ovl2 - put the car back on the route line at the first point (from
 * its own) on a section loaded for a player; how == 0: one point further on.
 * Also ends a spin-out or crash flip. */
void racerResetRoad(Car *car, s32 how) {
    s32 p = car->road.point;
    s32 last = gPathLength - 2;
    PathPoint *pt;
    s32 next;

    if (p < last) {
        pt = &gPathPoints[p];
        while (!(gRoadSections[pt->section].flags & 3)) {
            p++;
            pt++;
            if (p >= last) {
                break;
            }
        }
    }
    next = (how == 0);
    pt = &gPathPoints[p + next];
    car->pos[0] = pt->x * 5.0f;
    car->pos[1] = pt->y * 5.0f - 500.0f;
    car->road.point = p + next;
    car->spinTimer = 0;
    car->flipping = 0;
    car->heading = car->road.heading;
    car->travelDir = car->road.heading;
    car->pos[2] = pt->z * -5.0f;
    mtx3RotY(car->orient, car->road.heading);
}

/* 0x80037ECC ovl2 - clear a vehicle and give it a model's handling values */
Car *carReset(s32 vehicle, s32 model) {
    Car *car = &gCars[vehicle];
    s32 variant;
    CarModel *m;

    bytesClear((u8 *)car, sizeof(Car));
    if (vehicle <= gNumPlayersM1) {
        variant = gPlayerSetup[vehicle].unk008;
    } else {
        variant = 0;
    }
    car->model = model;
    if (car->manual != 0) {                  /* (always 0: just cleared) */
        car->gear = 1;
    } else {
        car->gear = 0;
    }
    car->throttle = 0.0f;
    car->brake = 0.0f;
    m = &gCarModels[model];
    car->enginePower = m->enginePower;
    car->dragRoad = m->dragRoad[variant];
    car->dragOffroad = m->dragOffroad[variant];
    car->grip = m->grip[variant];
    car->mass = m->mass[variant];
    car->unk7D = vehicle;
    car->speedScale = 1.0f;
    car->boostPower = 1.0f;
    car->bound = m->bound;
    return car;
}

/* 0x80038068 ovl2 - start-grid probes and orientation for a vehicle */
void carInitProbes(s32 vehicle) {
    StartPos *s = &gStartGrid[gCurrentTrack][vehicle];
    Car *car = &gCars[vehicle];
    s32 i;
    f32 *dst;
    f32 *src;

    for (i = 0; i < 5; i++) {
        car->probes[i].offroad = 0;
        car->probes[i].x = s->probes[i].x;
        car->probes[i].y = s->probes[i].y;
        car->probes[i].z = s->probes[i].z;
        car->probes[i].fallSpeed = s->probes[i].fallSpeed;
    }
    dst = &car->orient[0][0];
    src = &s->orient[0][0];
    for (i = 0; i < 9; i++) {
        dst[i] = src[i];
    }
}

/* 0x80038164 ovl2 - lean the body: pitch back when accelerating (forward when
 * braking, twice as much) and with the revs, roll into turns. Smoothed. */
void carBodyLean(Car *car, RenderObj *obj, s32 player) {
    f32 pitch, roll;

    if (car->unk2D != 0 || car->airborne != 0 || car->flipping != 0) {
        obj->angleYAdd = 0;
        obj->angleXAdd = 0;
        car->bodyRoll = 0.0f;
        return;
    }
    pitch = (car->speed - car->unk64) * -0.015f;
    if (pitch < 0.0f) {
        pitch += pitch;
    }
    pitch += (f32)car->rpm * -0.0005f;
    if (pitch < -0.1f) {
        pitch = -0.1f;
    } else {
        pitch = (pitch > 0.1f) ? 0.1f : pitch;
    }
    pitch = pitch * 0.3f + car->bodyPitch * 0.7f;
    car->bodyPitch = pitch;
    obj->angleYAdd = -(s32)((pitch * 32768.0f) / 3.1415927f);
    roll = (car->bodyRoll + angleWrap(car->heading - car->prevHeading) * (car->speed * 0.006f)) * 0.5f;
    car->bodyRoll = roll;
    roll *= 2.2f;
    if (roll < -0.1f) {
        roll = -0.1f;
    } else {
        roll = (roll > 0.1f) ? 0.1f : roll;
    }
    obj->angleXAdd = -(s32)((roll * 32768.0f) / 3.1415927f);
}
