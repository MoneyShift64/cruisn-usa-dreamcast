/*
 * race/dvalley.c - triangle tests used by the ground code, and the Death Valley
 * set piece: a runaway train of 7 objects that is placed from the track's
 * animated scenery, rolls along the track with a sound that fades, crashes when a
 * player hits it, and is drawn with its own matrices.
 * Original code: race overlay, ROM 0x51CD0-0x530E0 (RAM 0x80039260-0x8003A670).
 */
#include "common.h"
#include "race.h"
#include "road.h"
#include "scenery.h"
#include "ground.h"
#include "frontend.h"

extern s32 gDvSpawned;                   /* 0x800656B4 ovl2 - the objects have been placed */
extern s32 gDvRolling;                   /* 0x800656B8 ovl2 - set when a player triggers it */
extern f32 gDvTargetX;                   /* 0x800656BC ovl2 - a point 117465 units ahead of the first object */
extern f32 gDvTargetZ;                   /* 0x800656C0 ovl2 */
extern s32 gDvSoundFade;                 /* 0x800656C4 ovl2 - frames left of the rumble fade-out, -1 = stopped */
extern s32 gDvPlayer;                    /* 0x800656C8 ovl2 - the player who set it off */
extern Vec3f gDvBoxFirst[8];             /* 0x80043960 ovl2 - collision box of the first object (car space) */
extern Vec3f gDvBoxOther[8];             /* 0x800439C0 ovl2 - and of the others */
extern f32 gDrawDistance[2];             /* 0x80046E68 ovl2 */
extern f32 gCarDrawDistance;             /* 0x80046E70 ovl2 */
extern s32 gMaxVehicles;                 /* 0x8004A020 ovl2 */
extern u32 *gDlCursor;                   /* 0x80147684 */
extern s32 *gMtxCursor;                  /* 0x80147688 */

f32  sqrtf(f32 x);                       /* 0x80115720 */
void *assetLoad(s32 id);                 /* 0x80000480 */
u32  randInt(s32 n);                     /* 0x80103150 */
f32  randFloat(void);                    /* 0x80103104 */
s32  atan2s(f32 y, f32 x);               /* 0x80114100 */
void mtxScale(s32 *m, s32 sx, s32 sy, s32 sz); /* 0x801144DC - fixed-point 4x4 scale */
void mtxFromAngles(s32 *m, s32 ax, s32 ay, s32 az); /* 0x80114824 */
s32  roadIsLeft(RoadPos *rp);            /* 0x8011EEA0 */
void sfxPlay(s32 id, s32 player);        /* 0x8011F2A4 */
void sfxSetVolume(s32 id, s32 player, f32 volume); /* 0x8011F3E0 */
void sfxStop(s32 id, s32 player);        /* 0x8011F670 */
void carModelPose(Vec3f *local, CarBody *body); /* 0x8002662C ovl2 */
void carProbesFindGround(Car *car, s32 side); /* 0x8002690C ovl2 */
void carPlaySfx(Car *car, s32 id, f32 volume); /* 0x800378DC ovl2 */
s32  carCompareAhead(Car *a, Car *b);    /* 0x80037BB4 ovl2 */
void carStartSpin(Car *car, s32 steps, f32 rate, f32 amount); /* 0x80037C8C ovl2 */
f32  dist2D(f32 x1, f32 y1, f32 x2, f32 y2); /* 0x8003840C ovl2 */
f32  sinf_tbl(f32 a);                    /* 0x80038458 ovl2 */
f32  cosf_tbl(f32 a);                    /* 0x800384AC ovl2 */
void bytesClear(u8 *p, s32 n);           /* 0x80038500 ovl2 */
void mtx3RotY(Mtx3 m, f32 a);            /* 0x80038568 ovl2 */
void mtx3ToAngles(Mtx3 m, s32 *outX, s32 *outY, s32 *outZ); /* 0x800389D4 ovl2 */
void trafficCrash(Car *car, VehicleState *vs, s32 v); /* 0x8003C0CC ovl2 */

void dvStart(s32 player, s32 unused);    /* 0x8003A280 ovl2 */
void dvObjectDraw(s32 index, s32 player); /* 0x8003A304 ovl2 */

/* Triangle corner coordinates: u along corner 1, v along corner 2 (x/z only) */
#define TRI_UV(p, tri, u, v) {                                                   \
    f32 x0 = (tri)[0].x;                                                         \
    f32 z0 = (tri)[0].z;                                                         \
    f32 px = (p).x - x0, x1 = (tri)[1].x - x0, pz = (p).z - z0, x2 = (tri)[2].x - x0; \
    f32 z1 = (tri)[1].z - z0, z2 = (tri)[2].z - z0;                              \
    if (x1 == 0.0f) {                                                            \
        u = px / x2;                                                             \
    } else {                                                                     \
        u = (pz * x1 - px * z1) / (z2 * x1 - x2 * z1);                           \
    }                                                                            \
    if (x2 == 0.0f) {                                                            \
        v = px / x1;                                                             \
    } else {                                                                     \
        v = (z2 * px - x2 * pz) / (z2 * x1 - x2 * z1);                           \
    }                                                                            \
}

