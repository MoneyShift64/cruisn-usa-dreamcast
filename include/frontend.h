#ifndef FRONTEND_H
#define FRONTEND_H

/*
 * Front end (overlay 3, ROM 0x5EA10-0x7F100, RAM 0x80025C80, bss 0x80046370-0x800501E0):
 * title screen, attract mode, choose race, choose car, race results,
 * high-score table, options, controller-pak / save-game screens, credits.
 * See docs/frontend.md.
 */
#include "common.h"

/* ------------------------------------------------------------------ */
/* Game modes (src/mainloop.c)                                        */
/* ------------------------------------------------------------------ */
typedef void (*ModeFunc)(s32 arg);
extern ModeFunc gModeFunc;               /* 0x8014C690 */
extern s32      gModeArg;                /* 0x8014C694 */
extern s32      gOverlayRequest;         /* 0x8014C698 */
extern s32      gNoPlayerInput;          /* 0x8014C6A0 */
#define OVERLAY_REQUEST(id) (0x8000 | (id))

/* Loaded at start-up by the boot overlay */
extern u32 gFontPal;                     /* 0x80151E10 - palette file 0x2BD (fonts) */
extern u32 gFontImages[6];               /* 0x80151E18 - group 0x2BC: font images */
extern u32 gHudImages[];                 /* 0x80151E30 - group 0x2E3 */

/* ------------------------------------------------------------------ */
/* Controllers                                                        */
/* ------------------------------------------------------------------ */
typedef struct PadState {                /* 0x14 bytes, 0x8014E270 */
    /* 0x00 */ u32 held;                 /* BTN_* bits held this frame */
    /* 0x04 */ f32 x;                    /* stick 0..1 (0.5 = centre), with the dead zone */
    /* 0x08 */ f32 y;                    /* 0 = down, 1 = up */
    /* 0x0C */ f32 xRaw;                 /* without the dead zone */
    /* 0x10 */ f32 yRaw;
} PadState;
extern PadState gPads[];                 /* 0x8014E270 */
extern s32 gNumControllers;              /* 0x8014E23C - controllers plugged in */
u32  contPressed(s32 player, u32 mask);  /* 0x80111460 - returns new presses, then clears 'mask' from them */

/* Button bits as the game uses them (d-pad / stick / buttons) */
#define BTN_UP     0x10800
#define BTN_DOWN   0x20400
#define BTN_LEFT   0x40200
#define BTN_RIGHT  0x80100
#define BTN_START  0x01000
#define BTN_A      0x08000
#define BTN_B      0x04000
#define BTN_Z      0x02000
#define BTN_ALL    0xFFFFFFFF

/* Save devices. In the front end this word counts the save devices found at boot
 * (game pak EEPROM + controller paks); 0 = no saving. (Named after its use in sched.c.) */
extern s32 gContReadState;               /* 0x80147500 */

/* ------------------------------------------------------------------ */
/* 2D objects (main segment)                                          */
/* ------------------------------------------------------------------ */
typedef struct Xform {                   /* 0x20 bytes: position of a 2D element */
    /* 0x00 */ s32 x;                    /* 16.16 screen pixels */
    /* 0x04 */ s32 y;
    /* 0x08 */ s32 z;
    /* 0x0C */ s32 unk0C;
    /* 0x10 */ s32 unk10;
    /* 0x14 */ s32 unk14;
    /* 0x18 */ s32 scale;                /* 16.16, 0x10000 = 1 */
    /* 0x1C */ s32 visible;
} Xform;

/* A script for xformScriptRun: {cmd, index, value}, cmd 9 ends it */
typedef struct XformCmd {
    /* 0x00 */ u8  cmd;                  /* 0: reset (value = visible), 1..8: set word cmd-1 */
    /* 0x01 */ u8  index;                /* which Xform of the array */
    /* 0x02 */ u8  pad02[2];
    /* 0x04 */ s32 value;
} XformCmd;

