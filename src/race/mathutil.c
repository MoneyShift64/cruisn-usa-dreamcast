/*
 * race/mathutil.c - angle, vector and 3x3 matrix helpers used by the race code.
 * Original code: race overlay, ROM 0x50DE0-0x51740 (RAM 0x80038370-0x80038CD0).
 *
 * Angles are floats in radians. The N64 sine table uses 16-bit "binary angles"
 * (0x10000 = 360 degrees), so radians are converted with  x * 32768 / PI.
 * Matrices are 3x3 floats, row-major: Mtx3[row][col].
 */
#include "common.h"
#include "race.h"
#include "road.h"

/* 0x80038370 ovl2 - wrap an angle to (-PI, PI] */
f32 angleWrap(f32 a) {
    return ((f32)(s16)(s32)((a * 32768.0f) / 3.1415927f) * 3.1415927f) / 32768.0f;
}

/* 0x800383B8 ovl2 - wrap an angle to [0, 2*PI) */
f32 angleWrapPositive(f32 a) {
    return ((f32)(u32)((s32)((a * 32768.0f) / 3.1415927f) & 0xFFFF) * 3.1415927f) / 32768.0f;
}

/* 0x8003840C ovl2 - 2D distance */
f32 dist2D(f32 x1, f32 y1, f32 x2, f32 y2) {
    x1 -= x2;
    y1 -= y2;
    return sqrtf(x1 * x1 + y1 * y1);
}

/* 0x80038458 ovl2 */
f32 sinf_tbl(f32 a) {
    return tblSin((s32)((a * 32768.0f) / 3.1415927f)) * (1.0f / 16384.0f);
}

/* 0x800384AC ovl2 */
f32 cosf_tbl(f32 a) {
    return tblCos((s32)((a * 32768.0f) / 3.1415927f)) * (1.0f / 16384.0f);
}

/* 0x80038500 ovl2 */
void bytesClear(u8 *p, s32 n) {
    u8 *q = p;

    while (n-- != 0) {
        *q++ = 0;
    }
}

/* 0x8003852C ovl2 */
void mtx3Identity(Mtx3 m) {
    m[2][2] = 1.0f;
    m[1][1] = 1.0f;
    m[0][0] = 1.0f;
    m[2][1] = 0.0f;
    m[2][0] = 0.0f;
    m[1][2] = 0.0f;
    m[1][0] = 0.0f;
    m[0][2] = 0.0f;
    m[0][1] = 0.0f;
}

/* 0x80038568 ovl2 - rotation about Y */
void mtx3RotY(Mtx3 m, f32 a) {
    f32 s;

    m[0][0] = m[2][2] = cosf_tbl(a);
    s = sinf_tbl(a);
    m[2][0] = s;
    m[0][2] = -s;
    m[2][1] = 0.0f;
    m[1][2] = 0.0f;
    m[1][0] = 0.0f;
    m[0][1] = 0.0f;
    m[1][1] = 1.0f;
}

/* 0x800385D4 ovl2 - rotation about X */
void mtx3RotX(Mtx3 m, f32 a) {
    f32 s;

    m[1][1] = m[2][2] = cosf_tbl(a);
    s = sinf_tbl(a);
    m[2][1] = s;
    m[1][2] = -s;
    m[2][0] = 0.0f;
    m[1][0] = 0.0f;
    m[0][2] = 0.0f;
    m[0][1] = 0.0f;
    m[0][0] = 1.0f;
}

void mtx3MulRaw(Mtx3 out, Mtx3 a, Mtx3 b);
void mtx3Copy(Mtx3 dst, Mtx3 src);

/* 0x80038640 ovl2 - dst = b * a  (dst may alias a or b) */
void mtx3Mul(Mtx3 dst, Mtx3 a, Mtx3 b) {
    Mtx3 tmp;

    mtx3MulRaw(tmp, a, b);
    mtx3Copy(dst, tmp);
}

/* 0x80038670 ovl2 - out = b * a  (out must not alias) */
void mtx3MulRaw(Mtx3 out, Mtx3 a, Mtx3 b) {
    s32 i = 3;

    while (i-- != 0) {
        out[0][0] = b[0][0] * a[0][0] + b[0][1] * a[1][0] + a[2][0] * b[0][2];
        out[0][1] = b[0][0] * a[0][1] + b[0][1] * a[1][1] + a[2][1] * b[0][2];
        out[0][2] = b[0][0] * a[0][2] + b[0][1] * a[1][2] + a[2][2] * b[0][2];
        out++;
        b++;
    }
}

/* 0x80038720 ovl2 */
void mtx3Copy(Mtx3 dst, Mtx3 src) {
    f32 *d = &dst[0][0];
    f32 *s = &src[0][0];
    s32 i = 9;

    while (i-- != 0) {
        *d++ = *s++;
    }
}

