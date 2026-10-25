/*
 * sndcustom.c - driving held sounds directly, a command at a time (note on and
 * off, volume, pan, bend...), and "custom" sounds whose tracks run scripts the
 * game writes into a buffer of its own.
 * Original code: main segment, ROM 0xC3B0-0xCFD0 (RAM 0x8010B3B0-0x8010BFD0).
 */
#include "common.h"
#include "audio.h"

typedef void (*TrkCmdFunc)(SndTrack *t, s32 v);

extern s32 gSndCustomLabels;             /* 0x801326D0 - labels per track */
extern s32 gSndCustomCount0;             /* 0x801326D4 - tracks of each group */
extern s32 gSndCustomCount1;             /* 0x801326D8 */
extern s32 gSndCustomCount2;             /* 0x801326DC */
extern s32 gSndCustomOwned;              /* 0x801326E0 - we allocated the buffer */
extern u8 *gSndCustomBuf;                /* 0x801326E4 */
extern s32 gSndCustomSize;               /* 0x801326E8 */
extern u8 gSndCmdScratch[8];             /* 0x801494A0 */

SndTrack *heldTrack(s32 id, s32 k);      /* 0x80109C20 */
void trkRunInstrument(SndTrack *t, s32 v); /* 0x8010AE20 */
void trkRunBend(SndTrack *t, s32 v);     /* 0x8010AE84 */
void trkRunVolume(SndTrack *t, s32 v);   /* 0x8010AEE8 */
void trkRunPan(SndTrack *t, s32 v);      /* 0x8010AF40 */
void trkRunCmd(SndTrack *t, s32 v, TrkCmdFunc func); /* 0x8010AF98 */
void sndHeldQueueCmd(s32 id, s32 k, s32 v, TrkCmdFunc func);

/* 0x8010B3B0 */
void sndHeldInstrument(s32 id, s32 k, u16 v) {
    sndHeldQueueCmd(id, k, v, trkRunInstrument);
}

/* 0x8010B3DC - play a note on track k of a held sound */
void sndHeldNoteOn(s32 id, s32 k, u8 key, u8 vel) {
    sndLock();
    sndQueueOp(5);
    sndQueueArg(&id, 4);
    sndQueueArg(&k, 4);
    sndQueueArg(&key, 1);
    sndQueueArg(&vel, 1);
    sndUnlock();
}

/* 0x8010B448 - queued command 5 */
void sndCmdHeldNoteOn(void *p) {
    s32 id;
    s32 k;
    u8 key;
    u8 vel;

    sndQueueGet(&id, 4);
    sndQueueGet(&k, 4);
    sndQueueGet(&key, 1);
    sndQueueGet(&vel, 1);
    sndDoHeldNoteOn(id, k, key, vel);
}

/* 0x8010B4A8 */
void sndDoHeldNoteOn(s32 id, s32 k, u8 key, u8 vel) {
    SndTrack *t;
    u8 *saved;

    if (!sndIsLoaded()) {
        return;
    }
    sndLock();
    t = heldTrack(id, k);
    if (t != NULL) {
        saved = t->pc;
        t->pc = gSndCmdScratch;
        gSndCmdScratch[0] = 0x11;
        t->pc[1] = key;
        t->pc[2] = vel;
        gSndHandlers[t->handler][0x11](t);
        t->pc = saved;
    }
    sndUnlock();
}

/* 0x8010B57C */
void trkRunNoteOff(SndTrack *t, s32 v) {
    t->pc[0] = 0x12;
    t->pc[1] = v;
    gSndHandlers[t->handler][0x12](t);
}

/* 0x8010B5D4 */
void sndHeldNoteOff(s32 id, s32 k, u8 key) {
    sndHeldQueueCmd(id, k, key, trkRunNoteOff);
}

/* 0x8010B600 */
void sndHeldBend(s32 id, s32 k, s16 v) {
    sndHeldQueueCmd(id, k, v, trkRunBend);
}

/* 0x8010B630 */
void trkRunCmdA(SndTrack *t, s32 v) {
    t->pc[0] = 0xA;
    t->pc[1] = v;
    gSndHandlers[t->handler][0xA](t);
}

