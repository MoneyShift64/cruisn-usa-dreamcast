/*
 * mathlib.c - the main segment's maths: table sine/cosine and arctangent on
 * 16-bit binary angles (0x10000 = full turn), and the 4x4 matrices the game
 * builds for the RSP. Game matrices are s32 16.16 fixed point, row-major as
 * the N64 Mtx expects them (m[row*4 + col], translation in m[12..14]);
 * mtxConvertRange turns a frame's worth into the RSP's split format.
 * Original code: main segment, ROM 0x14E80-0x15F40 and 0x163B0-0x166E0
 * (RAM 0x80113E80-0x80114F40, 0x801153B0-0x801156E0; the last file is
 * hand-written assembly, written here as C with the same results).
 */
#include "common.h"

typedef struct AtanOct {                 /* one per octant */
    s32 sign;                            /* +1 or -1 */
    s32 base;                            /* angle at the octant's start */
} AtanOct;

extern s16 gSinQ[];                      /* 0x80132CA0 - sin * 16384 for 0..0x4000, 1025 entries (step 16) */
extern f32 gAtanTbl[];                   /* 0x801334B0 - atan(i/128) as binary angles, 129 entries */
extern AtanOct gAtanOct[8];              /* 0x801336B4 */
extern s32 gMtxIdentity[16];             /* 0x80133750 */

f32  sqrtf(f32 x);                       /* 0x80115720 */
f32  tableLerp(f32 *table, f32 pos, s32 count); /* 0x80038B2C ovl2 */

/* 0x80113F38 - sine of an angle, table resolution (16 units) */
s32 sinLookup(s32 a) {
    s32 i = (a & 0xFFFF) >> 4;

    if (a & 0x4000) {
        if (a & 0x8000) {
            return -gSinQ[4096 - i];
        }
        return gSinQ[2048 - i];
    }
    if (a & 0x8000) {
        return -gSinQ[i - 2048];
    }
    return gSinQ[i];
}

/* 0x80113FDC - cosine of an angle, table resolution */
s32 cosLookup(s32 a) {
    s32 i = (a & 0xFFFF) >> 4;

    if (a & 0x4000) {
        if (a & 0x8000) {
            return gSinQ[i - 3072];
        }
        return -gSinQ[i - 1024];
    }
    if (a & 0x8000) {
        return -gSinQ[3072 - i];
    }
    return gSinQ[1024 - i];
}

/* 0x80113E80 - sin * 16384, interpolated between table entries */
s32 tblSin(s32 a) {
    s32 v0 = sinLookup(a);
    s32 v1 = sinLookup((a + 16) & 0xFFFF);

    return (((v1 - v0) * (a & 0xF) + 8) >> 4) + v0;
}

/* 0x80113EDC - cos * 16384, interpolated */
s32 tblCos(s32 a) {
    s32 v0 = cosLookup(a);
    s32 v1 = cosLookup((a + 16) & 0xFFFF);

    return (((v1 - v0) * (a & 0xF) + 8) >> 4) + v0;
}

/* 0x80114080 - a vector of length len pointing at angle: *x = cos, *z = sin */
void angleToVec(s32 a, f32 len, f32 *x, f32 *z) {
    len *= 1.0f / 16384.0f;
    *x = (f32)tblCos(a) * len;
    *z = (f32)tblSin(a) * len;
}

/* 0x801142A8 - fold (a, b) into the first octant: both positive, a >= b */
void atanOctant(s32 *oct, f32 *a, f32 *b) {
    f32 t;

    *oct = 0;
    if (*a < 0.0) {
        *oct = 2;
        *a = -*a;
    }
    if (*b < 0.0) {
        *oct += 4;
        *b = -*b;
    }
    if (*a < *b) {
        t = *a;
        *a = *b;
        *b = t;
        *oct += 1;
    }
}

