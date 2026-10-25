/*
 * sndheld.c - "held" sounds: sequences started by slot rather than handle
 * (the id is the sound slot + 1), which stay allocated when they end so they
 * can be restarted, have their settings changed or read back, be rewound or
 * be moved to a point in time.
 * Original code: main segment, ROM 0xABC0-0xC3B0 (RAM 0x80109BC0-0x8010B3B0).
 */
#include "common.h"
#include "audio.h"

extern u8 gSndCmdScratch[8];             /* 0x801494A0 - script for one command */

void trkResume(SndTrack *t, SndSound *snd);  /* 0x801086E0 */
void trkSetParams(SndTrack *t, SndParams *prm);
u32 sndHeldTime(s32 id);

/* 0x80109BC0 */
SndSound *heldSound(s32 id) {
    SndSound *snd;

    if (id > 0 && (u32)id <= gSndCfg.maxSounds) {
        snd = &gSnd->sounds[(u8)(id - 1)];
        if (snd->flags & SND_HELD) {
            return snd;
        }
        return NULL;
    }
    return NULL;
}

/* 0x80109C20 - the k-th track of a held sound, if it is set aside */
SndTrack *heldTrack(s32 id, s32 k) {
    SndSound *snd;
    u8 idx;
    SndTrack *t;

    if (id > 0 && (u32)id <= gSndCfg.maxSounds) {
        snd = &gSnd->sounds[(u8)(id - 1)];
        if (snd->flags & SND_HELD) {
            idx = snd->tracks[k];
            if (idx != 0xFF) {
                t = &gSnd->tracks[idx];
                if (t->flags & TRK_PAUSED) {
                    return t;
                }
                return NULL;
            }
        }
    }
    return NULL;
}

/* 0x80109CC0 - start a sequence as a held sound; returns its id */
s32 sndPlayHeld(s32 seq) {
    if (!sndSeqValid(seq)) {
        return 0;
    }
    return sndStartSeq(&gSnd->hdr->seqs[seq], seq, 0, 1, NULL);
}

/* 0x80109D18 - 0: no such sound, 1: stopped, 2: playing */
s32 sndHeldState(s32 id) {
    SndSound *snd;
    s32 r;

    if (!sndIsLoaded()) {
        return 0;
    }
    r = 0;
    snd = heldSound(id);
    if (snd != NULL) {
        if (snd->group == 0) {
            r = 1;
        } else if (snd->group == 1) {
            r = 2;
        }
    }
    return r;
}

/* 0x80109D88 - stop and free a held sound */
void sndHeldFree(s32 id) {
    SndSound *snd;
    s32 n;
    s32 k;
    u8 *list;
    SndTrack *t;

    if (!sndIsLoaded()) {
        return;
    }
    sndLock();
    snd = heldSound(id);
    if (snd != NULL) {
        n = snd->numTracks;
        list = snd->tracks;
        for (k = gSnd->tracksPerSound; k-- != 0; list++) {
            if (*list != 0xFF) {
                t = &gSnd->tracks[*list];
                t->flags &= ~TRK_PAUSED;
                gSndHandlers[t->handler][5](t);
                if (--n == 0) {
                    break;
                }
            }
        }
        snd->flags &= ~SND_HELD;
    }
    sndUnlock();
}

/* 0x80109EC0 - (re)start a held sound */
void sndHeldStart(s32 id, SndParams *prm) {
    SndSound *snd;
    s32 n;
    s32 k;
    u8 *list;
    SndTrack *t;

    if (!sndIsLoaded()) {
        return;
    }
    sndLock();
    snd = heldSound(id);
    if (snd != NULL) {
        n = snd->numTracks;
        list = snd->tracks;
        for (k = gSnd->tracksPerSound; k-- != 0; list++) {
            if (*list != 0xFF) {
                t = &gSnd->tracks[*list];
                trkResume(t, snd);
                trkSetParams(t, prm);
                if (--n == 0) {
                    break;
                }
            }
        }
        snd->group = 1;
    }
    sndUnlock();
}

/* 0x80109FD8 */
void sndHeldStartDefault(s32 id) {
    sndHeldStart(id, NULL);
}