/* 0x80039260 ovl2 - is p inside the triangle (x/z), with a 1% tolerance */
s32 triHitXZ(Vec3s p, Vec3s *tri) {
    f32 u, v;

    TRI_UV(p, tri, u, v);
    return u >= -0.01f && v >= -0.01f && v + u <= 1.01f;
}

/* 0x800393C0 ovl2 - is the triangle wound clockwise seen from above */
s32 triFrontFacing(Vec3s *tri) {
    return (tri[1].z - tri[0].z) * (tri[2].x - tri[1].x) - (tri[2].z - tri[1].z) * (tri[1].x - tri[0].x) >= 0.0f;
}

/* 0x80039418 ovl2 - is p inside the triangle (x/z): both coordinates 0..1
 * (original bug: u + v is not checked, so this is really a parallelogram) */
s32 triHitXZStrict(Vec3s p, Vec3s *tri) {
    f32 u, v;

    TRI_UV(p, tri, u, v);
    return u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f;
}

/* 0x8003958C ovl2 - height of the triangle's plane at p.x, p.z */
f32 triHeightAt(Vec3s p, Vec3s *tri) {
    f32 ax = tri[0].x - tri[1].x, ay = tri[0].y - tri[1].y, az = tri[0].z - tri[1].z;
    f32 bx = tri[2].x - tri[1].x, by = tri[2].y - tri[1].y, bz = tri[2].z - tri[1].z;
    f32 nx = ay * bz - az * by;
    f32 ny = az * bx - ax * bz;
    f32 nz = ax * by - ay * bx;
    f32 len = sqrtf(nx * nx + ny * ny + nz * nz);
    f32 inv;

    if (len == 0.0f || ny == 0.0f) {
        return tri[0].y;
    }
    inv = 1.0f / len;
    nx *= inv;
    ny *= inv;
    nz *= inv;
    return ((tri[0].z - p.z) * nz + (nx * (tri[0].x - p.x) + ny * tri[0].y)) / ny;
}

/* 0x80039710 ovl2 - Death Valley: load the rumble sounds, nothing placed yet */
void deathValleyInit(void) {
    assetLoad(0x2A70);
    assetLoad(0x2789);
    gDvRolling = 0;
    gDvSpawned = 0;
    bytesClear((u8 *)gDvObjects, sizeof(gDvObjects));
}

/* 0x8003975C ovl2 - move the objects. The first one keeps pace with the leading
 * player (a little faster while it is behind), the others follow it; crashed
 * ones tumble. Each runs out at odometer 134465 and the rumble fades out. */
void deathValleyUpdate(s32 unused) {
    DvObject *e;
    Car *lead;
    Car *car;
    VehicleState *vs;
    Vec3f *box;
    s32 leader;
    s32 i, crashed;
    f32 speed, t1, t2;

    if (gDvSoundFade > 0) {
        if (--gDvSoundFade == 0) {
            sfxStop(0x64, gDvPlayer);
            gDvSoundFade = -1;
        } else {
            sfxSetVolume(0x64, gDvPlayer, (f32)gDvSoundFade * 0.02f);
        }
    }
    if (gDvSpawned == 0 || gDvRolling == 0) {
        return;
    }
    e = gDvObjects;
    leader = (gNumPlayersM1 != 0);
    speed = e[0].car->speed;
    if (leader) {
        leader = gPlayerSetup[1].place < gPlayerSetup[0].place;
    }
    lead = &gCars[leader];
    t1 = dist2D(lead->pos[0], lead->pos[2], gDvTargetX, gDvTargetZ);
    if (lead->speed > 1.0f) {
        t1 = t1 / lead->speed;              /* frames for the player to get there */
    }
    t2 = 142465.0f - e[0].car->odometer;
    if (e[0].car->speed > 1.0f) {
        t2 /= e[0].car->speed;              /* ... and for the object */
    }
    if (t1 < t2) {
        speed *= 1.01f - randFloat() * 0.003f;
    } else {
        speed *= 0.99f;
    }
    if (speed < 170.0f) {
        speed = 170.0f;
    } else {
        speed = (speed > 280.0f) ? 280.0f : speed;
    }
    crashed = 0;
    for (i = 0; i != 7; i++, e++) {
        if (e->active == 0) {
            continue;
        }
        car = e->car;
        vs = &gVehicleStates[car->unk7D];
        box = (i != 0) ? gDvBoxOther : gDvBoxFirst;
        carModelPose(box, CAR_BODY(car));
        if (car->flipping != 0) {
            if (car->pos[1] < -10000.0f) {
                e->active = 0;
                continue;
            }
            crashed++;
            trafficCrash(car, vs, 0);         /* (third argument unused) */
        } else {
            if (crashed != 0) {
                car->speed *= 0.96f;            /* behind a crashed one: slow down */
            } else {
                car->speed = speed;
                if (i == 0 && gRaceState == 1 && randInt(200) == 0) {
                    carPlaySfx(&gCars[leader], randInt(2) ? 0x62 : 0x63, 1.0f);
                }
            }
            car->vel[0] = -sinf_tbl(car->heading) * car->speed;
            car->vel[2] = cosf_tbl(car->heading) * car->speed;
            car->pos[0] += car->vel[0] + car->vel[0];
            car->odometer += car->speed + car->speed;
            car->pos[2] += car->vel[2] + car->vel[2];
            if (car->odometer > 134465.0f) {
                e->active = 0;
                gDvSoundFade = 50;
            }
            mtx3RotY(car->orient, car->heading);
        }
        roadLocate(&car->road, car->pos[0], car->pos[2], 0);
    }
}

