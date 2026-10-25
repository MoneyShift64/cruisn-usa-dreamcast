/*
 * race/ground.c - how cars touch the world: the five road-contact probes of
 * every car (ground polygon, height and surface type under each), gravity and
 * the airborne flags, knock-over scenery, and car-versus-car collisions.
 * Original code: race overlay, ROM 0x3E6F0-0x42D90 (RAM 0x80025C80-0x8002A320).
 *
 * Ground data: the scenery objects of the draw lists (scenery.h) carry the
 * mesh they are drawn with; the same triangles are used as the collision
 * surface. See include/ground.h and docs/ground.md.
 */
#include "common.h"
#include "race.h"
#include "road.h"
#include "scenery.h"
#include "ground.h"

#define PI_F      3.1415927f
#define HALF_PI_F 1.5707964f

typedef struct RacerHud {                /* 0x39C bytes, 0x80150210 (only the field used here) */
    /* 0x000 */ u8  pad000[0x38C];
    /* 0x38C */ s32 cameraFollowsCar;
    /* 0x390 */ u8  pad390[0x0C];
} RacerHud;
extern RacerHud gRacerHud[];             /* 0x80150210 */
extern u8 *gMtxCursor;                   /* 0x80147688 */
extern s32 gNumPlayersM1;                /* 0x8015095C */
extern s32 gKnockSfx[5];                 /* 0x80042590 ovl2 - knocked-over object, variant 1 */
extern u8  gBoxFaces[6][3];              /* 0x800425A4 ovl2 - 3 corners of each face of a collision box */
extern u8  gBoxFaceCorner[6];            /* 0x800425B8 ovl2 - a corner on each face */
extern s32 gCrashSfxFlipped[4];          /* 0x800425C0 ovl2 */
extern s32 gCrashSfx[5];                 /* 0x800425D0 ovl2 */
extern s32 gLaunchSfx[8];                /* 0x800425E4 ovl2 */
extern s32 gLaunchFlipSfx[4];            /* 0x80042604 ovl2 */

f32  randFloat(void);                    /* 0x80103104 */
u32  randInt(s32 n);                     /* 0x80103150 */
void mtxIdentity(s32 *m);                /* 0x80114450 - fixed-point 4x4 */
void mtxFromAngles(s32 *m, s32 ax, s32 ay, s32 az); /* 0x80114824 - fixed-point 4x4 rotation, binary angles */
s32  carCompareAhead(Car *a, Car *b);    /* 0x80037BB4 ovl2 */
void carPlaySfx(Car *car, s32 id, f32 volume); /* 0x800378DC ovl2 */
void carPlayRandomSfx(Car *car, s32 *table, s32 count, f32 volume); /* 0x80037944 ovl2 */
void carSparks(Car *car, Car *other, s32 front, s32 back, s32 side); /* 0x8003798C ovl2 */
void carStartSpin(Car *car, s32 steps, f32 rate, f32 amount); /* 0x80037C8C ovl2 */
void racerStartFlip(s32 racer);          /* 0x80033138 ovl2 */
s32  triHitXZ(Vec3s p, Vec3s *tri);      /* 0x80039260 ovl2 - p inside the triangle (x/z, small tolerance) */
s32  triFrontFacing(Vec3s *tri);         /* 0x800393C0 ovl2 - x/z winding test */
s32  triHitXZStrict(Vec3s p, Vec3s *tri); /* 0x80039418 ovl2 - p inside the triangle (x/z, exact 0..1) */
f32  triHeightAt(Vec3s p, Vec3s *tri);   /* 0x8003958C ovl2 - height of the triangle's plane at p.x, p.z */
void dvObjectHit(s32 racer, s32 index);  /* 0x8003A01C ovl2 - a player hit a Death Valley obstacle */
void debrisSpawn(f32 x, f32 y, f32 z);   /* 0x8003FE94 ovl2 */
s32  objectAnimHit(s32 kind, s32 variant, DrawObj *obj, Car *car); /* 0x80040CE4 ovl2 */

/* 0x80025C80 ovl2 - race start: nothing is being knocked over */
void raceObjectsInit(void) {
    gFallingObj.obj = NULL;
}

/* 0x80025C90 ovl2 - find the ground under the probes, apply gravity to each
 * probe, set the airborne flags and tilt the car to the ground.
 * side: road side to search (0/1), 2/3 = split road (try 0 then 1 / 1 then 0). */
void carGroundUpdate(Car *car, s32 side) {
    CarProbe *p;
    s32 i;

    carProbesFindGround(car, side);
    p = car->probes;
    for (i = 0; i < 5; i++, p++) {
        if (p->unk20 <= -9.0f) {
            /* sunk more than 9 below the ground: put it back 9 below */
            p->fallSpeed = 0.0f;
            p->y -= p->unk20 - 9.0f;
        } else {
            p->fallSpeed += 8.0f;
            /* 32 more unless the surface is flagged 0x30 (or there is no ground at all) */
            if (p->offroad < SURF_NONE && (p->offroad >= SURF_EDGE || (p->surfFlags & 0x30) != 0x30)) {
                p->fallSpeed += 32.0f;
            }
            if (p->unk20 < p->fallSpeed) {
                p->fallSpeed = p->unk20;  /* don't fall through: land on the ground (or climb onto it) */
            }
            if (p->fallSpeed < -100.0f) {
                p->fallSpeed = -100.0f;
            }
            p->y -= p->fallSpeed;
        }
    }
    car->airborne = (car->probes[0].unk20 >= 72.0f);
    car->unk2D = (car->probes[1].unk20 >= 72.0f) && (car->probes[2].unk20 >= 72.0f);
    carAlignToGround(car);
}

/* 0x80025E38 ovl2 - once per frame while racing: animate the knocked-over
 * object, then all collisions (cars with cars, cars with scenery objects,
 * Death Valley obstacles). */
void raceObjectsUpdate(void) {
    s32 i;
    s32 j;
    s32 *m;
    f32 r;

    if (gRaceState != 1) {
        return;
    }
    if (gFallingObj.obj != NULL) {
        gFallingObj.angle += gFallingObj.rate;
        if (gFallingObj.bouncing != 0) {
            if (--gFallingObj.timer == 0) {
                gFallingObj.timer = 3;
                if (gFallingObj.rate > 0.0f) {
                    gFallingObj.rate *= -0.6f;   /* swings back slower... */
                } else {
                    gFallingObj.rate *= -1.0f;
                }
            }
            r = (gFallingObj.rate < 0.0f) ? -gFallingObj.rate : gFallingObj.rate;
            if (r < 0.01f) {
                fallingObjectStop(0);
            }
        } else if (gFallingObj.angle >= HALF_PI_F) {
            fallingObjectStop(0);           /* lying flat */
        }
    }
    if (gFallingObj.obj != NULL) {
        m = (s32 *)gMtxCursor;
        gMtxCursor += 0x40;
        gFallingMtx = m;
        mtxFromAngles(m, 0, -(s32)((gFallingObj.angle * 32768.0f) / PI_F), 0);
        gFallingMtx[12] = 0;
        gFallingMtx[13] = 0;
        gFallingMtx[14] = 0;
    }

    if (gNumPlayersM1 != 0) {
        carCollide(0, 1, 1);
    }
    carsCollidePlayers();
    for (i = gNumPlayers; i < gNumVehicles - 1; i++) {
        if (gVehicleStates[i].timer == 0) {
            for (j = i + 1; j < gNumVehicles; j++) {
                if (i != j) {
                    carCollide(i, j, 0);
                }
            }
        }
    }

    for (i = 0; i < gNumVehicles; i++) {
        switch (gCars[i].unk84) {
            case 0:
                carHitObjects(i, i);
                break;
            case 1:
            case 2:
            case 3:
                if (gVehicleStates[i].onSplitRoad) {
                    carHitObjects(i, gVehicleStates[i].roadSide);
                }
                break;
        }
    }

    if (gCurrentTrack == 6) {
        for (i = 0; i < gNumVehicles; i++) {
            for (j = 0; j < 7; j++) {
                carCollideDvObject(i, j);
            }
        }
    }
    if (gCurrentTrack == 6) {
        /* tumbling obstacles also hit each other */
        for (i = 0; i < 6; i++) {
            if (gDvObjects[i].active && gDvObjects[i].car->flipping) {
                for (j = i + 1; j < 7; j++) {
                    if (gDvObjects[j].active && gDvObjects[j].car->flipping) {
                        carCollideDvObject(gDvObjects[i].car->unk7D, j);
                    }
                }
            }
        }
    }
}

