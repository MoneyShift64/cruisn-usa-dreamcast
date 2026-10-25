#ifndef SCENERY_H
#define SCENERY_H
/*
 * Track scenery: streaming of road sections around each player, the per-player
 * draw lists of scenery objects, and the code that turns them into display-list
 * commands. Formats: docs/scenery.md. Needs common.h, race.h and road.h first.
 *
 * Coordinates: scenery uses ROUTE units (= world / 5, z negated), the same as
 * gPathPoints, RoadSection and RaceCamera position (RaceCamera.posX/posY/posZ
 * are really the camera POSITION x/y/z in route units).
 */

/* DrawObj.drawFlags / ModelItem.drawFlags */
#define SCN_ANIMATED   0x01   /* drawn by animSceneryDraw (DrawObj.special = animated-object type) */
#define SCN_MTX_CAMERA 0x02   /* extra matrix: gSceneryCamMtx (also 0x40) */
#define SCN_MTX_UPRIGHT 0x04  /* extra matrix: constant 90-degree X rotation (0x80133790); never deferred */
#define SCN_FAR_LATE   0x08   /* with SCN_LATE: deferred up to 20000 units instead of 4000 */
#define SCN_MOVER      0x10   /* moving object: matrix from gMovers[special] (trains etc.) */
#define SCN_MOVER_B    0x20   /* clears gMoverActive when freed */
#define SCN_MTX_CAMERA2 0x40
#define SCN_LATE       0x80   /* list D props: drawn in a second, translucent pass when near */

/* A scenery object instance (0x38 bytes, pool of 2000 at 0x8004A0E0 ovl2).
 * Objects are shared between the two players: each player has its own link
 * (next[player]) and its own distance (dist2[player]). */
typedef struct DrawObj DrawObj;
struct DrawObj {
    /* 0x00 */ f32 xf[6];                     /* Xform2 placement: xf[2] = x, xf[5] = z (route units) */
    /* 0x18 */ f32 y;
    /* 0x1C */ f32 dist2[2];                  /* squared XZ distance to each player's camera */
    /* 0x24 */ ModelItem *item;
    /* 0x28 */ struct MeshCache *mesh;        /* compiled display list (NULL for animated objects) */
    /* 0x2C */ DrawObj *next[2];              /* per-player draw-list link */
    /* 0x34 */ u8  section;                   /* road section the object belongs to */
    /* 0x35 */ u8  special;                   /* animated/mover index; bit 7 = deferred to the late pass */
    /* 0x36 */ u8  drawFlags;                 /* SCN_* */
    /* 0x37 */ u8  pad37;
};

/* A draw list: one singly linked list per player (0x10 bytes) */
typedef struct DrawList {
    /* 0x00 */ DrawObj *head[2];
    /* 0x08 */ s32 count[2];
} DrawList;

/* Free pool of DrawObjs (0x8004A0D0 ovl2): head[0] = free list, count[0] = free count */
extern DrawList gDrawPool;               /* 0x8004A0D0 ovl2 */
extern DrawObj  gDrawObjs[2000];         /* 0x8004A0E0 ovl2 */

extern DrawList gDrawListA;              /* 0x8004A080 ovl2 - items of kind 0x300 */
extern DrawList gDrawListB;              /* 0x8004A090 ovl2 - default kinds: road surface, ground, buildings */
extern DrawList gDrawListC;              /* 0x8004A0A0 ovl2 - kind 0x400 */
extern DrawList gDrawListD;              /* 0x8004A0B0 ovl2 - kinds 0x600/0x700: props (trees, signs...) */
extern DrawList gDrawListE;              /* 0x8004A0C0 ovl2 - added by other code (sky / backdrop objects) */

/* Compiled-mesh cache entry (0x1C bytes, 512 at 0x801582F0) */
typedef struct MeshCache {
    /* 0x00 */ void *key;                     /* the MeshFile it was compiled from */
    /* 0x04 */ u32  *dl;                      /* display list (in the 256-byte page pool) */
    /* 0x08 */ u8   *vtx;                     /* vertices (in the 256-byte page pool) */
    /* 0x0C */ u32  lastFrame;                /* gFrameCounter when last drawn (pages are recycled after) */
    /* 0x10 */ f32  radius2;                  /* max (x^2 + z^2) of the vertices, route units^2, + 1 */
    /* 0x14 */ u8   refs;
    /* 0x15 */ u8   pad15[3];
    /* 0x18 */ struct MeshCache *next;
} MeshCache;

typedef struct MeshCacheList {
    /* 0x00 */ MeshCache *head;
    /* 0x04 */ s32 count;
} MeshCacheList;

/* Per-player distance fade of far scenery (main segment, 0x80146120..) */
extern f32 gFadeFar2[2];                 /* 0x80146120 - squared distance where objects are fully faded */
extern f32 gFadeNear2[2];                /* 0x80146128 - squared distance where fading starts */
extern f32 gFadeInvRange[2];             /* 0x80146130 - 1 / (fade range)^2 */
extern s32 gFadeEnabled[2];              /* 0x80146138 */
extern s32 gFadeToWhite[2];              /* 0x80146140 - 1: fade colour = grey level (fog), 0: alpha */
extern s32 gFadeUnk148[2];               /* 0x80146148 */

/* streaming (race/scenery.c) */
void sectionWindowUpdate(s32 player);
void sectionLoad(s32 player, s32 section);
void sectionUnload(s32 player, s32 section);
void drawListShare(DrawList *list, s32 player, s32 section);
void drawListSort(DrawList *list, s32 player, f32 x, f32 z);
void drawListMeasure(DrawList *list, s32 player, f32 x, f32 z);
void sceneryPoolInit(void);
DrawObj *drawListAdd(DrawList *list, s32 player, s32 section, ModelItem *item, ModelPart *part);
s32  drawListRemove(DrawList *list, s32 player, s32 section);
void drawObjSetSection(DrawObj *obj, s32 section);
DrawObj *drawObjAlloc(void);
void drawObjFree(DrawObj *obj);
void drawListUnlink(DrawList *list, s32 player, DrawObj *obj);
void drawListClear(DrawList *list);
void sceneryPoolFull(void);

/* drawing (race/scenery.c) */
void sceneryDraw(s32 player, f32 camX, f32 camY, f32 camZ);
void sceneryFadeColor(s32 flags, s32 player);
void raceDrawWorld(s32 player);
void sceneryDrawObj(DrawObj *obj, s32 player, f32 *xf, f32 tx, f32 ty, f32 tz, s32 setMode);
void moverMatrix(s32 *m, s32 mover, f32 tx, f32 ty, f32 tz);

/* main segment (scenery_main.c) */
void sectionsPlace(s32 extraHeap);
void sectionModelLoad(s32 section);
void sectionModelRelease(s32 section);
s32  modelPartsLoad(SectionModel *group);
s32  modelPartsRelease(SectionModel *group);
MeshCache *meshCacheGet(void *meshFile);
s32  meshCacheRelease(void *meshFile);
s32  fadeAlpha(s32 player, f32 dist2, s32 prev);
void fadeEnd(s32 player, s32 alpha);
void fadeSetRange(s32 player, f32 dist);
void fadeInit(void);

#endif
