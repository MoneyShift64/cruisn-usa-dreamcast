/*
 * input.c - controllers: the read threads, read timing, stick calibration,
 * button edges, and the player controls built from them (gas, brake,
 * steering, configurable buttons, cheat-code sequences).
 * Original code: main segment, ROM 0x12460-0x13A90 (RAM 0x80111460-0x80112A90).
 *
 * Two threads (created by the boot overlay) do the reading. A timer started
 * once per frame by contReadSchedule() wakes contTimerThread, which starts a
 * controller read; contThread collects it into gContSnap. The delay of that
 * timer adapts so the read finishes just before the game wants it. Every 30
 * reads the controllers are queried again (plugged in or not).
 * inputUpdate() (main loop, once per frame) turns the snapshot into gPads;
 * controlsUpdate() turns gPads into gControls for the race.
 */
#include "common.h"
#include "frontend.h"

typedef struct ContSnap {                /* 0x10 bytes - last read, per active controller */
    /* 0x00 */ u32 buttons;
    /* 0x04 */ s32 x;
    /* 0x08 */ s32 y;
    /* 0x0C */ s32 fresh;                /* set by the read, cleared by inputUpdate */
} ContSnap;

typedef struct Controls {                /* 0x18 bytes, per player */
    /* 0x00 */ s32 gas;                  /* 0..255 */
    /* 0x04 */ s32 brake;                /* 0..255 */
    /* 0x08 */ f32 steer;                /* -1..1 */
    /* 0x0C */ f32 steerDigital;         /* d-pad steering, ramps at gSteerRates[steerMode] */
    /* 0x10 */ u32 cheats;               /* bits set by entered codes */
    /* 0x14 */ s32 steerMode;            /* 0..4: response curve of the stick */
} Controls;

typedef struct CheatCode {               /* 0x1C bytes; list ends with seq == NULL */
    /* 0x00 */ u32 *seq;                 /* {mask, value} pairs, ends with mask 0 */
    /* 0x04 */ u32 set;                  /* cheat bits set when completed */
    /* 0x08 */ u32 clear;                /* and cleared */
    /* 0x0C */ s32 pos[2];               /* per player: steps matched so far */
    /* 0x14 */ u32 lastFrame[2];         /* frame of the last matched step (9 frames to the next) */
} CheatCode;

typedef struct ControlTarget {           /* where a control configuration is written */
    u32 *a;
    u32 *b;                              /* may be NULL */
} ControlTarget;

typedef struct ControlPair {
    u32 a, b;
} ControlPair;

extern OSContStatus gContStatus[4];      /* 0x8014E210 */
extern OSContPad gContPads[4];           /* 0x8014E220 */
extern s32  gContPorts[2];               /* 0x8014E240 - ports of the active controllers */
extern volatile s32 gContBusy;           /* 0x8014E248 - a read or query is under way */
extern volatile s32 gContHold;           /* 0x8014E24C - don't start reads */
extern f32  gStickDead[2];               /* 0x8014E250 - per player: dead zone around the centre (0..1 units) */
extern f32  gStickCenter[2][2];          /* 0x8014E258 - per player, per axis: calibrated centre */
extern s32  gContQueryCountdown;         /* 0x8014E268 - reads until the next status query */
extern OSTimer gContTimer;               /* 0x8014E298 */
extern u64  gContDelay;                  /* 0x8014E2B8 - timer delay before a read */
extern u64  gContReadStart;              /* 0x8014E2C0 */
extern u64  gContReadEnd;                /* 0x8014E2C8 */
extern ContSnap gContSnap[2];            /* 0x8014E2D0 */
extern s32  gContFound;                  /* 0x8014E2F0 - controllers found by the last query (max 2) */
extern s32  gContFoundPorts[2];          /* 0x8014E2F8 */
extern s32  gContRetries;                /* 0x8014E300 */
extern u32  gPadPressed[2];              /* 0x8014E308 - new presses, until taken with contPressed */
extern u32  gPadReady[2];                /* 0x8014E310 - buttons released since last taken */
extern s32  gPadRepeat[2];               /* 0x8014E318 - auto-repeat countdown of the menu directions */
extern s32  gContTimerSet;               /* 0x8014E320 */
extern u32  gGasMask[2];                 /* 0x8014E330 - per player configuration (controlsSetMapping) */
extern s32  gGasAxis[2];                 /* 0x8014E338 - -1 / 1: stick down / up, else gGasMask button */
extern u32  gBrakeMask[2];               /* 0x8014E340 */
extern s32  gBrakeAxis[2];               /* 0x8014E348 */
extern u32  gBtnShiftUp[2];              /* 0x8014E350 */
extern u32  gBtnShiftDown[2];            /* 0x8014E358 */
extern u32  gBtnLights[2];               /* 0x8014E360 */
extern u32  gBtnMusic[2];                /* 0x8014E368 */
extern u32  gBtnView[2];                 /* 0x8014E370 */
extern Controls gControls[2];            /* 0x8014E378 */
extern OSMesgQueue gContTimerQueue;      /* 0x80146640 */
extern OSMesgQueue gMainMesgQueue;       /* 0x80146620 - SI events */
extern s32  gNumPlayersM1;               /* 0x8015095C */
extern u32  gFrameCounter;               /* 0x80147698 */
extern s32  gStickRange[2];              /* 0x80132930 - raw stick range per axis (80) */
extern u32  gStickRemap[4][2];           /* 0x80132938 - stick direction -> menu direction bits */
extern f32  gSteerRates[5];              /* 0x80132960 - d-pad steering ramp per steer mode */
extern CheatCode gCheatCodes[];          /* 0x80132A14 */
extern ControlTarget gControlTargets[7]; /* 0x80132A84 */
extern ControlPair gControlConfigs[][7]; /* 0x80132ABC */
extern u64  osClockRate;                 /* 0x8013A430 */

