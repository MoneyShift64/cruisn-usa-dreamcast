/*
 * race/scenery.c - track scenery: streaming road sections in and out around
 * each player, the per-player draw lists of scenery objects, and drawing them.
 * Original code: race overlay, ROM 0x48ABC-0x4AC90 (RAM 0x8003004C-0x80032278),
 * 0x51740-0x51CD0 (RAM 0x80038CD0-0x80039260) and 0x800417E8.
 *
 * Data formats and the Dreamcast conversion plan: docs/scenery.md.
 *
 * All scenery positions are in ROUTE units (world / 5, z negated). Every
 * section that is loaded for a player has one DrawObj per model item, linked
 * into one of five draw lists by item kind. The lists are kept sorted
 * far-to-near by one bubble pass per frame (drawListSort), and drawn
 * back-to-front by merging them (sceneryDraw): the N64 version relies on
 * painter's order for its translucent props.
 */
#include "common.h"
#include "race.h"
#include "road.h"
#include "scenery.h"

extern u32 *gDlCursor;                   /* 0x80147684 */
extern s32 *gMtxCursor;                  /* 0x80147688 - 4x4 s32 16.16 matrices, converted at the end of the frame */
extern u32 gFrameCounter;                /* 0x80147698 */
extern f32 gDrawDistance[2];             /* 0x80046E68 ovl2 - per player, route units */
extern f32 gCarDrawDistance;             /* 0x80046E70 ovl2 - also how far behind the camera sections stay loaded */
extern s32 gCountdownPhase;              /* 0x800468A8 ovl2 */
extern s32 gFadeAlphaCur;                /* 0x80046DB0 ovl2 */
extern s32 *gSceneryCamMtx;              /* 0x80045FE8 ovl2 - extra matrix for SCN_MTX_CAMERA objects */
extern s32 gUprightMtx[];                /* 0x80133790 - constant 90-degree rotation about X */

/* Moving scenery objects (trains etc.), 0x94 bytes, at 0x80066DA8 ovl2 */
typedef struct Mover {
    /* 0x00 */ DrawObj *obj;
    /* 0x04 */ u8  pad04[4];
    /* 0x08 */ f32 rot[3][3];                 /* orientation, world scale */
    /* 0x2C */ u8  pad2C[0x5C - 0x2C];
    /* 0x5C */ u8  hidden;                    /* bit 0: don't draw */
    /* 0x5D */ u8  pad5D[0x94 - 0x5D];
} Mover;
extern Mover gMovers[];                  /* 0x80066DA8 ovl2 */
extern s32 gMoverActive;                 /* 0x80067938 ovl2 */

typedef struct SortEntry {               /* vehicles sorted by distance (0x80046DB8 ovl2) */
    /* 0x00 */ f32 dist2;
    /* 0x04 */ s32 vehicle;
} SortEntry;
extern SortEntry gVehicleSort[20];       /* 0x80046DB8 ovl2 */
extern SortEntry gVehicleSortTmp;        /* 0x80046E78 ovl2 */

/* Player-side data used for the police-car siren (gRacerHud 0x80150210, 0x39C bytes) */
typedef struct RacerHud {
    /* 0x000 */ u8  pad000[0x328];
    /* 0x328 */ s32 active;
    /* 0x32C */ u8  pad32C[0x39C - 0x32C];
} RacerHud;
extern RacerHud gRacerHud[];             /* 0x80150210 */
typedef struct PadState {                /* 0x18 bytes, 0x8014E378 */
    /* 0x00 */ u8  pad00[0x10];
    /* 0x10 */ u32 held;
    /* 0x14 */ u8  pad14[4];
} PadState;
extern PadState gPadState[];             /* 0x8014E378 */

