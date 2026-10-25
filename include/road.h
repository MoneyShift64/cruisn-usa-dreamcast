#ifndef ROAD_H
#define ROAD_H
/*
 * Track and route data (see NOTES.md, "Road data").
 *
 * A track is a chain of road SECTIONS. Each section has a model (its scenery
 * and road surface) and a placement step: where the next section starts and
 * how much the road turns. At race start routeBuild() walks the chain and
 * collects the "route markers" from every section model into one long list
 * of PATH POINTS in world space - the route every car follows.
 *
 * World units: path points are stored at 1/5 scale with Z negated
 * (world x = point.x * 5, world z = point.z * -5).
 */

/*
 * On-ROM layout (see docs/scenery.md). A track is file "track header" (TrackHeader)
 * that names two more files: the model table and the section table. Before a
 * file is loaded the pointer fields hold 16-bit FILE IDS (index into the asset
 * table at ROM 0x80470); the loader replaces them with RAM pointers and sets
 * bit 31 of the node's flags.
 */

/* An item inside a section model (0x14 bytes): a placed instance of one of the
 * model's parts, or a route marker. Kind = flags & 0xF00:
 *   0xB00 route marker (path point), 0x300 list A, 0x400 list C (0x247x: animated
 *   scenery), 0x600/0x700 list D (props, drawn back-to-front with the late pass),
 *   anything else list B (road surface / ground). See docs/scenery.md. */
typedef struct ModelItem {
    /* 0x00 */ u8  drawFlags;                 /* copied to DrawObj.drawFlags (SCN_* flags) */
    /* 0x01 */ u8  part;                      /* index into SectionModel.parts, or animated-object id if drawFlags & 1 */
    /* 0x02 */ u16 flags;                     /* kind (0xF00), subtype (0xF0), variant (0xF); 0x8000 = billboard (turns to face the camera) */
    /* 0x04 */ s32 x;                         /* 16.16 fixed point, section-local */
    /* 0x08 */ s32 y;
    /* 0x0C */ s32 z;
    /* 0x10 */ s32 angle;                     /* rotation about Y (binary angle, 0x10000 = 360 degrees) */
} ModelItem;

struct MeshCache;

/* A node of a model tree (12 bytes). Same layout for TrackHeader.models and
 * SectionModel.parts. */
typedef struct ModelPart {
    /* 0x00 */ void *data;                    /* file id before loading; then a MeshFile* (or a SectionModel* if flags & 1) */
    /* 0x04 */ u32  flags;                    /* bit 0: data is a sub-group (SectionModel); bit 31: loaded */
    /* 0x08 */ struct MeshCache *mesh;        /* compiled display list while the section is loaded (parts only) */
} ModelPart;

/* A section model / any "group" file (0x14 bytes) */
typedef struct SectionModel {
    /* 0x00 */ ModelPart *parts;
    /* 0x04 */ s32 numParts;
    /* 0x08 */ ModelItem *items;
    /* 0x0C */ s32 numItems;
    /* 0x10 */ s32 unk10;                     /* always 0 in the ROM */
} SectionModel;

typedef struct ModelRef {                /* 12 bytes, same layout as ModelPart */
    /* 0x00 */ SectionModel *model;
    /* 0x04 */ u32 flags;                     /* bit 0: group (always set for section models); bit 31: loaded */
    /* 0x08 */ u32 unk08;
} ModelRef;

/* Per-section placement (0x14 bytes) */
typedef struct SectionInfo {
    /* 0x00 */ u8  unk00;
    /* 0x01 */ u8  model;                     /* index into TrackHeader.models */
    /* 0x02 */ u16 flags;                     /* & 0x3F: script/event id (0x20+track = finish line); 0x40: 4-lane road */
    /* 0x04 */ s32 dx;                        /* offset to the next section's origin (x5 world units) */
    /* 0x08 */ s32 dy;
    /* 0x0C */ s32 dz;
    /* 0x10 */ s32 turn;                      /* heading change to the next section (binary angle) */
} SectionInfo;

