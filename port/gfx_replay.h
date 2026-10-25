/*
 * gfx_replay.h - part of gfx.c: the scenery replay.
 *
 * The track's scenery is a few hundred compiled meshes (src/meshcomp.c) drawn
 * again every frame, and interpreting their display lists - texture loads and
 * hashing, combiner and render state, a Vert per loaded vertex - cost more
 * than drawing them. sceneryDrawObj calls them with the port's own command
 * 0x07. The first time one runs (from a given render state) what it draws is
 * recorded: its vertices in model space with their finished texture
 * coordinates and colours, its triangles, the render state of each run of
 * them, and the state it leaves behind. After that it is replayed: only the
 * vertices are transformed and the visible triangles copied out. Triangles
 * that need clipping still go through the normal path, so the result is the
 * same as interpreting the list.
 *
 * A recording is dropped when the mesh is compiled again (port_dl_compiled),
 * when the mesh cache is emptied (port_dl_flush), or when a texture it uses
 * no longer has the same contents (checked through the hash memo, so it costs
 * little). The fade alpha (primitive colour) is not part of the key: the
 * colours are combined again when it differs from the last replay.
 */

#define RV_MAX 1024                      /* vertices, triangles and state runs in one recording */
#define RT_MAX 1024                      /* (the biggest seen: 233 vertices, 114 triangles, 20 runs) */
#define RS_MAX 64
#define RL_MAX 8                         /* TMEM loads left behind */
#define RTAB_N 1024
#define RPOOL_MAX (1024 << 10)           /* the recordings (less if memory is short) */
#define RSPARE (384 << 10)               /* left free for everything else (build 62 on SF: 717 KB was still free with a 768 KB pool) */

typedef struct RVert {
    f32 x, y, z;                         /* model space */
    f32 s, t, r, g, b, a;                /* as loaded: for clipped triangles and new colours */
    f32 u, v;                            /* finished */
    u8 c[4];
} RVert;

typedef struct RSeg {                    /* a run of triangles drawn with one render state */
    s32 textured, blend, zcmp, zupd, variant;
    GfxTex tex;
    u32 combineHi, combineLo, omH, envRaw;   /* the combiner's inputs (and the primitive colour) */
    f32 env[4];
    u16 uls, ult;
    const u8 *tsrc;                      /* the texels and palette (to check they are unchanged) */
    u32 trows, tpitch, tbytes, thash;
    const u8 *psrc;
    u32 plen, phash;
    u16 v0, nv, t0, nt;
} RSeg;

typedef struct RKey {                    /* the state a recording depends on */
    u32 omL, omH, geom, combineHi, combineLo, envRaw, firstEnv, texOn, texTile;
    f32 texS, texT, vp[6], fogc[4], blendc[4];
} RKey;

typedef struct RExit {                   /* the state the display list leaves */
    u32 omL, omH, geom, combineHi, combineLo, primRaw, envRaw;
    f32 prim[4], env[4];
    s32 firstPrim, firstEnv, texOn, texTile;
    f32 texS, texT;
    u32 timgAddr, timgFmt, timgSiz, timgWidth;
    Tile tile[8];
} RExit;

typedef struct REntry {
    struct REntry *next;
    u32 size;                            /* bytes of it in the pool (see rep_alloc) */
    const u32 *dl;
    RKey key;
    u32 segMask, segv[16];               /* segments its addresses went through, and their values */
    u32 prim;                            /* the primitive colour the vertex colours are for */
    s32 firstPrim;
    s32 okFrame;                         /* the frame it was last checked and found good (rep_check) */
    s32 bad;                             /* not replayable: always interpreted */
    u32 wrote;                           /* the state it sets (W_*): restored after a replay */
    s32 envFree;                         /* the environment colour at the call doesn't matter */
    s32 checked;                         /* a list in ordinary memory (0x05): dropped when the memory */
    u32 lo, hi, wn;                      /* it read ([lo, hi)) is written (gWrites since wn) */
    s32 ns, nv, nt, nl;
    f32 bmin[3], bmax[3];                /* the box its vertices are in (model space): see rep_run */
    RSeg *seg;
    RVert *v;
    u16 *tri;
    struct TmemLoad loads[RL_MAX];
    RExit ex;
} REntry;

/* the state a display list sets */
#define W_ENV 1
#define W_COMBINE 2
#define W_OML 4
#define W_OMH 8
#define W_TEXTURE 16
#define W_TIMG 32
#define W_TILE 64

typedef struct XVert {                   /* transformed for this frame */
    f32 x, y, z, w, nx, ny;              /* clip space, and x, y after the divide */
    s32 code;                            /* (on the screen: straight into gPV) */
} XVert;

/* the Dreamcast's matrix unit transforms a vertex in one instruction */
#if defined(__SH4_SINGLE_ONLY__) || defined(__SH4_SINGLE__)
#define REP_FTRV 1
void mat_load(const void *m);            /* KallistiOS: the 4x4 matrix into XMTRX (same layout as Mtx4) */
#endif
static s32 gUnknown[RS_MAX], gUnknownN;  /* runs whose texture isn't loaded (rep_check) */

static REntry *gRTab[RTAB_N];
static u8 *gRPool;
static u32 gRPoolUsed, gRPoolBytes;
static u32 gRHead, gRTail, gRWrap;       /* the pool as a ring: see rep_alloc */
static s32 gRInit;
/* recording */
static const u32 *gRecDl;
static RKey gRecKey;
static s32 gRecDepth = -1;
static s32 gRecNs, gRecNv, gRecNt;
static u32 gRecWrote;
static s32 gRecChecked;
static u32 gRecLo, gRecHi;               /* the memory the list and its vertices are in */
static s32 gRecEnvUsed;                  /* a run read the environment colour before the list set it */
static RSeg *gRecSeg;
static RVert *gRecV;
static u16 *gRecTri;
/* replay */
static XVert *gXV;
static GbPVert *gPV;
s32 gRepOff;                             /* the host's test: interpret everything */
s32 gStRepHit, gStRepRec, gStRepInterp, gStRepBad, gStRepPool, gStRepMaxV, gStRepMaxT, gStRepMaxS, gStRepMoved, gStRepCulled;
u32 gStRepUsed(void) { return gRPoolUsed; }
/* the start-of-race page (builds 72-73): this frame's checking time, clipped-triangle time
 * and count, time putting back each list's texture loads, triangles sent directly (us) */
