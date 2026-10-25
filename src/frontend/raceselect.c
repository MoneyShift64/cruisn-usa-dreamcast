/*
 * frontend/raceselect.c - CHOOSE RACE: the USA map with the 14 tracks, Cruise
 * the USA, and the random secret race; the "continue saved Cruise the USA?"
 * question; secret tracks picked by holding buttons.
 * Original code: overlay 3, ROM 0x626B0-0x64420 (RAM 0x80029920-0x8002B690).
 *
 * States (gRsState):
 *   0 intro animation running        1 (unused while intro runs)
 *   2 2-player challenge prompt     3 choosing: d-pad moves the cursor
 *   4 "CONTINUE <track>?" yes/no    5 choice made: outro started
 *   6/7 outro finished: start the race or go to car select
 *
 * The map is a graph of 16 cursor positions (RsNode, 0x80041338): each names
 * an entry of gRsEntries and the node reached by up/down/left/right. Each entry
 * (RsEntry, 0x800411E8) has a normal race and a SECRET race selected instead
 * while the player holds a button combination (B + C buttons + L/R, see
 * rsUpdateRaces).
 */
#include "common.h"
#include "frontend.h"

/* One cursor position on the map (0x14 bytes) */
typedef struct RsNode {
    /* 0x00 */ s32 entry;                /* index into gRsEntries */
    /* 0x04 */ s32 up, down, left, right; /* next node for each direction */
} RsNode;

/* One map spot (0x1C bytes) */
typedef struct RsEntry {
    /* 0x00 */ s32 race;                 /* set every frame: races[0] or races[1] (secret) */
    /* 0x04 */ s32 races[2];             /* normal race, secret race (0-13; 14 = Cruise the USA, 15 = random) */
    /* 0x0C */ s32 combo;                /* buttons (B, C, L, R) to hold for the secret race; 0 = none */
    /* 0x10 */ s32 xform;                /* index into gRsXf (its marker) */
    /* 0x14 */ s32 nameX, nameY;         /* where the race name is shown (16.16) */
} RsEntry;

/* A race-name caption; two alternate so one can scroll out as the next scrolls in (0xC bytes) */
typedef struct RsName {
    /* 0x00 */ TextObj *text;
    /* 0x04 */ s32 anim;                 /* running animation slot, -1 = none */
    /* 0x08 */ s32 race;
} RsName;

extern RsEntry gRsEntries[];             /* 0x800411E8 ovl3 (to 0x80041300: 10 entries) */
extern RsNode  gRsNodes[];               /* 0x80041338 ovl3 */
extern s32     gRsTrackNode[];           /* 0x80041478 ovl3 - node of each track (start position) */
extern AnimCmd *gRsNameScripts[];         /* 0x8004117C ovl3 - per entry: name scrolls in */
extern s32     gRsRaceDL[];              /* 0x800411AC ovl3 - per race: index of its picture in gRsDLs */
extern AnimCmd *gRsOutroScripts[];        /* 0x80040CDC ovl3 - per entry */
extern XformCmd gRsXfScript[];            /* 0x800402D0 ovl3 - initial layout */
extern AnimCmd gRsIntroScript[];         /* 0x800402F0 ovl3 */
extern AnimCmd gRsOutroScript[];         /* 0x800406C4 ovl3 */
extern AnimCmd gRsFadeScript[];          /* 0x80040D08 ovl3 */
extern AnimCmd gRsNameOutScript[];       /* 0x80040D44 ovl3 */
extern AnimCmd gRsNameOutScript2[];      /* 0x80040D80 ovl3 - for the random race */
extern char    gRsTitle[];               /* 0x800452E0 ovl3 */
extern char    gRsUnknownName[];         /* 0x800452EC ovl3 */
extern char    gRsContinueTitle[];       /* 0x800452F8 ovl3 */
extern char    gRsNewGame[];             /* 0x8004530C ovl3 */
extern char    gRsContinueFmt[];         /* 0x80045320 ovl3 - "CONTINUE %s" style */
extern char    gRsContinue[];            /* 0x80045334 ovl3 */
extern f64     gRsDelayFromRace;         /* 0x80045340 ovl3 */
extern f32     gRsNameTime;              /* 0x80045348 ovl3 */
extern char   *gRaceNames[];             /* 0x80133D54 */
extern char   *gTrackNames[];            /* 0x80133C54 */
extern s32     gRandomRaceHistory[8];    /* 0x801509A0 - last secret races, -1 = empty */
extern f32     gAiSkill[2];              /* 0x801460D0 */
extern u32    *gDlCursor;                /* 0x80147684 */

