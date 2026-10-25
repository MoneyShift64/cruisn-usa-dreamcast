/*
 * text.c - on-screen text: numbers and times as strings, text objects made
 * of one textured quad (a "sprite" of four vertices) per character, drawn
 * in two layers each frame; and a tiny 5x5 font drawn straight into a
 * 16-bit picture (for debugging).
 * Original code: main segment, ROM 0x1ABE0-0x1C710 (RAM 0x80119BE0-0x8011B710).
 */
#include "common.h"
#include "frontend.h"

/* A font: characters, the gap between them, and the size of a space */
typedef struct Glyph {
    /* 0x00 */ u8  ch;                   /* 0 ends the table */
    /* 0x01 */ u8  w, h;
    /* 0x03 */ u8  pad03;
    /* 0x04 */ u32 offset;               /* of its texels in the font image */
} Glyph;
typedef struct Font {
    /* 0x00 */ Glyph *glyphs;
    /* 0x04 */ s32 spacing;              /* between characters, and between lines */
    /* 0x08 */ s32 spaceW;               /* a space or a character the font has not */
    /* 0x0C */ s32 height;
} Font;
extern Glyph gFont0Glyphs[51];           /* 0x80134780 */
extern Glyph gFont1Glyphs[42];           /* 0x80134918 */
extern Glyph gFont2Glyphs[45];           /* 0x80134A68 */
extern Glyph gFont3Glyphs[13];           /* 0x80134BD0 */
extern Glyph gFont4Glyphs[11];           /* 0x80134C38 */
extern Glyph gFont5Glyphs[7];            /* 0x80134C90 */
extern Font  gFonts[6];                  /* 0x80134CC8 */
extern u8    gSmallFontChars[8];         /* 0x80134D28 - the punctuation it has */
extern u8    gSmallFont[208];            /* 0x80134D30 - 5 bytes (columns) a character: A-Z, 0-9, punctuation */

/* One character on screen: a quad. The first two vertices' flag words hold
 * the owning TextObj's address (high, low half; 0 = free), the third's the
 * glyph number (0xFFFF = blank). */
typedef struct TextSprite { Vtx v[4]; } TextSprite;
#define NUM_SPRITES 350
extern TextSprite gTextSprites[NUM_SPRITES]; /* 0x80151E58 */
extern TextObj gTextPool[50];            /* 0x801575D8 */
extern List gTextActive;                 /* 0x80158258 */
extern List gTextFree;                   /* 0x80158260 */

extern u32 *gDlCursor;                   /* 0x80147684 */
extern s32 *gMtxCursor;                  /* 0x80147688 */
extern u8  *gVtxCursor;                  /* 0x8014768C */

ListNode *listPushFront(List *list, ListNode *node);   /* 0x801032E0 */
ListNode *listPushBack(List *list, ListNode *node);    /* 0x80103350 */
ListNode *listRemove(List *list, ListNode *node);      /* 0x801033C0 */
ListNode *listPopFront(List *list);                    /* 0x8010345C */
void listInitPool(List *list, u8 *items, s32 stride, s32 count); /* 0x801034E4 */
void mtxFromAngles(s32 *m, s32 ax, s32 ay, s32 az);    /* 0x80114824 */
void mtxScaleBy(s32 *m, s32 sx, s32 sy, s32 sz);       /* 0x8011545C */
void bgDrawSetup(s32 viewer, s32 ortho); /* 0x80116D8C */

Glyph *glyphFind(u8 c, s32 font);        /* 0x8011B500 */
void spriteSetVtx(s32 sprite, s32 corner, s32 x, s32 y, s32 s, s32 t, s32 flag); /* 0x8011B55C */
s32  textNextSprite(TextObj *t, s32 *sprite); /* 0x8011B5B0 */
u8  *smallFontGlyph(u8 c);               /* 0x8011B648 */
void textSetup(TextObj *t, char *s, s32 style); /* 0x8011AC98 */
void textDrawSprites(TextObj *t);        /* 0x8011AF3C */

#define DL_PUSH(w0, w1) { u32 *_g = gDlCursor; gDlCursor = _g + 2; _g[0] = (w0); _g[1] = (w1); }

/* 0x80119BE0 - write a number: width > 0 right-aligned in 'width' characters
 * (padded with 'pad', a '-' first), width < 0 left-aligned and padded to
 * -width, 0 as long as it needs; too long for the width: "0". Returns the end. */
