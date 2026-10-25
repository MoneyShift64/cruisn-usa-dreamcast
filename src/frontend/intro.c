/*
 * intro.c - the opening sequence (after the credits roll, and at boot): the
 * Williams / Midway logos fade in and out, then four cars race down the road
 * towards the camera under a helicopter, and the leading car smashes through
 * the CRUIS'N USA logo, which flies apart in 280 pieces that bounce on the
 * road. START skips to Choose Race (by way of the title).
 * Original code: front-end overlay, ROM 0x739A0-0x74AA0 (RAM 0x8003AC10-0x8003BD10).
 * (Declared as creditsInit in frontend.h: an early guess at what it was.)
 *
 * The sequence is a list of animation scripts started on given frames
 * (gIntroEvents); they move the logos' alphas, the cars and the camera.
 */
#include "common.h"
#include "frontend.h"
#include "race.h"

typedef struct IntroEvent {              /* 0x10 bytes, table 0x800446A8 (ends with frame -1) */
    /* 0x00 */ s32 frame;                /* started on this frame of the sequence */
    /* 0x04 */ AnimCmd *script;
    /* 0x08 */ f32 seconds;
    /* 0x0C */ s32 slot;                 /* while it plays (animStart's), -1 = not */
} IntroEvent;

typedef struct IntroCar {                /* 0x0C bytes, table 0x80044748 */
    /* 0x00 */ s32 model;
    /* 0x04 */ s32 x;
    /* 0x08 */ s32 z;
} IntroCar;

typedef struct IntroScenery {            /* 0x18 bytes, table 0x80044778: two scenery parts, each */
    /* 0x00 */ s32 dl;                   /* drawn while the camera's yaw is in [lo, hi] */
    /* 0x04 */ u32 lo;
    /* 0x08 */ u32 hi;
    /* 0x0C */ s32 dl2;
    /* 0x10 */ u32 lo2;
    /* 0x14 */ u32 hi2;
} IntroScenery;

typedef struct IntroPiece {              /* 0x10 bytes, table 0x800432D0: a piece of the logo */
    /* 0x00 */ s32 x;                    /* pixels in the logo */
    /* 0x04 */ s32 y;
    /* 0x08 */ s32 angle;                /* direction it flies off in */
    /* 0x0C */ f32 dist;                 /* from the impact (0..1): nearer ones fly faster */
} IntroPiece;

typedef struct IntroVel {                /* 0x14 bytes: a flying piece's speeds */
    /* 0x00 */ s32 x, y, z;              /* per frame, 16.16 */
    /* 0x0C */ s32 spinA, spinB;         /* binary angles per frame */
} IntroVel;

typedef struct HeliDraw {
    /* 0x00 */ s32 x, y, z;
    /* 0x0C */ s32 yaw, pitch, roll;
    /* 0x18 */ s32 scale;
    /* 0x1C */ s32 visible;
} HeliDraw;

typedef struct ViewerCam { s32 pos[3], angles[3], target[3], useAngles; } ViewerCam;

#define INTRO_PIECES 280

extern IntroPiece   gIntroPieces[INTRO_PIECES]; /* 0x800432D0 ovl3 */
extern IntroEvent   gIntroEvents[10];    /* 0x800446A8 ovl3 - 9 and the end */
extern IntroCar     gIntroCars[4];       /* 0x80044748 ovl3 */
extern IntroScenery gIntroScenery[6];    /* 0x80044778 ovl3 */
extern f64          gIntroLength;        /* 0x80046000 ovl3 - 560 frames */
extern f64          gIntroLength2;       /* 0x80046008 ovl3 - (the same) */
extern f64          gIntroBounce;        /* 0x80046010 ovl3 - 0.3 */
extern f64          gIntroMinSpeed;      /* 0x80046018 ovl3 - 0.1 */

extern s32       gIntroAlphaB;           /* 0x8004B080 ovl3 - logo pictures, second pass */
extern s32       gIntroAlphaA;           /* 0x8004B084 ovl3 - ... first pass */
extern s32       gIntroAlphaC;           /* 0x8004B088 ovl3 - the picture after them */
extern RenderObj gIntroCarObjs[4];       /* 0x8004B090 ovl3 */
extern HeliDraw  gIntroHeli;             /* 0x8004BC50 ovl3 */
extern s32       gIntroFade;             /* 0x8004BC70 ovl3 - black over everything (alpha) */
extern u32       gIntroHeapMark;         /* 0x8004BC74 ovl3 */
extern s32       gIntroFrame;            /* 0x8004BC78 ovl3 */
extern s32       gIntroSmashed;          /* 0x8004BC7C ovl3 - the logo has been hit */
extern s32       gIntroFromCredits;      /* 0x8004BC80 ovl3 */
extern IntroVel  gIntroVel[INTRO_PIECES]; /* 0x8004BC88 ovl3 */
extern void     *gIntroDLs[20];          /* 0x8004D268 ovl3 - group 0x39C: logos, scenery, helicopter */
extern void     *gIntroPieceDLs[INTRO_PIECES]; /* 0x8004D2B8 ovl3 - group 0x406 */
extern Xform     gIntroPieceXf[INTRO_PIECES]; /* 0x8004D718 ovl3 */
extern s32       gIntroRotor;            /* 0x8004FA18 ovl3 */

