/*
 * sndutil.c - sound engine support: error and unpack hooks, the lock, the
 * 120 Hz tick, reading the sound bank, debug strings, and the command queue
 * that carries the game's calls to the engine's next tick.
 * Original code: main segment, ROM 0xCFD0-0xDC40 (RAM 0x8010BFD0-0x8010CC40).
 */
#include "common.h"
#include "audio.h"

typedef void (*SndErrorFunc)(const char *msg, s32 a, s32 b, s32 c);
typedef s32 (*SndUnpackFunc)(s32 type, s32 a, s32 b, s32 c, s32 d);


extern s32 gSndTicking;                  /* 0x80132750 - the player callback is installed */
extern s32 gSndTicks;                    /* 0x80132754 */
extern u32 gSndNow;                      /* 0x80132758 - ms */
extern s32 gSndRunning;                  /* 0x8013275C */
extern u32 gSndNowFrac;                  /* 0x80132760 */
extern s32 gSndLockDepth;                /* 0x80132764 */
extern SndFunc gSndQueueFuncs[15];       /* 0x80132770 */
extern s32 gSndQueueLen;                 /* 0x801327AC */
extern s32 gSndQueueFull;                /* 0x801327B0 */
extern u8 *gSndQueueWrite;               /* 0x801327B4 */
extern SndFile gSndFile;                 /* 0x801494C0 */
extern SndFile gSndFile2;                /* 0x801494C8 */
extern SndErrorFunc gSndErrorFunc;       /* 0x801494D0 */
extern SndUnpackFunc gSndUnpackFunc;     /* 0x801494D4 */
extern s32 gSndLockMask;                 /* 0x801494D8 */
extern char gSndDbgSounds[0x80];         /* 0x801494E0 */
extern char gSndDbgTracks[0x80];         /* 0x80149560 */
extern char gSndDbgNotes[0x80];          /* 0x801495E0 */
extern char gSndDbgRelease[0x80];        /* 0x80149660 */
extern u8 gSndQueueData[0x2000];         /* 0x801496E0 */
extern u8 gSndQueueOps[0x200];           /* 0x8014B6E0 */
extern u8 *gSndQueueRead;                /* 0x8014B8E0 */

s32 dmaRomRead(s32 rom, u8 *dst, s32 len);  /* 0x801053D4 */
SndState *sndGetState(void);             /* 0x80106504 */
void sndTracksTick(void);                /* 0x8010E0EC */
s32 intDisable(void);                    /* 0x80111110 */
void intRestore(s32 mask);               /* 0x80111130 */

/* 0x8010BFD0 */
void sndSetErrorFunc(SndErrorFunc f) {
    gSndErrorFunc = f;
}

/* 0x8010BFDC */
void sndError(const char *msg, s32 a, s32 b, s32 c) {
    if (gSndErrorFunc != NULL) {
        gSndErrorFunc(msg, a, b, c);
    }
}

/* 0x8010C008 */
void sndSetUnpackFunc(SndUnpackFunc f) {
    gSndUnpackFunc = f;
}

/* 0x8010C014 */
s32 sndUnpack(s32 type, s32 a, s32 b, s32 c, s32 d) {
    if (gSndUnpackFunc != NULL) {
        return gSndUnpackFunc(type & 0xFF, a, b, c, d);
    }
    return -1;
}

/* 0x8010C05C */
void sndOpenHook(void) {
}

/* 0x8010C064 */
void sndCloseHook(void) {
}

/* 0x8010C06C - no heap here */
void *sndAlloc(s32 size) {
    return NULL;
}

/* 0x8010C078 */
void sndFree(void *p) {
}

/* 0x8010C080 */
void sndStop(void) {
    gSndRunning = 0;
}

/* 0x8010C08C */
void sndStart(void) {
    gSndRunning = 1;
}

/* 0x8010C09C - the lock nests: interrupts are off until the last unlock */
void sndLock(void) {
    if (gSndLockDepth++ == 0) {
        gSndLockMask = intDisable();
    }
}

/* 0x8010C0DC */
void sndUnlock(void) {
    if (--gSndLockDepth == 0) {
        intRestore(gSndLockMask);
    }
}

/* 0x8010C11C */
s32 sndTicksPerSecond(void) {
    return 120;
}

/* 0x8010C124 - track time advance per tick (16.16) for a tempo */
u32 sndRate(s16 tps, s16 ppq, s16 tempo) {
    u64 v = (u64)((s64)tempo * 0x10000 * (s64)ppq);

    v /= 60;
    v /= (u64)(s64)tps;
    return (u32)v;
}

/* 0x8010C198 - the synthesizer's player callback: 120 times a second */
s32 sndTick(void) {
    gSndNowFrac += 0x85555;              /* 8.333 ms, 16.16 */
    gSndTicks++;
    gSndNow += gSndNowFrac >> 16;
    gSndNowFrac &= 0xFFFF;
    if (gSndRunning) {
        sndRunQueue();
        sndTracksTick();
    }
    return 0;
}

/* 0x8010C21C */
void sndTickInstalled(void) {
    gSndRunning = 0;
    gSndTicking = 1;
}

/* 0x8010C234 */
void sndTickRemoved(void) {
    gSndTicking = 0;
}

/* 0x8010C240 */
s32 sndFileExists(s32 rom) {
    return 1;
}

