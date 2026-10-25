/*
 * sndsynth.c - the sound engine's "synth" handler: plays a track's notes on
 * the synthesizer's voices (key maps, envelopes, voice stealing, volume, pan
 * and pitch bend), and a recorder that can replay the notes a stop cut off.
 * Original code: main segment, ROM 0xF490-0x11428 (RAM 0x8010E490-0x80110428).
 *
 * As in sndctl.c, the original's static scratch variables are locals here.
 */
#include "common.h"
#include "audio.h"

/* notes cut off by a stop, kept to be restarted later */
typedef struct SndRecNote {
    /* 0x00 */ s16 seq;
    /* 0x02 */ s16 track;
    /* 0x04 */ u8  key;
    /* 0x05 */ u8  velocity;
    /* 0x06 */ s16 pad06;
    /* 0x08 */ SndKeymap *keymap;
    /* 0x0C */ SndWave *wave;
} SndRecNote;

typedef struct SndRecord {
    /* 0x00 */ s32 count;
    /* 0x04 */ s32 mask;                /* 1: music, 2: effects */
    /* 0x08 */ SndRecNote notes[32];
} SndRecord;

extern s32 gSndReleaseTime;              /* 0x80132918 - ms */
extern SndRecord *gSndRec;               /* 0x8013291C - recording when set */
extern f32 gSndPitchScale;               /* 0x80132920 - 22050 / output rate */
extern SndState *gSynState;              /* 0x8014B9B0 */
extern SndSound *gSynSounds;             /* 0x8014B9B4 */
extern SndTrack *gSynTracks;             /* 0x8014B9B8 */
extern SndNote *gSynNotes;               /* 0x8014B9BC */
extern s32 gSynNumNotes;                 /* 0x8014B9C0 */
extern SndBankInfo *gSynInfo;            /* 0x8014B9C4 */
extern u32 *gSynNow;                     /* 0x8014B9C8 */
extern SndInstrument *gSndInstruments;   /* 0x8014B9CC */
extern SndKeymap *gSndKeymaps;           /* 0x8014B9D0 */
extern SndWave *gSndWaves;               /* 0x8014B9D4 */
extern u8 *gSndBankTable4;               /* 0x8014B9D8 */
extern s16 *gSndLoopHeader;              /* 0x8014B9DC */
extern u8 *gSndRawLoops;                 /* 0x8014B9E0 - 0x10 each */
extern u8 *gSndAdpcmLoops;               /* 0x8014B9E4 - 0x30 each */
extern u8 *gSndBooks;                    /* 0x8014B9E8 - 0x108 each */
extern u8 gSndNoRawLoop[0x10];           /* 0x8014B9F0 */
extern u8 gSndNoAdpcmLoop[0x30];         /* 0x8014BA00 */
extern SndRecord gSndRecData;            /* 0x8014BA30 */
extern s32 gSndSampleBase;               /* 0x8014BC38 - ROM address of the samples */
extern s32 gSndNumWaves;                 /* 0x8014BC5C */

void synNoteStart(SndNote *n, SndTrack *t, SndKeymap *km, SndWave *w, u8 key, u8 vel);
void synNoteFree(SndNote *n);
void synNoteRelease(SndNote *n, s32 ms);
void synNoteReleaseKm(SndNote *n);
void synNoteDecay(SndNote *n);
void synNoteAlloc(SndTrack *t, SndKeymap *km, SndWave *w, u8 key, u8 vel);
void synReleaseNotes(SndTrack *t);

#define VOICE(n) (&gSndVoices[(n)->voice])

/* 0x8010E490 */
void sndSetOutputRate(u32 rate) {
    gSndPitchScale = 22050.0f / (f32)rate;
}

/* 0x8010E4C8 */
void sndSetSampleBase(s32 rom) {
    gSndSampleBase = rom;
}

/* 0x8010E4D4 - start recording the notes that stops cut off */
void sndRecordStart(s32 mask) {
    gSndRec = &gSndRecData;
    if (gSndRec != NULL) {
        gSndRecData.count = 0;
        gSndRec->mask = mask;
    }
}

/* 0x8010E504 */
void sndRecordStop(void) {
    gSndRec = NULL;
}

