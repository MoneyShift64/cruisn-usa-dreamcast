/*
 * port_main.c - start-up and the parts of the main program that talk to the
 * N64 hardware (threads, DMA, the RSP scheduler), replaced for the port.
 * Everything else is the decompiled game, unchanged.
 */
#include "common.h"
#include "frontend.h"
#include "port.h"
void sndStopAll(s32 fade, s32 time);     /* 0x80107C08 */

/* 1: a test build with the snow cheat on from the start (snow on every track, its tune) */

#define FB_SIZE (320 * 240 * 2)
#define ALIGN64(x) (((x) + 0x3F) & ~0x3F)
#define DEMO_LOGO_X  0x8C0000             /* (y counts up from the bottom; the logo's art is */
#define DEMO_LOGO_Y  0x9C0000             /* centred 20 px right of and above this point) */
#define DEMO_NAME_Y  0x600000             /* "DREAMCAST", with "DEMO" a line below */
#define DEMO_DEMO_Y  0x460000
#define DEMO_TRACK_FONT 2
#define DEMO_PRESS_Y 0x1C0000

u8 gPortRam[PORT_RAM_SIZE] __attribute__((aligned(64)));
u8 *gPortAssets;
u32 gPortAssetsSize;

/* ---- the game's globals and functions used here ---- */
extern u32 gLowMemStart, gLowMemSize, gFramebuffers[3];
extern u32 gBufferSetA[3], gBufferSetB[3], gBufferSetC[3], gBufferSetD[3];
extern u32 gOverlayHeapStart, gOverlayHeapLimit, gFramebuffersEnd, gHighHeapEnd, gHeapPtr, gHighHeapPtr;
extern GfxSlot gGfxSlots[3];
extern s32 gGfxBuildingSlot, gGfxPendingSlot, gGfxRunningSlot;
extern s32 gFrameCredits, gFrameCounter, gLastShownFrame;
extern u32 *gDlCursor;
extern u8 *gMtxCursor, *gVtxCursor, *gVpCursor;
extern s32 gOverlayCurrent;
extern u32 gRandomSeed;
extern s32 gCurrentOverlay;
typedef struct ContSnap { u32 buttons; s32 x, y, fresh; } ContSnap;
extern ContSnap gContSnap[2];
extern s32 gContFound, gContFoundPorts[2];

void gameMainLoop(void);
void animCancelAll(void);
void animBlinkInit(void);
void textFreeAll(void);                  /* 0x8011A9D4 */
u32  lowMemOverlayLoad(void);
void lowMemOverlayRelease(void);
void gameSetupReset(void);
void playerSetupReset(s32 player);
void playerSetupView(s32 player);
void raceOptionsApply(void);
void trackLoad(s32 track);
void trackPrepare(void);
void raceStart(s32 arg);
void controlsSetMapping(s32 player, s32 config);
void carModelsLoad(void);                /* 0x80117D00 */
void trafficModelsLoad(void);            /* 0x80117DB4 */
void carPalettesLoad(void);              /* 0x80117DEC */
void musicReset(void);                   /* 0x8011F260 */
u32  highHeapMark(void);                 /* 0x80000A64 */
void highHeapRestore(u32 mark);          /* 0x80000A74 */
extern u16 gHudPalSave[28];              /* 0x801509C0 */
void port_sound_init(void);              /* port/audio.c */


/* ------------------------------------------------------------------ */
/* Interrupts, OS bits                                                */
/* ------------------------------------------------------------------ */
s32 disableInterrupts(void) {
    return 0;
}

void restoreInterrupts(s32 mask) {
}

u32 osVirtualToPhysical(void *p) {
    return (u32)p + 0x80000000u;
}

/* ------------------------------------------------------------------ */
/* ROM access: the asset pack stands in for the cartridge             */
/* ------------------------------------------------------------------ */
#define ASSET_ROM 0x80470

/* 0x801035D0 */
void dmaReadSync(u32 rom, u32 ram, u32 size, s32 flags) {
    u8 *dst = (u8 *)ram;
    u32 i;

    if (rom >= ASSET_ROM && rom - ASSET_ROM + size <= gPortAssetsSize) {
        u8 *src = gPortAssets + (rom - ASSET_ROM);
        for (i = 0; i < size; i++) {
            dst[i] = src[i];
        }
        port_mem_written(dst, size);
        return;
    }
    port_log("dmaReadSync: ROM %08x size %x not in the asset pack (file %x?, from %p)\n", rom, size,
             (rom - ASSET_ROM - 4) / 8, __builtin_return_address(0));
}

/* 0x80103264 - an overlay's code is native; its variables start afresh */
void overlayCopyIn(s32 id) {
    port_overlay_reset(id);
}

/* ------------------------------------------------------------------ */
/* Frames                                                             */
/* ------------------------------------------------------------------ */
static void input_poll(void) {
    s32 i, n = 0;
    u16 b;
    s8 x, y;

    for (i = 0; i < 2; i++) {
        if (plat_pad(i, &b, &x, &y)) {
            gContFoundPorts[n] = i;
            gContSnap[n].buttons = b;
            gContSnap[n].x = x;
            gContSnap[n].y = y;
            gContSnap[n].fresh = 1;
            n++;
        }
    }
    gContFound = n;
}

/* 0x801039E0 - always slot 0: the frame is drawn as soon as it is built */
s32 gfxBeginFrame(void) {
    GfxSlot *slot = &gGfxSlots[0];

    slot->free = FALSE;
    gDlCursor = slot->dlStart;
    gMtxCursor = slot->mtxStart + 0x40;
    gVtxCursor = slot->vtxStart;
    gVpCursor = slot->vpStart;
    slot->unk24 = 0;
    slot->frameNum = gFrameCounter;
    gGfxBuildingSlot = 0;
    input_poll();
    return 0;
}

/* 0x80103B10 - the RSP gets each matrix one slot below where the game built it
 * (mtxConvertRange); here they stay 16.16 and only move down */
