/*
 * renderobj.c - the cars as 3D objects: their models and palettes (loaded at
 * start-up), and the per-vehicle render object.
 * Original code: main segment, ROM 0x18D00-0x1ABE0 (RAM 0x80117D00-0x80119BE0).
 */
#include "common.h"
#include "race.h"

typedef struct CarPalette {              /* 8 bytes, 0x80133E20: one per palette file */
    /* 0x00 */ s32 id;                   /* palette file */
    /* 0x04 */ u16 *colours;             /* its colours once loaded (256 per slot) */
} CarPalette;

typedef struct CarModelPal {             /* 12 bytes, 0x80133E58, ends with id -1 */
    /* 0x00 */ s32 id;
    /* 0x04 */ u16 *colours;
    /* 0x08 */ s32 lights;               /* has brake lights (colour 0xFF changes) */
} CarModelPal;

extern CarPalette  gCarPalettes[7];      /* 0x80133E20 */
extern CarModelPal gCarModelPals[];      /* 0x80133E58 */
extern u32 gCarModelDLs[];                 /* 0x80150E60 - group 0x7B */
extern u32 gTrafficModels[];             /* 0x80150ECC - group 0x7C */
extern u16 gCarPalSave[7][256];          /* 0x80150F98 - the palettes as loaded */
extern u32 gCarPartsA[];                 /* 0x80151D98 - group 0x3055 */
extern u32 gCarPartsB[];                 /* 0x80151DC8 - group 0x3098: smoke frames */

typedef struct LightColour {             /* 12 bytes, list ends with index -1 */
    /* 0x00 */ s32 index;                /* palette entry */
    /* 0x04 */ u16 colour[3];            /* off, flash dim, flash bright */
    /* 0x0A */ u16 pad;
} LightColour;
typedef struct ShadowSize { s32 w, l, unk08; } ShadowSize;
typedef struct ViewerCam { s32 pos[3], angles[3], target[3], useAngles; } ViewerCam;
typedef void (*DrawHookFunc)(void);

extern LightColour gLightsTruck[];       /* 0x80133F24 - model 6 */
extern LightColour gLightsBus[];         /* 0x80133F60 - model 5 */
extern s32 gBackfireFrames[];            /* 0x80133F90 - gCarPartsA index per step, -1 ends */
extern s32 gSmokeFrames[];               /* 0x80133FC0 - gCarPartsB index per step, -1 ends */
extern s32 gSmokeFramesShort[];          /* 0x80134000 */
extern Vtx gShadowSquareVtx[4];          /* 0x80133A38 */
extern Vtx gShadowRoundVtx[5];           /* 0x80134028 */
extern ShadowSize gShadowSizes[16];      /* 0x801346C0 - per model */
extern f64 gGlowLift;                    /* 0x8013B9D0 */
extern DrawHookFunc gDrawHook;           /* 0x80150F90 */
extern ViewerCam gViewerCams[2];         /* 0x8014EF90 */
extern s32 gCurViewer;                   /* 0x8014EFE0 */
extern u32 gFrameCounter;                /* 0x80147698 */
extern u32 *gDlCursor;                   /* 0x80147684 */
extern s32 *gMtxCursor;                  /* 0x80147688 */
extern s32 gSegment3Base;                /* 0x80146594 */

s32  randRange(s32 lo, s32 hi);          /* 0x80103188 */
f32  sqrtf(f32 x);                       /* 0x80115720 */
void mtxCopy(s32 *dst, s32 *src);        /* 0x80114408 */
void mtxTranslate(s32 *m, s32 x, s32 y, s32 z); /* 0x80114498 */
void mtxFromAngles(s32 *m, s32 ax, s32 ay, s32 az); /* 0x80114824 */
void mtxBillboard(s32 *m, s32 *eye, s32 *at); /* 0x80114A0C */
void mtxScaleBy(s32 *m, s32 sx, s32 sy, s32 sz); /* 0x8011545C */
void mtxMulVec(s32 *out, s32 *m, s32 *v); /* 0x80115590 */