/* 0x80026244 ovl2 - every player against every computer car */
void carsCollidePlayers(void) {
    s32 i;
    s32 j;

    for (i = 0; i < gNumPlayers; i++) {
        for (j = gNumPlayers; j < gNumVehicles; j++) {
            if (i != j) {
                carCollide(i, j, 0);
            }
        }
    }
}

/* 0x800262D4 ovl2 - find the ground polygon under a probe: sets groundY,
 * unk20 (height above it), surfFlags, section and offroad (SURF_*).
 * vehicle < 0: no route-width check. side: which player's draw lists. */
void probeFindGround(CarProbe *p, s32 vehicle, s32 side) {
    GroundHit *e;
    s32 i;

    if (vehicle >= 0 && probeOutsideRoad(p->x, p->z, vehicle)) {
        p->offroad = SURF_OUTSIDE;
        gGroundHit.obj = NULL;
        p->unk20 = p->y - p->groundY;
        return;
    }
    /* Grand Canyon, section 0x3A: overlapping road surfaces - always do the full search */
    if (gCurrentTrack != 8 || vehicle < 0 || gPathPoints[gCars[vehicle].road.point].section != 0x3A) {
        groundCacheAdd((GroundHit *)p);
        /* (the original nests each of these loops in an outer loop on the same
         * counter, so they still run once) */
        for (i = 0, e = gGroundCache; i < gGroundCacheCount; i++, e++) {
            if (groundCacheTestFace(p, e) == 0) {   /* same triangle as a recent hit */
                p->unk20 = p->y - p->groundY;
                return;
            }
        }
        for (i = 0, e = gGroundCache; i < gGroundCacheCount; i++, e++) {
            if (groundCacheTestObj(p, e) == 0) {    /* another triangle of a recent object */
                p->unk20 = p->y - p->groundY;
                return;
            }
        }
    }
    if (groundSearchList(p, gDrawListA.head[side], side) != 0) {
        if (groundSearchOther(p, gDrawListB.head[side], side) != 0) {
            p->offroad = SURF_NONE;
            gGroundHit.obj = NULL;
        } else {
            p->offroad = SURF_EDGE;
        }
    } else if ((p->surfFlags & 0xB00) == 0xB00) {
        p->offroad = SURF_ROAD;
    } else {
        p->offroad = SURF_GROUND;
    }
    if (gGroundHit.obj != NULL) {
        gGroundHit.surface = p->offroad;
        groundCacheAdd(&gGroundHit);
    }
    p->unk20 = p->y - p->groundY;
    *(GroundHit *)p = gGroundHit;
}

/* 0x80026588 ovl2 */
void groundCacheClear(void) {
    gGroundCacheCount = 0;
}

/* 0x80026598 ovl2 - remember a polygon (unless full or already there) */
void groundCacheAdd(GroundHit *hit) {
    GroundHit *e;
    s32 i;

    if (gGroundCacheCount < 8) {
        e = gGroundCache;
        if (hit->obj != NULL) {
            for (i = 0; i < gGroundCacheCount; i++, e++) {
                if (hit->obj == e->obj && hit->face == e->face) {
                    return;
                }
            }
            *e = *hit;
            gGroundCacheCount++;
        }
    }
}

/* 0x8002662C ovl2 - the car's 8 collision-box corners in world space (body->corners) */
void carModelPose(Vec3f *local, CarBody *body) {
    Vec3f *out;
    s32 i;

    out = body->corners;
    for (i = 0; i < 8; i++) {
        vec3MulMtx(*out, *local, body->orient);
        local++;
        out++;
        out[-1][0] = out[-1][0] + body->pos[0];
        out[-1][1] = out[-1][1] + body->pos[1];
        out[-1][2] = out[-1][2] + body->pos[2];
    }
}

/* 0x800266D8 ovl2 - lowest clearance under two of the car's box corners
 * (pulled in to 70%): the corner that was lowest last time and the next one of a
 * rotating cycle. Returns FALSE if either has no ground. */
s32 carGroundDelta(Car *car, f32 *out) {
    VehicleState *vs;
    s32 idx[2];
    Vec3f pts[2];
    f32 best;
    s32 side;
    s32 k;

    vs = &gVehicleStates[car->unk7D];
    idx[0] = vs->pad40[0] & 7;              /* corner that was lowest last time */
    idx[1] = vs->pad40[1] & 7;              /* corner cycle */
    vs->pad40[1]++;
    side = car->unk7D;
    if (side >= gNumPlayers) {
        side = gVehicleStates[side].roadSide;
    }
    for (k = 0; k < 2; k++) {
        vec3MulMtx(pts[k], gCarModels[car->model].corners[idx[k]], car->orient);
        pts[k][0] *= 0.7f;
        pts[k][2] *= 0.7f;
        /* (the original also has a never-true test that would scale y) */
        pts[k][0] += car->pos[0];
        pts[k][1] += car->pos[1];
        pts[k][2] += car->pos[2];
        gProbeTmp.x = pts[k][0];
        gProbeTmp.y = pts[k][1];
        gProbeTmp.z = pts[k][2];
        probeFindGround(&gProbeTmp, car->unk7D, side);
        if (gProbeTmp.offroad >= SURF_NONE) {
            return FALSE;
        }
        if (k == 0 || gProbeTmp.unk20 < best) {
            best = gProbeTmp.unk20;
            vs->pad40[0] = idx[k];
        }
    }
    *out = best;
    return TRUE;
}

/* 0x8002690C ovl2 - place the 5 probes on the car and find the ground under
 * each. The polygon cache is seeded with the probes' hits from last frame. */
void carProbesFindGround(Car *car, s32 side) {
    CarProbe *p;
    s32 i;

    groundCacheClear();
    p = car->probes;
    for (i = 0; i < 5; i++, p++) {
        groundCacheAdd((GroundHit *)p);
    }
    p = car->probes;
    for (i = 0; i < 5; i++, p++) {
        vec3MulMtx(&p->x, gCarModels[car->model].probes[i], car->orient);
        p->x += car->pos[0];
        p->y += car->pos[1];
        p->z += car->pos[2];
        if (side >= 2) {
            /* split road: this side first, the other one if there is no ground */
            probeFindGround(p, car->unk7D, side - 2);
            if (p->offroad == SURF_NONE) {
                probeFindGround(p, car->unk7D, 3 - side);
            }
        } else {
            probeFindGround(p, car->unk7D, side);
        }
    }
}

/* 0x80026A5C ovl2 - tilt the car to the plane of probes 0, 1, 2 and put it
 * at the height of probe 0 */
