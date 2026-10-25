/*
 * race/effects.c - the track's extra animated things: particles (smoke, dust,
 * sparks and debris that bounces), animated scenery, and "movers" (trains,
 * planes and the like that travel through the scenery).
 * Original code: race overlay, ROM 0x57EE0-0x5B000 (RAM 0x8003F470-0x80042590).
 */
#include "common.h"
#include "race.h"
#include "road.h"
#include "scenery.h"
#include "frontend.h"

/* An asset reference: file id, then its address once loaded (8 bytes) */
typedef struct AssetRef {
    /* 0x00 */ s32 id;                       /* -1 ends a list */
    /* 0x04 */ void *data;
} AssetRef;

/* Animated scenery type (0x24 bytes, table 0x800450F0 ovl2, ends with id -1) */
typedef struct SceneryAnim {
    /* 0x00 */ s32 id;                       /* the scenery mesh file it replaces */
    /* 0x04 */ u32 tracks;                   /* bit per track it is used on */
    /* 0x08 */ u32 segOffset;                /* segment 3 offset per eighth of phase (texture variation) */
    /* 0x0C */ AssetRef *frames;
    /* 0x10 */ f32 rateLo;                   /* animation speed (frames per retrace) range, by phase */
    /* 0x14 */ f32 rateHi;
    /* 0x18 */ s32 phaseFrames;              /* frame offset per eighth of phase */
    /* 0x1C */ s32 always;                   /* animates while paused / before the start too */
    /* 0x20 */ s32 numFrames;
} SceneryAnim;
extern SceneryAnim gSceneryAnims[];      /* 0x800450F0 ovl2 */

/* Particle type (0x14 bytes, table 0x800452A0 ovl2, ends with id -1) */
typedef struct ParticleType {
    /* 0x00 */ s32 id;
    /* 0x04 */ u32 tracks;
    /* 0x08 */ AssetRef *frames;
    /* 0x0C */ s32 frameTime;                /* frames per animation frame */
    /* 0x10 */ s32 numFrames;
} ParticleType;
extern ParticleType gParticleTypes[];    /* 0x800452A0 ovl2 */

/* A live particle (0x3C bytes, 32 of them at 0x80066460 ovl2) */
#define PART_BILLBOARD 0x01                  /* always faces the camera */
#define PART_ONESHOT   0x02                  /* freed when its animation ends (else loops) */
#define PART_HIDDEN    0x04
#define PART_DEBRIS    0x08                  /* has a gDebris entry (falls and bounces) */
#define PART_USED      0x80
typedef struct Particle {
    /* 0x00 */ u32 flags;
    /* 0x04 */ ParticleType *type;
    /* 0x08 */ f32 x, y, z;                  /* drawing units */
    /* 0x14 */ f32 rx, ry, rz;               /* radians */
    /* 0x20 */ f32 vx, vy, vz;
    /* 0x2C */ s32 scale;                    /* 16.16 */
    /* 0x30 */ u32 frameStart;               /* gFrameCounter when the current frame began */
    /* 0x34 */ s32 frame;
    /* 0x38 */ s32 *owner;                   /* set to -1 when freed */
} Particle;
extern Particle gParticles[32];          /* 0x80066460 ovl2 */

/* Falling debris (0x10 bytes, 20 at 0x80066BE0 ovl2) */
typedef struct Debris {
    /* 0x00 */ s32 particle;                 /* -1 = free */
    /* 0x04 */ u32 offscreen;                /* bit per player it has gone out of range for */
    /* 0x08 */ f32 groundY;
    /* 0x0C */ s32 bounces;                  /* 3 = come to rest */
} Debris;
extern Debris gDebris[20];               /* 0x80066BE0 ovl2 */
extern u32 gEffectsViStart[2];           /* 0x80066D20 ovl2 */
/* The two special scenery objects that move across the track (files 0xD71 and
 * 0x27E6), at most two at once (0x20 bytes each, 0x80066D28 ovl2) */
typedef struct EffectTimer {
    /* 0x00 */ DrawObj *owner;
    /* 0x04 */ s32 frame;
    /* 0x08 */ s32 angle;                    /* binary angle */
    /* 0x0C */ f32 x, y, z;                  /* offset from the scenery object */
    /* 0x18 */ f32 t;                        /* speed; 0 = free */
    /* 0x1C */ u32 time;                     /* gFrameCounter of the last move */
} EffectTimer;
extern EffectTimer gEffectTimers[2];     /* 0x80066D28 ovl2 */
extern u32 gSegment3Base;                /* 0x80146594 */
void mtxFromAngles(s32 *m, s32 ax, s32 ay, s32 az); /* 0x80114824 */
extern CarProbe gMoverProbe;             /* 0x80066D70 ovl2 */

/* An animation list entry for movers (0x10 bytes, ends with id -1) */
typedef struct MoverAnim {
    /* 0x00 */ s32 id;
    /* 0x04 */ s32 file;
    /* 0x08 */ s32 frame;
    /* 0x0C */ void *anim;
} MoverAnim;
extern MoverAnim gMoverAnimsA[];         /* 0x800453CC ovl2 */
extern MoverAnim gMoverAnimsA9[];        /* 0x8004535C ovl2 - track 9 */
extern MoverAnim gMoverAnimsB[];         /* 0x800453EC ovl2 */
extern u8        gPlaneModelA[];         /* 0x80045360 ovl2 */
extern u8        gPlaneModelB[];         /* 0x800453D0 ovl2 */
extern ModelItem gPlaneItems[];          /* 0x80045320 ovl2 */
extern f32       gPlaneHeights[];        /* 0x8004545C ovl2 */

