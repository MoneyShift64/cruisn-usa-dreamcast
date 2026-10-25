/*
 * race/carphys.c - car engine, gearbox, steering, skids, barrier hits and car
 * sound effects. Used for the players' cars (and partly for the AI cars).
 * Original code: race overlay, ROM 0x4E340-0x50DE0 (RAM 0x800358D0-0x80038370),
 * shared with cars.c (vehicle set-up and drawing).
 *
 * Units: speed is in game units per frame-step (the speedometer shows
 * speed * 0.788 mph or * 0.49... see race.c); the engine is integrated in two
 * half-steps per frame.
 */
#include "common.h"
#include "race.h"
#include "road.h"

extern u8  gVehicleSoundBusy[];          /* 0x80065868 ovl2, stride 0x90 */
extern s32 gBarrierSfx[3];               /* 0x80043950 ovl2 */
extern s32 gFrameCounter;                /* 0x80147698 */

typedef struct Particle {                /* 0x3C bytes, 0x80066460 ovl2 */
    /* 0x00 */ u8  pad00[0x20];
    /* 0x20 */ f32 velX;
    /* 0x24 */ f32 velY;
    /* 0x28 */ f32 velZ;
    /* 0x2C */ s32 unk2C;
    /* 0x30 */ u8  pad30[0xC];
} Particle;
extern Particle gParticles[];            /* 0x80066460 ovl2 */

f32  randFloat(void);                    /* 0x80103104 */
u32  randInt(s32 n);                     /* 0x80103150 */
void sfxPlay(s32 id, s32 player);                   /* 0x8011F2A4  full volume */
void sfxPlayVolume(s32 id, s32 player, f32 volume); /* 0x8011F2C4 */
s32  particleSpawn(s32 type, f32 x, f32 y, f32 z, s32 kind); /* 0x80040070 ovl2 */
void carStartSpin(Car *car, s32 steps, f32 rate, f32 amount);
void carSparks(Car *car, Car *other, s32 front, s32 back, s32 side);
void carBarrierSpin(Car *car, f32 roadHeading, f32 offset);
void carBarrierGlance(Car *car, f32 roadHeading, f32 offset, f32 absOffset);
void carEndSpin(Car *car, s32 alignToRoad);
void carPlayRandomSfx(Car *car, s32 *table, s32 count, f32 volume);

/* 0x8003647C ovl2 - choose one of the 16 car models for a vehicle */
void vehicleSetModel(s32 obj, s32 model) {
    if (model < 0) {
        model = 15;
    } else if (model >= 16) {
        model = 0;
    }
    gRenderObjs[obj].model = model;
}

/* 0x800364C0 ovl2 - automatic gearbox: shift on rpm thresholds that rise with throttle */
s32 autoGearbox(s32 gear, s32 rpm, f32 throttle) {
    f32 r = rpm;
    f32 down = throttle * 11.0f + 12.0f;

    if (r < down && gear >= 2) {
        gear--;
    } else if (down + 18.0f < r && gear < 4) {
        gear++;
    }
    return gear;
}

