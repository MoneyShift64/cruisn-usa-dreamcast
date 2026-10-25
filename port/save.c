/*
 * save.c - saving to the VMU (stage 3).
 *
 * The N64 saved to the cartridge (EEPROM) and a controller pak, each holding a
 * 0x900-byte image (SaveImage, gSaveImage[]: the HOT TIMES tables, the track
 * records and six player slots), and asked each player to pick or make a slot.
 * Here every VMU is one save device with one slot (no picking): player 1's VMU
 * is device 0 and player 2's device 1 (or device 0 if player 1 has none).
 * The game's own code still decides when to save and what (saveSync in
 * results.c after a race and after the HOT TIMES initials, optionsSave when
 * the options close); only changed devices are written.
 *
 * Replaces (front-end overlay): the boot front end's device checks and loading
 * (0x8003F130-0x8003FC4C), saveMenuInit (0x80035760, the slot menus), slot
 * new / load (0x8003A394 / 0x8003A498), saveBegin (0x80035D0C), saveWrite
 * (0x80035A30 -> 0x8003A84C), the options' save (0x80035294 / 0x80035348) and
 * the image -> tables copy (0x80035A5C).
 *
 * Platform side: plat_save_devices / plat_save_read / plat_save_write
 * (dc/game/plat_dc.c: a VMU file; port/host/host.c: a file per device).
 */
#include "common.h"
#include "frontend.h"
#include "race.h"

typedef struct Controls {                /* 0x18 bytes (src/input.c) */
    s32 gas, brake;
    f32 steer, steerDigital;
    u32 cheats;
    s32 steerMode;
} Controls;

extern HiscoreEntry gHiscores[][15][10]; /* 0x801625C0 - per save device */
extern s32  gTrackRecords[][14][10];     /* 0x80150A00 - per save device */
extern Controls gControls[2];            /* 0x8014E378 */
extern u32  gGasMask[2];                 /* 0x8014E330 */
extern s32  gGasAxis[2];                 /* 0x8014E338 */
extern u32  gBrakeMask[2];               /* 0x8014E340 */
extern s32  gBrakeAxis[2];               /* 0x8014E348 */
extern u32  gBtnShiftUp[2];              /* 0x8014E350 */
extern u32  gBtnShiftDown[2];            /* 0x8014E358 */
extern u32  gBtnLights[2];               /* 0x8014E360 */
extern u32  gBtnMusic[2];                /* 0x8014E368 */
extern u32  gBtnView[2];                 /* 0x8014E370 */
extern s16  gViewLeftX;                  /* 0x8014EF80 */
extern s16  gViewTopY;                   /* 0x8014EF82 */

void volumeApply(s32 channel, s32 preview); /* 0x8011E628 */
void raceSelectInit(s32 fromRace);
void saveWrite(void (*done)(s32), s32 devices);
void saveBegin(void);

/* platform: devices found (max 2) and which player each belongs to */
int plat_save_devices(int *playerOfDevice);
int plat_save_read(int dev, void *buf, int size);
int plat_save_write(int dev, const void *buf, int size);

#define SAVE_MAGIC 0x43525553            /* "CRUS" */
#define SAVE_VERSION 2                   /* 2: Dreamcast buttons (1: N64 buttons, builds 49-50) */

typedef struct SaveFile {                /* what goes in the VMU file */
    u32 magic;
    u32 version;
    u32 size;
    u32 sum;
    SaveImage image;
} SaveFile;

static int gSaveDevPlayer[2];            /* device -> player */
static s32 gSaveDevOf[2] = { -1, -1 };   /* player -> device, -1: none */
static SaveFile gSaveFile;

static void saveSlotNew(s32 dev, s32 n);
static void saveSlotControls(SaveSlot *s);
static void saveSlotLoad(s32 p, s32 dev, s32 n);

/* 0x8003AAB8 ovl3 - the image's checksum (kept in header[0xC]) */
static u32 saveChecksum(SaveImage *img) {
    u8 *b = (u8 *)img;
    u8 s[4] = { 0, 0, 0, 0 };
    s32 i;

    for (i = 0x10; i < 0x900; i += 4) {
        s[0] += b[i];
        s[1] += b[i + 1];
        s[2] += b[i + 2];
        s[3] += b[i + 3];
    }
    return s[0] + (s[3] << 24) + (s[2] << 16) + (s[1] << 8);
}

