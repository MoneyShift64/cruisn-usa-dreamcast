/*
 * road.c - the route: building it from the track data, and finding where a
 * car is on it. Main segment, ROM 0x1E120-0x1E3E0 and 0x1F970-0x20260.
 *
 * See include/road.h and NOTES.md ("Road data") for the data format.
 */
#include "common.h"
#include "race.h"
#include "road.h"

extern Xform2 gXf2Identity;              /* 0x80158280 - result buffers when out == NULL */
extern Xform2 gXf2Rotate;                /* 0x80158298 */
extern Xform2 gXf2Mul;                   /* 0x801582B0 */
extern Xform2 gXf2RotateBy;              /* 0x801582C8 */

/* ------------------------------------------------------------------ */
/* 2D transforms (ROM 0x1E120)                                         */
/* ------------------------------------------------------------------ */

/* 0x8011D120 */
Xform2 *xf2Identity(Xform2 *m) {
    if (m == NULL) {
        m = &gXf2Identity;
    }
    (*m)[5] = 0.0f;
    (*m)[3] = 0.0f;
    (*m)[2] = 0.0f;
    (*m)[1] = 0.0f;
    (*m)[4] = 1.0f;
    (*m)[0] = 1.0f;
    return m;
}

/* 0x8011D15C - rotation by a binary angle (0x10000 = full turn), then translation */
Xform2 *xf2Rotate(Xform2 *m, s32 angle, f32 tx, f32 tz) {
    f32 c;
    f32 s;

    if (m == NULL) {
        m = &gXf2Rotate;
    }
    c = (f32)tblCos(angle) * (1.0f / 16384.0f);
    s = (f32)tblSin(angle) * (1.0f / 16384.0f);
    (*m)[4] = c;
    (*m)[0] = c;
    (*m)[1] = s;
    (*m)[3] = -s;
    (*m)[2] = tx;
    (*m)[5] = tz;
    return m;
}

/* 0x8011D200 - out = a applied after b */
Xform2 *xf2Mul(Xform2 *out, Xform2 *a, Xform2 *b) {
    f32 r[6];

    if (out == NULL) {
        out = &gXf2Mul;
    }
    r[0] = (*b)[3] * (*a)[1] + (*a)[0] * (*b)[0];
    r[1] = (*b)[4] * (*a)[1] + (*a)[0] * (*b)[1];
    r[2] = (*a)[2] + ((*a)[0] * (*b)[2] + (*a)[1] * (*b)[5]);
    r[3] = (*b)[3] * (*a)[4] + (*a)[3] * (*b)[0];
    r[4] = (*b)[4] * (*a)[4] + (*a)[3] * (*b)[1];
    r[5] = (*a)[5] + ((*a)[3] * (*b)[2] + (*a)[4] * (*b)[5]);
    (*out)[0] = r[0];
    (*out)[1] = r[1];
    (*out)[2] = r[2];
    (*out)[3] = r[3];
    (*out)[4] = r[4];
    (*out)[5] = r[5];
    return out;
}

/* 0x8011D330 */
Xform2 *xf2RotateBy(Xform2 *m, s32 angle, f32 tx, f32 tz) {
    Xform2 tmp;

    if (m == NULL) {
        m = &gXf2RotateBy;
    }
    return xf2Mul(m, m, xf2Rotate(&tmp, angle, tx, tz));
}

/* 0x8011D388 */
void xf2Apply(Xform2 *m, f32 *x, f32 *z) {
    f32 px = *x;
    f32 pz = *z;

    *x = (*m)[2] + (px * (*m)[0] + pz * (*m)[1]);
    *z = (*m)[5] + (px * (*m)[3] + pz * (*m)[4]);
}

/* ------------------------------------------------------------------ */
/* The route (ROM 0x1F970)                                             */
/* ------------------------------------------------------------------ */

/* 0x8011E970 - lay the sections end to end and collect every route marker
 * into gPathPoints. numSections = 0 means the whole track.
 */
void routeBuild(s32 numSections) {
    SectionInfo *sec;
    ModelRef *models;
    s32 count;
    s32 angle;
    f32 ox;
    f32 oz;
    f32 oy;
    Xform2 m;
    PathPoint *pt;
    SectionModel *model;
    ModelItem *item;
    s32 n;
    s32 i;
    s32 s;
    u8 lanes;
    f32 nx;
    f32 nz;

    gPathLength = 0;
    pt = gPathPoints;
    oz = 0.0f;
    sec = gTrack->sections;
    oy = 0.0f;
    ox = 0.0f;
    models = gTrack->models;
    if (numSections != 0) {
        count = numSections;
    } else {
        count = gTrack->numSections;
    }
    s = 0;
    if (count > 0) {
        angle = 0;
        do {
            i = 0;
            xf2Rotate(&m, -angle, ox, oz);
            lanes = 2;
            model = models[sec->model].model;
            item = model->items;
            n = model->numItems;
            if (sec->flags & 0x40) {
                lanes = 4;
            }
            if (n > 0) {
                do {
                    if ((item->flags & 0xF00) == 0xB00) {
                        pt->x = (f32)(item->x >> 16);
                        pt->y = (f32)(item->y >> 16);
                        pt->section = s;
                        pt->index = i;
                        pt->z = (f32)(item->z >> 16);
                        xf2Apply(&m, &pt->x, &pt->z);
                        pt->lanes = lanes;
                        pt->y = pt->y + oy;
                        pt++;
                        gPathLength++;
                    }
                    i++;
                    item++;
                } while (i != n);
            }
            nx = (f32)sec->dx * 0.2f;
            oy += (f32)sec->dy * 0.2f;
            nz = (f32)sec->dz * 0.2f;
            xf2Apply(&m, &nx, &nz);
            s++;
            angle += sec->turn;
            sec++;
            ox = nx;
            oz = nz;
        } while (s != count);
    }
    routeSetZones();
}