/* 0x80109FF8 - restart one track of a held sound */
void sndHeldStartTrack(s32 id, s32 k, SndParams *prm) {
    SndSound *snd;
    SndTrack *t;

    if (!sndIsLoaded()) {
        return;
    }
    sndLock();
    snd = heldSound(id);
    if (snd != NULL) {
        t = heldTrack(id, k);
        if (t != NULL) {
            trkResume(t, snd);
            trkSetParams(t, prm);
        }
    }
    sndUnlock();
}

/* 0x8010A084 */
void sndHeldStartTrackDefault(s32 id, s32 k) {
    sndHeldStartTrack(id, k, NULL);
}

/* 0x8010A0A4 - read a held sound's settings (the fields prm->mask asks for) */
void sndHeldGetParams(s32 id, SndParams *prm) {
    SndSound *snd;
    s32 n;
    s32 k;
    u8 *list;

    if (!sndIsLoaded()) {
        return;
    }
    sndLock();
    snd = heldSound(id);
    if (snd != NULL) {
        n = snd->numTracks;
        list = snd->tracks;
        for (k = gSnd->tracksPerSound; k-- != 0; list++) {
            if (*list != 0xFF) {
                trkGetParams(&gSnd->tracks[*list], prm);
                if (--n == 0) {
                    break;
                }
            }
        }
    }
    sndUnlock();
}

/* 0x8010A194 */
void sndHeldSetParams(s32 id, SndParams *prm) {
    SndSound *snd;
    s32 n;
    s32 k;
    u8 *list;

    if (!sndIsLoaded()) {
        return;
    }
    sndLock();
    snd = heldSound(id);
    if (snd != NULL) {
        n = snd->numTracks;
        list = snd->tracks;
        for (k = gSnd->tracksPerSound; k-- != 0; list++) {
            if (*list != 0xFF) {
                trkSetParams(&gSnd->tracks[*list], prm);
                if (--n == 0) {
                    break;
                }
            }
        }
    }
    sndUnlock();
}

/* 0x8010A284 - stop a held sound (it stays allocated) */
void sndHeldStop(s32 id) {
    SndSound *snd;
    s32 n;
    s32 k;
    u8 *list;
    SndTrack *t;

    if (!sndIsLoaded()) {
        return;
    }
    sndLock();
    snd = heldSound(id);
    if (snd != NULL) {
        n = snd->numTracks;
        list = snd->tracks;
        for (k = gSnd->tracksPerSound; k-- != 0; list++) {
            if (*list != 0xFF) {
                t = &gSnd->tracks[*list];
                gSndHandlers[t->handler][5](t);
                if (--n == 0) {
                    break;
                }
            }
        }
    }
    sndUnlock();
}

/* 0x8010A398 */
void sndHeldTempo(s32 id, s16 tempo) {
    SndSound *snd;
    s32 n;
    s32 k;
    u8 *list;
    SndTrack *t;
    u32 rate;

    if (!sndIsLoaded()) {
        return;
    }
    rate = 0;
    sndLock();
    snd = heldSound(id);
    if (snd != NULL) {
        n = snd->numTracks;
        list = snd->tracks;
        for (k = gSnd->tracksPerSound; k-- != 0; list++) {
            if (*list != 0xFF) {
                t = &gSnd->tracks[*list];
                t->tempo = tempo;
                if (rate == 0) {
                    rate = sndRate(sndTicksPerSecond(), t->ppq, t->tempo);
                }
                t->rate = rate;
                if (--n == 0) {
                    break;
                }
            }
        }
    }
    sndUnlock();
}

/* 0x8010A4C8 - (does nothing) */
void sndHeldUnused(s32 id, s32 a, s32 b) {
    sndIsLoaded();
}

/* 0x8010A4F0 - back to the start */
s32 sndHeldRewind(s32 id) {
    SndSound *snd;
    s32 n;
    s32 k;
    u8 *list;
    SndTrack *t;

    if (!sndIsLoaded()) {
        return 0;
    }
    sndLock();
    snd = heldSound(id);
    if (snd != NULL) {
        n = snd->numTracks;
        list = snd->tracks;
        for (k = gSnd->tracksPerSound; k-- != 0; list++) {
            if (*list != 0xFF) {
                t = &gSnd->tracks[*list];
                gSndHandlers[t->handler][6](t);
                t->frac = 0;
                t->time = 0;
                t->pc = sndReadDelta(t->script, &t->wait);
                if (--n == 0) {
                    break;
                }
            }
        }
    }
    sndUnlock();
    return 0;
}