char *intToStr(char *out, s32 value, s32 width, u8 pad) {
    char buf[40];
    char *p = &buf[15];
    s32 w = (width < 0) ? -width : width;
    s32 neg = 0;
    s32 n = 0;
    s32 total, len, i;

    if (value < 0) {
        value = -value;
        neg = 1;
    }
    if (value == 0) {
        p = &buf[14];
        *p = '0';
        n = 1;
    } else {
        while (value > 0) {
            *--p = value % 10 + '0';
            value /= 10;
            n++;
        }
    }
    total = n + neg;
    len = total;
    if (width != 0 && w < total) {
        neg = 0;
        n = 1;
        p = &buf[14];
        buf[14] = '0';
    }
    if (width <= 0) {
        if (neg) {
            *--p = '-';
        }
        for (i = 0; i < len; i++) {
            out[i] = p[i];
        }
        if (width != 0) {
            for (; i < w; i++) {
                out[i] = pad;
            }
        }
        return out + i;
    }
    if (pad == ' ') {
        for (i = 0; i < w - len; i++) {
            out[i] = pad;
        }
        if (neg) {
            out[i++] = '-';
        }
    } else {
        if (neg) {
            out[0] = '-';
        }
        for (i = neg; i < w - n; i++) {
            out[i] = pad;
        }
    }
    for (; i < w; i++) {
        out[i] = p[i - w + n];
    }
    return out + i;
}

/* 0x80119EEC - a time in 1/100 s as "-MM:SS:hh" (9 characters); outside
 * min..max or 100 minutes or more: blank */
char *timeToStr(char *out, s32 value, s32 min, s32 max) {
    s32 t, hund, secs;

    out[3] = ':';
    out[6] = ':';
    t = (value < 0) ? -value : value;
    hund = t % 100;
    t /= 100;
    secs = t % 60;
    t /= 60;
    if (value < min || max < value || t >= 100) {
        out[8] = ' ';
        out[7] = ' ';
        out[5] = ' ';
        out[4] = ' ';
        out[2] = ' ';
        out[1] = ' ';
        out[0] = ' ';
    } else {
        intToStr(out + 7, hund, 2, '0');
        intToStr(out + 4, secs, 2, '0');
        intToStr(out + 1, t, 2, '0');
        out[0] = (value < 0) ? '-' : ' ';
    }
    return out;
}

/* 0x8011A03C - width of a line of text in pixels */
s32 textWidth(char *s, s32 font) {
    Font *f;
    Glyph *g;
    s32 w = 0;
    u8 c;

    for (c = *s++; c != 0 && c != '\n'; c = *s++) {
        f = &gFonts[font & TEXT_FONT];
        if (c != ' ' && (g = glyphFind(c, font)) != NULL) {
            w += g->w;
        } else {
            w += f->spaceW;
        }
        w += f->spacing;
    }
    return w - gFonts[font & TEXT_FONT].spacing;
}

/* 0x8011A114 - a text object (hidden until placed with textPlace); style:
 * 0 left-aligned, 1 right-aligned, 2 centred */
TextObj *textCreate(s32 font, char *text, s32 style) {
    TextObj *t = (TextObj *)listPopFront(&gTextFree);

    if (t == NULL) {
        return NULL;
    }
    t->xf.visible = 0;
    t->blink = -1;
    t->viewer = -1;
    t->flags = 0;
    t->font = font;
    t->blinkLevel = 0.5f;
    listPushBack(&gTextActive, &t->node);
    textSetup(t, text, style);
    return t;
}

/* 0x8011A1A4 - ... showing a number (see intToStr) */
TextObj *textCreateNum(s32 font, s32 value, s32 width, s32 pad, s32 style) {
    char buf[12];

    *intToStr(buf, value, width, pad) = 0;
    return textCreate(font, buf, style);
}

/* 0x8011A1E8 - ... showing a time (see timeToStr) */
TextObj *textCreateTime(s32 font, s32 value, s32 min, s32 max, s32 style) {
    char buf[12];

    buf[9] = 0;
    return textCreate(font, timeToStr(buf, value, min, max), style);
}

/* 0x8011A220 - change the characters from 'start' on (the layout stays) */
TextObj *textSetString(TextObj *t, char *s, s32 start) {
    s32 sprite = -1;
    s32 n = t->count;
    s32 font = t->font;
    s32 i = 0;
    Glyph *g;
    u8 c;

    if (n > 0) {
        for (c = *s++; c != 0; c = *s++) {
            textNextSprite(t, &sprite);
            if (i >= start) {
                if (c != ' ' && (g = glyphFind(c, font)) != NULL) {
                    gTextSprites[sprite].v[2].v.flag = (u16)(((u8 *)g - (u8 *)gFonts[font & TEXT_FONT].glyphs) >> 3);
                } else {
                    gTextSprites[sprite].v[2].v.flag = 0xFFFF;
                }
            }
            i++;
            if (i >= n) {
                break;
            }
        }
    }
    return t;
}

