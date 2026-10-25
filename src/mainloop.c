/*
 * mainloop.c - the game's main loop (runs forever on the main thread).
 * Original code: ROM 0x121E0-0x12460 (RAM 0x801111E0-0x80111460).
 *
 * Game modes: the current mode is a function pointer (gModeFunc) called once
 * per frame. To change mode, code stores the new function plus an overlay
 * request (0x8000 | overlay id) in gOverlayRequest; the loop waits for the
 * graphics pipeline to drain, swaps the overlay, and from the next frame calls
 * the new mode function (which lives in the newly loaded overlay).
 */
#include "common.h"

#define OVERLAY_NONE    0x7FFF
#define OVERLAY_PENDING 0x8000

typedef void (*ModeFunc)(s32 arg);

extern ModeFunc gModeFunc;               /* 0x8014C690 */
extern s32      gModeArg;                /* 0x8014C694 */
extern s32      gOverlayRequest;         /* 0x8014C698 */
extern s32      gOverlayCurrent;         /* 0x8014C69C */
extern s32      gNoPlayerInput;          /* 0x8014C6A0 - set during attract/demo; freezes RNG, skips START checks */
extern s32      gFrameUserArg;           /* 0x8014C6A4 */
extern s32      gPreNmi;                 /* 0x80147680 */
extern u32     *gDlCursor;               /* 0x80147684 */
extern u32      gSegment3Base;           /* 0x80146594 */
extern Gfx      gInitDisplayList[];      /* 0x80133910 */

u32  randNext(void);                     /* 0x801030E0 */
void dmaRunCallbacks(void);              /* 0x80111184 */
void musicUpdate(void);                  /* 0x8011F780 */
s32  gfxWaitIdle(void);                  /* 0x80103D88 */
void overlayLoad(s32 id);                /* 0x80102CA0 */
void overlayUnload(s32 id);              /* 0x80102CE8 */
s32  gfxBeginFrame(void);                /* 0x801039E0 */
void gfxEndFrame(s32 userArg);           /* 0x80103B10 */
void inputUpdate(void);                  /* 0x80111530 */
void controlsUpdate(void);               /* 0x8011242C */
void drawLowMemLayer(void);              /* 0x801158EC */
void drawSpriteLayers(void);             /* 0x8011A934 */
void animUpdate(void);                   /* 0x801131C8 */

#define DL_PUSH(w0, w1) \
    { u32 *_dl = gDlCursor; gDlCursor = _dl + 2; _dl[1] = (w1); _dl[0] = (w0); }

/* 0x801111E0 */
void gameMainLoop(void) {
    s32 next;
    u32 *dl;

    while (TRUE) {
        while (gPreNmi != 0) {
            /* reset pressed: stop here */
        }
        if (!gNoPlayerInput) {
            randNext();
        }
        dmaRunCallbacks();
        musicUpdate();

        if (gOverlayCurrent != gOverlayRequest) {
            gfxWaitIdle();
            if (gOverlayCurrent != OVERLAY_NONE) {
                overlayUnload(gOverlayCurrent);
            }
            next = gOverlayRequest & ~OVERLAY_PENDING;
            gOverlayRequest = next;
            gOverlayCurrent = next;
            if (next != OVERLAY_NONE) {
                overlayLoad(next);
            }
        }

        if (gfxBeginFrame() != 0) {
            continue;                    /* no free slot yet (30 fps cap) */
        }

        inputUpdate();
        controlsUpdate();
        DL_PUSH(0xBC000806, 0);                      /* segment 2: framebuffer (patched later) */
        DL_PUSH(0xBC000006, 0);                      /* segment 0: physical RAM */
        DL_PUSH(0xBC000406, 0x100000);               /* segment 1: main code/data */
        DL_PUSH(0xBC000C06, gSegment3Base << 9);     /* segment 3 */
        dl = gDlCursor;
        gDlCursor = dl + 2;
        dl[0] = 0x06000000;                          /* gSPDisplayList */
        dl[1] = osVirtualToPhysical(gInitDisplayList);
        drawLowMemLayer();

        gFrameUserArg = 0;
        gModeFunc(gModeArg);                         /* run the current game mode */

        if (!gNoPlayerInput) {
            drawSpriteLayers();
        }
        DL_PUSH(0xE9000000, 0);                      /* gDPFullSync */
        DL_PUSH(0xB8000000, 0);                      /* gSPEndDisplayList */
        gfxEndFrame(gFrameUserArg);

        if (!gNoPlayerInput) {
            animUpdate();
        }
    }
}

/* 0x80111450 - empty function */
void func_80111450(s32 arg0) {
}