u32 gRpUs[5], gRpClipN, gRpTriN;   /* [3]: the rest of the triangle loop, [4]: triangles of interpreted lists */

static void rep_init(void) {
    gRInit = 1;
    for (gRPoolBytes = RPOOL_MAX; gRPoolBytes >= (256 << 10); gRPoolBytes -= 256 << 10) {
        u32 work = RS_MAX * sizeof(RSeg) + RV_MAX * (sizeof(RVert) + sizeof(XVert) + sizeof(GbPVert)) + RT_MAX * 3 * sizeof(u16);
        void *probe = gb_alloc(gRPoolBytes + work + RSPARE);   /* the pool, the work space, room to spare */
        if (probe != NULL) {
            gb_free(probe);
            break;
        }
    }
    if (gRPoolBytes < (256 << 10)) {
        return;                          /* everything is interpreted */
    }
    gRPool = (u8 *)gb_alloc(gRPoolBytes);
    gRecSeg = (RSeg *)gb_alloc(RS_MAX * sizeof(RSeg));
    gRecV = (RVert *)gb_alloc(RV_MAX * sizeof(RVert));
    gRecTri = (u16 *)gb_alloc(RT_MAX * 3 * sizeof(u16));
    gXV = (XVert *)gb_alloc(RV_MAX * sizeof(XVert));
    gPV = (GbPVert *)gb_alloc(RV_MAX * sizeof(GbPVert));
    if (gRecSeg == NULL || gRecV == NULL || gRecTri == NULL || gXV == NULL || gPV == NULL) {
        gRPool = NULL;                   /* no memory: everything is interpreted */
    }
}

u32 gStRepPoolKB(void) {                 /* the size of the recordings' memory, for the counters */
    return gRPool != NULL ? gRPoolBytes >> 10 : 0;
}

/* forget every recording */
void port_dl_flush(void) {
    s32 i;
    for (i = 0; i < RTAB_N; i++) {
        gRTab[i] = NULL;
    }
    gRPoolUsed = 0;
    gRHead = gRTail = gRWrap = 0;
    gRecOn = 0;
    gRecDepth = -1;
}

static u32 rep_slot(const u32 *dl) {
    return (((u32)dl >> 3) * 2654435761u) >> 22;   /* RTAB_N = 1024 */
}

/* take a recording out of the table (if it is still there) */
static void rep_unlink(REntry *e) {
    REntry **l = &gRTab[rep_slot(e->dl)];
    while (*l != NULL) {
        if (*l == e) {
            *l = e->next;
            return;
        }
        l = &(*l)->next;
    }
}

/* The pool is a ring: recordings are added at the head, and the oldest go from
 * the tail to make room (rather than emptying the whole pool when it is full,
 * which made every mesh on the screen be recorded again in one frame - the
 * worst stutters on San Francisco, Grand Canyon and Appalachia). A recording
 * still in use that gets near the tail is moved to the head (rep_keep).
 * Recordings lie in the order they were made, gRTail up to gRHead, wrapping at
 * gRWrap (the end of the data before the head went back to the start). */
s32 gStRepEvicted;                       /* recordings dropped to make room (the report) */
static void rep_evict(void) {
    REntry *e = (REntry *)(gRPool + gRTail);
    gStRepEvicted++;
    s32 wrapped = (gRHead <= gRTail);    /* the data runs from the tail to gRWrap, then 0 to the head */
    rep_unlink(e);
    gRTail += e->size;
    gRPoolUsed -= e->size;
    if (gRPoolUsed == 0) {
        gRHead = gRTail = gRWrap = 0;
    } else if (wrapped && gRTail >= gRWrap) {
        gRTail = 0;
    }
}

static void *rep_alloc(u32 n) {
    void *p;
    n = (n + 7) & ~7u;
    if (gRPool == NULL || n > gRPoolBytes / 4) {
        return NULL;
    }
    for (;;) {
        if (gRPoolUsed == 0) {
            gRHead = gRTail = gRWrap = 0;
            break;
        }
        if (gRTail < gRHead) {           /* not wrapped: free from the head to the end */
            if (gRHead + n <= gRPoolBytes) {
                break;
            }
            gRWrap = gRHead;             /* back to the start */
            gRHead = 0;
            continue;
        }
        if (gRHead + n <= gRTail) {      /* wrapped: free from the head to the tail */
            break;
        }
        rep_evict();                     /* the oldest are in the way */
    }
    p = gRPool + gRHead;
    ((REntry *)p)->size = n;
    gRHead += n;
    gRPoolUsed += n;
    return p;
}

/* a recording in use: if it is among the oldest quarter, move it to the head
 * so the ring doesn't drop it (copying is far cheaper than recording again) */