extern ViewerCam gViewerCams[2];         /* 0x8014EF90 */
extern s32       gFadeToWhite[2];        /* 0x80146140 */
extern u32      *gDlCursor;              /* 0x80147684 */
extern s32      *gMtxCursor;             /* 0x80147688 */

void carPalettesReset(void);             /* 0x80117EBC */
void trackEnvInit(s32 track);            /* 0x80101820 */
s32  gfxWaitIdle(void);
s32  viewYawTo(s32 player, s32 x, s32 z);
void skyDraw(s32 player, s32 yaw, s32 roll);
void viewBegin(s32 racer);
void viewSetupFull(void);                /* 0x80116ECC */
void modelDrawSpinning(void **parts, HeliDraw *obj, s32 spin, u32 dist); /* 0x8011E7B0 */
void mtxFromAngles(s32 *m, s32 ax, s32 ay, s32 az);
f32  randFloat(void);
s32  tblCos(s32 angle);
void titleInit(s32 skip);

void introFrame(s32 arg);
void introEvents(void);
s32  introEventDone(s32 slot, s32 b, f32 c);
void introCrashDraw(void);
void introSceneryDraw(s32 yaw);
void introLogosDraw(s32 arg);
void introPictureDraw(void);
void introPiecesDraw(void);
void introPiecesMove(void);
void introPiecesReset(void);
void introSmash(void);

#define DL_PUSH(w0, w1) { u32 *_g = gDlCursor; gDlCursor = _g + 2; _g[0] = (w0); _g[1] = (u32)(w1); }
#define MTX_NEW(m) { m = gMtxCursor; gMtxCursor = m + 16; }

/* 0x8003AC10 ovl3 - start the sequence (fromCredits: when it ends, the front end starts over) */
void creditsInit(s32 fromCredits) {
    IntroEvent *e;
    IntroCar *c;
    RenderObj *obj;
    ViewerCam *cam;
    s32 p;

    carPalettesReset();
    gFadeToWhite[1] = 0;
    gFadeToWhite[0] = 0;
    musicPlay(0xA0);
    gIntroFromCredits = fromCredits;
    gIntroHeapMark = lowMemOverlayLoad();
    assetLoadGroup(0x406, gIntroPieceDLs);
    assetLoadGroup(0x39C, gIntroDLs);
    lowMemOverlayRelease();
    trackEnvInit(3);
    introPiecesReset();
    for (e = gIntroEvents; e->frame != -1; e++) {
        e->slot = -1;
    }
    obj = gIntroCarObjs;
    for (c = gIntroCars; c != &gIntroCars[4]; c++, obj++) {
        renderObjInit(obj, c->model, 0, 1, 0, c->x, 0, c->z);
        obj->angleZ = 0x8000;
    }
    cam = &gViewerCams[0];
    cam->target[2] = -0x640000;
    cam->pos[0] = 0;
    cam->pos[1] = 0x500000;
    cam->pos[2] = 0;
    cam->target[0] = 0;
    cam->target[1] = 0x500000;
    cam->useAngles = 0;
    gIntroSmashed = 0;
    gIntroFrame = 0;
    gIntroAlphaB = 0;
    gIntroAlphaA = 0;
    gIntroAlphaC = 0;
    gIntroFade = 0;
    textPlace((Xform *)&gIntroHeli, 0, 0x12C0000, 0xC5680000, 1);
    gIntroHeli.yaw = 0x8000;
    gIntroHeli.pitch = -0x9C4;
    for (p = 0; p < gNumControllers; p++) {
        contPressed(p, BTN_ALL);
    }
    gModeFunc = introFrame;
    introFrame(0);
}

