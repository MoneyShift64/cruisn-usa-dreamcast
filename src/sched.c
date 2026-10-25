/*
 * sched.c - linked lists, the DMA thread, and the RSP/RDP/VI scheduler.
 * Original code: ROM 0x42E0-0x5550 (RAM 0x801032E0-0x80104550).
 *
 * Graphics: three GfxSlots are filled by the main loop (gfxBeginFrame /
 * gfxEndFrame). A finished slot becomes "pending"; the scheduler starts it on
 * the RSP when a free framebuffer exists ("running"), the RDP renders into the
 * framebuffer ("rendering"), and on the next retrace it is swapped to the
 * screen. The main loop can produce at most one frame per two retraces
 * (30 fps), tracked by gFrameCredits.
 *
 * Audio: every other retrace an audio task is built. Audio has priority: if a
 * graphics task is running on the RSP it is yielded, the audio task runs, and
 * the graphics task is resumed afterwards.
 */
#include "common.h"

/* --- lists ------------------------------------------------------------ */
s32  disableInterrupts(void);            /* 0x801156E0 */
void restoreInterrupts(s32 mask);        /* 0x80115700 */

/* --- DMA -------------------------------------------------------------- */
extern OSMesgQueue gDmaMesgQueue;        /* 0x801474B0 */
extern OSIoMesg    gDmaIoMesg;           /* 0x801474D0 */
extern List        gDmaQueueNormal;      /* 0x801474E8 */
extern List        gDmaQueueHigh;        /* 0x801474F0 */
extern DmaRequest *gDmaCurrent;          /* 0x801474F8 */
extern OSMesg      gDmaMesg;             /* 0x801466C8 */
extern List        gDmaDoneList;         /* 0x8014C680 */

/* --- controllers (SI) ---------------------------------------------------- */
extern OSMesgQueue gSiMesgQueue;         /* 0x801475F8 */
extern OSMesg      gSiMesgBuf[];         /* 0x80147610 */
extern s32         gContReadState;       /* 0x80147500 */
extern u8          gContPadsPrev[0x78];  /* 0x80147508 */
extern u8          gContPadsNew[0x78];   /* 0x80147580 */
extern OSMesgQueue gMainMesgQueue;       /* 0x80146620 */

/* --- scheduler ----------------------------------------------------------- */
extern OSTask      gGfxTask;             /* 0x80147620 */
extern OSMesgQueue gSchedMesgQueue;      /* 0x80146660 */
extern GfxSlot     gGfxSlots[3];         /* 0x801476A0 */
extern FrameBuffer gFrameBuffers[3];     /* 0x80147730 */
extern u32         gFramebuffers[3];     /* 0x801465A0 (physical) */
extern s32 gFrameCredits;                /* 0x80147660 */
extern s32 gGfxBuildingSlot;             /* 0x80147664 slot the main loop is filling */
extern s32 gGfxPendingSlot;              /* 0x80147668 finished, waiting for the RSP */
extern s32 gGfxRunningSlot;              /* 0x8014766C on the RSP */
extern s32 gFbRendering;                 /* 0x80147670 framebuffer the RDP is drawing */
extern s32 gFbQueued;                    /* 0x80147674 drawn, waiting to be shown */
extern s32 gFbSwapping;                  /* 0x80147678 passed to osViSwapBuffer */
extern s32 gFbDisplayed;                 /* 0x8014767C on screen now */
extern s32 gPreNmi;                      /* 0x80147680 reset button pressed */
extern u32 *gDlCursor;                   /* 0x80147684 */
extern u8  *gMtxCursor;                  /* 0x80147688 */
extern u8  *gVtxCursor;                  /* 0x8014768C */
extern u8  *gVpCursor;                   /* 0x80147690 */
extern s32 gViCount;                     /* 0x80147694 */
extern s32 gFrameCounter;                /* 0x80147698 */
extern s32 gLastShownFrame;              /* 0x8014769C */
extern s32 gGfxYielded;                  /* 0x801484F8 */
extern OSTask *gAudioTaskNew;            /* 0x801484FC */
extern OSTask *gAudioTaskQueued;         /* 0x80148500 */
extern OSTask *gAudioTaskRunning;        /* 0x80148504 */
extern s32 gAudioTaskStarted;            /* 0x80148508 */

extern char D_8013B2C0[];                /* "DLIST" */
extern char D_8013B2C8[];                /* "MTX" */
extern char D_8013B2CC[];                /* "VTX" */
extern char D_8013B2D0[];                /* "VIEWPORT" */
extern char D_8013B2DC[];                /* "---" */
extern char D_8013B2E0[];                /* "%s OVERFLOW OF %i BYTES\n" */

