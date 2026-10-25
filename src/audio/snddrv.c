/*
 * snddrv.c - sound engine core: configuration, loading the sound bank and
 * laying out the engine's tables in the caller's buffer, starting
 * sequences (a SndSound and a SndTrack for each of its tracks), stopping
 * them, timers, and the two master volumes (effects, music).
 * Original code: main segment, ROM 0x7210-0x96E0 (RAM 0x80106210-0x801086E0).
 */
#include "common.h"
#include "audio.h"

typedef void (*SndNotifyFunc)(s32 arg, s32 code);

extern SndFile *gSndFileHandle;          /* 0x801326A0 */
extern s32 gSndInited;                   /* 0x801326A4 */
extern s32 gSndLoaded;                   /* 0x801326A8 */
extern s32 gSndFlagAC;                   /* 0x801326AC */
extern s32 gSndBufOwned;                 /* 0x801326B0 - we allocated the buffer */
extern u8 *gSndBuf;                      /* 0x801326B4 */
extern s32 gSndUsed;                     /* 0x801326B8 - bytes of it used */
extern s32 gSndBufSize;                  /* 0x801326BC */
extern SndNotifyFunc gSndNotifyFunc;     /* 0x801326C0 */
extern s32 gSndNotifyArg;                /* 0x801326C4 */
extern u32 gSndNow;                      /* 0x80132758 */
extern s32 gSndTicking;                  /* 0x80132750 */
extern u32 gSndMagic;                    /* 0x8013B8E0 - 'SN64' */
extern u32 gSndMagic2;                   /* 0x8013B8E8 */
extern SndFileHeader gSndHdr;            /* 0x80149420 */
extern SndBankInfo gSndInfo;             /* 0x80149448 */
extern SndState gSndStateData;           /* 0x80149468 */

void sndSetSfxVolume_(u8 vol);           /* 0x8010828C */
void sndSetMusicVolume_(u8 vol);         /* 0x801084D8 */
void sndDoStop(s32 handle, s32 fade, s32 time);   /* 0x80107A08 */
void sndDoStopAll(s32 fade, s32 time);   /* 0x80107D94 */
void sndUnload(void);                    /* 0x80106784 */

#define ALIGN4(v) { (v) += (v) & 1; (v) += (v) & 2; }
#define ALIGN8(v) { (v) += (v) & 1; (v) += (v) & 2; (v) += (v) & 4; }

/* 0x80106210 - change the settings marked in the mask (before loading) */
void sndConfigSet(SndConfig *c) {
    if (c->mask & 0x1)    gSndCfg.numDmaBufs = c->s.numDmaBufs;
    if (c->mask & 0x2)    gSndCfg.numDmaMsgs = c->s.numDmaMsgs;
    if (c->mask & 0x4)    gSndCfg.dmaBufSize = c->s.dmaBufSize;
    if (c->mask & 0x8)    gSndCfg.extraSamples = c->s.extraSamples;
    if (c->mask & 0x10)   gSndCfg.frameLag = c->s.frameLag;
    if (c->mask & 0x20)   gSndCfg.maxVoices = c->s.maxVoices;
    if (c->mask & 0x40)   gSndCfg.maxUpdates = c->s.maxUpdates;
    if (c->mask & 0x80)   gSndCfg.maxSounds = c->s.maxSounds;
    if (c->mask & 0x100)  gSndCfg.maxTracks = c->s.maxTracks;
    if (c->mask & 0x200)  gSndCfg.regsA = c->s.regsA;
    if (c->mask & 0x400)  gSndCfg.regsB = c->s.regsB;
    if (c->mask & 0x800)  gSndCfg.maxTimers = c->s.maxTimers;
    if (c->mask & 0x1000) gSndCfg.tracksPerSound = c->s.tracksPerSound;
    if (c->mask & 0x2000) gSndCfg.stackDepth = c->s.stackDepth;
}