/* 0x80039B8C ovl2 - an animated scenery item of kind 'variant' (0-6) became
 * visible: turn it into Death Valley object 'variant', using vehicle slot
 * 19 - variant. */
void animScenerySpawn(s32 player, s32 variant, s32 section, ModelItem *item, MeshCache *mesh) {
    DvObject *e;
    Car *car;
    RoadSection *sec;
    f32 *m;
    s32 slot;
    f32 y, dx, dz, h;

    if ((u32)variant >= 7) {
        return;
    }
    slot = 19 - variant;
    if (20 - gNumVehicles < 4 || gNumVehicles >= slot) {
        return;
    }
    if (gDvRolling != 0 && player != gDvPlayer) {
        return;
    }
    mesh->refs++;
    e = &gDvObjects[variant];
    car = &gCars[slot];
    car->unk7D = slot;
    if (e->dl == NULL) {
        y = (f32)item->y * (1.0f / 65536.0f);
        sec = &gRoadSections[section];
        m = (f32 *)xf2Mul(NULL, (Xform2 *)sec->xf,
                          xf2Rotate(NULL, (s32)(f32)item->angle, (f32)item->x * (1.0f / 65536.0f),
                                    (f32)item->z * (1.0f / 65536.0f)));
        dx = 1.0f;
        dz = 0.0f;
        xf2Apply((Xform2 *)m, &dx, &dz);
        dx -= m[2];
        dz -= m[5];
        e->car = car;
        e->active = 1;
        e->dl = mesh->dl;
        gVehicleStates[slot].roadSide = player;
        h = ((f32)(s16)atan2s(dx, dz) * 3.1415927f) / 32768.0f;
        car->unk84 = 4;
        car->vel[0] = 0.0f;
        car->vel[1] = 0.0f;
        car->vel[2] = 0.0f;
        car->speed = 240.0f;
        car->mass = 2.0f;
        h = 0.0021f - h;
        car->heading = h;
        car->travelDir = h;
        car->pos[0] = m[2] * 5.0f;
        car->pos[1] = (sec->y + y) * 5.0f;
        car->pos[2] = m[5] * -5.0f;
        car->bound = mesh->radius2;
        car->bound = (variant != 0) ? 6496489.0f : 6557121.0f;
        car->model = 0x10;
        car->odometer = (f32)-variant * 4920.0f;  /* spacing behind the first */
        car->road = gCars[player].road;
        mtx3RotY(car->orient, car->heading);
    }
    gDvTargetX = sinf_tbl(car->heading) * -117465.0f + car->pos[0];
    gDvTargetZ = cosf_tbl(car->heading) * 117465.0f + car->pos[2];
    gDvSpawned = 1;
    gMaxVehicles--;
    if (gDvRolling != 0) {
        dvStart(0, 0);
    }
}

/* 0x80039FB0 ovl2 - draw the Death Valley objects in a player's view */
void sceneryDrawExtras(s32 player) {
    s32 i;

    if (gDvSpawned != 0 && gDvRolling != 0) {
        for (i = 0; i != 7; i++) {
            dvObjectDraw(i, player);
        }
    }
}

/* 0x8003A01C ovl2 - a player hit Death Valley object 'index': above 150 it and
 * every object behind it crash; the player is slowed, spun and turned. */
