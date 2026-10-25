/*
 * meshcomp.c - the run-time model converter and its cache.
 * Original code: main segment, ROM 0x1C710-0x1E120 and 0x1E3E0-0x1EBD0
 * (RAM 0x8011B710-0x8011D120, 0x8011D3E0-0x8011DBD0).
 *
 * Track scenery is stored as compact "mesh files" and turned into RSP display
 * lists only when a section comes into view (meshCacheGet). The compiled
 * lists and vertices live in a pool of 256-byte pages carved from free heap
 * memory; a page's last word links to the next page (physical address).
 * Display-list pages hold 31 commands plus a G_DL branch in the last slot;
 * vertex pages hold 16 vertices (the link overlaps the unused colour of the
 * 16th). Released meshes are recycled once the frame that last drew them has
 * been shown (meshCacheCollect).
 *
 * Mesh file (see GroundMesh in ground.h):
 *   u8 lastVert, u8 lastUV, s16 numRecords, s16 dlCmds, s16 numOutVerts,
 *   MeshVert verts[lastVert + 1]   (s16 x, y, z)
 *   s16 uv[lastUV + 1][2]          (u, v)
 *   6-byte records: a group header (texture file or RGBA colour, face count,
 *   flags) followed by its faces (u8 v0, v1, v2, uv0, uv1, uv2).
 * dlCmds / numOutVerts are filled in at load time by modelFixup() and give the
 * size of a pre-compiled copy (asset kind 2, compiled straight into memory).
 *
 * Group flags: 4 = untextured (the record holds an RGBA colour), 1 = combine
 * mode choice, 2 / 8 = S wrap / mirror, 0x20 / 0x10 = T wrap / mirror
 * (otherwise clamp).
 * Texture file: u8 width / 4, u8 height, s16 flags (bit 0: has palette),
 * u32 palette (physical address once loaded), then the texels.
 */
#include "common.h"
#include "race.h"
#include "road.h"
#include "scenery.h"

typedef struct TexState {
    /* 0x00 */ u32 tex;                       /* texel address (KSEG0) */
    /* 0x04 */ u32 color;                     /* RGBA environment colour (untextured groups) */
    /* 0x08 */ s32 flags;                     /* group flags */
} TexState;

typedef struct MeshVtx {
    s16 x, y, z;
} MeshVtx;

extern u32 *gMeshDL;                     /* 0x80158270 - result of the last modelCompile */
extern u8  *gMeshVtx;                    /* 0x80158274 */
extern u8  *gPagePool;                   /* 0x801582E0 - first page (slots are counted from here) */
extern u8  *gPageFree;                   /* 0x801582E4 - free page chain */
extern s32  gPageBytes;                  /* 0x801582E8 - size of the pool */
extern s32  gPagesFree;                  /* 0x801582EC */
extern MeshCache gMeshCache[512];        /* 0x801582F0 */
extern MeshCacheList gMeshUsed;          /* 0x8015BAF0 */
extern MeshCacheList gMeshFree;          /* 0x8015BAF8 */
extern MeshCacheList gMeshReleased;      /* 0x8015BB00 - waiting for the GPU to finish with them */
extern u32  gHeapPtr;                    /* 0x8014658C */
extern u32  gFramebuffersEnd;            /* 0x801465F4 */
extern u32  gLastShownFrame;             /* 0x8014769C */

u32  assetLoad(s32 id);                  /* 0x80000480 */
s32  heapFree(s32 area);                 /* 0x80102EC4 */
void fatalError(s32 code, char *msg);    /* 0x80104550 */