/* 0x801063D8 */
void sndConfigGet(SndConfig *c) {
    c->mask = 0;
    c->s.numDmaBufs = gSndCfg.numDmaBufs;
    c->s.numDmaMsgs = gSndCfg.numDmaMsgs;
    c->s.dmaBufSize = gSndCfg.dmaBufSize;
    c->s.extraSamples = gSndCfg.extraSamples;
    c->s.frameLag = gSndCfg.frameLag;
    c->s.maxVoices = gSndCfg.maxVoices;
    c->s.maxUpdates = gSndCfg.maxUpdates;
    c->s.maxSounds = gSndCfg.maxSounds;
    c->s.maxTracks = gSndCfg.maxTracks;
    c->s.regsA = gSndCfg.regsA;
    c->s.regsB = gSndCfg.regsB;
    c->s.maxTimers = gSndCfg.maxTimers;
    c->s.tracksPerSound = gSndCfg.tracksPerSound;
    c->s.stackDepth = gSndCfg.stackDepth;
}

/* 0x80106490 */
void sndNotify(s32 code) {
    if (gSndNotifyFunc != NULL) {
        gSndNotifyFunc(gSndNotifyArg, code);
    }
}

/* 0x801064C8 */
s32 sndZero(u8 *p, s32 n) {
    while (n-- != 0) {
        *p++ = 0;
    }
    return n;
}

/* 0x801064F0 */
void sndSetNotify(SndNotifyFunc f, s32 arg) {
    gSndNotifyFunc = f;
    gSndNotifyArg = arg;
}

/* 0x80106504 */
SndState *sndGetState(void) {
    return gSnd;
}

/* 0x80106514 */
s32 sndIsInited(void) {
    if (gSndInited == 0) {
        return 0;
    }
    return 1;
}

/* 0x80106538 */
s32 sndIsLoaded(void) {
    if (gSndLoaded == 0) {
        return 0;
    }
    return 1;
}

/* 0x8010655C */
s32 sndSeqValid(s32 seq) {
    if (seq < 0 || seq >= sndNumSeqs()) {
        return 0;
    }
    if (gSnd->hdr->seqs[seq].tracks == NULL) {
        return 0;
    }
    return 1;
}

/* 0x801065D0 */
void sndSetFlagAC(void) {
    if (gSndFlagAC == 0) {
        gSndFlagAC = 1;
    }
}

/* 0x801065F4 */
void sndInstallTick(void) {
    sndTickInstalled();
}

/* 0x80106614 */
void sndRemoveTick(void) {
    sndTickRemoved();
}

/* 0x80106634 */
s32 sndInit(void) {
    s32 ok = 0;

    if (gSndInited == 0) {
        sndStop();
        if (gSndTicking == 0) {
            sndInstallTick();
        }
        sndOpenHook();
        gSndInited = 1;
        ok = 1;
    }
    return ok;
}

/* 0x80106698 */
void sndShutdown(s32 removeTick) {
    if (sndIsInited() && gSndInited) {
        if (gSndLoaded) {
            sndUnload();
        }
        sndCloseHook();
        gSndInited = 0;
        if ((removeTick | gSndTicking) != 0) {
            sndRemoveTick();
        }
    }
}

/* 0x80106714 */
u8 *sndGetBuffer(void) {
    return gSndBuf;
}

/* 0x80106724 */
s32 sndGetUsed(void) {
    return gSndUsed;
}

/* 0x80106734 */
void sndFreeBuffer(void) {
    if (gSndBufOwned) {
        if (gSndBuf != NULL) {
            sndFree(gSndBuf);
            gSndBuf = NULL;
        }
        gSndBufOwned = 0;
    }
}

/* 0x80106784 */
void sndUnload(void) {
    if (gSndLoaded) {
        resetCleanup();
        sndStop();
        gSndHandlers[0][1](gSnd);
        gSndHandlers[1][1](gSnd);
        sndFreeBuffer();
        gSndLoaded = 0;
    }
}