f32  sectionLength(RoadSection *s);      /* 0x80031BA4 ovl2 (race.c) */
void sectionModelLoad(s32 section);      /* 0x80100AF0 */
void sectionModelRelease(s32 section);   /* 0x80100B5C */
s32  initialsMatch(s32 racer, char *name); /* 0x80032484 ovl2 */
void animScenerySpawn(s32 player, s32 variant, s32 section, ModelItem *item, MeshCache *mesh); /* 0x80039B8C ovl2 */
void fatalError(s32 code, char *msg);    /* 0x80104550 */
s32  angleDiff(s16 a, s16 b);            /* 0x80038C3C ovl2 */
s32  fadeAlpha(s32 player, f32 dist2, s32 prev); /* 0x8010139C */
void fadeEnd(s32 player, s32 alpha);     /* 0x80101518 */
void sceneryDrawExtras(s32 player);      /* 0x80039FB0 ovl2 */
void animSceneryDraw(s32 type, s32 player, DrawObj *obj, s32 one); /* 0x8003F708 ovl2 */
s32  carCompareAhead(Car *a, Car *b);    /* 0x80037BB4 ovl2 */
void carDraw(s32 vehicle, s32 player);   /* 0x80035F54 ovl2 */
s32  sfxState(s32 id, s32 player);       /* 0x8011F614  3 = playing */
void sfxPlay(s32 id, s32 player);        /* 0x8011F2A4 */
void sfxStop(s32 id, s32 player);        /* 0x8011F670 */

#define DL_PUSH(w0, w1) { u32 *_dl = gDlCursor; gDlCursor = _dl + 2; _dl[0] = (w0); _dl[1] = (w1); }

/* ------------------------------------------------------------------ */
/* Section streaming                                                  */
/* ------------------------------------------------------------------ */

/* 0x80031754 ovl2 - slide this player's window of loaded sections along the
 * track: drop sections more than gCarDrawDistance behind the camera, load
 * sections until the next one starts beyond the draw distance, then update
 * the player's first/last visible path points (gRacers pathBehind/pathAhead). */
void sectionWindowUpdate(s32 player) {
    RaceCamera *cam = &gRaceCameras[player];
    SectionWindow *w = &gSectionWindows[player];
    RacerInfo *racer;
    RoadSection *rs;
    s32 oldFirst = w->first;
    s32 first;
    s32 dropped;
    s32 added;
    s32 next;
    s32 lastSection;
    s32 last;
    s32 i;
    s32 s;
    s32 lim;
    f32 dx;
    f32 dz;
    f32 d;
    f32 len;

    roadLocate(&w->road, cam->posX * 5.0f, cam->posZ * -5.0f, 0);

    /* behind: distance from the camera's route point to the first section's origin */
    rs = &gRoadSections[w->first];
    dx = rs->xf[2] - w->road.pos[0] * 0.2f;
    dz = rs->xf[5] - w->road.pos[2] * -0.2f;
    d = sqrtf(dx * dx + dz * dz);
    first = oldFirst;
    dropped = 0;
    rs = &gRoadSections[w->first];
    while (gCarDrawDistance < d - sectionLength(rs)) {
        len = rs->length;
        rs++;
        dropped++;
        first++;
        d -= len;
        w->span -= len;
    }

    /* ahead: add sections while the next one starts inside the draw distance */
    next = w->first + w->count;
    rs = &gRoadSections[next];
    dx = rs->xf[2] - w->road.pos[0] * 0.2f;
    dz = rs->xf[5] - w->road.pos[2] * -0.2f;
    d = sqrtf(dx * dx + dz * dz);
    lastSection = gTrack->numSections - 1;
    added = 0;
    rs = &gRoadSections[w->first] + w->count;
    if (d <= gDrawDistance[player] && next < lastSection) {
        do {
            len = rs->length;
            next++;
            rs++;
            added++;
            d += len;
            w->span += len;
        } while (d <= gDrawDistance[player] && next < lastSection);
    }
    /* zero-length sections (pieces sharing an origin) always come with the next real one */
    while (rs->length == 0 && next < lastSection) {
        next++;
        rs++;
        added++;
    }

    w->count = w->count + added - dropped;
    w->first = first;
    last = w->count + first - 1;
    for (i = 0, s = last; i < added; i++, s--) {
        sectionLoad(player, s);
    }
    for (i = 0, s = oldFirst; i < dropped; i++, s++) {
        sectionUnload(player, s);
    }

    /* first / last path points inside the loaded window */
    racer = &gRacers[player];
    i = racer->pathAhead;
    lim = gPathLength - 1;
    while (i < lim && gPathPoints[i + 1].section <= last) {
        i++;
    }
    racer->pathAhead = i;
    i = racer->pathBehind;
    lim = gPathLength - 1;
    while (i < lim && gPathPoints[i + 1].section < first) {
        i++;
    }
    racer->pathBehind = i;
}

/* 0x80031CE8 ovl2 - section enters this player's window: create a DrawObj for
 * every item of its model (or, if the other player already has it, just link
 * the other player's objects into this player's lists) */
