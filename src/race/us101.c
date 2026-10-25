/*
 * race/us101.c - the US 101 helicopter: once a player triggers it, it flies along
 * the route ahead of them (1 or 2 passes, a random 4-22 s apart), with a rotor
 * sound that gets quieter with distance, until it reaches a section that is not
 * loaded.
 * Original code: race overlay, ROM 0x530E0-0x53950 (RAM 0x8003A670-0x8003AEE0).
 */
#include "common.h"
#include "race.h"
#include "road.h"

typedef struct Heli {
    /* 0x00 */ f32 x, y, z;                  /* world units */
    /* 0x0C */ u8  pad0C[0xA4 - 0x0C];
    /* 0xA4 */ RoadPos road;                 /* where it is on the route */
    /* 0xC0 */ s32 passes;                   /* passes still to fly */
    /* 0xC4 */ u32 nextTime;                 /* gFrameCounter for the next pass, 0 = none */
    /* 0xC8 */ s32 player;                   /* who triggered it */
    /* 0xCC */ f32 side;                     /* the player's sideways offset when it was launched */
    /* 0xD0 */ f32 heading;
    /* 0xD4 */ f32 dist;                     /* distance to the player */
    /* 0xD8 */ s32 spin;                     /* rotor angle */
} Heli;

/* What the main-segment model drawer takes (0x20 bytes) */
typedef struct HeliDraw {
    /* 0x00 */ s32 x, y, z;                  /* relative to the camera, 16.16 drawing units */
    /* 0x0C */ s32 yaw, pitch, roll;         /* binary angles */
    /* 0x18 */ s32 scale;                    /* 16.16 */
    /* 0x1C */ s32 visible;
} HeliDraw;

extern void    *gHeliModel[4];           /* 0x800656D0 ovl2 - body and rotor parts */
extern s32      gHeliState;              /* 0x800656E0 ovl2 - 0 none, 1 flying, 2 waiting for the next pass */
extern HeliDraw gHeliDraw;               /* 0x800656E8 ovl2 */
extern Heli     gHeli;                   /* 0x80065708 ovl2 */
extern f32      gDrawDistance[2];        /* 0x80046E68 ovl2 */
extern u32      gFrameCounter;           /* 0x80147698 */

void *assetLoad(s32 id);                 /* 0x80000480 */
u32  randInt(s32 n);                     /* 0x80103150 */
s32  atan2s(f32 y, f32 x);               /* 0x80114100 */
f32  sqrtf(f32 x);                       /* 0x80115720 */
void modelDrawSpinning(void **parts, HeliDraw *obj, s32 spin, u32 dist); /* 0x8011E7B0 */
s32  roadCurrentPoint(s32 player);       /* 0x8011EF10 - last path point loaded for the player */
void sfxPlayVolume(s32 id, s32 player, f32 volume); /* 0x8011F2C4 */
void sfxSetVolume(s32 id, s32 player, f32 volume); /* 0x8011F3E0 */
void sfxStop(s32 id, s32 player);        /* 0x8011F670 */
f32  angleWrap(f32 a);                   /* 0x80038370 ovl2 */
f32  dist2D(f32 x1, f32 y1, f32 x2, f32 y2); /* 0x8003840C ovl2 */
f32  sinf_tbl(f32 a);                    /* 0x80038458 ovl2 */
f32  cosf_tbl(f32 a);                    /* 0x800384AC ovl2 */

void heliLaunch(void);                   /* 0x8003AB04 ovl2 */
void heliVolume(void);                   /* 0x8003AC1C ovl2 */
void heliSteer(void);                    /* 0x8003ACBC ovl2 */

/* 0x8003A670 ovl2 */
void us101Init(void) {
    gHeliModel[0] = assetLoad(0x3053);
    gHeliModel[1] = assetLoad(0x3052);
    gHeliModel[2] = assetLoad(0x3051);
    gHeliModel[3] = assetLoad(0x3054);
}

/* 0x8003A6C8 ovl2 */
void us101Setup(s32 unused) {
    gHeliState = 0;
    gHeli.nextTime = 0;
}

/* 0x8003A6E0 ovl2 - once per frame */
void us101Update(s32 unused) {
    Car *car;

    if (gHeliState == 0) {
        return;
    }
    if (gHeliState == 2) {
        if (gHeli.nextTime != 0 && gFrameCounter >= gHeli.nextTime) {
            heliLaunch();
            gHeliState--;
        }
    } else if (gHeliState == 1) {
        roadLocate(&gHeli.road, gHeli.x, gHeli.z, 1);
        if (!(gRoadSections[gPathPoints[gHeli.road.point].section].flags & 3)) {
            /* flown out of the loaded track */
            sfxStop(0x41, gHeli.player);
            gHeliState++;
            if (gHeli.passes-- > 0) {
                gHeli.nextTime = randInt(540) + gFrameCounter + 120;
            }
        } else {
            car = &gCars[gHeli.player];
            gHeli.dist = dist2D(gHeli.x, gHeli.z, car->pos[0], car->pos[2]);
            heliSteer();
            gHeliDraw.pitch = 0;
            gHeliDraw.roll = 0;
            gHeliDraw.visible = 1;
            gHeliDraw.scale = 0x10000;
            gHeliDraw.yaw = (s32)((gHeli.heading * 32768.0f) / 3.1415927f);
            heliVolume();
        }
    }
}

