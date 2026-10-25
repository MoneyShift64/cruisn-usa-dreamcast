#ifndef GROUND_H
#define GROUND_H
/*
 * Ground contact and collisions (race overlay, src/race/ground.c).
 * Needs common.h, race.h, road.h and scenery.h first. Format notes: docs/ground.md.
 *
 * Three coordinate systems meet here:
 *   world   - cars, probes (CarProbe.x/y/z), RoadPos.pos
 *   route   - world / 5 with z negated (x_r = x_w * 0.2, z_r = z_w * -0.2);
 *             DrawObj placement (xf, y), MeshCache.radius2
 *   mesh    - s16 vertices of a mesh file, in world-sized units; the collision
 *             code scales them by 0.2 and places them with the object's Xform2
 */

/* A ground polygon that was hit (0x10 bytes). The first 0x10 bytes of a
 * CarProbe have this layout too (the probe's last hit). */
typedef struct GroundHit {
    /* 0x00 */ DrawObj   *obj;               /* scenery object the polygon belongs to */
    /* 0x04 */ ModelItem *item;              /* obj->item when the hit was made; a different value means
                                                the pooled DrawObj has been recycled -> stale */
    /* 0x08 */ s32        surface;           /* CarProbe.offroad value it gave (SURF_*) */
    /* 0x0C */ u8        *face;              /* the face record inside the mesh file */
} GroundHit;

/* CarProbe.offroad */
#define SURF_ROAD      0   /* list A polygon of a route-marker item (kind 0xB00): the road itself */
#define SURF_GROUND    1   /* other list A polygon (kind 0x300, e.g. 0x310): drivable verge / shoulder */
#define SURF_EDGE      2   /* list B polygon (terrain, buildings...): counts as touching the edge */
#define SURF_NONE      3   /* no polygon under the probe: barrier hit */
#define SURF_OUTSIDE   4   /* further from the route than the road's allowed half-width: barrier hit */

/* One vertex of a mesh file (6 bytes), world-sized units */
typedef struct MeshVert {
    /* 0x00 */ s16 x;
    /* 0x02 */ s16 y;
    /* 0x04 */ s16 z;
} MeshVert;

/* Header of a mesh file (the file a MeshCache entry was compiled from:
 * DrawObj.mesh->key). Layout after the header:
 *   MeshVert verts[lastVert + 1];
 *   u16      uv[lastUV + 1][2];               texture coordinates
 *   6-byte records, numRecords of them:       group headers and faces
 * A group header holds a u32 (texture / material) kept 4-byte aligned and a
 * u8 face count: at byte 4 if the record starts on a multiple of 4 from the
 * file start, else at byte 0. It is followed by `count` faces:
 * u8 v0, v1, v2 (vertex indices), u8 uv0, uv1, uv2. */
typedef struct GroundMesh {
    /* 0x00 */ u8  lastVert;                 /* number of vertices - 1 */
    /* 0x01 */ u8  lastUV;                   /* number of uv pairs - 1 */
    /* 0x02 */ s16 numRecords;               /* group headers + faces */
    /* 0x04 */ u16 unk04;
    /* 0x06 */ u16 unk06;
    /* 0x08 */ MeshVert verts[1];            /* [lastVert + 1] */
} GroundMesh;

/* A point / triangle corner passed by value to the triangle helpers */
typedef struct Vec3s {
    f32 x;
    f32 y;                                   /* only used by triHeightAt's triangle */
    f32 z;
} Vec3s;

/* Collision box size of a car model, world units (car space: x right, y up, z forward) */
typedef struct CarBox {
    f32 halfWidth, front, back, top, bottom;
} CarBox;

/* Car model table entry (0xF8 bytes, 0x80043A50 ovl2, indexed by Car.model).
 * race.c / ai.c call the same table gCarAnim (u8[]). The [4] arrays are per
 * variant (PlayerSetup.unk008 for the players, 0 for everyone else). */
typedef struct CarModel {
    /* 0x00 */ f32   enginePower;            /* -> Car.enginePower */
    /* 0x04 */ f32   grip[4];                /* -> Car.grip */
    /* 0x14 */ f32   dragRoad[4];            /* -> Car.dragRoad */
    /* 0x24 */ f32   dragOffroad[4];         /* -> Car.dragOffroad */
    /* 0x34 */ f32   mass[4];                /* -> Car.mass */
    /* 0x44 */ Vec3f probes[5];              /* road-contact probes, car space (world units): 0 = body, 1-4 = wheels */
    /* 0x80 */ CarBox box;                   /* corners[] and bound are built from this by carsInit */
    /* 0x94 */ Vec3f corners[8];             /* collision box, car space; 0,1,4,5 = front, 2,3,6,7 = back */
    /* 0xF4 */ f32   bound;                  /* -> Car.bound */
} CarModel;