static u32 fileSum(SaveFile *f) {
    u8 *b = (u8 *)&f->image;
    u32 s = 0x1234;
    s32 i;

    for (i = 0; i < (s32)sizeof(SaveImage); i++) {
        s = (s << 5) + (s >> 27) + b[i];
    }
    return s;
}

/* a new device: the default tables (as the game made them at boot), no slots */
static void saveImageNew(s32 dev) {
    SaveImage *img = &gSaveImage[dev];
    u8 *b = (u8 *)img;
    s32 t, e;

    for (t = 0; t < (s32)sizeof(SaveImage); t++) {
        b[t] = 0;
    }
    for (t = 0; t < 15; t++) {
        for (e = 0; e < 10; e++) {
            img->hiscores[t][e] = gHiscores[0][t][e];
            if (t < 14) {
                img->records[t][e] = -1;
            }
        }
    }
    img->viewLeftX = 0;
    img->viewTopY = 0;
}

static s32 saveDeviceWrite(s32 dev) {
    *(u32 *)&gSaveImage[dev].header[0xC] = saveChecksum(&gSaveImage[dev]); /* (as the N64 kept it) */
    gSaveFile.magic = SAVE_MAGIC;
    gSaveFile.version = SAVE_VERSION;
    gSaveFile.size = sizeof(SaveImage);
    gSaveFile.image = gSaveImage[dev];
    gSaveFile.sum = fileSum(&gSaveFile);
    return plat_save_write(dev, &gSaveFile, sizeof(SaveFile));
}

/* 1: read; 0: no save there; -1: a save there that isn't usable */
static s32 saveDeviceRead(s32 dev) {
    s32 r = plat_save_read(dev, &gSaveFile, sizeof(SaveFile));
    if (r <= 0) {
        return r;
    }
    if (gSaveFile.magic != SAVE_MAGIC || (gSaveFile.version != SAVE_VERSION && gSaveFile.version != 1) ||
        gSaveFile.size != sizeof(SaveImage) || gSaveFile.sum != fileSum(&gSaveFile)) {
        return -1;
    }
    gSaveImage[dev] = gSaveFile.image;
    if (gSaveFile.version == 1) {        /* saved with the N64's buttons: the Dreamcast defaults */
        s32 n;
        for (n = 0; n < 6; n++) {
            if (gSaveImage[dev].slots[n].initials[0] != 0 || (gSaveImage[dev].slotsUsed & (1 << n))) {
                saveSlotControls(&gSaveImage[dev].slots[n]);
            }
        }
    }
    return 1;
}

/* 0x80035A5C ovl3 - the devices' tables into the game's (a missing second
 * device: the first one's), and the saved screen position */
static void saveTablesLoad(void) {
    s32 v, dev, t, e;

    if (gContReadState == 0) {
        return;
    }
    gViewLeftX = gSaveImage[0].viewLeftX;
    gViewTopY = gSaveImage[0].viewTopY;
    for (v = 0; v < 2; v++) {
        dev = (v < gContReadState) ? v : 0;
        for (t = 0; t < 15; t++) {
            for (e = 0; e < 10; e++) {
                gHiscores[v][t][e] = gSaveImage[dev].hiscores[t][e];
                if (t < 14) {
                    gTrackRecords[v][t][e] = gSaveImage[dev].records[t][e];
                }
            }
        }
    }
}

/* at boot (the N64's 0x8003F130 screens): find the VMUs and read them; a VMU
 * without a (good) save gets a new one */
void saveBoot(void) {
    s32 dev, p;

    for (p = 0; p < 2; p++) {
        gSaveDevOf[p] = -1;
    }
    gContReadState = plat_save_devices(gSaveDevPlayer);
    for (dev = 0; dev < gContReadState; dev++) {
        gSaveDevOf[gSaveDevPlayer[dev]] = dev;
        s32 r = saveDeviceRead(dev);
        if (r <= 0) {
            saveImageNew(dev);
            saveSlotNew(dev, 0);
            if (r == 0) {
                saveDeviceWrite(dev);    /* no save yet: a new one. (One that's there but can't be read
                                          * is left alone here, not wiped at power-on: only the game's
                                          * own saves later write over it) */
            }
        }
    }
    saveTablesLoad();
    /* (port) player 1's settings straight away: the volumes and options of the
     * title and its demos, and options changed there are saved too */
    if (gSaveDevOf[0] >= 0 && (gSaveImage[gSaveDevOf[0]].slotsUsed & 1)) {
        saveSlotLoad(0, gSaveDevOf[0], 0);
    }
}