/* Movers: flying pieces (0x94 bytes each, 20 at 0x80066DA8 ovl2; see scenery.c) */
typedef struct Mover {
    /* 0x00 */ DrawObj *obj;                 /* NULL = free */
    /* 0x04 */ u32 soundTime;                /* gFrameCounter when a crash sound may play again */
    /* 0x08 */ Mtx3 rot;                     /* orientation */
    /* 0x2C */ Mtx3 spin;                    /* rotation applied each frame */
    /* 0x50 */ f32 vel[3];
    /* 0x5C */ u8  flags;                    /* 1: hidden, 8: flying, 0x10: ... */
    /* 0x5D */ u8  player;                   /* who broke it */
    /* 0x5E */ u8  pad5E[2];
    /* 0x60 */ CarProbe probe;               /* ground under it */
} Mover; /* size = 0x94 */
typedef struct PieceRef { void *model; s32 flag; } PieceRef;
extern PieceRef *gPlanePieces[];         /* 0x8004558C ovl2 - per plane kind, ends with model NULL */
extern ModelItem gPieceItem;             /* 0x80045348 ovl2 */
extern f32 gPieceSpread[2];              /* 0x80045594 ovl2 - 2.4 normal, 1.5 in the in-car view */
extern Mover gMovers[20];                /* 0x80066DA8 ovl2 */
extern s32 gMoversOn;                    /* 0x8006793C ovl2 */
extern s32 gPlaneKind;                   /* 0x80067944 ovl2 */
extern s32 gPlaneSmoke;                  /* 0x8006794C ovl2 - smoke particle of the broken plane, -1 none */
extern DrawObj *gPlaneObj;               /* 0x80067950 ovl2 */
extern u32 gMoverStartFrame;             /* 0x80067954 ovl2 - next frame a landing thud may play */
extern u32 gPlaneTime;                   /* 0x80067940 ovl2 - last chance of a plane */
extern s32 gPlaneSoundOn;                /* 0x80067948 ovl2 */
typedef struct JetFrame { MeshCache *mesh; u8 pad[0xC]; } JetFrame;
extern JetFrame gJetFrames[6];           /* 0x80045368 ovl2 - jet animation */
f32  sqrtf(f32 x);                       /* 0x80115720 */
void mtx3Mul(Mtx3 dst, Mtx3 a, Mtx3 b);  /* 0x80038640 ovl2 */
void drawObjSetSection(DrawObj *obj, s32 section); /* 0x80039000 ovl2 */
void planeSpawn(void);                   /* 0x80041AB4 ovl2 */

extern DrawList gDrawListE;              /* 0x8004A0C0 ovl2 */
extern s32 gCountdownPhase;              /* 0x800468A8 ovl2 */
extern u32 gFrameCounter;                /* 0x80147698 */
extern u32 gViCount;                     /* 0x80147694 */
extern u32 *gDlCursor;                   /* 0x80147684 */
extern s32 *gMtxCursor;                  /* 0x80147688 */
extern s32 gCurViewer;                   /* 0x8014EFE0 - viewer being drawn */
extern u8  gViewerCams[];                /* 0x8014EF90 (0x28 each) */

void *assetLoad(s32 id);                 /* 0x80000480 */
u8  *assetFindLoaded(s32 id);            /* 0x80000994 */
s32  randRange(s32 lo, s32 hi);          /* 0x80103188 */
f32  randFloat(void);                    /* 0x80103104 */
s32  randChance(f32 probability);        /* 0x801031DC */
void fatalError(s32 code, char *msg);    /* 0x80104550 */
void mtxBillboard(s32 *m, Xform *xf, void *viewer); /* 0x80114A0C */
void xformsUpdate(Xform *xf);            /* 0x8011508C */
void mtxScaleBy(s32 *m, s32 sx, s32 sy, s32 sz); /* 0x8011545C */
Xform2 *xf2Rotate(Xform2 *m, s32 angle, f32 tx, f32 tz); /* 0x8011D15C */
void xf2Apply(Xform2 *m, f32 *x, f32 *z); /* 0x8011D388 */
s32  roadCurrentPoint(s32 player);       /* 0x8011EF10 */
f32  roadHeadingAt(s32 point);           /* 0x8011EFA0 */
void sfxPlay(s32 id, s32 player);        /* 0x8011F2A4 */
void sfxFadeOut(s32 id, s32 player, s32 time); /* 0x8011F6CC */
void probeFindGround(CarProbe *p, s32 vehicle, s32 side); /* 0x800262D4 ovl2 */
void sfxAtPoint(f32 x, f32 z, s32 id, f32 volume); /* 0x8003777C ovl2 */

s8   particleSpawn(s32 type, f32 x, f32 y, f32 z, s32 flags); /* 0x80040070 ovl2 */
void particleFree(s8 index);             /* 0x800401E0 ovl2 */
void particleDraw(s32 player, s8 index); /* 0x800406DC ovl2 */
f32  effectEighths(u32 v);               /* 0x80040698 ovl2 */
s32  sceneryAnimClaim(void *mesh, s32 index); /* 0x80040A30 ovl2 */
ModelItem *modelItemClaim(void *mesh, u8 mask, u8 value); /* 0x80040A54 ovl2 */
void moverAnimsLoad(MoverAnim *list);    /* 0x80041A60 ovl2 */
DrawObj *drawObjAddBoth(void *part, ModelItem *item, s32 section, Xform2 *m, f32 y); /* 0x80041DA0 ovl2 */
void sectionNotVisible(s32 section);     /* 0x80041F3C ovl2 */
s32  moverLaunch(s32 slot, DrawObj *obj, Car *car); /* 0x80042354 ovl2 */
void planeShatter(DrawObj *obj, Car *car); /* 0x80041F74 ovl2 */
extern s32 gMoverActive;                 /* 0x80067938 ovl2 */
Xform2 *xf2Identity(Xform2 *m);          /* 0x8011D120 */
void sfxStop(s32 id, s32 player);        /* 0x8011F670 */
f32  sinf_tbl(f32 a);                    /* 0x80038458 ovl2 */
f32  cosf_tbl(f32 a);                    /* 0x800384AC ovl2 */
void mtx3Identity(Mtx3 m);               /* 0x8003852C ovl2 */
void mtx3RotY(Mtx3 m, f32 a);            /* 0x80038568 ovl2 */
void mtx3RotX(Mtx3 m, f32 a);            /* 0x800385D4 ovl2 */
void mtx3MulRaw(Mtx3 out, Mtx3 a, Mtx3 b); /* 0x80038670 ovl2 */
void drawObjSound(DrawObj *obj, s32 id, f32 volume); /* 0x80041A0C ovl2 */