/* The part of a Car from 0x1B8 on: everything the box collisions use.
 * Fake bodies (a hit post, a mirrored car) are built on the stack with only pos.x/z and vel.x/z set. */
typedef struct CarBody {
    /* 0x00 */ Vec3f pos;                    /* Car.pos (0x1B8) */
    /* 0x0C */ Mtx3  orient;
    /* 0x30 */ Vec3f vel;
    /* 0x3C */ f32   bound;
    /* 0x40 */ Vec3f corners[8];
} CarBody;
#define CAR_BODY(car) ((CarBody *)(car)->pos)

/* The one scenery object that is currently being knocked over (0x80046878 ovl2, 0x18 bytes) */
typedef struct FallingObj {
    /* 0x00 */ DrawObj *obj;                 /* NULL: none */
    /* 0x04 */ f32 angle;                    /* tilt (radians) */
    /* 0x08 */ f32 rate;                     /* added to angle every racing frame */
    /* 0x0C */ s32 bouncing;                 /* 1: springs back and forth (kind 0x10, variant 5), else falls flat */
    /* 0x10 */ s32 timer;                    /* bouncing: frames until the swing reverses */
    /* 0x14 */ s32 pad14;
} FallingObj;

/* Death Valley moving obstacles (7 x 12 bytes, 0x80065660 ovl2) */
typedef struct DvObject {
    /* 0x00 */ Car *car;                     /* a Car slot used for the obstacle's body */
    /* 0x04 */ u32 *dl;                      /* its display list (from the scenery mesh) */
    /* 0x08 */ s32 active;
} DvObject;

extern CarProbe   gProbeTmp;             /* 0x80045FA0 ovl2 - scratch probe for carGroundDelta */
extern GroundHit  gGroundHit;            /* 0x80045FD8 ovl2 - result of the last list search */
extern s32       *gFallingMtx;           /* 0x80045FE8 ovl2 - this frame's matrix for the falling object */
extern GroundHit  gGroundCache[8];       /* 0x80045FF0 ovl2 - polygons hit recently (this car's probes) */
extern s32        gGroundCacheCount;     /* 0x80046070 ovl2 */
extern f32        gMeshXZ[256][2];       /* 0x80046078 ovl2 - transformed vertices (route x, z) */
extern FallingObj gFallingObj;           /* 0x80046878 ovl2 */
extern DvObject   gDvObjects[7];         /* 0x80065660 ovl2 */
extern CarModel   gCarModels[];          /* 0x80043A50 ovl2 */

/* ground.c */
void raceObjectsInit(void);
void carGroundUpdate(Car *car, s32 side);
void raceObjectsUpdate(void);
void carsCollidePlayers(void);
void probeFindGround(CarProbe *p, s32 vehicle, s32 side);
void groundCacheClear(void);
void groundCacheAdd(GroundHit *hit);
void carModelPose(Vec3f *local, CarBody *body);
s32  carGroundDelta(Car *car, f32 *out);
void carProbesFindGround(Car *car, s32 side);
void carAlignToGround(Car *car);
s32  groundCacheTestFace(CarProbe *p, GroundHit *hit);
s32  groundCacheTestObj(CarProbe *p, GroundHit *hit);
s32  groundSearchList(CarProbe *p, DrawObj *obj, s32 side);
s32  groundSearchOther(CarProbe *p, DrawObj *obj, s32 side);
s32  meshFindGround(f32 mx, f32 mz, CarProbe *p, MeshCache *mc, DrawObj *obj);
s32  faceFindGround(f32 mx, f32 mz, CarProbe *p, u8 *face, MeshCache *mc, DrawObj *obj);
void carHitObjects(s32 vehicle, s32 side);
void fallingObjectStop(s32 unused);
void carHitObject(DrawObj *obj, Car *car, s32 side);
void carCollide(s32 a, s32 b, s32 gentle);
void carCollideDvObject(s32 vehicle, s32 index);
void boxFaceNormals(Vec3f *box, Vec3f *normals);
s32  boxContainsPoint(Vec3f *box, Vec3f *normals, Vec3f p);
s32  boxHitsBox(Vec3f *box, Vec3f *pts);
s32  boxesOverlap(Vec3f *a, Vec3f *b);
void carBounceOff(Car *car, CarBody *other, s32 forceSpin);
f32  bodyRelative(CarBody *a, CarBody *b, Vec3f dir);
void carCrash(Car *a, Car *b, f32 strength);
void carCrashSpin(Car *other, Car *car, Vec3f dir, f32 dvx, f32 dvz);
void carCrashSpinMore(Car *car, f32 spin);
void carCrashSound(Car *a, Car *b, f32 impact);
void trafficLaunch(Car *player, Car *traffic);
s32  carCrashImmune(Car *car);
s32  probeOutsideRoad(f32 x, f32 z, s32 vehicle);

#endif