s32  meshRadius2(MeshVtx *v, s32 n, f32 scale); /* 0x8011C6F0 */
u32 *texStateEmit(TexState *prev, TexState *cur, s32 width, s32 height, u32 palette, u32 *dl, s32 paged); /* 0x8011C77C */
u32 *dlReserve(u32 *dl, s32 n);          /* 0x8011D0C8 */
u32 *dlPageAlloc(void);                  /* 0x8011D738 */
u32 *dlPageBranch(u32 *dl);              /* 0x8011D794 */
u8  *vtxPageAlloc(void);                 /* 0x8011D8EC */
u8  *vtxPageNext(u8 *vtx);               /* 0x8011D948 */
void dlEnd(u32 *dl);                     /* 0x8011D8B0 */
void vtxEnd(u8 *vtx);                    /* 0x8011DA44 */
void dlPagesFree(u32 *dl);               /* 0x8011D80C */
void vtxPagesFree(u8 *vtx);              /* 0x8011D9A0 */
void dlWalk(u32 *dl);                    /* 0x8011DB28 */
u8  *pageNext(u8 *page);                 /* 0x8011DB94 */
void heapExhausted(void);                /* 0x8011DBA8 */
void modelCompile(u8 *mesh, u32 *out, f32 *radius2); /* 0x8011B710 */

#define PHYS(a)      ((u32)(a) + 0x80000000)   /* KSEG0 <-> physical */
#define PAGE_LINK(p) (*(u32 *)((u8 *)(p) + 0xFC))
#define G_VTX_N(n)   (0x04000000 | ((((n) - 1) * 16) & 0xFF) << 16 | (((n) * 16) & 0xFFFF))

static TexState sTexInitCompile = { 0, 0xFFFFFFFF, 1 };   /* 0x80134E00 */
static TexState sTexInitFixup   = { 0, 0xFFFFFFFF, 1 };   /* 0x80134E0C */

/* the record list follows the vertices and uv pairs */
#define MESH_UVS(m)     ((s16 *)((m) + 8 + ((m)[0] + 1) * 6))
#define MESH_RECORDS(m) ((u8 *)MESH_UVS(m) + ((m)[1] + 1) * 4)

/* write the vertices of a finished batch: position, then the uv the vertex
 * was first loaded with (later changes are G_MOVEWORDs) */
static void vtxBatchWrite(u8 *dst, u8 *mesh, s16 *uvs, s32 *cacheVert, s32 *cacheUV, s32 n) {
    s32 j;
    u16 *src, *d;

    for (j = 0; j < n; j++, dst += 16) {
        d = (u16 *)dst;
        src = (u16 *)(mesh + 8 + cacheVert[j] * 6);
        d[0] = src[0]; d[1] = src[1]; d[2] = src[2];
        src = (u16 *)&uvs[cacheUV[j] * 2];
        d[4] = src[0]; d[5] = src[1];
    }
}

/* 0x8011B710 - compile a mesh file into a display list. out != NULL: the list
 * and then the vertices go there (sized by modelFixup); out == NULL: into
 * pages of the pool. Up to 16 vertices are loaded at a time; a vertex used
 * again with another uv gets its texture coordinates patched in place.
 * radius2: max x^2 + z^2 of the vertices (scaled by 0.2), + 1. */
