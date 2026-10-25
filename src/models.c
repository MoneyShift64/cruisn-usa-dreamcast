/*
 * models.c - model trees: loading a group file (a list of parts, each a mesh file
 * or another group, and a list of placed items), compiling/releasing the meshes
 * of a section model, and drawing a whole tree (used for the distant backdrop).
 * Original code: main segment, ROM 0x18080-0x18AD0 (RAM 0x80117080-0x80117AD0).
 *
 * A group file holds file ids until loaded: SectionModel.parts / .items are ids,
 * replaced by addresses; ModelPart.data is an id until bit 31 of its flags is set.
 * The track header file is a group whose "items" are the SectionInfo list.
 */
#include "common.h"
#include "race.h"
#include "road.h"
#include "scenery.h"

extern s32  gModelDepth;                 /* 0x8014EFFC - recursion depth (not used otherwise) */
extern TrackHeader *gTrackGroup;         /* 0x8014F000 */
extern s32  gViewOrigin[3];              /* 0x8014EFF0 - camera, 16.16 relative to the view origin */
extern s32  gModelOriginX;               /* 0x8014F004 - section origin of the tree being drawn, 16.16 */
extern s32  gModelOriginZ;               /* 0x8014F008 */
extern f32  gDrawDist;                   /* 0x801460D8 - from the track table */
extern s32  gCurrentTrack;               /* 0x8013F9B0 */
extern u32 *gDlCursor;                   /* 0x80147684 */
extern s32 *gMtxCursor;                  /* 0x80147688 */

u32  assetLoad(s32 id);                  /* 0x80000480 */
s32  fadeAlpha(s32 player, f32 dist2, s32 prev); /* 0x8010139C */
void fadeEnd(s32 player, s32 alpha);     /* 0x80101518 */
s32  atan2s(f32 y, f32 x);               /* 0x80114100 */
void mtxRotYTranslate(s32 *m, s32 angle, s32 x, s32 y, s32 z); /* 0x80114DDC */
void mtxSection(s32 *m, RoadSection *sec, f32 y); /* 0x80114E84 */
void mtxScaleBy(s32 *m, s32 sx, s32 sy, s32 sz); /* 0x8011545C */
MeshCache *meshCacheGet(void *meshFile); /* 0x8011D5D4 */
s32  meshCacheRelease(void *meshFile);   /* 0x8011D6AC */

void modelTreeDraw(SectionModel *g, s32 reverse); /* 0x80117250 */
SectionModel *groupLoadAll(s32 id);      /* 0x80117748 */
SectionModel *groupLoad(s32 id, s32 numItems); /* 0x80117778 */
s32  groupLoadParts(SectionModel *g);    /* 0x801178B4 */
void modelTreeDrawPart(f32 x, f32 y, f32 z, s32 angle, ModelPart *part, s32 reverse); /* 0x8011794C */

#define KSEG0(a) ((u32)(a) + 0x80000000)
#define LOADED   0x80000000

#define DL_PUSH(w0, w1) { u32 *_g = gDlCursor; gDlCursor = _g + 2; _g[0] = (w0); _g[1] = (w1); }

/* 0x80117080 */
void modelDepthReset(void) {
    gModelDepth = 0;
}

/* 0x80117090 - load a track header group and the models of its first n sections */
TrackHeader *trackGroupLoad(s32 id, s32 numSections) {
    return gTrackGroup = (TrackHeader *)groupLoad(id, numSections);
}

/* 0x801170BC - and all its other models */
void trackGroupLoadRest(void) {
    groupLoadParts((SectionModel *)gTrackGroup);
}

/* 0x801170E0 (unused) */
TrackHeader *trackGroupLoadAll(s32 id) {
    trackGroupLoad(id, 0);
    trackGroupLoadRest();
    return gTrackGroup;
}

/* 0x80117110 - compile the meshes of a (loaded) group, recursively */
s32 modelPartsLoad(SectionModel *g) {
    s32 n;
    ModelPart *p;

    gModelDepth++;
    for (n = g->numParts, p = g->parts; n != 0; n--, p++) {
        if (p->flags & 1) {
            modelPartsLoad((SectionModel *)p->data);
        } else {
            p->mesh = meshCacheGet(p->data);
        }
    }
    gModelDepth--;
    return 0;
}

/* 0x801171AC - release them (mesh cleared when its last user is gone) */
s32 modelPartsRelease(SectionModel *g) {
    s32 n;
    ModelPart *p;

    gModelDepth++;
    for (n = g->numParts, p = g->parts; n != 0; n--, p++) {
        if (p->flags & 1) {
            modelPartsRelease((SectionModel *)p->data);
        } else if (meshCacheRelease(p->data) != 0) {
            p->mesh = NULL;
        }
    }
    gModelDepth--;
    return 0;
}

/* 0x80117250 - draw every item of a group within the draw distance, fading
 * with distance; near objects of kinds 0x600 / 0x700 (except subtype 0x40) use
 * the second render mode. reverse: items in reverse order. */
