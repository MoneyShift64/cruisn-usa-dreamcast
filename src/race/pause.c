/*
 * race/pause.c - pausing a race (pause menu: MUSIC, SOUND FX, QUIT GAME,
 * RETURN TO RACE) and an unused debug "track viewer" mode.
 * Original code: race overlay, ROM 0x42D90 area (RAM 0x8002A6B4, 0x8002EB60-0x8002F600).
 */
#include "common.h"
#include "race.h"
#include "road.h"

typedef void (*ModeFunc)(s32 arg);
extern ModeFunc gModeFunc;               /* 0x8014C690 */
extern s32 gModeArg;                     /* 0x8014C694 */
extern s32 gOverlayRequest;              /* 0x8014C698 */
extern s32 gNoPlayerInput;               /* 0x8014C6A0 */
extern s32 gFrameUserArg;                /* 0x8014C6A4 */
extern s32 gLowMemOverlayState;          /* 0x801466CC */
extern u32 gLowMemStart;                 /* 0x80146598 */
extern u32 *gDlCursor;                   /* 0x80147684 */
extern FrameBuffer gFrameBuffers[3];     /* 0x80147730 */
extern s32 gCountdownPhase;              /* 0x800468A8 ovl2 */
extern s32 gAttractMode;                 /* 0x8015096C */

/* pause menu state */
extern ModeFunc gPausedMode;             /* 0x80046A0C ovl2 - mode to return to */
extern s32 gPauseSel;                    /* 0x80046A10 ovl2 - 0 MUSIC, 1 SOUND FX, 2 QUIT, 3 RETURN */
extern s32 gPauseBlink;                  /* 0x80046A18 ovl2 */
extern s32 gPauseRepeat;                 /* 0x80046A08 ovl2 - slider sound-preview timer */
extern f32 gVolume[2];                   /* 0x80150990 - music, sound effects */
extern u32 gPadHeld[];                   /* 0x8014E270, stride 0x14 */
extern f32 gVolumeStep;                  /* 0x80045984 ovl2 */
extern f32 gVolumeStepDown;              /* 0x80045988 ovl2 */
extern s32 gPauseMusicTrack[];           /* 0x800428EC ovl2, stride 8 */
extern s32 gPauseSliderGfx;              /* 0x80046934 ovl2 */

typedef struct TextObj {
    /* 0x00 */ u8  pad00[0x32];
    /* 0x32 */ u16 flags;
    /* 0x34 */ u8  pad34[4];
    /* 0x38 */ u32 color;
} TextObj;
extern TextObj *gPauseItems[5];          /* 0x800469B0 ovl2: title + 4 options */

typedef struct PauseItemDef {            /* 0x14 bytes, 0x80042930 ovl2 */
    /* 0x00 */ char *text;
    /* 0x04 */ s32 layer;
    /* 0x08 */ s32 x;
    /* 0x0C */ s32 y;
    /* 0x10 */ s32 style;
} PauseItemDef;
extern PauseItemDef gPauseItemDefs[5];   /* 0x80042930 ovl2 */

typedef struct SliderPos { s32 x; s32 y; } SliderPos;
extern SliderPos gPauseSliders[2];       /* 0x80042994 ovl2 */

typedef struct FrameLine {               /* 0x20 bytes, 0x800429A4 ovl2: menu box outline */
    /* 0x00 */ s32 vertical;
    /* 0x04 */ s32 a;
    /* 0x08 */ s32 b;
    /* 0x0C */ s32 c;
    /* 0x10 */ s32 r, g, bl, alpha;
} FrameLine;
extern FrameLine gPauseFrameLines[6];    /* 0x800429A4 ovl2 */

typedef struct ViewerCam {               /* 0x28 bytes, 0x8014EF90 */
    s32 v[10];
} ViewerCam;
extern ViewerCam gViewerCams[2];         /* 0x8014EF90 */
extern s32 gViewerTexBank;               /* 0x80046DA0 ovl2 */