void puffsUpdate(RenderObj *obj, CarModelInfo *info); /* 0x80118EB0 */
void puffsDraw(RenderObj *obj, CarModelInfo *info);   /* 0x8011902C */
s32  puffSpawn(RenderObj *obj, CarModelInfo *info, s32 side, s32 flags); /* 0x80119390 */
void backfireDraw(u32 dl, s32 x, s32 y, s32 z);       /* 0x80119444 */
void backfirePart(u32 dl, s32 x, s32 y, s32 z, s32 ax, s32 az); /* 0x801194FC */
void flamesDraw(u32 dl, s32 x, s32 y, s32 z, s32 scale);       /* 0x80119660 */
void flamesPart(u32 dl, s32 x, s32 y, s32 z, s32 ax, s32 az, s32 scale); /* 0x80119728 */
void glowDraw(RenderObj *obj, CarModelInfo *info);    /* 0x801198C0 */
void dimOn(RenderObj *obj);              /* 0x80119B2C */
void dimOff(RenderObj *obj);             /* 0x80119B80 */

#define DL_PUSH(w0, w1) { u32 *_g = gDlCursor; gDlCursor = _g + 2; _g[0] = (w0); _g[1] = (u32)(w1); }
#define MTX_NEW(m) { m = gMtxCursor; gMtxCursor = m + 16; }
#define MTX_PUSH(m) DL_PUSH(0x01040040, (u32)((m) - 16))     /* gSPMatrix(push, multiply) */
#define POP() DL_PUSH(0xBD000000, 0)
#define SYNC() DL_PUSH(0xE7000000, 0)
#ifdef PORT
#define G_DL_STATIC 0x05000000           /* the port's G_DL for a list that doesn't change: replayed (port/gfx_replay.h) */
#else
#define G_DL_STATIC 0x06000000
#endif

u32  lowMemOverlayLoad(void);            /* 0x80102D74 */
void lowMemOverlayRelease(void);         /* 0x80102E84 */
s32  assetLoadGroup(s32 id, u32 *out);   /* 0x8000078C */
u32  assetFindLoaded(s32 id);            /* 0x80000994 */
void osInvalDCache(void *p, s32 size);   /* 0x80122610 */

/* 0x80117D00 - load the car models and find their palettes */
void carModelsLoad(void) {
    CarModelPal *e;
    s32 id;

    lowMemOverlayLoad();
    assetLoadGroup(0x7B, gCarModelDLs);
    assetLoadGroup(0x3055, gCarPartsA);
    assetLoadGroup(0x3098, gCarPartsB);
    e = gCarModelPals;
    if (e->id != -1) {
        id = e->id;
        do {
            u32 a = assetFindLoaded(id);
            id = e[1].id;
            e->colours = (u16 *)(a + 0x80000008);
            e++;
        } while (id != -1);
    }
    lowMemOverlayRelease();
}

/* 0x80117DB4 */
void trafficModelsLoad(void) {
    lowMemOverlayLoad();
    assetLoadGroup(0x7C, gTrafficModels);
    lowMemOverlayRelease();
}

/* 0x80117DEC - keep a copy of each car palette as loaded */
void carPalettesLoad(void) {
    CarPalette *p;
    u16 *save;
    s32 i;

    lowMemOverlayLoad();
    for (p = gCarPalettes, save = gCarPalSave[0]; save != gCarPalSave[7]; p++, save += 256) {
        p->colours = (u16 *)(assetFindLoaded(p->id) + 0x80000008);
        osInvalDCache(p->colours, 0x200);
        for (i = 0; i < 256; i++) {
            save[i] = p->colours[i];
        }
    }
    lowMemOverlayRelease();
}

/* 0x80117EBC - both colour slots of every car palette back to the original */
void carPalettesReset(void) {
    CarPalette *p;
    u16 *save;
    s32 i;

    for (p = gCarPalettes, save = gCarPalSave[0]; save != gCarPalSave[7]; p++, save += 256) {
        for (i = 0; i < 256; i++) {
            p->colours[i] = save[i];
            p->colours[256 + i] = save[i];
        }
    }
}

/* 0x80117F80 - colour slot 'slot' of a palette file gets paint scheme 'paint'
 * (0 = the original); files 5 and 6 skip their first scheme */
