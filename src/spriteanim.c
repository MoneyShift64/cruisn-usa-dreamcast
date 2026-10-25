/*
 * spriteanim.c - flip-book animations of display lists (the coast-to-coast
 * ending's crowd and fireworks), and the helicopter's spinning rotor.
 * Original code: main segment, RAM 0x8011DF70-0x8011E260 and 0x8011E7B0-0x8011E970.
 */
#include "common.h"
#include "frontend.h"

typedef struct SpriteFrame {             /* a frame: display list, how long (ends with 0) */
    /* 0x00 */ s32 dl;
    /* 0x04 */ f32 seconds;
} SpriteFrame;

typedef struct SpriteAnim {              /* 0x34 bytes, 20 at 0x8015BB10 */
    /* 0x00 */ Xform xf;
    /* 0x20 */ void **dls;
    /* 0x24 */ SpriteFrame *frames;      /* NULL: slot free */
    /* 0x28 */ f32 speed;                /* frame times are multiplied by this */
    /* 0x2C */ s32 event;                /* its animWait */
    /* 0x30 */ s32 frame;
} SpriteAnim;

typedef struct HeliDraw {
    /* 0x00 */ s32 x, y, z;
    /* 0x0C */ s32 yaw, pitch, roll;
    /* 0x18 */ s32 scale;
    /* 0x1C */ s32 visible;
} HeliDraw;

extern SpriteAnim gSpriteAnims[20];      /* 0x8015BB10 */
extern u32 gHeliLodDist[3];              /* 0x80134E50 */
extern u32 *gDlCursor;                   /* 0x80147684 */
extern s32 *gMtxCursor;                  /* 0x80147688 */

void xformsUpdate(Xform *xf);
void mtxFromAngles(s32 *m, s32 ax, s32 ay, s32 az);

f32  spriteAnimStep(s32 ramp, s32 i);
void spriteAnimNext(s32 i);
f32  spriteAnimTime(s32 i);

#define DL_PUSH(w0, w1) { u32 *_g = gDlCursor; gDlCursor = _g + 2; _g[0] = (w0); _g[1] = (u32)(w1); }

/* 0x8011DF70 - start an animation at a position: its frame list, the display lists the
 * frames name, the frame to start at, the speed. Returns which one it is, -1: none free.
 * (The original returns the animWait event instead, which its only caller, the ending,
 * then uses as this index; the port returns the index.) */
s32 spriteAnimStart(SpriteFrame *frames, void **dls, s32 start, f32 speed, s32 x, s32 y, s32 z) {
    SpriteAnim *a = gSpriteAnims;
    s32 i;

    for (i = 0; i < 20; i++, a++) {
        if (a->frames == NULL) {
            break;
        }
    }
    if (i == 20) {
        return -1;
    }
    textPlace(&a->xf, x, y, z, 1);
    a->frame = start - 1;
    a->frames = frames;
    a->dls = dls;
    a->speed = speed;
    spriteAnimNext(i);
    a->event = animWait(spriteAnimTime(i), (void *)i, (AnimDoneFunc)spriteAnimStep);
    return i;
}

/* 0x8011E040 */
void spriteAnimStop(s32 i) {
    SpriteAnim *a = &gSpriteAnims[i];

    animCancel(gAnimSlots[a->event].ramp);
    a->frames = NULL;
}

/* 0x8011E0A4 */
void spriteAnimStopAll(void) {
    s32 i;

    for (i = 0; i < 20; i++) {
        spriteAnimStop(i);
    }
}

/* 0x8011E0E4 */
void spriteAnimDraw(s32 i) {
    SpriteAnim *a = &gSpriteAnims[i];

    xformsUpdate(&a->xf);
    DL_PUSH(0x06000000, a->dls[a->frames[a->frame].dl]);
    DL_PUSH(0xBD000000, 0);
}

/* 0x8011E188 - (animWait) the next frame; returns how long it stays */
f32 spriteAnimStep(s32 ramp, s32 i) {
    spriteAnimNext(i);
    return spriteAnimTime(i);
}

/* 0x8011E1B8 */
void spriteAnimNext(s32 i) {
    SpriteAnim *a = &gSpriteAnims[i];

    a->frame++;
    if (a->frames[a->frame].seconds == 0.0f) {
        a->frame = 0;
    }
}

/* 0x8011E214 */
f32 spriteAnimTime(s32 i) {
    SpriteAnim *a = &gSpriteAnims[i];

    return a->speed * a->frames[a->frame].seconds;
}

/* 0x8011E7B0 - a helicopter: the body at the detail level for the distance, and the
 * rotor (see-through) turned by 'spin' */
void modelDrawSpinning(void **parts, HeliDraw *obj, s32 spin, u32 dist) {
    u32 *d = gHeliLodDist;
    s32 lod = 0;
    s32 *m;

    while (*d < dist) {
        d++;
        lod++;
        if (d == &gHeliLodDist[3]) {
            break;
        }
    }
    if (d >= &gHeliLodDist[3]) {
        return;
    }
    xformsUpdate((Xform *)obj);
    m = gMtxCursor;
    gMtxCursor = m + 16;
    mtxFromAngles(m, spin, 0, 0);
    m[12] = 0x4CCC;
    m[13] = 0xA0000;
    m[14] = 0x2B333;
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xFA000000, 0xFFFFFF64);
    DL_PUSH(0x01040040, m - 16);
    DL_PUSH(0x06000000, parts[3]);
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xFA000000, 0xFFFFFFFF);
    DL_PUSH(0xBD000000, 0);
    DL_PUSH(0x06000000, parts[lod]);
    DL_PUSH(0xBD000000, 0);
}
