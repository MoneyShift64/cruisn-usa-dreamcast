#ifndef COMMON_H
#define COMMON_H

#include <ultra64.h>

/* ------------------------------------------------------------------ */
/* Overlays (table at 0x801321D0, 24 bytes per entry)                 */
/* ------------------------------------------------------------------ */
typedef struct OverlayEntry {
    /* 0x00 */ u32 romStart;
    /* 0x04 */ u32 romEnd;
    /* 0x08 */ u32 vramStart;
    /* 0x0C */ u32 vramEnd;     /* end including bss */
    /* 0x10 */ u32 bssStart;
    /* 0x14 */ u32 bssEnd;
} OverlayEntry; /* size = 0x18 */

enum OverlayId {
    OVL_BOOT     = 0, /* tiny overlay run once at start-up        (ROM 0x3D860) */
    OVL_LOWMEM   = 1, /* resident code in low RAM at 0x80000480   (ROM 0x7F100) */
    OVL_RACE     = 2, /* in-race code                             (ROM 0x3E6F0) */
    OVL_FRONTEND = 3  /* title, menus, car select, credits        (ROM 0x5EA10) */
};

/* ------------------------------------------------------------------ */
/* DMA request (queued to the DMA thread)                             */
/* ------------------------------------------------------------------ */
/* Generic doubly-linked list used by the DMA queues and other pools. */
typedef struct ListNode {
    /* 0x00 */ struct ListNode *prev;
    /* 0x04 */ struct ListNode *next;
} ListNode;

typedef struct List {
    /* 0x00 */ ListNode *head;
    /* 0x04 */ ListNode *tail;
} List;

typedef struct DmaRequest {
    /* 0x00 */ ListNode node;
    /* 0x08 */ void (*callback)(struct DmaRequest *); /* if set, run by the main loop when done */
    /* 0x0C */ u32 romAddr;
    /* 0x10 */ u32 ramAddr;
    /* 0x14 */ u32 size;
    /* 0x18 */ volatile s32 flags; /* see DMAREQ_* */
} DmaRequest; /* size = 0x1C */

#define DMAREQ_WRITE      0x0001    /* RAM -> cart (otherwise cart -> RAM)  */
#define DMAREQ_DCACHE     0x0002    /* write back / invalidate data cache   */
#define DMAREQ_ICACHE     0x0004    /* invalidate instruction cache (code)  */
#define DMAREQ_HIGHPRI    0x0008    /* use the high-priority queue          */
#define DMAREQ_FRONT      0x0010    /* jump to the front of its queue       */
#define DMAREQ_DONE       0x8000    /* set by the DMA thread when finished  */

/* ------------------------------------------------------------------ */
/* Graphics frame slots (3, at 0x801476A0). Each owns four buffers:   */
/* display list (32 KB), matrices (64000 B), vertices (16000 B) and   */
/* viewports (320 B). Overflow of any is a fatal error.               */
/* ------------------------------------------------------------------ */
typedef struct GfxSlot {
    /* 0x00 */ u32 *dlStart;
    /* 0x04 */ u8  *mtxStart;
    /* 0x08 */ u8  *vtxStart;
    /* 0x0C */ u8  *vpStart;
    /* 0x10 */ u32 dlUsed;
    /* 0x14 */ u32 mtxUsed;
    /* 0x18 */ u32 vtxUsed;
    /* 0x1C */ u32 vpUsed;
    /* 0x20 */ s32 free;
    /* 0x24 */ s32 unk24;
    /* 0x28 */ s32 frameNum;
    /* 0x2C */ s32 userArg;
} GfxSlot; /* size = 0x30 */

typedef struct FrameBuffer {
    /* 0x00 */ u16 *addr;       /* KSEG1 (uncached) */
    /* 0x04 */ s32 free;
} FrameBuffer;

#define GFX_DL_SIZE   0x8000
#define GFX_MTX_SIZE  0xFA00
#define GFX_VTX_SIZE  0x3E80
#define GFX_VP_SIZE   0x140

/* Messages to the scheduler thread */
#define SCHED_MSG_SP_DONE   1
#define SCHED_MSG_DP_DONE   2
#define SCHED_MSG_VI        3
#define SCHED_MSG_KICK      4
#define SCHED_MSG_PRENMI    5


/* ------------------------------------------------------------------ */
/* A 32-bit word that may be only 2-byte aligned (the N64 code uses   */
/* lwl/lwr, swl/swr). Byte by byte, big-endian, on the N64; in the    */
/* port the data is little-endian like everything else.              */
/* ------------------------------------------------------------------ */
#ifdef PORT
#define LOAD_U32_UNALIGNED(p)  ((u32)((u8 *)(p))[0] | ((u32)((u8 *)(p))[1] << 8) | \
                                ((u32)((u8 *)(p))[2] << 16) | ((u32)((u8 *)(p))[3] << 24))
#define STORE_U32_UNALIGNED(p, v) { u32 _v = (u32)(v); ((u8 *)(p))[0] = _v; ((u8 *)(p))[1] = _v >> 8; \
                                    ((u8 *)(p))[2] = _v >> 16; ((u8 *)(p))[3] = _v >> 24; }
#else
#define LOAD_U32_UNALIGNED(p)  (((u8 *)(p))[0] << 24 | ((u8 *)(p))[1] << 16 | ((u8 *)(p))[2] << 8 | ((u8 *)(p))[3])
#define STORE_U32_UNALIGNED(p, v) { u32 _v = (u32)(v); ((u8 *)(p))[0] = _v >> 24; ((u8 *)(p))[1] = _v >> 16; \
                                    ((u8 *)(p))[2] = _v >> 8; ((u8 *)(p))[3] = _v; }
#endif

#endif