/* 0x8011EBE0 - find the nearest route segment to world (x, z), searching from
 * rp->point forwards (or backwards for oncoming traffic, reverse != 0) and
 * turning round if the start was already past the position.
 */
void roadLocate(RoadPos *rp, f32 x, f32 z, s32 reverse) {
    s32 i;
    s32 step;
    s32 n;
    s32 back;
    s32 start;
    PathPoint *a;
    PathPoint *b;
    f32 t;

    x *= 0.2f;
    t = 0.0f;
    i = rp->point;
    z *= -0.2f;
    if (reverse) {
        n = i - 2;
        step = -1;
        back = gPathLength - i - 2;
    } else {
        back = i - 2;
        step = 1;
        n = gPathLength - i - 2;
    }
    start = i;
    a = &gPathPoints[i];
    b = a + step;
    while (n-- > 0) {
        t = segmentParam(x, z, a->x, a->z, b->x, b->z);
        if (t > 1.0f && i < gPathLength - 3) {
            a = b;                           /* past the end of this segment: next one */
        } else if (t < -1.0f) {
            step = -step;                    /* behind the start: search the other way */
            n = back;
            back = 0;
            i = start - step;
            a = b = &gPathPoints[start];
        } else {
            break;
        }
        b += step;
        i += step;
    }
    rp->point = i;
    rp->t = t;
    rp->pos[0] = (a->x + t * (b->x - a->x)) * 5.0f;
    rp->pos[1] = (a->y + t * (b->y - a->y)) * 5.0f;
    rp->pos[2] = (a->z + t * (b->z - a->z)) * -5.0f;
    rp->heading = roadHeadingAt(i);
    if (n > 0 && step > 0) {
        b++;
    }
    rp->aim = ((f32)(s16)(0xC000 - atan2s(b->x - x, b->z - z)) * 3.1415927f) / 32768.0f;
}

/* 0x8011EEA0 - which side of the route the position is on */
s32 roadIsLeft(RoadPos *rp) {
    s32 r = 0;

    if (((f32)(s16)(s32)(((rp->aim - rp->heading) * 32768.0f) / 3.1415927f) * 3.1415927f) / 32768.0f >= 0.0f) {
        r = 1;
    }
    return r;
}

/* 0x8011EF10 - the last path point inside the sections loaded for this player */
s32 roadCurrentPoint(s32 player) {
    SectionWindow *w = &gSectionWindows[player];
    s32 i = w->road.point;
    s32 last = w->first + w->count - 1;

    while (i < gPathLength && gPathPoints[i].section <= last) {
        i++;
    }
    return i - 1;
}

/* 0x8011EFA0 - heading of the route at a point (radians, 0 = +z ... see atan2s) */
f32 roadHeadingAt(s32 point) {
    PathPoint *p;

    if (point >= gPathLength - 1) {
        point = gPathLength - 2;
    }
    p = &gPathPoints[point];
    return ((f32)(s16)(0xC000 - atan2s(p[1].x - p[0].x, p[1].z - p[0].z)) * 3.1415927f) / 32768.0f;
}

/* 0x8011F034 - project (px, pz) onto the line a->b: 0 at a, 1 at b */
f32 segmentParam(f32 px, f32 pz, f32 ax, f32 az, f32 bx, f32 bz) {
    f32 dx;
    f32 dz;
    f32 len;
    f32 inv;
    f32 ux;
    f32 uz;
    f32 nx;
    f32 d;
    f32 qx;
    f32 qz;

    dx = bx - ax;
    dz = bz - az;
    len = sqrtf(dx * dx + dz * dz);
    if (len == 0.0f) {
        return 0.0f;
    }
    inv = 1.0f / len;
    ux = dx * inv;
    uz = dz * inv;
    nx = -uz;
    d = (nx * px + ux * pz) - (ax * nx + az * ux);
    qx = px - d * nx;                        /* foot of the perpendicular */
    qz = pz - d * ux;
    if (bx == ax) {
        if (bz == az) {
            return 0.0f;
        }
        return (qz - az) / dz;
    }
    return (qx - ax) / dx;
}

/* 0x8011F188 - copy the per-stretch values from gRouteZones into every path point */
void routeSetZones(void) {
    s32 idx;
    s32 lastSec;
    PathPoint *pt;
    s32 i;
    RouteZone *z;

    idx = 0;
    lastSec = -1;
    pt = gPathPoints;
    for (i = 0; i < gPathLength; i++, pt++) {
        if (i != 0) {
            if (lastSec < pt->section) {
                lastSec = pt->section;
                idx = -1;
            }
            idx++;
            if (z[1].section < pt->section || (pt->section == z[1].section && idx >= z[1].point)) {
                z++;
            }
        } else {
            z = gRouteZones[gCurrentTrack];
        }
        pt->zoneA = z->a;
        pt->zoneB = z->b;
    }
}