void carAlignToGround(Car *car) {
    Vec3f n;
    Mtx3 m;

    vec3TriNormal(n, &car->probes[0].x, &car->probes[1].x, &car->probes[2].x);
    vec3Normalize(n);
    m[0][0] = n[1];
    m[0][1] = -n[0];
    m[0][2] = 0.0f;
    m[1][0] = n[0];
    m[1][1] = n[1];
    m[1][2] = n[2];
    vec3Normalize(m[0]);
    m[2][0] = m[0][1] * m[1][2] - m[1][1] * m[0][2];
    m[2][1] = m[0][2] * m[1][0] - m[1][2] * m[0][0];
    m[2][2] = m[0][0] * m[1][1] - m[1][0] * m[0][1];
    vec3Normalize(m[2]);
    mtx3Transpose(car->orient, m);
    car->pos[1] = car->probes[0].y - gCarModels[car->model].probes[0][1];
}

/* 0x80026B50 ovl2 - is the probe over the cached triangle itself? 0 = yes */
s32 groundCacheTestFace(CarProbe *p, GroundHit *hit) {
    DrawObj *obj;
    MeshCache *mc;
    f32 mx;
    f32 mz;
    f32 dx;
    f32 dz;

    obj = hit->obj;
    if (hit->item == obj->item) {
        mx = p->x * 0.2f;
        mz = p->z * -0.2f;
        mc = obj->mesh;
        dx = obj->xf[2] - mx;
        dz = obj->xf[5] - mz;
        if (mc != NULL && dx * dx + dz * dz <= mc->radius2
            && faceFindGround(mx, mz, p, hit->face, mc, obj) == 0) {
            p->surfFlags = obj->item->flags;
            p->groundY += obj->y * 5.0f;
            p->offroad = hit->surface;
            p->section = obj->section;
            return 0;
        }
    }
    return 1;
}

/* 0x80026C48 ovl2 - is the probe over any triangle of the cached object? 0 = yes */
s32 groundCacheTestObj(CarProbe *p, GroundHit *hit) {
    DrawObj *obj;
    MeshCache *mc;
    f32 mx;
    f32 mz;
    f32 dx;
    f32 dz;

    obj = hit->obj;
    if (hit->item == obj->item) {
        mx = p->x * 0.2f;
        mz = p->z * -0.2f;
        mc = obj->mesh;
        dx = obj->xf[2] - mx;
        dz = obj->xf[5] - mz;
        if (mc != NULL && dx * dx + dz * dz <= mc->radius2
            && meshFindGround(mx, mz, p, mc, obj) == 0) {
            p->surfFlags = obj->item->flags;
            p->groundY += obj->y * 5.0f;
            p->offroad = hit->surface;
            p->section = obj->section;
            return 0;
        }
    }
    return 1;
}

/* 0x80026D38 ovl2 - search a draw list (list A: road / verge) for a triangle
 * under the probe; the hit goes to gGroundHit. 0 = found. */
s32 groundSearchList(CarProbe *p, DrawObj *obj, s32 side) {
    GroundHit saved;
    MeshCache *mc;
    s32 found;
    f32 mx;
    f32 my;
    f32 mz;
    f32 dx;
    f32 dz;

    mx = p->x * 0.2f;
    my = p->y * 0.2f;
    found = FALSE;
    mz = p->z * -0.2f;
    for (; obj != NULL; obj = obj->next[side]) {
        mc = obj->mesh;
        dx = obj->xf[2] - mx;
        dz = obj->xf[5] - mz;
        if (mc == NULL || !(dx * dx + dz * dz <= mc->radius2)) {
            continue;
        }
        gGroundHit.obj = obj;
        gGroundHit.item = obj->item;
        if ((obj->drawFlags & 8) && my + 500.0f < obj->y) {
            continue;                       /* flagged object more than 500 above the probe (bridge deck) */
        }
        if (meshFindGround(mx, mz, p, obj->mesh, obj) != 0) {
            continue;
        }
        p->surfFlags = obj->item->flags;
        p->groundY += obj->y * 5.0f;
        p->section = obj->section;
        if (gCurrentTrack == 8 && obj->section == 0x3A && !found) {
            /* Grand Canyon section 0x3A: keep looking for a second surface,
             * fall back to this one */
            found = TRUE;
            saved = gGroundHit;
            continue;
        }
        return 0;
    }
    if (found) {
        gGroundHit = saved;
        return 0;
    }
    gGroundHit.obj = NULL;
    return 1;
}

/* 0x80026F7C ovl2 - search a draw list (list B: terrain and other scenery);
 * does not touch gGroundHit. 0 = found. */
s32 groundSearchOther(CarProbe *p, DrawObj *obj, s32 side) {
    MeshCache *mc;
    f32 mx;
    f32 mz;
    f32 dx;
    f32 dz;

    mx = p->x * 0.2f;
    mz = p->z * -0.2f;
    for (; obj != NULL; obj = obj->next[side]) {
        mc = obj->mesh;
        dx = obj->xf[2] - mx;
        dz = obj->xf[5] - mz;
        if (mc != NULL && dx * dx + dz * dz <= mc->radius2
            && meshFindGround(mx, mz, p, mc, obj) == 0) {
            p->surfFlags = obj->item->flags;
            p->groundY += obj->y * 5.0f;
            p->section = obj->section;
            return 0;
        }
    }
    return 1;
}

/* 0x80027090 ovl2 - find the triangle of an object's mesh under route point
 * (mx, mz). Sets p->tri, p->groundY (world units, without the object's y)
 * and gGroundHit.face. 0 = found. */
s32 meshFindGround(f32 mx, f32 mz, CarProbe *p, MeshCache *mc, DrawObj *obj) {
    GroundMesh *mesh;
    MeshVert *v;
    f32 (*xz)[2];
    u8 *rec;
    s32 off;
    s32 n;
    s32 records;
    s32 count;
    s32 tri;
    s32 k;
    f32 vx;
    f32 vz;
    f32 minX;
    f32 maxX;
    f32 minZ;
    f32 maxZ;
    Vec3s pt;
    Vec3s t[3];

    /* transform every vertex to route x/z */
    mesh = mc->key;
    v = mesh->verts;
    xz = gMeshXZ;
    n = mesh->lastVert + 1;
    while (n-- != 0) {
        vx = (f32)(v->x * 0.2);
        vz = (f32)(v->z * 0.2);
        v++;
        xz++;
        xz[-1][0] = obj->xf[2] + (obj->xf[0] * vx + obj->xf[1] * vz);
        xz[-1][1] = obj->xf[5] + (obj->xf[3] * vx + obj->xf[4] * vz);
    }

    rec = (u8 *)mesh + mesh->lastUV * 4 + mesh->lastVert * 6 + 0x12;
    tri = 0;
    records = mesh->numRecords;
    off = rec - (u8 *)mesh;
    while (records-- != 0) {
        /* group header: u32 kept 4-byte aligned, count byte on the other side */
        if (!(off & 2)) {
            count = rec[4];
        } else {
            count = rec[0];
        }
        off += 6;
        rec += 6;
        while (count-- != 0) {
            t[0].x = gMeshXZ[rec[0]][0];
            t[0].y = (f32)(mesh->verts[rec[0]].y * 0.2);
            t[0].z = gMeshXZ[rec[0]][1];
            minX = maxX = t[0].x;
            minZ = maxZ = t[0].z;
            for (k = 1; k < 3; k++) {
                t[k].x = gMeshXZ[rec[k]][0];
                t[k].y = (f32)(mesh->verts[rec[k]].y * 0.2);
                t[k].z = gMeshXZ[rec[k]][1];
                if (t[k].x < minX) {
                    minX = t[k].x;
                }
                if (maxX < t[k].x) {
                    maxX = t[k].x;
                }
                if (t[k].z < minZ) {
                    minZ = t[k].z;
                }
                if (maxZ < t[k].z) {
                    maxZ = t[k].z;
                }
            }
            if (minX <= mx && mx <= maxX && minZ <= mz && mz <= maxZ) {
                pt.x = mx;
                pt.z = mz;
                if (triHitXZ(pt, t) && triFrontFacing(t)) {
                    p->tri = tri;
                    p->groundY = triHeightAt(pt, t) * 5.0f;
                    gGroundHit.face = rec;
                    return 0;
                }
            }
            rec += 6;
            off += 6;
            records--;
            tri++;
        }
    }
    return 1;
}