u32  contPressed(s32 player, u32 mask);  /* 0x80111460 */
s32  gfxWaitIdle(void);                  /* 0x80103D88 */
void drawLowMemLayer(void);              /* 0x801158EC */
void resetCleanup(void);                 /* 0x80107CF8 */
void trackUnload(void);             /* 0x80100A84 */
void trackLoad(s32 track);               /* 0x80100340 */
void trackPrepare(void);                 /* 0x80100488 */
void skyDraw(s32 player, s32 yaw, s32 roll); /* 0x80101A68 */
void viewBegin(s32 racer);        /* 0x80116C10 */
void cameraApply(s32 p, s32 a, s32 b, s32 c, s32 d, s32 e); /* 0x801005BC */
void bgLoadImage(u32 dst, u32 src);      /* 0x801163A0 */
void bgDrawSetup(s32 a, s32 b);          /* 0x80116D8C */
void rectFill(s32 w, s32 h, s32 x, s32 y, s32 r, s32 g, s32 b, s32 a); /* 0x80115F30 */
void drawSlider(void *gfx, f32 value, s32 x, s32 y, s32 selected, s32 alpha); /* 0x8011E260 */
void volumeApply(s32 channel, s32 preview); /* 0x8011E628 */
void textDraw(TextObj *t, u32 unused);   /* 0x8011A4DC */
TextObj *textCreate(s32 layer, char *text, s32 style); /* 0x8011A114 */
void textPlace(void *pos, s32 x, s32 y, s32 a, s32 b); /* 0x80114F40 */
void textFree(TextObj *t);               /* 0x8011A43C */
void musicPlay(s32 track);               /* 0x8011F83C */
void musicStop(void);                    /* 0x8011F894 */
void sndPauseAll(s32 release, s32 recMask); /* 0x80108CA4 */
void sndResumeAll(s32 replay);           /* 0x80108F84 */
void sfxPlay(s32 id, s32 player);        /* 0x8011F2A4 */
void raceQuit(void);                     /* 0x8002FE98 ovl2 */
void animCancelAll(void);                    /* 0x80113528 */
void animBlinkInit(void);                    /* 0x80113CA0 */
void textFreeAll(void);                    /* 0x8011A9D4 */
s32  tblCos(s32 angle);                  /* 0x80113EDC */
void pauseEnter(void);
void pauseFrame(s32 arg);
void pauseExit(void);
void pauseMusicResume(void);
void pauseMusicStop(void);
void raceStart(s32 arg);                 /* 0x8002F600 ovl2 */
void debugViewerFrame(s32 arg);

extern void func_80026030_ovl3(s32);

#define BTN_UP     0x10800   /* d-pad or stick */
#define BTN_DOWN   0x20400
#define BTN_RIGHT  0x80100
#define BTN_LEFT   0x40200
#define BTN_START  0x01000
#define BTN_A      0x08000

/* 0x8002A6B4 ovl2 - START during the race opens the pause menu.
 * Returns TRUE if the game is now paused. */
s32 racePauseCheck(ModeFunc self) {
    s32 pressed;
    s32 p;

    if (gCountdownPhase >= 6 || gCountdownPhase <= 0) {
        return FALSE;
    }
    pressed = FALSE;
    for (p = 0; p <= gNumPlayersM1; p++) {
        if (contPressed(p, BTN_START) & BTN_START) {
            pressed = TRUE;
        }
    }
    if (pressed) {
        gNoPlayerInput = TRUE;
        gPausedMode = self;
        pauseEnter();
        return TRUE;
    }
    return FALSE;
}

/* 0x8002EB60 ovl2 */
void pauseMusicResume(void) {
#ifdef PORT
    {
        extern s32 gSnowOn;
        if (gSnowOn) {
            musicPlay(0xAD);                 /* the snow cheat's tune: Jingle Bells (tools/port_sound.py) */
            return;
        }
    }
#endif
    musicPlay(gPauseMusicTrack[((s32 *)&gAttractMode)[1] * 2]);
}

/* 0x8002EB94 ovl2 */
void pauseMusicStop(void) {
    musicStop();
}