/* 0x8010B688 */
void sndHeldCmdA(s32 id, s32 k, u8 v) {
    sndHeldQueueCmd(id, k, v, trkRunCmdA);
}

/* 0x8010B6B4 */
void trkRunCmdB(SndTrack *t, s32 v) {
    t->pc[0] = 0xB;
    t->pc[1] = v;
    gSndHandlers[t->handler][0xB](t);
}

/* 0x8010B70C */
void sndHeldCmdB(s32 id, s32 k, u8 v) {
    sndHeldQueueCmd(id, k, v, trkRunCmdB);
}

/* 0x8010B738 */
void sndHeldVolume(s32 id, s32 k, u8 v) {
    sndHeldQueueCmd(id, k, v, trkRunVolume);
}

/* 0x8010B764 */
void sndHeldPan(s32 id, s32 k, u8 v) {
    sndHeldQueueCmd(id, k, v, trkRunPan);
}

/* 0x8010B790 */
void trkRunPedal(SndTrack *t, s32 v) {
    t->pc[0] = 0xE;
    t->pc[1] = v;
    gSndHandlers[t->handler][0xE](t);
}

/* 0x8010B7E8 */
void sndHeldPedal(s32 id, s32 k, u8 v) {
    sndHeldQueueCmd(id, k, v, trkRunPedal);
}

/* 0x8010B814 */
void trkRunFx(SndTrack *t, s32 v) {
    t->pc[0] = 0xF;
    t->pc[1] = v;
    gSndHandlers[t->handler][0xF](t);
}

/* 0x8010B86C */
void sndHeldFx(s32 id, s32 k, u8 v) {
    sndHeldQueueCmd(id, k, v, trkRunFx);
}

/* 0x8010B898 */
void trkRunCmd10(SndTrack *t, s32 v) {
    t->pc[0] = 0x10;
    t->pc[1] = v;
    gSndHandlers[t->handler][0x10](t);
}

/* 0x8010B8F0 */
void sndHeldCmd10(s32 id, s32 k, u8 v) {
    sndHeldQueueCmd(id, k, v, trkRunCmd10);
}

/* 0x8010B91C */
void sndHeldQueueCmd(s32 id, s32 k, s32 v, TrkCmdFunc func) {
    sndLock();
    sndQueueOp(4);
    sndQueueArg(&id, 4);
    sndQueueArg(&k, 4);
    sndQueueArg(&v, 4);
    sndQueueArg(&func, 4);
    sndUnlock();
}

/* 0x8010B988 - queued command 4 */
void sndCmdHeldCmd(void *p) {
    s32 id;
    s32 k;
    s32 v;
    TrkCmdFunc func;

    sndQueueGet(&id, 4);
    sndQueueGet(&k, 4);
    sndQueueGet(&v, 4);
    sndQueueGet(&func, 4);
    sndDoHeldCmd(id, k, v, func);
}

/* 0x8010B9E8 */
void sndDoHeldCmd(s32 id, s32 k, s32 v, TrkCmdFunc func) {
    SndTrack *t;

    if (!sndIsLoaded()) {
        return;
    }
    sndLock();
    t = heldTrack(id, k);
    if (t != NULL) {
        trkRunCmd(t, v, func);
    }
    sndUnlock();
}

/* 0x8010BA60 */
void sndCustomFree(void) {
    if (gSndCustomOwned) {
        if (gSndCustomBuf != NULL) {
            sndFree(gSndCustomBuf);
            gSndCustomBuf = NULL;
        }
        gSndCustomOwned = 0;
    }
}

/* 0x8010BAB0 */
u8 *sndCustomBuffer(void) {
    return gSndCustomBuf;
}

/* 0x8010BAC0 */
s32 sndCustomBufferSize(void) {
    return gSndCustomSize;
}

/* 0x8010BAD0 */
void sndCustomConfig(s32 labels, s32 n0, s32 n1, s32 n2) {
    gSndCustomLabels = labels;
    gSndCustomCount0 = n0;
    gSndCustomCount1 = n1;
    gSndCustomCount2 = n2;
}

