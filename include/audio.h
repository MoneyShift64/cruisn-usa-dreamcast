#ifndef AUDIO_H
#define AUDIO_H
/*
 * The game's sound engine (main segment, RAM 0x801053A0-0x80111110).
 *
 * Everything that makes a noise - sound effects and music alike - is a small
 * "sequence": a list of byte-code tracks with MIDI-like note on/off, volume,
 * pan, pitch bend, loops and jumps, separated by delta times. A playing
 * sequence is a SndSound; each of its tracks is a SndTrack; a track's notes
 * are played by SndNotes, each one owning a voice of the N64 audio library
 * synthesizer (alSyn*). The sound bank ("SN64" in ROM at 0x5C40E0) holds the
 * sequence table, instruments, key maps and wave tables; the samples
 * themselves (VADPCM) follow it in ROM at 0x5D2BE0. Music sequences come
 * from a second file ("SSEQ" at 0x72AD60).
 *
 * Calls from the game don't change the engine state directly: they add a
 * command (an opcode byte plus argument bytes) to a queue that the engine
 * runs at the start of its next tick. The engine ticks 120 times a second,
 * driven by the synthesizer's player callback.
 *
 * Track flags (byte 0 of SndTrack; the code also tests them as the top byte
 * of a word, e.g. 0x80000000 = TRK_ACTIVE):
 */
#define TRK_ACTIVE   0x80
#define TRK_40       0x40   /* volume follows the sound's mask bits */
#define TRK_PAUSED   0x20
#define TRK_ENDED    0x10   /* reached its end (still releasing notes) */
#define TRK_TIMED    0x08   /* stops at `stopTime` */
#define TRK_LOOPS    0x04   /* restarts from the top when it ends */
#define TRK_JUMPED   0x02   /* program counter was moved by a command */
#define TRK_SUSTAIN  0x01   /* set: note-offs release at once (pedal up); clear: notes are held */

/* Sound flags (byte 0 of SndSound) */
#define SND_ACTIVE   0x80
#define SND_HELD     0x40   /* a held sound (sndheld.c): stays allocated when it ends */
#define SND_PAUSING  0x20   /* a pause is queued */
#define SND_RESUMING 0x10   /* a resume is queued */
#define SND_STOPPING 0x08

/* Note flags (byte 0 of SndNote) */
#define NOTE_ACTIVE  0x80
#define NOTE_RELEASE 0x40   /* fading out; freed at `time` */
#define NOTE_ATTACK  0x20   /* attack segment; decay starts after it */

/* ------------------------------------------------------------------ */
/* Sound bank file (big-endian in ROM)                                */
/* ------------------------------------------------------------------ */
typedef struct SndFileHeader {
    /* 0x00 */ u32 magic;               /* 'SN64' */
    /* 0x04 */ u32 version;             /* 2 */
    /* 0x08 */ u32 unk08;
    /* 0x0C */ u32 numSeqs;             /* sequences in the bank */
    /* 0x10 */ u8  packed;              /* nonzero: data is compressed (never in this game) */
    /* 0x11 */ u8  pad11[3];
    /* 0x14 */ u32 unk14;
    /* 0x18 */ u32 dataSize;            /* bytes of data after the two headers */
    /* 0x1C */ u32 unk1C;
    /* 0x20 */ struct SndSeqDef *seqs;  /* set at load: the sequence table */
    /* 0x24 */ u32 unk24;
} SndFileHeader;

/* Second header of the bank: counts of the synth tables (the sizes of the
 * tables' entries are stored next to them) */
typedef struct SndBankInfo {
    /* 0x00 */ s32 unk00;
    /* 0x04 */ s16 numInstruments;  s16 instSize;     /* 4 */
    /* 0x08 */ s16 numKeymaps;      s16 keymapSize;   /* 0x14 */
    /* 0x0C */ s16 numWaves;        s16 waveSize;     /* 0x18 */
    /* 0x10 */ s16 numUnk10;        s16 unk12;
    /* 0x14 */ s32 unk14;
    /* 0x18 */ u8 *data;            /* set at load: the bank data in RAM */
} SndBankInfo;

