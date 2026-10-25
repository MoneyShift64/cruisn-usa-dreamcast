/*
 * lowmem.c - the asset loader (resident overlay in low RAM): reading numbered
 * files from the ROM's asset table, unpacking the run-length packed ones, and
 * the "high heap" they are loaded into.
 * Original code: overlay 1, ROM 0x7F100-0x7F8B0 and 0x7FDE0-0x7FFF0 (RAM 0x80000480-0x80000C30,
 * 0x80001160-0x80001370). ROM 0x7F8B0-0x7FDE0 (0x80000C30-0x80001160) is an adaptive
 * Huffman decoder for packing method 1, which no file in the ROM uses: not decompiled.
 *
 * Asset table (ROM 0x80470): 8 bytes per file id, read from +4:
 *   byte 0: bit 7 packed, bits 4-6 kind, bits 0-3 packing method
 *   byte 1: bits 4-7 packed size bits 16-19 (x 256), bits 0-3 size bits 16-19
 *   bytes 2-3: size bits 0-15
 *   bytes 4-7: byte 4 = packed size bits 8-15 (x 256 with the nibble above),
 *              low 24 bits = offset from 0x80470
 * Kinds: 0 data, 1 model (fixed up after loading), 2 model compiled to a display list,
 * 3 palette, 4 texture.
 * Only files 0x2BE-0x2C0, 0x2E4 and 0x2E6 are packed (method 2, RLE on bytes).
 *
 * The high heap grows down from gHighHeapEnd to gFramebuffersEnd, then continues
 * below gOverlayHeapLimit. Each block has an 8-byte header at its top end
 * (u16 file id, u16 size / 8 - 1); addresses are physical (add 0x80000000).
 */
#include "common.h"

#define ASSET_ROM     0x80470                /* ROM address of the asset table and data */
#define ASSET_ROM_END 0x543C64               /* reads are clipped to here (offset) */
#define KSEG0(a)      ((u8 *)((u32)(a) + 0x80000000))

typedef struct BlockHeader {
    /* 0x00 */ u16 id;
    /* 0x02 */ u16 size8m1;                  /* size / 8 - 1 */
    /* 0x04 */ u32 pad;
} BlockHeader;
#define BLOCK_HDR(end) ((BlockHeader *)(KSEG0(end) - 8))

extern u32 gHeapPtr;                     /* 0x8014658C - low heap (physical) */
extern u32 gHighHeapPtr;                 /* 0x80146590 */
extern u32 gOverlayHeapLimit;            /* 0x801465F0 */
extern u32 gFramebuffersEnd;             /* 0x801465F4 */
extern u32 gHighHeapEnd;                 /* 0x801465F8 */
extern u8  gAssetHeader[8];              /* 0x800097F0 */
extern u8  gUnpackBuf[0x8000];           /* 0x800017F0 - packed data is streamed through here */

void dmaReadSync(u32 rom, u32 ram, u32 size, s32 flags); /* 0x801035D0 */
u32  heapAlloc(u32 size, s32 align);     /* 0x80102D18 */
void modelRelocate(void *model);         /* 0x8011C59C */
void modelFixup(void *model);            /* 0x8011C21C */
void modelCompile(void *model, void *out, f32 *radius2); /* 0x8011B710 */

void huffStart(u8 *in, s32 inWords, u8 *out, u32 size); /* 0x80000C30 (method 1, not decompiled) */
s32  huffStep(void);                     /* 0x80000CB0 */
void rleStart(s32 width, u8 *in, s32 inSize, u8 *out, s32 size); /* 0x80001160 */
s32  rleStep(void);                      /* 0x800011F4 */
u32  assetFindLoaded(s32 id);            /* 0x80000994 */
u32  highHeapMark(void);                 /* 0x80000A64 */
void highHeapRestore(u32 mark);          /* 0x80000A74 */
u32  highHeapAlloc(s32 id, u32 size);    /* 0x80000A84 */
u32  assetPostLoad(u32 addr, s32 id, s32 kind, u32 size); /* 0x80000B4C */
u32  assetCompile(void *model, s32 id, u32 size); /* 0x80000BDC */