/* 0x8011A368 */
TextObj *textSetNumber(TextObj *t, s32 value, s32 width, s32 pad, s32 start) {
    char buf[12];

    *intToStr(buf, value, width, pad) = 0;
    return textSetString(t, buf, start);
}

/* 0x8011A3AC */
TextObj *textSetTime(TextObj *t, s32 value, s32 min, s32 max) {
    char buf[12];

    buf[9] = 0;
    return textSetString(t, timeToStr(buf, value, min, max), 0);
}

/* 0x8011A3E4 - tint */
void textSetColor(TextObj *t, u8 r, u8 g, u8 b, u8 a) {
    t->color = (r << 24) + (g << 16) + (b << 8) + a;
    t->flags |= TEXTF_COLOR;
}

/* 0x8011A43C - give back a text object and its sprites */
void textFree(TextObj *t) {
    s32 sprite = t->sprite - 1;

    while (textNextSprite(t, &sprite)) {
        gTextSprites[sprite].v[1].v.flag = 0;
        gTextSprites[sprite].v[0].v.flag = 0;
    }
    listPushFront(&gTextFree, listRemove(&gTextActive, &t->node));
}

/* 0x8011A4DC - draw one text object now (if shown) */
void textDraw(TextObj *t, u32 unused) {
    if (t->xf.visible == 0) {
        return;
    }
    if (t->blink != -1 && !(gAnimRamps[t->blink].t <= t->blinkLevel)) {
        return;
    }
    bgDrawSetup(t->viewer, 1);
    DL_PUSH(0xB6000000, 0x2000);             /* no culling */
    DL_PUSH(0xFD100000, gFontPal);           /* load the fonts' palette */
    DL_PUSH(0xE8000000, 0);
    DL_PUSH(0xF5000100, 0x07000000);
    DL_PUSH(0xE6000000, 0);
    DL_PUSH(0xF0000000, 0x073FC000);
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xBA000E02, 0x8000);             /* RGBA16 palettes */
    textDrawSprites(t);
    DL_PUSH(0xBA000E02, 0);
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xB7000000, 0x2000);
}

/* 0x8011A69C - the race position picture (1st, 2nd...) */
void hudDrawPlace(s32 place, s32 x, s32 y) {
    drawDL((void *)((u32 *)gHudImages)[place - 1], x, y, 0, 0x10000);
}

/* 0x8011A6DC - a 5x5 character, 'shift' times doubled, into a 16-bit picture
 * (columns of bits; colours with the low bit clear are not drawn) */
void smallFontDrawChar(u16 *pic, s32 w, s32 h, u16 fg, u16 bg, s32 shift, s32 x, s32 y, u8 c) {
    u8 *cols = smallFontGlyph(c);
    s32 size, i, j, py;
    u8 bits;
    u16 v;

    if (cols == NULL) {
        return;
    }
    size = 5 << shift;
    for (i = 0; i < size; i++, x++) {
        bits = cols[i >> shift];
        for (j = 0, py = y; j < size; j++, py++) {
            if (x >= 0 && x < w && py >= 0 && py < h) {
                v = (bits & (1 << (j >> shift))) ? fg : bg;
                if (v & 1) {
                    pic[py * w + x] = v;
                }
            }
        }
    }
}

/* 0x8011A8D0 - size of a small-font character; returns 1 if it has none */
s32 smallFontCharSize(s32 *w, s32 *h, u8 c) {
    if (w != NULL) {
        *w = 5;
    }
    if (h != NULL) {
        *h = 5;
    }
    if (c == ' ') {
        return 0;
    }
    if (smallFontGlyph(c) == NULL) {
        return 1;
    }
    return 0;
}

void textDrawLayer(s32 layer);           /* 0x8011AA50 */

/* 0x8011A934 - draw all the text objects: the first layer, then the second */
void drawSpriteLayers(void) {
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xB6000000, 0x2000);
    textDrawLayer(0);
    textDrawLayer(TEXT_LAYER2);
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xB7000000, 0x2000);
}