/* the default controls (the N64's preset 1 on its buttons: 0x808 / 0x404 shift,
 * C-right display, B radio, A view, Z gas, L/R brake): here the Dreamcast's,
 * as port_main.c's first preset */
static void saveSlotControls(SaveSlot *s) {
    s->btnShiftUp = 0x0800;              /* d-pad up */
    s->btnShiftDown = 0x0400;            /* d-pad down */
    s->btnLights = 0x0080;               /* Y: display */
    s->btnMusic = 0x4000;                /* X: radio */
    s->btnView = 0x0040;                 /* B: view */
    s->gasMask = 0x0010;                 /* right trigger */
    s->brakeMask = 0x0020;               /* left trigger */
    s->gasAxis = 0;
    s->brakeAxis = 0;
}

/* 0x8003A394 ovl3 - a new slot: no initials, the first three tracks, the
 * default controls and options */
static void saveSlotNew(s32 dev, s32 n) {
    SaveSlot *s = &gSaveImage[dev].slots[n];
    u8 *b = (u8 *)s;
    s32 i;

    for (i = 0; i < (s32)sizeof(SaveSlot); i++) {
        b[i] = 0;
    }
    for (i = 0; i < 3; i++) {
        s->initials[i] = 0xFF;
    }
    for (i = 0; i < 7; i++) {
        s->carAwards[i] = 0;
    }
    s->tracksDone = 7;
    s->steerMode = 2;
    saveSlotControls(s);
    s->flags = 0x70;
    s->sfxVolume = 0.85f;
    s->musicVolume = 1.0f;
    /* the slot's name, "GAME 1" (0x800361C0) */
    for (i = 0; i < 8; i++) {
        s->pad00[i] = "GAME    "[i];
    }
    s->pad00[5] = '1' + n;
    gSaveImage[dev].slotsUsed |= 1 << n;
}

/* 0x8003A498 ovl3 - a slot into a player (player 1: also the options) */
static void saveSlotLoad(s32 p, s32 dev, s32 n) {
    SaveSlot *s = &gSaveImage[dev].slots[n];
    PlayerSetup *ps = &gPlayerSetup[p];
    s32 i;

    for (i = 0; i < 3; i++) {
        ps->initials[i] = s->initials[i];
    }
    for (i = 0; i < 7; i++) {
        ps->carAwards[i] = s->carAwards[i];
    }
    ps->tracksDone = s->tracksDone;
    gControls[p].steerMode = s->steerMode;
    gBtnShiftUp[p] = s->btnShiftUp;
    gBtnShiftDown[p] = s->btnShiftDown;
    gBtnLights[p] = s->btnLights;
    gBtnMusic[p] = s->btnMusic;
    gBtnView[p] = s->btnView;
    gGasMask[p] = s->gasMask;
    gBrakeMask[p] = s->brakeMask;
    gGasAxis[p] = s->gasAxis;
    gBrakeAxis[p] = s->brakeAxis;
    ps->unk328 = (s->flags & 0x80) != 0;
    if (p == 0) {
        gVolume[0] = s->musicVolume;
        gVolume[1] = s->sfxVolume;
        volumeApply(0, 0);
        volumeApply(1, 0);
        gUnitsKph = (s->flags & 8) != 0;
        gOptRacers = (s->flags & 0x20) != 0;
        gOptTraffic = (s->flags & 0x10) != 0;
        gOpt2PSound = (s->flags & 0x40) != 0;
        gDifficulty = s->flags & 7;
    }
}

/* 0x80035760 ovl3 - the players in 'players' (bit per player) take their own
 * VMU's save (made if new), then Choose Race (0x800364CC) */
void saveMenuInit(s32 players) {
    PlayerSetup *ps;
    s32 p, dev, dirty = 0;

    for (p = 0; p < 2; p++) {
        if (!((1 << p) & players)) {
            continue;
        }
        ps = &gPlayerSetup[p];
        dev = gSaveDevOf[p];
        if (dev < 0 || dev >= gContReadState) {
            ps->saveSlot = 6;
            continue;
        }
        if (!(gSaveImage[dev].slotsUsed & 1)) {
            saveSlotNew(dev, 0);
            dirty |= 1 << dev;
        }
        ps->saveSlot = 0;
        ps->unk35C = 0;
        ps->savePak = dev;
        ps->unk360 = dev;
        saveSlotLoad(p, dev, 0);
        contPressed(p, BTN_ALL);
    }
    if (dirty != 0) {
        saveWrite(NULL, dirty);
    }
    if (gCruiseMode != 0) {
        gCruiseMode = 0;
        gTrackChoice = 14;
        raceOptionsApply();
    }
    gModeFunc = raceSelectInit;
    gModeArg = 0;
    raceSelectInit(0);
}