/* One track of a sequence definition */
typedef struct SndTrackDef {
    /* 0x00 */ u8  group;               /* 0 = effects, 1 = music (master volumes) */
    /* 0x01 */ u8  priority;
    /* 0x02 */ s16 instrument;
    /* 0x04 */ s16 bend;
    /* 0x06 */ u8  volume;
    /* 0x07 */ u8  pan;
    /* 0x08 */ u8  unk08;
    /* 0x09 */ u8  volMask;
    /* 0x0A */ s16 ppq;                 /* ticks per beat */
    /* 0x0C */ u16 tempo;               /* beats per minute */
    /* 0x0E */ s16 numLabels;
    /* 0x10 */ s32 scriptSize;
} SndTrackDef;

typedef struct SndTrackRef {
    /* 0x00 */ SndTrackDef *def;
    /* 0x04 */ s32 *labels;             /* offsets of the jump targets from `script` */
    /* 0x08 */ u8  *script;
} SndTrackRef;

/* A sequence: one entry of the sequence table (from the SSEQ file). Its
 * data is loaded on demand: `tracks` is NULL until then. */
typedef struct SndSeqDef {
    /* 0x00 */ u16 numTracks;
    /* 0x02 */ u8  packed;              /* nonzero: compressed (sndUnpack) */
    /* 0x03 */ u8  pad03;
    /* 0x04 */ s32 size;                /* bytes of data */
    /* 0x08 */ s32 offset;              /* in the file, after the table */
    /* 0x0C */ SndTrackRef *tracks;
} SndSeqDef;

/* Optional overrides for a sequence's track settings (sfxPlay etc. pass one) */
typedef struct SndParams {
    /* 0x00 */ u32 mask;                /* which of the fields below to use */
    /* 0x04 */ u8  volume;              /* 0x001 */
    /* 0x05 */ u8  pan;                 /* 0x002 */
    /* 0x06 */ s16 instrument;          /* 0x004 */
    /* 0x08 */ s16 bend;                /* 0x008 */
    /* 0x0A */ u8  volBit;              /* 0x010: bit of the tracks' volMask to test */
    /* 0x0B */ u8  priority;            /* 0x100 */
    /* 0x0C */ u16 tempo;               /* 0x020 */
    /* 0x0E */ s16 pad0E;
    /* 0x10 */ s32 duration;            /* 0x040: stop after this many ticks */
} SndParams;                            /* 0x080: loop */

/* ------------------------------------------------------------------ */
/* Engine state                                                       */
/* ------------------------------------------------------------------ */
typedef struct SndSound {
    /* 0x00 */ u8  flags;
    /* 0x01 */ u8  group;               /* 0 = one-shot, 1 = can be held/paused */
    /* 0x02 */ s16 seq;
    /* 0x04 */ u8  numTracks;           /* tracks still active */
    /* 0x05 */ u8  numPlaying;          /* tracks not yet ended */
    /* 0x06 */ u8  volume;
    /* 0x07 */ u8  pan;
    /* 0x08 */ s32 handle;              /* the caller's id for it */
    /* 0x0C */ u8 *tracks;              /* track numbers, 0xFF = none */
    /* 0x10 */ u8 *regsA;               /* loop counters (commands 0x14/0x16) */
    /* 0x14 */ u8 *regsB;
} SndSound;