/* 0x8003AE14 ovl3 */
void introFrame(s32 arg) {
    IntroEvent *e;
    RenderObj *obj;
    s32 yaw, d;

    if (gIntroLength == (f64)gIntroFrame || (contPressed(0, BTN_START) & BTN_START)) {
        gfxWaitIdle();
        musicStop();
        for (e = gIntroEvents; e->frame != -1; e++) {
            if (e->slot != -1) {
                animCancel(gAnimSlots[e->slot].ramp);
                e->slot = -1;
            }
        }
        contPressed(0, BTN_ALL);
        lowMemOverlayLoad();
        highHeapRestore(gIntroHeapMark);
        lowMemOverlayRelease();
        if (gIntroFromCredits != 0) {
            gModeFunc = frontEndEnter;
            gModeArg = 0;
        } else {
            gModeFunc = titleInit;
            gModeArg = (gIntroLength2 == (f64)gIntroFrame) ? 0 : 1;   /* START: on to Choose Race */
        }
        gModeFunc(gModeArg);
        return;
    }
    introEvents();
    screenClear(-1, 1);
    if (gIntroSmashed == 0) {
        introLogosDraw(0);
        introPictureDraw();
        return;
    }
    yaw = viewYawTo(0, gIntroCarObjs[3].x, gIntroCarObjs[3].z);
    bgDrawSetup(-1, 1);
    rectFill(0x140, 2, -0x140000, 0x630000, 0x96, 0x96, 0xA, 0xFF);
    skyDraw(-1, yaw, 0);
    introSceneryDraw(yaw);
    gViewerCams[0].target[2] = gIntroCarObjs[3].z;
    gIntroCarObjs[3].flags |= RO_BACKFIRE;
    viewBegin(-1);
    for (obj = gIntroCarObjs; obj != &gIntroCarObjs[4]; obj++) {
        d = obj->z - gViewerCams[0].pos[2];
        obj->camDist = (d >= 0) ? d : -d;
        renderObjDraw(obj);
    }
    d = gIntroHeli.z - gViewerCams[0].pos[2];
    gIntroRotor -= 10000;
    modelDrawSpinning(&gIntroDLs[16], &gIntroHeli, gIntroRotor, (d >= 0) ? d : -d);
    viewSetupFull();
    if (yaw < 0xAFC8) {
        introPiecesDraw();
        introCrashDraw();
    }
    if (gIntroFade != 0) {
        bgDrawSetup(-1, 1);
        rectFill(0x140, 0xF0, -0x140000, 0xDB0000, 0, 0, 0, gIntroFade);
    }
    introPiecesMove();
}

/* 0x8003B178 ovl3 - start this frame's scripts; the leading car reaches the logo */
void introEvents(void) {
    IntroEvent *e;
    s32 i = 0;

    for (e = gIntroEvents; e->frame != -1; e++, i++) {
        if (gIntroFrame == e->frame) {
            e->slot = animStart(e->seconds, e->script, NULL, introEventDone, 0);
            gAnimSlots[e->slot].user = i;
        }
    }
    if (gIntroSmashed == 0 && gIntroCarObjs[0].z >= (s32)0xFCB07101) {
        introSmash();
        gIntroSmashed = 1;
    }
    gIntroFrame++;
}

/* 0x8003B29C ovl3 */
s32 introEventDone(s32 slot, s32 b, f32 c) {
    gIntroEvents[gAnimSlots[slot].user].slot = -1;
    return 2;
}

/* 0x8003B2DC ovl3 - what is left of the logo's frame, on the road */
void introCrashDraw(void) {
    s32 *m;

    MTX_NEW(m);
    mtxFromAngles(m, 0x8CA0, 0, 0);
    m[12] = 0x03CA0000;
    m[13] = 0xC40000;
    m[14] = 0x04B00000;
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xB900031D, 0x5041C8);
    DL_PUSH(0x01040040, m - 16);
    DL_PUSH(0x06000000, gIntroDLs[15]);
    DL_PUSH(0xBD000000, 0);
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xB900031D, 0x504240);
}

/* 0x8003B404 ovl3 - the roadside, by the direction the camera looks in */
void introSceneryDraw(s32 yaw) {
    IntroScenery *s;
    u32 y;

    viewBegin(-1);
    y = yaw & 0xFFFF;
    for (s = gIntroScenery; s != &gIntroScenery[6]; s++) {
        if (y >= s->lo && s->hi >= y) {
            DL_PUSH(0x06000000, gIntroDLs[s->dl]);
        }
        if (y >= s->lo2 && s->hi2 >= y) {
            DL_PUSH(0x06000000, gIntroDLs[s->dl2]);
        }
    }
}