typedef struct TextObj {                 /* allocated by textCreate (0x40 bytes, a pool of 50) */
    /* 0x00 */ ListNode node;
    /* 0x08 */ Xform xf;
    /* 0x28 */ s32 blink;                /* anim ramp that makes it flash (gBlink*), -1 = steady */
    /* 0x2C */ f32 blinkLevel;           /* shown while the ramp's t <= this (0.5) */
    /* 0x30 */ u8  count;                /* characters (sprites) */
    /* 0x31 */ s8  font;                 /* TEXT_* flags | font number */
    /* 0x32 */ u16 flags;                /* TEXTF_* */
    /* 0x34 */ s32 viewer;               /* bgDrawSetup viewer (-1: the whole screen) */
    /* 0x38 */ u32 color;                /* RGBA, with TEXTF_COLOR */
    /* 0x3C */ u16 sprite;               /* first sprite + 1 */
    /* 0x3E */ u8  pad3E[2];
} TextObj;
#define TEXT_FONT     0x3F
#define TEXT_SLANT    0x40               /* italic: tops 2 pixels right, bottoms 2 left */
#define TEXT_LAYER2   0x80               /* drawn in the second pass */
#define TEXTF_COLOR   0x0001             /* tinted by 'color' */
#define TEXTF_HIDDEN  0x0002

TextObj *textCreate(s32 layer, char *text, s32 style); /* 0x8011A114 */
void textFree(TextObj *t);               /* 0x8011A43C */
void textDraw(TextObj *t, u32 unused);   /* 0x8011A4DC - draw one text object now */
void textSetColor(TextObj *t, u8 r, u8 g, u8 b, u8 a); /* 0x8011A3E4 */
Xform *textPlace(Xform *xf, s32 x, s32 y, s32 z, s32 visible); /* 0x80114F40 - (re)initialise a position */
void xformScriptRun(void *script, Xform *xf); /* 0x80114F74 - apply a {cmd, index, value} table to an Xform array */
extern s32 gBlinkSlow;                   /* 0x8014EF70 - looping anim ramps for flashing text (1 s) */
extern s32 gBlinkMedium;                 /* 0x8014EF74 - (0.5 s) */

/* Drawing helpers (main segment) */
void screenClear(s32 player, s32 color); /* 0x80115730 */
void bgDrawSetup(s32 a, s32 b);          /* 0x80116D8C */
void drawDL(void *dl, s32 x, s32 y, s32 z, s32 scale); /* 0x80116254 - draw a display list at a 16.16 position */
void drawDLXform(void *dl, Xform *xf);   /* 0x80115164 */
void drawDLScript(void *script, Xform *xf, void **dls, s32 count); /* 0x801151C0 */
void rectFill(s32 w, s32 h, s32 x, s32 y, s32 r, s32 g, s32 b, s32 a); /* 0x80115F30 */
void viewBegin(s32 racer);        /* 0x80116C10 */
void renderObjInit(void *obj, s32 a, s32 b, s32 c, s32 d, s32 e, s32 f, s32 g); /* 0x8011802C */
void renderObjDraw(void *obj);           /* 0x801180EC */

/* Timed animations (src/anim.c). A ramp runs t from 0 to 1 (or back) in a
 * given time; events attached to it fire at points of t: callbacks, or a
 * script of value tracks applied to a target (animStart). Index -1 = none. */
/* Script entry (0x14 bytes); a script ends with cmd 0 */
typedef struct AnimCmd {
    /* 0x00 */ u8  cmd;                  /* 1 track, 2 branch, 3 call */
    /* 0x01 */ u8  arg1;                 /* track: run from 'to' to 'from'; branch: 0 jump, 1 call */
    /* 0x02 */ u8  arg2;                 /* track: 0 s32, 1 s16, 2 s8; branch: use 'extra' as target */
    /* 0x03 */ u8  arg3;                 /* branch: 0 always, 1 if *ptr == value, 2 if != */
    /* 0x04 */ s32 a;                    /* track: offset in the target; branch: s32 *ptr; call: argument */
    /* 0x08 */ s32 b;                    /* track: from; branch: value; call: time in microseconds */
    /* 0x0C */ s32 c;                    /* track: to; branch: script; call: function */
    /* 0x10 */ s32 d;                    /* track: ease curve; branch: target; call: 2nd argument */
} AnimCmd;