typedef struct SndTrack {
    /* 0x00 */ u8  flags;
    /* 0x01 */ u8  index;
    /* 0x02 */ u8  sound;               /* index of its SndSound */
    /* 0x03 */ u8  group;
    /* 0x04 */ s16 instrument;
    /* 0x06 */ s16 bend;
    /* 0x08 */ u32 wait;                /* delta time to the next command */
    /* 0x0C */ u8  priority;
    /* 0x0D */ u8  volume;
    /* 0x0E */ u8  pan;
    /* 0x0F */ u8  volMask;
    /* 0x10 */ u8  handler;             /* index into gSndHandlers */
    /* 0x11 */ u8  numNotes;            /* notes playing */
    /* 0x12 */ s16 pad12;
    /* 0x14 */ s16 ppq;
    /* 0x16 */ u16 tempo;
    /* 0x18 */ s16 numLabels;
    /* 0x1A */ s16 numLabelsDef;
    /* 0x1C */ u32 rate;                /* time advance per tick, 16.16 */
    /* 0x20 */ u32 frac;
    /* 0x24 */ u32 elapsed;             /* since the last command */
    /* 0x28 */ u32 time;                /* since the start */
    /* 0x2C */ u32 stopTime;
    /* 0x30 */ u8 *script;
    /* 0x34 */ u8 *pc;
    /* 0x38 */ s32 *labels;
    /* 0x3C */ u8 **stack;              /* call stack (commands 0x1A/0x1B, 0x1D/0x1E) */
    /* 0x40 */ u8 **sp;
    /* 0x44 */ u8 **stackEnd;
    /* 0x48 */ s32 unk48;
    /* 0x4C */ s32 unk4C;
} SndTrack;

typedef struct SndKeymap {
    /* 0x00 */ u8  priority;
    /* 0x01 */ u8  volume;
    /* 0x02 */ s8  pan;                 /* added to the track's pan */
    /* 0x03 */ u8  pad03;
    /* 0x04 */ u8  keyBase;
    /* 0x05 */ s8  detune;              /* cents */
    /* 0x06 */ u8  keyMin;
    /* 0x07 */ u8  keyMax;
    /* 0x08 */ s8  bendDown;            /* semitones */
    /* 0x09 */ s8  bendUp;
    /* 0x0A */ s16 wave;
    /* 0x0C */ u16 attackTime;          /* ms */
    /* 0x0E */ u16 decayTime;
    /* 0x10 */ u16 releaseTime;
    /* 0x12 */ u8  attackVolume;
    /* 0x13 */ u8  decayVolume;
} SndKeymap;

/* An instrument: a range of key maps */
typedef struct SndInstrument {
    /* 0x00 */ u8  numKeymaps;
    /* 0x01 */ u8  pad01;
    /* 0x02 */ s16 keymap;              /* the first */
} SndInstrument;

/* the N64 library's wave table (ALWaveTable) plus a fine tuning. In the file
 * 0x0C is the tuning and 0x10 the loop number; loading turns them into the
 * loop and ADPCM book pointers and moves the tuning to 0x14. */
typedef struct SndWave {
    /* 0x00 */ u8  *base;               /* ROM address of the samples */
    /* 0x04 */ s32  len;
    /* 0x08 */ u8   type;               /* 0 = ADPCM, 1 = raw 16-bit */
    /* 0x09 */ u8   flags;              /* set at load */
    /* 0x0A */ s16  pad0A;
    /* 0x0C */ void *loop;
    /* 0x10 */ void *book;
    /* 0x14 */ s32  tune;               /* cents */
} SndWave;
#define WAVE_ADPCM 0
#define WAVE_RAW16 1

/* The engine drives the N64 audio library's synthesizer (alSyn*, voices are
 * ALVoices); the port supplies its own implementation of those calls. */
s32 alSynAllocVoice(ALSynth *s, ALVoice *v, ALVoiceConfig *vc);  /* 0x80123E08 */

typedef struct SndNote {
    /* 0x00 */ u8  flags;
    /* 0x01 */ u8  pad01;
    /* 0x02 */ u8  voice;               /* index of its synthesizer voice */
    /* 0x03 */ u8  track;
    /* 0x04 */ u8  priority;
    /* 0x05 */ u8  key;
    /* 0x06 */ u8  velocity;
    /* 0x07 */ u8  held;                /* note-off came while the pedal was down */
    /* 0x08 */ SndKeymap *keymap;
    /* 0x0C */ SndWave *wave;
    /* 0x10 */ u32 time;
} SndNote;