/* 0x80000480 - load file 'id' (if not loaded already); returns its physical
 * address, 0 if it doesn't fit */
u32 assetLoad(s32 id) {
    u32 addr;
    s32 packed, kind, method;
    u32 offset, packedLeft, size, chunk, next;
    u32 highMark, lowMark;

    addr = assetFindLoaded(id);
    if (addr != 0) {
        return addr;
    }
    dmaReadSync(ASSET_ROM + id * 8 + 4, (u32)gAssetHeader, 8, 2);
    packed = (gAssetHeader[0] & 0x80) >> 7;
    packedLeft = ((*(u32 *)&gAssetHeader[4] >> 24) | ((gAssetHeader[1] >> 4) << 8)) << 8;
    kind = (gAssetHeader[0] & 0x70) >> 4;
    method = gAssetHeader[0] & 0xF;
    offset = *(u32 *)&gAssetHeader[4] & 0xFFFFFF;
    size = ((gAssetHeader[1] & 0xF) << 16) + *(u16 *)&gAssetHeader[2];
    highMark = gHighHeapPtr;
    lowMark = gHeapPtr;
    if (kind != 0 && kind != 1 && kind == 2) {
        addr = heapAlloc(size + 8, 8);          /* temporary: compiled into the high heap below */
    } else {
        addr = highHeapAlloc(id, size);
    }
    if (addr == 0) {
        return 0;
    }
    if (packed) {
        if (method == 1) {
            huffStart(gUnpackBuf, 0x2000, KSEG0(addr), size);
            do {
                chunk = (packedLeft > 0x8000) ? 0x8000 : packedLeft;
                next = offset + chunk;
                if (next >= ASSET_ROM_END + 1) {
                    chunk = ASSET_ROM_END - offset;
                    next = offset + chunk;
                }
                dmaReadSync(ASSET_ROM + offset, (u32)gUnpackBuf, chunk, 2);
                offset = next;
                packedLeft -= chunk;
            } while (huffStep() != 0);
        } else if (method >= 2 && method < 5) {
            rleStart(method - 2, gUnpackBuf, 0x2000, KSEG0(addr), size);
            do {
                chunk = (packedLeft > 0x8000) ? 0x8000 : packedLeft;
                next = offset + chunk;
                if (next >= ASSET_ROM_END + 1) {
                    chunk = ASSET_ROM_END - offset;
                    next = offset + chunk;
                }
                dmaReadSync(ASSET_ROM + offset, (u32)gUnpackBuf, chunk, 2);
                offset = next;
                packedLeft -= chunk;
            } while (rleStep() != 0);
        }
    } else {
        dmaReadSync(ASSET_ROM + offset, (u32)KSEG0(addr), size, 2);
    }
    addr = assetPostLoad(addr, id, kind, size);
    if (addr == 0) {
        gHighHeapPtr = highMark;
        return 0;
    }
    gHeapPtr = lowMark;
    return addr;
}

/* 0x8000078C - a group file is a list of file ids (u16, ends with 0xFFFF): load
 * them all; out[] gets their physical addresses. Returns the count. */
s32 assetLoadGroup(s32 id, u32 *out) {
    u32 mark = highHeapMark();
    s32 n = 0;
    s32 i;
    u16 *list;
    u32 v;

    list = (u16 *)KSEG0(assetLoad(id & 0xFFFF));
    if (list != NULL && list[0] != 0xFFFF) {
        v = list[n];
        do {
            out[n] = v;
            v = list[++n];
        } while (v != 0xFFFF);
    }
    highHeapRestore(mark);                      /* the list itself is dropped */
    if (list != NULL) {
        for (i = 0; i != n; i++) {
            out[i] = assetLoad(out[i]);
        }
    }
    return n;
}

/* 0x8000085C - as assetLoadGroup, with KSEG0 addresses (unused) */
void assetLoadGroupK0(s32 id, u32 *out) {
    s32 n = assetLoadGroup(id & 0xFFFF, out);

    if (n >= 0) {
        do {
            out[n] += 0x80000000;               /* (original bug: also out[count], one past the end) */
        } while (--n >= 0);
    }
}