void carSetPalette(s32 slot, s32 pal, s32 paint) {
    u16 *src, *dst;
    s32 i;

    if (paint != 0) {
        if (pal == 5 || pal == 6) {
            paint++;
        }
        src = gCarPalettes[pal].colours + paint * 256 + 256;
    } else {
        src = gCarPalSave[pal];
    }
    dst = gCarPalettes[pal].colours + slot * 256;
    for (i = 0; i < 256; i++) {
        dst[i] = src[i];
    }
}

/* 0x8011802C */
void renderObjInit(RenderObj *obj, s32 model, s32 palette, s32 flags, s32 camDist, s32 x, s32 y, s32 z) {
    s32 i, k;

    obj->model = model;
    obj->palette = palette;
    obj->flags = flags;
    obj->camDist = camDist;
    obj->x = x;
    obj->y = y;
    obj->z = z;
    obj->angleZ = 0;
    obj->angleY = 0;
    obj->angleX = 0;
    obj->bodyLift = 0;
    obj->angleYAdd = 0;
    obj->angleXAdd = 0;
    obj->wheelSpin[0] = 0;
    obj->wheelSpin[1] = 0;
    obj->steerAngle = 0;
    obj->backfire = -1;
    obj->lastFrame = 0;
    obj->lightTimer = 0;
    for (i = 0; i < 2; i++) {
        obj->smoke[i] = 0;
        for (k = 0; k < 12; k++) {
            obj->puffs[i][k].frame = -1;
        }
    }
}

/* 0x801180EC - draw a vehicle: lights, shadow, body at the detail level for
 * its distance, wheels, exhaust flames, tyre smoke and light glows */