/* 0x80106800 - memory needed for a bank and the engine tables */
s32 sndMemSize(s32 rom) {
    u32 p;
    u32 i;

    if (rom == 0) {
        return 0;
    }
    gSndFileHandle = sndFileOpen(rom);
    if (gSndFileHandle == NULL) {
        sndNotify(1);
        return gSndLoaded;
    }
    gSnd = &gSndStateData;
    gSndStateData.maxNotes = gSndCfg.maxVoices;
    gSnd->now = &gSndNow;
    gSnd->hdr = &gSndHdr;
    gSnd->info = &gSndInfo;
    if (sndFileRead((u8 *)gSnd->hdr, 0x20, gSndFileHandle) != 0x20) {
        sndNotify(2);
        return 0;
    }
    if (gSnd->hdr->magic != gSndMagic || gSnd->hdr->version != 2) {
        return 0;
    }
    if (sndFileRead((u8 *)&gSndInfo, 0x18, gSndFileHandle) != 0x18) {
        sndNotify(2);
        return 0;
    }
    sndFileClose(gSndFileHandle);
    gSnd->info->data = (u8 *)8;
    p = gSnd->hdr->dataSize + 8;
    gSnd->sounds = (SndSound *)p;
    p += gSndCfg.maxSounds * sizeof(SndSound);
    gSnd->tracks = (SndTrack *)p;
    p += gSndCfg.maxTracks * sizeof(SndTrack);
    gSnd->notes = (SndNote *)p;
    p += gSnd->maxNotes * sizeof(SndNote);
    gSnd->timers = (SndTimer *)p;
    gSnd->tracksPerSound = gSndCfg.tracksPerSound;
    p += gSndCfg.maxTimers * sizeof(SndTimer);
    for (i = 0; i < gSndCfg.maxSounds; i++) {
        p += gSndCfg.regsA;
        ALIGN4(p);
        p += gSndCfg.regsB;
        ALIGN4(p);
        p += gSnd->tracksPerSound;
        ALIGN4(p);
    }
    gSnd->stackDepth = gSndCfg.stackDepth;
    for (i = 0; i < gSndCfg.maxTracks; i++) {
        p += gSnd->stackDepth * 4;
    }
    ALIGN4(p);
    return p;
}

/* 0x80106ACC - load the bank at `rom` into `buf` (allocated if NULL) */
s32 sndLoad(s32 rom, u8 *buf, s32 size) {
    u32 p;
    u32 len;
    u32 i;
    SndNote *n;
    SndSound *s;
    SndTrack *t;
    u8 *q;
    s32 k;

    if (gSndLoaded) {
        sndUnload();
    }
    if (buf == NULL) {
        gSndBufOwned = 1;
        gSndBuf = sndAlloc(size);
        if (gSndBuf == NULL) {
            return gSndLoaded;
        }
    } else {
        gSndBufOwned = 0;
        gSndBuf = buf;
    }
    gSndBufSize = size;
    sndZero(gSndBuf, size);
    if (!sndIsInited()) {
        sndFreeBuffer();
        return gSndLoaded;
    }
    if (rom == 0) {
        sndFreeBuffer();
        return gSndLoaded;
    }
    gSndFileHandle = sndFileOpen(rom);
    if (gSndFileHandle == NULL) {
        sndNotify(1);
        sndFreeBuffer();
        return gSndLoaded;
    }
    gSnd = &gSndStateData;
    gSndStateData.maxNotes = gSndCfg.maxVoices;
    gSnd->now = &gSndNow;
    gSnd->hdr = &gSndHdr;
    gSnd->info = &gSndInfo;
    if (sndFileRead((u8 *)gSnd->hdr, 0x20, gSndFileHandle) != 0x20) {
        sndNotify(2);
        sndFreeBuffer();
        return 0;
    }
    if (gSnd->hdr->magic != gSndMagic2 || gSnd->hdr->version != 2) {
        sndFreeBuffer();
        return 0;
    }
    if (sndFileRead((u8 *)&gSndInfo, 0x18, gSndFileHandle) != 0x18) {
        sndNotify(2);
        sndFreeBuffer();
        return 0;
    }
    p = (u32)gSndBuf;
    ALIGN8(p);
    gSnd->info->data = (u8 *)p;
    len = gSnd->hdr->dataSize;
    p += len;
    if (gSnd->hdr->packed == 0) {
        if (sndFileRead(gSnd->info->data, len, gSndFileHandle) != len) {
            sndNotify(2);
            sndFreeBuffer();
            return 0;
        }
        sndFileClose(gSndFileHandle);
    } else {
        sndFileClose(gSndFileHandle);
        if (sndUnpack(gSnd->hdr->packed, rom, 0x38, (s32)gSnd->info->data, gSnd->hdr->dataSize) < 0) {
            return 0;
        }
    }
    gSnd->sounds = (SndSound *)p;
    p += gSndCfg.maxSounds * sizeof(SndSound);
    gSnd->tracks = (SndTrack *)p;
    p += gSndCfg.maxTracks * sizeof(SndTrack);
    gSnd->notes = (SndNote *)p;
    for (k = 0; k < gSnd->maxNotes; k++) {
        n = &gSnd->notes[k];
        n->pad01 = 1;
        n->voice = k;
    }
    p += gSnd->maxNotes * sizeof(SndNote);
    gSnd->timers = (SndTimer *)p;
    gSnd->tracksPerSound = gSndCfg.tracksPerSound;
    p += gSndCfg.maxTimers * sizeof(SndTimer);
    for (i = 0; i < gSndCfg.maxSounds; i++) {
        s = &gSnd->sounds[i];
        s->regsA = (u8 *)p;
        p += gSndCfg.regsA;
        ALIGN4(p);
        s->regsB = (u8 *)p;
        p += gSndCfg.regsB;
        ALIGN4(p);
        s->tracks = (u8 *)p;
        q = (u8 *)p;
        p += gSnd->tracksPerSound;
        ALIGN4(p);
        for (k = gSnd->tracksPerSound; k != 0; k--) {
            *q++ = 0xFF;
        }
    }
    gSnd->stackDepth = gSndCfg.stackDepth;
    for (i = 0; i < gSndCfg.maxTracks; i++) {
        t = &gSnd->tracks[i];
        t->index = i;
        t->stack = (u8 **)p;
        p += gSnd->stackDepth * 4;
        t->stackEnd = (u8 **)p;
    }
    ALIGN4(p);
    gSndHandlers[0][0](gSnd);
    gSndHandlers[1][0](gSnd);
    gSndUsed = p;
    gSndLoaded = 1;
    sndStart();
    return 1;
}

