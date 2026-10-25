/*
 * sndctl.c - the sound engine's track runner and its "control" handler: the
 * commands every track uses for flow (timers, loops, calls, jumps, tempo,
 * end of track) plus the simplest per-track settings.
 * Original code: main segment, ROM 0xDC40-0xF490 (RAM 0x8010CC40-0x8010E490).
 *
 * The original kept most of its working variables in static storage
 * (0x8014B8F0-0x8014B9B0); nothing else reads them, so they are locals here.
 */
#include "common.h"
#include "audio.h"

extern u8 gSndCtlNumTracks;              /* 0x801328A4 */
extern SndTrack *gSndCtlTracks;          /* 0x801328A8 */
extern SndSound *gSndCtlSounds;          /* 0x801328AC */
extern SndState *gSndCtlState;           /* 0x801328B0 */

/* the "call" commands push the address after themselves (both use 0x1A's length) */
#define CALL_LEN gSndCmdLen[0x1A]

#define ARG16(pc) ((pc)[1] | ((pc)[2] << 8))

/* 0x8010CC40 */
s32 sndCheckSeq(s32 seq, s32 a, s32 b) {
    return sndSeqValid(seq);
}

/* 0x8010CC64 */
s32 sndCheckLoaded(s32 a, s32 b) {
    return sndIsLoaded();
}

/* 0x8010CC88 */
s32 sndCheckSeqs(s32 seq, s32 seq2, s32 b) {
    s32 r = sndSeqValid(seq);

    if (r != 0) {
        r = sndSeqValid(seq2);
    }
    return r;
}

/* 0x8010CCC0 - read a delta time (MIDI variable-length number) */
u8 *sndReadDelta(u8 *p, u32 *out) {
    u32 v = *p++;

    if (v & 0x80) {
        u8 c;

        v &= 0x7F;
        do {
            c = *p++;
            v = (v << 7) + (c & 0x7F);
        } while (c & 0x80);
    }
    *out = v;
    return p;
}

/* 0x8010CD14 - write one */
u8 *sndWriteDelta(u8 *p, u32 v) {
    u8 buf[8];
    u8 *q = buf;

    *q++ = v & 0x7F;
    while ((v >>= 7) != 0) {
        *q++ = (v & 0x7F) | 0x80;
    }
    q--;
    for (;;) {
        *p++ = *q;
        if (!(*q & 0x80)) {
            break;
        }
        q--;
    }
    return p;
}

/* 0x8010CD84 - bytes needed to write one */
u8 sndDeltaSize(u32 v) {
    u8 out[8];

    return sndWriteDelta(out, v) - out;
}

/* 0x8010CE04 - hook 0 */
void ctlInit(SndState *st) {
    gSndCtlState = st;
    gSndCtlTracks = st->tracks;
    gSndCtlSounds = st->sounds;
    gSndCtlNumTracks = gSndCfg.maxTracks;
}

/* 0x8010CE3C */
void ctlHook1(void *p) {
}

/* 0x8010CE44 */
void ctlHook2(void *p) {
}

/* 0x8010CE4C */
void ctlHook3(void *p) {
}

/* 0x8010CE54 */
void ctlHook4(void *p) {
}

/* 0x8010CE5C - hook 5: the track has ended */
void ctlTrackEnd(SndTrack *t) {
    SndState *st = gSndCtlState;
    SndSound *snd = &st->sounds[t->sound];

    if (!(t->flags & TRK_ENDED)) {
        t->flags |= TRK_ENDED;
        if (--snd->numPlaying == 0) {
            snd->group = 0;
        }
    }
    if (!(t->flags & TRK_PAUSED)) {
        u8 *list = snd->tracks;
        s32 i = st->tracksPerSound;

        while (i-- != 0) {
            if (*list == t->index) {
                *list = 0xFF;
                break;
            }
            list++;
        }
        t->flags &= ~TRK_ACTIVE;
        gSndCtlState->numTracks--;
        if (--snd->numTracks == 0) {
            snd->flags &= ~SND_ACTIVE;
            gSndCtlState->numSounds--;
        }
    }
    t->flags &= ~TRK_TIMED;
}