static REntry *rep_keep(REntry *e) {
    u32 off = (u8 *)e - gRPool, age, n = e->size;
    REntry *d;
    age = (off >= gRTail) ? off - gRTail : (gRWrap - gRTail) + off;
    if (age >= gRPoolUsed / 4) {
        return e;
    }
    rep_unlink(e);                       /* (out first: making room may drop it) */
    d = (REntry *)rep_alloc(n);
    if (d == NULL) {
        return NULL;
    }
    if (d != e) {                        /* forwards: if they overlap, e's room was given up and d is below it */
        u32 *to = (u32 *)d, *from = (u32 *)e, i;
        for (i = 0; i < n / 4; i++) {
            to[i] = from[i];
        }
    }
    d->size = n;
    d->seg = (RSeg *)(d + 1);
    d->v = (RVert *)(d->seg + d->ns);
    d->tri = (u16 *)(d->v + d->nv);
    d->next = gRTab[rep_slot(d->dl)];
    gRTab[rep_slot(d->dl)] = d;
    gStRepMoved++;
    return d;
}

/* a mesh's display list was compiled (again) at dl */
void port_dl_compiled(const void *dl) {
    REntry **l = &gRTab[rep_slot((const u32 *)dl)];
    while (*l != NULL) {
        if ((*l)->dl == (const u32 *)dl) {
            *l = (*l)->next;             /* (its memory is reused when the pool is emptied) */
        } else {
            l = &(*l)->next;
        }
    }
    if (gRecOn && gRecDl == (const u32 *)dl) {
        gRecOn = 0;
        gRecDepth = -1;
    }
}

static void rkey_now(RKey *k) {
    s32 i;
    k->omL = G.omL; k->omH = G.omH; k->geom = G.geom;
    k->combineHi = G.combineHi; k->combineLo = G.combineLo;
    k->envRaw = G.envRaw; k->firstEnv = gFirstEnv;
    k->texOn = G.texOn; k->texTile = G.texTile;
    k->texS = G.texS; k->texT = G.texT;
    for (i = 0; i < 3; i++) {
        k->vp[i] = G.vpScale[i];
        k->vp[3 + i] = G.vpTrans[i];
    }
    for (i = 0; i < 4; i++) {
        k->fogc[i] = G.fogc[i];
        k->blendc[i] = G.blendc[i];
    }
}

static s32 rkey_same(const RKey *a, const RKey *b, s32 envFree) {
    const u32 *x = (const u32 *)a, *y = (const u32 *)b;
    u32 i;
    for (i = 0; i < sizeof(RKey) / 4; i++) {
        if (x[i] != y[i] && !(envFree && (&x[i] == &a->envRaw || &x[i] == &a->firstEnv))) {
            return 0;
        }
    }
    return 1;
}

/* does the combiner read the environment colour? */
static s32 cc_reads_env(void) {
    u32 hi = G.combineHi, lo = G.combineLo;
    s32 c0 = (hi >> 15) & 0x1F, c1 = hi & 0x1F;
    return ((hi >> 20) & 0xF) == 5 || ((lo >> 28) & 0xF) == 5 || c0 == 5 || c0 == 13 || ((lo >> 15) & 7) == 5 ||
           ((hi >> 12) & 7) == 5 || ((hi >> 9) & 7) == 5 || ((lo >> 12) & 7) == 5 || ((lo >> 9) & 7) == 5 ||
           ((hi >> 5) & 0xF) == 5 || ((lo >> 24) & 0xF) == 5 || c1 == 5 || c1 == 13 || ((lo >> 6) & 7) == 5 ||
           ((lo >> 21) & 7) == 5 || ((lo >> 18) & 7) == 5 || ((lo >> 3) & 7) == 5 || (lo & 7) == 5;
}

static void exit_save(RExit *x) {
    s32 i;
    x->omL = G.omL; x->omH = G.omH; x->geom = G.geom;
    x->combineHi = G.combineHi; x->combineLo = G.combineLo;
    x->primRaw = G.primRaw; x->envRaw = G.envRaw;
    for (i = 0; i < 4; i++) {
        x->prim[i] = G.prim[i];
        x->env[i] = G.env[i];
    }
    x->firstPrim = gFirstPrim; x->firstEnv = gFirstEnv;
    x->texOn = G.texOn; x->texTile = G.texTile; x->texS = G.texS; x->texT = G.texT;
    x->timgAddr = G.timgAddr; x->timgFmt = G.timgFmt; x->timgSiz = G.timgSiz; x->timgWidth = G.timgWidth;
    for (i = 0; i < 8; i++) {
        x->tile[i] = G.tile[i];
    }
}

/* only what the list sets: the rest is as it was at the call */
static void exit_load(const RExit *x, u32 w) {
    s32 i;
    if (w & W_OML) G.omL = x->omL;
    if (w & W_OMH) G.omH = x->omH;
    if (w & W_COMBINE) {
        G.combineHi = x->combineHi;
        G.combineLo = x->combineLo;
    }
    if (w & W_ENV) {
        G.envRaw = x->envRaw;
        for (i = 0; i < 4; i++) {
            G.env[i] = x->env[i];
        }
        gFirstEnv = x->firstEnv;
    }
    if (w & W_TEXTURE) {
        G.texOn = x->texOn; G.texTile = x->texTile; G.texS = x->texS; G.texT = x->texT;
    }
    if (w & W_TIMG) {
        G.timgAddr = x->timgAddr; G.timgFmt = x->timgFmt; G.timgSiz = x->timgSiz; G.timgWidth = x->timgWidth;
    }
    if (w & W_TILE) {
        for (i = 0; i < 8; i++) {
            G.tile[i] = x->tile[i];
        }
    }
}

/* ---- recording ---- */
static void rec_range(const void *p, u32 n) {
    if ((u32)p < gRecLo) gRecLo = (u32)p;
    if ((u32)p + n > gRecHi) gRecHi = (u32)p + n;
}