/* 0x8010E510 - restart the recorded notes of one track of a sequence */
void sndRecordReplay(s16 seq, s32 track, SndTrack *t) {
    s32 i;
    SndRecNote *r;

    for (i = 0, r = gSndRecData.notes; i < gSndRecData.count; i++, r++) {
        if ((track & 0xFF) == r->track && seq == r->seq && r->keymap != NULL) {
            synNoteAlloc(t, r->keymap, r->wave, r->key, r->velocity);
            r->keymap = NULL;
        }
    }
}

/* 0x8010E5E8 */
void sndRecordNote(SndTrack *t, s16 seq, s16 track, u8 key, u8 vel, SndKeymap *km, SndWave *w) {
    SndRecNote *r;

    if (gSndRec == NULL) {
        return;
    }
    if (!((t->group == 1 && (gSndRec->mask & 1)) || (t->group == 0 && (gSndRec->mask & 2)))) {
        return;
    }
    if (gSndRec->count < 0x20) {
        r = &gSndRec->notes[gSndRec->count];
        r->seq = seq;
        r->track = track;
        r->key = key;
        r->velocity = vel;
        r->keymap = km;
        r->wave = w;
        gSndRec->count++;
    }
}

/* 0x8010E724 */
void sndSetReleaseTime(s32 ms) {
    gSndReleaseTime = ms;
}

/* 0x8010E730 */
s32 sndGetReleaseTime(void) {
    return gSndReleaseTime;
}

/* 0x8010E740 - pitch ratio for a number of cents */
f32 sndPitch(s32 cents) {
    f32 r = 1.0f;
    f32 f;

    if (cents >= 0) {
        f = 1.0005778f;                  /* 2^(1/1200) */
    } else {
        f = 0.99942255f;
        cents = -cents;
    }
    while (cents != 0) {
        if (cents & 1) {
            r *= f;
        }
        f *= f;
        cents >>= 1;
    }
    return r * gSndPitchScale;
}

static u32 noteVolume(SndNote *n, SndTrack *t) {
    if (t->group == 0) {
        return (u32)(n->velocity * n->keymap->volume * t->volume * (gSndSfxVolume + 5)) >> 14;
    }
    return (u32)(n->velocity * n->keymap->volume * t->volume * (gSndMusicVolume + 5)) >> 14;
}

static s32 bendCents(SndKeymap *km, s16 bend) {
    if (bend == 0) {
        return 0;
    }
    if (bend > 0) {
        return km->bendUp * bend * 0.0122;
    }
    return km->bendDown * bend * 0.0122;
}

static s16 notePan(SndTrack *t, SndKeymap *km) {
    s16 pan = t->pan + km->pan - 0x40;

    if (pan >= 0x80) {
        pan = 0x7F;
    }
    if (pan < 0) {
        pan = 0;
    }
    return pan;
}

/* 0x8010E7A4 - start a note's voice: the attack */
void synVoiceStart(SndNote *n) {
    SndTrack *t = &gSynTracks[n->track];
    SndKeymap *km = n->keymap;
    s16 pan;
    u32 vol;
    s32 cents;
    f32 pitch;
    ALVoiceConfig cfg;

    if (gSndStereo) {
        pan = notePan(t, km);
    } else {
        pan = 0x40;
    }
    vol = noteVolume(n, t);
    vol = (km->attackVolume * vol) >> 7;
    cents = bendCents(km, t->bend);
    cfg.priority = 100;
    cfg.fxBus = 0;
    cfg.unityPitch = 0;
    alSynAllocVoice(gSndSynth, VOICE(n), &cfg);
    km = n->keymap;
    pitch = sndPitch(n->wave->tune + cents + (n->key - km->keyBase) * 100 - km->detune);
    alSynStartVoiceParams(gSndSynth, VOICE(n), (ALWaveTable *)n->wave, pitch, vol, pan, t->priority,
                          n->keymap->attackTime * 1000);
}

