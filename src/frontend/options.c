/*
 * options.c - the GAME OPTIONS screen (B in the menus) and its pieces.
 * Original code: front-end overlay, RAM 0x80030DE0-0x80035294 (asm/69B70.s).
 *
 * Items (gOptSel): 0 MUSIC VOLUME, 1 SOUND FX VOLUME, 2 2-PLAYER SOUND,
 * 3 DIFFICULTY (only where the caller allows it), 4 RACERS, 5 TRAFFIC (both
 * fixed on in Cruise the USA), 6 METRIC, 7 CENTER SCREEN, 8 CONTROLLER SETUP,
 * 9 PAK COPY (needs two controller paks), 10 RETURN TO GAME.
 *
 * gOptState: 0 closed, 1 the list, 2 controller setup, 3 center screen,
 * 4 pak copy.
 *
 * CONTROLLER SETUP (gOptState 2, "cs" below): per player, the buttons of GAS,
 * BRAKE, SHIFT-UP, SHIFT-DN, DISPLAY, RADIO, VIEW (up to two each: START on a
 * row, then press them), the STEER response meter, and EXIT; B cycles the four
 * presets. Port: the Dreamcast's buttons and pictures (CS_BUTTONS).
 *
 * Port: PAK COPY (cartridge <-> controller pak) has no Dreamcast use: shown
 * dimmed and skipped. Saving the settings on the way out (0x80035294) is
 * port/save.c's saveSettings (the VMU).
 */
#include "common.h"
#include "frontend.h"

typedef struct MeterColor { s32 r, g, b, a; } MeterColor;

typedef struct OptText {                 /* 0x14 bytes: a fixed label */
    /* 0x00 */ char *text;
    /* 0x04 */ s32 x, y;                 /* 16.16 */
    /* 0x0C */ u32 color;
    /* 0x10 */ TextObj *obj;
} OptText;

typedef struct OptLine {                 /* 0x10 bytes: a one-pixel line */
    /* 0x00 */ s32 x0, x1, y;            /* 16.16 */
    /* 0x0C */ u8 r, g, b, a;
} OptLine;

typedef struct OptItem {                 /* the label of item n: list[index] */
    OptText *list;
    s32 index;
} OptItem;

typedef struct OptIcon { s32 dl, dl2; } OptIcon; /* gOptDLs indices, dl2 -1: none */

typedef struct CsButton { u32 mask; s32 icon; } CsButton; /* a button and its gOptIcons picture */
typedef struct CsAction { s32 id; u32 *masks; } CsAction; /* a row and its per-player button masks */

typedef struct Controls {                /* 0x18 bytes (src/input.c) */
    s32 gas, brake;
    f32 steer, steerDigital;
    u32 cheats;
    s32 steerMode;                       /* 0..4 */
} Controls;

extern MeterColor gMeterColors[5];       /* 0x80042F34 ovl3 - easy (green) .. hardest (red) */
extern f64        gMeterDim;             /* 0x80045B98 ovl3 - 0.6 */
extern u32       *gDlCursor;             /* 0x80147684 */

extern OptLine  gOptCenterLines[2];      /* 0x80042CF0 ovl3 */
extern OptText  gOptCenterText[4];       /* 0x80042D10 ovl3 - USE .. TO POSITION THE / 4 FLASHING CORNERS ... */
extern OptLine  gOptLines[4];            /* 0x80042D60 ovl3 */
extern OptText  gOptTextA[4];            /* 0x80042DA0 ovl3 - MUSIC VOLUME .. DIFFICULTY */
extern OptText  gOptTextB[3];            /* 0x80042DF0 ovl3 - RACERS, TRAFFIC, METRIC */
extern OptText  gOptTextC[4];            /* 0x80042E2C ovl3 - CENTER SCREEN .. RETURN TO GAME */
extern s32      gOptStartSel[5];         /* 0x80042F84 ovl3 - where the cursor starts (by how we got here) */
extern OptItem  gOptItems[11];           /* 0x80042F98 ovl3 */
extern u32      gOpt2PColors[2][2];      /* 0x80042FF0 ovl3 - [selected][split] */
extern u32      gOptOnOffColors[2][2];   /* 0x80043000 ovl3 - [selected][on] */
extern s32      gOptSliderY[2];          /* 0x80043010 ovl3 */
extern s32      gOptOnOffY[3];           /* 0x80043018 ovl3 */
extern OptIcon  gOptIcons[15];           /* 0x800430B0 ovl3 */
extern f32      gOptVolUp;               /* 0x80045BA0 ovl3 - 1/68 a frame */
extern f32      gOptVolDown;             /* 0x80045BA4 ovl3 */
extern f64      gOptFlashStep;           /* 0x80045BB0 ovl3 - 0.08 */

extern OptLine  gCsLines[8];             /* 0x80042BB0 ovl3 */
extern OptText  gCsText[8];              /* 0x80042C30 ovl3 - GAS .. STEER */
extern CsButton gCsButtons[15];          /* 0x80042E7C ovl3 */
#ifdef PORT
/* the Dreamcast's buttons (dc/game/plat_dc.c plat_pad) and their pictures: the
 * N64's A, B, C-up and C-down pictures are drawn as the Dreamcast's A, X, B
 * and Y (port/gfx.c gRepl), the triggers as the N64's L and R */