void renderObjDraw(RenderObj *obj) {
    CarModelInfo *info;
    ShadowSize *shadow;
    CarModelPal *mp;
    LightColour *lc;
    s32 lod, i, step, wheels, y0;
    s32 *m, *m2;
    s32 *saved[2], *extra[2];
    u16 brake, brake5;
    u32 wheelDL;

    if (obj->flags & 0x400) {
        obj->lastFrame = gFrameCounter;
    }
    /* brake lights: palette entries 0xFF (and 0xFE on model 5) */
    mp = &gCarModelPals[obj->model];
    if (mp->lights != 0) {
        if (obj->model == 6) {
            brake = (obj->flags & RO_BRAKE) ? 0xD001 : 0x4001;
        } else if (obj->model == 5) {
            if (obj->flags & RO_BRAKE) {
                brake5 = 0xD001;
                brake = 0xD001;
            } else {
                brake5 = 0x9081;
                brake = 0x7801;
            }
        } else {
            brake = (obj->flags & RO_BRAKE) ? 0xD001 : 0x4001;
        }
        mp->colours[obj->palette * 256 + 0xFF] = brake;
        if (obj->model == 5) {
            gCarModelPals[obj->model].colours[obj->palette * 256 + 0xFE] = brake5;
        }
    }
    /* flashing lights on the bus and the truck */
    if (obj->model == 6 || obj->model == 5) {
        s32 thr;
        lc = (obj->model == 6) ? gLightsTruck : gLightsBus;
        step = 0;
        if (obj->flags & RO_LIGHTS_FLASH) {
            thr = (obj->model == 6) ? 20 : 30;
            if (obj->lightTimer == 0) {
                obj->lightTimer = thr;
                step = ((u32)(thr >> 1) < (u32)thr) + 1;
            } else {
                s32 t = obj->lightTimer;
                if (gFrameCounter != obj->lastFrame) {
                    obj->lightTimer = --t;
                }
                step = ((u32)(thr >> 1) < (u32)t) + 1;
            }
        } else {
            obj->lightTimer = 0;
        }
        for (; lc->index != -1; lc++) {
            gCarModelPals[obj->model].colours[obj->palette * 256 + lc->index] = lc->colour[step];
        }
    }

    info = &gCarModelInfo[obj->model];
    shadow = &gShadowSizes[obj->model];
    lod = 0;
    if (!(obj->flags & RO_LOD0)) {
        for (lod = 0; lod < 3; lod++) {
            if (info->lodDist[lod] != 0 && info->lodDist[lod] >= (u32)obj->camDist) {
                break;
            }
        }
        if (lod >= 3) {
            return;
        }
    }

    /* once a frame: exhaust flame steps, smoke */
    if (gFrameCounter != obj->lastFrame) {
        s32 b = obj->backfire;
        obj->lastFrame = gFrameCounter;
        if (b == -1) {
            if (obj->flags & RO_BACKFIRE) {
                obj->backfire = 0;
                obj->flags &= ~RO_BACKFIRE;
            }
        } else {
            obj->backfire = b + 1;
            if (gBackfireFrames[b + 1] == -1) {
                obj->backfire = -1;
            }
        }
        puffsUpdate(obj, info);
        for (i = 0; i < 2; i++) {
            u8 sm = obj->smoke[i];
            s32 dark = (sm & 0x80) ? 2 : 0;
            if ((sm & 0x7F) == 1) {
                if (puffSpawn(obj, info, i, (dark | 5) & 0xFF) != 0) {
                    obj->smoke[i] = 0;
                }
            } else if ((sm & 0x7F) == 2) {
                puffSpawn(obj, info, i, dark & 0xFF);
            }
        }
    }
    if (obj->flags & RO_HIDDEN) {
        return;
    }

    MTX_NEW(m);
    mtxFromAngles(m, obj->angleZ, obj->angleY, obj->angleX);
    m[12] = obj->x;
    m[13] = obj->y;
    m[14] = obj->z;
    MTX_PUSH(m);
    SYNC();
    DL_PUSH(0xB7000000, 1);                          /* z buffer */

    if (obj->flags & (RO_SHADOW | RO_SHADOW_FLAT)) {
        MTX_NEW(m2);
        m2[12] = 0;
        m2[13] = ((obj->flags & RO_SHADOW_FLAT) ? -obj->shadowDrop : 0) + 0x40000;
        m2[14] = 0;
        mtxFromAngles(m2, 0, -0x4000, 0);
        mtxScaleBy(m2, (shadow->w >> 1) + 0x60000, (shadow->l >> 1) + 0x60000, 0x10000);
        MTX_PUSH(m2);
        DL_PUSH(0xB900031D, 0x504A50);
        DL_PUSH(0xFCFFFFFF, 0xFFFDF6FB);             /* primitive colour */
        DL_PUSH(0xFA000000, 0x14141496);
        if (obj->flags & RO_SHADOW_FLAT) {
            DL_PUSH(0x04300040, (u32)gShadowSquareVtx);
            DL_PUSH(0xBF000000, 0x140A00);
            DL_PUSH(0xBF000000, 0x1E1400);
        } else {
            DL_PUSH(0x04400050, (u32)gShadowRoundVtx);
            DL_PUSH(0xBF000000, 0x140A28);
            DL_PUSH(0xBF000000, 0x0A0028);
            DL_PUSH(0xBF000000, 0x001E28);
            DL_PUSH(0xBF000000, 0x1E1428);
        }
        SYNC();
        POP();
        DL_PUSH(0xFC119623, 0xFF2FFFFF);
        DL_PUSH(0xFA000000, 0xFFFFFFFF);
    }
    SYNC();
    DL_PUSH(0xB900031D, 0x553078);

#ifdef PORT
    /* The Dreamcast draws see-through texels in order without an alpha test, so
     * they still write depth. Drawn after the body (as the game does, below),
     * the wheels were hidden behind its see-through wheel arches (half wheels);
     * drawn before it, their square pictures' see-through corners hid the body
     * (squares round the wheels). So the port draws them twice: first here,
     * before the body, without writing depth (the body then goes over them
     * where it is solid and leaves them where it is see-through), then again
     * after the body as the game does, which puts back the parts in front of it. */
    DL_PUSH(0xBC000C06, obj->palette << 9);          /* segment 3: palette slot */
    SYNC();
    DL_PUSH(0xB900031D, 0x553058);                   /* as below, without Z_UPD */
    {
        /* wheels: right side, then mirrored */
        wheels = info->wheelMode[lod];
        if (wheels != 2) {
            dimOn(obj);
            if (wheels == 1) {
                SYNC();
                DL_PUSH(0xB6000000, 0x2000);             /* both sides */
            }
            for (i = 0; i < 2; i++) {
                wheelDL = *info->wheelDL[wheels][i];
                MTX_NEW(m2);
                mtxFromAngles(m2, (i == 0 && wheels == 0) ? obj->steerAngle : 0, obj->wheelSpin[i], 0);
                m2[12] = info->wheelX[i];
                m2[13] = info->wheelY[i];
                m2[14] = info->wheelZ[i];
                MTX_PUSH(m2);
                if ((obj->flags & RO_HOOK) && i == 0) {
                    gDrawHook();
                    DL_PUSH(G_DL_STATIC, wheelDL);
                    POP();
                } else {
                    DL_PUSH(G_DL_STATIC, wheelDL);
                }
                POP();
                saved[0] = m2;
                MTX_NEW(saved[1]);
                mtxCopy(saved[1], m2);
                saved[1][12] = -info->wheelX[i];
                MTX_PUSH(saved[1]);
                DL_PUSH(G_DL_STATIC, wheelDL);
                POP();
            }
            if (info->flags & 1) {                       /* second pair of rear wheels */
                for (i = 0; i < 2; i++) {
                    MTX_NEW(extra[i]);
                    mtxCopy(extra[i], saved[i]);
                    extra[i][14] = info->extraWheelZ;
                    MTX_PUSH(extra[i]);
                    DL_PUSH(G_DL_STATIC, wheelDL);
                    POP();
                }
            }
            dimOff(obj);
            if (wheels == 1) {
                SYNC();
                DL_PUSH(0xB7000000, 0x2000);
            }
        }
    }
    SYNC();
    DL_PUSH(0xB900031D, 0x553078);
#endif
    /* body (tilted, at the nearest detail level) */
    if (lod == 0) {
        MTX_NEW(m2);
        mtxFromAngles(m2, 0, obj->angleYAdd, obj->angleXAdd);
        m2[12] = 0;
        m2[13] = obj->bodyLift + info->bodyHeight;
        m2[14] = 0;
        MTX_PUSH(m2);
    }
    dimOn(obj);
    DL_PUSH(0xBC000C06, obj->palette << 9);          /* segment 3: palette slot */
    DL_PUSH(G_DL_STATIC, *info->bodyDL[lod]);
    dimOff(obj);
    if (obj->flags & RO_FLAMES) {
        s32 f = randRange(3, 7);
        y0 = (lod == 0) ? 0 : info->bodyHeight;
        flamesDraw(gCarPartsA[f], info->exhaust[0], y0 + info->exhaust[1], info->exhaust[2], randRange(0x30000, 0x40000));
    } else if (obj->backfire != -1) {
        y0 = (lod == 0) ? 0 : info->bodyHeight;
        backfireDraw(gCarPartsA[gBackfireFrames[obj->backfire]], info->exhaust[0], y0 + info->exhaust[1], info->exhaust[2]);
    }
    if (lod == 0) {
        POP();
    }

    /* wheels: right side, then mirrored */
    wheels = info->wheelMode[lod];
    if (wheels != 2) {
        dimOn(obj);
        if (wheels == 1) {
            SYNC();
            DL_PUSH(0xB6000000, 0x2000);             /* both sides */
        }
        for (i = 0; i < 2; i++) {
            wheelDL = *info->wheelDL[wheels][i];
            MTX_NEW(m2);
            mtxFromAngles(m2, (i == 0 && wheels == 0) ? obj->steerAngle : 0, obj->wheelSpin[i], 0);
            m2[12] = info->wheelX[i];
            m2[13] = info->wheelY[i];
            m2[14] = info->wheelZ[i];
            MTX_PUSH(m2);
            if ((obj->flags & RO_HOOK) && i == 0) {
                gDrawHook();
                DL_PUSH(G_DL_STATIC, wheelDL);
                POP();
            } else {
                DL_PUSH(G_DL_STATIC, wheelDL);
            }
            POP();
            saved[0] = m2;
            MTX_NEW(saved[1]);
            mtxCopy(saved[1], m2);
            saved[1][12] = -info->wheelX[i];
            MTX_PUSH(saved[1]);
            DL_PUSH(G_DL_STATIC, wheelDL);
            POP();
        }
        if (info->flags & 1) {                       /* second pair of rear wheels */
            for (i = 0; i < 2; i++) {
                MTX_NEW(extra[i]);
                mtxCopy(extra[i], saved[i]);
                extra[i][14] = info->extraWheelZ;
                MTX_PUSH(extra[i]);
                DL_PUSH(G_DL_STATIC, wheelDL);
                POP();
            }
        }
        dimOff(obj);
        if (wheels == 1) {
            SYNC();
            DL_PUSH(0xB7000000, 0x2000);
        }
    }
    POP();
    SYNC();
    DL_PUSH(0xB900031D, 0x5049F0);
    if (lod != 2) {
        dimOn(obj);
        puffsDraw(obj, info);
        dimOff(obj);
    }
    DL_PUSH(0xBC000C06, 0);
    glowDraw(obj, info);
    DL_PUSH(0xBC000C06, gSegment3Base << 9);
    SYNC();
    DL_PUSH(0xB900031D, 0x504240);
    DL_PUSH(0xB6000000, 1);
}