void modelCompile(u8 *mesh, u32 *out, f32 *radius2) {
    TexState prev, cur;
    s32 cacheVert[17], cacheUV[17], curUV[17], tri[3];
    s32 numRecords, rec, numFaces, face, k, j, n, start, idx, flags, off, paged;
    u32 width, height, palette, value;
    u32 *dl, *dlStart, *vtxCmd;
    u8 *vtx, *batch, *p, *t;
    s16 *uvs, *st;

    prev = sTexInitCompile;
    cur = prev;
    numRecords = *(s16 *)(mesh + 2);
    uvs = MESH_UVS(mesh);
    p = MESH_RECORDS(mesh);
    if (out != NULL) {
        gMeshDL = dl = dlStart = out;
        gMeshVtx = vtx = (u8 *)(out + *(s16 *)(mesh + 4) * 2);
    } else {
        gMeshDL = dl = dlPageAlloc();
        gMeshVtx = vtx = vtxPageAlloc();
    }
    if (radius2 != NULL) {
        *radius2 = (f32)meshRadius2((MeshVtx *)(mesh + 8), mesh[0] + 1, 0.2f);
    }
    n = 0;
    batch = vtx;
    paged = (out == NULL);
    off = p - mesh;
    for (rec = 0; rec < numRecords; rec += numFaces + 1) {
        /* group header: the u32 is kept 4-byte aligned */
        if (!(off & 2)) {
            numFaces = p[4];
            value = *(u32 *)p;
            flags = p[5];
        } else {
            numFaces = p[0];
            value = LOAD_U32_UNALIGNED(p + 2);
            flags = p[1];
        }
        cur.flags = flags;
        off += 6;
        p += 6;
        if (flags & 4) {
            cur.color = value;
        } else {
            t = (u8 *)value;
            width = t[0] * 4;
            height = t[1];
            palette = (*(s16 *)(t + 2) & 1) ? *(u32 *)(t + 4) : 0;
            cur.tex = (u32)(t + 8);
        }
        dl = texStateEmit(&prev, &cur, width, height, palette, dl, paged);
        prev = cur;

        for (face = 0; face < numFaces; face++) {
            start = n;
            for (k = 0; k < 3; k++) {
                s32 v = p[k];
                s32 uv = p[k + 3];

                for (j = 0; j < start; j++) {
                    if (v == cacheVert[j] && uv == curUV[j]) {
                        idx = j;
                        break;
                    }
                }
                if (j == start) {
                    for (j = 0; j < start; j++) {
                        if (v == cacheVert[j]) {
                            curUV[j] = uv;
                            idx = j;
                            if (paged) {
                                dl = dlReserve(dl, 1);
                            }
                            st = &uvs[curUV[j] * 2];
                            dl[0] = 0xBC00000C | ((j * 40 + 0x14) & 0xFFFF) << 8;   /* G_MOVEWORD: vertex ST */
                            dl[1] = ((st[1] >> 1) & 0xFFFF) | ((st[0] << 15) & 0xFFFF0000);
                            dl += 2;
                            break;
                        }
                    }
                }
                if (j == start) {
                    cacheVert[n] = v;
                    cacheUV[n] = uv;
                    curUV[n] = uv;
                    idx = n;
                    if (n++ == 0) {
                        if (paged) {
                            dl = dlReserve(dl, 1);
                        }
                        vtxCmd = dl;
                        dl += 2;
                        vtxCmd[0] = 0x04F00000;          /* G_VTX, filled in when the batch is full */
                        vtxCmd[1] = 0;
                    }
                    vtx += 16;
                    if (n >= 17) {
                        /* batch full: drop this face's new vertices, flush, redo the face */
                        vtx -= (n - start) * 16;
                        if (paged) {
                            vtx = vtxPageNext(vtx - 16);
                        }
                        vtxCmd[0] = G_VTX_N(start);
                        vtxCmd[1] = PHYS(batch);
                        n = 0;
                        vtxBatchWrite(batch, mesh, uvs, cacheVert, cacheUV, start);
                        batch = vtx;
                        start = 0;
                        k = -1;
                        continue;
                    }
                }
                tri[k] = idx;
            }
            if (paged) {
                dl = dlReserve(dl, 1);
            }
            dl[0] = 0xBF000000;                          /* G_TRI1 */
            dl[1] = ((tri[2] * 10) & 0xFF) | ((tri[0] * 10) & 0xFF) << 16 | ((tri[1] * 10) & 0xFF) << 8;
            dl += 2;
            off += 6;
            p += 6;
        }
    }
    if (n != 0) {
        vtxCmd[0] = G_VTX_N(n);
        vtxCmd[1] = PHYS(batch);
        vtxBatchWrite(batch, mesh, uvs, cacheVert, cacheUV, n);
    }
    /* back to the default state */
    cur.flags = 1;
    cur.tex = prev.tex;
    dl = texStateEmit(&prev, &cur, 8, 8, 0, dl, paged);
    if (paged) {
        dl = dlReserve(dl, 1);
    }
    dl[0] = 0xBA000E02;                                  /* no texture LUT */
    dl[1] = 0;
    dl += 2;
    if (paged) {
        dlEnd(dl);
        dl += 2;
        vtxEnd(vtx - 16);
    } else {
        dl[0] = 0xB8000000;                              /* G_ENDDL */
        dl[1] = 0;
        dl += 2;
    }
    if (out != NULL &&
        *(s16 *)(mesh + 4) * 8 + *(s16 *)(mesh + 6) * 16 !=
            ((s32)(vtx - (u8 *)dl) >> 4) * 16 + ((s32)((u8 *)dl - (u8 *)dlStart) >> 3) * 8) {
        dlWalk(dlStart);                                 /* size mismatch (debug leftover) */
    }
}