static CsButton gCsButtonsDC[] = {
    { 0x0800, 0 }, { 0x0400, 1 }, { 0x0200, 2 }, { 0x0100, 3 },   /* d-pad */
    { 0x0020, 8 }, { 0x0010, 9 },                                 /* left / right trigger */
    { 0x8000, 12 }, { 0x4000, 13 }, { 0x0040, 4 }, { 0x0080, 5 }, /* A, X, B, Y */
    { 0x10000, 10 }, { 0x20000, 11 },                             /* stick up / down */
};
#define CS_BUTTONS gCsButtonsDC
#define CS_NBUTTONS ((s32)(sizeof(gCsButtonsDC) / sizeof(gCsButtonsDC[0])))
#else
#define CS_BUTTONS gCsButtons
#define CS_NBUTTONS 15
#endif
extern CsAction gCsActions[7];           /* 0x80042EF4 ovl3 - gGasMask .. gBtnView */
extern s32      gCsHintTimes[2][4];      /* 0x80043034 ovl3 - frames of each fade phase */
extern s32      gCsSteerX[2];            /* 0x80043054 ovl3 */
extern s32      gCsColX[4];              /* 0x80043064 ovl3 - [player * 3] */
extern s32      gCsIconW[15];            /* 0x80043074 ovl3 */
extern s32      gCsExitX[2];             /* 0x80043128 ovl3 */
extern s32      gCsBoxX[2];              /* 0x80043138 ovl3 */
extern f64      gCsFlashStep;            /* 0x80045BB8 ovl3 - 0.06 */
extern f64      gCsFadeIn;               /* 0x80045BC0 ovl3 - 255 */
extern f64      gCsFadeOut;              /* 0x80045BC8 ovl3 - 255 */

extern void    *gOptDLs[48];             /* 0x800494B0 ovl3 - group 0x2BF9 */
extern s32      gOptState;               /* 0x80049570 ovl3 */
extern ModeFunc gOptReturn;              /* 0x80049574 ovl3 */
extern s32      gOptAllowDiff;           /* 0x80049578 ovl3 */
extern u32      gOptHeapMark;            /* 0x8004957C ovl3 */
extern TextObj *gOptCenterTitle;         /* 0x80049590 ovl3 */
extern TextObj *gOptTitle;               /* 0x800495A4 ovl3 - GAME OPTIONS */
extern TextObj *gOpt2PText;              /* 0x800495A8 ovl3 - COMBINED / SPLIT L/R */
extern TextObj *gOptOnOff[3];            /* 0x800495B0 ovl3 */
extern f32      gOptFlash;               /* 0x800495BC ovl3 - center screen corners, 0..1 */
extern s32      gOptPulse;               /* 0x800495C0 ovl3 - title glow angle */
extern s32      gOptRepeat;              /* 0x800495C4 ovl3 - volume preview sound every 9 frames */
extern s32      gOptSel;                 /* 0x800495C8 ovl3 */
extern s32      gOptToggles[3];          /* 0x800495D0 ovl3 - racers, traffic, metric (being edited) */
extern s32      gOptCenterPlayer;        /* 0x800495DC ovl3 */
extern s32      gOptNoMusic;             /* 0x800496E8 ovl3 - no tune was playing: preview one on MUSIC */

extern TextObj *gCsGamepad;                /* 0x80049580 ovl3 - GAMEPAD (made, never drawn) */
extern TextObj *gCsExit[2];              /* 0x80049588 ovl3 */
extern s32      gCsSlots[2][8][2];       /* 0x80049610 ovl3 - per player, row: up to two gOptIcons, -1 none */
extern s32      gCsCursor[2];            /* 0x80049690 ovl3 - row 0..6, 7 STEER, 8 EXIT */
extern s32      gCsBoxY[2];              /* 0x80049698 ovl3 - the cursor box (eases to its row) */
extern s32      gCsDim[2];               /* 0x800496A0 ovl3 - no controller, or done */
extern s32      gCsDone[2];              /* 0x800496A8 ovl3 */
extern s32      gCsMissing[2];           /* 0x800496B0 ovl3 - EXIT with a row unset: those flash */
extern s32      gCsPreset[2];            /* 0x800496B8 ovl3 */
extern s32      gCsEditing[2];           /* 0x800496C0 ovl3 */
extern u32      gCsCaptured[2][2];       /* 0x800496C8 ovl3 */
extern s32      gCsHintPhase;            /* 0x800496D8 ovl3 - 0 off, 1 in, 2 on, 3 out */
extern s32      gCsHint;                 /* 0x800496DC ovl3 - 0 B FOR PRESETS, 1 START TO EDIT */
extern s32      gCsHintTimer;            /* 0x800496E0 ovl3 */
extern s32      gCsBlink;                /* 0x800496E4 ovl3 */

extern u32      gGasMask[2];             /* 0x8014E330 */
extern s32      gGasAxis[2];             /* 0x8014E338 - 1 / -1: stick up / down */
extern s32      gBrakeAxis[2];           /* 0x8014E348 */
extern Controls gControls[2];            /* 0x8014E378 */

extern s16      gViewLeftX;              /* 0x8014EF80 - screen shift (pixels) */
extern s16      gViewTopY;               /* 0x8014EF82 */

void sliderBox(void *dl, s32 x, s32 y, s32 selected, s32 alpha); /* 0x8011E4D4 */
void drawSlider(void *gfx, f32 value, s32 x, s32 y, s32 selected, s32 alpha); /* 0x8011E260 */
void volumeApply(s32 channel, s32 preview); /* 0x8011E628 */
s32  musicCurrent(void);                 /* 0x8011F8D8 */
s32  tblCos(s32 angle);                  /* 0x80113EDC */
void controlsSetMapping(s32 player, s32 config); /* 0x80112370 */

void optionsMeter(s32 x, s32 y, s32 selected, s32 value, s32 alpha, s32 unused, void *box, void *dl1,
                  void *dl2, void *tick, void *marker);
void optionsInit(void);
static void optionsOpen(s32 from);
static void optionsFrame(s32 arg);
static void optionsList(void);
static void optionsClose(void);
static void optionsDifficulty(s32 value, s32 x, s32 y, s32 selected);
static void options2PText(void);
static void optionsOnOffText(s32 i);
static void optionsSave(void);
static void centerOpen(s32 player);
static void centerFrame(void);
static void centerCorners(void);
static void optBackground(void);
static void optLines(OptLine *l, s32 n);
static void optIcon(s32 i, s32 x, s32 y);
static void optTextsDraw(OptText *t, s32 n);
static void optTextsCreate(OptText *t, s32 n, s32 style);
static void optTextsFree(OptText *t, s32 n);
static s32  optPulse(void);
static void csOpen(void);
static void csFrame(void);
static void csDimUpdate(s32 p);
static void csHintDraw(void);
static void csSteerDraw(void);
static void csSlotsDraw(void);
static void csSlotDraw(s32 p, s32 row);
static void csTextsDraw(void);
static void csTextsCreate(void);
static void csClose(void);
static void csApply(s32 p);
static void csIcons(s32 p);
static u32  csIconsFor(u32 mask);
static void csBoxesDraw(void);
static void csFrameBox(s32 w, s32 h, s32 x, s32 y, s32 r, s32 g, s32 b, s32 a);
static void csCaptureReset(s32 p);
static void csEdit(s32 p);
static u32  csCapture(s32 p);
static s32  csComplete(s32 p);