/* 0x8003B4EC ovl3 - the two company logos (drawn twice: two alpha-compare passes) */
void introLogosDraw(s32 arg) {
    bgDrawSetup(-1, 1);
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xB9000002, 3);
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xFA000000, (gIntroAlphaA & 0xFF) | ~0xFF);
    drawDL(gIntroDLs[0], 0x20000, 0x640000, 0, 0x10000);
    drawDL(gIntroDLs[1], 0x20000, 0x5A0000, 0, 0x10000);
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xB9000002, 1);
    DL_PUSH(0xFA000000, (gIntroAlphaB & 0xFF) | ~0xFF);
    drawDL(gIntroDLs[0], 0x20000, 0x640000, 0, 0x10000);
    drawDL(gIntroDLs[1], 0x20000, 0x5A0000, 0, 0x10000);
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xFA000000, 0xFFFFFFFF);
}

/* 0x8003B6B0 ovl3 */
void introPictureDraw(void) {
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xFA000000, (gIntroAlphaC & 0xFF) | ~0xFF);
    drawDL(gIntroDLs[2], 0x8B0000, 0x5A0000, 0, 0x10000);
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xFA000000, 0xFFFFFFFF);
}

/* 0x8003B770 ovl3 - the logo's pieces */
void introPiecesDraw(void) {
    Xform *xf;
    void **dl;

    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xB7000000, 1);                  /* z buffer */
    DL_PUSH(0xB6000000, 0x2000);             /* no back-face culling */
    DL_PUSH(0xB900031D, 0x552078);
    dl = gIntroPieceDLs;
    for (xf = gIntroPieceXf; xf != &gIntroPieceXf[INTRO_PIECES]; xf++, dl++) {
        drawDLXform(*dl, xf);
    }
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xB900031D, 0x504240);
    DL_PUSH(0xB7000000, 0x2000);
    DL_PUSH(0xB6000000, 1);
}

/* 0x8003B8C4 ovl3 - the pieces fly, fall and bounce on the road */
void introPiecesMove(void) {
    IntroVel *v = gIntroVel;
    Xform *xf = gIntroPieceXf;
    s32 vx, vy, vz, va, vb, ny, na, nb;

    for (; v != &gIntroVel[INTRO_PIECES]; v++, xf++) {
        vx = v->x;
        vy = v->y;
        vz = v->z;
        va = v->spinA;
        vb = v->spinB;
        xf->x += vx;
        ny = xf->y + vy;
        na = xf->unk0C + va;
        nb = xf->unk10 + vb;
        xf->y = ny;
        xf->z += vz;
        xf->unk0C = na;
        xf->unk10 = nb;
        if (ny < 0) {
            xf->y = -ny;
            v->y = (s32)((f64)-vy * gIntroBounce);
            v->spinA = (s32)((f64)-va * gIntroBounce);
            v->spinB = (s32)((f64)-vb * gIntroBounce);
            v->z = (s32)((f64)vz * gIntroBounce);
            v->x = (s32)((f64)vx * gIntroBounce);
            xf->unk0C = (s32)((f64)na * gIntroBounce);
            xf->unk10 = (s32)((f64)(nb - 0x4000) * gIntroBounce + 16384.0);
        }
        v->y -= 0x8000;                      /* gravity */
    }
}

/* 0x8003BA10 ovl3 - the pieces assembled into the logo, across the road */
void introPiecesReset(void) {
    IntroPiece *p = gIntroPieces;
    Xform *xf = gIntroPieceXf;

    for (; p != &gIntroPieces[INTRO_PIECES]; p++, xf++) {
        textPlace(xf, (p->x << 16) - 0x9DC780, (p->y << 16) - 0x27960, (s32)0xFEA47100, 1);
    }
}

/* 0x8003BABC ovl3 - the car hits the logo: every piece gets a speed away from the impact */
void introSmash(void) {
    IntroVel *v = gIntroVel;
    IntroPiece *p = gIntroPieces;
    f32 f, s, sn, cs, r1;
    f64 r2;

    for (; v != &gIntroVel[INTRO_PIECES]; v++, p++) {
        f = p->dist;
        if ((f64)f > 1.0) {
            f = 1.0f;
        }
        s = (f32)((f64)(1.0f - f) + gIntroMinSpeed);
        sn = (f32)((f64)(f32)tblSin(p->angle) / 16384.0);
        cs = (f32)((f64)(f32)tblCos(p->angle) / 16384.0);
        r1 = (f32)((f64)randFloat() * 1.0 + 0.5);
        r2 = (f64)randFloat() * 1.0;
        v->spinA = (s32)(cs * 2048.0f * s * r1);
        v->spinB = (s32)(sn * -2048.0f * s * (f32)(r2 + 0.5));
        v->z = (s32)(851968.0f * s * (f32)((f64)randFloat() * 1.0 + 0.5));
        v->x = (s32)(sn * 65536.0f * s * (f32)((f64)randFloat() * 1.0 + 0.5));
        v->y = (s32)(cs * 65536.0f * s * (f32)((f64)randFloat() * 1.0 + 0.5));
    }
}