void sectionLoad(s32 player, s32 section) {
    RoadSection *rs;
    SectionModel *model;
    ModelPart *parts;
    ModelItem *item;
    DrawObj *obj;
    s32 n;
    s32 kind;
    s32 sub;
    s32 var;
    u16 f;

    sectionModelLoad(section);
    rs = &gRoadSections[section];
    if (!(rs->flags & 3)) {
        rs->flags |= (player != 0) ? 2 : 1;
        model = gTrack->models[gTrack->sections[section].model].model;
        n = model->numItems;
        item = model->items;
        parts = model->parts;
        while (n-- != 0) {
            f = item->flags;
            kind = f & 0x700;
            sub = f & 0xF0;
            var = f & 0xF;
            if (kind == 0x400 && sub == 0x60) {
                /* converted for good into a list-D prop (variants 1-3 keep variant 1) */
                item->flags = 0x630;
                kind = 0x600;
                if (var > 0 && var < 4) {
                    item->flags = 0x630 | 1;
                }
            }
            switch (kind) {
                case 0x300:                     /* also 0xB00 route markers */
                    if (gCurrentTrack == 8 && section == 0x3B) {
                        item->drawFlags |= SCN_FAR_LATE;  /* Grand Canyon */
                    }
                    drawListAdd(&gDrawListA, player, section, item, &parts[item->part]);
                    break;
                case 0x600:
                case 0x700:
                    /* secret: two props on section 0x25 only for car type 5 driven by JXP / AEG */
                    if (sub == 0xA0 && var == 0xA) {
                        if (section != 0x25 || gCars[player].model != 5 || !initialsMatch(player, "JXP")) {
                            break;
                        }
                    }
                    if (sub == 0xA0 && var == 0xB) {
                        if (section != 0x25 || gCars[player].model != 5 || !initialsMatch(player, "AEG")) {
                            break;
                        }
                    }
                    obj = drawListAdd(&gDrawListD, player, section, item, &parts[item->part]);
                    if (sub != 0x40) {
                        obj->drawFlags |= SCN_LATE;
                        if (sub == 0xA0 && var == 0xE) {
                            obj->drawFlags |= SCN_FAR_LATE;
                        }
                    }
                    break;
                case 0x400:
                    if ((item->flags & 0xFFF0) == 0x2470) {
                        animScenerySpawn(player, var, section, item, parts[item->part].mesh);
                    } else {
                        drawListAdd(&gDrawListC, player, section, item, &parts[item->part]);
                    }
                    break;
                default:
                    drawListAdd(&gDrawListB, player, section, item, &parts[item->part]);
                    break;
            }
            item++;
        }
    } else if (!(((player != 0) ? 2 : 1) & rs->flags)) {
        drawListShare(&gDrawListA, player, section);
        drawListShare(&gDrawListD, player, section);
        drawListShare(&gDrawListE, player, section);
        drawListShare(&gDrawListB, player, section);
        drawListShare(&gDrawListC, player, section);
        rs->flags |= (player != 0) ? 2 : 1;
    }
}

/* 0x80032128 ovl2 - section leaves this player's window */
void sectionUnload(s32 player, s32 section) {
    s32 freed;

    sectionModelRelease(section);
    gRoadSections[section].flags &= ~((player != 0) ? 2 : 1);
    freed = drawListRemove(&gDrawListA, player, section);
    freed += drawListRemove(&gDrawListB, player, section);
    drawListRemove(&gDrawListC, player, section);
    drawListRemove(&gDrawListD, player, section);
    drawListRemove(&gDrawListE, player, section);
}

/* 0x80032214 ovl2 - link the other player's objects of this section into this player's list */
void drawListShare(DrawList *list, s32 player, s32 section) {
    s32 other = 1 - player;
    DrawObj *o = list->head[other];

    while (o != NULL) {
        if (section == o->section) {
            o->next[player] = list->head[player];
            list->head[player] = o;
            list->count[player]++;
        }
        o = o->next[other];
    }
}

/* 0x80031BD4 ovl2 - recompute distances and do ONE bubble-sort pass (farthest
 * first). Called every frame, so the order converges over a few frames. */