void dmaStart(DmaRequest *req);
DmaRequest *dmaSubmit(DmaRequest *req);
void dmaNotifyDone(DmaRequest *req);                 /* 0x80111150 */
void contReadBegin(void);                            /* 0x80111AAC */
void contReadEnd(void);                              /* 0x80111AE0 */
void contReadSchedule(void);                         /* 0x80111B28 */
OSTask *audioBuildTask(void);                        /* 0x80105BC4 */
void mtxConvertRange(u8 *start, u8 *end);            /* 0x801153B0 */
void fatalError(s32 code, char *msg);                /* 0x80104550 */
void resetCleanup(void);                             /* 0x80107CF8 */
void gfxReportOverflow(u32 dl, u32 mtx, u32 vtx, u32 vp);
void schedKick(void);
void schedStartTask(OSTask *task);
void gfxPatchSlot(s32 slot);
void schedOnSpDone(void);
void schedOnDpDone(void);
void schedOnVi(void);
void schedStartNext(void);
void schedOnPreNmi(void);

/* 0x801032E0 */
ListNode *listPushFront(List *list, ListNode *node) {
    s32 mask;

    if (node != NULL) {
        mask = disableInterrupts();
        node->prev = NULL;
        node->next = list->head;
        if (list->head != NULL) {
            list->head->prev = node;
        } else {
            list->tail = node;
        }
        list->head = node;
        restoreInterrupts(mask);
    }
    return node;
}

/* 0x80103350 */
ListNode *listPushBack(List *list, ListNode *node) {
    s32 mask;

    if (node != NULL) {
        mask = disableInterrupts();
        node->next = NULL;
        node->prev = list->tail;
        if (list->tail != NULL) {
            list->tail->next = node;
        } else {
            list->head = node;
        }
        list->tail = node;
        restoreInterrupts(mask);
    }
    return node;
}

/* 0x801033C0 - unlink a node; returns NULL if the list was empty */
ListNode *listRemove(List *list, ListNode *node) {
    s32 mask = disableInterrupts();

    if (list->head != NULL) {
        if (node->prev != NULL) {
            node->prev->next = node->next;
        } else {
            list->head = node->next;
        }
        if (node->next != NULL) {
            node->next->prev = node->prev;
        } else {
            list->tail = node->prev;
        }
        node->next = NULL;
        node->prev = NULL;
    } else {
        node = NULL;
    }
    restoreInterrupts(mask);
    return node;
}

/* 0x8010345C */
ListNode *listPopFront(List *list) {
    s32 mask = disableInterrupts();
    ListNode *node = listRemove(list, list->head);

    restoreInterrupts(mask);
    return node;
}

/* 0x801034A0 */
ListNode *listPopBack(List *list) {
    s32 mask = disableInterrupts();
    ListNode *node = listRemove(list, list->tail);

    restoreInterrupts(mask);
    return node;
}

/* 0x801034E4 - link 'count' items of 'stride' bytes into a list */
void listInitPool(List *list, u8 *items, s32 stride, s32 count) {
    s32 mask = disableInterrupts();
    ListNode *node;
    ListNode *next;

    if (count == 0) {
        list->head = NULL;
        list->tail = NULL;
    } else {
        list->head = (ListNode *)items;
        node = (ListNode *)items;
        node->prev = NULL;
        while (--count != 0) {
            next = (ListNode *)((u8 *)node + stride);
            next->prev = node;
            node->next = next;
            node = next;
        }
        node->next = NULL;
        list->tail = node;
    }
    restoreInterrupts(mask);
}

#ifndef PORT                             /* port/port_main.c has its own */
/* 0x801035D0 - read (or write) cartridge memory and wait until it's done */
void dmaReadSync(u32 rom, u32 ram, u32 size, s32 flags) {
    DmaRequest req;

    req.ramAddr = ram;
    req.size = size;
    req.flags = flags;
    req.callback = NULL;
    req.romAddr = rom;
    dmaSubmit(&req);
    while (!(req.flags & DMAREQ_DONE)) {
    }
}
#endif

/* 0x80103628 - queue a DMA (or start it at once if the DMA thread is idle) */
DmaRequest *dmaSubmit(DmaRequest *req) {
    List *queue = &gDmaQueueNormal;
    s32 mask;

    if (req->flags & DMAREQ_HIGHPRI) {
        queue = &gDmaQueueHigh;
    }
    req->flags &= ~DMAREQ_DONE;
    mask = disableInterrupts();
    if (gDmaCurrent != NULL) {
        if (req->flags & DMAREQ_FRONT) {
            listPushFront(queue, &req->node);
        } else {
            listPushBack(queue, &req->node);
        }
        restoreInterrupts(mask);
    } else {
        restoreInterrupts(mask);
        dmaStart(req);
    }
    return req;
}