/* 0x80036540 ovl2 - engine power, drag and speed integration */
void carEngineUpdate(Car *car) {
    f32 power;
    f32 drag;
    f32 speed;
    f32 brakeDrag;
    f32 overRev;
    f32 avg;
    f32 keep;
    f32 mid;
    s32 gear;

    if (car->airborne) {
        speed = car->speed;
        power = 0.0f;
        drag = 0.0f;
        car->vel[1] -= 8.0f;
    } else if (car->spinTimer) {
        power = 0.0f;
        drag = 0.015f;
        speed = car->speed;
    } else {
        power = car->throttle * car->enginePower;
        if (car->boosting) {
            power *= car->boostPower;
        }
        power *= tableLerp(gTorqueCurve, (f32)car->rpm * 0.333f, 20);
        if (gRaceState == 1) {
            gear = car->gear;
        } else {
            gear = 0;
        }
        power *= gGearPower[gear];
        if (!car->manual) {
            power *= 0.96f;
        }
        power = power * (1.0f - car->slide * 0.25f);

        /* uphill: lose power with the slope (only when accelerating, racing, not braking) */
        if (gRaceStateB == 0 && gRaceState != 0 && !(car->brake > 0.5f) && car->odometer != 0.0f) {
            f32 slope = 2.0f * car->orient[1][2];
            if (!(power > 0.5f) || !(power * 0.8f < slope)) {
                power -= slope;
            }
        }

        /* rolling drag: average of the five contact probes, on or off road */
        drag = (0.0f
                + (car->probes[0].offroad ? car->dragOffroad : car->dragRoad)
                + (car->probes[1].offroad ? car->dragOffroad : car->dragRoad)
                + (car->probes[2].offroad ? car->dragOffroad : car->dragRoad)
                + (car->probes[3].offroad ? car->dragOffroad : car->dragRoad)
                + (car->probes[4].offroad ? car->dragOffroad : car->dragRoad)) * 0.2f;
        if (car->boosting && car->speedScale > 1.09f) {
            drag *= 0.5f;
        }
        speed = car->speed;
        if (speed <= 100.0f && car->throttle >= 0.9f && car->gear < 3) {
            drag += car->slide * 0.003f;     /* wheelspin when launching hard */
        }
        brakeDrag = car->brake * 0.02f * (1.0f - car->slide);
        if (speed < 20.0f) {
            brakeDrag *= 2.0f;
        } else if (speed < 40.0f) {
            brakeDrag *= 1.5f;
        }
        if ((f32)car->rpm <= 42.0f) {
            overRev = 0.0f;
        } else {
            overRev = (f32)car->rpm - 42.0f;
        }
        drag = drag + brakeDrag + gGearRevDrag[car->gear] * (overRev * 0.04f + 1.0f);
    }

    /* two half-steps */
    keep = 1.0f - drag;
    car->speed = speed * keep;
    car->speed += power;
    mid = car->speed;
    car->speed = mid * keep;
    car->speed += power;
    avg = mid + car->speed;
    if (car->speed < 0.0f) {
        car->speed = 0.0f;
    }
    if (avg < 0.0f) {
        avg = 0.0f;
    }
    car->frameDist = avg * 1.5f;
    car->odometer += car->frameDist;
}

/* 0x8003698C ovl2 - engine revs from gear and speed (or free-revving in neutral / in the air) */
void carRpmUpdate(s32 player) {
    Car *car;
    f32 rpm;
    f32 speed;
    f32 hi;
    f32 lo;
    f32 load;
    f32 unused;

    if (gRaceState == 0 || (car = &gCars[player], car->gear == 0) || car->airborne) {
        car = &gCars[player];
        rpm = car->throttle * 47.0f;
        hi = (f32)car->rpm + 4.0f;     /* revs can rise or fall by 4 per frame */
        lo = hi - 8.0f;
        if (rpm < lo) {
            rpm = lo;
        } else if (hi < rpm) {
            rpm = hi;
        }
        speed = car->speed;
    } else {
        speed = car->speed;
        rpm = gGearRpmRatio[car->gear] * speed;
    }
    car->rpm = (s32)rpm;

    /* computed but never used in the final game (probably engine-sound load) */
    load = ((speed * 0.00333f) - 1.0f) * 0.75f + 0.25f;
    if (load < 0.0f) {
        unused = 0.0f;
    } else {
        unused = (load > 1.0f) ? 1.0f : load;
    }
}

/* 0x80036B0C ovl2 - how much the car slides (0..1) */
void carSlideUpdate(Car *car) {
    f32 a;
    f32 s;
    f32 speed;
    f32 throttle;

    if (car->spinTimer) {
        car->slide = 1.0f;
        return;
    }
    if (car->gear < 4 && (f32)car->rpm >= 51.0f) {
        car->slide = 0.8f;                 /* over-revving in a low gear: wheelspin */
        return;
    }
    a = angleWrap(car->heading - car->travelDir);
    if (a < -1.2f) {
        a = -1.2f;
    } else if (a > 1.2f) {
        a = 1.2f;
    }
    s = a * 0.2f + car->steer;
    if (s < 0.0f) {
        s = -s;
    }
    if (s > 0.3f) {
        s = 0.3f;
    }
    throttle = car->throttle;
    speed = car->speed;
    s *= speed;
    if (throttle >= 0.9f && (car->gear == 1 || car->gear == 2) && speed < 100.0f) {
        s += (100.0f - speed) * 1.1;
    }
    s = s * (car->brake + 1.0f) * (throttle * 0.25f + 1.0f) - 22.0f;
    if (s < 0.0f) {
        s = 0.0f;
    }
    s *= 0.045f;
    if (s > 1.0f) {
        s = 1.0f;
    }
    car->slide = s * car->grip;
}

