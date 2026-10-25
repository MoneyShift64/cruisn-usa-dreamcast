/*
 * anim.c - timed animations: "ramps" that run t between 0 and 1 over a given
 * time (30 fps), and events attached to them that fire as t passes given
 * points. animStart() attaches a script of value tracks that eases fields of
 * a target (positions, colours...) along t: this drives the menus and the
 * race HUD. Also: delays (animWait), flashing (animBlink and the three
 * looping blink ramps text objects use).
 * Original code: main segment, ROM 0x13A90-0x14E80 (RAM 0x80112A90-0x80113E80).
 */
#include "common.h"
#include "frontend.h"

typedef struct EaseCurve {               /* 0x10 bytes, 16 at 0x80132BA0 */
    /* 0x00 */ f32 accel;                /* t <= accelEnd: accel * t^2 */
    /* 0x04 */ f32 decel;                /* t >= decelStart: 1 - decel * (1 - t)^2 */
    /* 0x08 */ f32 accelEnd;             /* (computed by easeInit) */
    /* 0x0C */ f32 decelStart;           /* in between: linear */
} EaseCurve;

extern AnimRamp gAnimRamps[50];          /* 0x8014E3B0 */
extern AnimSlot gAnimSlots[100];         /* 0x8014E798 */
extern s32 gBlinkSlow;                   /* 0x8014EF70 */
extern s32 gBlinkMedium;                 /* 0x8014EF74 */
extern s32 gBlinkFast;                   /* 0x8014EF78 */
extern EaseCurve gEaseCurves[16];        /* 0x80132BA0 */

f32  sqrtf(f32 x);                       /* 0x80115720 */

s32  rampAlloc(s32 ramp, f32 t, f32 step, u8 flags); /* 0x80112A90 */
s32  animAddEvent(s32 ramp, AnimDoneFunc func, f32 at, u8 flags); /* 0x80112B74 */
void eventRemove(s16 event);             /* 0x80112C40 */
void rampClearEvents(s32 ramp);          /* 0x80112CBC */
s32  animScriptEvent(s16 event, s32 ramp, f32 t); /* 0x80112E84 */
void animScript(AnimCmd *s, u8 *target, f32 t); /* 0x80112EE4 */
void easeInit(void);                     /* 0x80113590 */
s32  rampFindFree(void);                 /* 0x8011364C */
s16  eventFindFree(void);                /* 0x8011372C */
void rampFireEvents(s32 ramp, f32 prevT); /* 0x801137C4 */
s32  eventFire(s32 event);               /* 0x80113A44 */
s32  waitEvent(s32 event, s32 ramp, f32 t); /* 0x80113D18 */
s32  blinkEvent(s32 event, s32 ramp, f32 t); /* 0x80113DE0 */

/* 0x80112A90 - set up a ramp (ramp == -1: a new one); returns it or -1 */
s32 rampAlloc(s32 ramp, f32 t, f32 step, u8 flags) {
    AnimRamp *r;

    if (ramp == -1) {
        ramp = rampFindFree();
        if (ramp == -1) {
            return -1;
        }
        gAnimRamps[ramp].first = -1;
    } else {
        rampClearEvents(ramp);
    }
    r = &gAnimRamps[ramp];
    r->t = t;
    r->step = step;
    r->flags = flags;
    return ramp;
}

/* 0x80112B38 */
void animCancel(s32 ramp) {
    rampClearEvents(ramp);
    gAnimRamps[ramp].used = 0;
}

/* 0x80112B74 - attach an event (at the head of the ramp's list) */
s32 animAddEvent(s32 ramp, AnimDoneFunc func, f32 at, u8 flags) {
    s16 e = eventFindFree();
    AnimSlot *ev;
    AnimRamp *r;
    s16 next;

    if (e == -1) {
        return -1;
    }
    ev = &gAnimSlots[e];
    ev->prev = -1;
    r = &gAnimRamps[ramp];
    next = r->first;
    ev->next = next;
    if (next != -1) {
        gAnimSlots[next].prev = e;
    }
    r->first = e;
    ev = &gAnimSlots[e];
    ev->ramp = ramp;
    ev->func = func;
    ev->at = at;
    ev->flags = flags;
    return e;
}