/* Choose Race state (ovl3 bss) */
extern Xform    gRsXf[15];               /* 0x80046FD0 ovl3 - 11 cursor, 13 name, 14 race picture */
extern void    *gRsDLs[];                /* 0x800471B0 ovl3 - [13..16] cursor frames, [17..19] banners, [20] background, [21] */
extern u32      gRsHeapMark;             /* 0x80047208 ovl3 */
extern s32      gRsFromRace;             /* 0x8004720C ovl3 - came back from a race: go straight to it */
extern s32      gRsState;                /* 0x80047210 ovl3 */
extern s32      gRsNode;                 /* 0x80047214 ovl3 */
extern s32      gRsRace;                 /* 0x80047218 ovl3 */
extern s32      gRsUnlocked;             /* 0x8004721C ovl3 - bit per track */
extern s32      gRsEntry;                /* 0x80047220 ovl3 */
extern s32      gRsShownRace;            /* 0x80047224 ovl3 */
extern s32      gRsBlinkFrame;           /* 0x8004722C ovl3 */
extern s32      gRsBlinkTimer;           /* 0x80047230 ovl3 */
extern void    *gRsBoxDL;                /* 0x80047234 ovl3 */
extern s32      gRsResume;               /* 0x80047238 ovl3 - 0 no save, 1 can continue, 2 players' saves differ */
extern s32      gRsResumeChosen;         /* 0x8004723C ovl3 */
extern s32      gRsResumeTrack;          /* 0x80047240 ovl3 */
extern s32      gRsYes;                  /* 0x80047244 ovl3 - continue? 1 yes, 0 new game */
extern s32      gRsPulse;                /* 0x80047248 ovl3 */
extern TextObj *gRsTitleText;            /* 0x8004724C ovl3 */
extern RsName   gRsNames[2];             /* 0x80047250 ovl3 */
extern s32      gRsNameIdx;              /* 0x80047268 ovl3 */

void p2JoinInit(void);                   /* 0x80025C80 ovl3 */
void p2JoinPressed(void);                /* 0x80025F10 ovl3 */
void p2JoinFree(void);                   /* 0x80025FD4 ovl3 */
void drawDLSimple(void *dl, s32 x, s32 y); /* 0x80026534 ovl3 */
void continueInit(s32 arg);              /* 0x8002EFC0 ovl3 */
s32  continueFrame(void);                /* 0x8002F27C ovl3 - 0 no, 1 yes, else still asking */
void frontEndEnter(s32 showTitle);       /* 0x80026030 ovl3 */
void saveMenuInit(s32 players);          /* 0x80035760 ovl3 */
void animCancelAll(void);                /* 0x80113528 */
void animBlinkInit(void);            /* 0x80113CA0 */
void textFreeAll(void);                  /* 0x8011A9D4 */
void xformsUpdate(Xform *xf);            /* 0x8011508C */
void carSetPalette(s32 slot, s32 car, s32 paint); /* 0x80117F80 - colour slot = player */
s32  randRange(s32 lo, s32 hi);          /* 0x80103188 */
s32  tblCos(s32 angle);                  /* 0x80113EDC */
extern s32 gP2Joined;                    /* 0x8004637C ovl3 */

