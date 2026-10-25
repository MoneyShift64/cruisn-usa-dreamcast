/*
 * track.c - loading a track: its header group (section models), placing the
 * sections in the world, loading section models around the players, the
 * per-track texture swaps, the vehicles' render objects at the start, and the
 * per-frame view set-up (backdrop models and vehicles).
 * Original code: main segment, ROM 0x1340-0x22C0 (RAM 0x80100340-0x801012C0).
 */
#include "common.h"
#include "race.h"
#include "road.h"
#include "scenery.h"
#include "frontend.h"

/* Per track and player count (0x10 bytes, [2][14] at 0x80131F70) */
typedef struct TrackDef {
    /* 0x00 */ s16 header;                   /* file id of the track header group */
    /* 0x04 */ s32 firstSections;            /* sections whose models load at the start */
    /* 0x08 */ f32 drawDist;                 /* scenery draw distance (route units) */
    /* 0x0C */ s32 fadeOn;                   /* scenery fades out with distance */
} TrackDef;
extern TrackDef gTrackDefs[2][14];       /* 0x80131F70 */

/* Height steps: from section 'start' on, every section offset rises by dy (5 units) */
typedef struct HeightStep { s16 start, dy; } HeightStep;
extern HeightStep *gTrackHeights[14];    /* 0x80131A3C */
/* Model scale: from section 'start' on */
typedef struct ScaleStep { s16 start, xz, y; } ScaleStep;
extern ScaleStep *gTrackScales[14];      /* 0x80131E94 */
/* Backdrop draw order for some tracks (12 bytes, ends with track -1) */
typedef struct BackdropOrder { s32 track; s8 order[8]; } BackdropOrder;
extern BackdropOrder gBackdropOrders[];  /* 0x80131ECC */
/* Texture swaps: {file id, bytes} for a texture and its palette */
typedef struct TexSwap { s16 id, size; } TexSwap;
extern TexSwap gTexSwaps[][2];           /* 0x80131F08 */

/* What the renderer looks through (0x28 bytes, 0x8014EF90) */
typedef struct ViewerCam { s32 x, y, z; s32 rest[7]; } ViewerCam;
extern ViewerCam gViewerCams[2];         /* 0x8014EF90 */

extern u32  gHighHeapPtr;                /* 0x80146590 */
extern u32  gTrackHeapMark;              /* 0x801460DC - high heap before the track was loaded */
extern f32  gDrawDist;                   /* 0x801460D8 */
extern s32  gNumRenderObjs;              /* 0x801460E0 - players, plus AI racers if racing */
extern s32  gSectionsPlaced;             /* 0x801460E4 */
extern s32  gViewOffset[3];              /* 0x801460E8 - view origin (16.16), subtracted from the camera */
extern SectionWindow gSectionWindowInit; /* 0x801460F8 */
extern s32  gTrackFirstSections;         /* 0x801434C8 */
extern s32  gViewOrigin[3];              /* 0x8014EFF0 */
extern u8   gStartGridRaw[];             /* 0x80135570 - start grid, 10 x 0x90 bytes per track (see cars.c) */
extern u32 *gDlCursor;                   /* 0x80147684 */
extern s32 *gMtxCursor;                  /* 0x80147688 */

void trackEnvInit(s32 track);            /* 0x80101820 */
void fadeInit(void);                     /* 0x801012C0 */
u32  lowMemOverlayLoad(void);            /* 0x80102D74 */
u32  assetFindLoaded(s32 id);            /* 0x80000994 */
void lowMemOverlayRelease(void);         /* 0x80102E84 */
void memCopy32(void *dst, void *src, s32 words); /* 0x80103054 */
f32  sqrtf(f32 x);                       /* 0x80115720 */
void mtxTranslate(s32 *m, s32 x, s32 y, s32 z); /* 0x80114498 */
void modelDepthReset(void);              /* 0x80117080 */
TrackHeader *trackGroupLoad(s32 id, s32 numSections); /* 0x80117090 */
void trackGroupLoadRest(void);           /* 0x801170BC */
s32  modelPartsLoad(SectionModel *g);    /* 0x80117110 */
s32  modelPartsRelease(SectionModel *g); /* 0x801171AC */
void modelTreeDrawAt(SectionModel *g, s32 reverse, RoadSection *sec, f32 y); /* 0x80117664 */
void renderObjInit(void *obj, s32 a, s32 b, s32 c, s32 d, s32 e, s32 f, s32 g); /* 0x8011802C */
void renderObjDraw(void *obj);           /* 0x801180EC */
void meshCacheInit(s32 extraHeap);       /* 0x8011D3E0 - 1: also use the heap above the framebuffers */
Xform2 *xf2Rotate(Xform2 *m, s32 angle, f32 tx, f32 tz); /* 0x8011D15C */
void xf2Apply(Xform2 *m, f32 *x, f32 *z); /* 0x8011D388 */
void routeBuild(s32 numSections);        /* 0x8011E970 */