static void rec_begin(const u32 *dl, const RKey *k, s32 depth, s32 checked) {
    s32 i;
    gRecOn = 1;
    gRecChecked = checked;
    gRecLo = 0xFFFFFFFFu;
    gRecHi = 0;
    gRecDl = dl;
    gRecKey = *k;
    gRecDepth = depth;
    gRecNs = gRecNv = gRecNt = 0;
    gRecWrote = 0;
    gRecEnvUsed = 0;
    gRecSerial0 = gLoadSerial;
    gRecSegMask = 0;
    gSlotLoaded = 0;
    for (i = 0; i < 32; i++) {
        gSlotRec[i] = -1;
    }
    gDirty = 1;                          /* the first triangle starts a run (it may still join the open batch) */
}

static void rep_insert(REntry *e) {
    u32 h = rep_slot(e->dl);
    e->next = gRTab[h];
    gRTab[h] = e;
}

/* this list can't be replayed: remember that, so it is just interpreted */
static void rec_abort(void) {
    REntry *e;
    if (!gRecOn) {
        return;
    }
    gRecOn = 0;
    gRecDepth = -1;
    e = (REntry *)rep_alloc(sizeof(REntry));
    if (e == NULL) {
        return;
    }
    e->dl = gRecDl;
    e->key = gRecKey;
    e->segMask = 0;
    e->envFree = 0;
    e->checked = 0;
    e->bad = 1;
    gStRepBad++;
    rep_insert(e);
}

static s32 rec_op_ok(u32 op, u32 w0) {
    switch (op) {
    case 0xFB: gRecWrote |= W_ENV; return 1;
    case 0xFC: gRecWrote |= W_COMBINE; return 1;
    case 0xB9: gRecWrote |= W_OML; return 1;
    case 0xBA: gRecWrote |= W_OMH; return 1;
    case 0xBB: gRecWrote |= W_TEXTURE; return 1;
    case 0xFD: gRecWrote |= W_TIMG; return 1;
    case 0xF2: case 0xF5: gRecWrote |= W_TILE; return 1;
    case 0x04: case 0xBF: case 0x06: case 0xB8: case 0xE6: case 0xE7: case 0xE8: case 0xE9:
    case 0xF0: case 0xF3: case 0xF4:
        return 1;
    case 0xBC:
        return (w0 & 0xFF) == 0x0C;      /* G_MW_POINTS */
    default:
        return 0;                        /* matrices, the primitive colour, rectangles... */
    }
}

static void rec_seg(s32 textured, s32 blend, s32 zcmp, s32 zupd, const GfxTex *t) {
    RSeg *s;
    s32 i;
    if (gRecNs >= RS_MAX || (textured && !gTk.ok)) {
        rec_abort();
        return;
    }
    if (!(gRecWrote & W_ENV) && cc_reads_env()) {
        gRecEnvUsed = 1;
    }
    s = &gRecSeg[gRecNs++];
    s->textured = textured;
    s->blend = blend;
    s->zcmp = zcmp;
    s->zupd = zupd;
    s->variant = textured ? (s32)tex_variant() : 0;
    if (textured) {
        s->tex = *t;
        s->tsrc = gTk.src; s->trows = gTk.rows; s->tpitch = gTk.pitch; s->tbytes = gTk.bytes; s->thash = gTk.hash;
        s->psrc = gTk.psrc; s->plen = gTk.plen; s->phash = gTk.phash;
    }
    s->combineHi = G.combineHi; s->combineLo = G.combineLo; s->omH = G.omH; s->envRaw = G.envRaw;
    for (i = 0; i < 4; i++) {
        s->env[i] = G.env[i];
    }
    s->uls = G.tile[G.texTile].uls;
    s->ult = G.tile[G.texTile].ult;
    s->v0 = gRecNv;
    s->t0 = gRecNt;
    for (i = 0; i < 32; i++) {
        gSlotRec[i] = -1;                /* new colours: new recorded vertices */
    }
}

static void rec_tri(s32 i0, s32 i1, s32 i2) {
    s32 idx[3], k;
    idx[0] = i0; idx[1] = i1; idx[2] = i2;
    if (gRecNs == 0 || gRecNt >= RT_MAX) {
        rec_abort();
        return;
    }
    for (k = 0; k < 3; k++) {
        s32 i = idx[k];
        if (!(gSlotLoaded & (1u << i))) {
            rec_abort();
            return;
        }
        if (gSlotRec[i] < 0) {
            RVert *r;
            const RSrc *src = &gSlotSrc[i];
            GfxVert o;
            if (gRecNv >= RV_MAX) {
                rec_abort();
                return;
            }
            out_vert(&o, &G.v[i]);       /* its finished texture coordinates and colour */
            r = &gRecV[gRecNv];
            r->x = src->x; r->y = src->y; r->z = src->z;
            r->s = src->s; r->t = src->t;
            r->r = src->r; r->g = src->g; r->b = src->b; r->a = src->a;
            r->u = o.u; r->v = o.v;
            r->c[0] = o.r; r->c[1] = o.g; r->c[2] = o.b; r->c[3] = o.a;
            gSlotRec[i] = gRecNv++;
        }
        gRecTri[gRecNt * 3 + k] = gSlotRec[i];
    }
    gRecNt++;
}