/* 0x80035D0C ovl3 - (kept the images to write only what changed: here a
 * device is written whole) */
void saveBegin(void) {
}

/* 0x80035A30 ovl3 - write the devices in 'devices' (bits), then done */
void saveWrite(void (*done)(s32), s32 devices) {
    s32 dev;

    for (dev = 0; dev < gContReadState; dev++) {
        if (devices & (1 << dev)) {
            saveDeviceWrite(dev);
        }
    }
    if (done != NULL) {
        done(0);
    }
}

static void slotFlag(SaveSlot *s, s32 on, s32 bit) {
    if (on) {
        s->flags |= bit;
    } else {
        s->flags &= ~bit;
    }
}

/* 0x80035348 ovl3 - a player's controls (and player 1's options) into their slot */
static s32 saveSlotSettings(s32 p, s32 dev, s32 n) {
    SaveImage *img = &gSaveImage[dev];
    SaveSlot *s = &img->slots[n];
    s32 diff = 0;

    if (s->steerMode != gControls[p].steerMode || s->btnShiftUp != gBtnShiftUp[p] ||
        s->btnShiftDown != gBtnShiftDown[p] || s->btnLights != gBtnLights[p] || s->btnMusic != gBtnMusic[p] ||
        s->btnView != gBtnView[p] || s->gasMask != gGasMask[p] || s->brakeMask != gBrakeMask[p] ||
        s->gasAxis != gGasAxis[p] || s->brakeAxis != gBrakeAxis[p]) {
        diff = 1;
    }
    if (dev == 0 && (img->viewLeftX != gViewLeftX || img->viewTopY != gViewTopY)) {
        diff = 1;
    }
    if (p == 0 && (s->musicVolume != gVolume[0] || s->sfxVolume != gVolume[1] ||
                   (gUnitsKph == 0) != ((s->flags & 8) == 0) || (gOptRacers == 0) != ((s->flags & 0x20) == 0) ||
                   (gOptTraffic == 0) != ((s->flags & 0x10) == 0) ||
                   (gOpt2PSound == 0) != ((s->flags & 0x40) == 0) ||
                   (gCruiseMode == 0 && gDifficulty != (s->flags & 7)))) {
        diff = 1;
    }
    if (!diff) {
        return 0;
    }
    s->steerMode = gControls[p].steerMode;
    s->btnShiftUp = gBtnShiftUp[p];
    s->btnShiftDown = gBtnShiftDown[p];
    s->btnLights = gBtnLights[p];
    s->btnMusic = gBtnMusic[p];
    s->btnView = gBtnView[p];
    s->gasMask = gGasMask[p];
    s->brakeMask = gBrakeMask[p];
    s->gasAxis = gGasAxis[p];
    s->brakeAxis = gBrakeAxis[p];
    if (dev == 0) {
        img->viewLeftX = gViewLeftX;
        img->viewTopY = gViewTopY;
    }
    if (p == 0) {
        s->musicVolume = gVolume[0];
        s->sfxVolume = gVolume[1];
        slotFlag(s, gUnitsKph, 8);
        slotFlag(s, gOptRacers, 0x20);
        slotFlag(s, gOptTraffic, 0x10);
        slotFlag(s, gOpt2PSound, 0x40);
        s->flags &= ~7;
        if (gCruiseMode == 0) {
            s->flags |= gDifficulty;
        }
    }
    return 1;
}

/* 0x80035294 ovl3 - after the options: the players' settings into their slots */
void saveSettings(void) {
    PlayerSetup *ps;
    s32 p, dev, n, dirty = 0;

    if (gContReadState == 0) {
        return;
    }
    saveBegin();
    for (p = 0; p < 2; p++) {
        ps = &gPlayerSetup[p];
        dev = ps->savePak;
        n = ps->saveSlot;
        if (n >= 6 && gSaveDevOf[p] >= 0 && (gSaveImage[gSaveDevOf[p]].slotsUsed & 1)) {
            dev = gSaveDevOf[p];         /* (port) not in a game yet (the title): their VMU's save */
            n = 0;
        }
        if (n < 6 && saveSlotSettings(p, dev, n)) {
            dirty |= 1 << dev;
        }
    }
    if (dirty != 0) {
        saveWrite(NULL, dirty);
    }
}