void drawListSort(DrawList *list, s32 player, f32 x, f32 z) {
    DrawObj **link = &list->head[player];
    DrawObj *a = *link;
    DrawObj *b;
    f32 dx;
    f32 dz;

    if (a == NULL) {
        return;
    }
    dx = a->xf[2] - x;
    dz = a->xf[5] - z;
    a->dist2[player] = dx * dx + dz * dz;
    for (b = a->next[player]; b != NULL; b = a->next[player]) {
        dx = b->xf[2] - x;
        dz = b->xf[5] - z;
        b->dist2[player] = dx * dx + dz * dz;
        if (a->dist2[player] < b->dist2[player]) {
            a->next[player] = b->next[player];
            b->next[player] = a;
            *link = b;
            link = &b->next[player];
        } else {
            link = &a->next[player];
            a = b;
        }
    }
}

/* 0x80031C94 ovl2 - recompute distances only (list E is not sorted) */
void drawListMeasure(DrawList *list, s32 player, f32 x, f32 z) {
    DrawObj *o = list->head[player];
    f32 dx;
    f32 dz;

    while (o != NULL) {
        dx = o->xf[2] - x;
        dz = o->xf[5] - z;
        o->dist2[player] = dx * dx + dz * dz;
        o = o->next[player];
    }
}

/* ------------------------------------------------------------------ */
/* DrawObj pool and lists (ROM 0x51740)                               */
/* ------------------------------------------------------------------ */

/* 0x80038CD0 ovl2 - empty the draw lists (not list C) and build the free pool of 2000 objects */
void sceneryPoolInit(void) {
    DrawObj *o;
    s32 i;

    drawListClear(&gDrawListA);
    drawListClear(&gDrawListB);
    drawListClear(&gDrawListD);
    drawListClear(&gDrawListE);
    gDrawPool.head[0] = gDrawObjs;
    gDrawPool.count[0] = 2000;
    o = gDrawObjs;
    for (i = 1999; i != 0; i--) {
        o->next[0] = o + 1;
        o++;
    }
    o->next[0] = NULL;
}

/* 0x80038D54 ovl2 - place an instance of a model item (section-local) into the world and link it */
DrawObj *drawListAdd(DrawList *list, s32 player, s32 section, ModelItem *item, ModelPart *part) {
    DrawObj *obj = drawObjAlloc();
    RoadSection *rs;
    f32 y;

    if (obj == NULL) {
        return NULL;
    }
    y = (f32)item->y * (1.0f / 65536.0f);
    rs = &gRoadSections[section];
    xf2Mul((Xform2 *)obj->xf, (Xform2 *)rs->xf,
           xf2Rotate(NULL, item->angle, (f32)item->x * (1.0f / 65536.0f), (f32)item->z * (1.0f / 65536.0f)));
    obj->y = rs->y + y;
    obj->item = item;
    obj->section = section;
    obj->drawFlags = item->drawFlags;
    if (item->drawFlags & SCN_ANIMATED) {
        obj->special = item->part;
    } else {
        obj->special = 0;
        obj->mesh = part->mesh;
    }
    obj->next[player] = list->head[player];
    list->head[player] = obj;
    list->count[player]++;
    return obj;
}

/* 0x80038EB4 ovl2 - unlink this section's objects from the player's list; free
 * them if the other player doesn't have the section. Returns the number freed. */
s32 drawListRemove(DrawList *list, s32 player, s32 section) {
    s32 otherBit = (player != 0) ? 1 : 2;
    s32 release = !(otherBit & gRoadSections[section].flags);
    DrawObj **link = &list->head[player];
    DrawObj *o = *link;
    DrawObj *next;
    s32 freed = 0;

    while (o != NULL) {
        next = o->next[player];
        if (section == o->section) {
            *link = next;
            if (release) {
                if (o->drawFlags & SCN_MOVER) {
                    gMovers[o->special].obj = NULL;
                }
                if (o->drawFlags & SCN_MOVER_B) {
                    gMoverActive = 0;
                }
                drawObjFree(o);
                freed++;
            }
            list->count[player]--;
        } else {
            link = &o->next[player];
        }
        o = next;
    }
    return freed;
}

