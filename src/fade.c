/*
 * fade.c - per-player distance fade of the scenery: objects between
 * gFadeNear2 and gFadeFar2 (squared distances) are drawn with falling alpha,
 * or (fog mode) darkening grey; and the per-player draw distance.
 * Original code: main segment, ROM 0x22C0-0x2820 (RAM 0x801012C0-0x80101820).
 */
#include "common.h"

typedef struct TrackDef {                /* 0x10 bytes, [2][14] at 0x80131F70 (see track.c) */
    s16 header;
    s32 firstSections;
    f32 drawDist;
    s32 fadeOn;
} TrackDef;

typedef struct FadeLen {                 /* per player count */
    f32 len;                             /* length of the fade */
    f32 extra;                           /* added to the draw distance */
} FadeLen;

extern f32  gFadeFar2[2];                /* 0x80146120 - squared distance where objects are fully faded */
extern f32  gFadeNear2[2];               /* 0x80146128 - squared distance where fading starts */
extern f32  gFadeInvRange[2];            /* 0x80146130 - 1 / (fade length)^2 */
extern s32  gFadeEnabled[2];             /* 0x80146138 */
extern s32  gFadeToWhite[2];             /* 0x80146140 - 1: fog (grey level), 0: alpha */
extern s32  gFadeUnk148[2];              /* 0x80146148 */
extern f32  gDrawDistance[2];            /* 0x80046E68 ovl2 - per player, route units */
extern TrackDef gTrackDefs[2][14];       /* 0x80131F70 */
extern FadeLen gFadeLens[2];             /* 0x80132130 */
extern s32  gNumPlayersM1;               /* 0x8015095C */
extern s32  gTrackChoice;                /* 0x80150960 */
extern u32 *gDlCursor;                   /* 0x80147684 */

void fadeSetRange(s32 player, f32 dist); /* 0x8010177C */

#define DL_PUSH(w0, w1) { u32 *_g = gDlCursor; gDlCursor = _g + 2; _g[1] = (w1); _g[0] = (w0); }

/* 0x801012C0 - at the start of a race: the track's fade setting and range */
void fadeInit(void) {
    s32 p;
    TrackDef *t;

    for (p = 0; p < 2; p++) {
        gFadeUnk148[p] = 0;
        t = &gTrackDefs[gNumPlayersM1][gTrackChoice];
        gFadeToWhite[p] = 0;
        gFadeEnabled[p] = t->fadeOn;
        fadeSetRange(p, t->drawDist);
    }
}

/* 0x8010139C - set the colour for an object at squared distance dist2;
 * prev: the alpha last set (255 = none). Returns the new one (0 = invisible) */
s32 fadeAlpha(s32 player, f32 dist2, s32 prev) {
    f32 near2 = gFadeNear2[player];
    f32 far2 = gFadeFar2[player];
    f32 d;
    s32 a;

    if (dist2 <= near2 || gFadeEnabled[player] == 0) {
        if (prev != 0xFF) {
            DL_PUSH(0xE7000000, 0);                      /* gDPPipeSync */
            DL_PUSH(0xFA000000, 0xFFFFFFFF);             /* prim colour: opaque white */
            prev = 0xFF;
        }
        return prev;
    }
    d = (far2 < dist2) ? far2 : dist2;
    a = -255.0f * ((d - near2) * gFadeInvRange[player]) + 255.0f;
    if (a != 0) {
        DL_PUSH(0xE7000000, 0);
        if (gFadeToWhite[player] != 0) {
            DL_PUSH(0xFA000000, (a & 0xFF) << 24 | (a & 0xFF) << 16 | (a & 0xFF) << 8 | 0xFF);
        } else {
            DL_PUSH(0xFA000000, (a & 0xFF) | 0xC8C8FF00);
        }
    }
    return a;
}

/* 0x80101518 - restore the colour after a run of fadeAlpha */
void fadeEnd(s32 player, s32 alpha) {
    if (alpha != 0xFF) {
        DL_PUSH(0xE7000000, 0);
        DL_PUSH(0xFA000000, 0xFFFFFFFF);
    }
}

/* 0x80101568 - mode 0 / 1: fade off / on; 2 / 3: the same, one player only;
 * 4: fog at the player's draw distance; 5: back to the track's range */
void fadeSetMode(s32 player, s32 mode) {
    if (mode < 2) {
        gFadeEnabled[player] = mode;
    } else if (mode >= 4) {
        if (mode == 4) {
            gFadeEnabled[player] = 1;
            gFadeToWhite[player] = 1;
            fadeSetRange(player, gDrawDistance[player]);
        } else {
            gFadeToWhite[player] = 0;
            fadeSetRange(player, gTrackDefs[gNumPlayersM1][gTrackChoice].drawDist);
        }
    } else if (gNumPlayersM1 == 0) {
        gFadeEnabled[player] = mode - 2;
    }
    if (gFadeEnabled[player] == 0) {
        gFadeToWhite[player] = 0;
    }
}

/* 0x80101684 - negative: exactly -dist; else not beyond the track's
 * distance in two-player games */
void drawDistSet(s32 player, s32 dist) {
    f32 d;

    if (dist < 0) {
        d = -(f32)dist;
    } else {
        d = dist;
        if (gNumPlayersM1 != 0 && gTrackDefs[gNumPlayersM1][gTrackChoice].drawDist < d) {
            d = gTrackDefs[gNumPlayersM1][gTrackChoice].drawDist;
        }
    }
    gDrawDistance[player] = d;
}

/* 0x80101708 */
void drawDistReset(s32 player) {
    gDrawDistance[player] = gTrackDefs[gNumPlayersM1][gTrackChoice].drawDist;
}

/* 0x80101754 */
void fadeSetUnk148(s32 player, s32 v) {
    gFadeUnk148[player] = v;
}

/* 0x8010176C */
void fadeNop(s32 player, s32 v) {
}

/* 0x8010177C - objects fade out over the last part before dist (+ extra) */
void fadeSetRange(s32 player, f32 dist) {
    FadeLen *l = &gFadeLens[gNumPlayersM1];
    f32 far = l->extra + dist;
    f32 far2 = far * far;
    f32 len = (gFadeToWhite[player] != 0) ? 10000.0f : l->len;
    f32 len2 = len * len;

    gFadeFar2[player] = far2;
    gFadeInvRange[player] = 1.0 / len2;
    gFadeNear2[player] = far2 - len2;
}