/* 0x8010EAD8 - hook 0: find the bank's tables and fix up the wave tables */
void synInit(SndState *st) {
    SndWave *w;
    s32 i;

    gSynState = st;
    gSynNow = st->now;
    gSynSounds = st->sounds;
    gSynTracks = st->tracks;
    gSynNotes = st->notes;
    gSynInfo = st->info;
    gSynNumNotes = gSndCfg.maxVoices;
    gSndInstruments = (SndInstrument *)gSynInfo->data;
    gSndKeymaps = (SndKeymap *)(((u32)(gSndInstruments + gSynInfo->numInstruments) + 7) & ~7);
    gSndWaves = (SndWave *)(((u32)(gSndKeymaps + gSynInfo->numKeymaps) + 7) & ~7);
    gSndBankTable4 = (u8 *)(((u32)(gSndWaves + gSynInfo->numWaves) + 7) & ~7);
    gSndLoopHeader = (s16 *)(((u32)(gSndBankTable4 + gSynInfo->numUnk10 * 4) + 7) & ~7);
    gSndRawLoops = (u8 *)gSndLoopHeader + 8;
    gSndAdpcmLoops = gSndRawLoops + gSndLoopHeader[1] * 0x10;
    gSndBooks = gSndAdpcmLoops + gSndLoopHeader[2] * 0x30;
    gSndNumWaves = 0;
    for (i = 0; i < gSynInfo->numWaves; i++) {
        w = &gSndWaves[i];
        w->base += gSndSampleBase;
        w->flags = 1;
        w->tune = (s32)w->loop;
        if (w->type == WAVE_RAW16) {
            if ((s32)w->book != -1) {
                w->loop = gSndRawLoops + (s32)w->book * 0x10;
            } else {
                w->loop = gSndNoRawLoop;
            }
        } else {
            if ((s32)w->book != -1) {
                w->loop = gSndAdpcmLoops + (s32)w->book * 0x30;
            } else {
                w->loop = gSndNoAdpcmLoop;
            }
            w->book = gSndBooks + i * 0x108;
        }
        gSndNumWaves = i + 1;
    }
}

/* 0x8010ED9C */
void synHook1(void *p) {
}

/* 0x8010EDA4 - hook 2, every tick: envelopes */
void synTick(void *p) {
    s32 left = gSynState->numNotes;
    SndNote *n;
    s32 i;
    u8 flags;

    if (left == 0) {
        return;
    }
    n = gSynNotes;
    for (i = gSynNumNotes; i-- != 0; n++) {
        flags = n->flags;
        if (!(flags & NOTE_ACTIVE)) {
            continue;
        }
        if ((flags & NOTE_RELEASE) && n->time < *gSynNow) {
            synNoteFree(n);
        } else if ((flags & NOTE_ATTACK) && n->time + n->keymap->attackTime < *gSynNow) {
            synNoteDecay(n);
        }
        if (--left == 0) {
            return;
        }
    }
}

/* 0x8010EEDC */
void synHook3(void *p) {
}

/* 0x8010EEE4 */
void synHook4(void *p) {
}

/* 0x8010EEEC - hook 5: the track has ended; it is freed once its notes are */
void synTrackEnd(SndTrack *t) {
    SndSound *snd = &gSynSounds[t->sound];

    synReleaseNotes(t);
    if (t->numNotes != 0) {
        t->flags |= TRK_ENDED;
        if (--snd->numPlaying == 0) {
            snd->group = 0;
        }
    } else {
        ctlTrackEnd(t);
    }
}

/* 0x8010EF84 - hook 6: release all the track's notes */
void synReleaseNotes(SndTrack *t) {
    s32 left = t->numNotes;
    SndNote *n;
    s32 i;
    u8 flags;

    if (left == 0) {
        return;
    }
    n = gSynNotes;
    for (i = gSynNumNotes; i-- != 0; n++) {
        flags = n->flags;
        if (!(flags & NOTE_ACTIVE) || t->index != n->track) {
            continue;
        }
        if (gSndRec != NULL && !(flags & NOTE_RELEASE) && n->held == 0) {
            sndRecordNote(t, gSynSounds[t->sound].seq, n->track, n->key, n->velocity, n->keymap, n->wave);
        }
        synNoteRelease(n, gSndReleaseTime);
        if (--left == 0) {
            return;
        }
    }
}

/* 0x8010F128 - command 7: instrument */
void synInstrument(SndTrack *t) {
    t->instrument = t->pc[1] | (t->pc[2] << 8);
}

/* 0x8010F14C */
void synCmd8(void *p) {
}