void modelTreeDraw(SectionModel *g, s32 reverse) {
    s32 alpha = 0xFF;
    s32 lastMode = -1;
    f32 maxD2, near;
    s32 n, start;
    ModelItem *it;
    ModelPart *part;
    s32 flags, angle, mode, kind;
    f32 x, y, z, dx, dz, d2;

    gModelDepth++;
    maxD2 = gDrawDist * gDrawDist;
    near = (gCurrentTrack == 13) ? 64000000.0f : 16000000.0f;
    n = g->numItems;
    start = 0;
    if (reverse != 0) {
        start = n - 1;
    }
    for (it = &g->items[start]; n != 0; n--) {
        if (!(it->drawFlags & 1)) {
            part = &g->parts[it->part];
            flags = it->flags;
            if (part->flags & 1) {
                x = (f32)(it->x << 16) * 0.2f;
                y = (f32)(it->y << 16) * 0.2f;
                z = (f32)(it->z << 16) * 0.2f;
            } else {
                x = it->x;
                y = it->y;
                z = it->z;
            }
            dx = ((f32)gViewOrigin[0] - (x + (f32)gModelOriginX)) * (1.0f / 65536.0f);
            dz = ((f32)gViewOrigin[2] - (z + (f32)gModelOriginZ)) * (1.0f / 65536.0f);
            if (flags & 0x8000) {
                angle = 0x4000 - atan2s(dx, dz);   /* billboard */
            } else {
                angle = it->angle;
            }
            d2 = dx * dx + dz * dz;
            if (d2 <= maxD2) {
                alpha = fadeAlpha(0, d2, alpha);
                if (alpha != 0) {
                    mode = (d2 < near);
                    kind = flags & 0x700;
                    if (mode) {
                        if (kind == 0x600 || (mode = (kind == 0x700))) {
                            mode = (flags & 0xF0) != 0x40;
                        }
                    }
                    if (lastMode != mode) {
                        DL_PUSH(0xE7000000, 0);                         /* gDPPipeSync */
                        DL_PUSH(0xB7000000, 1);                         /* set geometry mode */
                        DL_PUSH(0xB900031D, mode ? 0x504870 : 0x504260); /* render mode */
                    }
                    lastMode = mode;
                    modelTreeDrawPart(x, y, z, angle, part, reverse);
                }
            }
        }
        if (reverse != 0) {
            it--;
        } else {
            it++;
        }
    }
    gModelDepth--;
    fadeEnd(0, alpha);
}

/* 0x80117664 - draw a group placed at a road section */
void modelTreeDrawAt(SectionModel *g, s32 reverse, RoadSection *sec, f32 y) {
    s32 *m;

    gModelOriginX = (s32)(sec->xf[2] * 65536.0f);
    gModelOriginZ = (s32)(sec->xf[5] * 65536.0f);
    m = gMtxCursor;
    gMtxCursor = m + 16;
    mtxSection(m, sec, y);
    DL_PUSH(0x01040040, (u32)(m - 16));       /* gSPMatrix push */
    modelTreeDraw(g, reverse);
    DL_PUSH(0xBD000000, 0);                   /* gSPPopMatrix */
}

/* 0x80117748 - load a group and everything in it */
SectionModel *groupLoadAll(s32 id) {
    SectionModel *g = groupLoad(id, 0);

    groupLoadParts(g);
    return g;
}

/* 0x80117778 - load a group file, its parts and items lists, and the parts used
 * by its first numItems items */
SectionModel *groupLoad(s32 id, s32 numItems) {
    SectionModel *g = (SectionModel *)KSEG0(assetLoad(id));
    ModelPart *parts = (ModelPart *)KSEG0(assetLoad((s32)g->parts));
    ModelItem *items = (ModelItem *)KSEG0(assetLoad((s32)g->items));
    s32 i;
    ModelPart *p;

    i = g->numParts;
    g->parts = parts;
    g->items = items;
    while (i-- != 0) {
        parts[i].flags &= ~LOADED;
    }
    for (; numItems != 0; numItems--, items = (ModelItem *)((u8 *)items + 0x14)) {
        p = &parts[items->part];
        if (!(p->flags & LOADED)) {
            if (p->flags & 1) {
                p->data = groupLoadAll((s32)p->data);
            } else {
                p->data = (void *)KSEG0(assetLoad((s32)p->data));
            }
            p->flags |= LOADED;
        }
    }
    return g;
}

/* 0x801178B4 - load every part of a group not loaded yet */
s32 groupLoadParts(SectionModel *g) {
    s32 n;
    ModelPart *p;

    for (n = g->numParts, p = g->parts; n != 0; n--, p++) {
        if (!(p->flags & LOADED)) {
            if (p->flags & 1) {
                p->data = groupLoadAll((s32)p->data);
            } else {
                p->data = (void *)KSEG0(assetLoad((s32)p->data));
            }
            p->flags |= LOADED;
        }
    }
    return 0;
}

/* 0x8011794C - one part: a sub-group (drawn recursively) or a mesh (scaled
 * by 0.2) */
void modelTreeDrawPart(f32 x, f32 y, f32 z, s32 angle, ModelPart *part, s32 reverse) {
    s32 *m = gMtxCursor;

    gMtxCursor = m + 16;
    if (part->flags & 1) {
        mtxRotYTranslate(m, angle, (s32)x, (s32)y, (s32)z);
        DL_PUSH(0x01040040, (u32)(m - 16));
        modelTreeDraw((SectionModel *)part->data, reverse);
        DL_PUSH(0xBD000000, 0);
        return;
    }
    mtxRotYTranslate(m, angle, (s32)x, (s32)y, (s32)z);
    mtxScaleBy(m, 0x3353, 0x3353, 0x3353);
    DL_PUSH(0x01040040, (u32)(m - 16));
#ifdef PORT
    DL_PUSH(0x07000000, (u32)part->mesh->dl);   /* replayed (port/gfx_replay.h) */
#else
    DL_PUSH(0x06000000, (u32)part->mesh->dl);
#endif
    DL_PUSH(0xBD000000, 0);
}