/* 0x8011C21C - count what modelCompile will emit for this mesh and store it
 * in the header (dlCmds, numOutVerts) */
void modelFixup(u8 *mesh) {
    TexState prev, cur;
    s32 cacheVert[17], curUV[17], tri[3];
    s32 numRecords, rec, numFaces, face, k, j, n, start, idx, flags, off, cmds;
    s16 verts;
    u32 width, height, palette, value;
    u8 *p, *t;

    cmds = 0;
    verts = 0;
    n = 0;
    prev = sTexInitFixup;
    cur = prev;
    numRecords = *(s16 *)(mesh + 2);
    p = MESH_RECORDS(mesh);
    off = p - mesh;
    for (rec = 0; rec < numRecords; rec += numFaces + 1) {
        if (!(off & 2)) {
            numFaces = p[4];
            value = *(u32 *)p;
            flags = p[5];
        } else {
            numFaces = p[0];
            value = LOAD_U32_UNALIGNED(p + 2);
            flags = p[1];
        }
        cur.flags = flags;
        off += 6;
        if (flags & 4) {
            cur.color = value;
        } else {
            t = (u8 *)value;
            palette = 0;
            width = t[0] * 4;
            height = t[1];
            if (*(s16 *)(t + 2) & 1) {
                palette = *(u32 *)(t + 4);
            }
            cur.tex = (u32)(t + 8);
        }
        p += 6;
        cmds += (u32)texStateEmit(&prev, &cur, width, height, palette, NULL, 0) >> 3;
        prev = cur;

        for (face = 0; face < numFaces; face++) {
            start = n;
            for (k = 0; k < 3; k++) {
                s32 v = p[k];
                s32 uv = p[k + 3];

                for (j = 0; j < start; j++) {
                    if (v == cacheVert[j] && uv == curUV[j]) {
                        idx = j;
                        break;
                    }
                }
                if (j == start) {
                    for (j = 0; j < start; j++) {
                        if (v == cacheVert[j]) {
                            idx = j;
                            curUV[j] = uv;
                            cmds++;
                            break;
                        }
                    }
                }
                if (j == start) {
                    idx = n;
                    cacheVert[n] = v;
                    curUV[n] = uv;
                    if (n++ == 0) {
                        cmds++;
                    }
                    verts++;
                    if (n >= 17) {
                        verts = verts - n + start;
                        start = 0;
                        n = 0;
                        k = -1;
                        continue;
                    }
                }
                tri[k] = idx;
            }
            p += 6;
            off += 6;
            cmds++;
        }
    }
    cur.flags = 1;
    cur.tex = prev.tex;
    *(s16 *)(mesh + 4) = cmds + ((u32)texStateEmit(&prev, &cur, 8, 8, 0, NULL, 0) >> 3) + 2;
    *(s16 *)(mesh + 6) = verts;
}

/* 0x8011C59C - replace the texture file ids of a loaded mesh by addresses
 * (and load the palettes they use) */