void rsFrameMain(void);
s32  rsIntroDone(s32 slot, s32 b, f32 c);
s32  rsOutroDone(s32 slot, s32 b, f32 c);
s32  rsNameGone(s32 slot, s32 b, f32 c);
f32  rsIntroStart(s32 arg);
void rsChoose(void);
void rsLoadSave(s32 player);
void rsPickRandom(s32 allowed);
void rsUpdateRaces(void);
void rsFree(void);
s32  rsContinueMenu(void);

#define GDP_PIPESYNC() { u32 *g_ = gDlCursor; gDlCursor += 2; g_[1] = 0; g_[0] = 0xE7000000; }
#define GDP_PRIMCOLOR(c) { u32 *g_ = gDlCursor; gDlCursor += 2; g_[1] = (c); g_[0] = 0xFA000000; }

/* 0x80029920 ovl3 - enter Choose Race (fromRace: back from a race, skip car select) */
void raceSelectInit(s32 fromRace) {
    Xform *xf;
    RsName *n;
    s32 p;

    gRsFromRace = fromRace;
    gRsHeapMark = lowMemOverlayLoad();
    assetLoadGroup(0x2D19, gRsDLs);
    gRsBoxDL = assetLoad(0x2D6C);
    lowMemOverlayRelease();
    for (xf = gRsXf; xf < (Xform *)gRsDLs; xf++) {
        textPlace(xf, 0, 0, 0, 1);
    }
    for (p = 0; p < 2; p++) {
        contPressed(p, BTN_ALL);
    }
    xformScriptRun(gRsXfScript, gRsXf);
    if (gRsFromRace) {
        p2JoinInit();
    }
    for (n = gRsNames; n != &gRsNames[2]; n++) {
        n->anim = -1;
        n->text = NULL;
    }
    gRsNameIdx = 0;
    gRsState = 0;
    rsUpdateRaces();
    gRsTitleText = textCreate(1, gRsTitle, 2);
    textPlace(&gRsTitleText->xf, 0x8B0000, 0x6B0000, 0, 0);
    gRsBlinkFrame = 0;
    gRsBlinkTimer = 3;
    if (gSecretRace != 0) {
        gRsNode = 9;
    } else {
        gRsNode = gRsTrackNode[gTrackChoice];
    }
    gRsEntry = gRsNodes[gRsNode].entry;
    gRsRace = gRsEntries[gRsEntry].race;
    gRsShownRace = -1;
    animWait(fromRace ? (f32)gRsDelayFromRace : (f32)1.5, NULL, (AnimDoneFunc)rsIntroStart);
    gModeFunc = raceSelectFrame;
    raceSelectFrame(0);
}

/* 0x80029B74 ovl3 - intro finished: start choosing (or ask the 2-player CONTINUE?) */
s32 rsIntroDone(s32 slot, s32 b, f32 c) {
    s32 p;

    for (p = 0; p <= gNumPlayersM1; p++) {
        contPressed(p, BTN_ALL);
    }
    gRsState = gAnimSlots[slot].user;
    if (gRsState == 2) {
        if (gRsFromRace == 0 || gRaceWinner >= 0) {
            gRsState = 3;
        } else {
            continueInit(0);
        }
    }
    return 2;
}

/* 0x80029C40 ovl3 - outro finished: fade, then state 6 */
s32 rsOutroDone(s32 slot, s32 b, f32 c) {
    gAnimSlots[animStart(0.25f, gRsFadeScript, NULL, rsIntroDone, 0)].user = 6;
    return 2;
}

/* 0x80029CA8 ovl3 - a race name finished scrolling */
s32 rsNameGone(s32 slot, s32 b, f32 c) {
    gRsNames[gAnimSlots[slot].user].anim = -1;
    return 2;
}