/* 0x80112C40 */
void eventRemove(s16 event) {
    AnimSlot *ev = &gAnimSlots[event];
    s16 prev = ev->prev;
    s16 next = ev->next;

    if (prev == -1) {
        gAnimRamps[ev->ramp].first = next;
    } else {
        gAnimSlots[prev].next = next;
    }
    if (next != -1) {
        gAnimSlots[next].prev = prev;
    }
    ev->used = 0;
}

/* 0x80112CBC */
void rampClearEvents(s32 ramp) {
    AnimRamp *r = &gAnimRamps[ramp];

    while (r->first != -1) {
        eventRemove(r->first);
    }
}

/* 0x80112D24 - run 'script' on 'target' over 'seconds'; 'done' is called
 * at the end. Returns done's event, or the ramp when there is no 'done'. */
s32 animStart(f32 seconds, void *script, void *target, AnimDoneFunc done, u8 flags) {
    s32 ramp, ev, sev;
    AnimRamp *r;

    ramp = rampAlloc(-1, (flags & ANIM_REVERSE) != 0, 1.0f / (seconds * 30.0f), (flags | ANIM_PAUSED) & 0xFF);
    if (ramp == -1) {
        return -1;
    }
    if (done != NULL) {
        ev = animAddEvent(ramp, done, 0.0f, EV_END);
        if (ev == -1) {
            animCancel(ramp);
            return -1;
        }
    }
    sev = (s16)animAddEvent(ramp, (AnimDoneFunc)animScriptEvent, 0.0f, EV_CHANGED);
    if (sev == -1) {
        animCancel(ramp);
        return -1;
    }
    r = &gAnimRamps[ramp];
    r->user = (s32)target;
    gAnimSlots[sev].user = (s32)script;
    animScriptEvent(sev, ramp, r->t);                 /* apply the starting values */
    r->flags = flags;
    return (done != NULL) ? ev : ramp;
}

/* 0x80112E84 */
s32 animScriptEvent(s16 event, s32 ramp, f32 t) {
    animScript((AnimCmd *)gAnimSlots[event].user, (u8 *)gAnimRamps[ramp].user, t);
    return 0;
}

/* 0x80112EE4 - apply a script at time t */
void animScript(AnimCmd *s, u8 *target, f32 t) {
    EaseCurve *e;
    f32 f;
    s32 v, ok;
    u8 *dst;

    while (s->cmd != 0) {
        switch (s->cmd) {
            case 1:                                       /* eased track */
                e = &gEaseCurves[s->d];
                if (t <= e->accelEnd) {
                    f = e->accel * t * t;
                } else if (e->decelStart <= t) {
                    f = 1.0f - (1.0f - t) * e->decel * (1.0f - t);
                } else {
                    f = e->accel * e->accelEnd * (2.0f * t - e->accelEnd);
                }
                dst = target + s->a;
                if (s->arg1 == 0) {
                    v = (f32)(s->c - s->b) * f + (f32)s->b;
                } else {
                    v = (f32)(s->b - s->c) * f + (f32)s->c;
                }
                switch (s->arg2) {
                    case 0:
                        *(s32 *)dst = v;
                        break;
                    case 1:
                        *(s16 *)dst = v;
                        break;
                    case 2:
                        *(s8 *)dst = v;
                        break;
                }
                s++;
                break;
            case 2:                                       /* conditional jump / call */
                ok = 0;
                switch (s->arg3) {
                    case 0:
                        ok = 1;
                        break;
                    case 1:
                        if (s->b == *(s32 *)s->a) {
                            ok = 1;
                        }
                        break;
                    case 2:
                        if (s->b != *(s32 *)s->a) {
                            ok = 1;
                        }
                        break;
                }
                if (!ok) {
                    s++;
                    break;
                }
                {
                    u8 *tgt = (s->arg2 != 0) ? (u8 *)s->d : target;

                    if (s->arg1 == 0) {
                        target = tgt;
                        s = (AnimCmd *)s->c;
                    } else if (s->arg1 == 1) {
                        animScript((AnimCmd *)s->c, tgt, t);
                        s++;
                    }
                }
                break;
            case 3:                                       /* call once t reaches the time */
                if ((f32)s->b / 1000000.0f <= t) {
                    ((void (*)(s32, s32))s->c)(s->a, s->d);
                }
                s++;
                break;
        }
    }
}

