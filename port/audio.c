/*
 * audio.c - the port's replacement for the N64 audio library and the game's
 * audio manager (main segment 0x801053A0-0x80106210): the decompiled sound
 * engine (src/audio) drives voices through the alSyn* calls below, and a
 * small software mixer turns them into 16-bit stereo at 22050 Hz, running
 * the engine's 120 Hz tick as it goes.
 *
 * Samples are played straight from SOUND.BIN (tools/port_sound.py): VADPCM
 * decoded on the fly, or raw big-endian 16-bit.
 */
#include "common.h"
#include "audio.h"
#include "port.h"

#define OUT_RATE    22050
#define ROM_SOUND   0x5C40E0             /* ROM address of SOUND.BIN's first byte */
#define ROM_SAMPLES 0x5D2BE0
#define ROM_SSEQ    0x72AD60
#define ROM_BANK    0x5C40E0
#define MAX_VOICES  32
#define TICK_Q      735                  /* samples per engine tick, x4 (22050 / 120 = 183.75) */

static u8 *gSound;
static u32 gSoundSize;

/* ------------------------------------------------------------------ */
/* ROM reads (0x801053D4)                                             */
/* ------------------------------------------------------------------ */
s32 dmaRomRead(s32 rom, u8 *dst, s32 len) {
    u32 off = (u32)rom - ROM_SOUND;
    s32 i;

    if (len == 0) {
        return 0;
    }
    if ((u32)rom < ROM_SOUND || off + len > gSoundSize) {
        port_log("dmaRomRead: ROM %08x size %x not in SOUND.BIN\n", (u32)rom, len);
        return 0;
    }
    for (i = 0; i < len; i++) {
        dst[i] = gSound[off + i];
    }
    return len;
}