typedef struct TrackHeader {             /* *0x8013F9B4 (file 0x9B8 + 3 * track) */
    /* 0x00 */ ModelRef *models;
    /* 0x04 */ s32 numModels;
    /* 0x08 */ SectionInfo *sections;
    /* 0x0C */ s32 numSections;
    /* 0x10 */ s32 unk10;
} TrackHeader;
extern TrackHeader *gTrack;              /* 0x8013F9B4 */

/* One point of the route (0x14 bytes, 0x8015BF20) */
typedef struct PathPoint {
    /* 0x00 */ f32 x;
    /* 0x04 */ f32 y;
    /* 0x08 */ f32 z;
    /* 0x0C */ s16 index;                     /* marker number inside its section model */
    /* 0x0E */ u8  section;
    /* 0x0F */ u8  lanes;                     /* lanes each way: 2, or 4 on wide roads */
    /* 0x10 */ u8  zoneA;                     /* extra road half-width, in 100s of units (signed), on the */
    /* 0x11 */ u8  zoneB;                     /*   roadIsLeft()==0 / ==1 side; half-width = 576 * lanes + zone * 100 */
    /* 0x12 */ u8  pad12[2];
} PathPoint;
extern PathPoint gPathPoints[];          /* 0x8015BF20 */
extern s32 gPathLength;                  /* 0x80162578 - number of path points */

/* Stretches of route with their own zoneA/zoneB values (per track, 0x80135530) */
typedef struct RouteZone {
    /* 0x00 */ s16 section;                   /* starts at this section... */
    /* 0x02 */ s16 point;                     /* ...and this marker inside it */
    /* 0x04 */ s16 a;                         /* -> PathPoint.zoneA / zoneB: extra half-width each side */
    /* 0x06 */ s16 b;
} RouteZone;
extern RouteZone *gRouteZones[16];       /* 0x80135530 */

/* Placed section (0x2C bytes, 0x801434D0), built by sectionsPlace() */
typedef struct RoadSection {
    /* 0x00 */ f32 xf[6];                     /* Xform2: section-local -> route units (xf[2], xf[5] = origin x, z) */
    /* 0x18 */ f32 y;                         /* origin height */
    /* 0x1C */ f32 scaleXZ;                   /* model scale (per-track table 0x80131E94) */
    /* 0x20 */ f32 scaleY;
    /* 0x24 */ s32 flags;                     /* bit 0/1: loaded for player 1/2 */
    /* 0x28 */ s32 length;                    /* distance to the next section's origin (route units, 0 = none) */
} RoadSection;
extern RoadSection gRoadSections[];      /* 0x801434D0 */

/* Window of sections loaded around each player (0x28 bytes, 0x8013F9B8) */
typedef struct SectionWindow {
    /* 0x00 */ s32 first;
    /* 0x04 */ s32 count;
    /* 0x08 */ f32 span;                      /* running sum of loaded section lengths (never read back) */
    /* 0x0C */ RoadPos road;                  /* camera position on the route (roadLocate); road.point = search start for roadCurrentPoint */
} SectionWindow;
extern SectionWindow gSectionWindows[2]; /* 0x8013F9B8 */

/* 2D affine transform used for section placement: x' = m0*x + m1*z + m2, z' = m3*x + m4*z + m5 */
typedef f32 Xform2[6];

Xform2 *xf2Identity(Xform2 *m);
Xform2 *xf2Rotate(Xform2 *m, s32 angle, f32 tx, f32 tz);
Xform2 *xf2Mul(Xform2 *out, Xform2 *a, Xform2 *b);
Xform2 *xf2RotateBy(Xform2 *m, s32 angle, f32 tx, f32 tz);
void    xf2Apply(Xform2 *m, f32 *x, f32 *z);

void routeBuild(s32 numSections);
void roadLocate(RoadPos *rp, f32 x, f32 z, s32 reverse);
s32  roadIsLeft(RoadPos *rp);
s32  roadCurrentPoint(s32 player);
f32  roadHeadingAt(s32 point);
f32  segmentParam(f32 px, f32 pz, f32 ax, f32 az, f32 bx, f32 bz);
void routeSetZones(void);

#endif