/* the display list has returned: keep what it drew */
static void rec_end(void) {
    REntry *e;
    s32 i, nl = 0;
    u32 m;

    gRecOn = 0;
    gRecDepth = -1;
    for (i = 0; i < G.numLoads; i++) {
        if (G.loads[i].serial > gRecSerial0) {
            nl++;
        }
    }
    if (nl > RL_MAX || gRecNs == 0) {
        gRecOn = 1;
        rec_abort();
        return;
    }
    e = (REntry *)rep_alloc(sizeof(REntry) + gRecNs * sizeof(RSeg) + gRecNv * sizeof(RVert) +
                            gRecNt * 3 * sizeof(u16));
    if (e == NULL) {
        gStRepPool++;                    /* (too big for the pool: not recorded) */
        return;
    }
    if (gRecNv > gStRepMaxV) gStRepMaxV = gRecNv;
    if (gRecNt > gStRepMaxT) gStRepMaxT = gRecNt;
    if (gRecNs > gStRepMaxS) gStRepMaxS = gRecNs;
    e->dl = gRecDl;
    e->key = gRecKey;
    e->segMask = gRecSegMask;
    for (i = 0, m = gRecSegMask; i < 16; i++) {
        e->segv[i] = (m & (1u << i)) ? G.seg[i] : 0;
    }
    e->prim = G.primRaw;
    e->firstPrim = gFirstPrim;
    e->okFrame = 0;
    e->bad = 0;
    e->wrote = gRecWrote;
    e->checked = gRecChecked;
    e->lo = gRecLo;
    e->hi = gRecHi;
    e->wn = gWriteN;
    e->envFree = !gRecEnvUsed;
    e->ns = gRecNs;
    e->nv = gRecNv;
    e->nt = gRecNt;
    e->seg = (RSeg *)(e + 1);
    e->v = (RVert *)(e->seg + gRecNs);
    e->tri = (u16 *)(e->v + gRecNv);
    for (i = 0; i < gRecNs; i++) {
        e->seg[i] = gRecSeg[i];
        e->seg[i].nv = ((i + 1 < gRecNs) ? gRecSeg[i + 1].v0 : gRecNv) - gRecSeg[i].v0;
        e->seg[i].nt = ((i + 1 < gRecNs) ? gRecSeg[i + 1].t0 : gRecNt) - gRecSeg[i].t0;
    }
    for (i = 0; i < gRecNv; i++) {
        e->v[i] = gRecV[i];
    }
    for (i = 0; i < 3; i++) {
        e->bmin[i] = 1e30f;
        e->bmax[i] = -1e30f;
    }
    for (i = 0; i < gRecNv; i++) {
        const f32 p[3] = { gRecV[i].x, gRecV[i].y, gRecV[i].z };
        s32 j;
        for (j = 0; j < 3; j++) {
            if (p[j] < e->bmin[j]) e->bmin[j] = p[j];
            if (p[j] > e->bmax[j]) e->bmax[j] = p[j];
        }
    }
    for (i = 0; i < gRecNt * 3; i++) {
        e->tri[i] = gRecTri[i];
    }
    e->nl = 0;
    for (i = 0; i < G.numLoads; i++) {
        if (G.loads[i].serial > gRecSerial0) {
            e->loads[e->nl++] = G.loads[i];
        }
    }
    exit_save(&e->ex);
    gStRepRec++;
    rep_insert(e);
}

/* ---- replay ---- */
/* the render state of a run: the combiner's inputs as recorded, the current
 * primitive colour */
static void seg_cc(const RSeg *s) {
    s32 i;
    G.combineHi = s->combineHi;
    G.combineLo = s->combineLo;
    G.omH = s->omH;
    G.envRaw = s->envRaw;
    for (i = 0; i < 4; i++) {
        G.env[i] = s->env[i];
    }
    cc_prepare();
}

/* combine the colours again for another primitive colour (the fade); 0: a
 * texture's variant changed (record again) */
static s32 rep_recolour(REntry *e) {
    u32 hi = G.combineHi, lo = G.combineLo, omH = G.omH, envRaw = G.envRaw;
    f32 env[4];
    s32 textured = gCurTextured, ok = 1, i, j;
    Vert t;

    for (i = 0; i < 4; i++) {
        env[i] = G.env[i];
    }
    for (i = 0; i < e->ns && ok; i++) {
        RSeg *s = &e->seg[i];
        seg_cc(s);
        if (s->textured && (s32)tex_variant() != s->variant) {
            ok = 0;
            break;
        }
        gCurTextured = s->textured;
        for (j = s->v0; j < s->v0 + s->nv; j++) {
            RVert *r = &e->v[j];
            t.r = r->r; t.g = r->g; t.b = r->b; t.a = r->a;
            vert_colour(&t);
            r->c[0] = t.cr; r->c[1] = t.cg; r->c[2] = t.cb; r->c[3] = t.ca;
        }
    }
    G.combineHi = hi; G.combineLo = lo; G.omH = omH; G.envRaw = envRaw;
    for (i = 0; i < 4; i++) {
        G.env[i] = env[i];
    }
    gCurTextured = textured;
    gDirty = 1;
    if (ok) {
        e->prim = G.primRaw;
        e->firstPrim = gFirstPrim;
    }
    return ok;
}

/* 0: replay it, 1: out of date (record again), 2: a texture isn't loaded (interpret) */
static s32 rep_check(REntry *e) {
    s32 i;
    if (e->checked) {
        u32 w;
        if (gWriteN - e->wn > WRITES_N) {
            return 1;
        }
        for (w = e->wn; w != gWriteN; w++) {
            if (gWrites[w & (WRITES_N - 1)].lo < e->hi && gWrites[w & (WRITES_N - 1)].hi > e->lo) {
                return 1;
            }
        }
        e->wn = gWriteN;
    }
    for (i = 0; i < e->ns; i++) {
        RSeg *s = &e->seg[i];
        if (!s->textured) {
            continue;
        }
        if (mem_hash(s->tsrc, s->trows, s->tpitch, s->tbytes) != s->thash ||
            (s->psrc != NULL && mem_hash(s->psrc, 1, 0, s->plen) != s->phash)) {
            return 1;
        }
    }
    /* a texture that isn't loaded: fine if none of its triangles shows (often
     * the case: the normal path loads a texture only when it draws with it) */
    gUnknownN = 0;
    for (i = 0; i < e->ns; i++) {
        if (e->seg[i].textured && !gb_tex_known(e->seg[i].tex.key)) {
            gUnknown[gUnknownN++] = i;
        }
    }
    if ((e->prim != G.primRaw || e->firstPrim != gFirstPrim) && !rep_recolour(e)) {
        return 1;
    }
    return 0;
}

