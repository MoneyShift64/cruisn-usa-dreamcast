/*
 * race/camera.c - reading the players' controls each frame, and the race camera:
 * in-car / chase / far chase views, keeping the chase camera out of walls and
 * above the ground, and the orbit round the car after the finish.
 * Original code: race overlay, ROM 0x4D050-0x4E340 (RAM 0x800345E0-0x800358D0).
 *
 * Units: RaceCamera.pos/base are drawing units (world * 0.2, y and z negated);
 * yaw/unk1C/pitch are radians. The car's position is in world units.
 */
#include "common.h"
#include "race.h"
#include "road.h"
#include "frontend.h"

/* Raw controller state per pad (0x18 bytes, 0x8014E378), filled by the main loop */
typedef struct PadAnalog {
    /* 0x00 */ s32 gas;                      /* 0..255 */
    /* 0x04 */ s32 brake;                    /* 0..255 */
    /* 0x08 */ f32 steer;                    /* -1..1 */
    /* 0x0C */ u8  pad0C[0x0C];
} PadAnalog;
extern PadAnalog gPadAnalog[];           /* 0x8014E378 */

/* The player's configured buttons (controller options), one word per player */
extern u32 gBtnShiftUp[2];               /* 0x8014E350 */
extern u32 gBtnShiftDown[2];             /* 0x8014E358 */
extern u32 gBtnLights[2];                /* 0x8014E360 - configured button C */
extern u32 gBtnMusic[2];                 /* 0x8014E368 */
extern u32 gBtnView[2];                  /* 0x8014E370 */

/* What the main program's renderer looks through (0x28 bytes, 0x8014EF90) */
typedef struct ViewerCam {
    /* 0x00 */ s32 x, y, z;                  /* added to the camera matrix's translation */
    /* 0x0C */ s32 yaw, unk10, roll;         /* binary angles (0x8000 = PI) */
    /* 0x18 */ s32 unk18, unk1C, unk20;
    /* 0x24 */ s32 active;
} ViewerCam;
extern ViewerCam gViewerCams[2];         /* 0x8014EF90 */

/* Camera views: {distance, height} per view, a block of three per player count */
typedef struct CamView { f32 dist, height; } CamView;
extern CamView gCamViews[2][3];          /* 0x80133B5C */

extern s32 gCountdownPhase;              /* 0x800468A8 ovl2: 1-3 = 3,2,1 ; 4-5 = go ; 6+ = finished */
extern s32 gFinishOrbit[2];              /* 0x80046890 ovl2 - frames since the finish (camera orbit) */
extern s32 *gMtxCursor;                  /* 0x80147688 - the matrix viewBegin just wrote (4x4 s32) */

u32  contPressed(s32 player, u32 mask);  /* 0x80111460 */
s32  atan2Oct(f32 y, f32 x);             /* 0x801141D0 - binary angle (octant + table version of atan2s) */
void viewBegin(s32 racer);        /* 0x80116C10 */
void sfxPlay(s32 id, s32 player);        /* 0x8011F2A4 */
void probeFindGround(CarProbe *p, s32 vehicle, s32 side); /* 0x800262D4 ovl2 */
f32  angleWrap(f32 a);                   /* 0x80038370 ovl2 */
f32  sinf_tbl(f32 a);                    /* 0x80038458 ovl2 */
f32  cosf_tbl(f32 a);                    /* 0x800384AC ovl2 */
void mtx3RotY(Mtx3 m, f32 a);            /* 0x80038568 ovl2 */
void mtx3Mul(Mtx3 dst, Mtx3 a, Mtx3 b);  /* 0x80038640 ovl2 */
void vec3MulMtx(Vec3f out, Vec3f v, Mtx3 m); /* 0x8003879C ovl2 */