/* 0x80118EB0 - advance the smoke puffs: they drift up and back */
void puffsUpdate(RenderObj *obj, CarModelInfo *info) {
    SmokePuff *p;
    s32 side, k;
    s32 *frames;
    u8 fl;

    for (side = 0; side < 2; side++) {
        for (k = 0, p = obj->puffs[side]; k < 12; k++, p++) {
            if (p->frame == -1) {
                continue;
            }
            frames = (p->flags & 1) ? gSmokeFramesShort : gSmokeFrames;
            p->frame++;
            if (frames[p->frame] == -1) {
                p->frame = -1;
            } else {
                p->pos[0] += randRange(-0x20000, 0x20000);
                p->pos[1] += randRange(-0x20000, 0x20000) + 0x10000;
                fl = p->flags;
                p->pos[2] += randRange(-0x20000, 0x20000) + 0x40000;
                if (fl & 4) {
                    puffSpawn(obj, info, side, fl & 0xFB);
                }
            }
        }
    }
}

/* 0x8011902C - the smoke puffs, as sprites facing the camera */
void puffsDraw(RenderObj *obj, CarModelInfo *info) {
    SmokePuff *p;
    s32 side, k;
    s32 *frames, *m;
    s32 pos[3];
    f32 t;

    for (side = 0; side < 2; side++) {
        for (k = 0, p = obj->puffs[side]; k < 12; k++, p++) {
            if (p->frame == -1) {
                continue;
            }
            DL_PUSH(0xBC000C06, ((p->flags & 2) == 2) << 9);
            frames = (p->flags & 1) ? gSmokeFramesShort : gSmokeFrames;
            MTX_NEW(m);
            mtxFromAngles(m, p->angle[0], p->angle[1], p->angle[2]);
            m[12] = obj->x;
            m[13] = obj->y;
            m[14] = obj->z;
            mtxMulVec(pos, m, p->pos);
            mtxBillboard(m, pos, gViewerCams[gCurViewer].pos);
            m[0] = -m[0];
            m[12] = pos[0];
            m[13] = pos[1];
            m[14] = pos[2];
            mtxScaleBy(m, 0x9C40, 0x9C40, 0x9C40);
            t = (f32)p->frame / ((p->flags & 1) ? 8.0f : 15.0f);
            if (t > 1.0f) {
                t = 1.0f;
            }
            SYNC();
            DL_PUSH(0xFA000000, ((0xFF - (s32)(t * 200.0f)) & 0xFF) | 0xFFFFFF00);
            MTX_PUSH(m);
            DL_PUSH(0x06000000, gCarPartsB[frames[p->frame]]);
            POP();
            SYNC();
            DL_PUSH(0xFA000000, 0xFFFFFFFF);
        }
    }
    DL_PUSH(0xBC000C06, gSegment3Base << 9);
}