/* 0x8003F470 ovl2 - load the animation frames used on this track (scenery and
 * particles) and reset the particle and debris state */
void trackModelsLoad(s32 track) {
    SceneryAnim *a;
    ParticleType *t;
    s32 n, i, id;
    u32 bit = 1 << track;

    a = gSceneryAnims;
    if (a->id != -1) {
        do {
            if (a->tracks & bit) {
                n = 0;
                for (i = 0; (id = a->frames[i].id) != -1; i++) {
                    n++;
                    a->frames[i].data = assetLoad(id);
                }
                a->numFrames = n;
            }
        } while ((a++)[1].id != -1);
    }
    t = gParticleTypes;
    n = 0;
    if (t->id != -1) {
        do {
            if (t->tracks & bit) {
                for (i = 0; (id = t->frames[i].id) != -1; i++) {
                    n++;
                    t->frames[i].data = assetLoad(id);
                }
                t->numFrames = n;
                n = 0;
            }
        } while ((t++)[1].id != -1);
    }
    gEffectsViStart[1] = gViCount;
    gEffectsViStart[0] = gViCount;
    for (i = 0; i < 2; i++) {
        gEffectTimers[i].t = 0.0f;
    }
    for (i = 0; i < 20; i++) {
        gDebris[i].particle = -1;
    }
}

/* 0x8003F708 ovl2 - draw an animated scenery object. Normal types: pick the
 * animation frame from the retrace count, each object running at its own phase
 * (taken from bits 3-5 of its address). Types 0xD71 / 0x27E6 instead launch a
 * moving object (on track 5 it flies across in either direction). */
void animSceneryDraw(s32 type, s32 player, DrawObj *obj, s32 one) {
    SceneryAnim *a;
    EffectTimer *e;
    u32 vi;
    s32 animate;
    s32 i, k;
    u32 oldest;
    s32 r;
    s32 *m;
    u32 *g;
    f32 ph, rate, off;

    if (gCountdownPhase > 0 && gCountdownPhase < 6) {
        gEffectsViStart[1] = gViCount;
    }
    a = &gSceneryAnims[type];
    if (a->always != 0 || (gCountdownPhase > 0 && gCountdownPhase < 6)) {
        vi = gViCount;
        animate = 1;
    } else {
        vi = gEffectsViStart[1];             /* frozen */
        animate = 0;
    }
    if (a->id != 0xD71 && a->id != 0x27E6) {
        ph = effectEighths((u32)obj);
        rate = (a->rateHi - a->rateLo) * ph + a->rateLo;
        off = ph * 8.0f * (f32)a->phaseFrames;
        k = (u32)((f32)(vi - gEffectsViStart[0]) * rate + off) % (u32)a->numFrames;
        g = gDlCursor; gDlCursor = g + 2;
        g[0] = 0xBC000C06;                  /* gSPSegment 3 */
        g[1] = (s32)((f32)a->segOffset * ph) << 9;
        g = gDlCursor; gDlCursor = g + 2;
        g[0] = 0x06000000;
        g[1] = (u32)a->frames[k].data;
        g = gDlCursor; gDlCursor = g + 2;
        g[0] = 0xBC000C06;
        g[1] = gSegment3Base << 9;
        return;
    }
    for (e = gEffectTimers; e < &gEffectTimers[2]; e++) {
        if (e->t != 0.0f && obj == e->owner) {
            break;
        }
    }
    if (e == &gEffectTimers[2]) {
        if (gTrackChoice == 10 && gNumPlayersM1 != 0 &&
            gPlayerSetup[player == 0].place < gPlayerSetup[player].place) {
            return;                             /* only the leader sets it off */
        }
        for (i = 0, e = gEffectTimers; i != 2; i++, e++) {
            if (e->t == 0.0f) {
                break;
            }
        }
        if (i == 2) {                           /* both busy: reuse the oldest */
            oldest = gFrameCounter;
            for (k = 0; k != 2; k++) {
                if (gEffectTimers[k].time <= oldest) {
                    oldest = gEffectTimers[k].time;
                    i = k;
                }
            }
            e = &gEffectTimers[i];
        }
        e->owner = obj;
        e->t = 0.0f;
        if (gTrackChoice == 10 && gCountdownPhase == 5) {
            sfxPlay(0x43, player);
        }
    }
    if (e->t == 0.0f) {
        e->time = gFrameCounter;
        if (gTrackChoice == 5) {
            e->frame = randRange(0, 3);
            r = randRange(20, 30);
            r = randRange(1, 3) * r;
            e->angle = 0;
            e->z = -200.0f;
            e->y = -80.0f;
            e->x = 0.0f;
            e->t = r;
            if (randChance(0.5f) != 0) {
                e->angle = 0x8000;
                e->t = -e->t;
                e->z = 4200.0f;
            }
        } else {
            e->frame = 0;
            e->z = 0.0f;
            e->angle = 0;
            e->y = 0.0f;
            e->x = 0.0f;
            e->t = -100.0f;
        }
    }
    m = gMtxCursor;
    gMtxCursor = m + 16;
    mtxFromAngles(m, e->angle, 0, 0);
    m[12] = (s32)(e->x * 65536.0f);
    m[13] = (s32)(e->y * 65536.0f);
    m[14] = (s32)(e->z * -65536.0f);
    g = gDlCursor; gDlCursor = g + 2;
    g[1] = (u32)(m - 16);
    g[0] = 0x01040040;
    if (a->id == 0x27E6) {
        g = gDlCursor; gDlCursor = g + 2;
        g[1] = 0;
        g[0] = 0xE7000000;
        g = gDlCursor; gDlCursor = g + 2;
        g[1] = 0x5041C8;
        g[0] = 0xB900031D;                  /* render mode */
    }
    g = gDlCursor; gDlCursor = g + 2;
    g[0] = 0x06000000;
    g[1] = (u32)a->frames[e->frame].data;
    if (a->id == 0x27E6) {
        g = gDlCursor; gDlCursor = g + 2;
        g[1] = 0;
        g[0] = 0xE7000000;
        g = gDlCursor; gDlCursor = g + 2;
        g[1] = 0x504260;
        g[0] = 0xB900031D;
    }
    g = gDlCursor; gDlCursor = g + 2;
    g[1] = 0;
    g[0] = 0xBD000000;
    if (animate == 0) {
        return;
    }
    if (gTrackChoice == 5) {
        e->z += (f32)(gFrameCounter - e->time) * e->t;
        if (e->z > 6000.0f || e->z < -2000.0f) {
            e->t = 0.0f;
        }
    } else {
        e->x += (f32)(gFrameCounter - e->time) * e->t;
        if (e->x < -32000.0f) {
            e->t = 0.0f;
        }
        e->y += -10.0f;
    }
    e->time = gFrameCounter;
}