/* 0x801131C8 - once per frame: advance every running ramp and fire its events */
void animUpdate(void) {
    s32 list[50];
    s32 n = 0;
    s32 i, ramp, done;
    AnimRamp *r;
    u8 flags;
    f32 prev;

    for (i = 0; i < 50; i++) {
        if ((gAnimRamps[i].used & 0x80) && !(gAnimRamps[i].flags & ANIM_PAUSED)) {
            list[n++] = i;
        }
    }
    for (i = 0; i < n; i++) {
        ramp = list[i];
        r = &gAnimRamps[ramp];
        flags = r->flags;
        if (!(r->used & 0x80) || (flags & ANIM_PAUSED)) {
            continue;
        }
        prev = r->t;
        if (flags & ANIM_LOOP) {
            if (flags & ANIM_REVERSE) {
                done = ((f64)prev == 0.0);
            } else {
                done = ((f64)prev == 1.0);
            }
        } else {
            done = 0;
        }
        if (done) {
            r->t = 1.0 - (f64)prev;
        } else if (flags & ANIM_REVERSE) {
            r->t = prev - r->step;
            if ((f64)r->t < 0.0) {
                r->t = 0.0f;
            }
        } else {
            r->t = prev + r->step;
            if ((f64)r->t > 1.0) {
                r->t = 1.0f;
            }
        }
        rampFireEvents(ramp, prev);
        if (flags & ANIM_ONCE) {
            if (flags & ANIM_REVERSE) {
                done = ((f64)r->t == 0.0);
            } else {
                done = ((f64)r->t == 1.0);
            }
            if (done) {
                animCancel(ramp);
            }
        }
    }
}

/* 0x80113528 - free every ramp and event */
void animCancelAll(void) {
    s32 i;

    for (i = 0; i < 50; i++) {
        gAnimRamps[i].used = 0;
    }
    for (i = 0; i < 100; i++) {
        gAnimSlots[i].used = 0;
    }
    easeInit();
}

/* 0x80113590 - where each ease curve's acceleration and deceleration parts
 * meet the linear middle */
void easeInit(void) {
    EaseCurve *e;
    f32 a, b, a2, b2, d;

    for (e = gEaseCurves; e < &gEaseCurves[16]; e++) {
        a = e->accel;
        b = e->decel;
        a2 = a * a;
        b2 = b * b;
        d = (sqrtf(a2 * b2 - a * b2 - a2 * b) + b2) / (a * b + b2);
        e->decelStart = d;
        e->accelEnd = (1.0f - d) * b / a;
    }
}

/* 0x8011364C */
s32 rampFindFree(void) {
    s32 i;

    for (i = 0; i < 50; i++) {
        if (!(gAnimRamps[i].used & 0x80)) {
            gAnimRamps[i].used = 0x80;
            return i;
        }
    }
    return -1;
}

/* 0x8011372C */
s16 eventFindFree(void) {
    s16 i;

    for (i = 0; i < 100; i++) {
        if (!(gAnimSlots[i].used & 0x80)) {
            gAnimSlots[i].used = 0x80;
            return i;
        }
    }
    return -1;
}

/* 0x801137C4 - fire the events whose condition t (now) / prevT meets */
void rampFireEvents(s32 ramp, f32 prevT) {
    AnimRamp *r = &gAnimRamps[ramp];
    s16 e = r->first;
    s32 cancel = 0;
    s32 fire;
    AnimSlot *ev;
    u8 f;
    f32 at, t;

    while (e != -1) {
        ev = &gAnimSlots[e];
        if (!(ev->flags & EV_DISABLED)) {
            f = ev->flags & 0x7F;
            at = ev->at;
            t = r->t;
            if (f & EV_ALWAYS) {
                fire = 1;
            } else if ((f & EV_CHANGED) && t != prevT) {
                fire = 1;
            } else if ((f & EV_AT_0) && 0.0 == (f64)t) {
                fire = 1;
            } else if ((f & EV_AT_1) && 1.0 == (f64)t) {
                fire = 1;
            } else if (r->flags & ANIM_REVERSE) {
                fire = ((f & EV_PASS_BACK) && at < prevT && t <= at) ||
                       ((f & EV_END) && 0.0 < (f64)prevT && 0.0 == (f64)t);
            } else {
                fire = ((f & EV_PASS_FWD) && prevT < at && at <= t) ||
                       ((f & EV_END) && (f64)prevT < 1.0 && 1.0 == (f64)t);
            }
            if (fire) {
                cancel += eventFire(e);
            }
        }
        e = ev->next;
    }
    if (cancel != 0) {
        animCancel(ramp);
    }
}

