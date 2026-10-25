/*
 * sndpause.c - pausing and resuming sounds (one sequence, or everything).
 * A paused track is marked ended-but-held, so the runner skips it while its
 * notes stay allocated; resuming clears the marks.
 * Original code: main segment, ROM 0x96E0-0xA1F0 (RAM 0x801086E0-0x801091F0).
 */
#include "common.h"
#include "audio.h"

void sndRecordStart(s32 mask);           /* 0x8010E4D4 */
void sndRecordStop(void);                /* 0x8010E504 */
void sndRecordReplay(s16 seq, s32 track, SndTrack *t); /* 0x8010E510 */

/* 0x801086E0 */
void trkResume(SndTrack *t, SndSound *snd) {
    if (t->flags & TRK_ENDED) {
        t->flags &= ~TRK_40;
        t->flags &= ~TRK_ENDED;
        if (++snd->numPlaying != 0) {
            snd->group = 1;
        }
    }
}

/* 0x80108730 */
void trkPause(SndTrack *t, SndSound *snd) {
    if (!(t->flags & TRK_ENDED)) {
        t->flags |= TRK_40;
        t->flags |= TRK_ENDED;
        if (--snd->numPlaying == 0) {
            snd->group = 0;
        }
    }
}

/* mark the active sounds playing `seq` (any when seq < 0) */
static void markSounds(s32 seq, u8 flag) {
    u8 left = gSnd->numSounds;
    SndSound *snd = gSnd->sounds;
    u32 i;

    if (left == 0) {
        return;
    }
    for (i = gSndCfg.maxSounds; i-- != 0; snd++) {
        if (snd->flags & SND_ACTIVE) {
            left--;
            if (seq < 0 || seq == snd->seq) {
                snd->flags |= flag;
            }
            if (left == 0) {
                return;
            }
        }
    }
}

/* 0x80108780 - pause a sequence (release 1: release its notes too) */
void sndPause(s32 seq, s32 release) {
    if (!sndSeqValid(seq)) {
        return;
    }
    sndLock();
    markSounds(seq, SND_PAUSING);
    sndQueueOp(6);
    sndQueueArg(&seq, 4);
    sndQueueArg(&release, 4);
    sndUnlock();
}

/* 0x80108848 - queued command 6 */
void sndCmdPause(void *p) {
    s32 seq;
    s32 release;

    sndQueueGet(&seq, 4);
    sndQueueGet(&release, 4);
    sndDoPause(seq, release);
}