/* 0x801070BC - set up a track from its definition and the caller's overrides */
void sndTrackSetup(SndTrack *trk, SndTrackRef *ref, SndParams *prm) {
    u32 mask;

    trk->flags |= TRK_ACTIVE;
    trk->flags &= ~TRK_TIMED;
    trk->flags &= ~TRK_LOOPS;
    trk->flags &= ~TRK_JUMPED;
    trk->flags |= TRK_SUSTAIN;
    trk->handler = 1;
    trk->numNotes = 0;
    trk->group = ref->def->group;
    trk->frac = 0;
    trk->elapsed = 0;
    trk->time = 0;
    trk->sp = trk->stack;
    trk->ppq = ref->def->ppq;
    trk->numLabels = ref->def->numLabels;
    trk->unk48 = ref->def->scriptSize;
    trk->volMask = ref->def->volMask;
    if (prm == NULL || (mask = prm->mask) == 0) {
        mask = 0;
    }
    trk->volume = (mask & 0x1) ? prm->volume : ref->def->volume;
    trk->pan = (mask & 0x2) ? prm->pan : ref->def->pan;
    trk->instrument = (mask & 0x4) ? prm->instrument : ref->def->instrument;
    trk->bend = (mask & 0x8) ? prm->bend : ref->def->bend;
    if ((mask & 0x10) && (trk->volMask & (1 << prm->volBit))) {
        trk->flags |= TRK_40;
    } else {
        trk->flags &= ~TRK_40;
    }
    trk->tempo = (mask & 0x20) ? prm->tempo : ref->def->tempo;
    trk->rate = sndRate(sndTicksPerSecond(), trk->ppq, trk->tempo);
    if (mask & 0x40) {
        trk->stopTime = trk->time + prm->duration;
        trk->flags |= TRK_TIMED;
    } else {
        trk->flags &= ~TRK_TIMED;
    }
    if (mask & 0x80) {
        trk->flags |= TRK_LOOPS;
    } else {
        trk->flags &= ~TRK_LOOPS;
    }
    if (mask & 0x100) {
        trk->priority = prm->priority;
    } else {
        trk->priority = ref->def->priority;
    }
}

/* 0x80107388 */
void sndTrackSetScript(SndTrack *trk, SndTrackRef *ref) {
    trk->unk4C = ref->def->scriptSize;
    trk->numLabelsDef = ref->def->numLabels;
    trk->script = ref->script;
    trk->pc = sndReadDelta(ref->script, &trk->wait);
    trk->labels = ref->labels;
}

