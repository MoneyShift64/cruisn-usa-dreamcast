/*
 * xform.c - positions of 2D elements (Xform: 16.16 position, angles, scale,
 * visibility) and drawing display lists at them.
 * Original code: main segment, ROM 0x15F40-0x163B0 (RAM 0x80114F40-0x801153B0).
 */
#include "common.h"
#include "frontend.h"

extern u32 *gDlCursor;                   /* 0x80147684 */
extern s32 *gMtxCursor;                  /* 0x80147688 */

void mtxTranslate(s32 *m, s32 x, s32 y, s32 z);        /* 0x80114498 */
void mtxFromAngles(s32 *m, s32 ax, s32 ay, s32 az);    /* 0x80114824 */
void mtxScaleBy(s32 *m, s32 sx, s32 sy, s32 sz);       /* 0x8011545C */

#define DL_PUSH(w0, w1) { u32 *_g = gDlCursor; gDlCursor = _g + 2; _g[0] = (w0); _g[1] = (w1); }


/* 0x80114F40 - (re)initialise a position */
Xform *textPlace(Xform *xf, s32 x, s32 y, s32 z, s32 visible) {
    xf->x = x;
    xf->y = y;
    xf->z = z;
    xf->unk14 = 0;
    xf->unk10 = 0;
    xf->unk0C = 0;
    xf->scale = 0x10000;
    xf->visible = visible;
    return xf;
}

/* 0x80114F74 - apply a {cmd, index, value} table to an Xform array */
void xformScriptRun(void *script, Xform *xf) {
    XformCmd *cmd = (XformCmd *)script;

    while (cmd->cmd != 9) {
        switch (cmd->cmd) {
            case 0:
                textPlace(&xf[cmd->index], 0, 0, 0, cmd->value);
                break;
            case 1:
                xf[cmd->index].x = cmd->value;
                break;
            case 2:
                xf[cmd->index].y = cmd->value;
                break;
            case 3:
                xf[cmd->index].z = cmd->value;
                break;
            case 4:
                xf[cmd->index].unk0C = cmd->value;
                break;
            case 5:
                xf[cmd->index].unk10 = cmd->value;
                break;
            case 6:
                xf[cmd->index].unk14 = cmd->value;
                break;
            case 7:
                xf[cmd->index].scale = cmd->value;
                break;
            case 8:
                xf[cmd->index].visible = cmd->value;
                break;
        }
        cmd++;
    }
}

/* 0x8011508C - push a matrix for a position (angles in the low 16 bits) */
void xformsUpdate(Xform *xf) {
    s32 *m = gMtxCursor;

    gMtxCursor = m + 16;
    if ((xf->unk14 | xf->unk0C | xf->unk10) & 0xFFFF) {
        mtxFromAngles(m, xf->unk0C, xf->unk10, xf->unk14);
        m[12] = xf->x;
        m[13] = xf->y;
        m[14] = xf->z;
    } else {
        mtxTranslate(m, xf->x, xf->y, xf->z);
    }
    if (xf->scale != 0x10000) {
        mtxScaleBy(m, xf->scale, xf->scale, xf->scale);
    }
    DL_PUSH(0x01040040, (u32)(m - 16));  /* gSPMatrix push (the RSP copy is one slot down) */
}

/* 0x80115164 - draw a display list at a position */
void drawDLXform(void *dl, Xform *xf) {
    xformsUpdate(xf);
    DL_PUSH(0x06000000, (u32)dl);
    DL_PUSH(0xBD000000, 0);              /* gSPPopMatrix */
}

/* 0x801151C0 - draw display lists by a byte script: 0xFF end, 0xFE z-buffered
 * mode, 0xFD back to 2D, 0x7F pop (or close a skipped group), 0x80|n draw
 * dls[n], otherwise push xf[n] (a hidden one skips everything up to its pop) */
void drawDLScript(void *scriptp, Xform *xf, void **dls, s32 count) {
    u8 *script = (u8 *)scriptp;
    s32 skip = 0;
    s32 i;
    u8 c;

    for (i = 0; i < count; i++, script++) {
        c = *script;
        if (c == 0xFF) {
            return;
        }
        if (c == 0xFE) {
            DL_PUSH(0xB7000000, 1);           /* G_ZBUFFER on */
            DL_PUSH(0xB900031D, 0x553078);    /* render mode */
        } else if (c == 0xFD) {
            DL_PUSH(0xB900031D, 0x504240);
            DL_PUSH(0xB6000000, 1);
        } else if (c == 0x7F) {
            if (skip != 0) {
                skip--;
            } else {
                DL_PUSH(0xBD000000, 0);
            }
        } else if (c & 0x80) {
            if (skip == 0) {
                DL_PUSH(0x06000000, (u32)dls[*script & 0x7F]);
            }
        } else if (skip != 0 || xf[c].visible == 0) {
            skip++;
        } else {
            xformsUpdate(&xf[c]);
        }
    }
}