typedef s32 (*AnimDoneFunc)(s32 event, s32 ramp, f32 t);  /* returns 1: drop the event, 2: cancel the ramp */
typedef struct AnimRamp {                /* 0x14 bytes, 50 at 0x8014E3B0 */
    /* 0x00 */ f32 t;                    /* 0..1 */
    /* 0x04 */ f32 step;                 /* per frame */
    /* 0x08 */ s32 user;                 /* script target / caller's value */
    /* 0x0C */ s32 user2;
    /* 0x10 */ s16 first;                /* first event */
    /* 0x12 */ u8  flags;                /* ANIM_* */
    /* 0x13 */ u8  used;                 /* 0x80 */
} AnimRamp;
#define ANIM_REVERSE  0x01               /* t runs 1 -> 0 */
#define ANIM_LOOP     0x02               /* jump back to the start at the end */
#define ANIM_ONCE     0x04               /* cancel at the end */
#define ANIM_PAUSED   0x80
typedef struct AnimSlot {                /* an event: 0x14 bytes, 100 at 0x8014E798 */
    /* 0x00 */ s16 prev;
    /* 0x02 */ s16 next;
    /* 0x04 */ AnimDoneFunc func;
    /* 0x08 */ s32 user;                 /* caller's value, read back in the callback */
    /* 0x0C */ f32 at;                   /* point of t where it fires */
    /* 0x10 */ s16 ramp;                 /* id to pass to animCancel */
    /* 0x12 */ u8  flags;                /* when it fires: EV_* */
    /* 0x13 */ u8  used;                 /* 0x80 */
} AnimSlot;
#define EV_PASS_FWD   0x01               /* t passes 'at' going up */
#define EV_PASS_BACK  0x02               /* ... going down */
#define EV_END        0x04               /* t reaches the end */
#define EV_AT_0       0x08               /* while t == 0 */
#define EV_AT_1       0x10               /* while t == 1 */
#define EV_ALWAYS     0x20               /* every frame */
#define EV_CHANGED    0x40               /* every frame t moved */
#define EV_DISABLED   0x80
extern AnimRamp gAnimRamps[];            /* 0x8014E3B0 */
extern AnimSlot gAnimSlots[];            /* 0x8014E798 */
s32  animStart(f32 seconds, void *script, void *target, AnimDoneFunc done, u8 flags); /* 0x80112D24 */
void animCancel(s32 ramp);               /* 0x80112B38 */
s32  animWait(f32 seconds, void *unused, AnimDoneFunc done); /* 0x80113AE0 - call 'done' after a delay */

/* Asset loading (low-RAM overlay 1). Assets are loaded onto the high heap;
 * the front end remembers the heap pointer (returned by lowMemOverlayLoad)
 * and gives everything back with highHeapRestore. */
u32  lowMemOverlayLoad(void);            /* 0x80102D74 */
void lowMemOverlayRelease(void);         /* 0x80102E84 */
void *assetLoad(s32 id);                 /* 0x80000480 */
s32  assetLoadGroup(s32 id, void **out); /* 0x8000078C - loads a group, fills out[], returns count */
void highHeapRestore(u32 mark);          /* 0x80000A74 */

/* Sound */
void sfxPlay(s32 id, s32 player);        /* 0x8011F2A4 */
void sfxStop(s32 id, s32 player);        /* 0x8011F670 */
void musicPlay(s32 track);               /* 0x8011F83C */
void musicStop(void);                    /* 0x8011F894 */

/* Video mode for the TV type */
extern s32 gTvType;                      /* 0x80146588 */
extern s32 gViModeIndex[][2];            /* 0x80132234 */
extern OSViMode gViModes[];              /* 0x8013A460 */
#define VI_SET_DEFAULT_MODE() \
    { osViSetMode(&gViModes[gViModeIndex[gTvType][0]]); osViSetSpecialFeatures(OS_VI_GAMMA_OFF); }