s32  disableInterrupts(void);            /* 0x801156E0 */
void restoreInterrupts(s32 mask);        /* 0x80115700 */
OSTime osGetTime(void);                  /* 0x80122EE0 */
s32  osContStartReadData(OSMesgQueue *mq); /* 0x80124420 */
void osContGetReadData(OSContPad *pad);  /* 0x801244E4 */

void contFindPorts(void);                /* 0x801114A8 */
void contQueryDone(void);                /* 0x80111F34 */
void contReadDone(void);                 /* 0x80111FF8 */
void contReadTime(void);                 /* 0x801121CC */
s32  controlsButtons(s32 player);        /* 0x801128A4 */
s32  controlsAxis(s32 player, s32 axis, u32 mask, s32 scale); /* 0x801129A0 */

#define USEC(n) (((u64)(n) * osClockRate) / 1000000)

/* 0x80111460 - returns the new presses, then takes 'mask' of them (a taken
 * button counts again only after being released) */
u32 contPressed(s32 player, u32 mask) {
    u32 pressed = gPadPressed[player];

    gPadReady[player] &= ~(mask & pressed);
    gPadPressed[player] = pressed & ~mask;
    return pressed;
}

/* 0x801114A8 - the first two controllers that answered the status query */
void contFindPorts(void) {
    s32 mask = disableInterrupts();
    s32 i;

    gContFound = 0;
    for (i = 0; i < 4; i++) {
        if (gContStatus[i].errno == 0) {
            gContFoundPorts[gContFound] = i;
            if (++gContFound == 2) {
                break;
            }
        }
    }
    restoreInterrupts(mask);
}

/* 0x80111530 - once per frame: take the last read, calibrate the sticks
 * (0..1 with a dead zone; stick directions as buttons), auto-repeat the menu
 * directions and record new presses */