void cameraSetView(s32 player, s32 quiet); /* 0x80034830 ovl2 */
s32  camBlocked(Car *car, RaceCamera *cam, f32 yaw, s32 player); /* 0x80035370 ovl2 */
s32  camSidesBlocked(RaceCamera *cam, Vec3f pos, f32 yaw, s32 side); /* 0x800353BC ovl2 */
s32  camSideBlocked(RaceCamera *cam, Vec3f pos, f32 yaw, f32 offset, s32 side); /* 0x80035428 ovl2 */
s32  camProbe(RaceCamera *cam, f32 *height, s32 side); /* 0x800354B8 ovl2 */
void camSteerToRoad(RaceCamera *cam, Car *car, f32 *yaw); /* 0x80035508 ovl2 */
void camPlace(Car *car, f32 yaw, Vec3f out, s32 player, s32 lift); /* 0x800355B8 ovl2 */
void camKeepAboveGround(RaceCamera *cam, s32 player); /* 0x800356BC ovl2 */
f32  carYawLag(Car *car);                /* 0x800357F4 ovl2 */

/* 0x800345E0 ovl2 - clear every player's controls (buttons 0, pedals and steering 0) */
void playerInputClear(void) {
    PlayerInput *in;

    for (in = gPlayerInput; in < &gPlayerInput[gNumPlayers]; in++) {
        in->buttons = 0;
        in->steer = 0.0f;
        in->gas = 0.0f;
        in->brake = 0.0f;
        in->steerSetting = 5;
    }
}

/* 0x80034634 ovl2 - read the controllers into gPlayerInput: the five configured
 * buttons become RACEBTN bits 0x01-0x10 (new presses only), the pedals 0..1.
 * 0x20 is set throughout the race once the countdown is over. */
void playerInputRead(void) {
    s32 p;
    u32 pressed;
    u32 b;

    for (p = 0; p < gNumPlayers; p++) {
        pressed = contPressed(p, ~(gBtnView[p] | 0x1000 | gBtnLights[p] | gBtnMusic[p]));
        b = ((gBtnView[p] & pressed) ? 0x10 : 0)
          | ((gBtnShiftUp[p] & pressed) ? 1 : 0)
          | ((gBtnShiftDown[p] & pressed) ? 2 : 0)
          | ((gBtnLights[p] & pressed) ? 4 : 0)
          | ((gBtnMusic[p] & pressed) ? 8 : 0);
        gPlayerInput[p].steer = gPadAnalog[p].steer;
        gPlayerInput[p].buttons = b;
        gPlayerInput[p].brake = (f32)gPadAnalog[p].brake * (1.0f / 255.0f);
        gPlayerInput[p].gas = (f32)gPadAnalog[p].gas * (1.0f / 255.0f);
        if (gCountdownPhase == 0 && gRaceState > 0) {
            gPlayerInput[p].buttons = b | 0x20;
        }
    }
}

/* 0x80034830 ovl2 - start moving the camera to the player's chosen view
 * (cameraFollowsCar). Moves 1/20 of the way per frame, 4x faster for small
 * changes; 'quiet' = no whoosh sound (0x91 closer, 0x92 further). */
void cameraSetView(s32 player, s32 quiet) {
    PlayerSetup *ps = &gPlayerSetup[player];
    CamView *v = &gCamViews[gNumPlayersM1][ps->cameraFollowsCar];
    f32 stepDist, stepHeight, size;

    ps->camDistTarget = v->dist;
    stepDist = (v->dist - ps->camDist) * 0.05f;
    ps->camHeightTarget = v->height;
    stepHeight = (v->height - ps->camHeight) * 0.05f;
    if (stepDist < 0.0f) {
        if (quiet == 0) {
            sfxPlay(0x91, player);
        }
        size = -stepDist;
    } else {
        if (quiet == 0) {
            sfxPlay(0x92, player);
        }
        size = stepDist;
    }
    if (size < 25.0f) {
        stepDist *= 4.0f;
        stepHeight *= 4.0f;
    }
    if (stepDist == 0.0f || stepHeight == 0.0f) {
        stepHeight = 0.0f;
        stepDist = 0.0f;
        ps->camDist = ps->camDistTarget;
        ps->camHeight = ps->camHeightTarget;
    }
    ps->camDistStep = stepDist;
    ps->camHeightStep = stepHeight;
}

/* 0x800349BC ovl2 - on: switch out of the in-car view (remembering that it was
 * forced); off: go back to the in-car view if it was forced. */