#define DL_PUSH(w0, w1) { u32 *_g = gDlCursor; gDlCursor = _g + 2; _g[0] = (w0); _g[1] = (u32)(w1); }

/* the items the cursor skips (and the list shows dimmed) */
static s32 optSkipped(s32 i) {
    if (i == 3 && gOptAllowDiff == 0) {
        return 1;
    }
    if ((i == 4 || i == 5) && gCruiseMode != 0) {
        return 1;
    }
    if (i == 9 && gContReadState < 2) {
        return 1;
    }
#ifdef PORT
    if (i == 9) {                        /* pak copy: not on the Dreamcast yet */
        return 1;
    }
    if (i == 2 && !P2_ALLOWED) {         /* 2-player sound: 2-player games are off for now */
        return 1;
    }
#endif
    return 0;
}

/* 0x80030DE0 ovl3 - from a menu: B on any controller opens the options; the
 * menu ('returnTo') carries on when they close. allowDifficulty: show DIFFICULTY. */
s32 optionsCheck(ModeFunc returnTo, s32 allowDifficulty) {
    s32 p;

    for (p = 0; p < gNumControllers; p++) {
        if (contPressed(p, BTN_B) & BTN_B) {
            gOptReturn = returnTo;
            gOptAllowDiff = allowDifficulty;
            optionsInit();
            return 1;
        }
    }
    return 0;
}

/* 0x80030E74 ovl3 - a five-step meter (DIFFICULTY): its box, both ends, the steps,
 * and the marker on step 'value' (0..4; other values: none) in that step's colour */
void optionsMeter(s32 x, s32 y, s32 selected, s32 value, s32 alpha, s32 unused, void *box, void *dl1,
                  void *dl2, void *tick, void *marker) {
    MeterColor *c;
    s32 i, tx;
    f32 f;

    sliderBox(box, x, y, selected, alpha);
    if (selected == 0) {
        DL_PUSH(0xE7000000, 0);
        DL_PUSH(0xFA000000, (alpha & 0xFF) | 0x99999900);
    }
    drawDL(dl1, x, y, 0, 0x10000);
    drawDL(dl2, x, y, 0, 0x10000);
    tx = x - 0x160000;
    for (i = 0; i != 0x380000; i += 0xE0000, tx += 0xE0000) {
        drawDL(tick, tx, y, 0, 0x10000);
    }
    if (value >= 0 && value < 5) {
        f = (selected != 0) ? 1.0f : (f32)gMeterDim;
        c = &gMeterColors[value];
        DL_PUSH(0xE7000000, 0);
        DL_PUSH(0xFA000000, (((u32)((f32)c->b * f) & 0xFF) << 8) | ((u32)((f32)c->r * f) << 24) |
                            (((u32)((f32)c->g * f) & 0xFF) << 16) | (alpha & 0xFF));
        drawDL(marker, x + value * 0xE0000 - 0x1D0000, y, 0, 0x10000);
    }
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xFA000000, 0xFFFFFFFF);
}

/* 0x80031284 ovl3 - load the screen's pictures and open the list */
void optionsInit(void) {
    gNoPlayerInput = 1;
    gOptHeapMark = lowMemOverlayLoad();
    assetLoadGroup(0x2BF9, gOptDLs);
    lowMemOverlayRelease();
    gOptFlash = 0.0f;
    optionsOpen(0);
    sfxPlay(0x2C, -1);
    gModeFunc = optionsFrame;
}

/* 0x800312FC ovl3 */
static void optionsFrame(s32 arg) {
    switch (gOptState) {
    case 1:
        optionsList();
        break;
    case 2:
        csFrame();
        break;
    case 3:
        centerFrame();
        break;
    }
}

/* 0x80031384 ovl3 - the list, with the cursor on gOptStartSel[from] */
static void optionsOpen(s32 from) {
    s32 i;

    gOptState = 1;
    gOptNoMusic = musicCurrent() < 0;
    gOptPulse = 0;
    gOptSel = gOptStartSel[from];
    gOptToggles[0] = gOptRacers;
    gOptToggles[1] = gOptTraffic;
    gOptToggles[2] = gUnitsKph;
    options2PText();
    for (i = 0; i < 3; i++) {
        optionsOnOffText(i);
    }
    optTextsCreate(gOptTextA, 4, 1);
    optTextsCreate(gOptTextB, 3, 1);
    optTextsCreate(gOptTextC, 4, 2);
    gOptTitle = textCreate(2, "GAME OPTIONS", 2);
    textPlace(&gOptTitle->xf, 0x8B0000, 0xC80000, 0, 0x10000);
    gOptTitle->flags |= 3;
    for (i = 0; i != 2; i++) {
        contPressed(i, BTN_ALL);
    }
    gOptRepeat = 0;
    optionsList();
}