void gfxEndFrame(s32 userArg) {
    GfxSlot *slot = &gGfxSlots[0];
    u32 *src, *dst, *end;

    gFrameCounter++;
    {   /* the N64 counted vertical blanks in its scheduler (schedOnVi), which the port
         * doesn't run: two a frame at 30 frames a second. Flashing (the next-award
         * marker at the end, the HUD's lights) and animated scenery go by it. */
        extern u32 gViCount;
        gViCount += 2;
    }
    dst = (u32 *)slot->mtxStart;
    src = dst + 16;
    end = (u32 *)gMtxCursor;
    while (src < end) {
        *dst++ = *src++;
    }
    gfx_run(slot->dlStart);
    {   void port_vmu_frame(void);
        port_vmu_frame();                /* the VMU screen */
    }
    gLastShownFrame = slot->frameNum;
    slot->free = TRUE;
    gGfxBuildingSlot = -1;
    plat_frame_end();
}

/* 0x80103D88 */
s32 gfxWaitIdle(void) {
    return gFramebuffers[0];
}

/* ------------------------------------------------------------------ */
/* Start-up                                                           */
/* ------------------------------------------------------------------ */
static void memory_layout(void) {
    s32 i;

    /* as bootMainThread (src/boot.c), at the same N64 offsets inside gPortRam */
    gLowMemStart = PORT_PHYS(0x480);
    gLowMemSize = 0x3B6C00;
    gFramebuffers[0] = PORT_PHYS(ALIGN64(0x165810));
    gFramebuffers[1] = ALIGN64(gFramebuffers[0] + FB_SIZE);
    gFramebuffers[2] = ALIGN64(gFramebuffers[1] + FB_SIZE);
    gBufferSetA[0] = PORT_PHYS(0x39EC00);
    gBufferSetA[1] = PORT_PHYS(0x3A6C00);
    gBufferSetA[2] = PORT_PHYS(0x3AEC00);
    gBufferSetB[0] = PORT_PHYS(0x36FE00);
    gBufferSetB[1] = PORT_PHYS(0x37F800);
    gBufferSetB[2] = PORT_PHYS(0x38F200);
    gBufferSetC[0] = PORT_PHYS(0x364280);
    gBufferSetC[1] = PORT_PHYS(0x368100);
    gBufferSetC[2] = PORT_PHYS(0x36BF80);
    gBufferSetD[0] = PORT_PHYS(0x363EC0);
    gBufferSetD[1] = PORT_PHYS(0x364000);
    gBufferSetD[2] = PORT_PHYS(0x364140);
    gOverlayHeapStart = PORT_PHYS(0x25C80);
    gOverlayHeapLimit = PORT_PHYS(0x100000);
    gFramebuffersEnd = gFramebuffers[2] + FB_SIZE;
    gHighHeapEnd = PORT_PHYS(0x363EC0);
    gHeapPtr = gOverlayHeapStart;
    gHighHeapPtr = gHighHeapEnd & ~7;

    /* as the boot overlay (ROM 0x3D860) */
    gFrameCounter = 1;
    gLastShownFrame = 0;
    for (i = 0; i < 3; i++) {
        gGfxSlots[i].dlStart = (u32 *)(gBufferSetA[i] + 0x80000000);
        gGfxSlots[i].mtxStart = (u8 *)(gBufferSetB[i] + 0x80000000);
        gGfxSlots[i].vtxStart = (u8 *)(gBufferSetC[i] + 0x80000000);
        gGfxSlots[i].vpStart = (u8 *)(gBufferSetD[i] + 0x80000000);
        gGfxSlots[i].free = TRUE;
    }
    gFrameCredits = 1;
    gGfxBuildingSlot = -1;
    gGfxPendingSlot = -1;
    gGfxRunningSlot = -1;
}

extern s32 gControls[];                  /* 0x8014E378 - struct Controls (src/input.c), 0x18 bytes each */
void hiscoresDefault(void);
void saveBoot(void);

static void boot_game(void) {
    s32 p;

    /* the boot overlay's set-up (the parts the race needs so far) */
    gRandomSeed = 0x789ABCDE;
    gVolume[0] = 1.0f;                   /* music, effects (the boot overlay's defaults) */
    gVolume[1] = 0.85f;
    gOpt2PSound = 1;
    musicReset();
    port_sound_init();
    {
        extern s32 gSnowOn;
        gSnowOn = 0;
        {   /* by the console's clock: Christmas Eve and Christmas Day snow (and Jingle Bells), April 1st
             * rain, on by themselves; typing the code turns it off as usual */
            int y, m, d;
            extern s32 gRainOn;
            if (plat_date(&y, &m, &d)) {
                if (m == 12 && (d == 24 || d == 25)) {
                    gSnowOn = 1;
                }
                if (m == 4 && d == 1) {
                    gRainOn = 1;         /* April 1st: rain (APRILSHOWERS turns it off) */
                }
            }
        }
    }
    animCancelAll();
    animBlinkInit();
    textFreeAll();
    lowMemOverlayLoad();
    gFontPal = (u32)assetLoad(0x2BD);
    assetLoadGroup(0x2BC, (void **)gFontImages);
    assetLoadGroup(0x2E3, (void **)gHudImages);
    lowMemOverlayRelease();
    carModelsLoad();
    trafficModelsLoad();
    carPalettesLoad();
    {   /* the rev counter's and gear lights' colours, from the HUD's palette (file 0x2EA) */
        u32 mark;
        u16 *pal;
        s32 i;
        lowMemOverlayLoad();
        mark = highHeapMark();
        pal = (u16 *)((u8 *)PORT_PTR((u32)assetLoad(0x2EA)) + 0x1D8);
        for (i = 0; i < 24; i++) {
            gHudPalSave[i] = pal[i];
        }
        for (i = 0; i < 4; i++) {
            gHudPalSave[24 + i] = pal[5 + i];
        }
        highHeapRestore(mark);
        lowMemOverlayRelease();
    }
    gameSetupReset();
    hiscoresDefault();                   /* the default high-score tables (src/bootovl.c) */
    {   /* the boot overlay's stick calibration (0x80025FE4): centre 0.5, dead zone 0.1
         * (left at 0 the menus' stick values were infinite: the HOT TIMES camera sank) */
        extern f32 gStickDead[2];
        extern f32 gStickCenter[2][2];
        for (p = 0; p < 2; p++) {
            gStickCenter[p][0] = 0.5f;
            gStickCenter[p][1] = 0.5f;
            gStickDead[p] = 0.1f;
        }
    }
    gOptRacers = 1;                      /* the boot overlay's default options */
    gOptTraffic = 1;
    raceOptionsApply();
    gOverlayCurrent = 0x7FFF;
    {   /* (port) the Dreamcast's four presets (B on CONTROLLER SETUP, which is X here):
         * the N64's with Z -> right trigger, L/R -> left trigger, C-right -> Y,
         * N64 B -> X, N64 A -> B and no C-up/C-down shifting (plat_dc.c plat_pad).
         * Preset 1 (the default): gas right trigger, brake left trigger, shift
         * d-pad up / down, display Y, radio X, view B. */
        static const u32 dc[4][7][2] = {
            { { 0x0010, 0 }, { 0x0020, 0 }, { 0x0800, 0 }, { 0x0400, 0 }, { 0x0080, 0 }, { 0x4000, 0 }, { 0x0040, 0 } },
            { { 0x10000, 1 }, { 0x20000, (u32)-1 }, { 0x0800, 0 }, { 0x0400, 0 }, { 0x0080, 0 }, { 0x4000, 0 }, { 0x0040, 0 } },
            { { 0x0010, 0 }, { 0x0020, 0 }, { 0x10000, 0 }, { 0x20000, 0 }, { 0x0080, 0 }, { 0x4000, 0 }, { 0x0040, 0 } },
            { { 0x0800, 0 }, { 0x0400, 0 }, { 0x10000, 0 }, { 0x20000, 0 }, { 0x0080, 0 }, { 0x4000, 0 }, { 0x0040, 0 } },
        };
        extern u32 gControlConfigs[4][7][2];     /* 0x80132ABC - ControlPair[4][7] (src/input.c) */
        s32 c, k;
        for (c = 0; c < 4; c++) {
            for (k = 0; k < 7; k++) {
                gControlConfigs[c][k][0] = dc[c][k][0];
                gControlConfigs[c][k][1] = dc[c][k][1];
            }
        }
    }
    for (p = 0; p < 2; p++) {
        playerSetupReset(p);
        controlsSetMapping(p, 0);
        /* the boot overlay's other defaults (0x80026460): the first three tracks
         * unlocked in Choose Race, no car awards, steering response 2 */
        gPlayerSetup[p].tracksDone = 7;
        gPlayerSetup[p].unk35C = 6;
        gPlayerSetup[p].unk360 = p;
        gControls[p * 6 + 5] = 2;          /* steerMode */
    }
    saveBoot();                          /* the VMUs: saved tables and settings (port/save.c) */
}