/* 0x80039000 ovl2 - a list-D object moved to another section: fix up each player's list membership */
void drawObjSetSection(DrawObj *obj, s32 section) {
    s32 old = obj->section;
    s32 wasIn;
    s32 isIn;
    s32 bit;
    s32 p;

    if (old == section) {
        return;
    }
    for (p = 0; p <= gNumPlayersM1; p++) {
        bit = (p != 0) ? 2 : 1;
        wasIn = gRoadSections[old].flags & bit;
        isIn = gRoadSections[section].flags & bit;
        if (wasIn && !isIn) {
            drawListUnlink(&gDrawListD, p, obj);
        }
        if (isIn && !wasIn) {
            obj->next[p] = gDrawListD.head[p];
            gDrawListD.head[p] = obj;
            gDrawListD.count[p]++;
        }
    }
    obj->section = section;
}

/* 0x80039144 ovl2 */
DrawObj *drawObjAlloc(void) {
    DrawObj *o;

    if (gDrawPool.count[0] == 0) {
        return NULL;
    }
    if (--gDrawPool.count[0] == 0) {
        sceneryPoolFull();
    }
    o = gDrawPool.head[0];
    gDrawPool.head[0] = o->next[0];
    o->next[0] = NULL;
    o->next[1] = NULL;
    return o;
}

/* 0x800391A4 ovl2 */
void drawObjFree(DrawObj *obj) {
    obj->next[0] = gDrawPool.head[0];
    gDrawPool.head[0] = obj;
    gDrawPool.count[0]++;
}

/* 0x800391CC ovl2 */
void drawListUnlink(DrawList *list, s32 player, DrawObj *obj) {
    DrawObj **link = &list->head[player];
    DrawObj *o = *link;
    DrawObj *next;

    while (o != NULL) {
        next = o->next[player];
        if (o == obj) {
            *link = next;
            list->count[player]--;
        } else {
            link = &o->next[player];
        }
        o = next;
    }
}

/* 0x80039214 ovl2 */
void drawListClear(DrawList *list) {
    list->head[0] = NULL;
    list->head[1] = NULL;
    list->count[0] = 0;
    list->count[1] = 0;
}

/* 0x8003922C ovl2 */
void sceneryPoolFull(void) {
    fatalError(0, "OUT OF FREE PIECES\n");
}

/* ------------------------------------------------------------------ */
/* Drawing                                                            */
/* ------------------------------------------------------------------ */

#define CAR_RADIUS2(car) (*(f32 *)((u8 *)(car) + 0x1F4))  /* suggested Car field: f32 radius2 at 0x1F4 */

/* 0x8003004C ovl2 - draw this player's scenery, back to front.
 * Lists B, A, D and C are each sorted far-to-near; they are merged by always
 * taking the farthest head. An object is drawn if it is within the draw
 * distance and either in front of the camera (within 90 degrees of the view
 * direction) or close (closer than its radius + 2600). Props flagged SCN_LATE
 * that are near (< 4000, or < 20000 with SCN_FAR_LATE) are skipped here and
 * drawn afterwards in a translucent pass. List E is drawn last, unsorted. */
#ifdef PORT
/* the port's scenery range, as a fraction of the draw distance (the road
 * still goes all the way); its fade-in is scaled to end at the new limit */