/* 0x800008BC - replace a list of ids by their loaded addresses (unused) */
void assetFindList(u32 *ids, u32 n) {
    u32 i;

    for (i = 0; i < n; i++) {
        ids[i] = assetFindLoaded(ids[i]);
    }
}

/* 0x80000914 - size of a loaded file (unused; original bug: reads the headers
 * through physical addresses, so it could never work on the N64) */
s32 assetLoadedSize(s32 id) {
    u32 end = gFramebuffersEnd;
    u8 *h;

    id &= 0xFFFF;
    while (gHighHeapPtr < end) {
        h = (u8 *)((u32)KSEG0(end) - 8);
        if (id == *(u16 *)(h - 8)) {
            return (*(u16 *)((u32)h + 0x7FFFFFFA) + 1) * 8;
        }
        end -= (*(u16 *)((u32)h + 0x7FFFFFFA) + 1) * 8;
    }
    return 0;
}

/* 0x80000994 - physical address of file 'id' if it is in the high heap, else 0 */
u32 assetFindLoaded(s32 id) {
    u32 low = gHighHeapPtr;
    u32 end;

    if (low < gFramebuffersEnd) {
        low = gFramebuffersEnd;
    }
    for (end = gHighHeapEnd; low < end;) {
        BlockHeader *h = BLOCK_HDR(end);
        end -= (h->size8m1 + 1) * 8;
        if (id == h->id) {
            return end;
        }
    }
    if (low == gHighHeapPtr) {
        return 0;
    }
    for (end = gOverlayHeapLimit; gHighHeapPtr < end;) {
        BlockHeader *h = BLOCK_HDR(end);
        end -= (h->size8m1 + 1) * 8;
        if (id == h->id) {
            return end;
        }
    }
    return 0;
}

/* 0x80000A64 */
u32 highHeapMark(void) {
    return gHighHeapPtr;
}

/* 0x80000A74 */
void highHeapRestore(u32 mark) {
    gHighHeapPtr = mark;
}

/* 0x80000A84 - take a block for file 'id' (size rounded up, plus the header);
 * jumps below the overlay area when the space above the framebuffers runs out */
u32 highHeapAlloc(s32 id, u32 size) {
    u32 top = gHighHeapPtr;
    BlockHeader *h;

    size = (size + 15) & ~7;
    if (!(top < gFramebuffersEnd) && top - size < gFramebuffersEnd) {
        top = gOverlayHeapLimit;
        gHighHeapPtr = top;
    }
    if (!(top < gFramebuffersEnd) && top - size < gFramebuffersEnd) {
        return 0;
    }
    h = BLOCK_HDR(top);
    h->id = id;
    h->size8m1 = (s32)size / 8 - 1;
    top = gHighHeapPtr - size;
    gHighHeapPtr = top;
    if (top < gFramebuffersEnd && gOverlayHeapLimit < top) {
        gHighHeapPtr = gOverlayHeapLimit;
        top = gOverlayHeapLimit;
    }
#ifdef PORT
    {   /* the port's texture cache (port/gfx.c): new contents will go here */
        extern void port_mem_written(const void *p, u32 len);
        port_mem_written(KSEG0(top), size);
    }
#endif
    return top;
}

/* 0x80000B4C - models (kind 1 and 2) are relocated and fixed up; kind 2 is then
 * compiled into a display list in the high heap */
u32 assetPostLoad(u32 addr, s32 id, s32 kind, u32 size) {
    u8 *m;

    if (kind != 0 && (kind == 1 || kind == 2)) {
        m = KSEG0(addr);
        modelRelocate(m);
        modelFixup(m);
        if (kind == 2) {
            addr = assetCompile(m, id, *(s16 *)(m + 6) * 16 + *(s16 *)(m + 4) * 8);
        }
    }
    return addr;
}

/* 0x80000BDC */
u32 assetCompile(void *model, s32 id, u32 size) {
    u32 addr = highHeapAlloc(id, (size + 15) & ~7);

    modelCompile(model, KSEG0(addr), NULL);
    return addr;
}

/* ---- packing methods 2-4: run-length coding on 8, 16 or 32-bit units ----
 * A control unit c: top bit set = repeat the next unit (c & ~top) + 3 times,
 * else copy the next c + 1 units. Fed one DMA buffer at a time. */