/* 0x80029CF0 ovl3 - start the intro animation */
f32 rsIntroStart(s32 arg) {
    gAnimSlots[animStart(1.0f, gRsIntroScript, NULL, rsIntroDone, 0)].user = 2;
    sfxPlay(0x90, -1);
    gRsState = 1;
    return -1.0f;
}

/* 0x80029D6C ovl3 - Choose Race, every frame */
void raceSelectFrame(s32 arg) {
    if (optionsCheck(raceSelectFrame, 1) == 0) {
        rsFrameMain();
    }
}

/* 0x80029DA8 ovl3 */
void rsFrameMain(void) {
    RsEntry *e;
    RsName *n;
    PlayerSetup *ps;
    SaveSlot *slot;
    Xform *xf;
    s32 prev;
    s32 btn;
    s32 cur;
    s32 p;
    s32 count;
    s32 t;
    s32 dl;
    s32 r;
    char *name;
    void *script;
    s32 track[2];
    s32 diff[2];

#ifdef PORT
    {
        s32 port_cheats_frame(s32 typing);
        if (port_cheats_frame(gRsState == 3)) {      /* cheats typed on a keyboard (port_main.c) */
            gRsTitleText->flags |= TEXTF_HIDDEN;     /* (the message goes where the title is) */
        } else if (gRsTitleText != NULL) {
            gRsTitleText->flags &= ~TEXTF_HIDDEN;
        }
    }
#endif
    /* player 2 joins: with save devices, go to the save menu for both players */
    if (gNumPlayersM1 == 0 && P2_ALLOWED && (contPressed(1, BTN_START) & BTN_START)) {
        gNumPlayersM1 = 1;
        p2JoinPressed();
        gP2Joined = 0;
        gRsFromRace = 0;
        if (gContReadState != 0) {
            p2JoinFree();
            sfxStop(0x90, -1);
            rsFree();
            animCancelAll();
            animBlinkInit();
            textFreeAll();
            saveMenuInit(2);
            return;
        }
    }
    if (--gRsBlinkTimer == 0) {
        gRsBlinkFrame = (gRsBlinkFrame + 1) & 3;
        gRsBlinkTimer = 3;
    }
    gRsUnlocked = ((gNumPlayersM1 != 0) ? gPlayerSetup[1].tracksDone : 0) | gPlayerSetup[0].tracksDone;
#ifdef PORT
    {
        extern s32 gAllTracks;
        if (gAllTracks) {
            gRsUnlocked = 0x3FFF;            /* the MURICA cheat: all 14 tracks (straight away: worked out each frame) */
        }
    }
#endif

    if (gRsState == 3) {
        gRsTitleText->xf.visible = 1;
        for (p = 0; p <= gNumPlayersM1; p++) {
            prev = gRsNode;
            btn = contPressed(p, 0xF00F00);
            if (btn & 0x100800) {
                gRsNode = gRsNodes[gRsNode].up;
            }
            if (btn & 0x800100) {
                gRsNode = gRsNodes[gRsNode].down;
            }
            if (btn & 0x200400) {
                gRsNode = gRsNodes[gRsNode].left;
            }
            if (btn & 0x400200) {
                gRsNode = gRsNodes[gRsNode].right;
            }
            if (prev != gRsNode) {
                sfxPlay(0x81, p);
            }
        }
        rsUpdateRaces();
        gRsEntry = gRsNodes[gRsNode].entry;
        e = &gRsEntries[gRsEntry];
        gRsRace = e->race;
        if (gRsShownRace != gRsRace) {
            /* new race under the cursor: scroll its name in */
            gRsShownRace = gRsRace;
            gRsXf[13].x = e->nameX;
            cur = gRsNameIdx == 0;
            n = &gRsNames[cur];
            gRsXf[13].y = e->nameY;
            if (n->anim != -1) {
                animCancel(gAnimSlots[n->anim].ramp);
            }
            gRsNameIdx = cur;
            if (n->text != NULL) {
                textFree(n->text);
            }
            name = gRsUnknownName;
            if (gRsRace < 15) {
                name = gRaceNames[gRsRace];
            }
            n->text = textCreate(0xC0, name, 2);
            n->race = gRsRace;
            textPlace(&n->text->xf, 0, 0, 0, 1);
            n->anim = animStart(gRsNameTime, gRsNameScripts[gRsEntry], &n->text->xf, rsNameGone, 0);
            gAnimSlots[n->anim].user = cur;
        }
        /* the previous name scrolls out (or is freed if hidden) */
        cur = gRsNameIdx == 0;
        if (gRsNames[cur].anim == -1 && gRsNames[cur].text != NULL) {
            if (gRsNames[cur].text->xf.scale != 0) {
                script = gRsNameOutScript;
                if (gRsNames[cur].race == 15) {
                    script = gRsNameOutScript2;
                }
                gRsNames[cur].anim = animStart(0.25f, script, &gRsNames[cur].text->xf, rsNameGone, 0);
                gAnimSlots[gRsNames[gRsNameIdx == 0].anim].user = gRsNameIdx == 0;
            } else {
                textFree(gRsNames[cur].text);
                gRsNames[gRsNameIdx == 0].text = NULL;
            }
        }
        /* A, B or Z on an unlocked race */
        for (p = 0; p <= gNumPlayersM1; p++) {
            if (!(contPressed(p, 0xB000) & 0xB000)) {
                continue;
            }
            r = gRsRace;
            if (r != 14 && r != 15 && !((1 << r) & gRsUnlocked)) {
                continue;
            }
            gRsState = 5;
            gRsResumeChosen = 0;
            gRsResume = 0;
            if (gContReadState == 0 || r != 14) {
                break;
            }
            /* Cruise the USA with a saved game in progress for every player? */
            count = 0;
            for (t = 0, ps = gPlayerSetup; t <= gNumPlayersM1; ) {
                if (ps->saveSlot == 6) {
                    break;
                }
                slot = &gSaveImage[ps->savePak].slots[ps->saveSlot];
                t++;
                ps++;
                if (!(slot->cruise & 8)) {
                    break;
                }
                count++;
            }
            if (count == 0) {
                break;
            }
            gRsResume = 1;
            if (gNumPlayersM1 != 0) {
                if (count == 2) {
                    for (t = 0, ps = gPlayerSetup; t < 2; t++, ps++) {
                        slot = &gSaveImage[ps->savePak].slots[ps->saveSlot];
                        track[t] = slot->cruise >> 4;
                        diff[t] = slot->opts & 0xE0;
                    }
                    if (track[0] != track[1] || diff[0] != diff[1]) {
                        gRsResume = 2;   /* the two saves are at different points */
                    }
                } else {
                    gRsResume = 2;
                }
            }
            gRsTitleText->xf.visible = 0;
            gRsResumeTrack = 13;
            for (ps = gPlayerSetup; ps <= &gPlayerSetup[gNumPlayersM1]; ps++) {
                slot = &gSaveImage[ps->savePak].slots[ps->saveSlot];
                if ((slot->cruise >> 4) < gRsResumeTrack) {
                    gRsResumeTrack = slot->cruise >> 4;
                }
            }
            for (n = gRsNames; n != &gRsNames[2]; n++) {
                if (n->text != NULL) {
                    n->text->xf.visible = 0;
                }
            }
            gRsState = 4;
            contPressed(0, BTN_ALL);
            contPressed(1, BTN_ALL);
            gRsYes = 1;
            gRsPulse = 0;
            sfxPlay(0x2C, -1);
            return;
        }
        if (gRsState == 5) {
            rsChoose();
        }
    }

    screenClear(-1, 1);
    if (gRsState == 6) {
        gRsState = 7;
        return;
    }
    if (gRsState == 7) {
        for (p = 0; p <= gNumPlayersM1; p++) {
            contPressed(p, BTN_ALL);
        }
        if (gRsRace == 14) {
            gTrackChoice = 0;
            if (gRsResumeChosen != 0) {
                for (p = 0; p <= gNumPlayersM1; p++) {
                    rsLoadSave(p);
                }
                gTrackChoice = gRsResumeTrack;
                gRsFromRace = 1;
                gCruiseContinue = 1;
            }
            gCruiseMode = 1;
        } else {
            gTrackChoice = gRsRace;
            gCruiseMode = 0;
        }
        raceOptionsApply();
        rsFree();
        trackLoad(gTrackChoice);
        if (gRsFromRace != 0) {
            if (gRacersEnabled != 0) {
                sfxPlay(0x7D, -1);
            }
            trackPrepare();
            for (p = 0; p <= gNumPlayersM1; p++) {
                playerSetupView(p);
            }
            gModeArg = 1;
            gOverlayRequest = OVERLAY_REQUEST(OVL_RACE);
            gModeFunc = RACE_START_MODE;
            return;
        }
        gModeFunc = carSelectInit;
        return;
    }

    bgDrawSetup(-1, 1);
    if (gRsFromRace == 0 || gRsState < 4) {
        drawDLSimple(gRsDLs[20], 0, 0);
    }
    xformsUpdate(gRsXf);
    if (gRsState > 0) {
        for (e = gRsEntries; e < &gRsEntries[10]; e++) {
            xf = &gRsXf[e->xform];
            if (xf->visible == 0) {
                continue;
            }
            if (!((1 << e->race) & gRsUnlocked)) {
                GDP_PIPESYNC();
                GDP_PRIMCOLOR(0x505050FF);       /* locked: dark grey */
                xf = &gRsXf[e->xform];
            }
            drawDLXform(gRsDLs[gRsRaceDL[e->race]], xf);
            GDP_PIPESYNC();
            GDP_PRIMCOLOR(0xFFFFFFFF);
        }
        if (gRsXf[11].visible != 0) {
            drawDLXform(gRsDLs[13 + gRsBlinkFrame], &gRsXf[11]);
            if (gRsState == 3 && gRsRace == 14) {
                drawDL(gRsDLs[21], 0x8B0000, 0x830000, 0, 0x10000);
            }
        }
        if (gRsXf[13].visible != 0) {
            if (gRsState < 3 || gRsRace == 14) {
                dl = 18;
            } else if (gRsRace != 15) {
                dl = 17;
            } else {
                dl = 19;
            }
            drawDLXform(gRsDLs[dl], &gRsXf[13]);
        }
        if (gRsXf[14].visible != 0) {
            drawDLXform(gRsDLs[gRsRaceDL[gRsRace]], &gRsXf[14]);
        }
    }
    {
        u32 *g = gDlCursor;
        gDlCursor += 2;
        g[1] = 0;
        g[0] = 0xBD000000;                   /* gSPPopMatrix */
    }

    if (gRsState == 4) {
        r = rsContinueMenu();
        if (r != -1) {
            gRsTitleText->xf.visible = 1;
            for (n = gRsNames; n != &gRsNames[2]; n++) {
                if (n->text != NULL) {
                    n->text->xf.visible = 1;
                }
            }
            if (gRsResume == 2 && r == 1) {
                gRsState = 3;                    /* saves differ: can't continue */
            } else {
                gRsState = 5;
                gRsResumeChosen = r;
                rsChoose();
            }
        }
    }
    if (gRsState == 2) {
        switch (continueFrame()) {
            case 0:
                p2JoinFree();
                musicStop();
                rsFree();
                gModeArg = 0;
                gModeFunc = frontEndEnter;
                break;
            case 1:
                gRsState = 3;
                break;
        }
    }
}