void trackStart(void);                   /* 0x801004D8 */
void sectionsPlace(s32 extraHeap);       /* 0x8010079C */
void sectionModelLoad(s32 section);      /* 0x80100AF0 */
s32  cameraDistance2(s32 player, s32 vehicle); /* 0x80100BC8 */
void backdropDraw(s32 player);           /* 0x80100CB8 */
void renderObjsInit(void);               /* 0x80100E74 */
void vehicleDrawFor(s32 vehicle, s32 player); /* 0x80100FE0 */
void trackTexSwaps(s32 track);           /* 0x80101048 */
void texSwap(s32 dst, s32 src);          /* 0x80101198 */

#define DL_PUSH(w0, w1) { u32 *_g = gDlCursor; gDlCursor = _g + 2; _g[0] = (w0); _g[1] = (w1); }

/* 0x80100340 - load a track: header group and the first sections' models,
 * texture swaps, then build the route */
void trackLoad(s32 track) {
    TrackDef *t;

    gTrackHeapMark = gHighHeapPtr;
    gSectionsPlaced = 0;
    modelDepthReset();
    trackEnvInit(track);
    t = &gTrackDefs[0][track];
    gTrackFirstSections = t->firstSections;
    lowMemOverlayLoad();
    gTrack = trackGroupLoad(t->header, gTrackFirstSections);
    trackTexSwaps(track);
    lowMemOverlayRelease();
    gDrawDist = gTrackDefs[gNumPlayersM1][track].drawDist;
    routeBuild(gTrackFirstSections);
    gCurrentTrack = track;
}

/* 0x8010041C - load the rest of the track's models (after the start) */
void trackReload(void) {
    gTrackFirstSections = 0xFF;
    lowMemOverlayLoad();
    trackGroupLoadRest();
    trackTexSwaps(gCurrentTrack);
    lowMemOverlayRelease();
    sectionsPlace(1);
    routeBuild(0);
    gViewOffset[0] = 0;
    gViewOffset[1] = 0;
    gViewOffset[2] = 0;
}

/* 0x80100488 */
void trackPrepare(void) {
    if (gRacersEnabled != 0) {
        gNumRenderObjs = 10;
    } else {
        gNumRenderObjs = gNumPlayersM1 + 1;
    }
    renderObjsInit();
}

/* 0x801004D8 - first time: place the sections, compile the first ones' models,
 * and mark them loaded for every player */
void trackStart(void) {
    s32 i, n;
    SectionWindow *w;

    if (gSectionsPlaced != 0) {
        return;
    }
    fadeInit();
    gDrawDist = gTrackDefs[gNumPlayersM1][gCurrentTrack].drawDist;
    sectionsPlace(0);
    for (i = 0; i < gTrackFirstSections; i++) {
        sectionModelLoad(i);
    }
    n = gTrackFirstSections;
    for (w = gSectionWindows; w <= &gSectionWindows[gNumPlayersM1]; w++) {
        w->count = n;
    }
}

/* 0x801005BC - draw the view of the track from (x, y, z): backdrop models, then
 * the vehicles (own car only if showOwn, the other player's only if showOther,
 * never the own car in the in-car view) */