void port_main(s32 track) {
    gPortAssets = plat_load_file("ASSETS.BIN", &gPortAssetsSize);
    if (gPortAssets == NULL) {
        port_fatal("ASSETS.BIN not found\n");
    }
    port_segments_init();
    memory_layout();
    boot_game();
    /* as the boot overlay ends: into the front end (the intro credits are not converted yet) */
    gModeArg = 0;
    gModeFunc = frontEndEnter;
    gOverlayRequest = OVERLAY_REQUEST(OVL_FRONTEND);
    gameMainLoop();
}

/* ------------------------------------------------------------------ */
/* Snow (the snow cheat): flakes appear ahead of the camera, in a     */
/* cone that narrows as the car speeds up, drift down swaying from    */
/* side to side, and are drawn with a snowflake picture               */
/* (port/art/snowflake.h) facing the camera, white, see-through.      */
/* Called from the race's drawing (src/race/race.c) after the         */
/* particles: in front of the scenery, behind the HUD.                */
/* ------------------------------------------------------------------ */
#include "race.h"
#include "art/snowflake.h"
s32 gSnowOn;                             /* the cheat: snow on every track (and its tune, pause.c) */
extern u32 gRaceBaseDL[];                /* identity matrix (RSP format) */
extern s32 gCountdownPhase;

/* sizes in the race's drawing units (about 2.5 cm; the car is ~180 long, ~56 a frame at 94 mph) */
#define SNOW_N    128                    /* flakes */
#define SNOW_NEAR 225.0f                 /* they appear this far ahead */
#define SNOW_FAR  2475.0f
#define SNOW_SIZE 9.0f                   /* half a flake's width */
#define SNOW_SUB  4.0f                   /* vertex units per drawing unit (vertices are whole numbers) */
typedef struct SnowVtx { s16 x, y, z; u16 flag; s16 s, t; u8 c[4]; } SnowVtx;
typedef struct Flake {
    f32 x, y, z;                         /* where it is (drawing units) */
    f32 vy;                              /* falling (a frame) */
    f32 sway;                            /* how far it drifts either side */
    u16 angle, turn;                     /* the drift's phase, and how fast it goes round */
    s32 alive;
} Flake;
static Flake gFlakes[SNOW_N];
static s32 gSnowMoved = -1, gSnowBuf;
static SnowVtx gSnowVtx[3][SNOW_N * 4] __attribute__((aligned(16)));
static u32 gSnowDL[3][2 * (24 + SNOW_N / 4 * 9)] __attribute__((aligned(8)));
static s32 gSnowMtx[3][16] __attribute__((aligned(8)));

static u32 snow_rand(u32 n) {            /* 0 .. n-1 */
    static u32 s = 0x12345678;
    s = s * 1103515245u + 12345u;
    return (s >> 8) % n;
}

/* sin of a binary angle (0x10000 = a turn), good enough for a drift */
static f32 snow_sin(u16 a) {
    f32 x = (a & 0x7FFF) * (2.0f / 32768.0f) - 1.0f;    /* -1..1 over half a turn */
    f32 y = 1.0f - x * x;                                /* a parabola for the hump */
    return (a & 0x8000) ? -y : y;
}

/* a new flake ahead of the camera */
static void flake_new(Flake *f, RaceCamera *cam, f32 fwdX, f32 fwdZ, f32 speed) {
    f32 slow = speed / 56.0f;            /* 1 at about 94 mph */
    f32 spread = 0.6f / (1.0f + slow * 1.5f);           /* the cone: about +-34 degrees standing still */
    f32 a = ((s32)snow_rand(2001) - 1000) * (spread / 1000.0f);
    f32 d = SNOW_NEAR + snow_rand(1000) * ((SNOW_FAR - SNOW_NEAR) / 1000.0f);
    f32 c = 1.0f - a * a * 0.5f, s = a;                  /* (small angles) */
    f->x = cam->posX + (fwdX * c - fwdZ * s) * d;
    f->z = cam->posZ + (fwdZ * c + fwdX * s) * d;
    if (slow > 1.0f) {
        slow = 1.0f;
    }
    /* high up standing still (they drift down into view); spread down to the road when fast,
     * as the car passes them before they fall far */
    f->y = cam->posY + 340.0f - slow * 520.0f + snow_rand((u32)(110.0f + slow * 640.0f));
    f->vy = -(2.2f + snow_rand(50) * 0.075f);
    f->sway = 15.0f + snow_rand(100) * 0.22f;
    f->angle = snow_rand(0x10000);
    f->turn = 0x100 + snow_rand(0x400);
    f->alive = 1;
}