/* 0x8010CFD8 */
void ctlHook6(void *p) {
}

/* 0x8010CFE0 - command 7: instrument */
void ctlInstrument(SndTrack *t) {
    u8 v = ARG16(t->pc);

    t->instrument = v;
}

/* 0x8010D008 */
void ctlCmd8(void *p) {
}

/* 0x8010D010 - command 9: pitch bend */
void ctlBend(SndTrack *t) {
    t->bend = ARG16(t->pc);
}

/* 0x8010D034 */
void ctlCmdA(void *p) {
}

/* 0x8010D03C */
void ctlCmdB(void *p) {
}

/* 0x8010D044 - command 0xC: volume */
void ctlVolume(SndTrack *t) {
    t->volume = t->pc[1];
}

/* 0x8010D060 - command 0xD: pan */
void ctlPan(SndTrack *t) {
    t->pan = t->pc[1];
}

/* 0x8010D07C */
void ctlCmdE(void *p) {
}

/* 0x8010D084 */
void ctlCmdF(void *p) {
}

/* 0x8010D08C */
void ctlCmd10(void *p) {
}

/* 0x8010D094 */
void ctlCmd11(void *p) {
}

/* 0x8010D09C */
void ctlCmd12(void *p) {
}

/* 0x8010D0A4 - command 0x13: pass a value to the timer with an id */
void ctlTimer(SndTrack *t) {
    SndState *st = gSndCtlState;
    u8 left = st->numTimers;
    SndTimer *tm;
    u8 i;

    if (left == 0) {
        return;
    }
    tm = st->timers;
    for (i = gSndCfg.maxTimers; i-- != 0; tm++) {
        if (tm->active) {
            if (t->pc[1] == tm->id) {
                tm->value = ARG16(t->pc + 1);
                tm->func(tm->id, tm->value, st, i);
                return;
            }
            if (--left == 0) {
                return;
            }
        }
    }
}

static void jumpTo(SndTrack *t, s32 label) {
    t->pc = t->script + t->labels[label];
    t->wait = 0;
    t->flags |= TRK_JUMPED;
}

/* 0x8010D1C8 - command 0x14: jump while a counter A is nonzero */
void ctlLoopA(SndTrack *t) {
    u8 *pc = t->pc;
    u8 *reg = &gSndCtlSounds[t->sound].regsA[pc[1]];
    s16 label;

    if (*reg == 0) {
        return;
    }
    if (*reg == 0xFF) {
        *reg = pc[2];
    }
    label = t->pc[3] | (t->pc[4] << 8);
    if (label >= 0 && label < t->numLabels) {
        jumpTo(t, label);
    }
}

/* 0x8010D2A8 - command 0x15: counted loop on a counter B */
void ctlLoopB(SndTrack *t) {
    u8 *pc = t->pc;
    u8 *reg = &gSndCtlSounds[t->sound].regsB[pc[1]];
    s16 label;

    if (*reg == 0) {
        return;
    }
    if (*reg == 0xFF) {
        *reg = pc[2];
    } else {
        *reg = *reg - 1;
    }
    label = t->pc[3] | (t->pc[4] << 8);
    if (label >= 0 && label < t->numLabels) {
        jumpTo(t, label);
    }
}

/* 0x8010D394 - command 0x16: reset counter A (0xFF: all of them) */
void ctlClearA(SndTrack *t) {
    u8 r = t->pc[1];
    u8 *reg;
    u8 i;

    if (r == 0xFF) {
        reg = gSndCtlSounds[t->sound].regsA;
        for (i = gSndCfg.regsA; i-- != 0;) {
            *reg++ = 0xFF;
        }
    } else {
        gSndCtlSounds[t->sound].regsA[r] = 0xFF;
    }
}