struct SndState;
typedef struct SndTimer {
    /* 0x00 */ u8  active;
    /* 0x01 */ u8  id;
    /* 0x02 */ s16 value;
    /* 0x04 */ void (*func)(s32 id, s32 value, struct SndState *st, s32 slot);
} SndTimer;

typedef struct SndState {
    /* 0x00 */ u32 *now;                /* the engine's clock (ms) */
    /* 0x04 */ u8  numSounds;
    /* 0x05 */ u8  numTracks;
    /* 0x06 */ u8  numNotes;
    /* 0x07 */ u8  maxNotes;
    /* 0x08 */ u8  pad08;
    /* 0x09 */ u8  numTimers;
    /* 0x0A */ u8  tracksPerSound;
    /* 0x0B */ u8  stackDepth;
    /* 0x0C */ SndFileHeader *hdr;
    /* 0x10 */ SndTimer *timers;
    /* 0x14 */ SndBankInfo *info;
    /* 0x18 */ SndSound *sounds;
    /* 0x1C */ SndTrack *tracks;
    /* 0x20 */ SndNote *notes;
} SndState;

/* A handler is a table of functions indexed by command number: entries 0-6
 * are hooks (0 init, 1 shutdown, 2 tick, 5 track ended, 6 release notes),
 * 7-0x12 the per-handler commands (instrument, bend, volume, pan, pedal,
 * notes...), 0x13-0x23 the flow-control commands (only in the control
 * handler's table, which every track uses for them). */
typedef void (*SndFunc)(void *);

/* ------------------------------------------------------------------ */
/* Engine configuration (initialised data at 0x80132718)              */
/* ------------------------------------------------------------------ */
typedef struct SndSettings {
    /* 0x00 */ s32 numDmaBufs;          /* sample DMA buffers (24) */
    /* 0x04 */ s32 numDmaMsgs;          /* (32) */
    /* 0x08 */ s32 dmaBufSize;          /* (0x800) */
    /* 0x0C */ s32 extraSamples;        /* (0x50) */
    /* 0x10 */ s32 frameLag;            /* frames before an unused DMA buffer is reused (1) */
    /* 0x14 */ s32 maxVoices;           /* synthesizer voices (24) */
    /* 0x18 */ s32 maxUpdates;          /* (48) */
    /* 0x1C */ u32 maxSounds;           /* (26) */
    /* 0x20 */ u32 maxTracks;           /* (25) */
    /* 0x24 */ u32 regsA;               /* loop counters per sound */
    /* 0x28 */ u32 regsB;
    /* 0x2C */ u32 maxTimers;
    /* 0x30 */ u32 tracksPerSound;      /* (16) */
    /* 0x34 */ u32 stackDepth;
} SndSettings;
typedef struct SndConfig {
    /* 0x00 */ s32 mask;                /* sndConfigSet: which settings to take */
    /* 0x04 */ SndSettings s;
} SndConfig;
extern SndSettings gSndCfg;              /* 0x80132718 */

typedef struct SndFile {
    /* 0x00 */ s32 start;                /* ROM address */
    /* 0x04 */ s32 pos;
} SndFile;

extern SndState *gSnd;           /* 0x8014948C */
extern u8 gSndSfxVolume;                 /* 0x8013290C - master volumes, 0-127 */
extern u8 gSndMusicVolume;               /* 0x80132910 */
extern u8 gSndStereo;                    /* 0x80132914 */
extern ALSynth *gSndSynth;               /* 0x8013AD40 - the synthesizer driver */
extern ALVoice *gSndVoices;              /* 0x801493A0 */
extern SndFunc *gSndHandlers[10];       /* 0x801326F0 - 0 control, 1 synth */
extern SndFunc gSndCtlFuncs[36];        /* 0x80132810 */
extern SndFunc gSndSynthFuncs[19];      /* 0x801328C0 */
extern u8 gSndCmdLen[36];               /* 0x801327C0 - bytes of each command */

