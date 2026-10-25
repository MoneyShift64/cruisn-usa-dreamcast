/*
 * slider.c - the volume sliders of the pause menu and the options screen, and
 * the box behind an option.
 * Original code: main segment, RAM 0x8011E260-0x8011E628.
 */
#include "common.h"
#include "frontend.h"

typedef struct Rgb { s32 r, g, b; } Rgb;

extern Rgb  gSliderBarColor[2];          /* 0x80134E20 - [selected] */
extern Rgb  gSliderTickColor[2];         /* 0x80134E38 */
extern u32 *gDlCursor;                   /* 0x80147684 */

void sliderBox(void *dl, s32 x, s32 y, s32 selected, s32 alpha);

#define DL_PUSH(w0, w1) { u32 *_g = gDlCursor; gDlCursor = _g + 2; _g[0] = (w0); _g[1] = (u32)(w1); }

/* 0x8011E260 - a slider: gfx[0] the box, gfx[1]/[2] its ends; nine ticks and a bar
 * as long as value (0..1). Not selected: dimmed. */
void drawSlider(void *gfx, f32 value, s32 x, s32 y, s32 selected, s32 alpha) {
    void **dl = (void **)gfx;
    Rgb *c;
    s32 i, h, tx;

    sliderBox(dl[0], x, y, selected, alpha);
    if (selected == 0) {
        DL_PUSH(0xE7000000, 0);
        DL_PUSH(0xFA000000, (alpha & 0xFF) | 0x99999900);
    }
    drawDL(dl[1], x + 0x290000, y, 0, 0x10000);
    drawDL(dl[2], x - 0x270000, y, 0, 0x10000);
    if (selected == 0) {
        DL_PUSH(0xE7000000, 0);
        DL_PUSH(0xFA000000, 0xFFFFFFFF);
    }
    c = &gSliderTickColor[selected];
    tx = x - 0x1C0000;
    for (i = 0; i < 9; i++, tx += 0x70000) {
        h = (i & 1) ? 2 : 4;
        rectFill(1, h, tx, (y - (s32)((f64)((8 - h) / 2) * 65536.0)) + 0x40000, c->r, c->g, c->b, alpha);
    }
    c = &gSliderBarColor[selected];
    rectFill((s32)(value * 68.0f), 8, x - 0x220000, y + 0x40000, c->r, c->g, c->b, alpha);
}

/* 0x8011E4D4 - the box behind an option (not selected: dimmed), and its dark inside */
void sliderBox(void *dl, s32 x, s32 y, s32 selected, s32 alpha) {
    s32 grey;

    if (selected == 0) {
        DL_PUSH(0xE7000000, 0);
        DL_PUSH(0xFA000000, (alpha & 0xFF) | 0x99999900);
    }
    drawDL(dl, x, y, 0, 0x10000);
    if (selected == 0) {
        DL_PUSH(0xE7000000, 0);
        DL_PUSH(0xFA000000, 0xFFFFFFFF);
    }
    grey = (selected != 0) ? 30 : 18;
    rectFill(0x44, 8, x - 0x220000, y + 0x40000, grey, grey, grey, alpha);
}