void modelRelocate(u8 *mesh) {
    s32 numRecords = *(s16 *)(mesh + 2);
    s32 rec, count;
    u8 *p = MESH_RECORDS(mesh);
    u8 *field, flags;
    u32 *tex;

    for (rec = 0; rec < numRecords; rec += count + 1) {
        field = p + 2;
        if (!((p - mesh) & 2)) {
            count = p[4];
            field = p;
            flags = p[5];
        } else {
            count = p[0];
            flags = p[1];
        }
        if (!(flags & 4)) {
            tex = (u32 *)(assetLoad(LOAD_U32_UNALIGNED(field)) + 0x80000000);
            STORE_U32_UNALIGNED(field, tex);
            if ((*(s16 *)((u8 *)tex + 2) & 1) && (s32)tex[1] < 0x10000) {   /* 8-bit: palette id -> address */
                tex[1] = assetLoad(tex[1]);
            }
        }
        p += count * 6 + 6;
    }
}

/* 0x8011C6C8 - floor(log2(x)) (0 for 0) */
s32 log2u(u32 x) {
    s32 n = 0;

    for (x >>= 1; x != 0; x >>= 1) {
        n++;
    }
    return n;
}

/* 0x8011C6F0 */
s32 meshRadius2(MeshVtx *v, s32 n, f32 scale) {
    s32 max = 0;
    s32 x, z, d;

    for (; n != 0; n--, v++) {
        x = (s32)(v->x * scale);
        z = (s32)(v->z * scale);
        d = x * x + z * z;
        if (max < d) {
            max = d;
        }
    }
    return max + 1;
}

#define RESERVE(k)   if (paged) { dl = dlReserve(dl, k); }
#define EMIT(a, b)   if (dl != NULL) { dl[0] = (a); dl[1] = (b); dl += 2; } else { cnt++; }

/* 0x8011C77C - emit the RDP state changes from prev to cur: colour combiner,
 * environment colour or texture load (16-bit RGBA, or 8-bit colour index with
 * its 256-entry palette). dl == NULL only counts: returns (u32 *)(count * 8).
 * paged: dl is in the page pool (keep room for the branch to the next page). */