/* 0x8003F648 ovl2 - mark every scenery item that uses an animated mesh */
void trackObjectsLoad(s32 track) {
    SceneryAnim *a = gSceneryAnims;
    u32 bit = 1 << track;
    s32 i = 0;
    void *mesh;

    if (a->id != -1) {
        do {
            if (a->tracks & bit) {
                mesh = assetFindLoaded(a->id) + 0x80000000;
                while (sceneryAnimClaim(mesh, i) != 0) {
                }
            }
            i++;
        } while ((a++)[1].id != -1);
    }
}

/* 0x8003FE94 ovl2 - throw a piece of debris up from (x, y, z) */
s8 debrisSpawn(f32 x, f32 y, f32 z) {
    Debris *d;
    Particle *p;
    s8 index;
    f32 px;

    for (d = gDebris; d != &gDebris[20]; d++) {
        if (d->particle == -1) {
            break;
        }
    }
    if (d == &gDebris[20]) {
        return -1;
    }
    px = (f32)randRange(0, 20) * 0.6f + x - 6.0f;
    index = particleSpawn(4, px, y + 300.0f, (f32)randRange(0, 20) * -0.6f + z + 6.0f, PART_DEBRIS | PART_BILLBOARD);
    if (index != -1) {
        p = &gParticles[index];
        d->particle = index;
        d->offscreen = 0;
        d->bounces = 0;
        p->vz = 0.0f;
        p->vx = 0.0f;
        d->groundY = y;
        p->vy = (f32)randRange(0, 20) * -0.6f - 4.0f;
        if (gCountdownPhase == 5) {
            sfxAtPoint(p->x * 5.0f, p->z * -5.0f, 30, 1.0f);
        }
    }
    return index;
}

/* 0x80040070 ovl2 - start a particle of a type at (x, y, z); returns its slot or -1 */
s8 particleSpawn(s32 type, f32 x, f32 y, f32 z, s32 flags) {
    ParticleType *t = gParticleTypes;
    Particle *p;
    s32 i;

    while (t->id != -1 && type != t->id) {
        t++;
    }
    if (type == t->id) {
        for (i = 0, p = gParticles; i < 32; i++, p++) {
            if (!(p->flags & PART_USED)) {
                p->flags = flags | PART_USED;
                p->type = t;
                p->frame = 0;
                p->z = z;
                p->y = y;
                p->x = x;
                p->scale = 0x10000;
                p->owner = NULL;
                p->frameStart = gFrameCounter;
                p->rz = 0.0f;
                p->ry = 0.0f;
                p->rx = 0.0f;
                p->vz = 0.0f;
                p->vy = 0.0f;
                p->vx = 0.0f;
                return i;
            }
        }
    }
    return -1;
}

/* 0x800401E0 ovl2 */
void particleFree(s8 index) {
    Particle *p = &gParticles[index];

    if (p->owner != NULL) {
        *p->owner = -1;
    }
    p->flags = 0;
}

/* 0x80040224 ovl2 - draw the visible particles in a player's view */
void particlesDraw(s32 player) {
    s32 i;
    Particle *p;

    for (i = 0, p = gParticles; i != 32; i++, p++) {
        if ((p->flags & PART_USED) && !(p->flags & PART_HIDDEN)) {
            particleDraw(player, i);
        }
    }
}

/* 0x80040298 ovl2 - during the race: move and animate the particles, and make
 * debris fall (gravity 5, at most 90), bounce three times and settle */