void inputUpdate(void) {
    s32 fresh[2];
    u32 buttons[2];
    s32 raw[2][2];
    f32 cal[2][2];
    f32 norm[2][2];
    s32 i, k, n, mask, range, v;
    f32 dead, center, lo;
    u32 b;
    PadState *pad;

    mask = disableInterrupts();
    gNumControllers = gContFound;
    for (i = 0; i < 2; i++) {
        gContPorts[i] = gContFoundPorts[i];
    }
    for (i = 0; i < gNumControllers; i++) {
        if (gContSnap[i].fresh) {
            buttons[i] = gContSnap[i].buttons;
            gContSnap[i].fresh = 0;
            fresh[i] = 1;
            raw[i][0] = gContSnap[i].x;
            raw[i][1] = gContSnap[i].y;
        } else {
            fresh[i] = 0;
        }
    }
    restoreInterrupts(mask);

    n = gNumControllers;
    for (i = 0; i < n; i++) {
        if (!fresh[i]) {
            continue;
        }
        dead = gStickDead[i];
        for (k = 0; k < 2; k++) {
            range = gStickRange[k];
            v = raw[i][k];
            if (v < -range) {
                raw[i][k] = v = -range;
            } else if (range < v) {
                raw[i][k] = v = range;
            }
            center = gStickCenter[i][k];
            lo = center - dead;
            norm[i][k] = (f32)(v + range) / (f32)(range * 2);
            if (norm[i][k] < lo) {
                cal[i][k] = (f64)norm[i][k] * 0.5 / (f64)lo;
            } else if (center + dead < norm[i][k]) {
                cal[i][k] = (f64)(norm[i][k] - (1.0f - lo)) * 0.5 / (f64)lo + 0.5;
            } else {
                cal[i][k] = 0.5f;
            }
        }
        if (norm[i][0] <= 0.25) {
            buttons[i] |= 0x40000;
        } else if (norm[i][0] >= 0.75) {
            buttons[i] |= 0x80000;
        }
        if (norm[i][1] <= 0.25) {
            buttons[i] |= 0x20000;
        } else if (norm[i][1] >= 0.75) {
            buttons[i] |= 0x10000;
        }
    }

    for (i = 0; i < n; i++) {
        if (!fresh[i]) {
            continue;
        }
        b = buttons[i] & 0xFF0FFFFF;
        buttons[i] = b;
        for (k = 0; k < 4; k++) {
            if (gStickRemap[k][0] & b) {
                buttons[i] = b |= gStickRemap[k][1];
            }
        }
        if (b & 0xF00000) {
            if (gPadRepeat[i] != 0) {
                if (--gPadRepeat[i] == 0) {
                    gPadRepeat[i] = 8;
                } else {
                    buttons[i] = b & 0xFF0FFFFF;
                }
            } else {
                gPadRepeat[i] = 12;
            }
        } else {
            gPadRepeat[i] = 0;
        }
    }

    mask = disableInterrupts();
    for (i = 0; i < gNumControllers; i++) {
        if (fresh[i]) {
            b = buttons[i];
            gPadPressed[i] |= gPadReady[i] & b;
            gPadReady[i] |= ~b;
            pad = &gPads[i];
            pad->held = b;
            pad->x = cal[i][0];
            pad->y = cal[i][1];
            pad->xRaw = norm[i][0];
            pad->yRaw = norm[i][1];
        }
    }
    restoreInterrupts(mask);
}

/* 0x80111AAC - stop reads (e.g. while the main loop needs the SI) */
void contReadBegin(void) {
    gContHold = 1;
    while (gContBusy != 0) {
    }
}

/* 0x80111AD8 - allow them again and wait for one full read */
void contReadEnd(void) {
    gContHold = 0;
    while (gContBusy == 0) {
    }
    while (gContBusy != 0) {
    }
}

/* 0x80111B28 - once per frame (retrace): adapt the read delay and arm the
 * timer that starts the next read */
void contReadSchedule(void) {
    s32 mask = disableInterrupts();
    OSTime now = osGetTime();
    u64 t;

    if (gContReadEnd != 0) {
        now -= gContReadEnd;
        if (now > USEC(4000)) {
            now -= USEC(2000);
            t = gContDelay + now;
            if (t > USEC(29333)) {
                gContDelay = USEC(29333);
            } else {
                gContDelay = t;
            }
        }
    } else if (gContBusy != 0 && gContReadStart == 0) {
        gContReadStart = osGetTime();
    }
    restoreInterrupts(mask);
    if (gContTimerSet == 0 && gContHold == 0) {
        gContTimerSet = 1;
        osSetTimer(&gContTimer, gContDelay, 0, &gContTimerQueue, NULL);
    }
}

/* 0x80111DCC - thread: start a read when the timer fires */
void contTimerThread(void *arg) {
    for (;;) {
        osRecvMesg(&gContTimerQueue, NULL, OS_MESG_BLOCK);
        gContTimerSet = 0;
        if (gContBusy == 0 && gContHold == 0) {
            gContBusy = 1;
            gContReadStart = 0;
            gContReadEnd = 0;
            osContStartReadData(&gMainMesgQueue);
        }
    }
}