/* 0x8010D460 - command 0x17: reset counter B */
void ctlClearB(SndTrack *t) {
    u8 r = t->pc[1];
    u8 *reg;
    u8 i;

    if (r == 0xFF) {
        reg = gSndCtlSounds[t->sound].regsB;
        for (i = gSndCfg.regsB; i-- != 0;) {
            *reg++ = 0xFF;
        }
    } else {
        gSndCtlSounds[t->sound].regsB[r] = 0xFF;
    }
}

/* 0x8010D52C - command 0x18: set counter B */
void ctlSetB(SndTrack *t) {
    u8 *pc = t->pc;

    gSndCtlSounds[t->sound].regsB[pc[1]] = pc[2];
}

/* 0x8010D56C - command 0x19: tempo of every track of the sound */
void ctlTempoAll(SndTrack *t) {
    SndSound *snd = &gSndCtlSounds[t->sound];
    s16 i = gSndCtlState->hdr->seqs[snd->seq].numTracks;
    u8 *list = snd->tracks;
    u8 left = snd->numTracks;
    SndTrack *tr;

    for (; i-- != 0; list++) {
        if (*list != 0xFF) {
            tr = &gSndCtlTracks[*list];
            tr->tempo = ARG16(t->pc);
            tr->rate = sndRate(sndTicksPerSecond(), tr->ppq, tr->tempo);
            if (--left == 0) {
                return;
            }
        }
    }
}

/* 0x8010D708 - command 0x1A: every track of the sound calls a label */
void ctlCallAll(SndTrack *t) {
    s16 label = ARG16(t->pc);
    SndSound *snd;
    s16 i;
    u8 *list;
    u8 left;
    SndTrack *tr;

    if (label < 0 || label >= t->numLabels) {
        return;
    }
    snd = &gSndCtlSounds[t->sound];
    i = gSndCtlState->hdr->seqs[snd->seq].numTracks;
    left = snd->numTracks;
    list = snd->tracks;
    for (; i-- != 0; list++) {
        if (*list != 0xFF) {
            tr = &gSndCtlTracks[*list];
            *tr->sp = tr->pc + CALL_LEN;
            tr->sp++;
            jumpTo(tr, label);
            if (--left == 0) {
                return;
            }
        }
    }
}

/* 0x8010D8C0 - command 0x1B: every track jumps */
void ctlJumpAll(SndTrack *t) {
    s16 label = ARG16(t->pc);
    SndSound *snd;
    s16 i;
    u8 *list;
    u8 left;

    if (label < 0 || label >= t->numLabels) {
        return;
    }
    snd = &gSndCtlSounds[t->sound];
    i = gSndCtlState->hdr->seqs[snd->seq].numTracks;
    left = snd->numTracks;
    list = snd->tracks;
    for (; i-- != 0; list++) {
        if (*list != 0xFF) {
            jumpTo(&gSndCtlTracks[*list], label);
            if (--left == 0) {
                return;
            }
        }
    }
}

/* 0x8010DA50 - command 0x1C: every track returns */
void ctlReturnAll(SndTrack *t) {
    SndSound *snd = &gSndCtlSounds[t->sound];
    s16 i = gSndCtlState->hdr->seqs[snd->seq].numTracks;
    u8 left = snd->numTracks;
    u8 *list = snd->tracks;
    SndTrack *tr;

    for (; i-- != 0; list++) {
        if (*list != 0xFF) {
            tr = &gSndCtlTracks[*list];
            tr->sp--;
            tr->pc = *tr->sp;
            tr->wait = 0;
            tr->flags |= TRK_JUMPED;
            if (--left == 0) {
                return;
            }
        }
    }
}

/* 0x8010DB88 - command 0x1D (and any unknown command): end the whole sound */
void ctlEndAll(SndTrack *t) {
    u8 flags = t->flags;
    SndState *st = gSndCtlState;
    SndSound *snd = &st->sounds[t->sound];
    u8 *list = snd->tracks;
    s32 left = snd->numTracks;
    s32 i = st->tracksPerSound;
    SndTrack *tr;

    for (; i-- != 0; list++) {
        if (*list != 0xFF) {
            tr = &gSndCtlTracks[*list];
            gSndHandlers[tr->handler][5](tr);
            if (--left == 0) {
                break;
            }
        }
    }
    if (flags & TRK_PAUSED) {
        t->flags |= TRK_JUMPED;
    }
}