/* would any of this run's triangles be drawn? */
static s32 seg_visible(const REntry *e, const RSeg *s) {
    const u16 *tr = &e->tri[s->t0 * 3];
    s32 i;
    for (i = 0; i < s->nt; i++, tr += 3) {
        const XVert *a = &gXV[tr[0]], *b = &gXV[tr[1]], *c = &gXV[tr[2]];
        s32 c0 = a->code, c1 = b->code, c2 = c->code;
        if (c0 & c1 & c2) {
            continue;
        }
        if ((G.geom & 0x3000) && !((c0 | c1 | c2) & 1)) {
            f32 area = (b->nx - a->nx) * (c->ny - a->ny) - (c->nx - a->nx) * (b->ny - a->ny);
            if ((G.geom & 0x2000) && area < 0) {
                continue;
            }
            if ((G.geom & 0x1000) && area > 0) {
                continue;
            }
        }
        if ((c0 | c1 | c2) == 0 &&
            ((a->nx > 1.0f && b->nx > 1.0f && c->nx > 1.0f) || (a->nx < -1.0f && b->nx < -1.0f && c->nx < -1.0f) ||
             (a->ny > 1.0f && b->ny > 1.0f && c->ny > 1.0f) || (a->ny < -1.0f && b->ny < -1.0f && c->ny < -1.0f))) {
            continue;
        }
        return 1;
    }
    return 0;
}

/* 0: not replayed (a run with a texture that isn't loaded would show) */
/* the backend's run open for the replay's triangles (build 74): a run of 2-3
 * triangles is typical, and starting one (a polygon header) costs more than its
 * triangles, so a run is started only once one of its triangles shows, and one
 * drawn the same way as the run before it (same texture and modes, nothing else
 * sent in between) carries on under that run's header */
typedef struct RunOpen {
    s32 on, textured, blend, zcmp, zupd;
    u32 key;
    s32 wrapS, wrapT;
    GbPrim gp;
} RunOpen;

static s32 seg_open(const REntry *e, const RSeg *s, RunOpen *o) {
    s32 j;
    /* as state_update */
    if (s->textured == gCurTextured && s->blend == gCurBlend && s->zcmp == gCurZcmp && s->zupd == gCurZupd &&
        (!s->textured || (s->tex.key == gCurTex.key && s->tex.wrapS == gCurTex.wrapS && s->tex.wrapT == gCurTex.wrapT))) {
        gGfxMerged++;
    } else {
        flush();
        gGfxBatches++;
        gCurTextured = s->textured;
        gCurBlend = s->blend;
        gCurZcmp = s->zcmp;
        gCurZupd = s->zupd;
        gNeedDecode = 0;                 /* checked: loaded */
        gSkipBatch = 0;
        gCurBand.base = NULL;
        if (s->textured) {
            gCurTex = s->tex;
            gCurVariant = s->variant;
        }
    }
    gUOff = 0.0f;
    gColStamp++;

    if (!(o->on && o->textured == s->textured && o->blend == s->blend && o->zcmp == s->zcmp && o->zupd == s->zupd &&
          (!s->textured || (o->key == s->tex.key && o->wrapS == s->tex.wrapS && o->wrapT == s->tex.wrapT)))) {
        /* the direct path: what came before goes first */
        flush();
        o->on = 0;
        if (!gb_prim_begin(s->textured ? &s->tex : NULL, s->blend, s->zcmp, s->zupd, &o->gp)) {
            return 0;
        }
        o->on = 1;
        o->textured = s->textured;
        o->blend = s->blend;
        o->zcmp = s->zcmp;
        o->zupd = s->zupd;
        o->key = s->tex.key;
        o->wrapS = s->tex.wrapS;
        o->wrapT = s->tex.wrapT;
    }
    /* the run's vertices made ready for the backend once each */
    if (s->textured && (o->gp.us != 1.0f || o->gp.vs != 1.0f)) {
        for (j = s->v0; j < s->v0 + s->nv; j++) {
            const RVert *r = &e->v[j];
            GbPVert *p = &gPV[j];
            p->u = r->u * o->gp.us;
            p->v = r->v * o->gp.vs;
        }
    }
    return 1;
}