/* 0x8010F154 - command 9: pitch bend */
void synBend(SndTrack *t) {
    s16 bend = t->pc[1] | (t->pc[2] << 8);
    s32 left;
    SndNote *n;
    s32 i;
    s32 cents;
    SndKeymap *km;

    if (bend == t->bend) {
        return;
    }
    t->bend = bend;
    left = t->numNotes;
    if (left == 0) {
        return;
    }
    n = gSynNotes;
    for (i = gSynNumNotes; i-- != 0; n++) {
        if (!(n->flags & NOTE_ACTIVE) || t->index != n->track) {
            continue;
        }
        km = n->keymap;
        cents = bendCents(km, t->bend);
        alSynSetPitch(gSndSynth, VOICE(n),
                      sndPitch(n->wave->tune + cents + (n->key - km->keyBase) * 100 - km->detune));
        if (--left == 0) {
            return;
        }
    }
}

/* 0x8010F3F8 */
void synCmdA(void *p) {
}

/* 0x8010F400 */
void synCmdB(void *p) {
}

/* 0x8010F408 - command 0xC: volume */
void synVolume(SndTrack *t) {
    u8 v = t->pc[1];
    s32 left;
    SndNote *n;
    s32 i;

    if (v == t->volume) {
        return;
    }
    t->volume = v;
    left = t->numNotes;
    if (left == 0) {
        return;
    }
    n = gSynNotes;
    for (i = gSynNumNotes; i-- != 0; n++) {
        if (!(n->flags & NOTE_ACTIVE) || t->index != n->track) {
            continue;
        }
        alSynSetVol(gSndSynth, VOICE(n), noteVolume(n, t), 1000);
        if (--left == 0) {
            return;
        }
    }
}

/* 0x8010F63C - command 0xD: pan */
void synPan(SndTrack *t) {
    u8 v = t->pc[1];
    s32 left;
    SndNote *n;
    s32 i;

    if (v == t->pan) {
        return;
    }
    t->pan = v;
    if (!gSndStereo) {
        return;
    }
    left = t->numNotes;
    if (left == 0) {
        return;
    }
    n = gSynNotes;
    for (i = gSynNumNotes; i-- != 0; n++) {
        if (!(n->flags & NOTE_ACTIVE) || t->index != n->track) {
            continue;
        }
        alSynSetPan(gSndSynth, VOICE(n), notePan(t, n->keymap));
        if (--left == 0) {
            return;
        }
    }
}

/* 0x8010F7E0 - command 0xE: sustain pedal (0 = up: release the held notes) */
void synPedal(SndTrack *t) {
    u8 left;
    u8 i;
    SndNote *n;

    if (t->pc[1] != 0) {
        t->flags &= ~TRK_SUSTAIN;
        return;
    }
    t->flags |= TRK_SUSTAIN;
    left = t->numNotes;
    if (left == 0) {
        return;
    }
    n = gSynNotes;
    for (i = gSynNumNotes; i-- != 0; n++) {
        if ((n->flags & NOTE_ACTIVE) && !(n->flags & NOTE_RELEASE) && t->index == n->track && n->held) {
            n->held = 0;
            synNoteReleaseKm(n);
            if (--left == 0) {
                return;
            }
        }
    }
}

/* 0x8010F918 - command 0xF: effect setting (sent with the pan call) */
void synFx(SndTrack *t) {
    u8 v = t->pc[1];
    s32 left;
    SndNote *n;
    s32 i;

    if (v == t->priority) {
        return;
    }
    t->priority = v;
    left = t->numNotes;
    if (left == 0) {
        return;
    }
    n = gSynNotes;
    for (i = gSynNumNotes; i-- != 0; n++) {
        if (!(n->flags & NOTE_ACTIVE) || t->index != n->track) {
            continue;
        }
        alSynSetPan(gSndSynth, VOICE(n), v);
        if (--left == 0) {
            return;
        }
    }
}

/* 0x8010FA6C */
void synCmd10(void *p) {
}

/* 0x8010FA74 */
void synNoteStart(SndNote *n, SndTrack *t, SndKeymap *km, SndWave *w, u8 key, u8 vel) {
    n->flags |= NOTE_ACTIVE;
    n->flags &= ~NOTE_RELEASE;
    n->flags |= NOTE_ATTACK;
    n->track = t->index;
    n->priority = km->priority;
    n->key = key;
    n->held = 0;
    n->keymap = km;
    n->wave = w;
    n->velocity = vel;
    n->time = *gSynNow;
    t->numNotes++;
    gSynState->numNotes++;
    synVoiceStart(n);
}