/* 0x801073FC - start a sequence: returns the sound number + 1, or 0 */
s32 sndStartSeq(SndSeqDef *def, s32 seq, s32 handle, s32 held, SndParams *prm) {
    s32 slot;
    s16 left;
    u8 used;
    u8 t;
    u8 *list;
    SndSound *snd;
    SndTrack *trk;
    SndTrackRef *ref;

    if (!sndSeqValid(seq)) {
        return 0;
    }
    sndLock();
    for (slot = 0; slot < (u8)gSndCfg.maxSounds; slot = (slot + 1) & 0xFF) {
        if (!(gSnd->sounds[slot].flags & SND_ACTIVE)) {
            break;
        }
    }
    if ((u8)gSndCfg.maxSounds == slot) {
        sndUnlock();
        return 0;
    }
    left = def->numTracks;
    used = 0;
    snd = &gSnd->sounds[slot];
    list = snd->tracks;
    for (t = 0; t < (u8)gSndCfg.maxTracks; t = (t + 1) & 0xFF) {
        trk = &gSnd->tracks[t];
        if (trk->flags & TRK_ACTIVE) {
            continue;
        }
        trk->sound = slot;
        ref = &def->tracks[used];
        sndTrackSetup(trk, ref, prm);
        sndTrackSetScript(trk, ref);
        if (held) {
            trk->flags |= TRK_PAUSED;
            trk->flags |= TRK_ENDED;
        } else {
            trk->flags &= ~TRK_PAUSED;
            trk->flags &= ~TRK_ENDED;
            snd->numPlaying++;
        }
        snd->numTracks++;
        left--;
        gSnd->numTracks++;
        *list++ = t;
        used = (used + 1) & 0xFF;
        if (left == 0) {
            break;
        }
    }
    if (used != 0) {
        snd->seq = seq;
        snd->handle = handle;
        if (held) {
            snd->group = 0;
            snd->flags |= SND_HELD;
        } else {
            snd->group = 1;
            snd->flags &= ~SND_HELD;
        }
        snd->volume = 0x80;
        snd->pan = 0x40;
        snd->flags |= SND_ACTIVE;
        gSnd->numSounds++;
    }
    sndUnlock();
    if (used != 0) {
        return slot + 1;
    }
    return 0;
}

/* 0x801076E0 */
void sndPlay(s32 seq) {
    sndPlayHandle(seq, 0);
}

/* 0x80107700 */
void sndPlayParams(s32 seq, SndParams *prm) {
    sndStartSeq(&gSnd->hdr->seqs[seq], seq, 0, 0, prm);
}

/* 0x80107750 - 1 = not playing, 2 = held, 3 = playing */
s32 sndSeqState(s32 seq) {
    s32 state;
    u8 i;
    u8 left;
    SndSound *s;

    if (!sndSeqValid(seq)) {
        return 0;
    }
    sndLock();
    i = (u8)gSndCfg.maxSounds;
    left = gSnd->numSounds;
    state = 1;
    s = gSnd->sounds;
    if (left != 0) {
        while (i-- != 0) {
            if (s->flags & SND_ACTIVE) {
                if (seq == s->seq) {
                    if (s->group == 0) {
                        state = 2;
                    } else if (s->group == 1) {
                        state = 3;
                    }
                }
                if (--left == 0) {
                    break;
                }
            }
            s++;
        }
    }
    sndUnlock();
    return state;
}

/* 0x8010783C - stop a sequence (marked now, done at the next tick) */
void sndStopSeq(s32 seq, s32 fade, s32 time) {
    u8 i;
    u8 left;
    SndSound *s;

    if (!sndSeqValid(seq)) {
        return;
    }
    sndLock();
    left = gSnd->numSounds;
    s = gSnd->sounds;
    if (left != 0) {
        i = (u8)gSndCfg.maxSounds;
        while (i-- != 0) {
            if (s->flags & SND_ACTIVE) {
                if (seq == s->seq) {
                    s->flags |= SND_STOPPING;
                    s->flags &= ~0x20;
                    s->flags &= ~0x10;
                }
                if (--left == 0) {
                    break;
                }
            }
            s++;
        }
    }
    if (fade) {
        sndQueueOp(0xD);
        sndQueueArg(&time, 4);
    } else {
        sndQueueOp(1);
    }
    sndQueueArg(&seq, 4);
    sndUnlock();
}

/* 0x8010794C */
void sndStop0(s32 seq) {
    sndStopSeq(seq, 0, 0);
}

/* 0x80107970 */
void sndStopFade(s32 seq, s32 time) {
    sndStopSeq(seq, 1, time);
}