static s32 rep_run(REntry *e) {
    const f32 (*m)[4];
    f32 sx = G.vpScale[0], tx = G.vpTrans[0], sy = G.vpScale[1], ty = G.vpTrans[1];
    f32 xys = gb_prim_xyscale();         /* (gb_prim_begin's xs, ys: the same for every run) */
    s32 i, k;
    u32 t0 = TNOW();
    /* what the clipped triangles' state changes (put back afterwards) */
    u32 hi = G.combineHi, lo = G.combineLo, omH = G.omH, envRaw = G.envRaw;
    f32 env[4];
    u16 uls = G.tile[G.texTile].uls, ult = G.tile[G.texTile].ult;

    env[0] = G.env[0]; env[1] = G.env[1]; env[2] = G.env[2]; env[3] = G.env[3];
    if (G.mvpDirty) {
        mtx_mul(&G.mvp, &G.mv[G.mvTop], &G.proj);
        G.mvpDirty = 0;
    }
    m = G.mvp.m;
#ifdef REP_FTRV
    {   /* mat_load reads the matrix 8 bytes at a time: it must be 8-byte aligned
         * (G.mvp isn't; the real console stops on an unaligned read, emulators don't) */
        static Mtx4 aligned __attribute__((aligned(32)));
        aligned = G.mvp;
        mat_load(aligned.m);
    }
#endif
    /* A mesh entirely off the screen (all its box's corners beyond one edge of the
     * view, or behind the camera) draws nothing: its vertices aren't worth working
     * out. About a quarter of the meshes replayed on San Francisco, Death Valley and
     * Washington DC. Each test is a plane in clip space, so a box whose corners are
     * all beyond it is all beyond it. (Small meshes: not worth the eight corners.) */
    if (e->nv >= 12) {
        s32 all = 0x1F, c;
        for (c = 0; c < 8; c++) {
            f32 x = (c & 1) ? e->bmax[0] : e->bmin[0];
            f32 y = (c & 2) ? e->bmax[1] : e->bmin[1];
            f32 z = (c & 4) ? e->bmax[2] : e->bmin[2];
            f32 cx, cy, cz, cw;
#ifdef REP_FTRV
            register f32 fx __asm__("fr0") = x;
            register f32 fy __asm__("fr1") = y;
            register f32 fz __asm__("fr2") = z;
            register f32 fw __asm__("fr3") = 1.0f;
            __asm__ __volatile__("ftrv xmtrx,fv0" : "+f"(fx), "+f"(fy), "+f"(fz), "+f"(fw));
            cx = fx; cy = fy; cz = fz; cw = fw;
#else
            cx = x * m[0][0] + y * m[1][0] + z * m[2][0] + m[3][0];
            cy = x * m[0][1] + y * m[1][1] + z * m[2][1] + m[3][1];
            cz = x * m[0][2] + y * m[1][2] + z * m[2][2] + m[3][2];
            cw = x * m[0][3] + y * m[1][3] + z * m[2][3] + m[3][3];
#endif
            all &= (cw + cz <= 0.0f) | ((cx > cw) << 1) | ((-cx > cw) << 2) | ((cy > cw) << 3) | ((-cy > cw) << 4);
            if (all == 0) {
                break;
            }
        }
        if (all != 0) {
            gStRepCulled++;
            gGfxProf[2] += TNOW() - t0;
            goto done;                   /* (what the list leaves behind still applies) */
        }
    }
    /* the vertices: as cmd_vtx, vert_project and out_vert */
    for (i = 0; i < e->nv; i++) {
        const RVert *r = &e->v[i];
        XVert *d = &gXV[i];
        f32 q, gw;
#ifdef REP_FTRV
        {
            register f32 fx __asm__("fr0") = r->x;
            register f32 fy __asm__("fr1") = r->y;
            register f32 fz __asm__("fr2") = r->z;
            register f32 fw __asm__("fr3") = 1.0f;
            __asm__ __volatile__("ftrv xmtrx,fv0" : "+f"(fx), "+f"(fy), "+f"(fz), "+f"(fw));
            d->x = fx;
            d->y = fy;
            d->z = fz;
            d->w = fw;
        }
#else
        f32 x = r->x, y = r->y, z = r->z;
        d->x = x * m[0][0] + y * m[1][0] + z * m[2][0] + m[3][0];
        d->y = x * m[0][1] + y * m[1][1] + z * m[2][1] + m[3][1];
        d->z = x * m[0][2] + y * m[1][2] + z * m[2][2] + m[3][2];
        d->w = x * m[0][3] + y * m[1][3] + z * m[2][3] + m[3][3];
#endif
#ifdef REP_FTRV
        {   /* 1/w with the square-root reciprocal unit (a division takes ~10 times as long) */
            f32 w = d->w, w2 = w * w;
            __asm__("fsrra %0" : "+f"(w2));
            q = (w > 0.0f) ? w2 : (w < 0.0f) ? -w2 : 0.0f;
        }
#else
        q = (d->w != 0.0f) ? 1.0f / d->w : 0.0f;
#endif
        d->nx = d->x * q;
        d->ny = d->y * q;
        gw = 2.0f * d->w;
        d->code = (d->w + d->z <= 0.0f) | ((gw - d->x <= 0.0f) << 1) | ((gw + d->x <= 0.0f) << 2) |
                  ((gw - d->y <= 0.0f) << 3) | ((gw + d->y <= 0.0f) << 4);
        {   /* and ready for the backend, in the same pass (the vertex is in the cache now;
             * a second pass over them all was ~9% of a slow frame on San Francisco).
             * u, v: the run's texture scale is put on below, for the runs that need it */
            GbPVert *p = &gPV[i];
            p->x = (d->nx * sx + tx) * xys;
            p->y = (-d->ny * sy + ty) * xys;
            p->z = (d->z * q) * 0.5f + 0.5f;
            p->q = q;
            p->u = r->u;
            p->v = r->v;
            p->argb = ((u32)r->c[3] << 24) | ((u32)r->c[0] << 16) | ((u32)r->c[1] << 8) | r->c[2];
        }
    }
    gGfxProf[2] += TNOW() - t0;
    for (k = 0; k < gUnknownN; k++) {
        if (seg_visible(e, &e->seg[gUnknown[k]])) {
            return 0;
        }
    }

    {
    u32 tL = TNOW(), bL = gGfxProf[1], cL = gRpUs[1];
    RunOpen open;
    open.on = 0;
    for (k = 0; k < e->ns; k++) {
        const RSeg *s = &e->seg[k];
        const u16 *tr = &e->tri[s->t0 * 3];
        s32 clipReady = 0, begun = 0;

        if (s->textured && gUnknownN > 0 && !gb_tex_known(s->tex.key)) {
            continue;                        /* nothing of it shows (checked above) */
        }
        for (i = 0; i < s->nt; i++, tr += 3) {
            const XVert *a = &gXV[tr[0]], *b = &gXV[tr[1]], *c = &gXV[tr[2]];
            s32 c0 = a->code, c1 = b->code, c2 = c->code;
            if (c0 & c1 & c2) {
                continue;
            }
            if ((G.geom & 0x3000) && !((c0 | c1 | c2) & 1)) {
                f32 area = (b->nx - a->nx) * (c->ny - a->ny) - (c->nx - a->nx) * (b->ny - a->ny);
                if ((G.geom & 0x2000) && area < 0) {
                    continue;
                }
                if ((G.geom & 0x1000) && area > 0) {
                    continue;
                }
            }
            if ((c0 | c1 | c2) == 0 &&
                ((a->nx > 1.0f && b->nx > 1.0f && c->nx > 1.0f) || (a->nx < -1.0f && b->nx < -1.0f && c->nx < -1.0f) ||
                 (a->ny > 1.0f && b->ny > 1.0f && c->ny > 1.0f) || (a->ny < -1.0f && b->ny < -1.0f && c->ny < -1.0f))) {
                continue;
            }
            if (!begun) {                    /* the run's first triangle that shows: start it */
                if (!seg_open(e, s, &open)) {
                    break;                   /* (nothing drawn: the profiler's screen) */
                }
                begun = 1;
            }
            if ((c0 | c1 | c2) == 0) {
                gb_prim_tri(&gPV[tr[0]], &gPV[tr[1]], &gPV[tr[2]]);
                gRpTriN++;
            } else {
                Vert v[3];
                const Vert *in[3];
                s32 j;
                u32 tc = TNOW();
                gRpClipN++;
                if (!clipReady) {            /* the normal path's state for this run */
                    seg_cc(s);
                    G.tile[G.texTile].uls = s->uls;
                    G.tile[G.texTile].ult = s->ult;
                    clipReady = 1;
                }
                for (j = 0; j < 3; j++) {
                    const XVert *x = &gXV[tr[j]];
                    const RVert *r = &e->v[tr[j]];
                    v[j].x = x->x; v[j].y = x->y; v[j].z = x->z; v[j].w = x->w;
                    v[j].s = r->s; v[j].t = r->t;
                    v[j].r = r->r; v[j].g = r->g; v[j].b = r->b; v[j].a = r->a;
                    vert_project(&v[j]);
                    in[j] = &v[j];
                }
                tri_clip(in, c0, c1, c2);
                flush();                     /* (in order with the direct ones) */
                open.on = 0;                 /* (the normal path sent its own headers) */
                gRpUs[1] += TNOW() - tc;
            }
        }
    }
    gRpUs[3] += (TNOW() - tL) - (gGfxProf[1] - bL) - (gRpUs[1] - cL);
    }
    gStTris += e->nt;

done:
    /* what the list leaves behind */
    G.combineHi = hi; G.combineLo = lo; G.omH = omH; G.envRaw = envRaw;
    G.env[0] = env[0]; G.env[1] = env[1]; G.env[2] = env[2]; G.env[3] = env[3];
    G.tile[G.texTile].uls = uls;
    G.tile[G.texTile].ult = ult;
    {
    u32 tl = TNOW();
    exit_load(&e->ex, e->wrote);
    for (i = 0; i < e->nl; i++) {
        const struct TmemLoad *l = &e->loads[i];
        struct TmemLoad *n = tmem_load(l->dst, l->line, l->rows, l->src, l->srcPitch, l->rowBytes);
        n->rowBase = l->rowBase;
        n->x0 = l->x0;
        n->imgW = l->imgW;
    }
    gRpUs[2] += TNOW() - tl;
    }
    gDirty = 1;
    gColStamp++;
    return 1;
}