/* 0x8002ABD4 ovl3 - a race was chosen: hide the map (random race: pick it now) */
void rsChoose(void) {
    RsName *n;
    s32 cur;

    sfxPlay(0x90, -1);
    cur = gRsNameIdx == 0;
    if (gRsNames[cur].text != NULL) {
        textFree(gRsNames[cur].text);
        gRsNames[gRsNameIdx == 0].text = NULL;
    }
    if (gRsRace == 15) {
        gSecretRace = 1;
        rsPickRandom(gRsUnlocked);
        n = &gRsNames[gRsNameIdx];
        if (n->anim != -1) {
            animCancel(gAnimSlots[n->anim].ramp);
            n = &gRsNames[gRsNameIdx];
        }
        if (n->text != NULL) {
            textFree(n->text);
        }
        gRsNames[gRsNameIdx].text = textCreate(0xC0, gRaceNames[gRsRace], 2);
        textPlace(&gRsNames[gRsNameIdx].text->xf, 0x8B0000, 0x5A0000, 0, 1);
    } else {
        gSecretRace = 0;
    }
    gRsTitleText->xf.visible = 0;
    gRsXf[13].visible = 0;
    gRsXf[gRsEntries[gRsEntry].xform].visible = 0;
    gRsXf[14].visible = 1;
    animStart(0.5f, gRsOutroScript, NULL, NULL, 4);
    animStart(0.25f, gRsOutroScripts[gRsEntry], NULL, rsOutroDone, 0);
}

