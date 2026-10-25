/*
 * system.c - overlay loading, the simple heap, string/memory helpers and the
 * random number generator.
 * Original code: ROM 0x3CA0-0x42E0 (RAM 0x80102CA0-0x801032E0).
 */
#include "common.h"

extern OverlayEntry gOverlayTable[4];    /* 0x801321D0 */
extern s32 gCurrentOverlay;              /* 0x801466D0 */
extern u32 gHeapPtr;                     /* 0x8014658C */
extern u32 gHighHeapPtr;                 /* 0x80146590 */
extern u32 gOverlayHeapLimit;            /* 0x801465F0 */
extern u32 gFramebuffersEnd;             /* 0x801465F4 */
extern s32 gLowMemOverlayState;          /* 0x801466CC: 0 = none, 1 = waiting, 2 = loaded */
extern u32 gRandomSeed;                  /* 0x801466EC */

/* Graphics frame slots: the pending and the running display list must not be
 * using the low-RAM area (GfxSlot.unk24) when the low-RAM overlay is loaded. */
extern GfxSlot gGfxSlots[3];             /* 0x801476A0 */
extern s32 gGfxPendingSlot;              /* 0x80147668 */
extern s32 gGfxRunningSlot;              /* 0x8014766C */

s32  disableInterrupts(void);            /* 0x801156E0 (game copy of __osDisableInt) */
void restoreInterrupts(s32 mask);        /* 0x80115700 (game copy of __osRestoreInt) */
void dmaReadSync(u32 rom, u32 ram, u32 size, s32 flags); /* 0x801035D0 */
u32  overlaySize(s32 id);
void overlayCopyIn(s32 id);
void memFill32(u32 *dst, s32 count, u32 value);
void memClear32(u32 *dst, s32 count);
char *strCopy(char *dst, const char *src);
u32  randNext(void);
f32  randFloat(void);
s32  heapFree(s32 area);

/* 0x80102CA0 - load an overlay into the overlay area and reserve its memory */
void overlayLoad(s32 id) {
    u32 size;

    gCurrentOverlay = id;
    size = overlaySize(id);
    overlayCopyIn(id);
    gHeapPtr += size;
}

/* 0x80102CE8 - release the memory reserved by overlayLoad() */
void overlayUnload(s32 id) {
    gHeapPtr -= overlaySize(id);
}

/* 0x80102D18 - allocate from the low heap; returns 0 if it doesn't fit.
 * align must be a power of two. Result is rounded up to a 4-byte boundary. */
u32 heapAlloc(u32 size, s32 align) {
    u32 addr = (gHeapPtr + align - 1) & ~(align - 1);

    if (gOverlayHeapLimit - addr < size) {
        return 0;
    }
    gHeapPtr = (addr + size + 3) & ~3;
#ifdef PORT
    {   /* the port's texture cache (port/gfx.c): new contents will go here */
        extern void port_mem_written(const void *p, u32 len);
        port_mem_written((void *)(addr + 0x80000000u), size);
    }
#endif
    return addr;
}

/* 0x80102D74 - wait until neither the pending nor the running display list
 * uses the low-RAM area, then load the low-RAM overlay. Returns the high heap pointer. */
u32 lowMemOverlayLoad(void) {
    s32 mask;
    s32 i;

    while (TRUE) {
        mask = disableInterrupts();
        if ((gGfxPendingSlot >= 0 && gGfxSlots[gGfxPendingSlot].unk24 != 0) ||
            (gGfxRunningSlot >= 0 && gGfxSlots[gGfxRunningSlot].unk24 != 0)) {
            gLowMemOverlayState = 1;
            restoreInterrupts(mask);
            for (i = 0; i < 200; i += 4) {
                /* short delay */
            }
            continue;
        }
        break;
    }
    gLowMemOverlayState = 2;
    restoreInterrupts(mask);
    overlayCopyIn(OVL_LOWMEM);
    return gHighHeapPtr;
}