/* 0x800314E8 ovl3 - one frame of the list */
static void optionsList(void) {
    s32 p, n, old, sel, act, i;
    u32 held;
    f32 *vol;
    f32 was;
    OptItem *it;
    TextObj *t;

    gOptPulse += 0x800;
    n = gNumControllers;
    for (p = 0; p < n; p++) {
        old = gOptSel;
        act = 0;
        if (contPressed(p, 0x100800) & 0x100800) {
            do {
                gOptSel--;
            } while (optSkipped(gOptSel));
        }
        if (contPressed(p, 0x200400) & 0x200400) {
            do {
                gOptSel++;
            } while (optSkipped(gOptSel));
        }
        if (gOptSel < 0) {
            gOptSel = 10;
        }
        if (gOptSel >= 11) {
            gOptSel = 0;
        }
        if (old != gOptSel) {
            sfxPlay(0x80, -1);
            if (gOptNoMusic != 0) {
                if (gOptSel == 0) {
                    musicPlay(0xA1);
                } else if (old == 0) {
                    musicStop();
                }
            }
        }
        if (contPressed(p, 0x800100) & 0x800100) {
            act = 1;
        }
        if (contPressed(p, 0x400200) & 0x400200) {
            act = 2;
        }
        if (contPressed(p, 0x9000) & 0x9000) {
            act = 3;
        }

        sel = gOptSel;
        if (sel < 2) {
            /* volumes: held left / right slide */
            held = gPads[p].held;
            vol = &gVolume[sel];
            was = *vol;
            if (held & BTN_RIGHT) {
                *vol = was + gOptVolUp;
            }
            if (held & BTN_LEFT) {
                *vol -= gOptVolDown;
            }
            if ((f64)*vol < 0.0) {
                *vol = 0.0f;
            }
            if ((f64)*vol > 1.0) {
                *vol = 1.0f;
            }
            if (was != *vol) {
                volumeApply(sel, gOptRepeat == 0);
            }
        } else if (sel == 2) {
            if (act == 1 || act == 2) {
                gOpt2PSound ^= 1;
                textFree(gOpt2PText);
                options2PText();
                sfxPlay((gOpt2PSound != 0) ? 0x1D : 0x1E, -1);
            }
        } else if (sel == 3) {
            old = gDifficulty;
            if (act == 1) {
                gDifficulty++;
            }
            if (act == 2) {
                gDifficulty--;
            }
            if (gDifficulty < 0) {
                gDifficulty = 0;
            }
            if (gDifficulty >= 5) {
                gDifficulty = 4;
            }
            if (old != gDifficulty) {
                sfxPlay(0x28, -1);
            }
        } else if (sel >= 4 && sel < 7) {
            i = sel - 4;
            if (act == 1 || act == 2) {
                gOptToggles[i] ^= 1;
                textFree(gOptOnOff[i]);
                optionsOnOffText(i);
                sfxPlay((gOptToggles[i] != 0) ? 0x1D : 0x1E, -1);
            }
        } else if (sel == 8) {
            if (act == 3) {
                sfxPlay(0x2C, -1);
                optionsClose();
                csOpen();
                return;
            }
        } else if (sel == 7) {
            if (act == 3) {
                sfxPlay(0x2C, -1);
                optionsClose();
                centerOpen(p);
                return;
            }
        } else if (sel == 10 && act == 3) {
            sfxPlay(0x2C, -1);
            optionsClose();
            gOptState = 0;
            lowMemOverlayLoad();
            highHeapRestore(gOptHeapMark);
            lowMemOverlayRelease();
            raceOptionsApply();
            gModeFunc = gOptReturn;
            gNoPlayerInput = 0;
            gModeFunc(gModeArg);
            optionsSave();
            return;
        }
    }

    /* the volume preview sound repeats while left / right is held */
    if (gOptSel < 2) {
        for (p = 0; p < n; p++) {
            if (gPads[p].held & (BTN_LEFT | BTN_RIGHT)) {
                if (--gOptRepeat < 0) {
                    gOptRepeat = 8;
                }
                break;
            }
        }
        if (p == n) {
            gOptRepeat = 0;
        }
    }

    bgDrawSetup(-1, 1);
    screenClear(-1, 1);
    optBackground();
    rectFill(0x140, 0xF0, -0x140000, 0xB00000, 0x14, 0x14, 0x14, 0x8C);
    optLines(gOptLines, 4);
    for (i = 0, it = gOptItems; i < 11; i++, it++) {
        t = it->list[it->index].obj;
        if (i == gOptSel) {
            t->color = 0xFFFFFFFF;
        } else if (optSkipped(i)) {
            t->color = 0x66666664;
        } else {
            t->color = 0x666666FF;
        }
    }
    gOptTitle->color = optPulse() * 0x01010100 + 0xFF;
    textDraw(gOptTitle, 0);
    optTextsDraw(gOptTextA, 4);
    optTextsDraw(gOptTextB, 3);
    optTextsDraw(gOptTextC, 4);
    gOpt2PText->color = gOpt2PColors[gOptSel == 2][gOpt2PSound];
    textDraw(gOpt2PText, 0);
    for (i = 0; i < 3; i++) {
        if (i != 2 && gCruiseMode != 0) {
            gOptOnOff[i]->color = 0x18341882;
        } else {
            gOptOnOff[i]->color = gOptOnOffColors[i + 4 == gOptSel][gOptToggles[i]];
        }
        textDraw(gOptOnOff[i], 0);
    }
    for (i = 0; i != 2; i++) {
        drawSlider(&gOptDLs[21], gVolume[i], 0xD90000, gOptSliderY[i], i == gOptSel, 0xFF);
    }
    optionsDifficulty(gDifficulty, 0xD90000, 0x7A0000, gOptSel == 3);
}

/* 0x80031F44 ovl3 - keep the toggles and take the list down */
static void optionsClose(void) {
    s32 i;

    gOptRacers = gOptToggles[0];
    gOptTraffic = gOptToggles[1];
    gUnitsKph = gOptToggles[2];
    textFree(gOptTitle);
    optTextsFree(gOptTextA, 4);
    optTextsFree(gOptTextB, 3);
    optTextsFree(gOptTextC, 4);
    textFree(gOpt2PText);
    for (i = 0; i < 3; i++) {
        textFree(gOptOnOff[i]);
    }
    for (i = 0; i < gNumControllers; i++) {
        contPressed(i, BTN_ALL);
    }
}

/* 0x80032038 / 0x80032094 ovl3 - the DIFFICULTY meter (faded when it can't be changed) */
static void optionsDifficulty(s32 value, s32 x, s32 y, s32 selected) {
    optionsMeter(x, y, selected, value, (gOptAllowDiff != 0) ? 0xFF : 0x80, 0, gOptDLs[21], gOptDLs[24],
                 gOptDLs[25], gOptDLs[26], gOptDLs[27]);
}

