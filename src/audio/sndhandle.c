/*
 * sndhandle.c - controlling playing sounds by the handle the game gave them:
 * starting with settings, changing the settings, asking the state, stopping.
 * Original code: main segment, ROM 0xA1F0-0xABC0 (RAM 0x801091F0-0x80109BC0).
 */
#include "common.h"
#include "audio.h"

/* 0x801091F0 */
void sndPlayHandle(s32 seq, s32 handle) {
    sndStartSeq(&gSnd->hdr->seqs[seq], seq, handle, 0, NULL);
}

/* 0x80109238 */
void sndPlayHandleParams(s32 seq, s32 handle, SndParams *prm) {
    sndStartSeq(&gSnd->hdr->seqs[seq], seq, handle, 0, prm);
}

/* 0x8010928C - apply settings to a track. Volume, pan and bend go through
 * the track's handler by running a two-byte script of that command. */
void trkApply(SndTrack *t, SndParams *prm) {
    u8 *savedPc;
    u8 cmd[4];
    u32 mask;

    if (prm == NULL || prm->mask == 0) {
        return;
    }
    mask = prm->mask;
    if (mask & 1) {
        savedPc = t->pc;
        t->pc = cmd;
        cmd[0] = 0xC;
        t->pc[1] = prm->volume;
        gSndHandlers[t->handler][0xC](t);
        t->pc = savedPc;
    }
    if (mask & 2) {
        savedPc = t->pc;
        t->pc = cmd;
        cmd[0] = 0xD;
        t->pc[1] = prm->pan;
        gSndHandlers[t->handler][0xD](t);
        t->pc = savedPc;
    }
    if (mask & 4) {
        t->instrument = prm->instrument;
    }
    if (mask & 8) {
        savedPc = t->pc;
        t->pc = cmd;
        cmd[0] = 9;
        t->pc[1] = prm->bend;
        t->pc[2] = prm->bend >> 8;
        gSndHandlers[t->handler][9](t);
        t->pc = savedPc;
    }
    if (mask & 0x10) {
        if (t->volMask & (1 << prm->volBit)) {
            t->flags |= TRK_40;
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

/* 0x801094F0 - change the settings of the sounds with a handle */
void sndSetParams(s32 handle, SndParams *prm) {
    sndLock();
    sndQueueOp(0xB);
    sndQueueArg(&handle, 4);
    sndQueueArg(prm, sizeof(SndParams));
    sndUnlock();
}

/* 0x8010953C - queued command 0xB */
void sndCmdSetParams(void *p) {
    s32 handle;
    SndParams prm;

    sndQueueGet(&handle, 4);
    sndQueueGet(&prm, sizeof(SndParams));
    sndDoSetParams(handle, &prm);
}

/* 0x80109578 */
void sndDoSetParams(s32 handle, SndParams *prm) {
    u8 left;
    u8 i;
    SndSound *snd;
    u8 *list;
    s32 n;
    s32 k;

    if (!sndIsLoaded()) {
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
            if (handle == snd->handle) {
                n = snd->numTracks;
                list = snd->tracks;
                for (k = gSnd->tracksPerSound; k-- != 0; list++) {
                    if (*list != 0xFF) {
                        if (prm != NULL) {
                            trkApply(&gSnd->tracks[*list], prm);
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

/* 0x801096FC - 1: not playing, 2: playing (one-shot), 3: playing (held) */
s32 sndHandleState(s32 handle) {
    s32 r;
    u8 left;
    u8 i;
    SndSound *snd;

    if (!sndIsLoaded()) {
        return 1;
    }
    r = 1;
    sndLock();
    left = gSnd->numSounds;
    snd = gSnd->sounds;
    if (left != 0) {
        for (i = gSndCfg.maxSounds; i-- != 0; snd++) {
            if (!(snd->flags & SND_ACTIVE)) {
                continue;
            }
            left--;
            if (handle == snd->handle) {
                if (snd->group == 0) {
                    r = 2;
                } else if (snd->group == 1) {
                    r = 3;
                }
            }
            if (left == 0) {
                break;
            }
        }
    }
    sndUnlock();
    return r;
}

/* 0x801097E8 - stop the sounds with a handle (fade: release over `ms`) */
void sndStopHandleEx(s32 handle, s32 fade, s32 ms) {
    u8 left;
    u8 i;
    SndSound *snd;

    if (!sndIsLoaded()) {
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
            left--;
            if (handle == snd->handle) {
                snd->flags |= SND_STOPPING;
                snd->flags &= ~SND_PAUSING;
                snd->flags &= ~SND_RESUMING;
            }
            if (left == 0) {
                break;
            }
        }
    }
    if (fade) {
        sndQueueOp(0xE);
        sndQueueArg(&ms, 4);
    } else {
        sndQueueOp(0xA);
    }
    sndQueueArg(&handle, 4);
    sndUnlock();
}

/* 0x801098F8 */
void sndStopHandle(s32 handle) {
    sndStopHandleEx(handle, 0, 0);
}

/* 0x8010991C */
void sndFadeHandle(s32 handle, s32 ms) {
    sndStopHandleEx(handle, 1, ms);
}

/* 0x80109940 - queued command 0xA */
void sndCmdStopHandle(void *p) {
    s32 handle;

    sndQueueGet(&handle, 4);
    sndDoStopHandle(handle, 0, 0);
}

/* 0x80109974 - queued command 0xE */
void sndCmdFadeHandle(void *p) {
    s32 handle;
    s32 ms;

    sndQueueGet(&ms, 4);
    sndQueueGet(&handle, 4);
    sndDoStopHandle(handle, 1, ms);
}

/* 0x801099B4 */
void sndDoStopHandle(s32 handle, s32 fade, s32 ms) {
    u8 left;
    u8 i;
    SndSound *snd;
    u8 *list;
    s32 n;
    s32 k;
    SndTrack *t;
    s32 saved;

    if (!sndIsLoaded()) {
        return;
    }
    sndLock();
    left = gSnd->numSounds;
    i = gSndCfg.maxSounds;
    snd = gSnd->sounds;
    if (left != 0) {
        if (fade) {
            saved = sndGetReleaseTime();
            sndSetReleaseTime(ms);
        }
        while (i-- != 0) {
            if (snd->flags & SND_ACTIVE) {
                if (handle == snd->handle && (snd->flags & SND_STOPPING)) {
                    n = snd->numTracks;
                    snd->flags &= ~SND_STOPPING;
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
                if (--left == 0) {
                    break;
                }
            }
            snd++;
        }
        if (fade) {
            sndSetReleaseTime(saved);
        }
    }
    sndUnlock();
}