/* the display list for the weather (snow, rain): n camera-facing quads (v, 4 corners each), a 16-wide
 * I4 picture texH high (loaded as a block), colour combine (hi, lo), see-through, depth compared, at
 * 1/SNOW_SUB scale (m), the drawing state kept and put back around it; called from the race (gDlCursor) */
static void weather_list(u32 *d, SnowVtx *v, s32 n, s32 *m, const u8 *tex, s32 texH, u32 hi, u32 lo) {
    u32 *g;
    s32 i;
    for (i = 0; i < 16; i++) {
        m[i] = 0;
    }
    m[0] = m[5] = m[10] = (s32)(65536.0f / SNOW_SUB);
    m[15] = 0x10000;
    g = d;
    *d++ = 0xE7000000; *d++ = 0;                       /* gDPPipeSync */
    *d++ = 0x02000000; *d++ = 0;                       /* (the port's own: keep the drawing state) */
    *d++ = 0xB6000000; *d++ = 0x00023000;              /* no lighting, no culling */
    *d++ = 0xBB000001; *d++ = 0xFFFFFFFF;              /* texture on */
    *d++ = hi; *d++ = lo;                              /* colour combine */
    *d++ = 0xB900031D; *d++ = 0x00504DD8;              /* render mode: see-through, depth compare */
    *d++ = 0xFD900000; *d++ = (u32)tex;                /* texture image: I, 16-bit (loaded as a block) */
    *d++ = 0xF5900000; *d++ = 0x07080200;              /* tile 7 (loading): clamp */
    *d++ = 0xE6000000; *d++ = 0;                       /* gDPLoadSync */
    *d++ = 0xF3000000; *d++ = 0x07000800 | ((16 * texH / 4 - 1) << 12);   /* load block, dxt 2048 */
    *d++ = 0xE7000000; *d++ = 0;
    *d++ = 0xF5800200; *d++ = 0x00080200;              /* tile 0: I4, 8 bytes a line, clamp */
    *d++ = 0xF2000000; *d++ = (15 << 14) | ((texH - 1) << 2);   /* tile size 16 x texH */
    *d++ = 0x01020040; *d++ = (u32)m;                  /* modelview: 1/SNOW_SUB scale */
    for (i = 0; i < n; i += 4) {
        s32 c = n - i < 4 ? n - i : 4, j;
        *d++ = 0x04000000 | ((c * 4 - 1) << 20) | (c * 4 * 16); *d++ = (u32)&v[i * 4];
        for (j = 0; j < c; j++) {
            s32 q = j * 4;
            *d++ = 0xBF000000; *d++ = ((q * 10) << 16) | (((q + 1) * 10) << 8) | ((q + 2) * 10);
            *d++ = 0xBF000000; *d++ = ((q * 10) << 16) | (((q + 2) * 10) << 8) | ((q + 3) * 10);
        }
    }
    *d++ = 0x01020040; *d++ = (u32)gRaceBaseDL;        /* modelview: back to identity */
    *d++ = 0xE7000000; *d++ = 0;
    *d++ = 0x02000000; *d++ = 1;                       /* (put the drawing state back) */
    *d++ = 0xB8000000; *d++ = 0;                       /* end */
    d = gDlCursor; gDlCursor = d + 2;
    d[0] = 0x06000000; d[1] = (u32)g;                  /* gSPDisplayList */
}

void port_snow_draw(s32 player) {
    static f32 lastX, lastZ;
    RaceCamera *cam = &gRaceCameras[player];
    f32 rx, ry, rz, ux, uy, uz, fx, fy, fz, fwdX, fwdZ, len, speed;
    SnowVtx *v;
    u32 *d, *g;
    s32 *m;
    s32 i, n, b;

    if (!gSnowOn || gCountdownPhase <= 0) {
        return;
    }
    /* the camera's axes in the world: the columns of its view matrix (it looks down -z) */
    rx = cam->rot[0][0] / 65536.0f; ry = cam->rot[1][0] / 65536.0f; rz = cam->rot[2][0] / 65536.0f;
    ux = cam->rot[0][1] / 65536.0f; uy = cam->rot[1][1] / 65536.0f; uz = cam->rot[2][1] / 65536.0f;
    fx = cam->rot[0][2] / 65536.0f; fy = cam->rot[1][2] / 65536.0f; fz = cam->rot[2][2] / 65536.0f;
    len = sqrtf(fx * fx + fz * fz);
    fwdX = len > 0.01f ? -fx / len : 0.0f;
    fwdZ = len > 0.01f ? -fz / len : -1.0f;

    if (gSnowMoved != gFrameCounter && player == 0) {   /* once a frame */
        gSnowMoved = gFrameCounter;
        speed = sqrtf((cam->posX - lastX) * (cam->posX - lastX) + (cam->posZ - lastZ) * (cam->posZ - lastZ));
        if (speed > 200.0f) {
            speed = 0.0f;                /* (the camera jumped) */
        }
        lastX = cam->posX;
        lastZ = cam->posZ;
        for (i = 0; i < SNOW_N; i++) {
            Flake *f = &gFlakes[i];
            f32 x, y, z, dz;
            if (!f->alive) {
                flake_new(f, cam, fwdX, fwdZ, speed);
                f->y += snow_rand(400);  /* (the first ones fill the air) */
                continue;
            }
            f->y += f->vy;
            f->angle += f->turn;
            x = f->x - cam->posX; y = f->y - cam->posY; z = f->z - cam->posZ;
            dz = x * fx + y * fy + z * fz;
            if (y < -400.0f || dz > -60.0f || x * x + z * z > SNOW_FAR * SNOW_FAR * 1.4f) {
                flake_new(f, cam, fwdX, fwdZ, speed);   /* landed, passed, or left behind */
            }
        }
    }

    b = gSnowBuf;
    gSnowBuf = (gSnowBuf + 1) % 3;
    v = gSnowVtx[b];
    n = 0;
    for (i = 0; i < SNOW_N; i++) {
        Flake *f = &gFlakes[i];
        f32 sw = snow_sin(f->angle) * f->sway;
        f32 x = f->x - cam->posX + rx * sw, y = f->y - cam->posY + ry * sw, z = f->z - cam->posZ + rz * sw;
        f32 dz = x * fx + y * fy + z * fz, a;
        s32 k, al;
        if (!f->alive || dz > -40.0f) {
            continue;
        }
        a = (SNOW_FAR + dz) * (1.0f / 600.0f);           /* fading in far away */
        al = a >= 1.0f ? 255 : a <= 0.0f ? 0 : (s32)(a * 255.0f);
        if (al == 0) {
            continue;
        }
        for (k = 0; k < 4; k++) {
            f32 cx = (k == 1 || k == 2) ? SNOW_SIZE : -SNOW_SIZE, cy = (k >= 2) ? SNOW_SIZE : -SNOW_SIZE;
            SnowVtx *o = &v[n * 4 + k];
            o->x = (s16)((x + rx * cx + ux * cy) * SNOW_SUB);
            o->y = (s16)((y + ry * cx + uy * cy) * SNOW_SUB);
            o->z = (s16)((z + rz * cx + uz * cy) * SNOW_SUB);
            o->flag = 0;
            o->s = (k == 1 || k == 2) ? 16 << 5 : 0;
            o->t = (k >= 2) ? 0 : 16 << 5;
            o->c[0] = o->c[1] = o->c[2] = 255;
            o->c[3] = al;
        }
        n++;
    }
    if (n == 0) {
        return;
    }
    /* white (shade) with the picture as the see-through-ness */
    weather_list(gSnowDL[b], v, n, gSnowMtx[b], gSnowflake, 16, 0xFC627EC4, 0xFFFFF3F9);
}