/* 0x8002AE18 ovl3 - continue a saved Cruise the USA: load this player's car and options */
void rsLoadSave(s32 player) {
    PlayerSetup *ps = &gPlayerSetup[player];
    SaveSlot *slot = &gSaveImage[ps->savePak].slots[ps->saveSlot];
    s32 cam;

    ps->car = slot->cruise & 7;
    ps->unk008 = slot->opts & 3;
    cam = (slot->opts & 0xC) >> 2;
    ps->cameraFollowsCar = cam;
    if (gNumPlayersM1 != 0 && cam == 1) {
        ps->cameraFollowsCar = 2;
    }
    ps->manual = (slot->opts & 0x10) >> 4;
    ps->totalTime = slot->totalTime;
    gDifficulty = (slot->opts & 0xE0) >> 5;
    gAiSkill[player] = slot->aiSkill;
    carSetPalette(player, ps->car, ps->unk008);
}

/* 0x8002AF20 ovl3 - the random secret race: an unlocked track (0-12) not among the last 8 picked */
void rsPickRandom(s32 allowed) {
    s32 *h;
    s32 left;
    s32 r;

    left = allowed & ~0x2000;
    for (h = gRandomRaceHistory; h < &gRandomRaceHistory[8]; h++) {
        if (*h != -1) {
            left &= ~(1 << *h);
        }
    }
    if (left == 0) {
        /* every allowed track (Washington DC aside) was picked recently: forget the history */
        for (h = gRandomRaceHistory; h != &gRandomRaceHistory[8]; h += 4) {
            h[0] = -1;
            h[1] = -1;
            h[2] = -1;
            h[3] = -1;
        }
    }
    /* pick an allowed track (Washington DC is never in 0..12) not in the history */
    do {
        do {
            r = randRange(0, 12);
            gRsRace = r;
        } while (!((1 << r) & allowed));
        for (h = gRandomRaceHistory; h < &gRandomRaceHistory[8]; h++) {
            if (r == *h) {
                break;
            }
        }
    } while (h < &gRandomRaceHistory[8]);
    for (h = &gRandomRaceHistory[7]; h != gRandomRaceHistory; h--) {
        h[0] = h[-1];
    }
    gRandomRaceHistory[0] = r;
    gRsNode = gRsTrackNode[r];
    gRsEntry = gRsNodes[gRsNode].entry;
}