/* 0x80102E84 */
void lowMemOverlayRelease(void) {
    gLowMemOverlayState = 0;
}

/* 0x80102E94 - total free heap memory */
s32 heapFreeTotal(void) {
    s32 low = heapFree(1);
    return heapFree(2) + low;
}

/* 0x80102EC4 - free memory in heap area 1 (low) or 2 (above framebuffers) */
s32 heapFree(s32 area) {
    if (area == 1) {
        if (gHighHeapPtr < gFramebuffersEnd) {
            return gHighHeapPtr - gHeapPtr;
        }
        return gOverlayHeapLimit - gHeapPtr;
    }
    if (area == 2) {
        if (gHighHeapPtr < gFramebuffersEnd) {
            return 0;
        }
        return gHighHeapPtr - gFramebuffersEnd;
    }
    return 0;
}

/* 0x80102F4C */
char *strCopy(char *dst, const char *src) {
    const char *s = src;
    char *d = dst;

    while ((*d++ = *s++) != '\0') {
    }
    return dst;
}

/* 0x80102F80 */
char *strAppend(char *dst, const char *src) {
    s32 len = 0;
    char *p = dst;

    while (*p != '\0') {
        len++;
        p++;
    }
    strCopy(dst + len, src);
    return dst;
}

/* 0x80102FD4 - fill 'count' 32-bit words */
void memFill32(u32 *dst, s32 count, u32 value) {
    s32 i;

    for (i = 0; i < count; i++) {
        dst[i] = value;
    }
}

/* 0x80103034 */
void memClear32(u32 *dst, s32 count) {
    memFill32(dst, count, 0);
}

/* 0x80103054 - copy 'count' 32-bit words */
u32 *memCopy32(u32 *dst, u32 *src, s32 count) {
    s32 i;

    for (i = 0; i < count; i++) {
        dst[i] = src[i];
    }
    return dst;
}

/* 0x801030E0 - linear congruential generator: seed = seed * 5 + 1 */
u32 randNext(void) {
    return gRandomSeed = gRandomSeed * 5 + 1;
}

/* 0x80103104 - random float in [0, 1) */
f32 randFloat(void) {
    return (f32)(randNext() >> 12) / 1048576.0f;
}

/* 0x80103150 - random integer in [0, n) */
u32 randInt(s32 n) {
    return ((randNext() & 0xFFFF) * n) >> 16;
}

/* 0x80103188 - random integer in [lo, hi] */
s32 randRange(s32 lo, s32 hi) {
    return (s32)(randFloat() * (f32)(hi - lo + 1)) + lo;
}

/* 0x801031DC - TRUE with the given probability (0.0 - 1.0) */
s32 randChance(f32 probability) {
    f32 r = randFloat();
    s32 result = FALSE;

    if (r < probability) {
        result = TRUE;
    }
    return result;
}

/* 0x80103218 - intended to return log2(n), but the original forgets to shift,
 * so it never returns for n >= 2. It is never called. Kept for reference. */
s32 log2Broken(u32 n) {
    u32 x = n >> 1;
    s32 count = 0;

    while (x != 0) {
        count++;
    }
    return count;
}

/* 0x80103238 - bytes of RAM an overlay occupies (code + data + bss) */
u32 overlaySize(s32 id) {
    OverlayEntry *ovl = &gOverlayTable[id];

    return ovl->vramEnd - ovl->vramStart;
}

#ifndef PORT                             /* port/port_main.c has its own */
/* 0x80103264 - DMA an overlay's code/data from ROM and clear its bss */
void overlayCopyIn(s32 id) {
    OverlayEntry *ovl = &gOverlayTable[id];
    u32 bss = ovl->bssStart;
    u32 bssSize = ovl->bssEnd - bss;
    u32 romStart = ovl->romStart;

    dmaReadSync(romStart, ovl->vramStart, ovl->romEnd - romStart, DMAREQ_DCACHE | DMAREQ_ICACHE);
    memClear32((u32 *)bss, bssSize >> 2);
}
#endif