void cameraForceFar(s32 player, s32 on, s32 quiet) {
    PlayerSetup *ps;

    if (on != 0) {
        ps = &gPlayerSetup[player];
        if (ps->camOverride == 0) {
            if (ps->cameraFollowsCar == 0) {
                ps->camOverride = 2;
                ps->cameraFollowsCar = 2;
                cameraSetView(player, quiet);
                return;
            }
            ps->camOverride = 1;
        }
    } else {
        ps = &gPlayerSetup[player];
        if (ps->camOverride == 1) {
            ps->camOverride = 0;
        } else if (ps->camOverride == 2) {
            ps->cameraFollowsCar = 0;
            cameraSetView(player, quiet);
            ps->camOverride = 0;
        }
    }
}

/* 0x80034A94 ovl2 - one frame of a view change */
void cameraViewStep(s32 player) {
    PlayerSetup *ps = &gPlayerSetup[player];
    s32 done;

    if (ps->camDistStep != 0.0f) {
        done = FALSE;
        ps->camDist += ps->camDistStep;
        ps->camHeight += ps->camHeightStep;
        if (ps->camDistStep < 0.0f) {
            if (ps->camDist <= ps->camDistTarget) {
                done = TRUE;
            }
        } else if (ps->camDistTarget <= ps->camDist) {
            done = TRUE;
        }
        if (done) {
            ps->camDist = ps->camDistTarget;
            ps->camDistStep = 0.0f;
            ps->camHeightStep = 0.0f;
            ps->camHeight = ps->camHeightTarget;
            ps->camViewShown = ps->cameraFollowsCar;
        }
    }
}

/* 0x80034B64 ovl2 - position the player's camera for this frame and hand the
 * angles to the renderer (gViewerCams). In-car: follow the car body's angles.
 * Chase: swing 1/5 of the way towards the direction of travel, steering back
 * towards the road while a wall is in the way. After the finish: orbit. */
void raceCameraUpdate(s32 player) {
    RaceCamera *cam;
    Car *car;
    PlayerSetup *ps;
    ViewerCam *vc;
    RenderObj *obj;
    f32 tilt;
    s32 view;
    f32 vx, vz;
    f32 yaw, tried, d1, d2, t;
    Vec3f orbit, o;
    Mtx3 m;

    if (gCountdownPhase == 0) {
        return;
    }
    tilt = (gNumPlayersM1 != 0) ? 0.09587380290031433f : 0.0f;  /* 2-player: extra 5.5 degree tilt */
    ps = &gPlayerSetup[player];
    vc = &gViewerCams[player];
    view = ps->cameraFollowsCar;
    vc->active = 1;
    vc->z = 0;
    vc->y = 0;
    vc->x = 0;
    if (gCountdownPhase >= 6) {
        /* finished: circle round the car */
        orbit[2] = 0.0f;
        t = (f32)gFinishOrbit[player] / 1000.0f;
        orbit[1] = t * -525.0f;
        orbit[0] = t * -2880.0f;
        mtx3RotY(m, -gCars[player].road.heading);
        vec3MulMtx(o, orbit, m);
        cam = &gRaceCameras[player];
        cam->posX = o[2] * 0.2f + cam->baseX;
        cam->posY = o[1] * -0.2f + cam->baseY;
        cam->posZ = o[0] * -0.2f + cam->baseZ;
    } else {
        ps->viewAtEdge = (ps->camDist == 0.0f);
        if (view == 0) {
            /* in-car */
            car = &gCars[player];
            car->unk50 = 0.0f;
            obj = &gRenderObjs[player];
            cam = &gRaceCameras[player];
            cam->yaw = ((f32)(s16)obj->angleZ * 3.1415927f) / 32768.0f;
            cam->unk1C = (((f32)(s16)(obj->angleY + obj->angleYAdd) * 3.1415927f) / 32768.0f) * 0.7f + tilt;
            cam->pitch = (((f32)(s16)(obj->angleX + obj->angleXAdd) * 3.1415927f) / 32768.0f) * -0.5f;
        } else {
            car = &gCars[player];
            vz = car->vel[2];
            vx = -car->vel[0];
            if (car->spinTimer == 0 &&
                !(((vz < 0.0f) ? -vz : vz) + ((vx < 0.0f) ? -vx : vx) < 2.0f)) {
                cam = &gRaceCameras[player];
                yaw = angleWrap(((f32)(s16)atan2Oct(vz, vx) * 3.1415927f) / 32768.0f - cam->yaw) * 0.2f + cam->yaw;
                if (camBlocked(car, cam, yaw, player) != 0) {
                    tried = yaw;
                    camSteerToRoad(cam, car, &yaw);
                    d1 = angleWrap(yaw - cam->yaw);
                    if (d1 < 0.0f) {
                        d1 = -d1;
                    }
                    d2 = angleWrap(yaw - tried);
                    if (d2 < 0.0f) {
                        d2 = -d2;
                    }
                    if (d1 < d2) {
                        yaw = cam->yaw;
                        if (camBlocked(car, cam, yaw, player) != 0) {
                            camSteerToRoad(cam, car, &yaw);
                        }
                    }
                }
            } else {
                /* spinning or almost stopped: hold the direction */
                cam = &gRaceCameras[player];
                yaw = cam->yaw;
                if (camBlocked(car, cam, yaw, player) != 0) {
                    camSteerToRoad(cam, car, &yaw);
                }
            }
            cam->yaw = yaw;
            cam->pitch = 0.0f;
            cam->unk1C = tilt;
            mtx3RotY(m, -carYawLag(car));
            mtx3Mul(car->orient, m, car->orient);
        }
        camPlace(car, cam->yaw, &cam->posX, player, car->flipping);
        if (car->flipping != 0 || view != 0) {
            camKeepAboveGround(cam, player);
        }
        cam->posX *= 0.2f;
        cam->baseX = cam->posX;
        cam->posY *= -0.2f;
        cam->baseY = cam->posY;
        cam->posZ *= -0.2f;
        cam->baseZ = cam->posZ;
    }
    vc->yaw = (s32)((cam->yaw * 32768.0f) / 3.1415927f);
    vc->unk10 = (s32)((cam->unk1C * 32768.0f) / 3.1415927f);
    vc->roll = (s32)((-cam->pitch * 32768.0f) / 3.1415927f);
}