/* ------------------------------------------------------------------ */
/* Rain (the APRILSHOWERS cheat), after Top Gear Rally's              */
/* (src/tgrally/racing/weather.c in the brally decompilation): drops  */
/* in a box that travels with the camera, falling fast with a little  */
/* wind, each drawn as a streak along the way it moves past the       */
/* camera (so it slants as the car speeds up), brightest at its head, */
/* in TGR's cool blue-grey. The streak picture is made here (TGR      */
/* draws its own streak too: no game art). Called with the snow.      */
/* ------------------------------------------------------------------ */
s32 gRainOn;
#define RAIN_N    300                    /* drops */
#define RAIN_R    900.0f                 /* the box: +-RAIN_R across, +-RAIN_H up and down, */
#define RAIN_AHEAD 1000.0f               /* its middle this far ahead */
#define RAIN_NEAR 300.0f                 /* none nearer */
#define RAIN_H    500.0f
#define RAIN_FALL 34.0f                  /* a frame (build 84: twice as fast; it looked still at speed) */
#define RAIN_W    2.5f                   /* half a streak's width */
#define RAIN_LEN  45.0f                  /* and its length */
static f32 gRainP[RAIN_N][3];
static u8 gRainSpeed[RAIN_N];
static s32 gRainReady, gRainMoved = -1, gRainBuf;
static f32 gRainCam[3], gRainVel[3];     /* the camera last frame, and its move */
static SnowVtx gRainVtx[3][RAIN_N * 4] __attribute__((aligned(16)));
static u32 gRainDL[3][2 * (24 + RAIN_N / 4 * 9)] __attribute__((aligned(8)));
static s32 gRainMtx[3][16] __attribute__((aligned(8)));
static u8 gRainTex[16 * 32 / 2] __attribute__((aligned(8)));   /* 16x32 I4: the streak */

static void rain_tex(void) {
    s32 x, y;
    for (y = 0; y < 32; y++) {
        for (x = 0; x < 16; x++) {
            s32 across = x < 8 ? x : 15 - x;          /* 0 at the edges, 7 in the middle */
            s32 v = across >= 3 ? 15 : across >= 2 ? 8 : across >= 1 ? 2 : 0;   /* (thin: mostly solid) */
            v = v * (8 + y) / 39;                      /* faint at the tail, full at the head */
            if (x & 1) {
                gRainTex[y * 8 + x / 2] |= v;
            } else {
                gRainTex[y * 8 + x / 2] = v << 4;
            }
        }
    }
}