/* 0x800320F0 ovl3 */
static void options2PText(void) {
    gOpt2PText = textCreate(0, (gOpt2PSound != 0) ? "SPLIT L/R" : "COMBINED", 0);
    textPlace(&gOpt2PText->xf, 0xA40000, 0x910000, 0, 0x10000);
    gOpt2PText->flags |= 3;
}

/* 0x8003216C ovl3 - ON / OFF after RACERS, TRAFFIC, METRIC (racers and traffic
 * are always on in Cruise the USA) */
static void optionsOnOffText(s32 i) {
    char *s;

    if (gOptToggles[i] != 0 || (gCruiseMode != 0 && i != 2)) {
        s = "ON";
    } else {
        s = "OFF";
    }
    gOptOnOff[i] = textCreate(0, s, 0);
    textPlace(&gOptOnOff[i]->xf, 0xA40000, gOptOnOffY[i], 0, 0x10000);
    gOptOnOff[i]->flags |= 3;
}

/* 0x80035294 ovl3 - write the settings to the controller paks */
static void optionsSave(void) {
#ifdef PORT
    void saveSettings(void);             /* port/save.c: the VMU */
    saveSettings();
#endif
}

/* 0x80032FB4 ovl3 - CENTER SCREEN: 'player's stick moves the picture */
static void centerOpen(s32 player) {
    gOptCenterPlayer = player;
    gOptPulse = 0;
    gOptState = 3;
    optTextsCreate(gOptCenterText, 4, 2);
    gOptCenterTitle = textCreate(2, "CENTER SCREEN", 2);
    textPlace(&gOptCenterTitle->xf, 0x8B0000, 0xAA0000, 0, 0x10000);
    gOptCenterTitle->flags |= 3;
    centerFrame();
}

/* 0x80033054 ovl3 */
static void centerFrame(void) {
    s32 p;
    PadState *pad;

    p = (gNumControllers < 2) ? 0 : gOptCenterPlayer;
    gOptFlash = (f32)((f64)gOptFlash + gOptFlashStep);
    if ((f64)gOptFlash >= 1.0) {
        gOptFlash = 0.0f;
    }
    gOptPulse += 0x800;
    if (contPressed(p, 0x9000) & 0x9000) {
        sfxPlay(0x2C, -1);
        textFree(gOptCenterTitle);
        optTextsFree(gOptCenterText, 4);
        contPressed(0, BTN_ALL);
        contPressed(1, BTN_ALL);
        gOptState = 1;
        optionsOpen(3);
        return;
    }
    pad = &gPads[p];
    gViewLeftX = (s16)(s32)(((f64)pad->xRaw - 0.5) * 20.0);
    gViewTopY = (s16)(s32)(((f64)pad->yRaw - 0.5) * 20.0);
    bgDrawSetup(-1, 1);
    screenClear(-1, 1);
    optBackground();
    rectFill(0x140, 0xF0, -0x140000, 0xDB0000, 0x14, 0x14, 0x14, 0x8C);
    if ((f64)gOptFlash <= 0.5) {
        centerCorners();
    }
    drawDL(gOptDLs[17], 0x520000, 0x740000, 0, 0x10000);
    drawDL(gOptDLs[28], 0x520000, 0x740000, 0, 0x10000);
    optIcon(12, 0x820000, 0x2C0000);
    gOptCenterTitle->color = optPulse() * 0x01010100 + 0xFF;
    textDraw(gOptCenterTitle, 0);
    optTextsDraw(gOptCenterText, 4);
    optLines(gOptCenterLines, 2);
}

/* 0x80033310 ovl3 - the four white corners */
static void centerCorners(void) {
    rectFill(0x14, 5, -0x40000, 0xCC0000, 0xFF, 0xFF, 0xFF, 0xFF);
    rectFill(5, 0xF, -0x40000, 0xC70000, 0xFF, 0xFF, 0xFF, 0xFF);
    rectFill(0x14, 5, 0x01080000, 0xCC0000, 0xFF, 0xFF, 0xFF, 0xFF);
    rectFill(5, 0xF, 0x01170000, 0xC70000, 0xFF, 0xFF, 0xFF, 0xFF);
    rectFill(0x14, 5, -0x40000, 0x10000, 0xFF, 0xFF, 0xFF, 0xFF);
    rectFill(5, 0xF, -0x40000, 0x100000, 0xFF, 0xFF, 0xFF, 0xFF);
    rectFill(0x14, 5, 0x01080000, 0x10000, 0xFF, 0xFF, 0xFF, 0xFF);
    rectFill(5, 0xF, 0x01170000, 0x100000, 0xFF, 0xFF, 0xFF, 0xFF);
}

/* 0x80033E5C ovl3 - the backdrop, tinted */
static void optBackground(void) {
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xFA000000, 0x786464FF);
    drawDL(gOptDLs[0], 0xFFEC0000, 0xFFEC0000, 0, 0x10000);
    DL_PUSH(0xE7000000, 0);
    DL_PUSH(0xFA000000, 0xFFFFFFFF);
}

/* 0x80033F10 / 0x80033F78 ovl3 */
static void optLines(OptLine *l, s32 n) {
    s32 i;

    for (i = 0; i != n; i++, l++) {
        rectFill(((l->x1 - l->x0) >> 16) + 1, 1, l->x0, l->y, l->r, l->g, l->b, l->a);
    }
}

/* 0x80034498 ovl3 - a button picture */
static void optIcon(s32 i, s32 x, s32 y) {
    OptIcon *c = &gOptIcons[i];

    drawDL(gOptDLs[c->dl], x, y, 0, 0x10000);
    if (c->dl2 != -1) {
        drawDL(gOptDLs[c->dl2], x, y, 0, 0x10000);
    }
}

/* 0x80034538 ovl3 */
static void optTextsDraw(OptText *t, s32 n) {
    s32 i;

    for (i = 0; i != n; i++, t++) {
        textDraw(t->obj, 0);
    }
}