u32 *texStateEmit(TexState *prev, TexState *cur, s32 width, s32 height, u32 palette, u32 *dl, s32 paged) {
    s32 cnt = 0;
    s32 synced = 0;
    s32 diff = cur->flags ^ prev->flags;
    s32 prevSolid = prev->flags & 4;
    s32 cms, cmt, maskS, maskT, lrs, words, div;
    u32 tile;

    if (cur->flags & 4) {
        if (!prevSolid) {
            RESERVE(1);
            EMIT(0xE7000000, 0);                         /* gDPPipeSync */
            synced = 1;
            RESERVE(1);
            EMIT(0xFC51D6A3, 0xFFAFFFFF);                /* combine: environment colour */
        }
        if (!synced) {
            RESERVE(1);
            EMIT(0xE7000000, 0);
        }
        RESERVE(1);
        EMIT(0xFB000000, cur->color);                    /* gDPSetEnvColor */
    } else {
        if ((diff & 1) || prevSolid) {
            RESERVE(1);
            EMIT(0xE7000000, 0);
            synced = 1;
            RESERVE(1);
            if (cur->flags & 1) {
                EMIT(0xFC119623, 0xFF2FFFFF);            /* combine: texture x shade */
            } else {
                EMIT(0xFC11FE23, 0xFFFFF7FB);            /* combine: texture */
            }
        }
        if (prev->tex != cur->tex || (diff & 0x3A)) {
            if (!synced) {
                RESERVE(1);
                EMIT(0xE7000000, 0);
            }
            cms = 2;
            cmt = 2;
            maskT = 0;
            if (cur->flags & 0x3A) {
                maskS = log2u(width);
                maskT = log2u(height);
                if (cur->flags & 2) {
                    cms = 0;
                }
                if (cur->flags & 0x20) {
                    cmt = 0;
                }
                if (cur->flags & 8) {
                    cms = 1;
                }
                if (cur->flags & 0x10) {
                    cmt = 1;
                }
            } else {
                maskS = 0;
            }
            tile = (cmt & 3) << 18 | (maskT & 0xF) << 14 | (cms & 3) << 8 | (maskS & 0xF) << 4;
            if (palette != 0) {
                RESERVE(14);
                if (dl != NULL) {
                    dl[0] = 0xFD500000;                  /* set texture image: CI, 16-bit units */
                    dl[1] = cur->tex;
                    dl[2] = 0xF5500000;
                    dl[3] = 0x07000000 | tile;
                    dl[4] = 0xE6000000;                  /* load sync */
                    dl[5] = 0;
                    lrs = ((width * height + 1) >> 1) - 1;
                    if (lrs >= 0x7FF) {
                        lrs = 0x7FF;
                    }
                    words = width / 8;
                    div = (words <= 0) ? 1 : words;
                    dl[6] = 0xF3000000;                  /* load block */
                    dl[7] = 0x07000000 | (lrs & 0xFFF) << 12 | (((((words <= 0) ? 1 : words) + 0x7FF) / div) & 0xFFF);
                    dl[8] = 0xE7000000;
                    dl[9] = 0;
                    dl[10] = 0xF5480000 | (((width + 7) >> 3) & 0x1FF) << 9;
                    dl[11] = tile;
                    dl[12] = 0xF2000000;                 /* set tile size */
                    dl[13] = (((width - 1) * 4) & 0xFFF) << 12 | (((height - 1) * 4) & 0xFFF);
                    dl[14] = 0xFD100000;                 /* palette */
                    dl[15] = palette + ((*(s32 *)(palette + 0x80000004) >= 2) ? 0x03000008 : 8);
                    dl[16] = 0xE8000000;                 /* tile sync */
                    dl[17] = 0;
                    dl[18] = 0xF5000100;
                    dl[19] = 0x07000000;
                    dl[20] = 0xE6000000;
                    dl[21] = 0;
                    dl[22] = 0xF0000000;                 /* load TLUT, 256 colours */
                    dl[23] = 0x073FC000;
                    dl[24] = 0xE7000000;
                    dl[25] = 0;
                    dl[26] = 0xBA000E02;                 /* TLUT RGBA16 */
                    dl[27] = 0x8000;
                    dl += 28;
                } else {
                    cnt += 14;
                }
            } else {
                RESERVE(8);
                if (dl != NULL) {
                    dl[0] = 0xBA000E02;                  /* no TLUT */
                    dl[1] = 0;
                    dl[2] = 0xFD100000;                  /* set texture image: RGBA16 */
                    dl[3] = cur->tex;
                    dl[4] = 0xF5100000;
                    dl[5] = 0x07000000 | tile;
                    dl[6] = 0xE6000000;
                    dl[7] = 0;
                    lrs = width * height - 1;
                    if (lrs >= 0x7FF) {
                        lrs = 0x7FF;
                    }
                    words = (width * 2) / 8;
                    div = (words <= 0) ? 1 : words;
                    dl[8] = 0xF3000000;
                    dl[9] = 0x07000000 | (lrs & 0xFFF) << 12 | (((((words <= 0) ? 1 : words) + 0x7FF) / div) & 0xFFF);
                    dl[10] = 0xE7000000;
                    dl[11] = 0;
                    dl[12] = 0xF5100000 | (((width * 2 + 7) >> 3) & 0x1FF) << 9;
                    dl[13] = tile;
                    dl[14] = 0xF2000000;
                    dl[15] = (((width - 1) * 4) & 0xFFF) << 12 | (((height - 1) * 4) & 0xFFF);
                    dl += 16;
                } else {
                    cnt += 8;
                }
            }
        }
    }
    if (dl == NULL) {
        dl += cnt * 2;
    }
    return dl;
}

/* 0x8011D0C8 - make room for n more commands in the current page */
u32 *dlReserve(u32 *dl, s32 n) {
    if (32 - n - ((((u8 *)dl - gPagePool) >> 3) & 0x1F) > 0) {
        return dl;
    }
    return (u32 *)dlPageBranch(dl);
}