/* 0x8002B0D4 ovl3 - each map spot shows its secret race while a player holds its button combination */
void rsUpdateRaces(void) {
    RsEntry *e;
    s32 secret;
    s32 p;

    for (e = gRsEntries; e != (RsEntry *)gRsNodes; e++) {
        secret = 0;
        if (gRsState == 3) {
            for (p = 0; p <= gNumPlayersM1; p++) {
                if (e->combo != 0 && (gPads[p].held & 0x403F) == (e->combo | 0x20)) {
                    secret = 1;
                }
#ifdef PORT
                /* the Dreamcast has no C buttons: both triggers held instead, for every spot with a
                 * second race (Beverly Hills -> Indiana, Grand Canyon -> San Francisco, US 101 ->
                 * Golden Gate Park, and Death Valley -> Washington DC, which had no combination) */
                if (e->races[1] >= 0 && (gPads[p].held & 0x0030) == 0x0030) {
                    secret = 1;
                }
#endif
            }
        }
        e->race = e->races[secret];
    }
}

/* 0x8002B17C ovl3 - free everything Choose Race created */
void rsFree(void) {
    RsName *n;

#ifdef PORT
    {
        void port_cheats_leave(void);
        port_cheats_leave();
    }
#endif
    for (n = gRsNames; n != &gRsNames[2]; n++) {
        if (n->anim != -1) {
            animCancel(gAnimSlots[n->anim].ramp);
        }
        if (n->text != NULL) {
            textFree(n->text);
        }
    }
    textFree(gRsTitleText);
    if (gRsFromRace) {
        p2JoinFree();
    }
    lowMemOverlayLoad();
    highHeapRestore(gRsHeapMark);
    lowMemOverlayRelease();
}