/* 0x80107994 - queue command 1 */
void sndCmdStop(void) {
    s32 seq;

    sndQueueGet(&seq, 4);
    sndDoStop(seq, 0, 0);
}

/* 0x801079C8 - queue command 0xD */
void sndCmdStopFade(void) {
    s32 seq;
    s32 time;

    sndQueueGet(&time, 4);
    sndQueueGet(&seq, 4);
    sndDoStop(seq, 1, time);
}

/* 0x80107A08 */
void sndDoStop(s32 seq, s32 fade, s32 time) {
    u8 left;
    u8 i;
    u8 n;
    s32 k;
    s32 saved;
    u8 *list;
    SndSound *s;
    SndTrack *trk;

    if (!sndSeqValid(seq)) {
        return;
    }
    sndLock();
    left = gSnd->numSounds;
    s = gSnd->sounds;
    if (left != 0) {
        if (fade) {
            saved = sndGetReleaseTime();
            sndSetReleaseTime(time);
        }
        i = (u8)gSndCfg.maxSounds;
        while (i-- != 0) {
            if (s->flags & SND_ACTIVE) {
                if (seq == s->seq && (s->flags & SND_STOPPING)) {
                    n = s->numTracks;
                    s->flags &= ~SND_STOPPING;
                    list = s->tracks;
                    for (k = gSnd->tracksPerSound; k != 0; k--, list++) {
                        if (*list == 0xFF) {
                            continue;
                        }
                        trk = &gSnd->tracks[*list];
                        gSndHandlers[trk->handler][5](trk);
                        if (--n == 0) {
                            break;
                        }
                    }
                }
                if (--left == 0) {
                    break;
                }
            }
            s++;
        }
        if (fade) {
            sndSetReleaseTime(saved);
        }
    }
    sndUnlock();
}

/* 0x80107C08 - stop everything */
void sndStopAll(s32 fade, s32 time) {
    u8 i;
    u8 left;
    SndSound *s;

    if (!sndIsLoaded()) {
        return;
    }
    sndLock();
    left = gSnd->numSounds;
    s = gSnd->sounds;
    if (left != 0) {
        i = (u8)gSndCfg.maxSounds;
        while (i-- != 0) {
            if (s->flags & SND_ACTIVE) {
                s->flags |= SND_STOPPING;
                s->flags &= ~0x20;
                s->flags &= ~0x10;
                if (--left == 0) {
                    break;
                }
            }
            s++;
        }
    }
    if (fade) {
        sndQueueOp(0xC);
        sndQueueArg(&time, 4);
    } else {
        sndQueueOp(0);
    }
    sndUnlock();
}

/* 0x80107CF8 */
void resetCleanup(void) {
    sndStopAll(0, 0);
}

/* 0x80107D1C */
void sndStopAllFade(s32 time) {
    sndStopAll(1, time);
}

/* 0x80107D40 - queue command 0 */
void sndCmdStopAll(void) {
    sndDoStopAll(0, 0);
}

/* 0x80107D64 - queue command 0xC */
void sndCmdStopAllFade(void) {
    s32 time;

    sndQueueGet(&time, 4);
    sndDoStopAll(1, time);
}

/* 0x80107D94 */
void sndDoStopAll(s32 fade, s32 time) {
    u8 left;
    u8 i;
    u8 n;
    s32 k;
    s32 saved;
    u8 *list;
    SndSound *s;
    SndTrack *trk;

    if (!sndIsLoaded()) {
        return;
    }
    sndLock();
    left = gSnd->numSounds;
    s = gSnd->sounds;
    if (left != 0) {
        if (fade) {
            saved = sndGetReleaseTime();
            sndSetReleaseTime(time);
        }
        i = (u8)gSndCfg.maxSounds;
        while (i-- != 0) {
            if ((s->flags & SND_ACTIVE) && (s->flags & SND_STOPPING)) {
                n = s->numTracks;
                s->flags &= ~SND_STOPPING;
                list = s->tracks;
                for (k = gSnd->tracksPerSound; k != 0; k--, list++) {
                    if (*list == 0xFF) {
                        continue;
                    }
                    trk = &gSnd->tracks[*list];
                    gSndHandlers[trk->handler][5](trk);
                    if (--n == 0) {
                        break;
                    }
                }
                if (--left == 0) {
                    break;
                }
            }
            s++;
        }
        if (fade) {
            sndSetReleaseTime(saved);
        }
    }
    sndUnlock();
}