/* 0x800351B4 ovl2 - build the player's car matrix; in the in-car view during
 * the race, blend its rotation 80% towards last frame's (smooths the bumps).
 * The result is kept in the camera for next frame. */
void cameraBuildMatrix(s32 player) {
    s32 *mtx = gMtxCursor;
    s32 i, j;
    s32 (*rot)[4];
    s32 *row;

    viewBegin(player);
    if (gPlayerSetup[player].cameraFollowsCar == 0 && gCountdownPhase != 0 && gCountdownPhase < 6) {
        rot = gRaceCameras[player].rot;
        for (i = 0, row = mtx; i < 3; i++, row += 4) {
            for (j = 0; j < 3; j++) {
                row[j] = (s32)((f32)rot[i][j] * 0.8f + 0.2f * (f32)row[j]);
            }
        }
    }
    rot = gRaceCameras[player].rot;
    for (i = 0, row = mtx; i < 3; i++, row += 4) {
        for (j = 0; j < 3; j++) {
            rot[i][j] = row[j];
        }
    }
}

/* 0x80035370 ovl2 - would the chase camera at this yaw have a wall either side? */
s32 camBlocked(Car *car, RaceCamera *cam, f32 yaw, s32 player) {
    Vec3f pos;

    camPlace(car, yaw, pos, player, 0);
    return camSidesBlocked(cam, pos, yaw, player);
}

/* 0x800353BC ovl2 - 1 = blocked on the left, 2 = on the right, 3 = both */
s32 camSidesBlocked(RaceCamera *cam, Vec3f pos, f32 yaw, s32 side) {
    s32 left = camSideBlocked(cam, pos, yaw, -170.0f, side);

    return left + camSideBlocked(cam, pos, yaw, 170.0f, side) * 2;
}