/* 0x80027454 ovl2 - is route point (mx, mz) over this one face? Sets
 * p->groundY (world units, without the object's y). 0 = yes. */
s32 faceFindGround(f32 mx, f32 mz, CarProbe *p, u8 *face, MeshCache *mc, DrawObj *obj) {
    MeshVert *verts;
    MeshVert *v;
    s32 k;
    f32 vx;
    f32 vz;
    f32 minX;
    f32 maxX;
    f32 minZ;
    f32 maxZ;
    Vec3s pt;
    Vec3s t[3];

    verts = ((GroundMesh *)mc->key)->verts;
    for (k = 0; k < 3; k++) {
        v = &verts[face[k]];
        vx = (f32)(v->x * 0.2);
        vz = (f32)(v->z * 0.2);
        gMeshXZ[k][0] = obj->xf[2] + (obj->xf[0] * vx + obj->xf[1] * vz);
        gMeshXZ[k][1] = obj->xf[5] + (obj->xf[3] * vx + obj->xf[4] * vz);
    }
    t[0].x = gMeshXZ[0][0];
    t[0].z = gMeshXZ[0][1];
    minX = maxX = t[0].x;
    minZ = maxZ = t[0].z;
    t[0].y = (f32)(verts[face[0]].y * 0.2);
    for (k = 1; k < 3; k++) {
        t[k].x = gMeshXZ[k][0];
        t[k].z = gMeshXZ[k][1];
        t[k].y = (f32)(verts[face[k]].y * 0.2);
        if (t[k].x < minX) {
            minX = t[k].x;
        }
        if (maxX < t[k].x) {
            maxX = t[k].x;
        }
        if (t[k].z < minZ) {
            minZ = t[k].z;
        }
        if (maxZ < t[k].z) {
            maxZ = t[k].z;
        }
    }
    if (minX <= mx && mx <= maxX && minZ <= mz && mz <= maxZ) {
        pt.x = mx;
        pt.z = mz;
        if (triHitXZ(pt, t) && triFrontFacing(t)) {
            p->groundY = triHeightAt(pt, t) * 5.0f;
            return 0;
        }
    }
    return 1;
}

/* 0x800276F8 ovl2 - has the car run into a scenery object of list E or D?
 * The car's footprint is the triangle (corner 3, corner 2, corner 1 pushed
 * forward by the speed); at most one object per list per frame. */
void carHitObjects(s32 vehicle, s32 side) {
    Car *car;
    DrawObj *obj;
    s32 pass;
    f32 carX;
    f32 carY;
    f32 carZ;
    f32 dx;
    f32 dy;
    f32 dz;
    f32 ax;
    f32 az;
    f32 len;
    f32 inv;
    Vec3s pt;
    Vec3s t[3];

    car = &gCars[vehicle];
    carY = car->pos[1];
    carZ = car->pos[2];
    carX = car->pos[0];
    for (pass = 0; pass != 2; pass++) {
        obj = (pass != 0) ? gDrawListD.head[side] : gDrawListE.head[side];
        for (; obj != NULL; obj = obj->next[side]) {
            if (obj->drawFlags & 4) {
                continue;                   /* already knocked over */
            }
            pt.x = obj->xf[2] * 5.0f;
            pt.y = obj->y * 5.0f;
            pt.z = obj->xf[5] * -5.0f;
            dx = pt.x - carX;
            dy = pt.y - carY;
            dz = pt.z - carZ;
            if (!(dx * dx + dz * dz <= 1000000.0f)) {
                continue;
            }
            ax = car->corners[1][0] - car->corners[3][0];
            az = car->corners[1][2] - car->corners[3][2];
            len = sqrtf(ax * ax + az * az);
            inv = (1.0f < len) ? 1.0f / len : 1.0f;
            inv *= car->speed;
            t[2].x = car->corners[1][0] + ax * inv;
            t[2].z = car->corners[1][2] + az * inv;
            t[0].x = car->corners[3][0];
            t[0].z = car->corners[3][2];
            t[1].x = car->corners[2][0];
            t[1].z = car->corners[2][2];
            if (((dy < 0.0f) ? -dy : dy) < 2000.0f && triHitXZStrict(pt, t)) {
                carHitObject(obj, car, side);
                break;
            }
        }
    }
}

/* 0x800279BC ovl2 - the knocked-over object stops moving */
void fallingObjectStop(s32 unused) {
    if (gFallingObj.obj != NULL) {
        gFallingObj.obj->drawFlags &= ~0x42;  /* no longer drawn with gFallingMtx (0x04 = lying flat stays) */
        gFallingObj.obj = NULL;
        gFallingObj.bouncing = 0;
    }
}

/* 0x800279F0 ovl2 - a car ran into a scenery object; what happens depends on
 * the item's subtype (flags & 0xF0) and variant (flags & 0xF):
 *   0x10, 0x20  knocked over (0x10 variant 5: springs back and forth, solid)
 *   0x30, 0x50, 0xF0  animated object (objectAnimHit) if within 250 in height
 *   0x40  variant 0: solid
 * A solid object bounces the car off (as from a car standing still there). */
void carHitObject(DrawObj *obj, Car *car, s32 side) {
    CarBody post;
    s32 *m;
    u16 flags;
    s32 kind;
    s32 variant;
    s32 solid;
    s32 sfx;
    s32 n;
    f32 v;
    f32 dy;

    flags = obj->item->flags;
    solid = FALSE;
    kind = flags & 0xF0;
    variant = flags & 0xF;
    if (kind > 0x50) {
        if (kind == 0xF0) {
            goto anim;
        }
        goto done;
    }
    switch (kind) {
        case 0x10:
        case 0x20:
            fallingObjectStop(0);
            m = (s32 *)gMtxCursor;
            gFallingObj.obj = obj;
            gMtxCursor += 0x40;
            gFallingMtx = m;
            gFallingObj.angle = 0.0f;
            mtxIdentity(m);
            gFallingObj.bouncing = (kind == 0x10 && variant == 5);
            if (gFallingObj.bouncing) {
                v = car->speed * 0.04f;
                if (v < 0.13f) {
                    v = 0.13f;
                } else if (1.0f < v) {
                    v = 1.0f;
                }
                gFallingObj.timer = 3;
                gFallingObj.rate = v * -0.4f;
                obj->drawFlags |= 0x40;
                for (n = randInt(2) + 1; n != 0; n--) {
                    debrisSpawn(obj->xf[2], obj->y, obj->xf[5]);
                }
                solid = TRUE;
            } else {
                v = car->speed * 0.03f;
                if (v < 0.1f) {
                    gFallingObj.rate = 0.1f;
                } else {
                    gFallingObj.rate = (0.7f < v) ? 0.7f : v;
                }
                obj->drawFlags |= 6;
                /* falls in the direction the car is going; the car loses 20% of its speed (at least 10) */
                xf2Rotate((Xform2 *)obj->xf, (s32)((car->heading * 32768.0f) / PI_F), obj->xf[2], obj->xf[5]);
                v = car->speed * 0.2f;
                if (v < 10.0f) {
                    v = 10.0f;
                }
                if (car->speed < v) {
                    v = car->speed;
                }
                car->speed = car->speed - v;
            }
            sfx = 0x73;
            switch (variant) {
                case 1:
                    sfx = gKnockSfx[randInt(5)];
                    break;
                case 2:
                    sfx = 0x28;
                    break;
                case 3:
                    sfx = 0x45;
                    break;
                case 5:
                    if (gCurrentTrack == 4) {
                        sfx = 0x9C;
                    }
                    break;
            }
            carPlaySfx(car, sfx, 1.0f);
            goto done;
        case 0x30:
        case 0x50:
        anim:
            dy = car->pos[1] - obj->y * 5.0f;
            if (-250.0f <= dy && dy <= 250.0f) {
                objectAnimHit(kind, variant, obj, car);
            }
            goto done;
        case 0x40:
            solid = (variant == 0);
            break;
    }
done:
    if (solid) {
        post.pos[0] = obj->xf[2] * 5.0f;
        post.pos[2] = obj->xf[5] * -5.0f;
        post.vel[0] = 0.0f;
        post.vel[2] = 0.0f;
        carBounceOff(car, &post, FALSE);
        carPlaySfx(car, 0x55, 1.0f);
    }
}