/* 0x80119390 - start a smoke puff at a rear wheel; returns 0 if all are in use */
s32 puffSpawn(RenderObj *obj, CarModelInfo *info, s32 side, s32 flags) {
    SmokePuff *p = obj->puffs[side];
    s32 k;

    for (k = 0; k < 12; k++, p++) {
        if (p->frame == -1) {
            p->frame = 0;
            p->flags = flags;
            p->angle[0] = obj->angleZ;
            p->angle[1] = obj->angleY;
            p->angle[2] = obj->angleX;
            p->pos[0] = (side != 0) ? info->wheelX[1] : -info->wheelX[1];
            p->pos[1] = info->wheelY[1] >> 1;
            p->pos[2] = info->wheelY[1] + info->wheelZ[1];
            return 1;
        }
    }
    return 0;
}

/* 0x80119444 - an exhaust backfire at both pipes */
void backfireDraw(u32 dl, s32 x, s32 y, s32 z) {
    DL_PUSH(0xB6000000, 0x2000);
    backfirePart(dl, x, y, z, 0x400, 0);
    backfirePart(dl, -x, y, z, -0x400, 0x8000);
    DL_PUSH(0xB7000000, 0x2000);
}

/* 0x801194FC - two crossed sprites */
void backfirePart(u32 dl, s32 x, s32 y, s32 z, s32 ax, s32 az) {
    s32 *m;

    MTX_NEW(m);
    mtxFromAngles(m, 0, 0x1000, az);
    m[12] = x;
    m[13] = y;
    m[14] = z;
    MTX_PUSH(m);
    DL_PUSH(0x06000000, dl);
    POP();
    MTX_NEW(m);
    mtxFromAngles(m, ax, 0x1000, 0x4000);
    m[12] = x;
    m[13] = y;
    m[14] = z;
    MTX_PUSH(m);
    DL_PUSH(0x06000000, dl);
    POP();
}