/* 0x80036D14 ovl2 - steering, yaw and spin-outs */
void carSteerUpdate(Car *car) {
    f32 rate;
    f32 target;
    f32 diff;
    f32 slide;
    f32 travel;
    f32 heading;
    f32 steer;
    f32 pull;
    f32 swing;
    s32 i;

    if (car->unk2D) {
        car->yawDelta = 0.0f;
        car->heading += car->yawRate;
        return;
    }
    if (car->spinTimer) {
        if (car->spinTimer >= 2) {
            car->spinTimer -= 2;
            if (car->spinTimer < 3) {
                carEndSpin(car, FALSE);
                car->yawDelta = 0.0f;
                return;
            }
            car->yawDelta = 0.0f;
            car->heading += car->yawRate;
            return;
        }
        /* spin-out: two sub-steps */
        for (i = 0; i != 2; i++) {
            rate = car->yawRate;
            car->spinRemaining -= (rate < 0.0f) ? -rate : rate;
            car->heading += rate;
            if (car->spinRemaining < 0.0f) {
                if (car->unk84 != 0 && car->unk7C != 0) {
                    car->yawRate = rate * 0.98f;
                    rate = car->yawRate;
                    if (((rate < 0.0f) ? -rate : rate) < 0.04f) {
                        car->yawRate = 0.0f;
                    }
                } else {
                    target = car->road.heading;
                    if (car->unk84 == 3) {
                        target += 3.1415927f;
                    }
                    diff = angleWrap(target - car->heading);
                    if (((diff < 0.0f) ? -diff : diff) < 0.2f) {
                        carEndSpin(car, TRUE);
                        return;
                    }
                }
            }
        }
        car->yawDelta = 0.0f;
        return;
    }

    slide = car->slide;
    travel = car->travelDir;
    heading = car->heading;
    steer = car->steerYaw * ((1.0f - slide) * 0.75f + 0.25f);
    pull = angleWrap(heading - travel) * 0.095f;
    car->travelDir = travel + pull + steer;
    swing = slide * car->yawRate * 0.7f * ((car->throttle * 0.5f + 0.5f) * 1.2f);
    steer += swing;
    car->yawRate = steer;
    car->yawDelta = steer;
    car->heading = heading + (-pull * 0.5f) + steer;
    car->heading = angleWrap(car->heading);
    car->yawRate = angleWrap(car->yawRate);
}

/* 0x800370B8 ovl2 */
f32 carCameraSideOffset(Car *car, s32 player) {
    return cameraSideOffset(car->pos[0], car->pos[2], player);
}

/* 0x800370E0 ovl2 - classify the probes: 0 on road, 1 touching kerb, 2 off the track */
void carCheckProbes(Car *car) {
    s32 contact;
    u8 s;

    car->unk1AC = 0;
    contact = 0;
    s = car->probes[1].offroad;
    if (s >= 3) {
        car->unk1AC = 1;
    }
    if (s == 2) {
        contact = 1;
    }
    s = car->probes[2].offroad;
    if (s >= 3) {
        car->unk1AC++;
    }
    if (s == 2) {
        contact = 1;
    }
    s = car->probes[3].offroad;
    if (s >= 3) {
        car->unk1AC++;
    }
    if (s == 2) {
        contact = 1;
    }
    s = car->probes[4].offroad;
    if (s >= 3) {
        car->unk1AC++;
    }
    if (s == 2) {
        contact = 1;
    }
    if (car->unk1AC != 0) {
        contact = 2;
        car->unk1A8++;
    } else {
        car->unk1A8 = 0;
    }
    car->unk80 = contact;
}