/* Run a track's commands, without its notes, up to time `target`
 * (stopping early at the end of the track). Returns the time reached.
 * (The two versions differ in the pc they report after jumps.) */
static u32 trkSeekLoop(SndTrack *t, u32 target, u8 **outPc, u32 time, u32 wait, u8 *pc, s32 fwd) {
    s32 done;
    s32 cmd;

    for (;;) {
        time += wait;
        if (time >= target) {
            done = 1;
            break;
        }
        cmd = *t->pc;
        if (cmd == 0x22) {
            done = 0;
            break;
        }
        if (cmd == 0x11 || cmd == 0x12) {
            pc = t->pc + gSndCmdLen[cmd];
            t->pc = pc;
            t->pc = sndReadDelta(pc, &wait);
        } else if (cmd >= 7 && cmd < 0x13) {
            gSndHandlers[t->handler][cmd](t);
            pc = t->pc + gSndCmdLen[cmd];
            t->pc = pc;
            t->pc = sndReadDelta(pc, &wait);
        } else if (cmd >= 0x13 && cmd < 0x24) {
            gSndCtlFuncs[cmd](t);
            if (!(t->flags & TRK_JUMPED)) {
                pc = t->pc + gSndCmdLen[cmd];
                t->pc = pc;
                t->pc = sndReadDelta(pc, &wait);
            } else {
                if (fwd) {
                    pc = t->pc;
                }
                t->flags &= ~TRK_JUMPED;
            }
        } else {
            gSndCtlFuncs[0x1D](t);
            if (fwd) {
                pc = t->pc;
            }
        }
    }
    if (done) {
        t->wait = time - target;
        t->frac = 0;
        t->time = target;
    } else {
        t->wait = 0;
        t->frac = 0;
        t->time = time;
    }
    *outPc = pc;
    return t->time;
}

/* 0x8010A630 - from where the track is */
u32 trkSeekForward(SndTrack *t, u32 target, u8 **outPc) {
    return trkSeekLoop(t, target, outPc, t->time + (t->frac >> 16), t->wait, t->pc, 1);
}

/* 0x8010A864 - from the start */
u32 trkSeekFromStart(SndTrack *t, u32 target, u8 **outPc) {
    u32 wait = 0;
    u8 *pc;

    t->pc = t->script;
    pc = t->pc;
    t->pc = sndReadDelta(t->pc, &wait);
    return trkSeekLoop(t, target, outPc, 0, wait, pc, 0);
}

/* 0x8010AA98 - skip a held sound ahead by `dt`; returns its time */
u32 sndHeldSkip(s32 id, u32 dt) {
    SndSound *snd;
    s32 n;
    s32 k;
    u8 *list;
    SndTrack *t;
    u8 *pc;

    if (!sndIsLoaded()) {
        return 0;
    }
    sndLock();
    snd = heldSound(id);
    if (snd != NULL) {
        n = snd->numTracks;
        list = snd->tracks;
        for (k = gSnd->tracksPerSound; k-- != 0; list++) {
            if (*list != 0xFF) {
                t = &gSnd->tracks[*list];
                gSndHandlers[t->handler][6](t);
                trkSeekForward(t, t->time + dt, &pc);
                if (--n == 0) {
                    break;
                }
            }
        }
    }
    sndUnlock();
    return sndHeldTime(id);
}

/* 0x8010ABF8 - move a held sound to a time */
u32 sndHeldSeek(s32 id, u32 time) {
    SndSound *snd;
    s32 n;
    s32 k;
    u8 *list;
    SndTrack *t;
    u8 *pc;

    if (!sndIsLoaded()) {
        return 0;
    }
    sndLock();
    snd = heldSound(id);
    if (snd != NULL) {
        n = snd->numTracks;
        list = snd->tracks;
        for (k = gSnd->tracksPerSound; k-- != 0; list++) {
            if (*list != 0xFF) {
                t = &gSnd->tracks[*list];
                gSndHandlers[t->handler][6](t);
                trkSeekFromStart(t, time, &pc);
                if (--n == 0) {
                    break;
                }
            }
        }
    }
    sndUnlock();
    return sndHeldTime(id);
}