/* 0x80034588 ovl3 */
static void optTextsCreate(OptText *t, s32 n, s32 style) {
    s32 i;

    for (i = 0; i != n; i++, t++) {
        t->obj = textCreate(0, t->text, style);
        textPlace(&t->obj->xf, t->x, t->y, 0, 1);
        t->obj->color = t->color;
        t->obj->flags |= 3;
    }
}

/* 0x80034628 ovl3 */
static void optTextsFree(OptText *t, s32 n) {
    s32 i;

    for (i = 0; i != n; i++, t++) {
        textFree(t->obj);
    }
}

/* 0x80035228 ovl3 - about 128..255, following gOptPulse */
static s32 optPulse(void) {
    return (s32)((f64)((f32)((f64)(f32)tblCos(gOptPulse) / 32768.0) * 127.0f) + 192.0);
}

/* ------------------------------------------------------------------ */
/* CONTROLLER SETUP                                                    */
/* ------------------------------------------------------------------ */

/* 0x800334C8 ovl3 */
static void csOpen(void) {
    s32 p;

    gOptPulse = 0;
    gOptState = 2;
    gCsHintPhase = 0;
    gCsHint = 0;
    gCsHintTimer = 0;
    gCsBlink = 0;
    for (p = 0; p != 2; p++) {
        gCsCursor[p] = 8;
        gCsBoxY[p] = 0xF0000;
        gCsMissing[p] = 0;
        gCsPreset[p] = 0;
        gCsEditing[p] = 0;
        gCsDone[p] = 0;
        csIcons(p);
        csCaptureReset(p);
    }
    csTextsCreate();
    csFrame();
}

/* 0x800335EC ovl3 - one frame */
static void csFrame(void) {
    s32 p, q, old;
    u32 pr;
    Controls *c;

    for (p = 0; p < 2; p++) {
        csDimUpdate(p);
    }
    gOptFlash = (f32)((f64)gOptFlash + gCsFlashStep);
    if ((f64)gOptFlash >= 1.0) {
        gOptFlash = 0.0f;
    }
    gOptPulse += 0x800;
    for (p = 0; p < gNumControllers; p++) {
        if (gCsEditing[p] != 0) {
            /* START on a row: the next buttons pressed become its buttons, START again ends */
            csEdit(p);
            if (contPressed(p, BTN_START) & BTN_START) {
                sfxPlay(0x13, -1);
                csApply(p);
                gCsEditing[p] = 0;
                contPressed(p, BTN_ALL);
            }
            continue;
        }
        if (gCsDim[p] == 0) {
            old = gCsCursor[p];
            if (contPressed(p, 0x100800) & 0x100800) {
                gCsCursor[p]--;
            }
            if (contPressed(p, 0x200400) & 0x200400) {
                gCsCursor[p]++;
            }
            if (gCsCursor[p] < 0) {
                gCsCursor[p] = 8;
            }
            if (gCsCursor[p] >= 9) {
                gCsCursor[p] = 0;
            }
            if (old != gCsCursor[p]) {
                sfxPlay(0x24, -1);
            }
            if (contPressed(p, BTN_B) & BTN_B) {
                sfxPlay(0x13, -1);
#ifdef PORT
                gCsPreset[p] = 0;        /* (port) X: back to the Dreamcast default controls */
#else
                if (++gCsPreset[p] >= 4) {
                    gCsPreset[p] = 0;
                }
#endif
                controlsSetMapping(p, gCsPreset[p]);
                csIcons(p);
            }
        }
        if (gCsCursor[p] == 8) {
            /* EXIT: done (again: not done); everyone done leaves */
            if (contPressed(p, 0xC09300) & 0x9000) {
                contPressed(p, BTN_ALL);
                if (gCsDone[p] != 0) {
                    gCsDone[p] = 0;
                    csDimUpdate(p);
                } else if (csComplete(p)) {
                    gCsDone[p] = 1;
                    csDimUpdate(p);
                    q = (p == 0);
                    if (q >= gNumControllers || (gCsCursor[q] == 8 && csComplete(q))) {
                        sfxPlay(0x2C, -1);
                        csClose();
                        optionsOpen(2);
                        return;
                    }
                    sfxPlay(0x81, -1);
                } else {
                    sfxPlay(3, -1);
                    gCsMissing[p] = 1;
                }
            }
        } else if (gCsCursor[p] == 7) {
            pr = contPressed(p, 0xC09300) & 0xC00300;
            c = &gControls[p];
            if ((pr & 0x800100) && c->steerMode < 4) {
                c->steerMode++;
                sfxPlay(0x28, -1);
            }
            if ((pr & 0x400200) && c->steerMode > 0) {
                c->steerMode--;
                sfxPlay(0x28, -1);
            }
        } else if (contPressed(p, 0x9000) & BTN_START) {
            sfxPlay(0x13, -1);
            contPressed(p, BTN_ALL);
            gCsMissing[p] = 0;
            csCaptureReset(p);
            gCsEditing[p] = 1;
        }
    }

    bgDrawSetup(-1, 1);
    screenClear(-1, 1);
    optBackground();
    rectFill(0x140, 0xF0, -0x140000, 0xB00000, 0x14, 0x14, 0x14, 0x8C);
    csHintDraw();
    optLines(gCsLines, 8);
    csBoxesDraw();
    csSteerDraw();
    csSlotsDraw();
    csTextsDraw();
}

/* 0x80033AF4 ovl3 - "B FOR PRESETS" and "START TO EDIT" fading in and out in turn
 * (only the first once both players are on STEER / EXIT) */