void cameraApply(s32 player, s32 x, s32 y, s32 z, s32 showOwn, s32 showOther) {
    s32 *m;
    s32 i;

    gViewOffset[0] = x;
    gViewOffset[1] = y;
    gViewOffset[2] = z;
    trackStart();
    m = gMtxCursor;
    gMtxCursor = m + 16;
    mtxTranslate(m, x, y, z);
    DL_PUSH(0x01040040, (u32)(m - 16));
    backdropDraw(player);
    for (i = 0; i < gNumRenderObjs; i++) {
        if (i == player && showOwn == 0) {
            continue;
        }
        if (i == (player == 0) && gNumPlayersM1 != 0 && showOther == 0) {
            continue;
        }
        if (i == player && gPlayerSetup[i].viewAtEdge != 0) {
            continue;
        }
        vehicleDrawFor(i, player);
    }
    DL_PUSH(0xBD000000, 0);
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xB900031D, 0x504240);
    DL_PUSH(0xB6000000, 1);
}

/* 0x8010079C - lay the track's sections out in the world: each starts where the
 * previous one's offset (rotated by the accumulated turn) ends; heights get the
 * per-track steps; model scales from the per-track table */
void sectionsPlace(s32 extraHeap) {
    s32 i;
    SectionWindow *w;
    RoadSection *sec;
    SectionInfo *info;
    HeightStep *hs;
    ScaleStep *ss;
    s32 angle = 0;
    f32 y = 0.0f;
    f32 x, z, dy;

    meshCacheInit(extraHeap);
    for (w = gSectionWindows; w <= &gSectionWindows[gNumPlayersM1]; w++) {
        *w = gSectionWindowInit;
    }
    for (i = 0; i < gTrack->numSections; i++) {
        gRoadSections[i].flags = 0;
    }
    z = 0.0f;
    x = 0.0f;
    hs = gTrackHeights[gCurrentTrack];
    ss = gTrackScales[gCurrentTrack];
    info = gTrack->sections;
    sec = gRoadSections;
    for (i = 0; i < gTrack->numSections; i++, info++, sec++) {
        if (i >= hs[1].start) {
            hs++;
        }
        if (i >= ss[1].start) {
            ss++;
        }
        xf2Rotate((Xform2 *)sec->xf, -angle, x, z);
        sec->y = y;
        sec->scaleXZ = (f32)ss->xz * 1e-05f + 0.2f;
        sec->scaleY = (f32)ss->y * 1e-05f + 0.2f;
        x = (f32)info->dx * 0.2f;
        dy = (f32)info->dy * 0.2f;
        z = (f32)info->dz * 0.2f;
        if (x != 0.0f || z != 0.0f) {
            dy += 0.2f * (f32)hs->dy;
        }
        y += dy;
        sec->length = (s32)sqrtf(x * x + z * z);
        xf2Apply((Xform2 *)sec->xf, &x, &z);
        angle += info->turn;
    }
    gSectionsPlaced = 1;
}

/* 0x80100A84 - free the track's memory (back to before trackLoad) */
void trackUnload(void) {
    if (gTrackHeapMark != 0) {
        gHighHeapPtr = gTrackHeapMark;
        gTrackHeapMark = 0;
    }
}

/* 0x80100AAC - an empty loop over the sections (code left over; does nothing) */
void trackUnused(void) {
}

/* 0x80100AF0 - compile the meshes of a section's model */
void sectionModelLoad(s32 section) {
    if (section >= 0 && section < gTrack->numSections) {
        modelPartsLoad(gTrack->models[gTrack->sections[section].model].model);
    }
}

/* 0x80100B5C */
void sectionModelRelease(s32 section) {
    if (section >= 0 && section < gTrack->numSections) {
        modelPartsRelease(gTrack->models[gTrack->sections[section].model].model);
    }
}

/* 0x80100BC8 - x/z distance from a player's viewer to a vehicle (16.16, at
 * most 32766) - picks the model's level of detail */
s32 cameraDistance2(s32 player, s32 vehicle) {
    RenderObj *obj = &gRenderObjs[vehicle];
    ViewerCam *vc = &gViewerCams[player];
    f32 dx = (f32)(obj->x - vc->x + gViewOffset[0]) * (1.0f / 65536.0f);
    f32 dz = (f32)(obj->z - vc->z + gViewOffset[2]) * (1.0f / 65536.0f);
    f32 d = sqrtf(dx * dx + dz * dz);

    if (32766.0f <= d) {
        d = 32766.0f;
    }
    return (s32)(d * 65536.0f);
}

/* 0x80100CB8 - draw the backdrop: sections in the player's window whose model is
 * a group are drawn as whole trees (in the track's order, else last first) */