void particlesEndFrame(void) {
    s32 i;
    Particle *p;
    ParticleType *t;
    s32 elapsed;
    Debris *d;

    if (gCountdownPhase <= 0 || gCountdownPhase >= 6) {
        return;
    }
    for (i = 0, p = gParticles; i < 32; i++, p++) {
        if (!(p->flags & PART_USED)) {
            continue;
        }
        t = p->type;
        p->x += p->vx;
        elapsed = gFrameCounter - p->frameStart;
        p->y += p->vy;
        p->z += p->vz;
        if (elapsed >= t->frameTime) {
            p->frameStart = gFrameCounter - elapsed % t->frameTime;
            p->frame += elapsed / t->frameTime;
            if (p->frame >= t->numFrames) {
                if (p->flags & PART_ONESHOT) {
                    particleFree(i);
                } else {
                    p->frame %= t->numFrames;
                }
            }
        }
    }
    for (d = gDebris; d != &gDebris[20]; d++) {
        if (d->particle == -1) {
            continue;
        }
        p = &gParticles[d->particle];
        if (d->bounces < 3) {
            p->vy -= 5.0f;
            if (p->vy < -90.0f) {
                p->vy = -90.0f;
            }
            if (p->y <= d->groundY) {
                if (d->bounces != 0) {
                    p->vx *= 0.5f;
                    p->vz *= 0.5f;
                } else {
                    p->vx = (f32)randRange(0, 30) * 0.6f - 9.0f;
                    p->vz = (f32)randRange(0, 30) * -0.6f + 9.0f;
                }
                d->bounces++;
                if (-0.6f <= p->vy) {
                    d->bounces = 3;
                } else {
                    p->vy *= -0.5f;
                    p->y += p->vy;
                    if (gCountdownPhase == 5) {
                        sfxAtPoint(p->x * 5.0f, p->z * -5.0f, randRange(29, 30), 1.0f);
                    }
                }
            }
        }
        if (d->bounces >= 3) {
            p->vz = 0.0f;
            p->vy = 0.0f;
            p->vx = 0.0f;
        }
    }
}

/* 0x80040654 ovl2 - the crowd/cheer loop for a player on or off */
void effectSound(s32 player, s32 on) {
    if (on != 0) {
        sfxPlay(0x9E, player);
        return;
    }
    sfxFadeOut(0x9E, player, 3000);
}

/* 0x80040698 ovl2 - bits 3-5 of a value as eighths (0..0.875) */
f32 effectEighths(u32 v) {
    return (f32)((v >> 3) & 7) * 0.125;
}

/* 0x800406DC ovl2 - draw one particle. Debris out of range of every player is freed. */
void particleDraw(s32 player, s8 index) {
    Particle *p = &gParticles[index];
    s32 d;
    RaceCamera *cam;
    f32 dx, dy, dz;
    Xform xf;
    void *dl;
    s32 *m;
    u32 *g;

    if (p->flags & PART_DEBRIS) {
        for (d = 0; d != 20; d++) {
            if (index == gDebris[d].particle) {
                break;
            }
        }                                       /* (original bug: not found -> d = 20, past the end) */
    } else {
        d = -1;
    }
    cam = &gRaceCameras[player];
    dx = p->x - cam->posX;
    dy = p->y - cam->posY;
    dz = p->z - cam->posZ;
    if (gFadeFar2[player] * 0.04f < dz * dz + (dx * dx + dy * dy)) {
        if (d != -1) {
            gDebris[d].offscreen |= 1 << player;
            if (gDebris[d].offscreen == 3 || (gDebris[d].offscreen == 1 && gNumPlayersM1 == 0)) {
                particleFree(index);
                gDebris[d].particle = -1;
            }
        }
        return;
    }
    xf.x = (s32)(dx * 65536.0f);
    xf.y = (s32)(dy * 65536.0f);
    xf.z = (s32)(dz * 65536.0f);
    xf.scale = p->scale;
    dl = p->type->frames[p->frame].data;
    if (p->flags & PART_BILLBOARD) {
        m = gMtxCursor;
        gMtxCursor = m + 16;
        mtxBillboard(m, &xf, &gViewerCams[gCurViewer * 0x28]);
        m[0] = -m[0];
        m[12] = xf.x;
        m[13] = xf.y;
        m[14] = xf.z;
        if (xf.scale != 0x10000) {
            mtxScaleBy(m, xf.scale, xf.scale, xf.scale);
        }
        g = gDlCursor; gDlCursor = g + 2;
        g[1] = (u32)(m - 16);
        g[0] = 0x01040040;                  /* gSPMatrix: load, push */
    } else {
        xf.unk0C = (s32)((p->rx * 32768.0f) / 3.1415927f);
        xf.unk10 = (s32)((p->ry * 32768.0f) / 3.1415927f);
        xf.visible = 1;
        xf.unk14 = (s32)((p->rz * 32768.0f) / 3.1415927f);
        xformsUpdate(&xf);                  /* (unk0C..14 = rotation here) */
    }
    g = gDlCursor; gDlCursor = g + 2;
    g[0] = 0x06000000;                      /* gSPDisplayList */
    g[1] = (u32)dl;
    g = gDlCursor; gDlCursor = g + 2;
    g[1] = 0;
    g[0] = 0xBD000000;                      /* gSPPopMatrix */
}

/* 0x80040A30 ovl2 - claim the next scenery item using this mesh as animated object 'index' */
s32 sceneryAnimClaim(void *mesh, s32 index) {
    return (s32)modelItemClaim(mesh, 1, index);
}

/* 0x80040A54 ovl2 - find a scenery item (in any of the track's section models)
 * whose part is 'mesh' and which doesn't have 'mask' yet; mark it and set its
 * part number to 'value' */