/* 0x80108888 */
void sndDoPause(s32 seq, s32 release) {
    u32 i;
    s32 left;
    SndSound *snd;
    u8 *list;
    s32 n;
    s32 k;
    SndTrack *t;

    if (!sndSeqValid(seq)) {
        return;
    }
    sndLock();
    left = gSnd->numSounds;
    snd = gSnd->sounds;
    if (left != 0) {
        for (i = gSndCfg.maxSounds; i-- != 0; snd++) {
            if (!(snd->flags & SND_ACTIVE)) {
                continue;
            }
            if (seq == snd->seq && (snd->flags & SND_PAUSING)) {
                snd->flags &= ~SND_PAUSING;
                n = snd->numTracks;
                list = snd->tracks;
                for (k = gSnd->tracksPerSound; k-- != 0; list++) {
                    if (*list != 0xFF) {
                        t = &gSnd->tracks[*list];
                        trkPause(t, snd);
                        if (release == 1) {
                            gSndHandlers[t->handler][6](t);
                        }
                        if (--n == 0) {
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
    sndUnlock();
}

/* 0x80108A54 - resume a paused sequence */
void sndResume(s32 seq) {
    if (!sndSeqValid(seq)) {
        return;
    }
    sndLock();
    markSounds(seq, SND_RESUMING);
    sndQueueOp(7);
    sndQueueArg(&seq, 4);
    sndUnlock();
}

/* 0x80108B0C - queued command 7 */
void sndCmdResume(void *p) {
    s32 seq;

    sndQueueGet(&seq, 4);
    sndDoResume(seq);
}

/* 0x80108B3C */
void sndDoResume(s32 seq) {
    u32 i;
    u8 left;
    SndSound *snd;
    u8 *list;
    s32 n;
    s32 k;

    if (!sndSeqValid(seq)) {
        return;
    }
    sndLock();
    left = gSnd->numSounds;
    snd = gSnd->sounds;
    if (left != 0) {
        for (i = gSndCfg.maxSounds; i-- != 0; snd++) {
            if (!(snd->flags & SND_ACTIVE)) {
                continue;
            }
            if (seq == snd->seq && (snd->flags & SND_RESUMING)) {
                n = snd->numTracks;
                snd->flags &= ~SND_RESUMING;
                list = snd->tracks;
                for (k = gSnd->tracksPerSound; k-- != 0; list++) {
                    if (*list != 0xFF) {
                        trkResume(&gSnd->tracks[*list], snd);
                        if (--n == 0) {
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
    sndUnlock();
}

/* 0x80108CA4 - pause everything (release 1: release the notes, recording
 * those of the groups in recMask to restart on resume) */
void sndPauseAll(s32 release, s32 recMask) {
    if (!sndIsLoaded()) {
        return;
    }
    sndLock();
    markSounds(-1, SND_PAUSING);
    sndQueueOp(8);
    sndQueueArg(&release, 4);
    sndQueueArg(&recMask, 4);
    sndUnlock();
}

/* 0x80108D5C - queued command 8 */
void sndCmdPauseAll(void *p) {
    s32 release;
    s32 recMask;

    sndQueueGet(&release, 4);
    sndQueueGet(&recMask, 4);
    sndDoPauseAll(release, recMask);
}

/* 0x80108D9C */
void sndDoPauseAll(s32 release, s32 recMask) {
    u32 i;
    s32 left;
    SndSound *snd;
    u8 *list;
    s32 n;
    s32 k;
    SndTrack *t;

    if (!sndIsLoaded()) {
        return;
    }
    sndLock();
    if (release == 1) {
        sndRecordStart(recMask);
    }
    left = gSnd->numSounds;
    snd = gSnd->sounds;
    if (left != 0) {
        for (i = gSndCfg.maxSounds; i-- != 0; snd++) {
            if (!(snd->flags & SND_ACTIVE) || !(snd->flags & SND_PAUSING)) {
                continue;
            }
            n = snd->numTracks;
            snd->flags &= ~SND_PAUSING;
            list = snd->tracks;
            for (k = gSnd->tracksPerSound; k-- != 0; list++) {
                if (*list != 0xFF) {
                    t = &gSnd->tracks[*list];
                    if (release == 1) {
                        gSndHandlers[t->handler][6](t);
                    }
                    trkPause(t, snd);
                    if (--n == 0) {
                        break;
                    }
                }
            }
            if (--left == 0) {
                break;
            }
        }
    }
    if (release == 1) {
        sndRecordStop();
    }
    sndUnlock();
}

/* 0x80108F84 - resume everything (replay 1: restart the recorded notes) */
void sndResumeAll(s32 replay) {
    if (!sndIsLoaded()) {
        return;
    }
    sndLock();
    markSounds(-1, SND_RESUMING);
    sndQueueOp(9);
    sndQueueArg(&replay, 4);
    sndUnlock();
}

/* 0x8010902C - queued command 9 */
void sndCmdResumeAll(void *p) {
    s32 replay;

    sndQueueGet(&replay, 4);
    sndDoResumeAll(replay);
}

/* 0x8010905C */
void sndDoResumeAll(s32 replay) {
    u32 i;
    s32 left;
    SndSound *snd;
    u8 *list;
    s32 n;
    s32 k;
    SndTrack *t;

    if (!sndIsLoaded()) {
        return;
    }
    sndLock();
    left = gSnd->numSounds;
    snd = gSnd->sounds;
    if (left != 0) {
        for (i = gSndCfg.maxSounds; i-- != 0; snd++) {
            if (!(snd->flags & SND_ACTIVE) || !(snd->flags & SND_RESUMING)) {
                continue;
            }
            n = snd->numTracks;
            snd->flags &= ~SND_RESUMING;
            list = snd->tracks;
            for (k = gSnd->tracksPerSound; k-- != 0; list++) {
                if (*list != 0xFF) {
                    t = &gSnd->tracks[*list];
                    trkResume(t, snd);
                    if (replay) {
                        sndRecordReplay(snd->seq, *list, t);
                    }
                    if (--n == 0) {
                        break;
                    }
                }
            }
            if (--left == 0) {
                break;
            }
        }
    }
    sndUnlock();
}