/* 0x8011A9D4 - free all sprites and text objects */
void textFreeAll(void) {
    TextSprite *s;
    s32 k;

    for (s = gTextSprites; s != &gTextSprites[NUM_SPRITES]; s++) {
        for (k = 0; k < 4; k++) {
            s->v[k].v.flag = 0;
            s->v[k].v.ob[2] = 0;
        }
    }
    listInitPool(&gTextFree, (u8 *)gTextPool, sizeof(TextObj), 50);
    gTextActive.tail = NULL;
    gTextActive.head = NULL;
}

/* 0x8011AA50 - draw the shown text objects of a layer */
void textDrawLayer(s32 layer) {
    TextObj *t;
    s32 viewer = 99;

    DL_PUSH(0xFD100000, gFontPal);
    DL_PUSH(0xE8000000, 0);
    DL_PUSH(0xF5000100, 0x07000000);
    DL_PUSH(0xE6000000, 0);
    DL_PUSH(0xF0000000, 0x073FC000);
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xBA000E02, 0x8000);
    for (t = (TextObj *)gTextActive.head; t != NULL; t = (TextObj *)t->node.next) {
        if (t->xf.visible == 0 || (t->flags & TEXTF_HIDDEN) || (t->font & TEXT_LAYER2) != layer) {
            continue;
        }
        if (t->blink != -1 && !(gAnimRamps[t->blink].t <= t->blinkLevel)) {
            continue;
        }
        if (t->viewer != viewer) {
            bgDrawSetup(t->viewer, 1);
            viewer = t->viewer;
        }
        textDrawSprites(t);
    }
    DL_PUSH(0xBA000E02, 0);
}

/* 0x8011AC34 - where a line starts for an alignment (0 left, 1 right, 2 centre) */
s32 textAlignX(char *s, s32 style, TextObj *t) {
    s32 w = textWidth(s, t->font);

    switch (style) {
        case 0:
            return 0;
        case 1:
            return 1 - w;
        case 2:
            return -(w >> 1);
    }
    return w;
}

/* 0x8011AC98 - lay out a string: one sprite per character, lines going down */
void textSetup(TextObj *t, char *s, s32 style) {
    s32 x = textAlignX(s, style, t);
    s32 y = 0;
    s32 lineH = 0;
    s32 sprite = -1;
    s32 count = 0;
    s32 font = t->font;
    s32 slant = (font & TEXT_SLANT) ? 2 : 0;
    Font *f;
    Glyph *g;
    s32 w, h, gi, hi, lo;
    u8 c;

    if (textNextSprite(NULL, &sprite)) {
        t->sprite = sprite;
        f = &gFonts[font & TEXT_FONT];
        for (c = *s++; c != 0; c = *s++) {
            if (c == '\n') {
                x = textAlignX(s, style, t);
                y = y - lineH - f->spacing;
                lineH = 0;
                continue;
            }
            hi = ((u32)t >> 16) & 0xFFFF;
            lo = (u32)t & 0xFFFF;
            if (c != ' ' && (g = glyphFind(c, font)) != NULL) {
                w = g->w;
                h = g->h;
                gi = (((u8 *)g - (u8 *)f->glyphs) >> 3) & 0xFFFF;
            } else {
                gi = 0xFFFF;
                w = f->spaceW;
                h = f->height;
            }
            if (lineH < h) {
                lineH = h;
            }
            spriteSetVtx(sprite, 0, x + slant, y, 0, 0, hi);
            spriteSetVtx(sprite, 1, x + slant + w, y, w << 6, 0, lo);
            spriteSetVtx(sprite, 2, x - slant + w, y - h, w << 6, h << 6, gi);
            spriteSetVtx(sprite, 3, x - slant, y - h, 0, h << 6, 0);
            count++;
            x = x + w + f->spacing;
            if (!textNextSprite(NULL, &sprite)) {
                break;
            }
        }
    }
    t->count = count;
}

/* 0x8011AF34 */
void textNop(void) {
}

/* 0x8011AF3C - a text object's characters: each glyph's texels loaded as
 * colour-indexed (font palette), its quad as two triangles */