/* 0x8010AD50 - the furthest time of its tracks */
u32 sndHeldTime(s32 id) {
    u32 max = 0;
    SndSound *snd;
    s32 n;
    s32 k;
    u8 *list;
    u8 idx;
    u32 tm;

    if (!sndIsLoaded()) {
        return 0;
    }
    sndLock();
    snd = heldSound(id);
    if (snd != NULL) {
        n = snd->numTracks;
        list = snd->tracks;
        for (k = gSnd->tracksPerSound; k-- != 0;) {
            idx = *list++;
            if (idx != 0xFF) {
                n--;
                tm = gSnd->tracks[idx].time;
                if (max < tm) {
                    max = tm;
                }
                if (n == 0) {
                    break;
                }
            }
        }
    }
    sndUnlock();
    return max;
}

/* 0x8010AE20 - run one command through the track's handler (the track's pc
 * points at a scratch script while these run) */
void trkRunInstrument(SndTrack *t, s32 v) {
    t->pc[0] = 7;
    t->pc[1] = v;
    t->pc[2] = v >> 8;
    gSndHandlers[t->handler][7](t);
}

/* 0x8010AE84 */
void trkRunBend(SndTrack *t, s32 v) {
    t->pc[0] = 9;
    t->pc[1] = v;
    t->pc[2] = v >> 8;
    gSndHandlers[t->handler][9](t);
}

/* 0x8010AEE8 */
void trkRunVolume(SndTrack *t, s32 v) {
    t->pc[0] = 0xC;
    t->pc[1] = v;
    gSndHandlers[t->handler][0xC](t);
}

/* 0x8010AF40 */
void trkRunPan(SndTrack *t, s32 v) {
    t->pc[0] = 0xD;
    t->pc[1] = v;
    gSndHandlers[t->handler][0xD](t);
}

/* 0x8010AF98 */
void trkRunCmd(SndTrack *t, s32 v, void (*func)(SndTrack *, s32)) {
    u8 *saved = t->pc;

    t->pc = gSndCmdScratch;
    func(t, v);
    t->pc = saved;
}

/* 0x8010AFDC - set a track's settings (clears its stop time and looping) */
void trkSetParams(SndTrack *t, SndParams *prm) {
    u32 mask;

    t->flags &= ~TRK_TIMED;
    t->flags &= ~TRK_LOOPS;
    if (prm == NULL || prm->mask == 0) {
        return;
    }
    mask = prm->mask;
    if (mask & 1) {
        t->volume = prm->volume;
        trkRunCmd(t, prm->volume, trkRunVolume);
    }
    if (mask & 2) {
        t->pan = prm->pan;
        trkRunCmd(t, prm->pan, trkRunPan);
    }
    if (mask & 4) {
        t->instrument = prm->instrument;
        trkRunCmd(t, prm->instrument, trkRunInstrument);
    }
    if (mask & 8) {
        t->bend = prm->bend;
        trkRunCmd(t, prm->bend, trkRunBend);
    }
    if (mask & 0x10) {
        if (t->volMask & (1 << prm->volBit)) {
            t->flags |= TRK_40;
            gSndHandlers[t->handler][6](t);
        } else {
            t->flags &= ~TRK_40;
        }
    }
    if (mask & 0x20) {
        t->tempo = prm->tempo;
        t->rate = sndRate(sndTicksPerSecond(), t->ppq, t->tempo);
    }
    if (mask & 0x40) {
        t->stopTime = t->time + prm->duration;
        t->flags |= TRK_TIMED;
    }
    if (mask & 0x80) {
        t->flags |= TRK_LOOPS;
    }
}

/* 0x8010B248 - read a track's settings into prm (the fields in its mask;
 * the duration and loop bits are cleared if they're not set) */
void trkGetParams(SndTrack *t, SndParams *prm) {
    u32 mask = prm->mask;

    if (mask == 0) {
        return;
    }
    if (mask & 1) {
        prm->volume = t->volume;
    }
    if (mask & 2) {
        prm->pan = t->pan;
    }
    if (mask & 4) {
        prm->instrument = t->instrument;
    }
    if (mask & 8) {
        prm->bend = t->bend;
    }
    if (mask & 0x10) {
        prm->volBit = t->volMask;
    }
    if (mask & 0x20) {
        prm->tempo = t->tempo;
    }
    if (mask & 0x40) {
        if (!(t->flags & TRK_TIMED)) {
            prm->mask &= ~0x40;
        } else {
            prm->duration = t->stopTime - t->time;
        }
    }
    if (mask & 0x80) {
        if (!(t->flags & TRK_LOOPS)) {
            prm->mask &= ~0x80;
        }
    }
}