ModelItem *modelItemClaim(void *mesh, u8 mask, u8 value) {
    ModelRef *r = gTrack->models;
    s32 n = gTrack->numModels;
    s32 m, i;
    SectionModel *sm;
    ModelItem *it;

    for (m = 0; m < n; m++, r++) {
        sm = r->model;
        for (i = 0, it = sm->items; i < sm->numItems; i++, it++) {
            if (!(it->drawFlags & mask) && mesh == sm->parts[it->part].data) {
                it->drawFlags |= mask;
                it->part = value;
                return it;
            }
        }
    }
    return NULL;
}

/* 0x80040C30 ovl2 - no movers yet */
void moversInit(void) {
    s32 i;

    for (i = 0; i < 20; i++) {
        gMovers[i].obj = NULL;
    }
    gMoversOn = 0;
    gMoverStartFrame = gFrameCounter;
    gPlaneSmoke = -1;
}

/* 0x80040C84 ovl2 - load the movers' animations (track 9 has its own set) */
void moversStart(void) {
    if (gMoversOn != 0) {
        moverAnimsLoad((gTrackChoice == 9) ? gMoverAnimsA9 : gMoverAnimsA);
        moverAnimsLoad(gMoverAnimsB);
    }
}

/* 0x80041964 ovl2 - the mover slot for a draw object, else a free slot, else -1 */
s32 moverSlot(DrawObj *obj) {
    s32 i;

    for (i = 0; i < 20; i++) {
        if (obj == gMovers[i].obj) {
            return i;
        }
    }
    for (i = 0; i != 20; i++) {
        if (gMovers[i].obj == NULL) {
            return i;
        }
    }
    return -1;
}

/* 0x80041A0C ovl2 - a sound at a draw object's position */
void drawObjSound(DrawObj *obj, s32 id, f32 volume) {
    sfxAtPoint(obj->xf[2] * 5.0f, obj->xf[5] * -5.0f, id, volume);
}

/* 0x80041A60 ovl2 */
void moverAnimsLoad(MoverAnim *list) {
    MoverAnim *e = list;

    if (e->id != -1) {
        do {
            e->anim = meshCacheGet((void *)e->file);
            e->frame = 0;
        } while ((e++)[1].id != -1);
    }
}

/* 0x80041AB4 ovl2 - launch a plane over the scenery at the far end of a
 * player's loaded track (in 2-player, usually the leader's) */
void planeSpawn(void) {
    s32 p, other, point, section;
    SectionWindow *w;
    RoadSection *sec;
    f32 height, x, z, y;
    Xform2 m;

    if (gNumPlayersM1 != 0) {
        p = gPlayerSetup[1].place < gPlayerSetup[0].place;
        if (randChance(0.2f) != 0) {
            p = (p == 0);
        }
    } else {
        p = 0;
    }
    point = roadCurrentPoint(p);
    section = gPathPoints[point].section;
    if (gNumPlayersM1 != 0) {
        other = (p == 0);
        w = &gSectionWindows[other];
        if (section >= w->first && section < w->first + w->count) {
            p = other;
            point = roadCurrentPoint(other);
            section = gPathPoints[point].section;
        }
    }
    height = gPlaneHeights[gPlaneKind];
    sec = &gRoadSections[section];
    x = (randFloat() - 0.5f) * 200.0f + sec->xf[2];
    z = (randFloat() - 0.5f) * 200.0f + sec->xf[5];
    xf2Rotate(&m, (s32)((roadHeadingAt(point) * 32768.0f) / 3.1415927f), x, z);
    x = 150.0f;
    z = 0.0f;
    xf2Apply(&m, &x, &z);
    y = sec->y + height;
    gMoverProbe.x = x * 5.0f;
    m[2] = x;
    gMoverProbe.y = y * 5.0f;
    m[5] = z;
    gMoverProbe.z = z * -5.0f;
    probeFindGround(&gMoverProbe, 0, p);
    if (gMoverProbe.offroad < 3) {
        y = gMoverProbe.groundY * 0.2f + height;
    }
    gPlaneObj = drawObjAddBoth((gPlaneKind != 0) ? gPlaneModelA : gPlaneModelB, &gPlaneItems[gPlaneKind],
                               section, &m, y);
    gPlaneObj->drawFlags |= 0x20;
}

/* 0x80041DA0 ovl2 - add a draw object to the extra list for the first player
 * whose view has the section loaded, and link it into the other's list too */
DrawObj *drawObjAddBoth(void *part, ModelItem *item, s32 section, Xform2 *m, f32 y) {
    DrawObj *obj = NULL;
    s32 p = 0;
    s32 flags;
    s32 other;
    s32 i;

    if (gNumPlayersM1 >= 0) {
        flags = gRoadSections[section].flags;
        do {
            if (flags & ((p != 0) ? 2 : 1)) {
                obj = drawListAdd(&gDrawListE, p, section, item, (ModelPart *)part);  /* 0x80038D54 ovl2 */
                for (i = 0; i < 6; i++) {
                    obj->xf[i] = (*m)[i];
                }
                obj->y = y;
                break;
            }
            p++;
        } while (gNumPlayersM1 >= p);
    }
    if (obj == NULL) {
        sectionNotVisible(section);
    }
    other = (p == 0);
    if (gNumPlayersM1 != 0 && (gRoadSections[section].flags & (other ? 2 : 1))) {
        obj->next[other] = gDrawListE.head[other];
        gDrawListE.head[other] = obj;
        gDrawListE.count[other]++;
    }
    return obj;
}

/* 0x80041F3C ovl2 */
void sectionNotVisible(s32 section) {
    char msg[32];

    sprintf(msg, "RK SECT %i NOT VISIBLE\n", section);
    fatalError(0, msg);
}

