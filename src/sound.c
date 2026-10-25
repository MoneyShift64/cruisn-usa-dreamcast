/*
 * sound.c - the game's side of the sound engine: sound effects (each one a
 * short sequence, played on a channel per player so one car's sounds can be
 * changed or stopped without touching another's) and the music, which is
 * started at the next frame once the previous tune has stopped.
 * Original code: main segment, ROM 0x20260-0x209A0 (RAM 0x8011F260-0x8011F9A0).
 *
 * A sound's handle is (effect << 16) | channel; channels 0 and 1 are the two
 * players' own in a split-sound 2-player game, otherwise player + 3 (2 for
 * sounds belonging to no player).
 */
#include "common.h"
#include "audio.h"

extern s32 gNumPlayersM1;                /* 0x8015095C */
extern s32 gOpt2PSound;                  /* 0x80150998 - 2-player sound split left/right */
extern u8 *gMusicBuf;                    /* 0x801466E8 - where tunes are loaded */
extern SndParams gSfxParams;             /* 0x801625A0 */
extern s32 gMusicCurrent;                /* 0x801625B4 */
extern s32 gMusicPending;                /* 0x801625B8 */

s32 disableInterrupts(void);
void restoreInterrupts(s32 mask);
s32 sndSeqState(s32 seq);                /* 0x80107750 */
void sndPlay(s32 seq);                   /* 0x801076E0 */
void sndStop0(s32 seq);                  /* 0x8010794C */
void sndPlayHandleParams(s32 seq, s32 handle, SndParams *prm); /* 0x80109238 */
void sndSetParams(s32 handle, SndParams *prm);   /* 0x801094F0 */
s32 sndHandleState(s32 handle);          /* 0x801096FC */
void sndStopHandle(s32 handle);          /* 0x801098F8 */
void sndFadeHandle(s32 handle, s32 ms);  /* 0x8010991C */
s32 seqLoadIfNeeded(s32 seq, u8 *dst);   /* 0x80110CB4 */
s32 seqUnload(s32 seq);                  /* 0x80110D40 */
void sfxPlayVolume(s32 id, s32 player, f32 vol);
void fatalError(s32 code, char *msg);    /* 0x80104550 */

static s32 sfxChannel(s32 player) {
    if (gNumPlayersM1 == 0 || gOpt2PSound == 0 || player == -1) {
        return player + 3;
    }
    return player;
}

/* 0x8011F260 */
void musicReset(void) {
    s32 mask = disableInterrupts();

    gMusicCurrent = -1;
    gMusicPending = -1;
    restoreInterrupts(mask);
}

/* 0x8011F2A4 */
void sfxPlay(s32 id, s32 player) {
    sfxPlayVolume(id, player, 1.0f);
}

/* 0x8011F2C4 - in a split-sound 2-player game, player 1's sounds are on the
 * left and player 2's on the right */
void sfxPlayVolume(s32 id, s32 player, f32 vol) {
    s32 chan = player + 3;

    gSfxParams.mask = 3;
    gSfxParams.volume = (u32)(vol * 127.0f);
    if (gNumPlayersM1 == 0 || gOpt2PSound == 0 || player == -1) {
        gSfxParams.pan = 0x40;
    } else {
        gSfxParams.pan = player * 127;
        chan = player;
    }
    sndPlayHandleParams(id, (id << 16) | chan, &gSfxParams);
}

/* 0x8011F3E0 */
void sfxSetVolume(s32 id, s32 player, f32 vol) {
    gSfxParams.mask = 1;
    gSfxParams.volume = (u32)(vol * 127.0f);
    sndSetParams((id << 16) | sfxChannel(player), &gSfxParams);
}

/* 0x8011F4E8 - engine sounds: pitch (-1..1 of the bend range) and volume */
void sfxEngine(s32 id, s32 player, f32 pitch, f32 vol) {
    gSfxParams.mask = 9;
    gSfxParams.volume = (u32)(vol * 127.0f);
    gSfxParams.bend = pitch * 8191.0f;
    sndSetParams((id << 16) | sfxChannel(player), &gSfxParams);
}

/* 0x8011F614 */
s32 sfxState(s32 id, s32 player) {
    return sndHandleState((id << 16) | sfxChannel(player));
}

/* 0x8011F670 */
void sfxStop(s32 id, s32 player) {
    sndStopHandle((id << 16) | sfxChannel(player));
}

/* 0x8011F6CC */
void sfxFadeOut(s32 id, s32 player, s32 ms) {
    sndFadeHandle((id << 16) | sfxChannel(player), ms);
}

/* 0x8011F72C - an effect on every channel */
void sfxStopAll(s32 id) {
    id <<= 16;
    sndStopHandle(id);
    sndStopHandle(id | 1);
    sndStopHandle(id | 2);
    sndStopHandle(id | 3);
    sndStopHandle(id | 4);
}

/* 0x8011F780 - once a frame: start the requested tune when the last has stopped */
void musicUpdate(void) {
    if (gMusicPending < 0) {
        return;
    }
    if (gMusicCurrent >= 0 && sndSeqState(gMusicCurrent) == 3) {
        return;
    }
    if (gMusicCurrent >= 0) {
        seqUnload(gMusicCurrent);
    }
    seqLoadIfNeeded(gMusicPending, gMusicBuf);
    sndPlay(gMusicPending);
    gMusicCurrent = gMusicPending;
    gMusicPending = -1;
}

/* 0x8011F83C */
void musicPlay(s32 seq) {
    gMusicPending = -1;
    if (gMusicCurrent >= 0) {
        sndStop0(gMusicCurrent);
    }
    gMusicPending = seq;
}

/* 0x8011F894 */
void musicStop(void) {
    gMusicPending = -1;
    if (gMusicCurrent >= 0) {
        sndStop0(gMusicCurrent);
    }
}

/* 0x8011F8D8 - the tune playing or about to, -1 if none */
s32 musicCurrent(void) {
    s32 seq;

    if (gMusicPending >= 0) {
        return gMusicPending;
    }
    seq = gMusicCurrent;
    if (seq < 0) {
        return -1;
    }
    if (sndSeqState(seq) == 3) {
        return seq;
    }
    return -1;
}

/* 0x8011F940 - the engine's error hook */
void sndFatal(const char *msg, s32 n1, s32 n2) {
    char buf[200];

    sprintf(buf, "WESS ERROR\n%s\nn1=%i  n2=%i\n", msg, n1, n2);
    fatalError(0, buf);
}

extern f32 gVolume[2];                   /* 0x80150990 - music, sound effects (0..1) */
void sndSetSfxVolume(u8 vol);            /* 0x80108220 */
void sndSetMusicVolume(u8 vol);          /* 0x8010846C */

/* 0x8011E628 - the pause menu's volume sliders (channel 1: effects, with a
 * sample sound when `preview`) */
void volumeApply(s32 channel, s32 preview) {
    f32 v = gVolume[channel] * 127.0f;

    if (channel != 0) {
        sndSetSfxVolume((u32)v);
        if (preview) {
            sfxPlay(0x58, -1);
        }
    } else {
        sndSetMusicVolume((u32)v);
    }
}
