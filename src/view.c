/*
 * view.c - start drawing through a viewer: the camera matrix from
 * gViewerCams, the projection, viewport and scissor for the screen layout.
 * Original code: main segment, ROM 0x17C10-0x18080 (RAM 0x80116C10-0x80117080).
 */
#include "common.h"

typedef struct ViewerCam {               /* 0x28 bytes, 0x8014EF90 */
    /* 0x00 */ s32 pos[3];               /* 16.16 */
    /* 0x0C */ s32 angles[3];            /* binary angles (mtxView) */
    /* 0x18 */ s32 target[3];            /* look-at point, when not using angles */
    /* 0x24 */ s32 useAngles;
} ViewerCam;

typedef struct Vp16 { s16 v[8]; } Vp16;  /* RSP viewport: scale, translation (x4) */

extern ViewerCam gViewerCams[2];         /* 0x8014EF90 */
extern s32 gCurViewer;                   /* 0x8014EFE0 - viewer being drawn */
extern s32 gNumPlayersM1;                /* 0x8015095C */
extern u32 *gDlCursor;                   /* 0x80147684 */
extern s32 *gMtxCursor;                  /* 0x80147688 */
extern void *gOrthoMtx[2];               /* 0x80133700 - 2D projection per player count */
extern void *gPerspMtx[2];               /* 0x80133708 - 3D projection per player count */
extern u32 gRaceBaseDL[];                /* 0x80133710 - identity (RSP format) */
extern u32 gPerspIdentity[];             /* 0x801338D0 - projection for the full-screen pictures */
extern Vp16 gViewports[3];               /* 0x80133AB0 - full screen, top, bottom */
extern Vp16 gOrthoViewports[3];          /* 0x80133AE0 */
extern Vp16 gBgViewport;                 /* 0x80133B10 */

void mtxTranslate(s32 *m, s32 x, s32 y, s32 z); /* 0x80114498 */
void mtxView(s32 *m, s32 *pos, s32 *angles); /* 0x80114520 */
void mtxBillboard(s32 *m, s32 *eye, s32 *at); /* 0x80114A0C */
void viewportSet(Vp16 *vp, s32 dx, s32 dy); /* 0x80115C10 */
void scissorSet(s32 viewer);             /* 0x80115DD4 */

#define DL_PUSH(w0, w1) { u32 *_g = gDlCursor; gDlCursor = _g + 2; _g[0] = (w0); _g[1] = (w1); }

/* 0x80116C10 - set up drawing the 3D scene for a viewer (-1: full screen) */
void viewBegin(s32 viewer) {
    s32 *m;
    s32 v, layout;
    ViewerCam *cam;

    if (viewer == -1) {
        v = 0;
        layout = 0;
    } else {
        v = viewer;
        layout = gNumPlayersM1;
    }
    gCurViewer = v;
    m = gMtxCursor;
    cam = &gViewerCams[v];
    gMtxCursor = m + 16;
    if (cam->useAngles == 0) {
        mtxBillboard(m, cam->pos, cam->target);
    } else {
        mtxView(m, cam->pos, cam->angles);
    }
    DL_PUSH(0x01030040, (u32)gPerspMtx[layout]);    /* gSPMatrix(projection, load) */
    DL_PUSH(0x01010040, (u32)(m - 16));             /* gSPMatrix(camera, multiply into projection) */
    DL_PUSH(0x01020040, (u32)gRaceBaseDL);          /* gSPMatrix(identity, load modelview) */
    DL_PUSH(0xE7000000, 0);                         /* gDPPipeSync */
    viewportSet(&gViewports[layout + v], 0, 0);
    scissorSet(viewer);
}

/* 0x80116D8C - set up 2D drawing for a viewer: ortho (pictures in front) or
 * a far perspective plane */
void bgDrawSetup(s32 viewer, s32 ortho) {
    s32 *m;
    void **proj;
    Vp16 *vps;
    s32 z, layout, vp;

    if (viewer == -1) {
        layout = 0;
        vp = 0;
    } else {
        layout = gNumPlayersM1;
        vp = layout + viewer;
    }
    if (ortho != 0) {
        proj = gOrthoMtx;
        z = -100 << 16;
        vps = gOrthoViewports;
    } else {
        proj = gPerspMtx;
        z = -30000 << 16;
        vps = gViewports;
    }
    m = gMtxCursor;
    gMtxCursor = m + 16;
    mtxTranslate(m, 0, 0, z);
    DL_PUSH(0x01020040, (u32)(m - 16));             /* modelview */
    DL_PUSH(0x01030040, (u32)proj[layout]);         /* projection */
    DL_PUSH(0xE7000000, 0);
    viewportSet(&vps[vp], 0, 0);
    scissorSet(viewer);
}

/* 0x80116ECC - full-screen 3D view through viewer 0 with the flat projection */
void viewSetupFull(void) {
    s32 *m;
    ViewerCam *cam = &gViewerCams[0];

    m = gMtxCursor;
    gMtxCursor = m + 16;
    if (cam->useAngles == 0) {
        mtxBillboard(m, cam->pos, cam->target);
    } else {
        mtxView(m, cam->pos, cam->angles);
    }
    DL_PUSH(0x01020040, (u32)(m - 16));
    DL_PUSH(0x01030040, (u32)gPerspIdentity);
    DL_PUSH(0xE7000000, 0);
    viewportSet(&gViewports[0], 0, 0);
    scissorSet(-1);
}

/* 0x80116FB8 - full-screen background picture plane */
void bgDrawSetupFull(void) {
    s32 *m;

    m = gMtxCursor;
    gMtxCursor = m + 16;
    mtxTranslate(m, 0, 0, -30000 << 16);
    DL_PUSH(0x01020040, (u32)(m - 16));
    DL_PUSH(0x01030040, (u32)gPerspIdentity);
    DL_PUSH(0xE7000000, 0);
    viewportSet(&gBgViewport, 0, 0);
    scissorSet(-1);
}