/* 0x801036FC - DMA thread: finish the current request, start the next one */
void dmaThread(void *arg) {
    while (TRUE) {
        osRecvMesg(&gDmaMesgQueue, &gDmaMesg, OS_MESG_BLOCK);
        gDmaCurrent->flags |= DMAREQ_DONE;
        dmaNotifyDone(gDmaCurrent);
        if (gDmaQueueHigh.head != NULL) {
            dmaStart((DmaRequest *)listPopFront(&gDmaQueueHigh));
        } else if (gDmaQueueNormal.head != NULL) {
            dmaStart((DmaRequest *)listPopFront(&gDmaQueueNormal));
        } else {
            gDmaCurrent = NULL;
        }
    }
}

/* 0x801037DC */
void dmaStart(DmaRequest *req) {
    s32 direction;

    gDmaCurrent = req;
    if (req == NULL) {
        return;
    }
    if (req->flags & DMAREQ_WRITE) {
        if (req->flags & DMAREQ_DCACHE) {
            osWritebackDCache((void *)req->ramAddr, req->size);
        }
    } else {
        if (req->flags & DMAREQ_DCACHE) {
            osInvalDCache((void *)req->ramAddr, req->size);
        }
        if (req->flags & DMAREQ_ICACHE) {
            osInvalICache((void *)req->ramAddr, req->size);
        }
    }
    if (gPreNmi != 0) {
        return;
    }
    direction = OS_READ;
    if (req->flags & DMAREQ_WRITE) {
        direction = OS_WRITE;
    }
    osPiStartDma(&gDmaIoMesg, OS_MESG_PRI_NORMAL, direction, req->romAddr,
                 (void *)req->ramAddr, req->size, &gDmaMesgQueue);
}

/* 0x801038E0 */
void contInit(void) {
    osCreateMesgQueue(&gSiMesgQueue, gSiMesgBuf, 1);
    gContReadState = 0;
}

/* 0x80103918 - route SI (controller) completion messages to our queue */
void contAcquireSi(void) {
    contReadBegin();
    osSetEventMesg(OS_EVENT_SI, &gSiMesgQueue, NULL);
}

/* 0x8010394C - give SI completion messages back to the main queue */
void contReleaseSi(void) {
    osSetEventMesg(OS_EVENT_SI, &gMainMesgQueue, NULL);
    contReadEnd();
}

/* 0x80103980 */
void contReadStep(s32 keep) {
    if (gContReadState == 2 && keep != 0) {
        bcopy(gContPadsNew, gContPadsPrev, sizeof(gContPadsPrev));
    }
    gContReadState--;
}

#ifndef PORT                             /* port/port_main.c has its own */
/* 0x801039E0 - claim a free graphics slot for the next frame.
 * Returns 0 on success, 1 if no slot / no frame credit is available yet. */
s32 gfxBeginFrame(void) {
    s32 mask = disableInterrupts();
    s32 waiting = gGfxPendingSlot >= 0 && gGfxSlots[gGfxPendingSlot].userArg != 0;
    GfxSlot *slot;
    s32 i;

    restoreInterrupts(mask);
    if (!waiting && gFrameCredits != 0) {
        for (i = 0, slot = gGfxSlots; i < 3; i++, slot++) {
            if (slot->free) {
                slot->free = FALSE;
                gDlCursor  = slot->dlStart;
                gMtxCursor = slot->mtxStart + 0x40;
                gVtxCursor = slot->vtxStart;
                gVpCursor  = slot->vpStart;
                slot->unk24 = 0;
                slot->frameNum = gFrameCounter;
                gGfxBuildingSlot = i;
                mask = disableInterrupts();
                gFrameCredits--;
                restoreInterrupts(mask);
                return 0;
            }
        }
    }
    return 1;
}
#endif