f32 gPortSceneryRange = 1.0f;
#define SCN_RANGE gPortSceneryRange
#else
#define SCN_RANGE 1.0f
#endif
void sceneryDraw(s32 player, f32 camX, f32 camY, f32 camZ) {
    DrawObj *heads[4];
    DrawObj *obj;
    DrawObj *o;
    MeshCache *mesh;
    s32 yaw;
    s32 alpha;
    s32 i;
    s32 pick;
    s32 late;
    s32 drawLater;
    s32 inFront;
    f32 maxD2;
    f32 maxD2Big;
    f32 best;
    f32 r2;
    f32 ox;
    f32 oz;

    DL_PUSH(0xE7000000, 0);                   /* gDPPipeSync */
    DL_PUSH(0xB900031D, 0x504260);            /* render mode: opaque, z-buffered */
    DL_PUSH(0xB7000000, 1);                   /* gSPSetGeometryMode(G_ZBUFFER) */
    yaw = 0xC000 - (s32)((gRaceCameras[player].yaw * 32768.0f) / 3.1415927f);
    alpha = 0xFF;
    maxD2Big = gDrawDistance[player] * gDrawDistance[player];   /* landmarks (bridges, buildings): all the way */
    maxD2 = maxD2Big * (SCN_RANGE * SCN_RANGE);
    heads[0] = gDrawListB.head[player];
    heads[1] = gDrawListA.head[player];
    heads[2] = gDrawListD.head[player];
    heads[3] = gDrawListC.head[player];

    /* original bug: meant to skip the objects beyond the draw distance, but the
     * result is never stored back into heads[] (they are skipped below anyway) */
    for (i = 0; i < 4; i++) {
        o = heads[i];
        while (o != NULL && maxD2 < o->dist2[player]) {
            o = o->next[player];
        }
    }

    while (heads[3] != NULL || heads[0] != NULL || heads[1] != NULL || heads[2] != NULL) {
        best = 0.0f;
        drawLater = 1;
        for (i = 0; i < 4; i++) {
            o = heads[i];
            if (o != NULL && best <= o->dist2[player]) {
                best = o->dist2[player];
                pick = i;
            }
        }
        obj = heads[pick];
        late = obj->drawFlags & SCN_LATE;
        if (late) {
            late |= obj->drawFlags & SCN_FAR_LATE;
        }
        if (best <= maxD2 || (best <= maxD2Big && obj->mesh != NULL && obj->mesh->radius2 > 500000.0f)) {
            ox = obj->xf[2];
            oz = obj->xf[5];
            inFront = (angleDiff(atan2s(ox - camX, oz - camZ), yaw) & 0xFFFF) < 0x4000;
            mesh = obj->mesh;
            if (mesh != NULL) {
                r2 = mesh->radius2;
                if (r2 < 6760000.0f) {
                    r2 = 6760000.0f;
                }
            } else {
                r2 = 6760000.0f;
            }
            if (inFront || best < r2 + 6760000.0f) {
                if (obj->drawFlags & SCN_MTX_UPRIGHT) {
                    drawLater = 0;
                } else if (obj->item->flags & 0x8000) {
                    /* billboard: turn to face the camera */
                    xf2Rotate((Xform2 *)obj->xf, 0x4000 - atan2s(camX - ox, camZ - oz), ox, oz);
                }
                if (late && drawLater && best < ((late & SCN_FAR_LATE) ? 400000000.0f : 16000000.0f)) {
                    /* near translucent prop: drawn by the late pass below */
                } else {
                    drawLater = 0;
                    alpha = fadeAlpha(player, (obj->mesh != NULL && obj->mesh->radius2 > 500000.0f)
                                              ? best : best * (1.0f / (SCN_RANGE * SCN_RANGE)), alpha);
                    if (alpha != 0) {
                        sceneryDrawObj(obj, player, obj->xf, -camX, obj->y - camY, -camZ, 0);
                    }
                }
            } else {
                drawLater = 0;
            }
        } else {
            drawLater = 0;
        }
        if (late) {
            obj->special |= drawLater ? 0x80 : 0;
        }
        heads[pick] = obj->next[player];
    }

    /* late pass: near props of list D, translucent render mode */
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xB7000000, 1);
    DL_PUSH(0xB900031D, 0x504870);
    for (o = gDrawListD.head[player]; o != NULL; o = o->next[player]) {
        if ((o->drawFlags & SCN_LATE) && (o->special & 0x80)) {
            alpha = fadeAlpha(player, o->dist2[player] * (1.0f / (SCN_RANGE * SCN_RANGE)), alpha);
            o->special &= ~0x80;
            if (alpha != 0) {
                sceneryDrawObj(o, player, o->xf, -camX, o->y - camY, -camZ, 0);
            }
        }
    }

    /* list E: no sorting, no fade, no view test */
    for (o = gDrawListE.head[player]; o != NULL; o = o->next[player]) {
        if (o->dist2[player] <= maxD2) {
            ox = o->xf[2];
            oz = o->xf[5];
            if (o->item->flags & 0x8000) {
                xf2Rotate((Xform2 *)o->xf, 0x4000 - atan2s(camX - ox, camZ - oz), ox, oz);
            }
            sceneryDrawObj(o, player, o->xf, -camX, o->y - camY, -camZ, 0);
        }
    }
    fadeEnd(player, alpha);
    sceneryDrawExtras(player);
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xB900031D, 0x504240);
    DL_PUSH(0xB6000000, 1);                   /* gSPClearGeometryMode(G_ZBUFFER) */
}