/* 0x80038748 ovl2 */
void mtx3Transpose(Mtx3 dst, Mtx3 src) {
    dst[0][0] = src[0][0];
    dst[0][1] = src[1][0];
    dst[0][2] = src[2][0];
    dst[1][0] = src[0][1];
    dst[1][1] = src[1][1];
    dst[1][2] = src[2][1];
    dst[2][0] = src[0][2];
    dst[2][1] = src[1][2];
    dst[2][2] = src[2][2];
}

/* 0x8003879C ovl2 - out = v * m (row vector times matrix) */
void vec3MulMtx(Vec3f out, Vec3f v, Mtx3 m) {
    f32 x = v[0];
    f32 y = v[1];
    f32 z = v[2];

    out[0] = x * m[0][0] + y * m[0][1] + m[0][2] * z;
    out[1] = x * m[1][0] + y * m[1][1] + m[1][2] * z;
    out[2] = x * m[2][0] + y * m[2][1] + m[2][2] * z;
}

/* 0x8003881C ovl2 - out = (0, 0, len) rotated by m: a vector pointing "forward" */
void vec3Forward(Vec3f out, f32 len, Mtx3 m) {
    out[0] = 0.0f;
    out[1] = 0.0f;
    out[2] = len;
    vec3MulMtx(out, out, m);
}

/* 0x80038850 ovl2 */
void vec3Add(Vec3f out, Vec3f a, Vec3f b) {
    out[0] = b[0] + a[0];
    out[1] = b[1] + a[1];
    out[2] = b[2] + a[2];
}

/* 0x80038894 ovl2 */
void vec3Normalize(Vec3f v) {
    f32 len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    f32 inv;

    if (len > 0.00001f) {
        inv = 1.0f / len;
        v[0] = v[0] * inv;
        v[1] = v[1] * inv;
        v[2] = v[2] * inv;
    }
}

/* 0x8003892C ovl2 - normal of the triangle (a, b, c): (a - b) x (c - b) */
void vec3TriNormal(Vec3f out, Vec3f a, Vec3f b, Vec3f c) {
    f32 ax = a[0] - b[0];
    f32 ay = a[1] - b[1];
    f32 az = a[2] - b[2];
    f32 cx = c[0] - b[0];
    f32 cy = c[1] - b[1];
    f32 cz = c[2] - b[2];

    out[0] = ay * cz - az * cy;
    out[1] = az * cx - ax * cz;
    out[2] = ax * cy - ay * cx;
}

/* 0x800389D4 ovl2 - extract binary angles (pitch, yaw, roll) from a matrix */
void mtx3ToAngles(Mtx3 m, s32 *outX, s32 *outY, s32 *outZ) {
    f32 m00 = m[0][0];
    f32 m10 = m[1][0];
    f32 m20 = m[2][0];
    f32 m02 = m[0][2];
    f32 m12 = m[1][2];
    f32 m22 = m[2][2];
    f32 lenXZ2 = sqrtf(m02 * m02 + m22 * m22);
    f32 lenXZ0 = sqrtf(m00 * m00 + m20 * m20);

    *outY = atan2s(lenXZ2, m12);
    if (m[1][1] > 0.0f) {
        *outZ = atan2s(m02, m22);
        *outX = atan2s(lenXZ0, m10);
    } else if (m[1][1] < 0.0f) {
        *outZ = atan2s(m02, m22);
        *outX = atan2s(lenXZ0, m10) + 0x8000;
    } else {
        *outZ = atan2s(m00, m20) + 0x4000;
        *outX = atan2s(lenXZ0, m10);
    }
}

/* 0x80038B2C ovl2 - sample a float table at a fractional index (clamped) */
f32 tableLerp(f32 *table, f32 pos, s32 count) {
    s32 i = (s32)pos;
    f32 t;

    if (i <= 0) {
        return table[0];
    }
    if (i >= count - 1) {
        return table[count - 1];
    }
    t = pos - (f32)i;
    return table[i + 1] * t + table[i] * (1.0f - t);
}

/* 0x80038BA8 ovl2 - signed sideways offset of a point relative to a player's camera */
f32 cameraSideOffset(f32 x, f32 z, s32 player) {
    RaceCamera *cam = &gRaceCameras[player];
    f32 yaw = cam->yaw;
    f32 s;

    z += cam->posZ * 5.0f;
    x -= cam->posX * 5.0f;
    s = sinf_tbl(yaw);
    return cosf_tbl(yaw) * z + -s * x;
}

/* 0x80038C3C ovl2 - absolute difference of two binary angles (0..0x8000) */
s32 angleDiff(s16 a, s16 b) {
    s32 d = (a - b) & 0xFFFF;
    s32 r = d;

    if (d > 0x8000) {
        r = -d & 0xFFFF;
    }
    return r;
}

/* 0x80038C84 ovl2 - a / b, or a if b is (nearly) zero */
f32 safeDiv(f32 a, f32 b) {
    if (b < 0.00001f && b > -0.00001f) {
        return a;
    }
    return a / b;
}