/* 0x8002B258 ovl3 - "CONTINUE <track>" / "NEW GAME" box: 1 continue, 0 new game, -1 still choosing */
s32 rsContinueMenu(void) {
    TextObj *t;
    char buf[0x70];
    u32 pulse;
    u32 btn;
    s32 p;

    bgDrawSetup(-1, 1);
    rectFill(320, 240, 0xFFEC0000, 0xDB0000, 20, 20, 20, 150);
    GDP_PIPESYNC();
    GDP_PRIMCOLOR(0x2A4260FF);
    drawDL(gRsBoxDL, 0x8B0000, 0x680000, 0, 0x13851);
    GDP_PIPESYNC();
    GDP_PRIMCOLOR(0xFFFFFFFF);
    pulse = (u32)(((f32)tblCos(gRsPulse) / 32768.0f) * 127.0f + 192.0f) * 0x01010100 + 0xFF;
    gRsPulse += 0x800;

    t = textCreate(1, gRsContinueTitle, 2);
    textPlace(&t->xf, 0x8B0000, 0x9F0000, 0, 0x10000);
    t->color = 0xFFFFFFFF;
    t->flags |= 3;
    textDraw(t, 0);
    textFree(t);

    t = textCreate(0, gRsNewGame, 2);
    textPlace(&t->xf, 0x8B0000, 0x6D0000, 0, 0x10000);
    t->flags |= 3;
    t->color = gRsYes ? 0x505050FF : pulse;
    textDraw(t, 0);
    textFree(t);

    if (gRsResume == 1) {
        sprintf(buf, gRsContinueFmt, gTrackNames[gRsResumeTrack]);
    } else {
        sprintf(buf, gRsContinue);
    }
    t = textCreate(0, buf, 2);
    textPlace(&t->xf, 0x8B0000, 0x4A0000, 0, 0x10000);
    t->flags |= 3;
    t->color = gRsYes ? pulse : 0x505050FF;
    textDraw(t, 0);
    textFree(t);

    for (p = 0; p <= gNumPlayersM1; p++) {
        btn = contPressed(p, BTN_ALL);
        if (btn & 0xB000) {
            return gRsYes;
        }
        if (btn & 0x30C00) {
            if (btn & 0x10800) {
                if (gRsYes == 1) {
                    gRsYes = 0;
                    sfxPlay(0x81, p);
                }
            } else if (gRsYes == 0) {
                gRsYes = 1;
                sfxPlay(0x81, p);
            }
        }
    }
    return -1;
}