/* 0x8010C24C */
SndFile *sndFileOpen(s32 rom) {
    gSndFile.start = rom;
    gSndFile.pos = rom;
    return &gSndFile;
}

/* 0x8010C260 */
s32 sndFileRead(u8 *dst, s32 len, SndFile *f) {
    dmaRomRead(f->pos, dst, len);
    f->pos += len;
    return len;
}

/* 0x8010C2B4 */
s32 sndFileSeek(SndFile *f, s32 off, s32 whence) {
    if (whence == 0) {
        f->pos = f->start + off;
        return 0;
    }
    if (whence == 1) {
        f->pos += off;
    }
    return 0;
}

/* 0x8010C2F0 */
s32 sndFileTell(SndFile *f) {
    return f->pos;
}

/* 0x8010C2FC */
void sndFileClose(SndFile *f) {
}

/* 0x8010C304 */
s32 sndFileExists2(s32 rom) {
    return 1;
}

/* 0x8010C310 */
SndFile *sndFileOpen2(s32 rom) {
    return &gSndFile2;
}

/* 0x8010C320 */
s32 sndFileRead2(u8 *dst, s32 len, SndFile *f, s32 unused) {
    return 0;
}

/* 0x8010C338 */
void sndFileClose2(SndFile *f) {
}

/* 0x8010C340 - '1' for each sound in use */
char *sndDebugSounds(void) {
    SndState *st = sndGetState();
    u32 n = gSndCfg.maxSounds;
    u32 i;

    if (n >= 0x80) {
        n = 0x7F;
    }
    if (st != NULL) {
        for (i = 0; i < n; i++) {
            gSndDbgSounds[i] = (st->sounds[i].flags & SND_ACTIVE) ? '1' : '0';
        }
        gSndDbgSounds[n] = 0;
    }
    return gSndDbgSounds;
}

/* 0x8010C4E0 - '1' for each track in use */
char *sndDebugTracks(void) {
    SndState *st = sndGetState();
    u32 n = gSndCfg.maxTracks;
    u32 i;

    if (n >= 0x80) {
        n = 0x7F;
    }
    if (st != NULL) {
        for (i = 0; i < n; i++) {
            gSndDbgTracks[i] = (st->tracks[i].flags & TRK_ACTIVE) ? '1' : '0';
        }
        gSndDbgTracks[n] = 0;
    }
    return gSndDbgTracks;
}

/* 0x8010C67C */
u8 sndTrackVolume(s32 track) {
    SndState *st = sndGetState();

    if (st != NULL) {
        return st->tracks[track].volume;
    }
    return 0xFF;
}

/* 0x8010C6C8 - '1' for each note playing */
char *sndDebugNotes(void) {
    SndState *st = sndGetState();
    u32 n = gSndCfg.maxVoices;
    u32 i;

    if (n >= 0x80) {
        n = 0x7F;
    }
    if (st != NULL) {
        for (i = 0; i < n; i++) {
            gSndDbgNotes[i] = (st->notes[i].flags & NOTE_ACTIVE) ? '1' : '0';
        }
        gSndDbgNotes[n] = 0;
    }
    return gSndDbgNotes;
}

/* 0x8010C864 - '1' for each note releasing */
char *sndDebugRelease(void) {
    SndState *st = sndGetState();
    u32 n = gSndCfg.maxVoices;
    u32 i;

    if (n >= 0x80) {
        n = 0x7F;
    }
    if (st != NULL) {
        for (i = 0; i < n; i++) {
            gSndDbgRelease[i] = (st->notes[i].flags & NOTE_RELEASE) ? '1' : '0';
        }
        gSndDbgRelease[n] = 0;
    }
    return gSndDbgRelease;
}

/* 0x8010CA00 */
s32 sndCopy(u8 *dst, u8 *src, s32 n) {
    while (n-- != 0) {
        *dst++ = *src++;
    }
    return n;
}

/* 0x8010CA30 - queue a command */
void sndQueueOp(u8 op) {
    if (gSndQueueFull == 0) {
        if (gSndQueueLen < 0x200) {
            gSndQueueOps[gSndQueueLen++] = op;
            return;
        }
        gSndQueueFull = 1;
    }
}

/* 0x8010CA88 - and its arguments */
void sndQueueArg(void *src, s32 n) {
    if (gSndQueueFull == 0) {
        if ((gSndQueueData - gSndQueueWrite) + 0x2000 >= n) {
            sndCopy(gSndQueueWrite, src, n);
            gSndQueueWrite += n;
            return;
        }
        if (gSndQueueLen > 0) {
            gSndQueueLen--;
        }
        gSndQueueFull = 1;
    }
}

/* 0x8010CB34 - a command reads its arguments */
void sndQueueGet(void *dst, s32 n) {
    sndCopy(dst, gSndQueueRead, n);
    gSndQueueRead += n;
}

/* 0x8010CB78 - run the queued commands (start of a tick) */
void sndRunQueue(void) {
    s32 i;

    gSndQueueFull = 0;
    gSndQueueRead = gSndQueueData;
    for (i = 0; i < gSndQueueLen; i++) {
        gSndQueueFuncs[gSndQueueOps[i]](NULL);
    }
    sndLock();
    gSndQueueLen = 0;
    gSndQueueWrite = gSndQueueData;
    sndUnlock();
}