/* 0x80027E08 ovl2 - collide car a with car b. Each car's box is first
 * stretched forward by how fast it closes on the other (front corners 0, 1,
 * 4, 5), tested, and restored. gentle (1 = player against player): full strength. */
void carCollide(s32 ia, s32 ib, s32 gentle) {
    Car *a;
    Car *b;
    Vec3f saveA[4];
    Vec3f saveB[4];
    f32 dx;
    f32 dz;
    f32 dvx;
    f32 dvz;
    f32 sinA;
    f32 cosA;
    f32 sinB;
    f32 cosB;
    f32 closeA;
    f32 closeB;
    f32 ex;
    f32 ez;
    f32 strength;
    s32 movedA;
    s32 movedB;
    s32 k;

    if (gVehicleStates[ib].timer != 0) {
        return;
    }
    a = &gCars[ia];
    b = &gCars[ib];
    dx = a->pos[0] - b->pos[0];
    dz = a->pos[2] - b->pos[2];
    if (!(dx * dx + dz * dz <= a->bound + b->bound)) {
        return;
    }
    saveB[0][0] = b->corners[0][0]; saveB[0][1] = b->corners[0][1]; saveB[0][2] = b->corners[0][2];
    saveB[1][0] = b->corners[1][0]; saveB[1][1] = b->corners[1][1]; saveB[1][2] = b->corners[1][2];
    saveB[2][0] = b->corners[4][0]; saveB[2][1] = b->corners[4][1]; saveB[2][2] = b->corners[4][2];
    saveB[3][0] = b->corners[5][0]; saveB[3][1] = b->corners[5][1]; saveB[3][2] = b->corners[5][2];
    dvx = a->vel[0] - b->vel[0];
    dvz = a->vel[2] - b->vel[2];
    sinA = -sinf_tbl(a->heading);
    cosA = cosf_tbl(a->heading);
    sinB = -sinf_tbl(b->heading);
    cosB = cosf_tbl(b->heading);
    closeA = (cosA * dvz + sinA * dvx) * 0.5f;
    closeB = (cosB * dvz + sinB * dvx) * -0.5f;
    movedA = (closeA > 0.0f);
    if (movedA) {
        saveA[0][0] = a->corners[0][0]; saveA[0][1] = a->corners[0][1]; saveA[0][2] = a->corners[0][2];
        saveA[1][0] = a->corners[1][0]; saveA[1][1] = a->corners[1][1]; saveA[1][2] = a->corners[1][2];
        saveA[2][0] = a->corners[4][0]; saveA[2][1] = a->corners[4][1]; saveA[2][2] = a->corners[4][2];
        saveA[3][0] = a->corners[5][0]; saveA[3][1] = a->corners[5][1]; saveA[3][2] = a->corners[5][2];
        ex = sinA * closeA;
        ez = cosA * closeA;
        for (k = 0; k < 6; k++) {           /* corners 0, 1, 4, 5 */
            a->corners[k][0] += ex;
            a->corners[k][2] += ez;
            if (k == 1) {
                k = 3;
            }
        }
    }
    movedB = (closeB > 0.0f);
    if (movedB) {
        ex = sinB * closeB;
        ez = cosB * closeB;
        for (k = 0; k < 6; k++) {
            b->corners[k][0] += ex;
            b->corners[k][2] += ez;
            if (k == 1) {
                k = 3;
            }
        }
    }
    if (boxesOverlap(a->corners, b->corners)) {
        if (gentle) {
            strength = 1.0f;
        } else {
            /* slow crashes push harder: up to 3x at a closing speed of 0 */
            strength = a->speed + b->speed;
            if (100.0f < strength) {
                strength = 100.0f;
            }
            strength = (100.0f - strength) * 0.02f + 1.0f;
        }
        carCrash(a, b, strength);
    }
    if (movedA) {
        a->corners[0][0] = saveA[0][0]; a->corners[0][1] = saveA[0][1]; a->corners[0][2] = saveA[0][2];
        a->corners[1][0] = saveA[1][0]; a->corners[1][1] = saveA[1][1]; a->corners[1][2] = saveA[1][2];
        a->corners[4][0] = saveA[2][0]; a->corners[4][1] = saveA[2][1]; a->corners[4][2] = saveA[2][2];
        a->corners[5][0] = saveA[3][0]; a->corners[5][1] = saveA[3][1]; a->corners[5][2] = saveA[3][2];
    }
    if (movedB) {
        b->corners[0][0] = saveB[0][0]; b->corners[0][1] = saveB[0][1]; b->corners[0][2] = saveB[0][2];
        b->corners[1][0] = saveB[1][0]; b->corners[1][1] = saveB[1][1]; b->corners[1][2] = saveB[1][2];
        b->corners[4][0] = saveB[2][0]; b->corners[4][1] = saveB[2][1]; b->corners[4][2] = saveB[2][2];
        b->corners[5][0] = saveB[3][0]; b->corners[5][1] = saveB[3][1]; b->corners[5][2] = saveB[3][2];
    }
}

/* 0x8002838C ovl2 - a car against one of the Death Valley obstacles */
void carCollideDvObject(s32 vehicle, s32 index) {
    DvObject *e;
    Car *obj;
    Car *car;
    f32 dx;
    f32 dz;

    e = &gDvObjects[index];
    obj = e->car;
    if (e->active) {
        car = &gCars[vehicle];
        dx = car->pos[0] - obj->pos[0];
        dz = car->pos[2] - obj->pos[2];
        if (dx * dx + dz * dz <= car->bound + obj->bound && boxesOverlap(car->corners, obj->corners)) {
            carCrash(car, obj, 1.0f);
            if (vehicle < gNumPlayers) {
                dvObjectHit(vehicle, index);
            }
        }
    }
}

/* 0x80028490 ovl2 - (unnormalised) outward normals of the 6 faces of a box */
void boxFaceNormals(Vec3f *box, Vec3f *normals) {
    f32 *a;
    f32 *b;
    f32 *c;
    f32 e1x;
    f32 e1y;
    f32 e1z;
    f32 e2x;
    f32 e2y;
    f32 e2z;
    s32 i;

    for (i = 0; i < 6; i++) {
        a = box[gBoxFaces[i][0]];
        b = box[gBoxFaces[i][1]];
        c = box[gBoxFaces[i][2]];
        e1x = b[0] - a[0];
        e1y = b[1] - a[1];
        e1z = b[2] - a[2];
        e2y = c[1] - b[1];
        e2z = c[2] - b[2];
        e2x = c[0] - b[0];
        normals[i][0] = e1z * e2y - e1y * e2z;
        normals[i][1] = e1x * e2z - e1z * e2x;
        normals[i][2] = e1y * e2x - e1x * e2y;
    }
}