/* 0x80114100 - binary angle of the vector (y, x) (table resolution) */
s32 atan2s(f32 y, f32 x) {
    s32 oct;

    atanOctant(&oct, &y, &x);
    if (y == 0.0) {
        return (oct < 4) ? 0xC000 : 0x4000;
    }
    return gAtanOct[oct].base + (s32)gAtanTbl[(s32)((x / y) * 128.0f)] * gAtanOct[oct].sign;
}

/* 0x801141D0 - as atan2s, interpolating the table */
s32 atan2Oct(f32 y, f32 x) {
    s32 oct;

    atanOctant(&oct, &y, &x);
    if (y == 0.0) {
        return (oct < 4) ? 0xC000 : 0x4000;
    }
    return gAtanOct[oct].base + (s32)tableLerp(gAtanTbl, (f32)((f64)(x / y) * 128.0), 0x81) * gAtanOct[oct].sign;
}

/* 0x80114350 - convert the matrices in [start, end) to the RSP format in place
 * (16 integer halves, then 16 fraction halves) */
void mtxToRsp(s32 *start, s32 *end) {
    union { s16 h[32]; s32 w[16]; } tmp;
    s32 i;

    for (; start < end; start += 16) {
        for (i = 0; i < 16; i++) {
            tmp.h[i] = (u32)start[i] >> 16;
            tmp.h[16 + i] = start[i];
        }
        for (i = 0; i < 16; i++) {
            start[i] = tmp.w[i];
        }
    }
}

/* 0x80114408 */
void mtxCopy(s32 *dst, s32 *src) {
    s32 i;

    for (i = 0; i < 16; i++) {
        dst[i] = src[i];
    }
}

/* 0x80114450 */
void mtxIdentity(s32 *m) {
    s32 i;

    for (i = 0; i < 16; i++) {
        m[i] = gMtxIdentity[i];
    }
}

/* 0x80114498 */
void mtxTranslate(s32 *m, s32 x, s32 y, s32 z) {
    mtxIdentity(m);
    m[12] = x;
    m[13] = y;
    m[14] = z;
}

/* 0x801144DC */
void mtxScale(s32 *m, s32 sx, s32 sy, s32 sz) {
    mtxIdentity(m);
    m[0] = sx;
    m[5] = sy;
    m[10] = sz;
}

/* 0x80114520 - camera matrix: the inverse of a placement at pos (16.16; x
 * mirrored) turned by angles[0..2] (x, y, z) */
void mtxView(s32 *m, s32 *pos, s32 *angles) {
    f32 px = -(f32)pos[0];
    f32 py = (f32)pos[1];
    f32 pz = (f32)pos[2];
    s32 sy = sinLookup((angles[1] + 8) & 0xFFFF);
    s32 sx = sinLookup((angles[0] + 8) & 0xFFFF);
    s32 sz = sinLookup((angles[2] + 8) & 0xFFFF);
    s32 cy = cosLookup((angles[1] + 8) & 0xFFFF);
    s32 cx = cosLookup((angles[0] + 8) & 0xFFFF);
    s32 cz = cosLookup((angles[2] + 8) & 0xFFFF);
    s32 sysz = (sy * sz) >> 14;
    s32 sycz = (sy * cz) >> 14;
    s32 t;

    m[0] = ((cx * cz) >> 12) + ((sysz * sx) >> 12);
    m[1] = ((sycz * sx) >> 12) - ((cx * sz) >> 12);
    m[6] = sy * -4;
    t = (sx * cy) >> 12;
    m[2] = t;
    m[4] = (sz * cy) >> 12;
    m[5] = (cz * cy) >> 12;
    m[3] = 0;
    m[7] = 0;
    m[11] = 0;
    m[8] = ((sysz * cx) >> 12) - t;
    m[9] = ((sycz * cx) >> 12) + ((sx * sz) >> 12);
    m[10] = (cy * cx) >> 12;
    m[15] = 0x10000;
    m[12] = (((px * (f32)m[0]) - (py * (f32)m[4])) - ((f32)m[8] * pz)) / 65536.0f;
    m[13] = (((px * (f32)m[1]) - (py * (f32)m[5])) - ((f32)m[9] * pz)) / 65536.0f;
    m[14] = (((px * (f32)m[2]) - (py * (f32)m[6])) - ((f32)m[10] * pz)) / 65536.0f;
}