/* 0x80041F74 ovl2 - a car hit the plane: it breaks into pieces that fly off
 * (the plane object itself becomes the first piece), the jet trails smoke, and
 * the car is slowed and may spin. */
void planeShatter(DrawObj *obj, Car *car) {
    s32 section = obj->section;
    s32 player = car->unk7D;
    f32 y = obj->y;
    Xform2 m;
    PieceRef *piece;
    void *model;
    s32 made = 0;
    s32 slot;
    Mover *mv;
    Particle *p;
    f32 sp, d, r;

    xf2Identity(&m);
    m[2] = obj->xf[2];
    piece = gPlanePieces[gPlaneKind];
    m[5] = obj->xf[5];
    model = piece->model;
    while (model != NULL) {
        slot = moverSlot(NULL);
        if (slot == -1) {
            break;
        }
        if (made) {
            obj = drawObjAddBoth(model, &gPieceItem, section, &m, y);
        } else {
            obj->item = &gPieceItem;
            obj->mesh = ((ModelPart *)model)->mesh;
            obj->xf[0] = m[0];
            obj->xf[1] = m[1];
            made = 1;
            obj->xf[2] = m[2];
            obj->xf[3] = m[3];
            obj->xf[4] = m[4];
            obj->xf[5] = m[5];
            obj->y = y;
        }
        mv = &gMovers[slot];
        mv->obj = obj;
        mv->flags = 8;
        if (piece->flag != 0) {
            mv->flags = 8 | 0x10;
        }
        obj->special = slot;
        obj->drawFlags = gPieceItem.drawFlags | 0x10;
        mv->player = player;
        moverLaunch(slot, obj, car);
        model = (++piece)->model;
    }
    if (!made) {
        return;
    }
    if (gPlaneKind != 0 && gPlaneSmoke == -1) {
        gPlaneSmoke = particleSpawn(3, m[2], y, m[5], 3);
        if (gPlaneSmoke != -1) {
            gParticles[gPlaneSmoke].owner = &gPlaneSmoke;
            sp = (f64)car->speed * -0.45;
            gParticles[gPlaneSmoke].vx = sinf_tbl(car->travelDir) * sp;
            gParticles[gPlaneSmoke].vy = 0.0f;
            gParticles[gPlaneSmoke].vz = cosf_tbl(car->travelDir) * sp;
        }
    }
    if (gNumPlayersM1 < player || gPlayerSetup[player].viewAtEdge == 0) {
        sp = car->speed;
        d = 750.0f - car->roadOffset;
        if (d < 0.0f) {
            sp = -sp;
        }
        r = (sp + d) * 0.0002f;
        if (((r < 0.0f) ? -r : r) >= 0.06f) {
            car->yawRate = r;
            car->spinTimer = 1;
            car->spinRemaining = 3.14f;
        }
    }
    car->speed *= 0.5f;
    drawObjSound(gPlaneObj, 0x38, 1.0f);
    if (gPlaneKind == 0) {
        sfxStop(0x4F, 0);
        if (gNumPlayersM1 != 0) {
            sfxStop(0x4F, 1);
        }
    }
    drawObjSound(gPlaneObj, 0x36, 1.0f);
    gMoverActive = 0;
}

/* 0x80042354 ovl2 - send a piece flying off in roughly the car's direction */
s32 moverLaunch(s32 slot, DrawObj *obj, Car *car) {
    Mover *mv = &gMovers[slot];
    f32 k, speed, dir, v, up, cap;
    Mtx3 a, b;

    mtx3Identity(mv->rot);
    if (car->unk7D <= gNumPlayersM1) {
        k = gPieceSpread[gPlayerSetup[car->unk7D].viewAtEdge];
    } else {
        k = gPieceSpread[0];
    }
    speed = (car->speed < 100.0f) ? 100.0f : car->speed;
    dir = (randFloat() - 0.5f) * 0.24f * k + car->travelDir;
    v = ((randFloat() * -0.5f) * k - 0.7f) * speed;
    mv->vel[0] = sinf_tbl(dir) * v;
    mv->vel[2] = cosf_tbl(dir) * v;
    up = (randFloat() * 0.05f + 0.1f) * k * 1.5f * speed;
    cap = 27.0f * k;
    if (cap < up) {
        up = cap;
    }
    mv->vel[1] = up;
    mtx3RotX(a, (randFloat() - 0.5f) * 0.8f);
    mtx3RotY(b, (randFloat() - 0.5f) * 0.4f);
    mtx3MulRaw(mv->spin, a, b);
    return 0;
}

/* 0x80040CE4 ovl2 - a car hit an animated scenery object (kind 0x30 / 0x50) or
 * the plane (0xF0): knock it flying (again, unless it is a plane piece), with a
 * crash sound at most every 15-30 frames (none for kind 0x50). 1 = no free slot. */