/* 0x801053A0 */
s32 sndMemset(u8 *p, s32 c, s32 n) {
    while (n-- != 0) {
        *p++ = c;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Voices                                                             */
/* ------------------------------------------------------------------ */
typedef struct PVoice {
    s32 alloc;
    s32 playing;
    SndWave *wave;
    u8 *data;                            /* the wave's bytes */
    s32 adpcm;
    s32 order, npred;
    s16 *book;
    u32 loopStart, loopEnd, loopCount;
    s16 *loopState;                      /* ADPCM: decoder history at the loop start */
    u32 len;                             /* in samples */
    u32 pos;                             /* integer sample position */
    u32 frac;                            /* 16.16 fraction */
    u32 step;                            /* 16.16 */
    s32 frame;                           /* ADPCM frame decoded into dec[] (-1: none) */
    s32 hist[16];                        /* the previous frame's output (decoder history) */
    s16 dec[16];
    s32 vol, volTarget, volStep, volLeft; /* 16.16 volume 0..0x7FFF */
    s32 pan;
    /* played by the sound chip (gHwVoices) */
    s32 hwWave;                          /* its samples in sound memory (gHwWave[]), -1: none */
    s32 hwVol, hwPan;                    /* what the chip has (-1: not sent yet) */
    u32 hwFreq;
    s32 rFrom, rTo;                      /* a volume ramp, 0..0x7FFF, from ms rStart to rEnd */
    u32 rStart, rEnd;
    s32 ramping;
} PVoice;

s32 gHwVoices;                           /* 1: the voices play on the sound chip, nothing is mixed here */
static u32 gHwMs;                        /* when the voice changes being made take effect (the chip's clock) */
static void hw_start(PVoice *p, f32 pitch, s32 vol, s32 us);
static void hw_set_vol(PVoice *p, s32 vol, s32 us);
static void hw_set_pitch(PVoice *p);
static void hw_set_pan(PVoice *p);
static void hw_stop(PVoice *p);

static PVoice gVoices[MAX_VOICES];
static ALVoice gVoiceObjs[MAX_VOICES];
static u8 gSynthObj[0x100];
static s32 gNumVoices;
s32 gAudioDebug;                         /* log this many voice starts */

/* equal power pan: 32767 * cos(i/127 * pi/2), i = 0..127 */
static const u16 gPanCos[128] = {
    32767, 32765, 32757, 32745, 32727, 32705, 32678, 32646, 32609, 32567, 32520, 32468, 32412, 32350, 32284, 32213,
    32137, 32056, 31970, 31880, 31785, 31685, 31580, 31470, 31356, 31237, 31113, 30985, 30852, 30714, 30572, 30425,
    30273, 30117, 29956, 29791, 29621, 29447, 29268, 29085, 28897, 28705, 28509, 28308, 28103, 27893, 27680, 27462,
    27240, 27014, 26783, 26549, 26310, 26067, 25821, 25570, 25315, 25057, 24794, 24528, 24258, 23984, 23706, 23425,
    23140, 22852, 22560, 22264, 21965, 21663, 21357, 21048, 20735, 20420, 20101, 19779, 19454, 19126, 18795, 18461,
    18124, 17785, 17442, 17097, 16749, 16399, 16046, 15690, 15332, 14972, 14609, 14244, 13877, 13508, 13136, 12763,
    12387, 12010, 11631, 11250, 10867, 10483, 10097, 9709, 9320, 8930, 8538, 8145, 7751, 7355, 6959, 6561,
    6163, 5763, 5363, 4962, 4561, 4158, 3756, 3352, 2949, 2545, 2141, 1736, 1331, 926, 521, 117,
};

static PVoice *pv(ALVoice *v) {
    s32 i = v - gVoiceObjs;

    if (i < 0 || i >= MAX_VOICES) {
        port_fatal("audio: bad voice %p\n", v);
    }
    return &gVoices[i];
}

static void set_ramp(PVoice *p, s32 target, s32 us) {
    s32 n = (s32)(((s64)us * OUT_RATE) / 1000000);

    p->volTarget = target << 16;
    if (n <= 0) {
        p->vol = p->volTarget;
        p->volLeft = 0;
        return;
    }
    p->volStep = (p->volTarget - p->vol) / n;
    p->volLeft = n;
}

static u32 pitch_step(f32 pitch) {
    if (pitch <= 0.0f) {
        return 0;
    }
    if (pitch > 16.0f) {
        pitch = 16.0f;
    }
    return (u32)(pitch * 65536.0f);
}

s32 alSynAllocVoice(ALSynth *s, ALVoice *v, ALVoiceConfig *vc) {
    PVoice *p = pv(v);

    p->alloc = 1;
    p->playing = 0;
    return 1;
}

void alSynStartVoiceParams(ALSynth *s, ALVoice *v, ALWaveTable *wt, f32 pitch, s16 vol, ALPan pan, u8 fxmix,
                           ALMicroTime t) {
    PVoice *p = pv(v);
    SndWave *w = (SndWave *)wt;
    u32 off = (u32)w->base - ROM_SOUND;
    s32 i;

    if ((u32)w->base < ROM_SAMPLES || off + w->len > gSoundSize) {
        port_log("audio: wave %p base %x outside the samples\n", w, (u32)w->base);
        p->playing = 0;
        return;
    }
    p->wave = w;
    p->data = gSound + off;
    p->adpcm = (w->type == WAVE_ADPCM);
    if (p->adpcm) {
        ALADPCMloop *l = (ALADPCMloop *)w->loop;
        ALADPCMBook *b = (ALADPCMBook *)w->book;

        p->order = b->order;
        p->npred = b->npredictors;
        p->book = b->book;
        p->len = (w->len / 9) * 16;
        p->loopStart = l->start;
        p->loopEnd = l->end;
        p->loopCount = l->count;
        p->loopState = (s16 *)l->state;
    } else {
        ALRawLoop *l = (ALRawLoop *)w->loop;

        p->len = w->len / 2;
        p->loopStart = l->start;
        p->loopEnd = l->end;
        p->loopCount = l->count;
        p->loopState = NULL;
    }
    if (p->loopEnd > p->len) {
        p->loopEnd = p->len;
    }
    if (p->order < 1 || p->order > 8) {
        p->order = 2;
    }
    p->pos = 0;
    p->frac = 0;
    p->step = pitch_step(pitch);
    p->frame = -1;
    for (i = 0; i < 16; i++) {
        p->hist[i] = 0;
    }
    p->vol = 0;
    set_ramp(p, vol, t);
    p->pan = pan & 0x7F;
    p->playing = 1;
    if (gHwVoices) {
        hw_start(p, pitch, vol, t);
    }
    if (gAudioDebug && gAudioDebug-- > 0) {
        port_log("voice %d: wave %x len %d vol %d pan %d pitch %d/1000 t %d loop %d-%d x%d\n", (int)(p - gVoices),
                 (u32)w->base - ROM_SAMPLES, (int)p->len, (int)vol, (int)pan, (int)(pitch * 1000.0f), (int)t,
                 (int)p->loopStart, (int)p->loopEnd, (int)p->loopCount);
    }
}

void alSynSetPitch(ALSynth *s, ALVoice *v, f32 pitch) {
    pv(v)->step = pitch_step(pitch);
    if (gHwVoices) {
        hw_set_pitch(pv(v));
    }
}

void alSynSetVol(ALSynth *s, ALVoice *v, s16 vol, ALMicroTime t) {
    PVoice *p = pv(v);

    if (vol < 0) {
        vol = 0;
    }
    if (gHwVoices) {
        hw_set_vol(p, vol, t);
    }
    set_ramp(p, vol, t);
}

void alSynSetPan(ALSynth *s, ALVoice *v, ALPan pan) {
    pv(v)->pan = pan & 0x7F;
    if (gHwVoices) {
        hw_set_pan(pv(v));
    }
}

void alSynStopVoice(ALSynth *s, ALVoice *v) {
    if (gHwVoices) {
        hw_stop(pv(v));
    }
    pv(v)->playing = 0;
}

void alSynFreeVoice(ALSynth *s, ALVoice *v) {
    PVoice *p = pv(v);

    if (gHwVoices) {
        hw_stop(p);
    }
    p->playing = 0;
    p->alloc = 0;
}

void alSynSetPriority(ALSynth *s, ALVoice *v, s16 priority) {
}

/* ------------------------------------------------------------------ */
/* Decoding                                                           */
/* ------------------------------------------------------------------ */
static s32 clamp16(s32 v) {
    if (v > 32767) {
        return 32767;
    }
    if (v < -32768) {
        return -32768;
    }
    return v;
}

s32 gStDecHit, gStDecMiss;

/* one VADPCM frame (9 bytes -> 16 samples), as the N64 SDK's decoder */
static void adpcm_frame(PVoice *p, s32 f) {
    u8 *in = p->data + f * 9;
    s32 scale = 1 << (in[0] >> 4);
    s32 pred = in[0] & 0xF;
    s32 order = p->order;
    s16 *bk;
    s32 ix[16];
    s32 vec[8 + 8];
    s32 out[16];
    s32 i, j, k, h;

    if (pred >= p->npred) {
        pred = 0;
    }
    bk = p->book + pred * order * 8;     /* book[pred][order][8] */
    for (i = 0; i < 8; i++) {
        s32 c = in[1 + i];
        s32 hi = c >> 4, lo = c & 0xF;
        if (hi >= 8) hi -= 16;
        if (lo >= 8) lo -= 16;
        ix[i * 2] = hi * scale;
        ix[i * 2 + 1] = lo * scale;
    }
    if (order == 2) {
        /* the usual case, written out: the same sums, without the general loops */
        const s16 *b0 = bk, *b1 = bk + 8;
        s32 p2 = p->hist[14], p1 = p->hist[15];
        gStDecMiss++;
        for (h = 0; h < 2; h++) {
            const s32 *x = ix + h * 8;
            s32 *o = out + h * 8;
            for (i = 0; i < 8; i++) {
                s32 acc = b0[i] * p2 + b1[i] * p1 + (x[i] << 11);
                for (k = 0; k < i; k++) {
                    acc += b1[i - 1 - k] * x[k];
                }
                acc >>= 11;
                o[i] = acc > 32767 ? 32767 : acc < -32768 ? -32768 : acc;
            }
            p2 = o[6];
            p1 = o[7];
        }
        for (i = 0; i < 16; i++) {
            p->hist[i] = out[i];
            p->dec[i] = out[i];
        }
        p->frame = f;
        return;
    }
    for (h = 0; h < 2; h++) {
        for (i = 0; i < order; i++) {
            vec[i] = (h == 0) ? p->hist[16 - order + i] : out[8 - order + i];
        }
        for (i = 0; i < 8; i++) {
            vec[order + i] = ix[h * 8 + i];
        }
        for (i = 0; i < 8; i++) {
            /* coefficients of output i: book[pred][0..order-1][i], then the
             * implied impulse response of the inputs 0..i */
            s32 acc = 0;
            for (j = 0; j < order; j++) {
                acc += bk[j * 8 + i] * vec[j];
            }
            acc += vec[order + i] << 11;
            for (k = 0; k < i; k++) {
                /* table[i][order + 1 + k'] = table[i - 1 - k'][order], i.e. the
                 * last book row's coefficient for output i-1-k' */
                s32 kk = i - 1 - k;
                acc += bk[(order - 1) * 8 + kk] * vec[order + k];
            }
            {
                s32 d = acc >> 11;           /* floor division */
                out[h * 8 + i] = clamp16(d);
            }
        }
    }
    for (i = 0; i < 16; i++) {
        p->hist[i] = out[i];
        p->dec[i] = out[i];
    }
    p->frame = f;
}

/* the sample at the voice's position; 0 past the end */
static s32 voice_sample(PVoice *p) {
    u32 s = p->pos;
    s32 f;

    if (s >= p->len) {
        return 0;
    }
    if (!p->adpcm) {
        u8 *q = p->data + s * 2;
        return (s16)((q[0] << 8) | q[1]);
    }
    f = s >> 4;
    if (f != p->frame) {
        if (p->frame >= 0 && f > p->frame + 1 && f - p->frame < 64) {
            while (p->frame < f - 1) {   /* skipped frames still feed the history */
                adpcm_frame(p, p->frame + 1);
            }
        } else if (f != p->frame + 1) {
            /* a jump (loop): the history is the loop's saved state if we're
             * going to its frame, else what we had */
            if (p->loopState != NULL && f == (s32)(p->loopStart >> 4)) {
                s32 i;
                for (i = 0; i < 16; i++) {
                    p->hist[i] = p->loopState[i];
                }
            }
        }
        adpcm_frame(p, f);
    }
    return p->dec[s & 15];
}

static void voice_advance(PVoice *p) {
    p->frac += p->step;
    p->pos += p->frac >> 16;
    p->frac &= 0xFFFF;
    if (p->loopCount != 0 && p->loopEnd > p->loopStart && p->pos >= p->loopEnd) {
        p->pos = p->loopStart + (p->pos - p->loopEnd);
        if (p->loopCount != 0xFFFFFFFF) {
            p->loopCount--;
        }
        if (p->adpcm) {
            p->frame = -2;               /* force the loop-state history */
        }
    }
}

/* ------------------------------------------------------------------ */
/* Mixing                                                             */
/* ------------------------------------------------------------------ */
s32 sndTick(void);                       /* 0x8010C198 */
static s32 gTickPhase;                   /* in quarter samples */
static s32 gAudioOn;

/* Mixed in blocks: from one engine tick to the next nothing changes a
 * voice's settings, so each voice is run through the whole block in one tight
 * loop (its state in registers) instead of every voice for every sample. The
 * result is the same, sample for sample. */
#define MIX_BLOCK 256
static s32 gMixL[MIX_BLOCK], gMixR[MIX_BLOCK];

static void mix_voice(PVoice *p, s32 n) {
    /* (everything the loop uses in locals: the compiler can't keep fields of
     * *p in registers while it stores to the mix buffers) */
    s32 *__restrict ml = gMixL, *__restrict mr = gMixR;
    s32 pl = gPanCos[p->pan], pr = gPanCos[127 - p->pan];
    s32 vol = p->vol, volStep = p->volStep, volLeft = p->volLeft, volTarget = p->volTarget;
    u32 pos = p->pos, frac = p->frac, step = p->step, len = p->len;
    u32 loopStart = p->loopStart, loopEnd = p->loopEnd, loopCount = p->loopCount;
    s32 loops = loopCount != 0 && loopEnd > loopStart;
    s32 adpcm = p->adpcm;
    s32 frame = p->frame;
    const s16 *dec = p->dec;
    const u8 *data = p->data;
    s32 i;

    for (i = 0; i < n; i++) {
        s32 s;

        if (volLeft > 0) {
            vol += volStep;
            if (--volLeft == 0) {
                vol = volTarget;
            }
        }
        if (pos >= len) {
            continue;
        }
        if (!adpcm) {
            const u8 *q = data + pos * 2;
            s = (s16)((q[0] << 8) | q[1]);
        } else if ((s32)(pos >> 4) == frame) {
            s = dec[pos & 15];
        } else {
            p->pos = pos;
            p->frame = frame;
            s = voice_sample(p);
            frame = p->frame;
        }
        s = (s * (vol >> 16)) >> 15;
        ml[i] += (s * pl) >> 15;
        mr[i] += (s * pr) >> 15;
        frac += step;
        pos += frac >> 16;
        frac &= 0xFFFF;
        if (loops && pos >= loopEnd) {
            pos = loopStart + (pos - loopEnd);
            if (loopCount != 0xFFFFFFFF) {
                loopCount--;
                loops = loopCount != 0;
            }
            if (adpcm) {
                frame = -2;                  /* force the loop-state history */
            }
        }
    }
    p->vol = vol;
    p->volLeft = volLeft;
    p->pos = pos;
    p->frac = frac;
    p->loopCount = loopCount;
    p->frame = frame;
}

void port_audio_mix(s16 *out, u32 frames) {
    u32 n = 0;
    s32 i, k, len;

    while (n < frames) {
        if (!gAudioOn) {
            out[n * 2] = out[n * 2 + 1] = 0;
            n++;
            continue;
        }
        /* the engine ticks before the sample that takes the phase to 0 or below */
        if (gTickPhase <= 4) {
            gTickPhase += TICK_Q - 4;
            sndTick();
            len = 1 + (gTickPhase - 1) / 4;
            gTickPhase -= 4 * (len - 1);
        } else {
            len = (gTickPhase - 1) / 4;
            gTickPhase -= 4 * len;
        }
        if (len > (s32)(frames - n)) {
            gTickPhase += 4 * (len - (s32)(frames - n));
            len = frames - n;
        }
        if (len > MIX_BLOCK) {
            gTickPhase += 4 * (len - MIX_BLOCK);
            len = MIX_BLOCK;
        }
        for (k = 0; k < len; k++) {
            gMixL[k] = gMixR[k] = 0;
        }
        for (i = 0; i < gNumVoices; i++) {
            if (gVoices[i].playing) {
                mix_voice(&gVoices[i], len);
            }
        }
        for (k = 0; k < len; k++, n++) {
            out[n * 2] = clamp16((gMixL[k] * 3) >> 2);     /* a little headroom: many voices at once */
            out[n * 2 + 1] = clamp16((gMixR[k] * 3) >> 2);
        }
    }
}

/* ------------------------------------------------------------------ */
/* The sound chip's voices                                            */
/* ------------------------------------------------------------------ */
/* On the Dreamcast the voices can play on the sound chip itself (the AICA:
 * 64 channels with their own pitch, volume and pan): each sample is decoded
 * to 16-bit once, the first time it is played, and kept in sound memory, and
 * the engine's voice changes become commands to the chip, stamped with the
 * time they are for. The engine runs a little ahead of the chip's clock, as
 * the mixer ran ahead of the stream. (port/host/host.c imitates the chip.) */
#define HW_LEAD_MS 60                    /* the engine runs this far ahead of the chip (the old stream: up to 93) */
#define HW_CATCHUP_MS 150                /* after a longer stop (loading) the missed ticks are dropped */
#define HW_WAVES 256

typedef struct HwWave {
    SndWave *w;
    u32 addr;                            /* in sound memory */
    u32 end;                             /* samples kept: the length, or the loop end for a looping one */
    s32 users;
    u32 lastUse;
} HwWave;
static HwWave gHwWave[HW_WAVES];
static s32 gHwWaveN;
static u32 gHwUse;
static u64 gHwTickQ;                     /* the next engine tick, in quarter samples at OUT_RATE */
static s32 gHwStarted;
s32 gStHwCmds, gStHwDecodes;

/* the chip's pan for ours (equal power, 0..127) in its 3 dB steps, and the
 * volume scale that keeps the louder side as loud as the mixer had it
 * (x 3/4 headroom): volume (0..255) = (v * gHwGain[pan]) >> 15 */
static const u8 gHwPan[128] = {
    7, 23, 39, 47, 55, 63, 63, 71, 71, 79, 79, 79, 79, 87, 87, 87, 87, 95, 95, 95, 95, 95, 95, 95, 103, 103, 103,
    103, 103, 103, 103, 103, 103, 111, 111, 111, 111, 111, 111, 111, 111, 111, 111, 111, 119, 119, 119, 119, 119,
    119, 119, 119, 119, 119, 119, 119, 119, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
    136, 136, 136, 136, 136, 136, 136, 136, 136, 136, 136, 136, 136, 144, 144, 144, 144, 144, 144, 144, 144, 144,
    144, 144, 152, 152, 152, 152, 152, 152, 152, 152, 152, 160, 160, 160, 160, 160, 160, 160, 168, 168, 168, 168,
    176, 176, 176, 176, 184, 184, 192, 192, 200, 208, 216, 232, 248,
};
static const u8 gHwGain[128] = {
    192, 192, 192, 192, 192, 192, 191, 191, 191, 191, 191, 190, 190, 190, 189, 189, 188, 188, 187, 187, 186, 186,
    185, 184, 184, 183, 182, 181, 181, 180, 179, 178, 177, 176, 175, 174, 173, 172, 171, 170, 169, 168, 167, 165,
    164, 163, 162, 160, 159, 158, 156, 155, 154, 152, 151, 149, 148, 146, 145, 143, 142, 140, 138, 137, 137, 138,
    140, 142, 143, 145, 146, 148, 149, 151, 152, 154, 155, 156, 158, 159, 160, 162, 163, 164, 165, 167, 168, 169,
    170, 171, 172, 173, 174, 175, 176, 177, 178, 179, 180, 181, 181, 182, 183, 184, 184, 185, 186, 186, 187, 187,
    188, 188, 189, 189, 190, 190, 190, 191, 191, 191, 191, 191, 192, 192, 192, 192, 192, 192,
};

static u32 hw_q_ms(u64 q) {
    return (u32)(q * 1000 / (OUT_RATE * 4));
}

static s32 hw_loops(const PVoice *p) {
    return p->loopCount != 0 && p->loopEnd > p->loopStart;
}

/* the voice's samples in sound memory (decoded the first time): its index, or -1 */
static s32 hw_wave(PVoice *p) {
    static s16 buf[2048];
    HwWave *h;
    PVoice d;
    u32 end, n, i, k, at;
    s32 slot = -1;

    for (i = 0; i < (u32)gHwWaveN; i++) {
        if (gHwWave[i].w == p->wave && gHwWave[i].addr != 0) {
            gHwWave[i].lastUse = ++gHwUse;
            return i;
        }
        if (gHwWave[i].addr == 0 && slot < 0) {
            slot = i;
        }
    }
    end = hw_loops(p) ? p->loopEnd : p->len;
    if (end > 65535) {
        end = 65535;                     /* (the chip counts samples in 16 bits) */
    }
    if (end == 0) {
        return -1;
    }
    if (slot < 0) {
        if (gHwWaveN == HW_WAVES) {
            return -1;
        }
        slot = gHwWaveN++;
    }
    h = &gHwWave[slot];
    h->addr = hw_snd_alloc(end * 2);
    while (h->addr == 0) {               /* sound memory full: drop the least recently used one not playing */
        s32 old = -1;
        for (i = 0; i < (u32)gHwWaveN; i++) {
            if (gHwWave[i].addr != 0 && gHwWave[i].users == 0 && (old < 0 || gHwWave[i].lastUse < gHwWave[old].lastUse)) {
                old = i;
            }
        }
        if (old < 0) {
            return -1;
        }
        hw_snd_free(gHwWave[old].addr);
        gHwWave[old].addr = 0;
        gHwWave[old].w = NULL;
        h->addr = hw_snd_alloc(end * 2);
    }
    h->w = p->wave;
    h->end = end;
    h->users = 0;
    h->lastUse = ++gHwUse;
    gStHwDecodes++;
    /* decode it as the mixer would play it (straight through, from silence) */
    d = *p;
    d.frame = -1;
    for (i = 0; i < 16; i++) {
        d.hist[i] = 0;
    }
    for (at = 0; at < end; at += n) {
        n = end - at;
        if (n > 2048) {
            n = 2048;
        }
        for (k = 0; k < n; k++) {
            d.pos = at + k;
            buf[k] = (s16)voice_sample(&d);
        }
        hw_snd_write(h->addr + at * 2, buf, n * 2);
    }
    return slot;
}

static s32 hw_vol8(s32 v, s32 pan) {
    return (v * gHwGain[pan]) >> 15;
}

static u32 hw_freq(const PVoice *p) {
    return (u32)(((u64)p->step * OUT_RATE) >> 16);
}

/* the voice's volume (0..0x7FFF) at ms, on its ramp */
static s32 hw_vol_at(const PVoice *p, u32 ms) {
    if (!p->ramping || ms >= p->rEnd) {
        return p->rTo;
    }
    if (ms <= p->rStart) {
        return p->rFrom;
    }
    return p->rFrom + (s32)(((s64)(p->rTo - p->rFrom) * (s32)(ms - p->rStart)) / (s32)(p->rEnd - p->rStart));
}

static void hw_send_vol(PVoice *p, s32 v) {
    s32 v8 = hw_vol8(v, p->pan), pan8 = gHwPan[p->pan];
    s32 what = (v8 != p->hwVol ? 2 : 0) | (pan8 != p->hwPan ? 4 : 0);
    if (what != 0 && p->hwWave >= 0) {
        hw_snd_set(p - gVoices, what, 0, v8, pan8, gHwMs);
        gStHwCmds++;
        p->hwVol = v8;
        p->hwPan = pan8;
    }
}

static void hw_start(PVoice *p, f32 pitch, s32 vol, s32 us) {
    HwWave *h;
    s32 v0;

    if (p->hwWave >= 0) {                /* (it was still playing something) */
        gHwWave[p->hwWave].users--;
        p->hwWave = -1;
    }
    p->hwWave = hw_wave(p);
    if (p->hwWave < 0) {
        return;
    }
    h = &gHwWave[p->hwWave];
    h->users++;
    /* the attack: from 0 to vol over us */
    p->rFrom = 0;
    p->rTo = vol;
    p->rStart = gHwMs;
    p->rEnd = gHwMs + (u32)us / 1000;
    p->ramping = us >= 2000;
    v0 = p->ramping ? 0 : vol;
    p->hwVol = hw_vol8(v0, p->pan);
    p->hwPan = gHwPan[p->pan];
    p->hwFreq = hw_freq(p);
    hw_snd_start(p - gVoices, h->addr, hw_loops(p), p->loopStart, hw_loops(p) ? h->end : h->end, p->hwFreq,
                 p->hwVol, p->hwPan, gHwMs);
    gStHwCmds++;
}

static void hw_set_vol(PVoice *p, s32 vol, s32 us) {
    p->rFrom = hw_vol_at(p, gHwMs);
    p->rTo = vol;
    p->rStart = gHwMs;
    p->rEnd = gHwMs + (u32)us / 1000;
    p->ramping = us >= 2000;
    if (!p->ramping) {
        hw_send_vol(p, vol);
    }
}

static void hw_set_pitch(PVoice *p) {
    u32 f = hw_freq(p);
    if (f != p->hwFreq && p->hwWave >= 0 && p->playing) {
        hw_snd_set(p - gVoices, 1, f, 0, 0, gHwMs);
        gStHwCmds++;
        p->hwFreq = f;
    }
}

static void hw_set_pan(PVoice *p) {
    if (p->playing) {
        hw_send_vol(p, hw_vol_at(p, gHwMs));
    }
}

static void hw_stop(PVoice *p) {
    if (p->hwWave >= 0) {
        if (p->playing) {
            hw_snd_stop(p - gVoices, gHwMs);
            gStHwCmds++;
        }
        gHwWave[p->hwWave].users--;
        p->hwWave = -1;
    }
    p->ramping = 0;
}

/* the volume ramps, at each tick */
static void hw_ramps(void) {
    s32 i;
    for (i = 0; i < gNumVoices; i++) {
        PVoice *p = &gVoices[i];
        if (p->playing && p->ramping) {
            s32 v = hw_vol_at(p, gHwMs);
            if (gHwMs >= p->rEnd) {
                p->ramping = 0;
            }
            hw_send_vol(p, v);
        }
    }
}

/* the engine's ticks up to a little past the chip's clock now */
void port_audio_hw_run(u32 nowMs) {
    u32 target = nowMs + HW_LEAD_MS;

    if (!gHwStarted) {
        gHwTickQ = (u64)nowMs * (OUT_RATE * 4) / 1000;
        gHwStarted = 1;
    }
    if (hw_q_ms(gHwTickQ) + HW_CATCHUP_MS < nowMs) {
        gHwTickQ = (u64)(nowMs - HW_CATCHUP_MS) * (OUT_RATE * 4) / 1000;
    }
    while (hw_q_ms(gHwTickQ) < target) {
        gHwMs = hw_q_ms(gHwTickQ);
        if (gHwMs == 0) {
            gHwMs = 1;                   /* (0 means "now" to the chip) */
        }
        if (gAudioOn) {
            sndTick();
            hw_ramps();
        }
        gHwTickQ += TICK_Q;
    }
}

/* ------------------------------------------------------------------ */
/* Start-up (the boot overlay's sound set-up, ROM 0x3E54C)            */
/* ------------------------------------------------------------------ */
extern s32 gSndInited;
extern f32 gVolume[2];                   /* 0x80150990 - music, effects */
extern u8 *gMusicBuf;                    /* 0x801466E8 */
void sndConfigSet(SndConfig *c);
s32 sndMemSize(s32 rom);
s32 sndLoad(s32 rom, u8 *buf, s32 size);
s32 sndInit(void);
void sndSetErrorFunc(void (*f)(const char *, s32, s32, s32));
void sndFatal(const char *msg, s32 n1, s32 n2);
void sndSetSampleBase(s32 rom);
void sndSetOutputRate(u32 rate);
void sndSetSfxVolume(u8 vol);
void sndSetMusicVolume(u8 vol);
s32 seqTableSize(SndState *st, s32 rom);
s32 seqInit(SndState *st, s32 rom, s32 keepOpen, u8 *dst);
s32 seqRangeSize(s32 seq, s32 n);
s32 seqRangeLoad(s32 seq, s32 n, u8 *dst);

static u8 *gHeap;
static u32 gHeapLeft;

static u8 *heap_alloc(u32 n) {
    u8 *p = gHeap;
    u32 i;

    n = (n + 15) & ~15;
    if (n > gHeapLeft) {
        port_fatal("audio: out of heap (%x wanted, %x left)\n", n, gHeapLeft);
    }
    for (i = 0; i < n; i++) {
        p[i] = 0;
    }
    gHeap += n;
    gHeapLeft -= n;
    return p;
}

void port_sound_init(void) {
    SndConfig cfg;
    s32 size;
    u8 *buf;
    u32 i;

    gSound = plat_load_file("SOUND.BIN", &gSoundSize);
    if (gSound == NULL) {
        port_log("audio: SOUND.BIN not found, no sound\n");
        return;
    }
    /* the audio heap: the top 0x41400 bytes of RDRAM, as on the N64 */
    gHeap = PORT_PTR(PORT_PHYS(0x3BEC00));
    gHeapLeft = 0x41400;

    sndSetErrorFunc((void (*)(const char *, s32, s32, s32))sndFatal);
    for (i = 0; i < sizeof(cfg); i++) {
        ((u8 *)&cfg)[i] = 0;
    }
    cfg.mask = 0x143;
    cfg.s.numDmaBufs = 0x28;
    cfg.s.numDmaMsgs = 0x50;
    cfg.s.maxUpdates = 0x38;
    cfg.s.maxTracks = 0x22;
    sndConfigSet(&cfg);

    /* the audio manager's part (0x801054BC) */
    sndSetSampleBase(ROM_SAMPLES);
    sndSetOutputRate(OUT_RATE);
    gNumVoices = gSndCfg.maxVoices;
    if (gNumVoices > MAX_VOICES) {
        gNumVoices = MAX_VOICES;
    }
    gSndVoices = gVoiceObjs;
    gSndSynth = (ALSynth *)gSynthObj;
    sndInit();

    size = sndMemSize(ROM_BANK);
    buf = heap_alloc(size);
    sndLoad(ROM_BANK, buf, size);
    size = seqTableSize(sndGetState(), ROM_SSEQ);
    buf = heap_alloc(size);
    seqInit(sndGetState(), ROM_SSEQ, 0, buf);
    size = seqRangeSize(0, 0x9F);        /* the effects stay loaded */
    buf = heap_alloc(size);
    seqRangeLoad(0, 0x9F, buf);
    gMusicBuf = heap_alloc(0x7148);      /* one tune at a time */
    port_log("audio: bank %s, %d sequences, %x bytes of heap left\n", sndIsLoaded() ? "loaded" : "NOT loaded",
             sndNumSeqs(), gHeapLeft);

    sndSetMusicVolume((u8)(s32)(gVolume[0] * 127.0f));
    sndSetSfxVolume((u8)(s32)(gVolume[1] * 127.0f));
    gTickPhase = TICK_Q;
    for (i = 0; i < MAX_VOICES; i++) {
        gVoices[i].hwWave = -1;
    }
    gAudioOn = 1;
}