/* ------------------------------------------------------------------ */
/* Engine functions                                                   */
/* ------------------------------------------------------------------ */
/* snddrv.c */
SndState *sndGetState(void);             /* 0x80106504 */
s32  sndIsInited(void);                  /* 0x80106514 */
s32  sndIsLoaded(void);                  /* 0x80106538 */
s32  sndSeqValid(s32 seq);               /* 0x8010655C */
void sndNotify(s32 code);                /* 0x80106490 */
s32  sndStartSeq(SndSeqDef *def, s32 seq, s32 handle, s32 held, SndParams *prm); /* 0x801073FC */
void resetCleanup(void);                 /* 0x80107CF8 */
void sndPlayHandle(s32 seq, s32 handle); /* 0x801091F0 */
s32  sndNumSeqs(void);                   /* 0x8011043C */
u8  *sndReadDelta(u8 *p, u32 *out);      /* 0x8010CCC0 */
void sndSetReleaseTime(s32 t);           /* 0x8010E724 */
void ctlTrackEnd(SndTrack *t);           /* 0x8010CE5C */
s32  sndGetReleaseTime(void);            /* 0x8010E730 */
/* sndpause.c */
void sndDoPause(s32 seq, s32 release);   /* 0x80108888 */
void sndDoResume(s32 seq);               /* 0x80108B3C */
void sndDoPauseAll(s32 release, s32 recMask); /* 0x80108D9C */
void sndDoResumeAll(s32 replay);         /* 0x8010905C */
/* sndhandle.c */
void sndDoSetParams(s32 handle, SndParams *prm); /* 0x80109578 */
void sndDoStopHandle(s32 handle, s32 fade, s32 ms); /* 0x801099B4 */
/* sndheld.c */
void trkGetParams(SndTrack *t, SndParams *prm); /* 0x8010B248 */
/* sndcustom.c */
void sndDoHeldNoteOn(s32 id, s32 k, u8 key, u8 vel); /* 0x8010B4A8 */
void sndDoHeldCmd(s32 id, s32 k, s32 v, void (*func)(SndTrack *, s32)); /* 0x8010B9E8 */
/* sndutil.c */
SndFile *sndFileOpen(s32 rom);           /* 0x8010C24C */
s32  sndFileRead(u8 *dst, s32 len, SndFile *f); /* 0x8010C260 */
s32  sndFileSeek(SndFile *f, s32 off, s32 whence); /* 0x8010C2B4 */
void sndFileClose(SndFile *f);           /* 0x8010C2FC */
void sndLock(void);                      /* 0x8010C09C */
void sndUnlock(void);                    /* 0x8010C0DC */
s32  sndTicksPerSecond(void);            /* 0x8010C11C */
u32  sndRate(s16 tps, s16 ppq, s16 tempo); /* 0x8010C124 */
void sndRunQueue(void);                  /* 0x8010CB78 */
void sndQueueOp(u8 op);                  /* 0x8010CA30 */
void sndQueueArg(void *src, s32 n);      /* 0x8010CA88 */
void sndQueueGet(void *dst, s32 n);      /* 0x8010CB34 */
void sndError(const char *msg, s32 a, s32 b, s32 c); /* 0x8010BFDC */
void *sndAlloc(s32 size);                /* 0x8010C06C */
void sndFree(void *p);                   /* 0x8010C078 */
void sndStop(void);                      /* 0x8010C080 */
void sndStart(void);                     /* 0x8010C08C */
void sndOpenHook(void);                  /* 0x8010C05C */
void sndCloseHook(void);                 /* 0x8010C064 */
void sndTickInstalled(void);             /* 0x8010C21C */
void sndTickRemoved(void);               /* 0x8010C234 */
s32  sndUnpack(s32 type, s32 a, s32 b, s32 c, s32 d); /* 0x8010C014 */
void sndTracksTick(void);                /* 0x8010E0EC */

#endif
