/*
 * sndseq.c - the sequence file ("SSEQ" in ROM at 0x72AD60): its table of
 * sequences, and loading sequences' data into RAM on demand.
 * Original code: main segment, ROM 0x113F0-0x11F60 and 0x11F60-0x12110
 * (RAM 0x801103F0-0x80111110).
 */
#include "common.h"
#include "audio.h"

typedef void (*SndSeqErrorFunc)(s32 arg, s32 code);

/* the file's header */
typedef struct SndSeqHeader {
    /* 0x00 */ u32 magic;               /* 'SSEQ' */
    /* 0x04 */ u32 unk04;
    /* 0x08 */ u32 unk08;
    /* 0x0C */ u16 unk0C;
    /* 0x0E */ u16 numSeqs;
    /* 0x10 */ u8  packed;
    /* 0x11 */ u8  pad11[3];
    /* 0x14 */ s32 packedSize;
    /* 0x18 */ s32 tableSize;
    /* 0x1C */ u32 unk1C;
} SndSeqHeader;

extern SndSeqHeader gSeqHeader;          /* 0x8014BD20 */
extern SndState *gSeqState;              /* 0x8014BD40 */
extern s32 gSeqRom;                      /* 0x8014BD44 */
extern s32 gSndNumSeqs;                  /* 0x8014BD48 */
extern s32 gSeqOpenCount;                /* 0x8014BD4C */
extern SndSeqErrorFunc gSeqErrorFunc;    /* 0x8014BD50 */
extern s32 gSeqErrorArg;                 /* 0x8014BD54 */
extern SndFile *gSeqFile;                /* 0x8014BD58 */
extern s32 gSeqDataStart;                /* 0x8014BD5C - file offset of the sequences' data */
extern s32 gSeqReady;                    /* 0x8014BD60 */

s32 sndUnpack(s32 type, s32 a, s32 b, s32 c, s32 d);

/* errors: 1 = can't open, 2 = short read, 3 = can't seek */
/* 0x801103F0 */
void seqError(s32 code) {
    if (gSeqErrorFunc != NULL) {
        gSeqErrorFunc(gSeqErrorArg, code);
    }
}

/* 0x80110428 */
void seqSetErrorFunc(SndSeqErrorFunc f, s32 arg) {
    gSeqErrorFunc = f;
    gSeqErrorArg = arg;
}

/* 0x8011043C */
s32 sndNumSeqs(void) {
    return gSndNumSeqs;
}

/* 0x8011044C */
s32 seqValid(s32 seq) {
    if (seq < 0 || seq >= gSndNumSeqs) {
        return 0;
    }
    return 1;
}

/* 0x80110478 - open the file (the opens nest) */
s32 seqOpen(void) {
    if (gSeqOpenCount == 0) {
        gSeqFile = sndFileOpen(gSeqRom);
        if (gSeqFile == NULL) {
            seqError(1);
            return 0;
        }
    }
    gSeqOpenCount++;
    return 1;
}

/* 0x801104E8 */
void seqClose(void) {
    s32 n = gSeqOpenCount;

    if (n == 1) {
        sndFileClose(gSeqFile);
        n = gSeqOpenCount;
    }
    if (n > 0) {
        gSeqOpenCount = n - 1;
    }
}

/* round up to a multiple of 8 */
#define ALIGN8(x) ((x) += (x) & 1, (x) += (x) & 2, (x) += (x) & 4)

/* 0x8011053C - load a sequence's data to dst; returns the bytes used */
s32 seqLoad(s32 seq, u8 *dst) {
    u32 p = (u32)dst;
    SndSeqDef *def;
    u32 n;
    s32 i;
    s32 off;
    u8 *data;
    SndTrackRef *ref;

    if (!gSeqReady) {
        return p - (u32)dst;
    }
    if (!seqValid(seq)) {
        return 0;
    }
    def = &gSeqState->hdr->seqs[seq];
    n = def->numTracks;
    if (gSndCfg.tracksPerSound < n) {
        return 0;
    }
    def->tracks = (SndTrackRef *)p;
    p += n * sizeof(SndTrackRef);
    ALIGN8(p);
    data = (u8 *)p;
    off = def->offset + gSeqDataStart;
    p += def->size;
    if (def->packed == 0) {
        if (!seqOpen()) {
            seqError(1);
            return 0;
        }
        if (sndFileSeek(gSeqFile, off, 0) != 0) {
            seqError(3);
            return 0;
        }
        if (def->size != sndFileRead(data, def->size, gSeqFile)) {
            seqError(2);
            return 0;
        }
        seqClose();
    } else if (sndUnpack(def->packed, gSeqRom, off, (s32)data, def->size) < 0) {
        return 0;
    }
    for (i = 0; i < (s32)n; i++) {
        ref = &def->tracks[i];
        ref->def = (SndTrackDef *)data;
        data += sizeof(SndTrackDef);
        ref->labels = (s32 *)data;
        data += ref->def->numLabels * 4;
        ref->script = data;
        data += ref->def->scriptSize;
    }
    return p - (u32)dst;
}

/* 0x80110950 - read the header; returns the size of the sequence table */
s32 seqTableSize(SndState *st, s32 rom) {
    s32 size = 0;

    gSeqRom = rom;
    gSeqState = st;
    if (st != NULL) {
        if (!seqOpen()) {
            seqError(1);
            return 0;
        }
        if (sndFileSeek(gSeqFile, 0, 0) != 0) {
            seqError(3);
            return 0;
        }
        if (sndFileRead((u8 *)&gSeqHeader, 0x20, gSeqFile) != 0x20) {
            seqError(2);
            return 0;
        }
        size = gSeqHeader.tableSize;
        seqClose();
    }
    return size;
}