/* 0x80113A44 - call an event; returns 1 if its ramp is to be cancelled */
s32 eventFire(s32 event) {
    AnimSlot *ev = &gAnimSlots[event];
    s16 ramp = ev->ramp;

    switch (ev->func(event, ramp, gAnimRamps[ramp].t)) {
        case 1:
            eventRemove(event);
            return 0;
        case 2:
            return 1;
        default:
            return 0;
    }
}

/* 0x80113AE0 - call next(ramp, user) after 'seconds'; it returns the time to
 * the next call (0: same again, -1: stop) */
s32 animWait(f32 seconds, void *user, AnimDoneFunc next) {
    s32 ramp, ev;

    ramp = rampAlloc(-1, 0.0f, 1.0f / (seconds * 30.0f), 0);
    if (ramp == -1) {
        return ramp;
    }
    ev = animAddEvent(ramp, (AnimDoneFunc)waitEvent, 0.0f, EV_AT_1);
    if (ev == -1) {
        animCancel(ramp);
        return ev;
    }
    gAnimRamps[ramp].user = (s32)user;
    gAnimSlots[ev].user = (s32)next;
    return ev;
}

/* 0x80113BB4 - toggle *flag (starting at 1) every time t loops past 'at',
 * 'count' times on (0 = forever); then call done(event, ramp) */
s32 animBlink(f32 seconds, f32 at, s32 count, s32 *flag, void (*done)(s32, s32)) {
    s32 ramp, ev;
    AnimRamp *r;

    ramp = rampAlloc(-1, 0.0f, 1.0f / (seconds * 30.0f), ANIM_LOOP);
    if (ramp == -1) {
        return ramp;
    }
    ev = animAddEvent(ramp, (AnimDoneFunc)blinkEvent, at, EV_PASS_FWD | EV_AT_0);
    if (ev == -1) {
        animCancel(ramp);
        return ev;
    }
    *flag = 1;
    r = &gAnimRamps[ramp];
    r->user = (s32)flag;
    r->user2 = (s32)done;
    gAnimSlots[ev].user = count;
    return ev;
}

/* 0x80113CA0 - the looping ramps flashing text uses (TextObj.blink) */
void animBlinkInit(void) {
    gBlinkSlow = rampAlloc(-1, 0.0f, 0.033333335f, ANIM_LOOP);    /* 1 s */
    gBlinkMedium = rampAlloc(-1, 0.0f, 0.06666667f, ANIM_LOOP);  /* 0.5 s */
    gBlinkFast = rampAlloc(-1, 0.0f, 0.13333334f, ANIM_LOOP);    /* 0.25 s */
}

/* 0x80113D18 */
s32 waitEvent(s32 event, s32 ramp, f32 t) {
    AnimRamp *r = &gAnimRamps[ramp];
    f32 next = ((f32 (*)(s32, s32))gAnimSlots[event].user)(ramp, r->user);

    if (next == -1.0f) {
        return 2;
    }
    if (next != 0.0f) {
        r->step = 1.0f / (next * 30.0f);
    }
    r->t = 0.0f;
    return 0;
}

/* 0x80113DE0 */
s32 blinkEvent(s32 event, s32 ramp, f32 t) {
    AnimRamp *r = &gAnimRamps[ramp];
    s32 *flag = (s32 *)r->user;
    s32 n;

    *flag = 1 - *flag;
    if (*flag != 0) {
        n = gAnimSlots[event].user;
        if (n != 0) {
            gAnimSlots[event].user = --n;
            if (n == 0) {
                if (r->user2 != 0) {
                    ((void (*)(s32, s32))r->user2)(event, ramp);
                }
                return 2;
            }
        }
    }
    return 0;
}