/* 0x80030714 ovl2 - set the primitive colour for the current fade level */
void sceneryFadeColor(s32 flags, s32 player) {
    s32 a;

    if (gFadeAlphaCur != 0xFF && gFadeAlphaCur != 0 && (flags & 7)) {
        DL_PUSH(0xE7000000, 0);
        if (gFadeToWhite[player]) {
            a = gFadeAlphaCur & 0xFF;
            DL_PUSH(0xFA000000, (a << 24) | (a << 16) | (a << 8) | 0xFF);
            return;
        }
        DL_PUSH(0xFA000000, (gFadeAlphaCur & 0xFF) | 0xC8C8FF00);
    }
}

/* 0x80030EFC ovl2 - draw one player's view: scenery, then the vehicles
 * (sorted far-to-near by one bubble pass per frame) */
void raceDrawWorld(s32 player) {
    RaceCamera *cam = &gRaceCameras[player];
    RacerHud *hud;
    RenderObj *robj;
    SortEntry *e;
    Car *car;
    Car *me;
    f32 camX = cam->posX;
    f32 camZ = cam->posZ;
    f32 maxD2;
    f32 carD2;
    f32 lastD2;
    f32 lim;
    f32 dx;
    f32 dz;
    s32 yaw;
    s32 ahead;
    s32 v;
    s32 i;

    sceneryDraw(player, camX, cam->posY, camZ);

    /* car model 6 (police car?): light flag 0x80 and siren sound 0x98 while button bit 0 is held */
    for (i = 0, hud = gRacerHud; i <= gNumPlayersM1; i++, hud++) {
        if (hud->active) {
            robj = &gRenderObjs[i];
            if ((gPadState[i].held & 1) && gCountdownPhase == 5) {
                robj->flags |= 0x80;
                if (robj->model == 6 && sfxState(0x98, i) != 3) {
                    sfxPlay(0x98, i);
                }
            } else {
                robj->flags &= 0xFF7F;
                if (robj->model == 6 && sfxState(0x98, i) == 3) {
                    sfxStop(0x98, i);
                }
            }
        }
    }

    maxD2 = gDrawDistance[player] * gDrawDistance[player];
    carD2 = gCarDrawDistance * gCarDrawDistance;
    yaw = 0xC000 - (s32)((cam->yaw * 32768.0f) / 3.1415927f);
    for (e = gVehicleSort; e < &gVehicleSort[20]; e++) {
        v = e->vehicle;
        if (v < gNumVehicles) {
            car = &gCars[v];
            dx = car->pos[0] * 0.2f - camX;
            dz = car->pos[2] * -0.2f - camZ;
            lastD2 = dx * dx + dz * dz;
            e->dist2 = lastD2;
        }
    }
    for (e = gVehicleSort; e < &gVehicleSort[19]; e++) {
        if (e[0].dist2 < e[1].dist2) {
            gVehicleSortTmp = e[0];
            e[0] = e[1];
            e[1] = gVehicleSortTmp;
        }
    }
    me = &gCars[player];
    for (e = gVehicleSort; e != &gVehicleSort[20]; e++) {
        v = e->vehicle;
        car = &gCars[v];
        ahead = carCompareAhead(me, car);
        if (v < gNumVehicles) {
            lim = maxD2;
            if (ahead < 0) {
                lim = carD2;              /* cars behind: only within gCarDrawDistance */
            }
            if (e->dist2 <= lim) {
                /* original bug: lastD2 is the distance of the last vehicle measured above, not this one */
                if (angleDiff(atan2s(car->pos[0] * 0.2f - camX, car->pos[2] * -0.2f - camZ), yaw) < 0x4000 ||
                    lastD2 < CAR_RADIUS2(car)) {
                    carDraw(v, player);
                }
            }
        }
    }
}

/* 0x80031308 ovl2 - emit the matrix and display list for one scenery object.
 * Matrices are written as 4x4 s32 16.16 (row-major, row 3 = translation) to
 * gMtxCursor; the display list refers to them 0x40 bytes back (the end-of-frame
 * conversion stores the N64 fixed-point Mtx there). */