/* 0x80114824 - rotation from angles (x, y, z), no translation */
void mtxFromAngles(s32 *m, s32 ax, s32 ay, s32 az) {
    s32 sy = sinLookup((ay + 8) & 0xFFFF);
    s32 sx = sinLookup((ax + 8) & 0xFFFF);
    s32 sz = sinLookup((az + 8) & 0xFFFF);
    s32 cy = cosLookup((ay + 8) & 0xFFFF);
    s32 cx = cosLookup((ax + 8) & 0xFFFF);
    s32 cz = cosLookup((az + 8) & 0xFFFF);
    s32 sysz, sycz;

    m[3] = 0;
    m[7] = 0;
    m[11] = 0;
    sysz = (sy * sz) >> 14;
    m[0] = ((cx * cz) >> 12) + ((sysz * sx) >> 12);
    m[1] = (cy * sz) >> 12;
    m[2] = ((sysz * cx) >> 12) - ((sx * cz) >> 12);
    sycz = (sy * cz) >> 14;
    m[4] = ((sycz * sx) >> 12) - ((cx * sz) >> 12);
    m[5] = (cy * cz) >> 12;
    m[9] = sy * -4;
    m[6] = ((sycz * cx) >> 12) + ((sx * sz) >> 12);
    m[15] = 0x10000;
    m[8] = (sx * cy) >> 12;
    m[10] = (cx * cy) >> 12;
}

/* 0x801149E0 */
void mtxTranslateFlip(s32 *m, s32 x, s32 y, s32 z) {
    mtxTranslate(m, x, y, z);
    m[10] = -1;
    m[0] = -1;
}

/* 0x80114A0C - look-at matrix: the view from eye towards at (16.16 positions),
 * level (world up is +y) */
void mtxBillboard(s32 *m, s32 *eye, s32 *at) {
    f32 ex = (f32)((f64)eye[0] / 65536.0);
    f32 ey = (f32)((f64)eye[1] / 65536.0);
    f32 ez = (f32)((f64)eye[2] / 65536.0);
    f32 dx = (f32)((f64)at[0] / 65536.0) - ex;
    f32 dy = (f32)((f64)at[1] / 65536.0) - ey;
    f32 dz = (f32)((f64)at[2] / 65536.0) - ez;
    f32 len2, k;
    f32 fx, fy, fz;                      /* forward (towards the eye) */
    f32 rx, rz;                          /* right */
    f32 ux, uy, uz;                      /* up */

    len2 = dx * dx + dy * dy + dz * dz;
    if (len2 == 0.0f) {
        k = -1.0f;
    } else {
        k = -1.0 / sqrtf(len2);
    }
    fx = dx * k;
    fy = dy * k;
    fz = dz * k;

    len2 = fz * fz + -fx * -fx;
    if (len2 == 0.0f) {
        k = 1.0f;
    } else {
        k = 1.0 / sqrtf(len2);
    }
    rx = fz * k;
    rz = -fx * k;

    ux = fy * rz;
    uy = fz * rx - fx * rz;
    uz = -fy * rx;
    len2 = ux * ux + uy * uy + uz * uz;
    if (len2 == 0.0f) {
        k = 1.0f;
    } else {
        k = 1.0 / sqrtf(len2);
    }
    ux *= k;
    uz *= k;
    uy *= k;

    m[4] = 0;
    m[0] = rx * 65536.0f;
    m[8] = rz * 65536.0f;
    m[12] = (ex * rx + ez * rz) * -65536.0f;
    m[1] = ux * 65536.0f;
    m[5] = uy * 65536.0f;
    m[9] = uz * 65536.0f;
    m[3] = 0;
    m[7] = 0;
    m[11] = 0;
    m[15] = 0x10000;
    m[13] = (ex * ux + ey * uy + ez * uz) * -65536.0f;
    m[2] = fx * 65536.0f;
    m[6] = fy * 65536.0f;
    m[10] = fz * 65536.0f;
    m[14] = (ex * fx + ey * fy + ez * fz) * -65536.0f;
}