extern u8 *gRleIn;                       /* 0x8000AA20 */
extern u8 *gRleOut;                      /* 0x8000AA24 */
extern s32 gRleWidth;                    /* 0x8000AA28 - 0: bytes, 1: halfwords, 2: words */
extern s32 gRleInSize;                   /* 0x8000AA2C - units per buffer */
extern s32 gRleOutLeft;                  /* 0x8000AA30 - units still to write */
extern u8  gRleCtrl8;                    /* 0x8000AA34 */
extern u8  gRleVal8;                     /* 0x8000AA35 */
extern u16 gRleCtrl16;                   /* 0x8000AA36 */
extern u16 gRleVal16;                    /* 0x8000AA38 */
extern u32 gRleCtrl32;                   /* 0x8000AA3C */
extern u32 gRleVal32;                    /* 0x8000AA40 */
extern s32 gRleState;                    /* 0x8000AA44 - 0 need control, 1 run, 2 literals */

void rleStep8(void);                     /* 0x8000125C */
void rleStep16(void);                    /* 0x80001434 */
void rleStep32(void);                    /* 0x80001610 */

/* 0x80001160 */
void rleStart(s32 width, u8 *in, s32 inSize, u8 *out, s32 size) {
    gRleWidth = width;
    gRleIn = in;
    gRleOut = out;
    if (gRleWidth == 0) {
        gRleInSize = inSize * 4;
        gRleOutLeft = size;
    } else if (gRleWidth == 1) {
        gRleInSize = inSize * 2;
        gRleOutLeft = size >> 1;
    } else {
        gRleInSize = inSize;
        gRleOutLeft = size >> 2;
    }
    gRleState = 0;
}

/* 0x800011F4 - unpack one buffer; TRUE while more output is needed */
s32 rleStep(void) {
    if (gRleWidth == 0) {
        rleStep8();
    } else if (gRleWidth == 1) {
        rleStep16();
    } else {
        rleStep32();
    }
    return gRleOutLeft > 0;
}

#define RLE_STEP(T, CTRL, VAL, TOP)                                              \
    s32 inLeft = gRleInSize;                                                     \
    T *in = (T *)gRleIn;                                                         \
    T *out = (T *)gRleOut;                                                       \
    s32 i, n;                                                                    \
                                                                                 \
    while (inLeft != 0 && gRleOutLeft != 0) {                                    \
        if (gRleState == 0) {                                                    \
            CTRL = *in++;                                                        \
            inLeft--;                                                            \
            gRleState = (CTRL & TOP) ? 1 : 2;                                    \
            if (inLeft == 0) {                                                   \
                break;                                                           \
            }                                                                    \
        }                                                                        \
        if (gRleState == 1) {                                                    \
            VAL = *in++;                                                         \
            n = (CTRL & ~TOP) + 3;                                               \
            inLeft--;                                                            \
            for (i = 0; i < n; i++) {                                            \
                *out++ = VAL;                                                    \
            }                                                                    \
            gRleState = 0;                                                       \
        } else {                                                                 \
            n = CTRL + 1;                                                        \
            if ((u32)inLeft < (u32)(CTRL + 1)) {                                 \
                n = inLeft;                                                      \
                CTRL -= inLeft;                                                  \
            } else {                                                             \
                gRleState = 0;                                                   \
            }                                                                    \
            inLeft -= n;                                                         \
            for (i = 0; i < n; i++) {                                            \
                *out++ = *in++;                                                  \
            }                                                                    \
        }                                                                        \
        gRleOutLeft -= n;                                                        \
    }                                                                            \
    gRleOut = (u8 *)out;

/* 0x8000125C */
void rleStep8(void) {
    RLE_STEP(u8, gRleCtrl8, gRleVal8, 0x80)
}

/* 0x80001434 */
void rleStep16(void) {
    RLE_STEP(u16, gRleCtrl16, gRleVal16, 0x8000)
}

/* 0x80001610 */
void rleStep32(void) {
    RLE_STEP(u32, gRleCtrl32, gRleVal32, 0x80000000)
}