/* 0x8011D3E0 - empty the cache and turn the free heap (area 1, and area 2
 * above the framebuffers when extraHeap) into the page pool */
void meshCacheInit(s32 extraHeap) {
    MeshCache *e;
    u32 pages[2];
    u8 *base[2];
    u8 *pg, *last;
    s32 i, k, areas;

#ifdef PORT
    { extern void port_dl_flush(void); port_dl_flush(); }
#endif
    gMeshUsed.count = 0;
    gMeshUsed.head = NULL;
    gMeshReleased.count = 0;
    gMeshReleased.head = NULL;
    for (e = gMeshCache; e < &gMeshCache[511]; e++) {
        e->next = e + 1;
    }
    e->next = NULL;
    gMeshFree.count = 512;
    gMeshFree.head = gMeshCache;
    gPageFree = gPagePool = (u8 *)PHYS(gHeapPtr);
    base[0] = gPagePool;
    base[1] = (u8 *)PHYS(gFramebuffersEnd);
    gPageBytes = 0;
    gPagesFree = 0;
    areas = extraHeap + 1;
    for (i = 0; i < areas; i++) {
        pages[i] = (u32)heapFree(i + 1) >> 8;
        gPageBytes += pages[i] << 8;
        gPagesFree += pages[i];
    }
    for (i = 0; i < areas && pages[i] != 0; i++) {
        if (i != 0) {
            PAGE_LINK(last) = PHYS(base[i]);
        }
        pg = base[i];
        for (k = pages[i] - 1; k != 0; k--) {
            PAGE_LINK(pg) = PHYS(pg + 0x100);
            pg += 0x100;
        }
        last = pg;
    }
    PAGE_LINK(last) = 0;
}

/* 0x8011D5D4 - the compiled form of a mesh file (compiled on first use) */
MeshCache *meshCacheGet(void *meshFile) {
    MeshCache *e;

    for (e = gMeshUsed.head; e != NULL; e = e->next) {
        if (meshFile == e->key) {
            e->refs++;
            return e;
        }
    }
    if (gMeshFree.count == 0) {
        return gMeshUsed.head;
    }
    e = gMeshFree.head;
    gMeshFree.count--;
    gMeshFree.head = e->next;
    e->next = gMeshUsed.head;
    gMeshUsed.head = e;
    gMeshUsed.count++;
    e->lastFrame = 0;
    e->key = meshFile;
    e->refs = 1;
    modelCompile(meshFile, NULL, &e->radius2);
#ifdef PORT
    { extern void port_dl_compiled(const void *dl); port_dl_compiled(gMeshDL); }
#endif
    e->dl = gMeshDL;
    e->vtx = gMeshVtx;
    return e;
}

/* 0x8011D6AC - drop a user; the last one moves it to the released list
 * (gMeshUsed.count is not decremented) */
s32 meshCacheRelease(void *meshFile) {
    MeshCache **link = &gMeshUsed.head;
    MeshCache *e;

    for (e = *link; e != NULL; link = &e->next, e = e->next) {
        if (meshFile == e->key) {
            if (--e->refs == 0) {
                *link = e->next;
                e->next = gMeshReleased.head;
                gMeshReleased.head = e;
                gMeshReleased.count++;
                return 1;
            }
            return 0;
        }
    }
    return 0;
}

/* 0x8011D738 - take a page for a display list */
u32 *dlPageAlloc(void) {
    u8 *p = gPageFree;

    gPageFree = pageNext(gPageFree);
    PAGE_LINK(p) = 0;
    if (--gPagesFree == 0) {
        heapExhausted();
    }
    return (u32 *)p;
}

/* 0x8011D794 - continue the list in a new page: branch to it from here and
 * from the page's last slot */
