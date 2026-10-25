/*
 * gamesetup.c - the game set-up the front end hands to the race: a new game,
 * per-player view and camera choice, and which opponents the options allow.
 * Original code: main segment, ROM 0x18AD0-0x18D00 (RAM 0x80117AD0-0x80117D00).
 */
#include "frontend.h"

typedef struct CamView { f32 dist, height; } CamView;
extern CamView gCamViews[2][3];          /* 0x80133B5C - per player count: in-car, chase, far chase */
extern s32 gRandomRaceHistory[8];        /* 0x801509A0 */
extern s32 gSegment3Base;                /* 0x80146594 */

/* 0x80117AD0 - new game: Cruise the USA, one player, no saves chosen */
void gameSetupReset(void) {
    s32 i;

    gTrackChoice = 14;
    gSecretRace = 0;
    gNumPlayersM1 = 0;
    gCruiseMode = 0;
    gCruiseContinue = 0;
    gSegment3Base = 0;
    for (i = 0; i < 2; i++) {
        gPlayerSetup[i].savePak = i;
        gPlayerSetup[i].saveSlot = 6;
    }
    for (i = 0; i < 8; i++) {
        gRandomRaceHistory[i] = -1;
    }
    contPressed(0, -1);
    contPressed(1, -1);
}

/* 0x80117B88 - a player's settings back to the defaults: chase view */
void playerSetupReset(s32 player) {
    PlayerSetup *ps = &gPlayerSetup[player];

    ps->cameraFollowsCar = 1;
    ps->unk390 = -1;
    playerSetupView(player);
    ps->unk01A = 3;
    ps->unk030 = 0;
    ps->initials[0] = -1;                /* no initials entered */
    ps->totalTime = 0.0f;
}

/* 0x80117C00 - put the camera straight at the chosen view (for the player count) */
void playerSetupView(s32 player) {
    PlayerSetup *ps = &gPlayerSetup[player];
    CamView *v;

    ps->camViewShown = ps->cameraFollowsCar;
    v = &gCamViews[gNumPlayersM1][ps->camViewShown];
    ps->camDist = v->dist;
    ps->camDistTarget = v->dist;
    ps->camDistStep = 0.0f;
    ps->camHeightStep = 0.0f;
    ps->camHeightTarget = v->height;
    ps->camHeight = v->height;
    ps->viewAtEdge = (ps->camDist == 0.0) ? 1 : 0;
    ps->camOverride = 0;
}

/* 0x80117CAC - opponents for this race: Cruise the USA always has both */
void raceOptionsApply(void) {
    if (gCruiseMode != 0) {
        gRacersEnabled = 1;
        gTrafficEnabled = 1;
        return;
    }
    gRacersEnabled = gOptRacers;
    gTrafficEnabled = gOptTraffic;
}