void sceneryDrawObj(DrawObj *obj, s32 player, f32 *xf, f32 tx, f32 ty, f32 tz, s32 setMode) {
    RoadSection *rs;
    s32 *m;
    f32 s;
    s32 v;
    s32 single;
    s32 pushed;
    u8 flags;
    u8 idx;

    if (1024000000.0f < obj->dist2[player]) {     /* beyond 32000 units */
        return;
    }
    flags = obj->drawFlags;
    idx = obj->special & 0x7F;
    obj->special = idx;
    if (flags & SCN_MOVER) {
        if (gMovers[idx].hidden & 1) {
            return;
        }
        m = gMtxCursor;
        gMtxCursor = m + 16;
        moverMatrix(m, idx, tx, ty, tz);
        single = flags & SCN_ANIMATED;
    } else {
        m = gMtxCursor;
        single = flags & SCN_ANIMATED;
        gMtxCursor = m + 16;
        if (single) {
            /* animated objects: unscaled */
            v = (s32)(xf[0] * 65536.0f);
            m[10] = v;
            m[0] = v;
            m[5] = 0x10000;
            v = (s32)(xf[1] * 65536.0f);
            m[2] = -v;
            m[8] = v;
        } else {
            /* section models: scaled by the section's scale (about 0.2: world -> route units) */
            rs = &gRoadSections[obj->section];
            s = 65536.0f * rs->scaleXZ;
            v = (s32)(xf[0] * s);
            m[0] = v;
            m[10] = v;
            v = (s32)(xf[1] * s);
            m[2] = -v;
            m[8] = v;
            m[5] = (s32)(65536.0f * rs->scaleY);
        }
        m[12] = (s32)((xf[2] + tx) * 65536.0f);
        m[13] = (s32)(ty * 65536.0f);
        m[4] = 0;
        m[1] = 0;
        m[9] = 0;
        m[6] = 0;
        m[3] = 0;
        m[7] = 0;
        m[11] = 0;
        m[15] = 0x10000;
        m[14] = (s32)((xf[5] + tz) * 65536.0f);
    }
    DL_PUSH(0x01020040, (u32)(m - 16));       /* gSPMatrix(load, modelview) */
    pushed = 0;
    if (flags & (SCN_MTX_CAMERA | SCN_MTX_UPRIGHT | SCN_MTX_CAMERA2)) {
        pushed = 1;
        DL_PUSH(0x01040040, (flags & (SCN_MTX_CAMERA | SCN_MTX_CAMERA2)) ? (u32)(gSceneryCamMtx - 16) : (u32)gUprightMtx);
    }
    if (single) {
        if (setMode) {
            DL_PUSH(0xE7000000, 0);
            DL_PUSH(0xB7000000, 1);
            DL_PUSH(0xB900031D, 0x504870);
        }
        animSceneryDraw(obj->special, player, obj, single);
        if (setMode) {
            DL_PUSH(0xE7000000, 0);
            DL_PUSH(0xB7000000, 1);
            DL_PUSH(0xB900031D, 0x504260);
        }
    } else if (obj->mesh != NULL) {
        obj->mesh->lastFrame = gFrameCounter;  /* keeps the mesh's pages alive until this frame is shown */
#ifdef PORT
        DL_PUSH(0x07000000, (u32)obj->mesh->dl);   /* the port's G_DL for a mesh: replayed (port/gfx_replay.h) */
#else
        DL_PUSH(0x06000000, (u32)obj->mesh->dl);
#endif
    }
    if (pushed) {
        DL_PUSH(0xBD000000, 0);               /* gSPPopMatrix */
    }
}

/* 0x800417E8 ovl2 - matrix of a moving scenery object (its orientation is world scale: x 1/5) */
void moverMatrix(s32 *m, s32 mover, f32 tx, f32 ty, f32 tz) {
    Mover *mv = &gMovers[mover];
    DrawObj *o = mv->obj;

    m[12] = (s32)((o->xf[2] + tx) * 65536.0f);
    m[13] = (s32)(ty * 65536.0f);
    m[15] = 0x10000;
    m[14] = (s32)((o->xf[5] + tz) * 65536.0f);
    m[0] = (s32)(mv->rot[0][0] * 13107.2f);
    m[1] = (s32)(mv->rot[0][1] * 13107.2f);
    m[2] = (s32)(mv->rot[0][2] * 13107.2f);
    m[4] = (s32)(mv->rot[1][0] * 13107.2f);
    m[5] = (s32)(mv->rot[1][1] * 13107.2f);
    m[6] = (s32)(mv->rot[1][2] * 13107.2f);
    m[8] = (s32)(mv->rot[2][0] * 13107.2f);
    m[9] = (s32)(mv->rot[2][1] * 13107.2f);
    m[3] = 0;
    m[7] = 0;
    m[11] = 0;
    m[10] = (s32)(mv->rot[2][2] * 13107.2f);
}