void dvObjectHit(s32 racer, s32 index) {
    Car *me = &gCars[racer];
    DvObject *e;
    Car *car;
    VehicleState *vs;
    f32 r;

    if (me->speed > 150.0f) {
        carPlaySfx(me, 0x83, 1.0f);
        if (index < 7) {
            for (e = &gDvObjects[index]; e < (DvObject *)&gDvSpawned; e++) {
                if (e->active == 0) {
                    continue;
                }
                car = e->car;
                vs = &gVehicleStates[car->unk7D];
                if (car->flipping != 0) {
                    continue;
                }
                if (roadIsLeft(&car->road) != 0) {
                    car->travelDir += randFloat() * 0.05f;
                }
                car->vel[1] = 0.0f;
                car->flipping = 2;
                carProbesFindGround(car, racer);
                car->unk40 = car->probes[0].groundY;
                if (car->odometer - 117465.0f < -10000.0f) {
                    car->speed = 0.0f;
                }
                vs->crashPitchRate = 0.0f;
                r = randFloat() * 0.1f - 0.05f;
                vs->crashYawRate = r;
                car->yawRate = r;
                vs->crashPitch = 0.0f;
                vs->crashYaw = car->heading;
            }
        }
        me->speed *= 0.5f;
    }
    me->travelDir += 3.1415927f;
    if (me->speed < 80.0f) {
        me->speed = 80.0f;
    }
    carStartSpin(me, 0, 0.1f, 3.1415927f);
}

/* 0x8003A280 ovl2 - a player set the objects rolling: start the rumble */
void dvStart(s32 player, s32 unused) {
    if (gDvRolling == 0) {
        gDvRolling = 1;
        gDvPlayer = player;
        sfxPlay(0x64, player);
    }
}

/* 0x8003A2CC ovl2 - fade the rumble out (if it is that player's) */
void dvSoundFade(s32 player, s32 unused) {
    if (player == gDvPlayer && gDvSoundFade == 0) {
        gDvSoundFade = 50;
    }
}

/* 0x8003A304 ovl2 - draw one object (if within draw distance); fades to blue-white
 * as it nears the end of its run */
void dvObjectDraw(s32 index, s32 player) {
    DvObject *e = &gDvObjects[index];
    Car *car;
    RaceCamera *cam;
    f32 x, y, z, d2, fade;
    s32 ax, ay, az;
    s32 *m;
    u32 *g;
    s32 fading;

    if (e->active == 0) {
        return;
    }
    car = e->car;
    cam = &gRaceCameras[player];
    x = car->pos[0] * 0.2f - cam->posX;
    y = car->pos[1] * 0.2f - cam->posY;
    z = car->pos[2] * -0.2f - cam->posZ;
    if (carCompareAhead(&gCars[player], car) > 0) {
        d2 = gDrawDistance[player] * gDrawDistance[player];
    } else {
        d2 = gCarDrawDistance * gCarDrawDistance;
    }
    if (!(x * x + z * z < d2)) {
        return;
    }
    mtx3ToAngles(car->orient, &ax, &ay, &az);
    m = gMtxCursor;
    gMtxCursor = m + 16;
    mtxFromAngles(m, az - 0x4000, ay, ax);
    m[12] = (s32)(x * 65536.0f);
    m[13] = (s32)(y * 65536.0f);
    m[14] = (s32)(z * 65536.0f);
    g = gDlCursor; gDlCursor = g + 2;
    g[1] = (u32)(m - 16);                   /* (the matrix address is stored 0x40 low) */
    g[0] = 0x01020040;                      /* gSPMatrix: load */
    m = gMtxCursor;
    gMtxCursor = m + 16;
    mtxScale(m, 0x3333, 0x3333, 0x3333);    /* x 0.2 */
    g = gDlCursor; gDlCursor = g + 2;
    g[1] = (u32)(m - 16);
    g[0] = 0x01000040;                      /* gSPMatrix: multiply */
    fade = car->odometer - 129465.0f;
    fading = (fade > 0.0f);
    if (fading) {
        fade *= 0.0002f;
        if (fade > 1.0f) {
            fade = 1.0f;
        }
        g = gDlCursor; gDlCursor = g + 2;
        g[1] = 0;
        g[0] = 0xE7000000;                  /* gDPPipeSync */
        g = gDlCursor; gDlCursor = g + 2;
        g[0] = 0xFA000000;                  /* gDPSetPrimColor */
        g[1] = ((s32)((1.0f - fade) * 255.0f) & 0xFF) | 0xC8C8FF00;
    }
    g = gDlCursor; gDlCursor = g + 2;
    g[0] = 0x06000000;                      /* gSPDisplayList */
    g[1] = (u32)e->dl;
    if (fading) {
        g = gDlCursor; gDlCursor = g + 2;
        g[1] = 0;
        g[0] = 0xE7000000;
        g = gDlCursor; gDlCursor = g + 2;
        g[1] = 0xFFFFFFFF;
        g[0] = 0xFA000000;
    }
}