/* G_DL of a scenery mesh: 1 if it was replayed, 0 if it is to be interpreted
 * (and perhaps recorded) */
static s32 segs_same(const REntry *e) {
    u32 m = e->segMask;
    s32 i;
    for (i = 0; m != 0; i++, m >>= 1) {
        if ((m & 1) && G.seg[i] != e->segv[i]) {
            return 0;
        }
    }
    return 1;
}

static s32 rep_draw(const u32 *dl, s32 depth, s32 checked) {
    RKey k;
    REntry *e;
    s32 n = 0;

    if (!gRInit) {
        rep_init();
    }
    if (gRPool == NULL || gRepOff || gGfxTrace || gOffscreen || gRecOn) {
        return 0;
    }
    rkey_now(&k);
    for (e = gRTab[rep_slot(dl)]; e != NULL; e = e->next) {
        if (e->dl == dl) {
            n++;
            if (rkey_same(&e->key, &k, e->envFree) && segs_same(e)) {
                break;
            }
        }
    }
    if (e != NULL) {
        e = rep_keep(e);
    }
    if (e == NULL) {
        if (n < 8) {
            rec_begin(dl, &k, depth, checked);
        }
        gStRepInterp++;
        return 0;
    }
    if (e->bad) {
        gStRepInterp++;
        return 0;
    }
    {
    u32 tk = TNOW();
    s32 ck;
    if (e->okFrame == gGfxFrameNo + 1 && !e->checked) {   /* (lists in ordinary memory: always) */
        /* already checked this frame (the same model again: the rival cars, drawn one after another):
         * its memory and textures can't have changed since, so only the colour is looked at */
        gUnknownN = 0;
        ck = ((e->prim != G.primRaw || e->firstPrim != gFirstPrim) && !rep_recolour(e)) ? 1 : 0;
    } else {
        ck = rep_check(e);
        if (ck == 0 && gUnknownN == 0) {
            e->okFrame = gGfxFrameNo + 1;           /* (+1: 0 is never a frame it was checked in) */
        }
    }
    gRpUs[0] += TNOW() - tk;
    switch (ck) {
    case 0:
        if (rep_run(e)) {
            gStRepHit++;
            return 1;
        }
        break;
    case 1:
        port_dl_compiled(dl);            /* drop it (and the others for this list) */
        rec_begin(dl, &k, depth, checked);
        break;
    default:
        break;
    }
    }
    gStRepInterp++;
    return 0;
}