static void csHintDraw(void) {
    s32 t, a;

    if (gCsBlink-- == 0) {
        gCsBlink = 15;
    }
    if (gCsHintTimer-- == 0) {
        if (gCsHintPhase++ == 3) {
            gCsHintPhase = 0;
            gCsHint ^= 1;
        }
        if (gCsCursor[0] >= 7 && gCsCursor[1] >= 7 && gCsHint != 0 && gCsHintPhase == 0) {
            gCsHint = 0;
        }
        gCsHintTimer = gCsHintTimes[gCsHint][gCsHintPhase];
    }
    t = gCsHintTimes[gCsHint][gCsHintPhase];
    if (gCsCursor[0] >= 7 && gCsCursor[1] >= 7) {
        if (gCsHint != 0) {
            if (gCsHintPhase == 2) {
                gCsHintTimer = 0;
            }
        } else if (gCsHintPhase == 2) {
            gCsHintTimer = t;
        }
    }
    a = 0;
    switch (gCsHintPhase) {
    case 1:
        a = (s32)(((f64)(t - gCsHintTimer) * gCsFadeIn) / (f64)t);
        break;
    case 2:
        a = 0xFF;
        break;
    case 3:
        a = (s32)(((f64)gCsHintTimer * gCsFadeOut) / (f64)t);
        break;
    }
    if (gCsHintPhase != 0) {
        DL_PUSH(0xE7000000, 0);
        DL_PUSH(0xFA000000, (a & 0xFF) | 0xFFFFFF00);
        if (gCsHint != 0) {
            drawDL(gOptDLs[30], 0x8B0000, 0x60000, 0, 0x10000);
            if (gCsBlink >= 5) {
                drawDL(gOptDLs[31], 0x8B0000, 0x60000, 0, 0x10000);
            }
        } else {
            drawDL(gOptDLs[29], 0x8B0000, 0x60000, 0, 0x10000);
        }
        DL_PUSH(0xE7000000, 0);
        DL_PUSH(0xFA000000, 0xFFFFFFFF);
    }
}

/* 0x80033E18 ovl3 - a player without a controller, or done, is shown dimmed */
static void csDimUpdate(s32 p) {
    gCsDim[p] = (p >= gNumControllers) || (gCsDone[p] != 0);
}

/* 0x8003406C / 0x80033FEC ovl3 - the STEER meters */
static void csSteerDraw(void) {
    s32 p, a;

    for (p = 0; p < 2; p++) {
        a = (gCsDim[p] != 0) ? 0x73 : 0xFF;
        optionsMeter(gCsSteerX[p], 0x1A0000, a == 0xFF, gControls[p].steerMode, a, 1, gOptDLs[21], gOptDLs[24],
                     gOptDLs[25], gOptDLs[26], gOptDLs[27]);
    }
}

/* 0x80034104 ovl3 - both players' buttons (the row being set flashes) */
static void csSlotsDraw(void) {
    s32 p, row;

    for (p = 0; p != 2; p++) {
        if (gCsDim[p] != 0) {
            DL_PUSH(0xE7000000, 0);
            DL_PUSH(0xFA000000, 0xFFFFFF73);
        }
        for (row = 0; row != 7; row++) {
            if (gCsEditing[p] == 0 || row != gCsCursor[p] || (f64)gOptFlash < 0.75) {
                csSlotDraw(p, row);
            }
        }
        if (gCsDim[p] != 0) {
            DL_PUSH(0xE7000000, 0);
            DL_PUSH(0xFA000000, 0xFFFFFFFF);
        }
    }
}

/* 0x8003428C ovl3 - one row's buttons: one, two side by side, or a "!" */
static void csSlotDraw(s32 p, s32 row) {
    s32 a = gCsSlots[p][row][0];
    s32 b = gCsSlots[p][row][1];
    s32 n = (a != -1) + (b != -1);
    s32 x = gCsColX[p * 3];
    s32 y = 0xBB0000 - row * 0x170000;
    s32 wa, wb, off;

    if (n == 1) {
        optIcon(a, x, y);
    } else if (n == 2) {
        wa = gCsIconW[a];
        wb = gCsIconW[b];
        off = x - ((((x - wa) - 0x10000) + (wb + x + 0x10000)) / 2 & 0xFFFF0000);
        optIcon(a, (x - wa / 2) - 0x10000 + off, y);
        optIcon(b, wb / 2 + x + 0x10000 + off, y);
    } else if (gCsMissing[p] == 0 || (f64)gOptFlash < 0.5) {
        drawDL(gOptDLs[3], x, y, 0, 0x10000);
    }
}

/* 0x80034678 / 0x800346A0 ovl3 */
static void csTextsDraw(void) {
    s32 p;

    optTextsDraw(gCsText, 8);
    for (p = 0; p < 2; p++) {
        gCsExit[p]->color = (gCsDim[p] != 0) ? 0x28286464 : 0x327832FF;
        textDraw(gCsExit[p], 0);
    }
    gCsGamepad->color = optPulse() * 0x01010100 + 0xFF;
}

/* 0x80034778 ovl3 */
static void csTextsCreate(void) {
    s32 p;

    optTextsCreate(gCsText, 8, 2);
    gCsGamepad = textCreate(2, "GAMEPAD", 2);
    textPlace(&gCsGamepad->xf, 0x8B0000, 0xC80000, 0, 1);
    gCsGamepad->flags |= 3;
    for (p = 0; p < 2; p++) {
        gCsExit[p] = textCreate(0, "EXIT", 2);
        textPlace(&gCsExit[p]->xf, gCsExitX[p], 0xD0000, 0, 1);
        gCsExit[p]->flags |= 3;
    }
}

/* 0x80034884 ovl3 */
static void csClose(void) {
    optTextsFree(gCsText, 8);
    textFree(gCsGamepad);
    textFree(gCsExit[0]);
    textFree(gCsExit[1]);
}

/* 0x800348D8 ovl3 - the rows' pictures back into button masks (a stick
 * direction on GAS / BRAKE: the stick axis) */
static void csApply(s32 p) {
    CsAction *act;
    CsButton *b;
    s32 r, k, axis;
    u32 mask;

    for (r = 0, act = gCsActions; r < 7; r++, act++) {
        mask = 0;
        for (k = 0; k != 2; k++) {
            for (b = CS_BUTTONS; b < &CS_BUTTONS[CS_NBUTTONS]; b++) {
                if (b->icon == gCsSlots[p][r][k]) {
                    mask |= b->mask;
                    break;
                }
            }
        }
        act->masks[p] = mask;
        if (r < 2) {
            axis = 0;
            if (gCsSlots[p][r][0] == 10) {
                axis = 1;
            } else if (gCsSlots[p][r][0] == 11) {
                axis = -1;
            }
            if (r != 0) {
                gBrakeAxis[p] = axis;
            } else {
                gGasAxis[p] = axis;
            }
        }
    }
}