void port_rain_draw(s32 player) {
    RaceCamera *cam = &gRaceCameras[player];
    f32 rx, ry, rz, ux, uy, uz, fx, fy, fz, cx, cz;
    SnowVtx *v;
    u32 *d, *g;
    s32 *m;
    s32 i, n, b;

    if (!gRainOn || gCountdownPhase <= 0) {
        return;
    }
    rx = cam->rot[0][0] / 65536.0f; ry = cam->rot[1][0] / 65536.0f; rz = cam->rot[2][0] / 65536.0f;
    fx = cam->rot[0][2] / 65536.0f; fy = cam->rot[1][2] / 65536.0f; fz = cam->rot[2][2] / 65536.0f;
    ux = cam->rot[0][1] / 65536.0f; uy = cam->rot[1][1] / 65536.0f; uz = cam->rot[2][1] / 65536.0f;
    if (!gRainReady) {
        rain_tex();
        for (i = 0; i < RAIN_N; i++) {
            gRainP[i][0] = cam->posX + ((s32)snow_rand(2001) - 1000) * (RAIN_R / 1000.0f);
            gRainP[i][1] = cam->posY + ((s32)snow_rand(2001) - 1000) * (RAIN_H / 1000.0f);
            gRainP[i][2] = cam->posZ + ((s32)snow_rand(2001) - 1000) * (RAIN_R / 1000.0f);
            gRainSpeed[i] = 200 + snow_rand(56);
        }
        gRainCam[0] = cam->posX; gRainCam[1] = cam->posY; gRainCam[2] = cam->posZ;
        gRainReady = 1;
    }
    if (gRainMoved != gFrameCounter && player == 0) {   /* once a frame: fall, and the camera's move */
        gRainMoved = gFrameCounter;
        gRainVel[0] = cam->posX - gRainCam[0];
        gRainVel[1] = cam->posY - gRainCam[1];
        gRainVel[2] = cam->posZ - gRainCam[2];
        if (gRainVel[0] * gRainVel[0] + gRainVel[1] * gRainVel[1] + gRainVel[2] * gRainVel[2] > 200.0f * 200.0f) {
            gRainVel[0] = gRainVel[1] = gRainVel[2] = 0.0f;   /* (the camera jumped) */
        }
        gRainCam[0] = cam->posX; gRainCam[1] = cam->posY; gRainCam[2] = cam->posZ;
        for (i = 0; i < RAIN_N; i++) {
            gRainP[i][1] -= RAIN_FALL * gRainSpeed[i] * (1.0f / 228.0f);
            gRainP[i][0] += 1.5f;                       /* the wind */
        }
    }

    {   /* the middle of the box: RAIN_AHEAD in front */
        f32 l = sqrtf(fx * fx + fz * fz);
        cx = cam->posX - (l > 0.01f ? fx / l : 0.0f) * RAIN_AHEAD;
        cz = cam->posZ - (l > 0.01f ? fz / l : 1.0f) * RAIN_AHEAD;
    }
    b = gRainBuf;
    gRainBuf = (gRainBuf + 1) % 3;
    v = gRainVtx[b];
    n = 0;
    for (i = 0; i < RAIN_N; i++) {
        f32 *p = gRainP[i];
        f32 x, y, z, dz, a, mx, my, mz, len, wx, wy, wz, wl, tx, ty, tz;
        s32 k, al;
        /* the box is ahead of the camera (nothing nearer than about 450 is drawn: the near plane) */
        x = p[0] - cx;
        if (x > RAIN_R) { p[0] -= 2 * RAIN_R; x -= 2 * RAIN_R; } else if (x < -RAIN_R) { p[0] += 2 * RAIN_R; x += 2 * RAIN_R; }
        z = p[2] - cz;
        if (z > RAIN_R) { p[2] -= 2 * RAIN_R; z -= 2 * RAIN_R; } else if (z < -RAIN_R) { p[2] += 2 * RAIN_R; z += 2 * RAIN_R; }
        y = p[1] - cam->posY;
        if (y < -RAIN_H) { p[1] += 2 * RAIN_H; y += 2 * RAIN_H; } else if (y > RAIN_H) { p[1] -= 2 * RAIN_H; y -= 2 * RAIN_H; }
        x = p[0] - cam->posX;
        z = p[2] - cam->posZ;
        dz = x * fx + y * fy + z * fz;
        if (dz > -RAIN_NEAR) {
            continue;
        }
        a = (RAIN_AHEAD + RAIN_R + dz) * (1.0f / 500.0f);   /* fading in far away */
        if (a > 1.0f) {
            a = 1.0f;
        }
        if (a <= 0.05f) {
            continue;
        }
        al = a >= 0.8f ? 255 : (s32)(a * (255.0f / 0.8f));
        /* the streak, in the screen's plane (as TGR draws it): along the drop's move past the camera as
         * the camera sees it (its fall and the wind, less some of the camera's own move), tail behind */
        mx = 1.5f - gRainVel[0] * 0.25f;
        my = -RAIN_FALL - gRainVel[1] * 0.25f;
        mz = -gRainVel[2] * 0.25f;
        {
            f32 sx = mx * rx + my * ry + mz * rz, sy = mx * ux + my * uy + mz * uz, sl = sqrtf(sx * sx + sy * sy);
            if (sl < 0.5f) {
                sx = 0.0f; sy = -1.0f; sl = 1.0f;       /* (straight at the camera: as falling) */
            }
            sx /= sl; sy /= sl;
            tx = -(rx * sx + ux * sy) * RAIN_LEN; ty = -(ry * sx + uy * sy) * RAIN_LEN; tz = -(rz * sx + uz * sy) * RAIN_LEN;
            wx = (rx * sy - ux * sx) * RAIN_W; wy = (ry * sy - uy * sx) * RAIN_W; wz = (rz * sy - uz * sx) * RAIN_W;
        }
        for (k = 0; k < 4; k++) {
            f32 sd = (k == 1 || k == 2) ? 1.0f : -1.0f;  /* side */
            f32 tl = (k >= 2) ? 0.0f : 1.0f;             /* 1: the tail, 0: the head */
            SnowVtx *o = &v[n * 4 + k];
            o->x = (s16)((x + tx * tl + wx * sd) * SNOW_SUB);
            o->y = (s16)((y + ty * tl + wy * sd) * SNOW_SUB);
            o->z = (s16)((z + tz * tl + wz * sd) * SNOW_SUB);
            o->flag = 0;
            o->s = sd > 0 ? 16 << 5 : 0;
            o->t = tl > 0 ? 0 : 32 << 5;
            o->c[0] = 200; o->c[1] = 210; o->c[2] = 230;   /* TGR's blue-grey (0x788088), a little lighter */
            o->c[3] = al;
        }
        n++;
    }
    if (n == 0) {
        return;
    }
    /* its colour (shade), see-through as the streak and the distance (TEXEL0 x SHADE) */
    weather_list(gRainDL[b], v, n, gRainMtx[b], gRainTex, 32, 0xFC6218C4, 0xFF33FFFF);
}

/* ------------------------------------------------------------------ */
/* Cheats: typed on a Dreamcast keyboard (any port) on the Choose     */
/* Race screen, then Enter (src/frontend/raceselect.c calls these).   */
/* A right code plays the "cheat" sound (effect 0x19, the ROM's wave  */
/* 103) and shows CHEAT ENABLED and the cheat's name for 3 seconds;   */
/* typing it again turns it off (CHEAT DISABLED). Cheats stay on     */
/* until switched off or the console is.                              */
/* ------------------------------------------------------------------ */
typedef struct Cheat {
    char *code;                          /* what to type (letters any case) */
    char *name;                          /* shown under CHEAT ENABLED */
    s32 *flag;
} Cheat;
extern s32 gTrophyOriginal;              /* src/race/hud.c (and port/gfx.c's CHECKPOINT banner) */
s32 gHotRods, gAllTracks, gSecretCars, gRealNames;
s32 gPortTwoPlayer = 0;                  /* 2-player games: off until tested (include/frontend.h P2_ALLOWED) */
static Cheat gCheats[] = {
    { "XMAS", "SNOW RACING!", &gSnowOn },
    { "TAKEMEBACK", "ORIGINAL N64 SPRITES", &gTrophyOriginal },  /* the trophy girl and the CHECKPOINT banner */
    { "HOTRODS", "FASTER CARS UNLOCKED", &gHotRods },          /* every faster car on Choose Car (carselect.c) */
    { "MURICA", "ALL TRACKS UNLOCKED", &gAllTracks },            /* every track on Choose Race (raceselect.c) */
    { "APRILSHOWERS", "RAIN RACING!", &gRainOn },                /* rain on every track (port_rain_draw) */
    { "BARNFIND", "SECRET CARS UNLOCKED", &gSecretCars },        /* the Jeep, School Bus, Police Car (carselect.c) */
    { "ENTHUSIAST", "REAL CAR NAMES", &gRealNames },              /* Choose Car's details (carselect.c, tools/port_realnames.py) */
};
#ifndef CHEAT_FONT2
#define CHEAT_FONT2 0
#endif
#define CHEAT_SHOW 90                    /* frames the message stays */
static char gCheatTyped[16];
static s32 gCheatLen, gCheatTimer;
static TextObj *gCheatMsg[2];