void backdropDraw(s32 player) {
    BackdropOrder *o;
    s8 *order = NULL;
    SectionWindow *w;
    ViewerCam *vc;
    ModelPart *part;
    s32 n, idx;

    for (o = gBackdropOrders; o->track >= 0; o++) {
        if (gCurrentTrack == o->track) {
            order = o->order;
            break;
        }
    }
    w = &gSectionWindows[player];
    vc = &gViewerCams[player];
    for (n = w->count; n-- != 0;) {
        if (order != NULL) {
            idx = *order++;
        } else {
            idx = n;
        }
        gViewOrigin[0] = vc->x - gViewOffset[0];
        gViewOrigin[1] = vc->y - gViewOffset[1];
        gViewOrigin[2] = vc->z - gViewOffset[2];
        part = (ModelPart *)&gTrack->models[gTrack->sections[w->first + idx].model];
        if (part->flags & 1) {
            /* (original bug: the section's placement is taken from gRoadSections[idx], not [first + idx]) */
            modelTreeDrawAt((SectionModel *)part->data, 1, &gRoadSections[idx], gRoadSections[idx].y);
        }
    }
}

/* 0x80100E74 - the vehicles' render objects at their grid positions (players
 * use their chosen car and colour) */
void renderObjsInit(void) {
    RenderObj *obj = gRenderObjs;
    s32 i;
    u8 *s;
    s32 model, colour;

    for (i = 0; i < gNumRenderObjs; i++, obj++) {
        s = (u8 *)gStartGridRaw + gCurrentTrack * 0x5A0 + i * 0x90;
        if (i <= gNumPlayersM1) {
            model = gPlayerSetup[i].car;
            colour = i;
        } else {
            colour = s[1];
            model = s[0];
        }
        renderObjInit(obj, (u8)model, (u8)colour, 1, cameraDistance2(0, i),
                      *(s32 *)(s + 4), *(s32 *)(s + 8), *(s32 *)(s + 0xC));
        obj->angleY = *(s32 *)(s + 0x14);
        obj->angleZ = *(s32 *)(s + 0x10);
        obj->angleX = *(s32 *)(s + 0x18);
    }
}

/* 0x80100FE0 */
void vehicleDrawFor(s32 vehicle, s32 player) {
    RenderObj *obj = &gRenderObjs[vehicle];

    obj->flags &= 0xE7FF;
    obj->camDist = cameraDistance2(player, vehicle);
    renderObjDraw(obj);
}

/* 0x80101048 - tracks 9, 11 and 12 use other versions of some textures */
void trackTexSwaps(s32 track) {
    switch (track) {
        case 9:
            texSwap(1, 6);
            texSwap(4, 3);
            texSwap(5, 3);
            texSwap(7, 8);
            texSwap(9, 8);
            texSwap(10, 8);
            texSwap(11, 8);
            break;
        case 11:
            texSwap(2, 12);
            texSwap(0, 3);
            texSwap(1, 6);
            texSwap(4, 3);
            texSwap(5, 3);
            texSwap(7, 8);
            texSwap(9, 8);
            texSwap(10, 8);
            texSwap(11, 8);
            break;
        case 12:
            texSwap(2, 12);
            texSwap(0, 3);
            texSwap(7, 8);
            texSwap(9, 8);
            texSwap(10, 8);
            texSwap(11, 8);
            break;
    }
}

/* 0x80101198 - copy texture + palette set 'src' over set 'dst' (if loaded) */
void texSwap(s32 dst, s32 src) {
    u8 *to[2];
    s32 size[2];
    TexSwap *e;
    s32 i;
    u32 a;

    for (i = 0, e = gTexSwaps[dst]; i < 2; i++, e++) {
        to[i] = NULL;
        if (e->id != 0) {
            a = (u32)assetFindLoaded(e->id);
            if (a != 0) {
                to[i] = (u8 *)(a + 0x80000000);
                size[i] = e->size;
            }
        }
    }
    for (i = 0, e = gTexSwaps[src]; i != 2; i++, e++) {
        a = (u32)assetLoad(e->id);
        if (a != 0 && to[i] != NULL) {
            memCopy32(to[i] + 8, (u8 *)(a + 0x80000008), size[i] >> 2);
        }
    }
}