/* ------------------------------------------------------------------ */
/* Game set-up handed to the race (main segment 0x80150210..)         */
/* ------------------------------------------------------------------ */
typedef struct PlayerSetup {             /* 0x39C bytes per player, 0x80150210 */
    /* 0x000 */ s32 manual;              /* transmission: 0 automatic, 1 manual */
    /* 0x004 */ s32 car;                 /* car type 0..? */
    /* 0x008 */ s32 unk008;
    /* 0x00C */ f32 speedo;              /* race HUD values */
    /* 0x010 */ f32 odometer;
    /* 0x014 */ f32 distance;
    /* 0x018 */ u8  tacho;
    /* 0x019 */ u8  gear;
    /* 0x01A */ u8  unk01A;
    /* 0x01B */ u8  place;               /* finishing place */
    /* 0x01C */ s32 unk01C;
    /* 0x020 */ s32 unk020;
    /* 0x024 */ f32 raceTime;            /* last race time */
    /* 0x028 */ f32 totalTime;           /* Cruise the USA: total time so far */
    /* 0x02C */ s32 unk02C;
    /* 0x030 */ s32 unk030;
    /* 0x034 */ u8  pad034[0x324 - 0x34];
    /* 0x324 */ s8  initials[4];         /* initials[0] = -1: not entered */
    /* 0x328 */ s32 unk328;
    /* 0x32C */ s32 carAwards[7];        /* per car: Cruise the USA wins (faster car awards) */
    /* 0x348 */ s32 awarded;             /* a faster car was awarded this time */
    /* 0x34C */ s32 nextAward;           /* difficulty needed for the next award */
    /* 0x350 */ s32 tracksDone;          /* bit per track: unlocked in Choose Race */
    /* 0x354 */ s32 saveSlot;            /* 0..5, 6 = none */
    /* 0x358 */ s32 savePak;             /* save device */
    /* 0x35C */ s32 unk35C;
    /* 0x360 */ s32 unk360;
    /* 0x364 */ f32 camDist;             /* camera distance behind the car (0 = in-car view) */
    /* 0x368 */ f32 camHeight;           /* camera height (negative = up) */
    /* 0x36C */ f32 unk36C;
    /* 0x370 */ f32 camDistTarget;       /* view being moved to (table 0x80133B5C) */
    /* 0x374 */ f32 camHeightTarget;
    /* 0x378 */ f32 unk378;
    /* 0x37C */ f32 camDistStep;         /* per frame while changing view, 0 = settled */
    /* 0x380 */ f32 camHeightStep;
    /* 0x384 */ s32 unk384;
    /* 0x388 */ s32 camViewShown;        /* view reached once a view change finishes */
    /* 0x38C */ s32 cameraFollowsCar;    /* chosen view: 0 in-car, 1 chase, 2 far chase */
    /* 0x390 */ s32 unk390;
    /* 0x394 */ s32 viewAtEdge;          /* 1 while the camera is at distance 0 (in-car); set by raceCameraUpdate */
    /* 0x398 */ s32 camOverride;         /* cameraForceFar: 1 = was not in-car, 2 = forced out of the in-car view */
} PlayerSetup; /* size = 0x39C */
extern PlayerSetup gPlayerSetup[2];      /* 0x80150210 */

/* Save image of each save device (0x900 bytes each), 0x8014F010 */
typedef struct HiscoreEntry {            /* 8 bytes */
    /* 0x00 */ u8  initials[3];
    /* 0x03 */ u8  pad;                  /* (port) 1: a player's entry (not one of the built-in times) */
    /* 0x04 */ f32 time;
} HiscoreEntry;
typedef struct SaveSlot {                /* 0x50 bytes: one saved player */
    /* 0x00 */ u8  pad00[8];
    /* 0x08 */ u8  initials[3];
    /* 0x0B */ u8  pad0B;
    /* 0x0C */ u16 tracksDone;           /* PlayerSetup.tracksDone */
    /* 0x0E */ u8  flags;                /* bit 7: PlayerSetup.unk328; player 1's slot also: bits 0-2
                                          * difficulty, 3 metric, 4 traffic, 5 racers, 6 2-player split */
    /* 0x0F */ s8  steerMode;            /* Controls.steerMode */
    /* 0x10 */ u32 btnShiftUp;           /* the controller setup: gBtnShiftUp .. gBrakeMask */
    /* 0x14 */ u32 btnShiftDown;
    /* 0x18 */ u32 btnLights;
    /* 0x1C */ u32 btnMusic;
    /* 0x20 */ u32 btnView;
    /* 0x24 */ u32 gasMask;
    /* 0x28 */ u32 brakeMask;
    /* 0x2C */ s8  gasAxis;
    /* 0x2D */ s8  brakeAxis;
    /* 0x2E */ u8  cruise;               /* bits 0-2 car, bit 3 Cruise the USA in progress, bits 4-7 next track */
    /* 0x2F */ u8  opts;                 /* bits 0-1 unk008, bits 2-3 camera, bit 4 manual, bits 5-7 difficulty */
    /* 0x30 */ s8  carAwards[7];
    /* 0x37 */ u8  pad37[0x40 - 0x37];
    /* 0x40 */ f32 totalTime;
    /* 0x44 */ f32 aiSkill;
    /* 0x48 */ f32 musicVolume;          /* player 1's slot only */
    /* 0x4C */ f32 sfxVolume;
} SaveSlot;
typedef struct SaveImage {               /* 0x900 bytes */
    /* 0x000 */ u8  header[0x10];
    /* 0x010 */ HiscoreEntry hiscores[15][10]; /* 14 tracks + Cruise the USA total */
    /* 0x4C0 */ s32 records[14][10];
    /* 0x6F0 */ u8  pad6F0;
    /* 0x6F1 */ u8  slotsUsed;            /* bit per slot */
    /* 0x6F2 */ s8  viewLeftX, viewTopY;   /* CENTER SCREEN (device 0 only) */
    /* 0x6F4 */ u8  pad6F4[0x2C];
    /* 0x720 */ SaveSlot slots[6];
} SaveImage;
extern SaveImage gSaveImage[];           /* 0x8014F010 */