/* 0x8002EBB4 ovl2 - freeze the picture and build the pause menu */
void pauseEnter(void) {
    PauseItemDef *def;
    TextObj **item;
    s32 fb;
    s32 p;

    fb = gfxWaitIdle();
    gLowMemOverlayState = 2;
    gFrameUserArg = 1;
    bgLoadImage(gLowMemStart, osVirtualToPhysical(gFrameBuffers[fb].addr));   /* the frozen frame */
    for (item = gPauseItems, def = gPauseItemDefs; item < &gPauseItems[5]; item++, def++) {
        *item = textCreate(def->layer, def->text, def->style);
        textPlace((u8 *)*item + 8, def->x, def->y, 0, 1);
        (*item)->flags |= 3;
    }
    gPauseBlink = 0;
    gPauseRepeat = 0;
    for (p = 0; p <= gNumPlayersM1; p++) {
        contPressed(p, -1);
    }
    gPauseSel = 3;                       /* RETURN TO RACE */
    pauseMusicStop();
    sndPauseAll(1, 2);
    gModeFunc = pauseFrame;
    pauseFrame(0);
}

/* 0x8002ED04 ovl2 - pause menu, every frame */
void pauseFrame(s32 arg) {
    FrameLine *ln;
    TextObj **item;
    SliderPos *sp;
    f32 *vol;
    f32 old;
    u32 pressed;
    u32 held;
    u32 color;
    s32 before;
    s32 sel;
    s32 n;
    s32 p;
    s32 i;
    s32 y;
    s32 x;

    n = gNumPlayersM1;
    gPauseBlink += 0x800;
    for (p = 0; p <= gNumPlayersM1; p++) {
        pressed = contPressed(p, -1);
        sel = gPauseSel;
        before = sel;
        if (pressed & BTN_UP) {
            gPauseSel = --sel;
        }
        if (pressed & BTN_DOWN) {
            gPauseSel = ++sel;
        }
        if (sel < 0) {
            gPauseSel = sel = 3;
        }
        if (sel >= 4) {
            gPauseSel = sel = 0;
        }
        if (before != sel) {
            sfxPlay(0x80, -1);
            sel = gPauseSel;
            if (sel == 0) {
                pauseMusicResume();          /* hear the music while adjusting it */
                sel = gPauseSel;
            } else if (before == 0) {
                pauseMusicStop();
                sel = gPauseSel;
            }
        }
        if (sel == 3) {
            if (pressed & BTN_START) {
                sndResumeAll(1);
                pauseExit();
                if (gCountdownPhase >= 4) {
                    pauseMusicResume();
                }
                gModeFunc(gModeArg);
                return;
            }
        } else if (sel == 0 || sel == 1) {
            held = gPadHeld[p * 5];
            vol = &gVolume[sel];
            old = *vol;
            if (held & BTN_RIGHT) {
                *vol = old + gVolumeStep;
            }
            if (held & BTN_LEFT) {
                *vol -= gVolumeStepDown;
            }
            if (*vol < 0.0) {
                *vol = 0.0f;
            }
            if (*vol > 1.0) {
                *vol = 1.0f;
            }
            if (old != *vol) {
                volumeApply(sel, gPauseRepeat == 0);
            }
        } else if (sel == 2 && (pressed & BTN_START)) {
            resetCleanup();
            pauseExit();
            sfxPlay(0x60, -1);
            raceQuit();
            animCancelAll();
            animBlinkInit();
            textFreeAll();
            gModeArg = 0;
            gModeFunc = func_80026030_ovl3;
            gOverlayRequest = 0x8000 | OVL_FRONTEND;
            return;
        }
    }

    /* slider sound preview: repeats every 9 frames while left/right is held */
    if (gPauseSel == 0 || gPauseSel == 1) {
        for (p = 0; p <= n; p++) {
            if (gPadHeld[p * 5] & (BTN_RIGHT | BTN_LEFT)) {
                if (--gPauseRepeat < 0) {
                    gPauseRepeat = 8;
                }
                break;
            }
        }
        if (p == n + 1) {
            gPauseRepeat = 0;
        }
    }

    /* draw: frozen frame, dark box, sliders, options, outline */
    bgLoadImage(0x02000000, gLowMemStart);
    bgDrawSetup(-1, 1);
    rectFill(320, 240, -0x140000, 0xDB0000, 0, 0, 0, 200);
    rectFill(200, 115, 0x270000, 0x9C8000, 0, 0, 0, 140);
    for (i = 0, vol = gVolume, sp = gPauseSliders; i < 2; i++, vol++, sp++) {
        drawSlider(&gPauseSliderGfx, *vol, sp->x, sp->y, i == gPauseSel, 0xFF);
    }
    for (i = 0, item = gPauseItems; i < 5; i++, item++) {
        if (i != 0) {
            color = (i == gPauseSel + 1) ? 0xFFFFFFFF : 0x5F5F5FFF;
        } else {
            /* "GAME PAUSED" pulses */
            color = (u32)((f32)((f32)tblCos(gPauseBlink) / 32768.0) * 127.0f + 192.0) * 0x01010100 + 0xFF;
        }
        (*item)->color = color;
        textDraw(*item, color);
    }
    for (ln = gPauseFrameLines; ln != &gPauseFrameLines[6]; ln++) {
        if (ln->vertical) {
            y = ln->a;
            rectFill(1, ((y - ln->b) >> 16) + 1, ln->c, y, ln->r, ln->g, ln->bl, ln->alpha);
        } else {
            x = ln->a;
            rectFill(((ln->b - x) >> 16) + 1, 1, x, ln->c, ln->r, ln->g, ln->bl, ln->alpha);
        }
    }
}