/* 0x80107F80 - register a timer function (command 0x13 calls it) */
void sndTimerAdd(u8 id, void *func) {
    u8 i;
    SndTimer *t;

    if (!sndIsLoaded()) {
        return;
    }
    sndLock();
    i = (u8)gSndCfg.maxTimers;
    if (i != gSnd->numTimers) {
        t = gSnd->timers;
        while (i-- != 0) {
            if (t->active == 0) {
                t->value = 0;
                t->id = id;
                t->func = func;
                gSnd->numTimers++;
                break;
            }
            t++;
        }
    }
    sndUnlock();
}

/* 0x80108080 */
void sndTimerRemove(u8 id) {
    u8 i;
    u8 left;
    SndTimer *t;

    if (!sndIsLoaded()) {
        return;
    }
    sndLock();
    left = gSnd->numTimers;
    if (left != 0) {
        t = gSnd->timers;
        i = (u8)gSndCfg.maxTimers;
        while (i-- != 0) {
            if (t->active != 0) {
                if (id == t->id) {
                    t->active = 0;
                    gSnd->numTimers--;
                    break;
                }
                if (--left == 0) {
                    break;
                }
            }
            t++;
        }
    }
    sndUnlock();
}

/* 0x801081B0 */
u8 sndGetSfxVolume(void) {
    if (!sndIsLoaded()) {
        return 0;
    }
    return gSndSfxVolume;
}

/* 0x801081E8 */
u8 sndGetMusicVolume(void) {
    if (!sndIsLoaded()) {
        return 0;
    }
    return gSndMusicVolume;
}

/* 0x80108220 */
void sndSetSfxVolume(u8 vol) {
    sndLock();
    sndQueueOp(2);
    sndQueueArg(&vol, 1);
    sndUnlock();
}

/* 0x8010825C - queue command 2 */
void sndCmdSfxVolume(void) {
    u8 vol;

    sndQueueGet(&vol, 1);
    sndSetSfxVolume_(vol);
}

/* resend each playing track's volume so the new master volume applies */
static void sndResendVolumes(s32 group) {
    u8 i;
    u8 left;
    u8 n;
    s32 k;
    u8 *list;
    u8 *savedPc;
    u8 cmd[16];
    SndSound *s;
    SndTrack *trk;

    i = (u8)gSndCfg.maxSounds;
    left = gSnd->numSounds;
    s = gSnd->sounds;
    if (left != 0) {
        while (i-- != 0) {
            if (s->flags & SND_ACTIVE) {
                n = s->numTracks;
                list = s->tracks;
                for (k = gSnd->tracksPerSound; k != 0; k--, list++) {
                    if (*list == 0xFF) {
                        continue;
                    }
                    trk = &gSnd->tracks[*list];
                    if (trk->group == group) {
                        savedPc = trk->pc;
                        trk->pc = cmd;
                        cmd[0] = 0xC;
                        trk->pc[1] = trk->volume;
                        trk->volume = 0;
                        gSndHandlers[trk->handler][0xC](trk);
                        trk->pc = savedPc;
                    }
                    if (--n == 0) {
                        break;
                    }
                }
                if (--left == 0) {
                    break;
                }
            }
            s++;
        }
    }
}

/* 0x8010828C */
void sndSetSfxVolume_(u8 vol) {
    if (!sndIsLoaded()) {
        return;
    }
    gSndSfxVolume = vol;
    sndLock();
    sndResendVolumes(0);
    sndUnlock();
}

/* 0x8010846C */
void sndSetMusicVolume(u8 vol) {
    sndLock();
    sndQueueOp(3);
    sndQueueArg(&vol, 1);
    sndUnlock();
}

/* 0x801084A8 - queue command 3 */
void sndCmdMusicVolume(void) {
    u8 vol;

    sndQueueGet(&vol, 1);
    sndSetMusicVolume_(vol);
}

/* 0x801084D8 */
void sndSetMusicVolume_(u8 vol) {
    if (!sndIsLoaded()) {
        return;
    }
    gSndMusicVolume = vol;
    sndLock();
    sndResendVolumes(1);
    sndUnlock();
}

/* 0x801086B4 */
u8 sndGetStereo(void) {
    return gSndStereo;
}

/* 0x801086C4 */
void sndSetStereo(u8 on) {
    gSndStereo = on;
}