/* 0x8010FB24 */
void synNoteFree(SndNote *n) {
    SndTrack *t;

    alSynStopVoice(gSndSynth, VOICE(n));
    alSynFreeVoice(gSndSynth, VOICE(n));
    gSynState->numNotes--;
    t = &gSynTracks[n->track];
    if (--t->numNotes == 0 && (t->flags & TRK_ENDED) && !(t->flags & TRK_40)) {
        ctlTrackEnd(t);
    }
    n->flags &= ~(NOTE_ACTIVE | NOTE_RELEASE | NOTE_ATTACK);
}

/* 0x8010FC38 */
void synNoteRelease(SndNote *n, s32 ms) {
    alSynSetPriority(gSndSynth, VOICE(n), 0);
    alSynSetVol(gSndSynth, VOICE(n), 0, ms * 1000);
    n->flags |= NOTE_RELEASE;
    n->flags &= ~NOTE_ATTACK;
    n->time = *gSynNow + ms;
}

/* 0x8010FCFC */
void synNoteReleaseKm(SndNote *n) {
    alSynSetPriority(gSndSynth, VOICE(n), 0);
    alSynSetVol(gSndSynth, VOICE(n), 0, n->keymap->releaseTime * 1000);
    n->flags |= NOTE_RELEASE;
    n->flags &= ~NOTE_ATTACK;
    n->time = *gSynNow + n->keymap->releaseTime;
}

/* 0x8010FDC4 - end of the attack: on to the decay level */
void synNoteDecay(SndNote *n) {
    SndTrack *t = &gSynTracks[n->track];
    u32 vol = noteVolume(n, t);

    vol = (n->keymap->decayVolume * vol) >> 7;
    alSynSetVol(gSndSynth, VOICE(n), vol, n->keymap->decayTime * 1000);
    n->flags &= ~NOTE_ATTACK;
}

/* 0x8010FF2C - find a voice for a note, taking one from a lower or equal
 * priority note if they're all busy (releasing ones first, then the oldest) */
void synNoteAlloc(SndTrack *t, SndKeymap *km, SndWave *w, u8 key, u8 vel) {
    u32 oldest = 0xFFFFFFFF;
    s32 minPri = 0x100;
    SndNote *best = NULL;
    s32 found = 0;
    SndNote *n = gSynNotes;
    s32 i;
    u8 flags;
    s32 pri;

    for (i = gSynNumNotes; i-- != 0; n++) {
        flags = n->flags;
        if (!(flags & NOTE_ACTIVE)) {
            synNoteStart(n, t, km, w, key, vel);
            return;
        }
        pri = n->priority;
        if (km->priority < pri) {
            continue;
        }
        if (pri < minPri) {
            oldest = n->time;
            found = 1;
            minPri = pri;
            best = n;
        } else if (flags & NOTE_RELEASE) {
            if (!(best->flags & NOTE_RELEASE) || n->time < oldest) {
                found = 1;
                minPri = pri;
                oldest = n->time;
                best = n;
            }
        } else if (!(best->flags & NOTE_RELEASE) && n->time < oldest) {
            found = 1;
            minPri = pri;
            oldest = n->time;
            best = n;
        }
    }
    if (found) {
        synNoteFree(best);
        synNoteStart(best, t, km, w, key, vel);
    }
}

/* 0x80110168 - command 0x11: note on - every key map of the instrument that
 * covers the key plays */
void synNoteOn(SndTrack *t) {
    u8 key = t->pc[1];
    u8 vel = t->pc[2];
    SndInstrument *inst = &gSndInstruments[t->instrument];
    u8 i = inst->numKeymaps;
    s32 k = 0;
    SndKeymap *km;

    while (i-- != 0) {
        km = &gSndKeymaps[inst->keymap + k];
        if (key >= km->keyMin && km->keyMax >= key) {
            synNoteAlloc(t, km, &gSndWaves[km->wave], key, vel);
        }
        k++;
    }
}

/* 0x801102E8 - command 0x12: note off (held while the pedal is down) */
void synNoteOff(SndTrack *t) {
    SndNote *n = gSynNotes;
    s32 i;

    for (i = gSynNumNotes; i-- != 0; n++) {
        if ((n->flags & NOTE_ACTIVE) && !(n->flags & NOTE_RELEASE) && t->pc[1] == n->key && t->index == n->track) {
            if (t->flags & TRK_SUSTAIN) {
                synNoteReleaseKm(n);
            } else {
                n->held = 1;
            }
        }
    }
}