/* 0x8010BAF4 - make a held sound of empty tracks (as configured) whose
 * scripts live in buf (allocated when NULL): each track gets `trackSize`
 * bytes, except group 2 tracks share what's left. Returns its id, 0 if none. */
s32 sndCustomCreate(u8 *buf, s32 trackSize, s32 size) {
    u8 n0 = 0;
    u8 n1;
    u8 n2;
    s32 created;
    s32 max;
    u8 s;
    u8 ti;
    u8 slot;
    SndSound *snd;
    u8 *list;
    SndTrack *t;
    s32 i;
    s32 *lbl;
    u8 *base;

    if (!sndIsLoaded()) {
        return 0;
    }
    n1 = 0;
    n2 = 0;
    created = 0;
    if (buf == NULL) {
        gSndCustomOwned = 1;
        gSndCustomBuf = sndAlloc(size);
        if (gSndCustomBuf == NULL) {
            return 0;
        }
    } else {
        gSndCustomOwned = 0;
        gSndCustomBuf = buf;
    }
    gSndCustomSize = size;
    sndLock();
    max = (u8)gSndCfg.maxTracks;
    for (s = 0; s < max; s++) {
        if (!(gSnd->sounds[s].flags & SND_ACTIVE)) {
            break;
        }
    }
    if (max == s) {
        sndUnlock();
        return 0;
    }
    snd = &gSnd->sounds[s];
    list = snd->tracks;
    for (ti = 0; ti < max; ti++) {
        t = &gSnd->tracks[ti];
        if (t->flags & TRK_ACTIVE) {
            continue;
        }
        if (n0 < gSndCustomCount0) {
            t->group = 0;
            t->handler = 1;
            slot = n0++;
            t->unk4C = trackSize - gSndCustomLabels * 4;
        } else if (n1 < gSndCustomCount1) {
            t->group = 1;
            t->handler = 1;
            slot = n1 + gSndCustomCount0;
            n1++;
            t->unk4C = trackSize - gSndCustomLabels * 4;
        } else if (n2 < gSndCustomCount2) {
            t->group = 2;
            t->handler = 1;
            slot = n2 + gSndCustomCount0 + gSndCustomCount1;
            n2++;
            t->unk4C = size - (gSndCustomCount0 + gSndCustomCount1) * trackSize - gSndCustomLabels * 4;
        } else {
            break;
        }
        created = 1;
        list[slot] = ti;
        t->sound = s;
        t->instrument = 0;
        t->volume = 0x7F;
        t->bend = 0;
        t->pan = 0x40;
        t->numNotes = 0;
        t->ppq = 120;
        t->tempo = 120;
        t->rate = sndRate(sndTicksPerSecond(), t->ppq, t->tempo);
        t->frac = 0;
        t->time = 0;
        t->numLabels = 0;
        t->unk48 = 2;
        t->sp = t->stack;
        t->numLabelsDef = gSndCustomLabels;
        lbl = (s32 *)(gSndCustomBuf + slot * trackSize);
        t->labels = lbl;
        for (i = 0; i < t->numLabelsDef; i++) {
            *lbl++ = 0;
        }
        base = gSndCustomBuf + slot * trackSize + gSndCustomLabels * 4;
        t->script = base;
        base[0] = 0;
        base[1] = 0x22;
        t->pc = sndReadDelta(t->script, &t->wait);
        t->flags &= ~TRK_40;
        t->flags |= TRK_PAUSED;
        t->flags |= TRK_ENDED;
        t->flags |= TRK_ACTIVE;
        t->flags &= ~TRK_TIMED;
        t->flags &= ~TRK_LOOPS;
        t->flags &= ~TRK_JUMPED;
        t->volMask = 0;
        t->flags |= TRK_SUSTAIN;
        snd->numTracks++;
        gSnd->numTracks++;
    }
    if (created) {
        snd->flags |= SND_HELD;
        snd->seq = -1;
        snd->handle = 0;
        snd->group = 0;
        snd->volume = 0x80;
        snd->pan = 0x40;
        snd->flags |= SND_ACTIVE;
        gSnd->numSounds++;
    }
    sndUnlock();
    if (created) {
        return s + 1;
    }
    return 0;
}