s32 objectAnimHit(s32 kind, s32 variant, DrawObj *obj, Car *car) {
    s32 slot;
    Mover *mv;
    s32 player;
    f32 dir, v, up;
    Mtx3 a, b;

    if (kind == 0xF0) {
        planeShatter(obj, car);
        return 0;
    }
    slot = moverSlot(obj);
    if (slot == -1) {
        return 1;
    }
    mv = &gMovers[slot];
    if (mv->obj == NULL) {
        mv->obj = obj;
        mv->flags = 0;
        if (variant == 1) {
            mv->flags = 4;
        }
        mv = &gMovers[slot];
        mtx3Identity(mv->rot);
        mv->soundTime = gFrameCounter;
        obj->mesh->refs += 2;
        obj->special = slot;
        obj->drawFlags |= 0x10;
    } else {
        if (mv->flags & 0x10) {
            return 0;
        }
        mv->flags &= ~2;
        mv = &gMovers[slot];
    }
    player = car->unk7D;
    mv->player = player;
    dir = (randFloat() - 0.5f) * 0.2f + car->travelDir;
    v = (randFloat() * 0.65f + 0.8f) * -1.5f * car->speed;
    mv->vel[0] = sinf_tbl(dir) * v;
    mv->vel[2] = cosf_tbl(dir) * v;
    up = (randFloat() * 0.3f + 0.2f) * 1.5f * car->speed;
    if (up > 65.0f) {
        up = 65.0f;
    }
    mv->vel[1] = up;
    mtx3RotX(a, (randFloat() - 0.5f) * 0.8f);
    mtx3RotY(b, (randFloat() - 0.5f) * 0.4f);
    mtx3MulRaw(mv->spin, a, b);
    if (kind != 0x50 && gFrameCounter >= mv->soundTime) {
        mv->soundTime = randRange(15, 30) + gFrameCounter;
        if (gNumPlayersM1 >= player) {
            drawObjSound(obj, (variant == 1) ? 0x2B : 0x73, 1.0f);
        } else {
            drawObjSound(obj, (variant == 1) ? 0x2A : 0x2C, 1.0f);
        }
    }
    return 0;
}

/* 0x80041024 ovl2 - during the race: the plane (every 10 s, a 50% chance of
 * one flying over; its engine sound starts when the nearest player is under 20
 * frames away; jet planes animate), then move the flying pieces: spin, gravity
 * 4, land on the ground (bounce at half speed with a thud), stop when slow. */
void moversUpdate(void) {
    s32 p, best;
    f32 bestDist, d, spd;
    s32 flags;
    Mover *mv;
    DrawObj *obj;
    CarProbe *pr;
    f32 lim, h, vy;
    u32 n;

    if (gCountdownPhase <= 0 || gCountdownPhase >= 6) {
        return;
    }
    if (gMoversOn != 0) {
        if (gPlaneKind == 0 && gMoverActive != 0 && gPlaneSoundOn == 0) {
            bestDist = 1e21f;
            best = -1;
            if (gNumPlayersM1 >= 0) {
                flags = gRoadSections[gPlaneObj->section].flags;
                p = 0;
                do {
                    if (flags & ((p != 0) ? 2 : 1)) {
                        if (gPlaneObj->dist2[p] < bestDist) {
                            best = p;
                            bestDist = gPlaneObj->dist2[p];
                        }
                    }
                    p++;
                } while (gNumPlayersM1 >= p);
            }
            if (best != -1) {
                d = sqrtf(bestDist);
                spd = (u32)(gCars[best].speed * 1.5f);
                if (spd > 0.0001f && d / spd <= 20.0f) {
                    sfxPlay(0x4F, best);
                    gPlaneSoundOn = 1;
                }
            }
        }
        if (gPlaneKind != 0 && gMoverActive != 0) {
            gPlaneObj->mesh = gJetFrames[(gFrameCounter >> 3) % 6].mesh;
        }
        if (gFrameCounter - gPlaneTime >= 601) {
            gPlaneTime = gFrameCounter;
            if (gMoverActive == 0 && randChance(0.5f) != 0) {
                gMoverActive = 1;
                gPlaneSoundOn = 0;
                planeSpawn();
            }
        }
    }
    for (mv = gMovers; mv != &gMovers[20]; mv++) {
        obj = mv->obj;
        if (obj == NULL) {
            continue;
        }
        flags = mv->flags;
        if (flags & 2) {
            continue;
        }
        mtx3Mul(mv->rot, mv->rot, mv->spin);
        obj->xf[2] = (f64)obj->xf[2] + (f64)mv->vel[0] * 0.4;
        obj->y = (f64)obj->y + (f64)mv->vel[1] * 0.4;
        obj->xf[5] = (f64)obj->xf[5] + (f64)mv->vel[2] * 0.4;
        mv->vel[1] -= 4.0f;
        if (mv->vel[1] < -100.0f) {
            mv->flags |= 3;                  /* fell off the world */
            continue;
        }
        pr = &mv->probe;
        pr->x = (f64)obj->xf[2] * 5.0;
        pr->y = (f64)obj->y * 5.0;
        pr->z = (f64)obj->xf[5] * -5.0;
        if (gNumPlayersM1 >= mv->player) {
            probeFindGround(pr, 0, mv->player);
        } else {
            probeFindGround(pr, 0, 0);
            if (pr->offroad >= 3 && gNumPlayersM1 != 0) {
                probeFindGround(pr, 0, 1);
            }
        }
        if (pr->offroad >= 3) {
            continue;
        }
        drawObjSetSection(obj, pr->section);
        lim = (flags & 8) ? 55.0f : 200.0f;
        h = pr->unk20;
        if (lim < h || mv->vel[1] > 0.0f) {
            continue;
        }
        obj->y = (f64)obj->y - (f64)(lim - h) * 0.2;
        vy = mv->vel[1];
        if (vy > -20.0f) {
            mv->flags |= 2;                  /* at rest */
            continue;
        }
        mv->vel[0] = (f64)mv->vel[0] * 0.5;
        mv->vel[1] = (f64)vy * -0.5;
        mv->vel[2] = (f64)mv->vel[2] * 0.5;
        if (gFrameCounter >= gMoverStartFrame) {
            gMoverStartFrame = randRange(2, 4) + gFrameCounter;
            vy = mv->vel[1];
            if (vy > 30.0f) {
                n = 30.0f;
            } else {
                n = vy;
            }
            drawObjSound(obj, (flags & 4) ? 0x2A : 0x69, (f32)n * 0.033333335f);
        }
    }
}