/* 0x80028584 ovl2 - is p inside the box (behind all 6 faces)? */
s32 boxContainsPoint(Vec3f *box, Vec3f *normals, Vec3f p) {
    f32 *c;
    f32 d;
    s32 i;

    for (i = 0; i < 6; i++) {
        c = box[gBoxFaceCorner[i]];
        d = normals[i][0] * (p[0] - c[0]) + normals[i][1] * (p[1] - c[1]) + normals[i][2] * (p[2] - c[2]);
        if (d > 0.0f) {
            return FALSE;
        }
    }
    return TRUE;
}

/* 0x80028638 ovl2 - does any of the 8 points of `pts` lie inside `box`, or any
 * point on the line from the centre of its back face (corners 2,3,6,7) to the
 * centre of its front face (0,1,4,5), sampled every 500 units? */
s32 boxHitsBox(Vec3f *box, Vec3f *pts) {
    Vec3f normals[6];
    Vec3f back;
    f32 frontX;
    f32 frontY;
    f32 frontZ;
    f32 axX;
    f32 axY;
    f32 axZ;
    f32 len;
    f32 step;
    s32 n;
    s32 i;

    boxFaceNormals(box, normals);
    frontZ = 0.0f;
    i = 0;
    frontX = frontZ;
    frontY = frontZ;
    back[1] = 0.0f;
    back[2] = 0.0f;
    back[0] = 0.0f;
    do {
        if (boxContainsPoint(box, normals, *pts)) {
            return TRUE;
        }
        if (i++ & 2) {
            back[0] += pts[0][0];
            back[1] += pts[0][1];
            back[2] += pts[0][2];
        } else {
            frontX += pts[0][0];
            frontY += pts[0][1];
            frontZ += pts[0][2];
        }
        pts++;
    } while (i < 8);
    frontX *= 0.25f;
    frontY *= 0.25f;
    frontZ *= 0.25f;
    back[0] *= 0.25f;
    axX = frontX - back[0];
    back[1] *= 0.25f;
    back[2] *= 0.25f;
    axY = frontY - back[1];
    axZ = frontZ - back[2];
    len = sqrtf(axX * axX + axY * axY + axZ * axZ);
    if (500.0f < len) {
        step = 500.0f / len;
        n = (s32)(len * 0.002f);
        axX *= step;
        axY *= step;
        axZ *= step;
        while (n-- > 0) {
            back[0] += axX;
            back[1] += axY;
            back[2] += axZ;
            if (boxContainsPoint(box, normals, back)) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

/* 0x8002885C ovl2 */
s32 boxesOverlap(Vec3f *a, Vec3f *b) {
    if (boxHitsBox(a, b)) {
        return TRUE;
    }
    if (boxHitsBox(b, a)) {
        return TRUE;
    }
    return FALSE;
}

/* 0x800288B4 ovl2 - bounce the car off something it ran into (a solid scenery
 * object, or a mirror of itself): pushed apart, 60% speed (at least 37),
 * a spin, and turned round if it was heading into the obstacle. */
void carBounceOff(Car *car, CarBody *other, s32 forceSpin) {
    Vec3f dir;
    f32 rel;
    f32 d;
    f32 ad;
    f32 v;
    s32 slow;

    rel = bodyRelative(CAR_BODY(car), other, dir);
    car->pos[0] = car->pos[0] + rel * dir[0];
    car->pos[2] = car->pos[2] + rel * dir[2];
    v = car->speed * 0.6f;
    slow = FALSE;
    if (v < 37.0f) {
        slow = TRUE;
    }
    if (slow) {
        v = 37.0f;
    }
    car->speed = v;
    if (forceSpin || slow || randInt(2)) {
        carStartSpin(car, 60, (angleWrap(car->road.heading - car->heading) > 0.0f) ? 0.02f : -0.02f, 0.0f);
    } else {
        carStartSpin(car, 0, 0.1f, PI_F);
    }
    d = angleWrap(((f32)(s16)(atan2s(dir[0], dir[2]) + 0xC000) * PI_F) / 32768.0f - car->travelDir);
    ad = d;
    if (d < 0.0f) {
        ad = -d;
    }
    if (forceSpin || HALF_PI_F <= ad) {
        car->travelDir = angleWrap(car->travelDir + PI_F);
    }
}

/* 0x80028A74 ovl2 - dir = unit x/z vector from b to a; returns the x/z
 * relative speed of the two bodies */
f32 bodyRelative(CarBody *a, CarBody *b, Vec3f dir) {
    f32 dvx;
    f32 dvz;

    dir[1] = 0.0f;
    dir[0] = a->pos[0] - b->pos[0];
    dir[2] = a->pos[2] - b->pos[2];
    vec3Normalize(dir);
    dvx = a->vel[0] - b->vel[0];
    dvz = a->vel[2] - b->vel[2];
    return sqrtf(dvx * dvx + dvz * dvz);
}

/* 0x80028AFC ovl2 - two cars' boxes touch: push them apart and share out the
 * momentum (weights mass * speedScale^3), then spin them. Players can launch
 * traffic into the air. strength scales b's new velocity. */
void carCrash(Car *a, Car *b, f32 strength) {
    CarBody mirror;
    Vec3f dir;
    f32 dy;
    f32 rel;
    f32 half;
    f32 px;
    f32 pz;
    f32 vAx;
    f32 vAz;
    f32 vBx;
    f32 vBz;
    f32 d2;
    f32 r;
    f32 mA;
    f32 mB;
    f32 tot;
    f32 inv;
    f32 k;
    f32 kA2;
    f32 kB2;
    f32 cmx;
    f32 cmz;
    f32 nx;
    f32 nz;
    f32 dvx;
    f32 dvz;
    f32 ang;
    s32 i;

    dy = a->pos[1] - b->pos[1];
    if (750.0f < ((dy < 0.0f) ? -dy : dy)) {
        return;                             /* one is far above the other */
    }
    gVehicleStates[b->unk7D].timer = 3;
    carSparks(a, b, 1, 0, 0);
    rel = bodyRelative(CAR_BODY(a), CAR_BODY(b), dir);
    carCrashSound(a, b, rel);
    half = rel * 0.5f;
    px = dir[0] * half;
    pz = dir[2] * half;
    a->pos[0] += px;
    a->pos[2] += pz;
    if (b->unk84 == 4) {
        /* b is traffic */
        if (a->unk7D < gNumPlayers) {
            if (b->flipping == 0) {
                return;
            }
            if (50.0f < b->speed) {
                return;
            }
        } else {
            if (b->flipping == 0) {
                /* computer car against traffic: just bounce off a mirror of itself */
                mirror.vel[0] = -a->vel[0];
                mirror.vel[1] = 0.0f;
                mirror.vel[2] = -a->vel[2];
                mirror.pos[0] = a->pos[0] - mirror.vel[0];
                mirror.pos[2] = a->pos[2] - mirror.vel[2];
                mirror.pos[1] = a->pos[1];
                carBounceOff(a, &mirror, a->spinTimer == 0);
                return;
            }
            if (50.0f < b->speed) {
                return;
            }
        }
    }
    i = b->unk7D;
    if (i >= gNumPlayers && i < gNumPlayers + gNumAiRacers) {
        b->pos[0] -= 4.0f * px;             /* AI racers are pushed 4x as far */
        b->pos[2] -= 4.0f * pz;
    } else {
        b->pos[0] -= px;
        b->pos[2] -= pz;
    }

    ang = a->travelDir;
    vAx = sinf_tbl(ang) * -a->speed;
    vAz = cosf_tbl(ang) * a->speed;
    ang = b->travelDir;
    vBx = sinf_tbl(ang) * -b->speed;
    vBz = cosf_tbl(ang) * b->speed;
    if (a->unk7D < gNumPlayers && b->unk7D >= gNumPlayers) {
        dvx = vAx - vBx;
        dvz = vAz - vBz;
        d2 = dvx * dvx + dvz * dvz;
        if (40000.0f < d2 && carCrashImmune(b) == 0) {
            /* player faster than 160 hitting something at > 200 relative:
             * launch it (the chance factor is 0/1 * d2, so it always happens) */
            r = randFloat();
            if (r < (f32)(160.0f < a->speed) * d2) {
                trafficLaunch(a, b);
                return;
            }
        }
    }
    mA = a->mass * a->speedScale * a->speedScale * a->speedScale;
    mB = b->mass * b->speedScale * b->speedScale * b->speedScale;
    dvx = vAx - vBx;
    dvz = vAz - vBz;
    tot = mA + mB;
    if (tot < 2.0f) {
        tot = 2.0f;
    }
    inv = 1.0f / tot;
    k = (mA - mB) * inv;
    cmx = (mA * vAx + mB * vBx) * inv * 0.75f;
    kB2 = (mB + mB) * inv;
    nx = (dir[0] * 10.0f + cmx) + (vAx * k + vBx * kB2) * 0.25f;
    cmz = (mA * vAz + mB * vBz) * inv * 0.75f;
    nz = (dir[2] * 10.0f + cmz) + (vAz * k + vBz * kB2) * 0.25f;
    a->travelDir = angleWrap(((f32)(s16)atan2s(nx, nz) * PI_F) / 32768.0f - HALF_PI_F);
    a->speed = sqrtf(nx * nx + nz * nz);
    k = -k;
    kA2 = (mA + mA) * inv;
    nx = ((dir[0] * -10.0f + cmx) + (vBx * k + vAx * kA2) * 0.25f) * strength;
    nz = ((dir[2] * -10.0f + cmz) + (vBz * k + vAz * kA2) * 0.25f) * strength;
    ang = ((f32)(s16)atan2s(nx, nz) * PI_F) / 32768.0f - HALF_PI_F;
    b->travelDir = angleWrap(randFloat() * 0.666f + ang - 0.333f);
    b->speed = sqrtf(nx * nx + nz * nz);
    carCrashSpin(a, b, dir, dvx, dvz);
    carCrashSpin(b, a, dir, -dvx, -dvz);
}

/* 0x800291FC ovl2 - how `car` reacts to being hit by `other`: a spin back
 * towards the road, a spin-out, or a small yaw kick. (dvx, dvz) = relative
 * velocity, dir = unit vector between them. */
void carCrashSpin(Car *other, Car *car, Vec3f dir, f32 dvx, f32 dvz) {
    s32 behind;
    s32 spinBack;
    s32 steps;
    f32 impact;
    f32 nx;
    f32 nz;
    f32 mx;
    f32 mz;
    f32 cross;
    f32 len;
    f32 inv;
    f32 dot;
    f32 t;
    f32 sign;
    f32 m;
    f32 spin;
    f32 ang;
    f32 a;
    f32 rate;
    f32 amount;
    f32 k;

    behind = FALSE;
    impact = sqrtf(dvx * dvx + dvz * dvz);
    car->hitFlag = (20.0f < impact);
    nx = dir[0];
    nz = dir[2];
    mx = nx + dvx * 0.5f;
    mz = nz + dvz * 0.5f;
    cross = dvz * nx - nz * dvx;
    len = sqrtf(nz * nz + nx * nx);
    if (len < 1.0f) {
        len = 1.0f;
    }
    inv = 1.0f / len;
    nx *= inv;
    nz *= inv;
    t = sqrtf(mz * mz + mx * mx);
    inv = (0.01f < t) ? 1.0f / t : 1.0f;
    dot = nz * (mz * inv) + nx * (mx * inv);
    if (dot < 0.0f) {
        dot = -dot;
    }
    t = 2.0f * (len * (1.0f - dot));       /* glancing blow -> more spin */
    sign = (cross < 0.0f) ? -15.0f : 15.0f;
    m = car->mass;
    if (2.0f <= m) {
        m = 10.0f;
    }
    if (m < 1.0f) {
        m = 1.0f;
    }
    spin = (sign * t) / m;
    if (car->spinTimer == 1) {
        carCrashSpinMore(car, spin);
        return;
    }
    if (car->unk7D <= gNumPlayersM1) {
        /* a player's car */
        behind = carCompareAhead(car, other) < 0;
        t = angleWrap(car->road.heading - car->travelDir);
        if (t < 0.0f) {
            t = -t;
        }
        spinBack = FALSE;
        ang = 0.0f;
        if (1.75f <= t) {
            /* knocked into going the wrong way: spin back round if the nose
             * still points roughly along the road */
            ang = angleWrap(car->heading - car->road.heading);
            t = (ang < 0.0f) ? -ang : ang;
            if (t <= 1.4f) {
                spinBack = TRUE;
            }
        }
        if (impact <= 50.0f) {
            goto react;
        }
        if (impact <= 100.0f) {
            if (randInt(2) != 0 && spinBack) {
                goto react;
            }
            if (randInt(6) == 0) {
                if (behind) {
                    spinBack = FALSE;
                    goto react;
                }
                carCrashSpinMore(car, spin);
                return;
            }
            spinBack = FALSE;
            goto react;
        }
        if (randInt(3) == 0) {
            if (behind) {
                spinBack = FALSE;
                goto react;
            }
            carCrashSpinMore(car, spin);
            return;
        }
        goto react;
    }
    /* a computer car: hard enough hits spin it out */
    spinBack = FALSE;
    a = (spin < 0.0f) ? -spin : spin;
    if (0.1f <= a && 30.0f <= impact
        && (140.0f < impact || randFloat() < ((impact - 30.0f) * 6.0f + 160.0f) * 0.001f)) {
        a = (spin < 0.0f) ? -spin : spin;
        if (10.0f < a) {
            a = 10.0f;
        }
        rate = randFloat() * 0.06f + (a * 0.008f + 0.04f);
        if (spin < 0.0f) {
            rate = -rate;
        }
        amount = randFloat() * 6.2831855f + PI_F;
        if (2.0f <= car->mass) {
            amount = PI_F;
            rate *= 0.5f;
        }
        if (car->spinTimer != 0) {
            /* original quirk: the spin amount becomes half the yaw rate */
            amount = car->yawRate * 0.5f;
            rate = (rate + amount) * 0.67f;
        }
        carStartSpin(car, 0, rate, amount);
        if (car->orient[1][1] < 0.0f) {
            car->yawRate = -car->yawRate;   /* upside down */
        }
        return;
    }

react:
    if (spinBack) {
        if (car->speed < 20.0f) {
            car->speed = 20.0f;
        }
        steps = (s32)(car->speed * 1.5f);
        if (steps >= 61) {
            steps = 60;
        }
        if (steps <= 0) {
            steps = 1;
        }
        carStartSpin(car, steps, (-6.0f * ang) / (f32)steps, 0.0f);
        if (car->orient[1][1] < 0.0f) {
            car->yawRate = -car->yawRate;
        }
    } else {
        /* small yaw kick; t is the glance factor (computer cars) or the last
         * angle measured above (players) - one variable in the original */
        if (spin < -0.05f) {
            spin = -0.05f;
        } else {
            spin = (0.05f < spin) ? 0.05f : spin;
        }
        if (car->spinTimer != 0) {
            t = car->yawRate * 0.5f;
            spin = (spin + t) * 0.67f;
        }
        if (80.0f < t) {
            t = 80.0f;
        }
        spin = (f32)(spin * (t * 0.0125));
        k = impact * 0.4f;
        if (35.0f < k) {
            k = 35.0f;
        }
        k *= randFloat();
        if (car->unk7D <= gNumPlayersM1) {
            k *= 0.7f;
        }
        if (k < 8.0f) {
            k = 8.0f;
        }
        if (other->unk7D <= gNumPlayersM1) {
            k += 5.0f;
        }
        if (!behind) {
            k *= 0.5f;
            if (7.0f < k) {
                k = 7.0f;
            }
        }
        if (car->orient[1][1] < 0.0f) {
            spin = -spin;
        }
        carStartSpin(car, (s32)k, spin, 0.0f);
    }
}

/* 0x80029AB8 ovl2 - a crash while already spinning (or a big one): spin harder */
void carCrashSpinMore(Car *car, f32 spin) {
    f32 a;
    f32 rate;
    f32 min;
    f32 amount;

    a = (spin < 0.0f) ? -spin : spin;
    if (10.0f < a) {
        a = 10.0f;
    }
    rate = (f32)(a * 0.008 + 0.1f);
    if (spin < 0.0f) {
        rate = -rate;
    }
    if (car->spinTimer != 0) {
        rate = (car->yawRate * 0.5f + rate) * 0.67f;
    }
    min = (car->unk7D <= gNumPlayersM1) ? 0.08f : 0.02f;
    if (((rate < 0.0f) ? -rate : rate) < min) {
        rate = (rate <= 0.0f) ? -min : min;
    }
    amount = (car->spinTimer != 1) ? PI_F : 0.0f;
    if (car->orient[1][1] < 0.0f) {
        rate = -rate;
    }
    carStartSpin(car, 0, rate, amount);
}

/* 0x80029C24 ovl2 - crash sound, louder for harder hits */
void carCrashSound(Car *a, Car *b, f32 impact) {
    s32 *table;
    s32 count;
    f32 v;

    impact *= 0.5f;
    count = 4;
    v = impact * 0.007f;
    if (1.0f < v) {
        v = 1.0f;
    }
    if (b->orient[1][1] < 0.0f) {
        table = gCrashSfxFlipped;
    } else {
        table = gCrashSfx;
        count = 5;
        if (220.0f <= impact) {
            count = 3;
        }
    }
    carPlayRandomSfx(a, table, count, v * 0.5f + 0.5f);
    if (a->unk84 != 0) {
        a->muted = 10;
    }
}

/* 0x80029CEC ovl2 - a player hit a car very hard: it is launched into the air
 * (tumbling), the player loses speed and maybe flips or spins too */
void trafficLaunch(Car *player, Car *traffic) {
    VehicleState *vs;
    s32 *table;
    s32 count;
    f32 v;
    f32 s;

    traffic->travelDir = randFloat() * 0.2f + player->travelDir - 0.1f;
    v = (randFloat() * 0.45f + 0.8f) * player->speed * 0.75f;
    if (traffic->mass < 1.0f) {
        traffic->mass = 1.0f;
    }
    traffic->speed = v / traffic->mass;
    v = (randFloat() * 0.3f + 0.2f) * 1.5f * player->speed;
    if (65.0f < v) {
        v = 65.0f;
    }
    traffic->vel[1] = v;
    traffic->flipping = 1;
    traffic->unk40 = traffic->probes[0].groundY;
    s = player->speedScale;
    player->speed = player->speed * s * s * 0.5f;
    v = player->speed * 1.25f;
    if (traffic->speed < v) {
        traffic->speed = v;
    }
    vs = &gVehicleStates[traffic->unk7D];
    vs->crashPitch = 0.0f;
    vs->crashYaw = traffic->heading;
    vs->crashPitchRate = randFloat() * 0.4f - 0.2f;
    v = randFloat() * 0.2f - 0.1f;
    vs->crashYawRate = v;
    traffic->yawRate = v;
    vs->roadSide = player->unk7D;
    if (player->flipping == 0 && carCrashImmune(player) == 0 && randFloat() < 0.4f) {
        racerStartFlip(player->unk7D);
        table = gLaunchFlipSfx;
        count = 4;
    } else {
        if (gRacerHud[player->unk7D].cameraFollowsCar && randFloat() < 0.25f) {
            player->travelDir = player->travelDir + PI_F;
            carStartSpin(player, 15, 0.0f, 0.0f);
        } else {
            v = randFloat() * 0.04f + 0.08f;
            if (randInt(2)) {
                v = -v;
            }
            carStartSpin(player, 0, v, PI_F);
        }
        table = gLaunchSfx;
        count = 8;
    }
    if (traffic->model == 7) {
        carPlaySfx(player, 0x61, 1.0f);
    } else if (traffic->model == 13) {
        carPlaySfx(player, 0x44, 1.0f);
    } else {
        carPlayRandomSfx(player, table, count, 1.0f);
    }
}

/* 0x8002A05C ovl2 - always FALSE (a disabled cheat / debug hook?) */
s32 carCrashImmune(Car *car) {
    return FALSE;
}

/* 0x8002A06C ovl2 - is (x, z) further from the route than the road's
 * half-width there? half-width = zone value * 100 + 576 * lanes, taken on the
 * side of the route the point is on and interpolated along the segment. */
s32 probeOutsideRoad(f32 x, f32 z, s32 vehicle) {
    RoadPos rp;
    PathPoint *pt;
    s32 oncoming;
    s32 side;
    s32 i0;
    s32 i1;
    f32 w0;
    f32 w1;
    f32 w;
    f32 dx;
    f32 dz;

    oncoming = vehicle >= gNumPlayers;
    if (oncoming) {
        oncoming = gCars[vehicle].unk84 == 3;
    }
    rp.point = (oncoming ? 1 : -1) + gCars[vehicle].road.point;
    if (rp.point <= 0) {
        return FALSE;
    }
    roadLocate(&rp, x, z, oncoming);
    side = roadIsLeft(&rp);
    if (rp.point <= 0) {
        i0 = 0;
    } else if (rp.point >= gPathLength) {
        i0 = gPathLength - 1;
    } else {
        i0 = rp.point;
    }
    /* original bug: i1 is left uninitialised at the first and last route point */
    if (rp.point > 0 && rp.point < gPathLength - 1) {
        i1 = (oncoming ? -1 : 1) + i0;
    }
    if (i1 <= 0) {
        i1 = 0;
    } else if (i1 >= gPathLength) {
        i1 = gPathLength - 1;
    }
    pt = &gPathPoints[i0];
    w0 = (f32)((s8 *)&pt->zoneA)[side] * 100.0f + 576.0f * (f32)(u32)pt->lanes;
    pt = &gPathPoints[i1];
    w1 = (f32)((s8 *)&pt->zoneA)[side] * 100.0f + 576.0f * (f32)(u32)pt->lanes;
    if (rp.t < 0.0f) {
        w = w0;
    } else if (rp.t > 1.0f) {
        w = w1;
    } else {
        w = (w1 - w0) * rp.t + w0;
    }
    dx = rp.pos[0] - x;
    dz = rp.pos[2] - z;
    return (w * w < dx * dx + dz * dz);
}