/* 0x800371D4 ovl2 - the car hit the edge of the track: push it back and react */
void carHitBarrier(Car *car, s32 player, s32 noSteer) {
    f32 road;
    f32 roadTurn;
    f32 off;
    f32 speed;
    f32 dir;
    f32 push;
    f32 dx;
    f32 dz;
    f32 side;
    s32 right;
    s32 offSide;
    s32 left;
    s32 playSfx;
    u8 idx;

    if (gVehicleSoundBusy[car->unk7D * 0x90] == 0) {
        carPlayRandomSfx(car, gBarrierSfx, 3, 1.0f);
    }
    road = car->road.heading;
    roadTurn = angleWrap(road - car->road.aim);
    off = angleWrap(road - car->travelDir);

    speed = car->speed;
    if (speed < 15.0f) {
        speed = 15.0f;
    }
    car->speed = speed;

    /* nudge the car sideways, back towards the road */
    side = -1.5707964f;
    right = FALSE;
    if (roadTurn < 0.0f) {
        right = TRUE;
    }
    if (right) {
        side = 1.5707964f;
    }
    push = speed * 3.0f;
    dir = side + road;
    if (sinf_tbl(dir - car->travelDir) < 0.0f) {
        side = -sinf_tbl(dir - car->travelDir);
    } else {
        side = sinf_tbl(dir - car->travelDir);
    }
    push *= side;
    dz = cosf_tbl(dir) * push;
    dx = sinf_tbl(dir) * -push;
    car->pos[0] += dx;
    car->pos[2] += dz;

    left = roadIsLeft(&car->road);
    idx = car->unk7D;
    if (idx < gNumPlayers) {
        playSfx = FALSE;
        if (gRacers[idx].barrierSfxFrame < (u32)gFrameCounter) {
            playSfx = TRUE;
        }
        gRacers[idx].barrierSfxFrame = gFrameCounter + 15;
    } else {
        playSfx = TRUE;
    }
    if (playSfx) {
        carSparks(car, NULL, 1, 0, left ? 1 : -1);
    }

    offSide = FALSE;
    if (off < 0.0f) {
        offSide = TRUE;
    }
    if (right == offSide) {
        off = -off;
    }
    if (noSteer == 0) {
        if (car->spinTimer) {
            carBarrierSpin(car, road, off);
            return;
        }
        carBarrierGlance(car, road, off, (off < 0.0f) ? -off : off);
    }
}

/* 0x8003749C ovl2 - barrier hit that ends in a spin */
void carBarrierSpin(Car *car, f32 roadHeading, f32 offset) {
    f32 diff;
    f32 rate;
    f32 a;
    f32 amount;

    car->travelDir = offset + roadHeading;
    diff = angleWrap(roadHeading - car->heading);
    rate = randFloat() * 0.025f + 0.075f;
    if (diff <= 0.0f) {
        rate = -rate;
    }
    a = (diff < 0.0f) ? -diff : diff;
    amount = a + 0.1f;
    if (amount > 1.0f) {
        amount = 3.1415927f;
    }
    if (car->speed < 30.0f) {
        amount = a + 0.1f;
    }
    carStartSpin(car, 0, rate, amount);
}

/* 0x80037594 ovl2 - barrier hit: lose speed, glance off or spin */
void carBarrierGlance(Car *car, f32 roadHeading, f32 offset, f32 absOffset) {
    f32 diff;
    f32 rate;
    f32 turn;
    f32 limit;

    if (car->speed >= 180.0f) {
        car->speed = 180.0f;
    }
    car->speed *= 0.8f;
    if (absOffset > 1.2f) {
        carBarrierSpin(car, roadHeading, offset);
        return;
    }
    if (absOffset > 0.4f) {
        car->travelDir = roadHeading + offset;
        diff = angleWrap(roadHeading - car->heading);
        rate = diff * 0.1f;
        if (rate < 0.0f) {
            limit = -0.09f;
            if (limit < rate) {
                rate = limit;
            }
        } else {
            limit = 0.09f;
            if (rate < limit) {
                rate = limit;
            }
        }
        carStartSpin(car, 0, rate, (diff < 0.0f) ? -diff : diff);
        return;
    }
    turn = offset * 0.3f;
    if (absOffset * 0.3f < 0.03f) {
        turn = (turn < 0.0f) ? -0.03f : 0.03f;
    }
    if (absOffset * 0.3f > 0.06f) {
        turn = (turn < 0.0f) ? -0.06f : 0.06f;
    }
    car->heading = car->travelDir = turn + roadHeading;
}

/* 0x8003777C ovl2 - play a sound at a position, quieter with distance from the player(s).
 * NOTE: original bug - in 2-player games both distances are measured from player 1's car. */