#ifndef PORT                             /* port/port_main.c has its own */
/* 0x80103B10 - close the frame being built and hand it to the scheduler */
void gfxEndFrame(s32 userArg) {
    GfxSlot *slot;
    u32 dlUsed, mtxUsed, vtxUsed, vpUsed;
    s32 mask;

    gFrameCounter++;
    slot = &gGfxSlots[gGfxBuildingSlot];
    dlUsed  = (u8 *)gDlCursor - (u8 *)slot->dlStart;
    mtxUsed = gMtxCursor - slot->mtxStart;
    vtxUsed = gVtxCursor - slot->vtxStart;
    vpUsed  = gVpCursor - slot->vpStart;
    if (dlUsed > GFX_DL_SIZE || mtxUsed > GFX_MTX_SIZE || vtxUsed > GFX_VTX_SIZE || vpUsed > GFX_VP_SIZE) {
        gfxReportOverflow(dlUsed, mtxUsed, vtxUsed, vpUsed);
    }
    mtxConvertRange(gGfxSlots[gGfxBuildingSlot].mtxStart + 0x40, gMtxCursor);
    gGfxSlots[gGfxBuildingSlot].dlUsed  = dlUsed;
    gGfxSlots[gGfxBuildingSlot].mtxUsed = mtxUsed;
    gGfxSlots[gGfxBuildingSlot].vtxUsed = vtxUsed;
    gGfxSlots[gGfxBuildingSlot].vpUsed  = vpUsed;
    gGfxSlots[gGfxBuildingSlot].userArg = userArg;

    mask = disableInterrupts();
    if (gGfxPendingSlot >= 0) {
        /* an older frame never reached the RSP: drop it, keep the newest */
        gLastShownFrame = gGfxSlots[gGfxPendingSlot].frameNum;
        gfxPatchSlot(gGfxBuildingSlot);
        gGfxSlots[gGfxPendingSlot].free = TRUE;
        gGfxPendingSlot = gGfxBuildingSlot;
        gGfxBuildingSlot = -1;
        restoreInterrupts(mask);
        return;
    }
    gGfxPendingSlot = gGfxBuildingSlot;
    gGfxBuildingSlot = -1;
    restoreInterrupts(mask);
    schedKick();
}
#endif

#ifndef PORT                             /* port/port_main.c has its own */
/* 0x80103D88 - wait until no graphics work is in flight; returns the
 * framebuffer currently on screen */
s32 gfxWaitIdle(void) {
    s32 busy;

    do {
        busy = gGfxPendingSlot & gGfxRunningSlot & gFbRendering & gFbQueued & gFbSwapping;
        restoreInterrupts(disableInterrupts());
    } while (busy != -1);
    return gFbDisplayed;
}
#endif

/* 0x80103E48 - scheduler thread */
void schedThread(void *arg) {
    OSMesg msg;

    while (TRUE) {
        osRecvMesg(&gSchedMesgQueue, &msg, OS_MESG_BLOCK);
        switch ((s32)msg) {
            case SCHED_MSG_SP_DONE: schedOnSpDone(); break;
            case SCHED_MSG_DP_DONE: schedOnDpDone(); break;
            case SCHED_MSG_VI:      schedOnVi(); break;
            case SCHED_MSG_KICK:    schedStartNext(); break;
            case SCHED_MSG_PRENMI:  schedOnPreNmi(); break;
        }
    }
}

/* 0x80103F08 - an RSP task finished (or yielded) */
void schedOnSpDone(void) {
    if (gAudioTaskRunning != NULL) {
        if (gAudioTaskStarted) {
            /* audio task finished */
            gAudioTaskRunning = NULL;
            schedKick();
            return;
        }
        /* the graphics task stopped for the audio task */
        if (!osSpTaskYielded(&gGfxTask)) {
            /* it actually completed before it could yield */
            gGfxYielded = FALSE;
            gLastShownFrame = gGfxSlots[gGfxRunningSlot].frameNum;
            gGfxSlots[gGfxRunningSlot].free = TRUE;
            gGfxRunningSlot = -1;
        }
        schedStartTask(gAudioTaskRunning);
        gAudioTaskStarted = TRUE;
        return;
    }
    /* graphics task finished */
    gLastShownFrame = gGfxSlots[gGfxRunningSlot].frameNum;
    gGfxSlots[gGfxRunningSlot].free = TRUE;
    gGfxRunningSlot = -1;
    schedKick();
}

/* 0x80104038 - the RDP finished drawing a framebuffer */
void schedOnDpDone(void) {
    s32 fb = gFbRendering;

    gFbRendering = -1;
    if (gFbSwapping >= 0) {
        gFbQueued = fb;
    } else {
        gFbSwapping = fb;
        osViSwapBuffer(gFrameBuffers[fb].addr);
    }
    schedKick();
}