/* 0x80111EC0 - thread: a query or read has completed */
void contThread(void *arg) {
    for (;;) {
        osRecvMesg(&gMainMesgQueue, NULL, OS_MESG_BLOCK);
        if (gContQueryCountdown == 0) {
            contQueryDone();
        } else {
            contReadDone();
        }
    }
}

/* 0x80111F34 */
void contQueryDone(void) {
    s32 retry = 0;
    OSContStatus *s;

    osContGetQuery(gContStatus);
    for (s = gContStatus; s < &gContStatus[4]; s++) {
        if (s->errno == CONT_OVERRUN_ERROR) {
            retry = 1;
            break;
        }
    }
    if (retry) {
        if (--gContRetries != 0) {
            if (gContRetries < 0) {
                gContRetries = 3;
            }
            osContStartQuery(&gMainMesgQueue);
        } else {
            retry = 0;
        }
    }
    if (!retry) {
        gContQueryCountdown = 30;
        gContBusy = 0;
        contFindPorts();
    }
}

/* 0x80111FF8 */
void contReadDone(void) {
    s32 errors = 0;
    s32 n, i, mask;
    OSContPad *pad;

    osContGetReadData(gContPads);
    n = gNumControllers;
    for (i = 0; i < n; i++) {
        pad = &gContPads[gContPorts[i]];
        if (pad->errno != 0) {
            if (pad->errno == CONT_NO_RESPONSE_ERROR) {
                mask = disableInterrupts();
                gContSnap[i].buttons = 0;
                gContSnap[i].x = 0;
                gContSnap[i].y = 0;
                gContSnap[i].fresh = 1;
                restoreInterrupts(mask);
            }
            errors++;
        } else {
            mask = disableInterrupts();
            gContSnap[i].buttons = pad->button;
            gContSnap[i].x = pad->stick_x;
            gContSnap[i].y = pad->stick_y;
            gContSnap[i].fresh = 1;
            restoreInterrupts(mask);
        }
    }
    if (errors) {
        if (--gContRetries != 0) {
            if (gContRetries < 0) {
                gContRetries = 3;
            }
            osContStartReadData(&gMainMesgQueue);
        } else {
            errors = 0;
        }
    } else {
        contReadTime();
    }
    if (!errors) {
        gContRetries = 0;
        if (--gContQueryCountdown == 0) {
            osContStartQuery(&gMainMesgQueue);
            return;
        }
        gContBusy = 0;
    }
}

/* 0x801121CC - a read finished: if the frame had already asked for input,
 * start reads earlier next time (not below 10 ms) */
void contReadTime(void) {
    s32 mask = disableInterrupts();
    OSTime now = osGetTime();
    u64 t;

    if (gContReadStart != 0) {
        now = now - gContReadStart + USEC(2000);
        t = gContDelay - now;
        if (t < USEC(10000)) {
            gContDelay = USEC(10000);
        } else {
            gContDelay = t;
        }
    } else {
        gContReadEnd = now;
    }
    restoreInterrupts(mask);
}

/* 0x80112370 - apply a control configuration to a player; resets the
 * cheat-code trackers and d-pad steering */
void controlsSetMapping(s32 player, s32 config) {
    ControlTarget *t;
    ControlPair *c = gControlConfigs[config];
    CheatCode *cc;

    for (t = gControlTargets; t < &gControlTargets[7]; t++, c++) {
        t->a[player] = c->a;
        if (t->b != NULL) {
            t->b[player] = c->b;
        }
    }
    for (cc = gCheatCodes; cc->seq != NULL; cc++) {
        cc->pos[1] = 0;
        cc->pos[0] = 0;
        cc->lastFrame[1] = gFrameCounter;
        cc->lastFrame[0] = gFrameCounter;
    }
    gControls[1].steerDigital = 0.0f;
    gControls[0].steerDigital = gControls[1].steerDigital;
}

/* 0x8011242C - per player: gas, brake, steering (stick through the
 * steer mode's curve plus d-pad ramp), and the cheat-code sequences */