/* 0x80119660 - exhaust flames at both pipes */
void flamesDraw(u32 dl, s32 x, s32 y, s32 z, s32 scale) {
    DL_PUSH(0xB6000000, 0x2000);
    flamesPart(dl, x, y, z, 0x400, 0, scale);
    flamesPart(dl, -x, y, z, -0x400, 0x8000, scale);
    DL_PUSH(0xB7000000, 0x2000);
}

/* 0x80119728 */
void flamesPart(u32 dl, s32 x, s32 y, s32 z, s32 ax, s32 az, s32 scale) {
    s32 *m;

    MTX_NEW(m);
    mtxFromAngles(m, 0, -0x100, az);
    m[12] = x;
    m[13] = y;
    m[14] = z;
    mtxScaleBy(m, scale, scale, scale);
    MTX_PUSH(m);
    DL_PUSH(0x06000000, dl);
    POP();
    MTX_NEW(m);
    mtxFromAngles(m, ax, -0x100, 0x4000);
    m[12] = x;
    m[13] = y;
    m[14] = z;
    mtxScaleBy(m, scale, scale, scale);
    MTX_PUSH(m);
    DL_PUSH(0x06000000, dl);
    POP();
}

/* 0x801198C0 - a light glow facing the camera, bigger with distance */
void glowDraw(RenderObj *obj, CarModelInfo *info) {
    u16 glow = obj->flags & (RO_GLOW_A | RO_GLOW_B);
    ViewerCam *cam;
    f32 dx, dz, d;
    s32 *m;
    s32 v[3], pos[3];
    s32 scale;

    if (glow == 0) {
        return;
    }
    cam = &gViewerCams[gCurViewer];
    dx = (f32)((f64)(f32)((f64)(f32)obj->x / 65536.0) - (f64)(f32)cam->pos[0] / 65536.0);
    dz = (f32)((f64)(f32)((f64)(f32)obj->z / 65536.0) - (f64)(f32)cam->pos[2] / 65536.0);
    d = sqrtf(dx * dx + dz * dz);
    MTX_NEW(m);
    scale = (f64)d * 100.0;
    mtxTranslate(m, obj->x, obj->y, obj->z);
    v[0] = 0;
    v[1] = (f64)sqrtf(d) * 20.0 * 65536.0 * gGlowLift;
    v[2] = 0;
    mtxMulVec(pos, m, v);
    mtxBillboard(m, pos, cam->pos);
    m[0] = -m[0];
    m[12] = pos[0];
    m[13] = pos[1];
    m[14] = pos[2];
    mtxScaleBy(m, scale, scale, scale);
    MTX_PUSH(m);
    DL_PUSH(0x06000000, gCarModelDLs[(glow & RO_GLOW_A) == 0]);
    POP();
}

/* 0x80119B2C */
void dimOn(RenderObj *obj) {
    if (obj->flags & RO_DIM) {
        SYNC();
        DL_PUSH(0xFA000000, 0x808080FF);
    }
}

/* 0x80119B80 */
void dimOff(RenderObj *obj) {
    if (obj->flags & RO_DIM) {
        SYNC();
        DL_PUSH(0xFA000000, 0xFFFFFFFF);
    }
}