/* 0x8002F36C ovl2 - close the pause menu and resume the paused mode */
void pauseExit(void) {
    TextObj **item;
    s32 p;

    for (item = gPauseItems; item < &gPauseItems[5]; item++) {
        textFree(*item);
    }
    for (p = 0; p <= gNumPlayersM1; p++) {
        contPressed(p, -1);
    }
    gLowMemOverlayState = 0;
    drawLowMemLayer();
    gNoPlayerInput = FALSE;
    gModeFunc = gPausedMode;
}

/* 0x8002F410 ovl2 - UNUSED debug mode: show San Francisco from a fixed camera */
void debugViewerStart(s32 arg) {
    gNumPlayersM1 = 0;
    gCurrentTrack = 1;
    trackUnload();
    trackLoad(gCurrentTrack);
    trackPrepare();
    gModeFunc = debugViewerFrame;
}

/* 0x8002F468 ovl2 - UNUSED debug viewer: A cycles the 4 texture banks, START races */
void debugViewerFrame(s32 arg) {
    ViewerCam *cam;
    u32 pressed;
    u32 *dl;
    s32 p;

    for (p = 0; p <= gNumPlayersM1; p++) {
        pressed = contPressed(p, -1);
        if (pressed & BTN_START) {
            gModeArg = 0;
            gModeFunc = raceStart;
            gOverlayRequest = 0x8000 | OVL_RACE;
            return;
        }
        cam = &gViewerCams[p];
        if (pressed & BTN_A) {
            gViewerTexBank++;
        }
        cam->v[9] = 0;
        cam->v[0] = 0;
        cam->v[2] = 0;
        cam->v[3] = 0;
        cam->v[1] = ((gViewerTexBank & 3) << 12) + 0x800000;
        cam->v[4] = 0;
        cam->v[5] = 0;
        cam->v[6] = 0;
        cam->v[7] = 0x800000;
        cam->v[8] = 0xFFF00000;
        skyDraw(p, 0, 0);
        viewBegin(p);
        cameraApply(p, 0, 0, 0, 1, 1);
        dl = gDlCursor;
        gDlCursor = dl + 2;
        dl[0] = 0xBC000C06;
        dl[1] = (gViewerTexBank & 3) << 9;
    }
}