/* 0x80034A30 ovl3 - the rows' pictures from the button masks */
static void csIcons(s32 p) {
    s32 r;
    u32 v;

    for (r = 0; r < 7; r++) {
        v = csIconsFor(gCsActions[r].masks[p]);
        gCsSlots[p][r][0] = (s16)(v & 0xFFFF);
        gCsSlots[p][r][1] = (s16)(v >> 16);
    }
}

/* 0x80034AC0 ovl3 - the first two buttons in 'mask' (-1: none) */
static u32 csIconsFor(u32 mask) {
    CsButton *b;
    s16 first = -1, second = -1;

    for (b = CS_BUTTONS; b < &CS_BUTTONS[CS_NBUTTONS]; b++) {
        if (b->mask & mask) {
            if (first == -1) {
                first = (s16)b->icon;
            } else {
                second = (s16)b->icon;
                break;
            }
        }
    }
    return (first & 0xFFFF) | ((u32)second << 16);
}

/* 0x80034B24 ovl3 - each player's cursor box, easing to its row */
static void csBoxesDraw(void) {
    s32 p, h, ty, y;

    for (p = 0; p != 2; p++) {
        if (gCsDim[p] != 0) {
            continue;
        }
        if (gCsCursor[p] == 8) {
            h = 0x11;
            ty = 0xF0000;
        } else {
            h = 0x18;
            ty = 0xC70000 - gCsCursor[p] * 0x170000;
        }
        y = gCsBoxY[p];
        if (y < ty) {
            gCsBoxY[p] = (s32)((f64)y + ((f64)(ty - y) * 0.25 + 65536.0));
            if (ty < gCsBoxY[p]) {
                gCsBoxY[p] = ty;
            }
        } else {
            gCsBoxY[p] = (s32)((f64)y + ((f64)(ty - y) * 0.25 - 65536.0));
            if (gCsBoxY[p] < ty) {
                gCsBoxY[p] = ty;
            }
        }
        if (gCsEditing[p] != 0) {
            csFrameBox(0x62, h, gCsBoxX[p], gCsBoxY[p], 0x64, 0, 0, 0x7D);
        } else {
            csFrameBox(0x62, h, gCsBoxX[p], gCsBoxY[p], 0x32, 0x32, 0x2D, 0x87);
        }
    }
}

/* 0x80034D20 ovl3 - a box with a light edge */
static void csFrameBox(s32 w, s32 h, s32 x, s32 y, s32 r, s32 g, s32 b, s32 a) {
    rectFill(w, 1, x, y, 0xB4, 0xB4, 0x78, 0xC8);
    rectFill(w, 1, x, y - (s32)((f64)(h - 1) * 65536.0), 0xB4, 0xB4, 0x78, 0xC8);
    rectFill(1, h, x, y, 0xB4, 0xB4, 0x78, 0xC8);
    rectFill(1, h, (s32)((f64)(w - 1) * 65536.0) + x, y, 0xB4, 0xB4, 0x78, 0xC8);
    rectFill(w - 2, h - 2, x + 0x10000, y - 0x10000, r, g, b, a);
}

/* 0x80034EB8 ovl3 */
static void csCaptureReset(s32 p) {
    gCsCaptured[p][0] = 0;
    gCsCaptured[p][1] = 0;
}

/* 0x80034ED8 ovl3 - setting a row: a new button replaces the older one unless
 * that is still held (two held together: both). A stick direction on GAS or
 * BRAKE stands alone. The buttons are taken off the other rows' pictures. */
static void csEdit(s32 p) {
    s32 row = gCsCursor[p];
    u32 bit, held, n[2];
    s32 k, r, *slot, *mine;

    bit = csCapture(p);
    if (bit == 0) {
        return;
    }
    held = gPads[p].held;
    for (k = 0; k < 2; k++) {
        if (gCsCaptured[p][k] & held) {
            n[k] = gCsCaptured[p][k];
        } else {
            n[k] = bit;
            bit = 0;
        }
    }
    if (n[1] == n[0]) {
        n[1] = 0;
    }
    if (row < 2) {
        for (k = 0; k < 2; k++) {
            if (n[k] == 0x10000 || n[k] == 0x20000) {
                n[k == 0] = 0;
            }
        }
    }
    if (n[0] == 0) {
        n[0] = n[1];
        n[1] = 0;
    }
    gCsCaptured[p][0] = n[0];
    gCsCaptured[p][1] = n[1];
    gCsActions[row].masks[p] = n[1] | n[0];
    csIcons(p);
    mine = gCsSlots[p][row];
    for (r = 0; r != 7; r++) {
        if (r == row) {
            continue;
        }
        slot = gCsSlots[p][r];
        for (k = 0; k != 2; k++) {
            if (mine[0] == slot[k] || mine[1] == slot[k]) {
                slot[k] = -1;
            }
        }
        if (slot[0] == -1) {
            slot[0] = slot[1];
            slot[1] = -1;
        }
    }
}

/* 0x800350EC ovl3 - the lowest button newly pressed (not START or left / right) */
static u32 csCapture(s32 p) {
    u32 pr = contPressed(p, 0) & 0xFFCFF;
    u32 bit = pr;
    s32 i;

    for (i = 0; i != 32; i++) {
        if ((pr >> i) & 1) {
            bit = 1u << i;
            break;
        }
    }
    if (bit & 0xC1300) {
        return 0;
    }
    if (bit != 0) {
        sfxPlay(0x73, -1);
    }
    contPressed(p, bit | 0xFFF00300);
    return bit;
}

/* 0x800351A4 ovl3 - every row has a button */
static s32 csComplete(s32 p) {
    s32 r;

    for (r = 0; r < 8; r++) {
        if (gCsSlots[p][r][0] == -1) {
            return 0;
        }
    }
    return 1;
}