/* 0x8010DE18 - command 0x1E: tempo */
void ctlTempo(SndTrack *t) {
    t->tempo = ARG16(t->pc);
    t->rate = sndRate(sndTicksPerSecond(), t->ppq, t->tempo);
}

/* 0x8010DE7C - command 0x1F: call */
void ctlCall(SndTrack *t) {
    s16 label = ARG16(t->pc);

    if (label >= 0 && label < t->numLabels) {
        *t->sp = t->pc + CALL_LEN;
        t->sp++;
        jumpTo(t, label);
    }
}

/* 0x8010DF04 - command 0x20: jump */
void ctlJump(SndTrack *t) {
    s16 label = ARG16(t->pc);

    if (label >= 0 && label < t->numLabels) {
        jumpTo(t, label);
    }
}

/* 0x8010DF70 - command 0x21: return (to the delta time after the call) */
void ctlReturn(SndTrack *t) {
    t->sp--;
    t->pc = *t->sp;
    t->pc = sndReadDelta(t->pc, &t->wait);
    t->wait = 0;
    t->flags |= TRK_JUMPED;
}

/* 0x8010DFCC - command 0x22: end of the track - loop or end */
void ctlEnd(SndTrack *t) {
    u8 flags = t->flags;

    if ((flags & TRK_LOOPS) && t->time >= 16) {
        t->flags |= TRK_JUMPED;
        t->pc = t->script;
        t->pc = sndReadDelta(t->pc, &t->wait);
        return;
    }
    gSndHandlers[t->handler][5](t);
    if (flags & TRK_PAUSED) {
        t->flags |= TRK_JUMPED;
    }
}

/* 0x8010E0E4 */
void ctlCmd23(void *p) {
}

/* 0x8010E0EC - advance every track by one tick, running its due commands */
void sndTracksTick(void) {
    u8 left = gSndCtlState->numTracks;
    SndTrack *t;
    s32 i;
    u8 flags;
    u32 adv;
    s32 cmd;

    if (left != 0) {
        t = gSndCtlTracks;
        for (i = gSndCtlNumTracks; i-- != 0; t++) {
            flags = t->flags;
            if (!(flags & TRK_ACTIVE)) {
                continue;
            }
            if (!(flags & TRK_ENDED)) {
                t->frac += t->rate;
                adv = t->frac >> 16;
                t->time += adv;
                t->elapsed += adv;
                t->frac &= 0xFFFF;
                if ((flags & TRK_TIMED) && t->time >= t->stopTime) {
                    gSndHandlers[t->handler][5](t);
                } else {
                    while (t->elapsed >= t->wait) {
                        t->elapsed -= t->wait;
                        cmd = *t->pc;
                        if (cmd >= 7 && cmd < 0x13) {
                            gSndHandlers[t->handler][cmd](t);
                            t->pc += gSndCmdLen[cmd];
                            t->pc = sndReadDelta(t->pc, &t->wait);
                        } else if (cmd >= 0x13 && cmd < 0x24) {
                            gSndCtlFuncs[cmd](t);
                            if ((t->flags & TRK_ACTIVE) && !(t->flags & TRK_JUMPED)) {
                                t->pc += gSndCmdLen[cmd];
                                t->pc = sndReadDelta(t->pc, &t->wait);
                            } else {
                                t->flags &= ~TRK_JUMPED;
                            }
                        } else {
                            gSndCtlFuncs[0x1D](t);
                        }
                        if (t->elapsed < t->wait || !(t->flags & TRK_ACTIVE) || (t->flags & TRK_ENDED)) {
                            break;
                        }
                    }
                }
            }
            if (--left == 0) {
                break;
            }
        }
    }
    gSndHandlers[gSndCtlTracks->handler][2](gSndCtlTracks);
}