/* the message off the screen (and when leaving Choose Race) */
void port_cheats_leave(void) {
    s32 i;
    for (i = 0; i < 2; i++) {
        if (gCheatMsg[i] != NULL) {
            textFree(gCheatMsg[i]);
            gCheatMsg[i] = NULL;
        }
    }
    gCheatTimer = 0;
    gCheatLen = 0;
}

static void cheat_message(char *top, char *name) {
    port_cheats_leave();
    gCheatMsg[0] = textCreate(2, top, 2);                /* the big chrome letters */
    textPlace(&gCheatMsg[0]->xf, 0x8B0000, 0x7C0000, 0, 1);
    gCheatMsg[1] = textCreate(CHEAT_FONT2, name, 2);     /* the race names' yellow letters */
    textPlace(&gCheatMsg[1]->xf, 0x8B0000, 0x680000, 0, 1);
    gCheatTimer = CHEAT_SHOW;
}

/* once a frame on Choose Race (typing: the screen is waiting for a choice; codes are taken in any
 * case). 1: the message is showing (the screen hides its title under it) */
s32 port_cheats_frame(s32 typing) {
    s32 k, i, j;

    while ((k = plat_key()) != 0) {     /* (taken while the screen opens too: typing is never lost) */
        if (k == '\n' || k == '\r') {
            /* the code at the end of what was typed (letters before it don't matter) */
            for (i = 0; i < (s32)(sizeof(gCheats) / sizeof(gCheats[0])); i++) {
                char *c = gCheats[i].code;
                s32 len = 0, at;
                while (c[len] != 0) {
                    len++;
                }
                at = gCheatLen - len;
                for (j = 0; at >= 0 && j < len && c[j] == gCheatTyped[at + j]; j++) {
                }
                if (at >= 0 && j == len) {
                    *gCheats[i].flag = !*gCheats[i].flag;
                    if (*gCheats[i].flag && gCheats[i].flag == &gSnowOn) {
                        gRainOn = 0;             /* snow or rain, not both: the newest wins */
                    }
                    if (*gCheats[i].flag && gCheats[i].flag == &gRainOn) {
                        gSnowOn = 0;
                    }
                    sfxPlay(0x19, 0);
                    cheat_message(*gCheats[i].flag ? "CHEAT ENABLED" : "CHEAT DISABLED", gCheats[i].name);
                    break;
                }
            }
            gCheatLen = 0;
        } else if (k == 8 || k == 127) {
            if (gCheatLen > 0) {
                gCheatLen--;
            }
        } else if (k > ' ' && k < 127) {
            if (k >= 'a' && k <= 'z') {
                k -= 'a' - 'A';
            }
            if (gCheatLen == (s32)sizeof(gCheatTyped)) {      /* keep the last letters typed */
                for (i = 1; i < gCheatLen; i++) {
                    gCheatTyped[i - 1] = gCheatTyped[i];
                }
                gCheatLen--;
            }
            gCheatTyped[gCheatLen++] = k;
        }
    }
    if (gCheatTimer > 0 && --gCheatTimer == 0) {
        port_cheats_leave();
    }
    return gCheatTimer > 0;
}

/* ------------------------------------------------------------------ */
/* The VMU screen (user's design): during a race "DISTANCE" and how   */
/* far through the race player 1 is (0% at the start, 100% at the     */
/* finish); everywhere else "CRUIS'N" / "USA" (a placeholder for the  */
/* user's logo). Only sent when it changes (plat_vmu_lcd).            */
/* ------------------------------------------------------------------ */
#include "road.h"
static const char *const gVmuFont[][8] = {   /* character, then 7 rows */
    { "0", "01110", "10001", "10011", "10101", "11001", "10001", "01110" },
    { "1", "00100", "01100", "00100", "00100", "00100", "00100", "01110" },
    { "2", "01110", "10001", "00001", "00010", "00100", "01000", "11111" },
    { "3", "11110", "00001", "00001", "01110", "00001", "00001", "11110" },
    { "4", "00010", "00110", "01010", "10010", "11111", "00010", "00010" },
    { "5", "11111", "10000", "11110", "00001", "00001", "10001", "01110" },
    { "6", "00110", "01000", "10000", "11110", "10001", "10001", "01110" },
    { "7", "11111", "00001", "00010", "00100", "01000", "01000", "01000" },
    { "8", "01110", "10001", "10001", "01110", "10001", "10001", "01110" },
    { "9", "01110", "10001", "10001", "01111", "00001", "00010", "01100" },
    { "%", "11001", "11010", "00010", "00100", "01000", "01011", "10011" },
    { "A", "01110", "10001", "10001", "11111", "10001", "10001", "10001" },
    { "C", "01110", "10001", "10000", "10000", "10000", "10001", "01110" },
    { "D", "11110", "10001", "10001", "10001", "10001", "10001", "11110" },
    { "E", "11111", "10000", "10000", "11110", "10000", "10000", "11111" },
    { "I", "01110", "00100", "00100", "00100", "00100", "00100", "01110" },
    { "N", "10001", "11001", "10101", "10011", "10001", "10001", "10001" },
    { "R", "11110", "10001", "10001", "11110", "10100", "10010", "10001" },
    { "S", "01111", "10000", "10000", "01110", "00001", "00001", "11110" },
    { "T", "11111", "00100", "00100", "00100", "00100", "00100", "00100" },
    { "U", "10001", "10001", "10001", "10001", "10001", "10001", "01110" },
    { "'", "1", "1", "0", "0", "0", "0", "0" },
};
static u8 gVmuBits[192];