/* 0x80110A14 - load the sequence table to dst (keepOpen 1: leave the file open) */
s32 seqInit(SndState *st, s32 rom, s32 keepOpen, u8 *dst) {
    s32 size = 0;

    gSeqReady = 0;
    gSeqRom = rom;
    gSeqState = st;
    if (st == NULL) {
        return size;
    }
    if (!seqOpen()) {
        seqError(1);
        return 0;
    }
    if (sndFileSeek(gSeqFile, 0, 0) != 0) {
        seqError(3);
        return 0;
    }
    if (sndFileRead((u8 *)&gSeqHeader, 0x20, gSeqFile) != 0x20) {
        seqError(2);
        return 0;
    }
    gSndNumSeqs = gSeqHeader.numSeqs;
    size = gSeqHeader.tableSize;
    gSeqState->hdr->seqs = (SndSeqDef *)dst;
    if (gSeqHeader.packed == 0) {
        if (gSeqHeader.tableSize != sndFileRead((u8 *)gSeqState->hdr->seqs, gSeqHeader.tableSize, gSeqFile)) {
            seqError(2);
            return 0;
        }
        gSeqDataStart = gSeqHeader.tableSize + 0x20;
    } else {
        if (sndUnpack(gSeqHeader.packed, gSeqRom, 0x20, (s32)gSeqState->hdr->seqs, gSeqHeader.tableSize) < 0) {
            return 0;
        }
        gSeqDataStart = gSeqHeader.packedSize + 0x20;
    }
    if (keepOpen != 1) {
        seqClose();
    }
    gSeqReady = 1;
    return size;
}

/* 0x80110BD4 */
void seqShutdown(void) {
    seqClose();
    gSeqReady = 0;
}

/* 0x80110BF8 - bytes a sequence needs loaded (0 if it is) */
s32 seqSize(s32 seq) {
    s32 size = 0;
    SndSeqDef *def;
    u32 n;
    s32 t;

    if (!gSeqReady) {
        return size;
    }
    if (!seqValid(seq)) {
        return 0;
    }
    def = &gSeqState->hdr->seqs[seq];
    n = def->numTracks;
    if (gSndCfg.tracksPerSound < n) {
        return 0;
    }
    if (def->tracks == NULL) {
        t = n * sizeof(SndTrackRef);
        ALIGN8(t);
        size = def->size + t;
    }
    return size;
}

/* 0x80110CB4 - load a sequence unless it is loaded */
s32 seqLoadIfNeeded(s32 seq, u8 *dst) {
    s32 size = 0;

    if (!gSeqReady) {
        return size;
    }
    if (!seqValid(seq)) {
        return 0;
    }
    if (gSeqState->hdr->seqs[seq].tracks != NULL) {
        return size;
    }
    return seqLoad(seq, dst);
}

/* 0x80110D40 */
s32 seqUnload(s32 seq) {
    s32 r = 0;
    SndSeqDef *def;

    if (!gSeqReady) {
        return r;
    }
    if (!seqValid(seq)) {
        return 0;
    }
    def = &gSeqState->hdr->seqs[seq];
    if (def->tracks != NULL) {
        def->tracks = NULL;
        r = 1;
    }
    return r;
}

/* 0x80110DC0 - lists end with -1 */
s32 seqListSize(s16 *list) {
    s32 size = 0;
    s16 seq;

    if (gSeqReady) {
        for (seq = *list; seq != -1; seq = *++list) {
            size += seqSize(seq);
        }
    }
    return size;
}

/* 0x80110E38 */
s32 seqListLoad(s16 *list, u8 *dst) {
    s32 size = 0;
    s16 seq;

    if (!gSeqReady) {
        return size;
    }
    if (!seqOpen()) {
        return 0;
    }
    for (seq = *list; seq != -1; seq = *++list) {
        size += seqLoadIfNeeded(seq, dst + size);
    }
    seqClose();
    return size;
}

/* 0x80110EE4 */
s32 seqListUnload(s16 *list) {
    s32 r = 0;
    s16 seq;

    if (gSeqReady && *list != -1) {
        r = 1;
        for (seq = *list; seq != -1; seq = *++list) {
            seqUnload(seq);
        }
    }
    return r;
}

/* 0x80110F60 - the same for a range of sequences */
s32 seqRangeSize(s32 seq, s32 n) {
    s32 size = 0;

    if (!gSeqReady) {
        return size;
    }
    if (n == 0) {
        return 0;
    }
    while (n-- != 0) {
        size += seqSize(seq);
        seq++;
    }
    return size;
}

/* 0x80110FDC */
s32 seqRangeLoad(s32 seq, s32 n, u8 *dst) {
    s32 size = 0;

    if (!gSeqReady) {
        return size;
    }
    if (!seqOpen()) {
        return 0;
    }
    if (n == 0) {
        return 0;               /* (leaves the file open) */
    }
    while (n-- != 0) {
        size += seqLoadIfNeeded(seq, dst + size);
        seq++;
    }
    seqClose();
    return size;
}

/* 0x8011108C */
s32 seqRangeUnload(s32 seq, s32 n) {
    s32 r = 0;

    if (!gSeqReady) {
        return r;
    }
    if (n == 0) {
        return 0;
    }
    r = 1;
    while (n-- != 0) {
        seqUnload(seq);
        seq++;
    }
    return r;
}