/* 0x80035428 ovl2 - test the ground 'offset' units to the side of pos */
s32 camSideBlocked(RaceCamera *cam, Vec3f pos, f32 yaw, f32 offset, s32 side) {
    cam->probe.x = cosf_tbl(yaw) * offset + pos[0];
    cam->probe.y = -pos[1];
    cam->probe.z = pos[2] - sinf_tbl(yaw) * offset;
    return camProbe(cam, NULL, side) == 0;
}

/* 0x800354B8 ovl2 - find the ground under the camera's probe; TRUE if there is
 * ground there (road, verge or other ground). height = height above it. */
s32 camProbe(RaceCamera *cam, f32 *height, s32 side) {
    probeFindGround(&cam->probe, -1, side);
    if (height != NULL) {
        *height = cam->probe.unk20;
    }
    return cam->probe.offroad < 3;
}

/* 0x80035508 ovl2 - turn yaw towards the road direction by up to 0.1 rad */
void camSteerToRoad(RaceCamera *cam, Car *car, f32 *yaw) {
    f32 d = angleWrap(*yaw + -car->road.heading);
    f32 size = (d < 0.0f) ? -d : d;

    if (size > 0.1f) {
        *yaw += (d >= 0.0f) ? -0.1f : 0.1f;
        return;
    }
    *yaw -= d;
}

/* 0x800355B8 ovl2 - the chase camera position for a yaw, in world units with y
 * up (= car position + (-dist, height) rotated). lift: raise it by the car's
 * height above the ground (car tumbling). */
void camPlace(Car *car, f32 yaw, Vec3f out, s32 player, s32 lift) {
    PlayerSetup *ps;
    f32 up;
    Vec3f v, o;
    Mtx3 m;

    v[2] = 0.0f;
    if (lift != 0) {
        up = car->probes[0].unk20 * 0.6f;
    } else {
        up = 0.0f;
    }
    ps = &gPlayerSetup[player];
    v[1] = ps->camHeight - up;
    v[0] = -ps->camDist;
    mtx3RotY(m, yaw);
    vec3MulMtx(o, v, m);
    out[0] = car->pos[0] - o[2];
    out[1] = (f64)o[1] - ((f64)car->pos[1] + 12.5);
    out[2] = o[0] + car->pos[2];
}

/* 0x800356BC ovl2 - keep the camera at least 133 units above the ground (the
 * hill limit depends on the track); in 2-player it also levels the view. */
void camKeepAboveGround(RaceCamera *cam, s32 player) {
    f32 height;
    f32 limit, rate, below;

    cam->probe.x = cam->posX;
    cam->probe.y = -cam->posY;
    cam->probe.z = cam->posZ;
    if (camProbe(cam, &height, player) != 0) {
        if (gTrackChoice == 2) {
            limit = 2000.0f;
            rate = 0.0005f;
        } else if (gNumPlayersM1 != 0 && gTrackChoice == 1) {
            limit = 1000.0f;
            rate = 0.001f;
        } else if (gNumPlayersM1 != 0 && gTrackChoice == 10) {
            limit = 900.0f;
            rate = 0.0011111111f;
        } else {
            limit = 500.0f;
            rate = 0.002f;
        }
        below = 133.0f - height;
        if (height < 133.0f && below < limit) {
            cam->posY -= below;
            if (gNumPlayersM1 != 0) {
                cam->unk1C = (0.0f - cam->unk1C) * (below * rate) + cam->unk1C;
            }
        }
    }
}

/* 0x800357F4 ovl2 - the car's drawn yaw lags behind its steering (car->unk50),
 * catching up by up to 0.005 rad per unit travelled (max 10 per frame). */
f32 carYawLag(Car *car) {
    f32 cur, target, step, diff, dist;

    if (car->flipping != 0) {
        target = 0.0f;
    } else {
        cur = car->unk50;
        if (car->unk2D != 0) {
            target = cur;
        } else {
            target = car->steer * -0.5f;
        }
        dist = (car->frameDist > 10.0f) ? 10.0f : car->frameDist;
        step = dist * 0.0050000004f;            /* (the ROM constant is 1 bit above 0.005f) */
        diff = cur - target;
        if (cur < target) {
            diff = -diff;
        }
        if (diff > step) {
            target = cur + ((target <= cur) ? -step : step);
        }
    }
    car->unk50 = target;
    return target;
}