static s32 vmu_len(const char *s) {
    s32 n = 0;
    while (s[n]) n++;
    return n;
}

static const char *const *vmu_glyph(char c) {
    u32 i;
    for (i = 0; i < sizeof(gVmuFont) / sizeof(gVmuFont[0]); i++) {
        if (gVmuFont[i][0][0] == c) {
            return &gVmuFont[i][1];
        }
    }
    return NULL;
}

static s32 vmu_text_width(const char *s, s32 scale) {
    s32 w = 0;
    for (; *s; s++) {
        const char *const *g = vmu_glyph(*s);
        w += ((g ? vmu_len(g[0]) : 3) + 1) * scale;
    }
    return w - scale;
}

static void vmu_text(const char *s, s32 y, s32 scale) {
    s32 x = (48 - vmu_text_width(s, scale)) / 2;
    for (; *s; s++) {
        const char *const *g = vmu_glyph(*s);
        s32 w = g ? vmu_len(g[0]) : 3, r, c, dx, dy;
        for (r = 0; g != NULL && r < 7; r++) {
            for (c = 0; c < w; c++) {
                if (g[r][c] != '1') continue;
                for (dy = 0; dy < scale; dy++) {
                    for (dx = 0; dx < scale; dx++) {
                        s32 px = x + c * scale + dx, py = y + r * scale + dy;
                        if (px >= 0 && px < 48 && py >= 0 && py < 32) {
                            gVmuBits[py * 6 + (px >> 3)] |= 0x80 >> (px & 7);
                        }
                    }
                }
            }
        }
        x += (w + 1) * scale;
    }
}

/* how far through the race player 1 is, 0..1: along the route's path points
 * (by distance, not by count: they aren't evenly spaced) */
static f32 gVmuTotal, gVmuDone;
static s32 gVmuPoint = -1, gVmuEnd;

static f32 vmu_seg(s32 i) {
    f32 dx = gPathPoints[i + 1].x - gPathPoints[i].x, dz = gPathPoints[i + 1].z - gPathPoints[i].z;
    return sqrtf(dx * dx + dz * dz);
}

static f32 vmu_progress(void) {
    Car *car = &gCars[0];
    s32 p = car->road.point, i;
    if (gPathLength < 2) {
        return 0.0f;
    }
    if (p < 0) p = 0;
    if (p > gPathLength - 2) p = gPathLength - 2;
    if (gVmuPoint < 0) {                 /* a new race: the length of the route to the finish line */
        s32 fs = -1, end = gPathLength - 1;
        /* the finish: the first section whose event is "end the race" (0x20 + track:
         * gScriptCmds 0x20-0x2D are raceFinish), crossed on reaching its first path point */
        for (i = 0; gTrack != NULL && i < gTrack->numSections; i++) {
            s32 cmd = gTrack->sections[i].flags & 0x3F;
            if (cmd >= 0x20 && cmd <= 0x2D) {        /* (0x2E.. are other events) */
                fs = i;
                break;
            }
        }
        for (i = 0; fs >= 0 && i < gPathLength; i++) {
            if (gPathPoints[i].section >= fs) {
                end = i;
                break;
            }
        }
        if (end < 1) end = gPathLength - 1;
        gVmuEnd = end;
        gVmuTotal = 0.0f;
        for (i = 0; i < end; i++) {
            gVmuTotal += vmu_seg(i);
        }
        gVmuDone = 0.0f;
        gVmuPoint = 0;
    }
    while (gVmuPoint < p) {              /* (a few points a frame at most) */
        gVmuDone += vmu_seg(gVmuPoint++);
    }
    while (gVmuPoint > p) {
        gVmuDone -= vmu_seg(--gVmuPoint);
    }
    if (gVmuTotal <= 0.0f) {
        return 0.0f;
    }
    return (gVmuDone + car->road.t * vmu_seg(p)) / gVmuTotal;
}

/* the animated logo (port/art/vmu_logo.h): which frame shows now; the game
 * runs at 30 frames a second, the logo's own timing is in ms */
#include "art/vmu_logo.h"
static s32 vmu_logo_frame(void) {
    static u32 ms;
    u32 total = 0, t;
    s32 i;
    for (i = 0; i < VMU_LOGO_FRAMES; i++) {
        total += gVmuLogoMs[i];
    }
    ms = (ms + 33) % (total ? total : 1);   /* (a game frame: 1/30 s) */
    for (i = 0, t = ms; i < VMU_LOGO_FRAMES - 1 && t >= gVmuLogoMs[i]; i++) {
        t -= gVmuLogoMs[i];
    }
    return i;
}

void port_vmu_frame(void) {
    static s32 shown = -2;               /* -1000 - n: the logo's frame n; 0..100: the distance */
    s32 want;
    if (gCurrentOverlay == OVL_RACE) {
        f32 f = vmu_progress();
        want = (gRaceState == 2) ? 100 : (gRaceState == 0) ? 0 : (s32)(f * 100.0f);
        if (want < 0) want = 0;
        if (want > 100) want = 100;
        if (want == 100 && gRaceState != 2) want = 99;   /* (100% when the race is over) */
    } else {
        gVmuPoint = -1;                  /* (the next race starts afresh) */
        want = -1000 - vmu_logo_frame();
    }
    if (want == shown) {
        return;
    }
    shown = want;
    {
        u32 i;
        for (i = 0; i < sizeof(gVmuBits); i++) gVmuBits[i] = 0;
    }
    if (want < 0) {
        plat_vmu_lcd(gVmuLogo[-1000 - want]);
        return;
    } else {
        char num[8];
        s32 n = 0;
        if (want >= 100) num[n++] = '0' + want / 100;
        if (want >= 10) num[n++] = '0' + want / 10 % 10;
        num[n++] = '0' + want % 10;
        num[n++] = '%';
        num[n] = 0;
        vmu_text("DISTANCE", 1, 1);
        vmu_text(num, 12, 2);
    }
    plat_vmu_lcd(gVmuBits);
}