void controlsUpdate(void) {
    s32 p;
    Controls *c;
    PadState *pad;
    CheatCode *cc;
    u32 *step;
    u32 bits;
    f32 rate, v, steer;
    f32 curve;

    for (p = 0; p <= gNumPlayersM1; p++) {
        c = &gControls[p];
        pad = &gPads[p];
        c->gas = controlsAxis(p, gGasAxis[p], gGasMask[p], 255);
        c->brake = controlsAxis(p, gBrakeAxis[p], gBrakeMask[p], 255);
        rate = gSteerRates[c->steerMode];
        if (pad->held & 0x200) {
            if (0.0f < c->steerDigital) {
                c->steerDigital = 0.0f;
            }
            c->steerDigital -= rate;
            if (c->steerDigital < -1.0f) {
                c->steerDigital = -1.0f;
            }
        } else if (pad->held & 0x100) {
            if (c->steerDigital < 0.0f) {
                c->steerDigital = 0.0f;
            }
            c->steerDigital += rate;
            if (1.0f < c->steerDigital) {
                c->steerDigital = 1.0f;
            }
        } else if (0.0f < c->steerDigital) {
            c->steerDigital -= rate;
            if (c->steerDigital < 0.0f) {
                c->steerDigital = 0.0f;
            }
        } else if (c->steerDigital < 0.0f) {
            c->steerDigital += rate;
            if (0.0f < c->steerDigital) {
                c->steerDigital = 0.0f;
            }
        }
        v = (pad->xRaw - 0.5) * 2;
        switch (c->steerMode) {
            case 0:
                curve = v * v * v;
                break;
            case 1:
                curve = ((v < 0.0f) ? -v : v) * v;
                break;
            case 2:
                curve = (v * v * v + v) * 0.5f;
                break;
            case 3:
                curve = (((v < 0.0f) ? -v : v) * v + v) * 0.5f;
                break;
            case 4:
                curve = v;
                break;
        }
        steer = c->steerDigital + curve;
        c->steer = steer;
        if (steer < -1.0f) {
            c->steer = -1.0f;
        } else if (1.0f < c->steer) {
            c->steer = 1.0f;
        }
        bits = controlsButtons(p);
        for (cc = gCheatCodes; cc->seq != NULL; cc++) {
            if (gFrameCounter - cc->lastFrame[p] >= 9) {
                cc->pos[p] = 0;
                cc->lastFrame[p] = gFrameCounter;
            }
            step = &cc->seq[cc->pos[p] * 2];
            if ((step[0] & bits) == step[1]) {
                cc->lastFrame[p] = gFrameCounter;
                cc->pos[p]++;
                if (step[2] == 0) {
                    cc->pos[p] = 0;
                    c->cheats |= cc->set;
                    c->cheats &= ~cc->clear;
                }
            } else if (cc->pos[p] != 0 && (step[-2] & bits) != step[-1]) {
                cc->pos[p] = 0;
            }
        }
    }
}

/* 0x801128A4 - the configured buttons held, as bits 1..0x10, plus gas (0x20)
 * and brake (0x40) more than half on */
s32 controlsButtons(s32 player) {
    u32 held = gPads[player].held;
    s32 bits = 0;

    if (gBtnShiftUp[player] & held) {
        bits = 1;
    }
    if (gBtnShiftDown[player] & held) {
        bits |= 2;
    }
    if (gBtnLights[player] & held) {
        bits |= 4;
    }
    if (gBtnMusic[player] & held) {
        bits |= 8;
    }
    if (gBtnView[player] & held) {
        bits |= 0x10;
    }
    if (gControls[player].gas >= 0x80) {
        bits |= 0x20;
    }
    if (gControls[player].brake >= 0x80) {
        bits |= 0x40;
    }
    return bits;
}

/* 0x801129A0 - an analog control 0..scale: stick down (-1) / up (1), or a button */
s32 controlsAxis(s32 player, s32 axis, u32 mask, s32 scale) {
    PadState *pad = &gPads[player];
    f32 y = pad->y;

    if (axis == -1) {
        if (y < 0.5) {
            return 2.0 * (0.5 - y) * scale;
        }
        return 0;
    }
    if (axis == 1) {
        if (y > 0.5) {
            return 2.0 * (y - 0.5) * scale;
        }
        return 0;
    }
    return ((pad->held & mask) != 0) * scale;
}