/* 0x801040A4 - vertical retrace */
void schedOnVi(void) {
    void *cur;
    s32 fb;

    gViCount++;
    if (gAudioTaskNew != NULL) {
        gAudioTaskQueued = gAudioTaskNew;
        gAudioTaskNew = NULL;
        schedStartNext();
    }

    cur = osViGetCurrentFramebuffer();
    if (cur == gFrameBuffers[0].addr) {
        fb = 0;
    } else if (cur == gFrameBuffers[1].addr) {
        fb = 1;
    } else if (cur == gFrameBuffers[2].addr) {
        fb = 2;
    } else {
        fb = -1;
    }
    if (fb != gFbDisplayed) {
        if (gFbDisplayed >= 0) {
            gFrameBuffers[gFbDisplayed].free = TRUE;
        }
        gFbDisplayed = fb;
        if (gFbQueued >= 0) {
            gFbSwapping = gFbQueued;
            gFbQueued = -1;
            osViSwapBuffer(gFrameBuffers[gFbSwapping].addr);
        } else {
            gFbSwapping = -1;
        }
        schedKick();
    }

    if (gViCount & 1) {
        /* 30 fps: one new frame allowed every second retrace (max 2 banked) */
        if (++gFrameCredits >= 3) {
            gFrameCredits = 2;
        }
        contReadSchedule();
        gAudioTaskNew = audioBuildTask();
    }
}

/* 0x80104228 - start whatever should run next on the RSP */
void schedStartNext(void) {
    FrameBuffer *fb;
    s32 i;

    if (gAudioTaskRunning != NULL) {
        return;
    }
    if (gAudioTaskQueued != NULL) {
        gAudioTaskRunning = gAudioTaskQueued;
        gAudioTaskQueued = NULL;
        if (gGfxRunningSlot >= 0) {
            gGfxYielded = TRUE;
            gAudioTaskStarted = FALSE;
            osSpTaskYield();
            return;
        }
        gAudioTaskStarted = TRUE;
        schedStartTask(gAudioTaskRunning);
        return;
    }
    if (gGfxYielded) {
        gGfxYielded = FALSE;
        schedStartTask(&gGfxTask);
        return;
    }
    if (gFbRendering < 0 && gGfxRunningSlot < 0 && gGfxPendingSlot >= 0) {
        for (i = 0, fb = gFrameBuffers; i < 3; i++, fb++) {
            if (fb->free) {
                fb->free = FALSE;
                gGfxRunningSlot = gGfxPendingSlot;
                gFbRendering = i;
                gGfxPendingSlot = -1;
                gGfxTask.t.flags = 0;
                gfxPatchSlot(gGfxRunningSlot);
                schedStartTask(&gGfxTask);
                return;
            }
        }
    }
}

/* 0x80104384 - reset pressed: blank the screen and stop starting DMAs */
void schedOnPreNmi(void) {
    gPreNmi = TRUE;
    osViBlack(TRUE);
    resetCleanup();
}

/* 0x801043BC */
void schedKick(void) {
    osSendMesg(&gSchedMesgQueue, (OSMesg)SCHED_MSG_KICK, OS_MESG_NOBLOCK);
}

/* 0x801043E8 */
void schedStartTask(OSTask *task) {
    osWritebackDCacheAll();
    osSpTaskLoad(task);
    osSpTaskStartGo(task);
}

/* 0x8010441C - point the slot's display list at the target framebuffer
 * (segment 2) and attach it to the graphics task */
void gfxPatchSlot(s32 slotIdx) {
    GfxSlot *slot = &gGfxSlots[slotIdx];

    slot->dlStart[0] = 0xBC000806;          /* gsSPSegment(2, ...) */
    slot->dlStart[1] = gFramebuffers[gFbRendering];
    gGfxTask.t.data_ptr = (u64 *)slot->dlStart;
    gGfxTask.t.data_size = slot->dlUsed;
}

/* 0x80104484 - fatal "<buffer> OVERFLOW OF n BYTES" error */
void gfxReportOverflow(u32 dl, u32 mtx, u32 vtx, u32 vp) {
    char msg[0x40];
    char *name;
    s32 excess;

    if (dl > GFX_DL_SIZE) {
        name = D_8013B2C0;
        excess = dl - GFX_DL_SIZE;
    } else if (mtx > GFX_MTX_SIZE) {
        name = D_8013B2C8;
        excess = mtx - GFX_MTX_SIZE;
    } else if (vtx > GFX_VTX_SIZE) {
        name = D_8013B2CC;
        excess = vtx - GFX_VTX_SIZE;
    } else if (vp > GFX_VP_SIZE) {
        name = D_8013B2D0;
        excess = vp - GFX_VP_SIZE;
    } else {
        name = D_8013B2DC;
        excess = 0;
    }
    sprintf(msg, D_8013B2E0, name, excess);
    fatalError(0, msg);
}