void textDrawSprites(TextObj *t) {
    s32 sprite = t->sprite - 1;
    s32 font = t->font & TEXT_FONT;
    s32 n = t->count;
    u32 img = gFontImages[font];
    s32 *m;
    Glyph *g;
    Vtx *v;
    s32 gi, w8, h, texels, lrs, words, dxt;

    if (t->flags & TEXTF_COLOR) {
        DL_PUSH(0xE7000000, 0);
        DL_PUSH(0xFA000000, t->color);
    }
    m = gMtxCursor;
    gMtxCursor = m + 16;
    mtxFromAngles(m, t->xf.unk0C, t->xf.unk10, t->xf.unk14);
    m[12] = t->xf.x;
    m[13] = t->xf.y;
    m[14] = t->xf.z;
    if (t->xf.scale != 0x10000) {
        mtxScaleBy(m, t->xf.scale, t->xf.scale, t->xf.scale);
    }
    DL_PUSH(0x01040040, (u32)(m - 16));
    while (n-- != 0) {
        textNextSprite(t, &sprite);
        gi = gTextSprites[sprite].v[2].v.flag;
        if (gi == 0xFFFF) {
            continue;
        }
        g = &gFonts[font].glyphs[gi];
        w8 = ((g->w + 7) >> 3) << 3;
        h = g->h;
        DL_PUSH(0xFD500000, img + g->offset);    /* CI, loaded as 16-bit */
        DL_PUSH(0xF5500000, 0x07080200);
        DL_PUSH(0xE6000000, 0);
        texels = ((w8 * h + 1) >> 1) - 1;
        lrs = (texels < 0x7FF) ? texels : 0x7FF;
        words = w8 / 8;
        if (words <= 0) {
            words = 1;
        }
        dxt = (words + 0x7FF) / words;
        DL_PUSH(0xF3000000, 0x07000000 | ((lrs & 0xFFF) << 12) | (dxt & 0xFFF));
        DL_PUSH(0xE7000000, 0);
        DL_PUSH(0xF5480000 | ((((w8 + 7) >> 3) & 0x1FF) << 9), 0x00080200);
        DL_PUSH(0xF2000000, ((((w8 - 1) << 2) & 0xFFF) << 12) | (((h - 1) << 2) & 0xFFF));
        DL_PUSH(0x04300040, (u32)gVtxCursor);
        v = (Vtx *)gVtxCursor;
        v[0] = gTextSprites[sprite].v[0];
        v[1] = gTextSprites[sprite].v[1];
        v[2] = gTextSprites[sprite].v[2];
        v[3] = gTextSprites[sprite].v[3];
        gVtxCursor += 4 * sizeof(Vtx);
        DL_PUSH(0xBF000000, 0x0A14);             /* 0, 1, 2 */
        DL_PUSH(0xBF000000, 0x1E14);             /* 0, 3, 2 */
    }
    if (t->flags & TEXTF_COLOR) {
        DL_PUSH(0xE7000000, 0);
        DL_PUSH(0xFA000000, 0xFFFFFFFF);
    }
    DL_PUSH(0xBD000000, 0);
}

/* 0x8011B500 - a character of a font (NULL: not in it) */
Glyph *glyphFind(u8 c, s32 font) {
    Glyph *g = gFonts[font & TEXT_FONT].glyphs;

    for (; g->ch != 0; g++) {
        if (c == g->ch) {
            return g;
        }
    }
    return NULL;
}

/* 0x8011B55C - one corner of a sprite (white, texture coordinates in 1/32) */
void spriteSetVtx(s32 sprite, s32 corner, s32 x, s32 y, s32 s, s32 t, s32 flag) {
    Vtx *v = &gTextSprites[sprite].v[corner];

    v->v.ob[0] = x;
    v->v.ob[1] = y;
    v->v.cn[0] = 0xFF;
    v->v.cn[1] = 0xFF;
    v->v.cn[2] = 0xFF;
    v->v.cn[3] = 0xFF;
    v->v.flag = flag;
    v->v.tc[0] = s;
    v->v.tc[1] = t;
}

/* 0x8011B5B0 - the next sprite after *sprite belonging to t (NULL: a free one) */
s32 textNextSprite(TextObj *t, s32 *sprite) {
    u16 hi = (u32)t >> 16;
    u16 lo = (u32)t & 0xFFFF;
    s32 i;

    for (i = *sprite + 1; i < NUM_SPRITES; i++) {
        if (gTextSprites[i].v[0].v.flag == hi && gTextSprites[i].v[1].v.flag == lo) {
            break;
        }
    }
    if (i >= NUM_SPRITES) {
        return 0;
    }
    *sprite = i;
    return 1;
}

/* 0x8011B648 - a small-font character's five columns (NULL: none) */
u8 *smallFontGlyph(u8 c) {
    s32 i;

    if (c >= 'A' && c <= 'Z') {
        return &gSmallFont[(c - 'A') * 5];
    }
    if (c >= '0' && c <= '9') {
        return &gSmallFont[(c - '0') * 5 + 130];
    }
    for (i = 0; gSmallFontChars[i] != 0; i++) {
        if (c == gSmallFontChars[i]) {
            return &gSmallFont[i * 5 + 180];
        }
    }
    return NULL;
}