void sfxAtPoint(f32 x, f32 z, s32 id, f32 volume) {
    f32 dist;
    f32 side;
    f32 dist2;
    f32 side2;
    s32 player;
    f32 v;

    dist = dist2D(gCars[0].pos[0], gCars[0].pos[2], x, z);
    side = cameraSideOffset(x, z, 0);
    player = 0;
    if (gNumPlayersM1 != 0) {
        dist2 = dist2D(gCars[0].pos[0], gCars[0].pos[2], x, z);
        side2 = cameraSideOffset(x, z, 0);
        if (side2 > -500.0f && dist2 < dist) {
            player = 1;
            dist = dist2;
            side = side2;
        }
    }
    if (side >= -500.0f) {
        v = 1.0f - dist * 0.00002f;
        if (v > 0.86f) {
            v = 0.86f;
        }
        if (v > 0.0f) {
            sfxPlayVolume(id, player, v * volume);
        }
    }
}

/* 0x800378DC ovl2 */
void carPlaySfx(Car *car, s32 id, f32 volume) {
    if (car->muted == 0) {
        if (car->unk7D <= gNumPlayersM1) {
            sfxPlay(id, car->unk7D);   /* players' own sounds ignore 'volume' */
            return;
        }
        sfxAtPoint(car->pos[0], car->pos[2], id, volume);
    }
}

/* 0x80037944 ovl2 */
void carPlayRandomSfx(Car *car, s32 *table, s32 count, f32 volume) {
    carPlaySfx(car, table[randInt(count)], volume);
}

/* 0x80037C8C ovl2 - start a spin-out */
void carStartSpin(Car *car, s32 steps, f32 rate, f32 amount) {
    f32 h;
    f32 d;
    f32 road;

    if (steps < 2) {
        steps = 1;
    } else {
        road = car->road.heading;
        h = (f32)((steps >> 1) - 2) * rate + car->heading;
        if (h < road) {
            d = -h - road;               /* sic: original sign */
        } else {
            d = h - road;
        }
        if (d > 1.05f) {
            steps = 1;
            rate = (rate <= 0.0f) ? -0.1f : 0.1f;
            amount = 1.0f;
        }
    }
    car->spinTimer = steps;
    car->yawRate = rate;
    car->spinRemaining = amount;
    if (car->unk7D < gNumPlayers && gRacers[car->unk7D].noSpin) {
        car->spinTimer = 0;
    }
}

/* 0x8003798C ovl2 - sparks/debris where the car hit something */
void carSparks(Car *car, Car *other, s32 front, s32 back, s32 side) {
    f32 x;
    f32 y;
    f32 z;
    f32 vx;
    f32 vy;
    f32 vz;
    s32 i;
    s32 on;
    s32 p;
    f32 *pa;
    f32 *pb;

    i = 1;
    if (car->sparkTimer == 0) {
        if (other != NULL) {
            x = (car->pos[0] + other->pos[0]) * 0.5f;
            y = (car->pos[1] + other->pos[1]) * 0.5f;
            z = (car->pos[2] + other->pos[2]) * 0.5f;
        } else {
            x = car->pos[0];
            y = car->pos[1];
            z = car->pos[2];
            pa = (f32 *)((u8 *)car + 0x134);
            pb = (f32 *)((u8 *)car + 0x168);
            if (side < 0) {
                x -= (pb[0] - pa[0]) * 0.5f;
                z -= (pb[2] - pa[2]) * 0.5f;
            } else if (side > 0) {
                x += (pb[0] - pa[0]) * 0.5f;
                z += (pb[2] - pa[2]) * 0.5f;
            }
        }
        x *= 0.2f;
        y *= 0.2f;
        z *= -0.2f;
        vx = car->vel[0] * 0.2f;
        vy = car->vel[1] * 0.080000006f;
        vz = car->vel[2] * -0.2f;
        do {
            on = back;
            if (i != 0) {
                on = front;
            }
            if (on) {
                p = particleSpawn(i != 0, x, y, z, 3);
                if (p >= 0) {
                    gParticles[p].velX = vx;
                    gParticles[p].velY = vy;
                    gParticles[p].velZ = vz;
                    gParticles[p].unk2C = 0x6666;
                }
            }
            i++;
        } while (i != 2);
        car->sparkTimer = 7;
    }
}

/* 0x80037FFC ovl2 - finish a spin-out (optionally snapping to the road direction) */
void carEndSpin(Car *car, s32 alignToRoad) {
    f32 h;

    if (alignToRoad) {
        car->heading = car->road.heading;
        if (car->unk84 == 3) {
            car->heading += 3.1415927f;
        }
    }
    h = angleWrapPositive(car->heading);
    car->heading = h;
    car->travelDir = h;
    car->spinTimer = 0;
    car->spinRemaining = 0.0f;
    car->yawRate = 0.0f;
}