u32 *dlPageBranch(u32 *dl) {
    s32 slot = (((u8 *)dl - gPagePool) >> 3) & 0x1F;
    u32 *pg = dlPageAlloc();
    u32 phys = PHYS(pg);

    dl[1] = phys;
    dl[0] = 0x06010000;                                  /* G_DL, no push */
    if (31 - slot != 0) {
        dl[(31 - slot) * 2] = 0x06010000;
        dl[(31 - slot) * 2 + 1] = phys;
    }
    return pg;
}

/* 0x8011D80C - return a chain of pages to the pool */
void dlPagesFree(u32 *dl) {
    u8 *last = (u8 *)dl;
    u8 *p;

    gPagesFree++;
    for (p = pageNext((u8 *)dl); PHYS(p) != 0; p = pageNext(p)) {
        last = p;
        gPagesFree++;
    }
    PAGE_LINK(last) = PHYS(gPageFree);
    gPageFree = (u8 *)dl;
}

/* 0x8011D8B0 - end the list and the page chain */
void dlEnd(u32 *dl) {
    u32 slot = (((u8 *)dl - gPagePool) >> 3) & 0x1F;

    dl[0] = 0xB8000000;
    dl[1] = 0;
    if (slot < 31) {
        PAGE_LINK((u8 *)dl - slot * 8) = 0;
    }
}

/* 0x8011D8EC - take a page for vertices */
u8 *vtxPageAlloc(void) {
    u8 *p = gPageFree;

    gPageFree = pageNext(gPageFree);
    PAGE_LINK(p) = 0;
    if (--gPagesFree == 0) {
        heapExhausted();
    }
    return p;
}

/* 0x8011D948 - link a new vertex page after the one holding vtx */
u8 *vtxPageNext(u8 *vtx) {
    s32 slot = ((vtx - gPagePool) >> 4) & 0xF;
    u8 *pg = vtxPageAlloc();

    PAGE_LINK(vtx - slot * 16) = PHYS(pg);
    return pg;
}

/* 0x8011D9A0 */
void vtxPagesFree(u8 *vtx) {
    u8 *last = vtx;
    u8 *p;

    gPagesFree++;
    for (p = pageNext(vtx); PHYS(p) != 0; p = pageNext(p)) {
        last = p;
        gPagesFree++;
    }
    PAGE_LINK(last) = PHYS(gPageFree);
    gPageFree = vtx;
}

/* 0x8011DA44 */
void vtxEnd(u8 *vtx) {
}

/* 0x8011DA50 - recycle released meshes the screen no longer shows; returns
 * how many are still waiting */
s32 meshCacheCollect(void) {
    MeshCache **link = &gMeshReleased.head;
    MeshCache *e, *next;

    for (e = *link; e != NULL; e = next) {
        next = e->next;
        if (gLastShownFrame >= e->lastFrame) {
            *link = next;
            gMeshReleased.count--;
            e->next = gMeshFree.head;
            gMeshFree.head = e;
            gMeshFree.count++;
            dlPagesFree(e->dl);
            vtxPagesFree(e->vtx);
        } else {
            link = &e->next;
        }
    }
    return gMeshReleased.count;
}

/* 0x8011DB20 */
void meshCacheNop(void) {
}

/* 0x8011DB28 - walk a display list to its end (does nothing with it) */
void dlWalk(u32 *dl) {
    u32 op;
    s32 i;

    for (;;) {
        op = dl[0] >> 24;
        if (op == 4) {
            for (i = (dl[0] >> 4) & 0x1F; i != 0; i--) {
            }
        }
        if (op == 6) {
            dl = (u32 *)(dl[1] + 0x7FFFFFF8);
        }
        if (op == 0xB8) {
            return;
        }
        dl += 2;
    }
}

/* 0x8011DB94 */
u8 *pageNext(u8 *page) {
    return (u8 *)(PAGE_LINK(page) + 0x80000000);
}

/* 0x8011DBA8 */
void heapExhausted(void) {
    fatalError(0, "OUT OF FREE HEAP\n");
}