/* 0x80114DDC - turn about y and translate */
void mtxRotYTranslate(s32 *m, s32 angle, s32 x, s32 y, s32 z) {
    s32 a = (angle + 8) & 0xFFFF;
    s32 s = sinLookup(a) * 4;
    s32 c = cosLookup(a) * 4;

    m[0] = c;
    m[10] = c;
    m[2] = -s;
    m[8] = s;
    m[12] = x;
    m[13] = y;
    m[5] = 0x10000;
    m[15] = 0x10000;
    m[4] = 0;
    m[1] = 0;
    m[9] = 0;
    m[6] = 0;
    m[3] = 0;
    m[7] = 0;
    m[11] = 0;
    m[14] = z;
}

/* 0x80114E84 - a road section's placement (xf: cos, sin, x, -, -, z) at height y */
void mtxSection(s32 *m, f32 *xf, f32 y) {
    s32 c = xf[0] * 65536.0f;
    s32 s;

    m[10] = c;
    m[0] = c;
    s = xf[1] * 65536.0f;
    m[2] = -s;
    m[8] = s;
    m[12] = xf[2] * 65536.0f;
    m[13] = y * 65536.0f;
    m[5] = 0x10000;
    m[15] = 0x10000;
    m[4] = 0;
    m[1] = 0;
    m[9] = 0;
    m[6] = 0;
    m[3] = 0;
    m[7] = 0;
    m[11] = 0;
    m[14] = xf[5] * 65536.0f;
}

/* 16.16 product */
#define FMUL(a, b) ((s32)(((s64)(a) * (s64)(b)) >> 16))

/* 0x801153B0 - convert the frame's matrices in [start, end) to the RSP
 * format, each moving down one slot (the display list points there) */
void mtxConvertRange(s32 *start, s32 *end) {
    u16 *out = (u16 *)(start - 16);
    s32 i;

    do {
        for (i = 0; i < 16; i++) {
            u32 v = start[i];
            out[i] = v >> 16;
            out[16 + i] = v;
        }
        start += 16;
        out += 32;
    } while (start < end);
}

/* 0x8011545C - scale the rows */
void mtxScaleBy(s32 *m, s32 sx, s32 sy, s32 sz) {
    m[0] = FMUL(m[0], sx);
    m[1] = FMUL(m[1], sx);
    m[2] = FMUL(m[2], sx);
    m[4] = FMUL(m[4], sy);
    m[5] = FMUL(m[5], sy);
    m[6] = FMUL(m[6], sy);
    m[8] = FMUL(m[8], sz);
    m[9] = FMUL(m[9], sz);
    m[10] = FMUL(m[10], sz);
}

/* 0x80115590 - out = v transformed by m (16.16) */
void mtxMulVec(s32 *out, s32 *m, s32 *v) {
    out[0] = m[12] + FMUL(m[0], v[0]) + FMUL(m[4], v[1]) + FMUL(m[8], v[2]);
    out[1] = m[13] + FMUL(m[1], v[0]) + FMUL(m[5], v[1]) + FMUL(m[9], v[2]);
    out[2] = m[14] + FMUL(m[2], v[0]) + FMUL(m[6], v[1]) + FMUL(m[10], v[2]);
}

/* 0x80115580 - out = m's rotation applied to v (no translation) */
void mtxRotVec(s32 *out, s32 *m, s32 *v) {
    out[0] = FMUL(m[0], v[0]) + FMUL(m[4], v[1]) + FMUL(m[8], v[2]);
    out[1] = FMUL(m[1], v[0]) + FMUL(m[5], v[1]) + FMUL(m[9], v[2]);
    out[2] = FMUL(m[2], v[0]) + FMUL(m[6], v[1]) + FMUL(m[10], v[2]);
}