extern f32 gRaceTimeLeft;                /* 0x8015094C */
extern s32 gRaceWinner;                  /* 0x80150954 - player who won the last race, -1 = none */
extern s32 gNumPlayersM1;                /* 0x8015095C - number of players minus 1 */
extern s32 gTrackChoice;                 /* 0x80150960 - race chosen (0..13, 14 = Cruise the USA) / next track */
extern s32 gSecretRace;                  /* 0x80150964 */
extern s32 gCruiseMode;                  /* 0x80150968 - 1 = Cruise the USA (all 14 races in order) */
extern s32 gCruiseContinue;              /* 0x8015096C - (named gAttractMode in race.c) */
extern s32 gUnitsKph;                    /* 0x80150974 - option METRIC */
extern s32 gTrafficEnabled;              /* 0x80150978 - this race has traffic */
extern s32 gRacersEnabled;               /* 0x8015097C - this race has AI racers */
extern s32 gOptTraffic;                  /* 0x80150980 - option TRAFFIC */
extern s32 gOptRacers;                   /* 0x80150984 - option RACERS */
extern s32 gDifficulty;                  /* 0x80150988 */
extern f32 gVolume[2];                   /* 0x80150990 - music, sound effects */
extern s32 gOpt2PSound;                  /* 0x80150998 - option 2-PLAYER SOUND (split L/R / combined) */
extern s32 gCurrentTrack;                /* 0x8013F9B0 - set by trackLoad */

void trackLoad(s32 track);               /* 0x80100340 - sets gCurrentTrack */
void trackPrepare(void);                 /* 0x80100488 */
void trackUnload(void);             /* 0x80100A84 */
void gameSetupReset(void);               /* 0x80117AD0 - new game: Cruise the USA, 1 player */
void playerSetupReset(s32 player);       /* 0x80117B88 */
void playerSetupView(s32 player);        /* 0x80117C00 - screen window for the player count */
void raceOptionsApply(void);             /* 0x80117CAC - racers/traffic from the options (always on in Cruise the USA) */

/* ------------------------------------------------------------------ */
/* Front-end mode functions (ovl3)                                    */
/* ------------------------------------------------------------------ */
void frontEndEnter(s32 showTitle);       /* 0x80026030 ovl3 */
void titleInit(s32 skip);                /* 0x80026108 ovl3 */
void titleFrame(s32 skip);               /* 0x8002621C ovl3 */
void raceSelectInit(s32 fromRace);       /* 0x80029920 ovl3 */
void raceSelectFrame(s32 arg);           /* 0x80029D6C ovl3 */
void carSelectInit(s32 arg);             /* 0x8002B690 ovl3 */
void resultsInit(s32 arg);               /* 0x80026C98 ovl3 */
void resultsFrame(s32 arg);              /* 0x80026F7C ovl3 */
void hiscoreInit(s32 fromAttract);       /* 0x8002E750 ovl3 - (actually the credits roll: src/frontend/credits.c) */
void hiscoreFrame(s32 arg);              /* 0x8002E824 ovl3 */
void hiscoreExit(s32 toTitle);           /* 0x8002EE58 ovl3 */
void congratsInit(s32 arg);              /* 0x8002FAA0 ovl3 */
s32  optionsCheck(ModeFunc returnTo, s32 inGame); /* 0x80030DE0 ovl3 */
void saveMenuInit(s32 players);          /* 0x80035760 ovl3 */
void creditsInit(s32 fromCredits);       /* 0x8003AC10 ovl3 - (actually the intro: src/frontend/intro.c) */
void attractInit(s32 *tracks);           /* 0x8003BD10 ovl3 - HOT TIMES tables (src/frontend/hottimes.c) */
/* the race overlay's start function (raceStart in src/race/race.c); raw name so it
 * resolves while the front-end overlay is loaded */
void func_8002F600_ovl2(s32 arg);
#define RACE_START_MODE func_8002F600_ovl2


/* Port: 2-player games are switched off until they are tested (port/port_main.c gPortTwoPlayer):
 * player 2's START does nothing, no "PLAYER 2 PRESS START", 2-PLAYER SOUND dimmed in the options */
#ifdef PORT
extern s32 gPortTwoPlayer;
#define P2_ALLOWED (gPortTwoPlayer != 0)
#else
#define P2_ALLOWED 1
#endif
#endif