/* 0x8003A8BC ovl2 - draw it in a player's view (within draw distance), nose down */
void us101Draw(s32 player) {
    RaceCamera *cam;
    f32 x, y, z, d2;

    if (gHeliState != 1) {
        return;
    }
    cam = &gRaceCameras[player];
    x = gHeli.x * 0.2f - cam->posX;
    y = gHeli.y * 0.2f - cam->posY;
    z = gHeli.z * -0.2f - cam->posZ;
    d2 = x * x + z * z;
    if (d2 < gDrawDistance[player] * gDrawDistance[player]) {
        gHeliDraw.pitch = -0x9C4;
        gHeliDraw.x = (s32)(x * 65536.0f);
        gHeliDraw.y = (s32)(y * 65536.0f);
        gHeliDraw.z = (s32)(z * 65536.0f);
        gHeli.spin -= 10000;
        modelDrawSpinning(gHeliModel, &gHeliDraw, gHeli.spin, (u32)sqrtf(d2));
    }
}

/* 0x8003AA68 ovl2 - track trigger: on = start (1 or 2 passes) for this player;
 * off = no more passes */
void us101Trigger(s32 player, s32 on) {
    if (on != 0 && gHeliState == 0) {
        gHeliState++;
        gHeli.passes = randInt(2) + 1;
        gHeli.player = player;
        heliLaunch();
        return;
    }
    if (on == 0 && gHeliState > 0 && player == gHeli.player) {
        gHeli.passes = 0;
        gHeli.nextTime = 0;
    }
}

/* 0x8003AB04 ovl2 - start a pass from the far end of the player's loaded track,
 * 1300 units up, heading back along the route */
void heliLaunch(void) {
    Car *car = &gCars[gHeli.player];
    PathPoint *pt;

    gHeli.side = car->roadOffset;
    pt = &gPathPoints[roadCurrentPoint(gHeli.player)];
    gHeli.road.point = car->road.point;
    roadLocate(&gHeli.road, pt[-1].x * 5.0f, pt[-1].z * -5.0f, 0);
    gHeli.x = gHeli.road.pos[0];
    gHeli.y = 1300.0f + gHeli.road.pos[1];
    gHeli.z = gHeli.road.pos[2];
    gHeli.nextTime = 0;
    gHeli.heading = gHeli.road.heading + 3.1415927f;
    sfxPlayVolume(0x41, gHeli.player, 0.0f);
}

/* 0x8003AC1C ovl2 - rotor volume: full within 5000, fading to half at 25000 */
void heliVolume(void) {
    f32 d = gHeli.dist - 5000.0f;

    if (d < 0.0f) {
        d = 0.0f;
    } else {
        d = (d > 20000.0f) ? 20000.0f : d;
    }
    sfxSetVolume(0x41, gHeli.player, 1.0f - d * 2.5e-05f);
}

/* 0x8003ACBC ovl2 - fly 300 units a frame towards a point 5 route points behind
 * it, offset to the player's side of the road */
void heliSteer(void) {
    s32 p = gHeli.road.point - 5;
    PathPoint *pt;
    f32 dx, dz, t, a, ox, oz, d;

    if (p < 0) {
        p = 0;
    }
    pt = &gPathPoints[p];
    t = dist2D(gHeli.x, gHeli.z, pt->x * 5.0f, pt->z * -5.0f) / 600.0f;
    dx = pt->x * 5.0f - gHeli.x;
    dz = pt->z * -5.0f - gHeli.z;
    a = ((f32)(s16)(atan2s(dx, dz) + 0xC000) * 3.1415927f) / 32768.0f;
    ox = cosf_tbl(a) * gHeli.side;
    oz = sinf_tbl(a) * gHeli.side;
    d = angleWrap(gHeli.heading - ((f32)(s16)(atan2s(dx - ox, dz - oz) + 0xC000) * 3.1415927f) / 32768.0f);
    if (t > 1.0f) {
        d = d / (1.0f - t);
    }
    gHeli.heading += d * 1.95f;
    gHeli.x -= sinf_tbl(gHeli.heading) * 300.0f;
    gHeli.z += cosf_tbl(gHeli.heading) * 300.0f;
    gHeli.y = 1300.0f + gHeli.road.pos[1];
}
