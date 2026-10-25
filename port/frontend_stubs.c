/*
 * frontend_stubs.c - stand-ins for the front-end screens that are not
 * converted yet, so the converted
 * ones (title, Choose Race, results) run as a complete loop:
 *     title -> Choose Race -> car select -> race -> results -> ...
 * Each one goes where its screen would end up. Replace them as the real ones
 * are converted.
 */
#include "common.h"
#include "frontend.h"
#include "race.h"

extern s32 gTitleVariant;                /* 0x800466D0 ovl3 */

void titleInit(s32 skip);
void frontEndEnter(s32 showIntro);
void raceSelectInit(s32 fromRace);
void resultsInit(s32 arg);
void raceStart(s32 arg);
void trackPrepare(void);
void playerSetupView(s32 player);

/* the race overlay's start (RACE_START_MODE) and the two front-end entries the
 * race code switches to, under the names the generated tables use */
void func_8002F600_ovl2(s32 arg) {
    raceStart(arg);
}
void func_80026C98_ovl3(s32 arg) {
    resultsInit(arg);
}
void func_80026030_ovl3(s32 arg) {
    frontEndEnter(arg);
}
