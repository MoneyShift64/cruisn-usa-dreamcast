#ifndef RACE_H
#define RACE_H

/* Shared declarations for the race overlay (overlay 2). */

typedef f32 Vec3f[3];
typedef f32 Mtx3[3][3];

typedef struct CarProbe {
    /* 0x00 */ void *hitObj;                 /* ground polygon found last time (a GroundHit, see ground.h): scenery object */
    /* 0x04 */ void *hitItem;                /*   ...its ModelItem (detects a recycled object) */
    /* 0x08 */ s32 hitSurface;               /*   ...surface type it gave (copy of offroad) */
    /* 0x0C */ u8 *hitFace;                  /*   ...the face record in the object's collision mesh */
    /* 0x10 */ f32 x;                        /* world position of the probe */
    /* 0x14 */ f32 y;
    /* 0x18 */ f32 z;
    /* 0x1C */ f32 groundY;                  /* ground height under the probe (world units) */
    /* 0x20 */ f32 unk20;                    /* height above the ground = y - groundY */
    /* 0x24 */ f32 fallSpeed;                /* downward speed (gravity 8, +32 off the road surface; >= -100) */
    /* 0x28 */ u8  pad28[4];
    /* 0x2C */ s32 tri;                      /* triangle number of the hit in its mesh */
    /* 0x30 */ s16 surfFlags;                /* ModelItem flags of the ground object (0xB00 = road) */
    /* 0x32 */ u8  section;                  /* road section of the ground object */
    /* 0x33 */ u8  offroad;                  /* surface: 0 road, 1 other ground, 2 kerb/verge (list B), 3 no ground, 4 outside the road edge */
} CarProbe; /* size = 0x34 */

/* Road position (result of roadLocate) */
typedef struct RoadPos {
    /* 0x00 */ f32 heading;                  /* road heading at this point */
    /* 0x04 */ f32 aim;                      /* heading from the query position to the next path point */
    /* 0x08 */ Vec3f pos;                    /* nearest point on the route (world units) */
    /* 0x14 */ f32 t;                        /* 0..1 along the segment point -> point+1 */
    /* 0x18 */ s32 point;                    /* path point index (also the search start) */
} RoadPos;

typedef struct Car {
    /* 0x000 */ u8        model; /* car model (gCarModels index) */
    /* 0x001 */ u8        gear; /* current gear 1-4 (0 = neutral/none) */
    /* 0x002 */ u8        gearTarget; /* manual gearbox: requested gear */
    /* 0x003 */ u8        shiftTimer; /* frames until gearTarget is engaged */
    /* 0x004 */ u8        manual; /* non-zero: manual transmission */
    /* 0x005 */ u8        pad005[3];
    /* 0x008 */ f32       throttle; /* 0..1 */
    /* 0x00C */ f32       brake; /* 0..1 */
    /* 0x010 */ f32       steer; /* -1..1 */
    /* 0x014 */ f32       enginePower;
    /* 0x018 */ f32       dragRoad; /* drag per probe on road */
    /* 0x01C */ f32       dragOffroad; /* drag per probe off road */
    /* 0x020 */ f32       grip;
    /* 0x024 */ f32       mass; /* crash weight (model table +0x34); >= 2 counts as heavy */
    /* 0x028 */ f32       slide; /* 0..1, sideways slide / skid amount */
    /* 0x02C */ u8        spinTimer; /* spin-out in progress */
    /* 0x02D */ u8        unk2D;
    /* 0x02E */ u8        airborne;
    /* 0x02F */ u8        flipping; /* big crash: car tumbling through the air */
    /* 0x030 */ u8        muted;
    /* 0x031 */ u8        sparkTimer;
    /* 0x032 */ u8        pad032[2];
    /* 0x034 */ f32       prevHeading;
    /* 0x038 */ f32       bodyRoll; /* drawn lean into turns (carBodyLean) */
    /* 0x03C */ f32       bodyPitch; /* drawn pitch from accelerating / braking */
    /* 0x040 */ f32       unk40;
    /* 0x044 */ f32       heading; /* direction the car body points (radians) */
    /* 0x048 */ f32       travelDir; /* direction the car is moving (radians) */
    /* 0x04C */ f32       yawRate;
    /* 0x050 */ f32       unk50;
    /* 0x054 */ f32       spinRemaining;
    /* 0x058 */ f32       yawDelta;
    /* 0x05C */ f32       steerYaw;
    /* 0x060 */ f32       speed;
    /* 0x064 */ f32       unk64;
    /* 0x068 */ f32       frameDist; /* distance covered this frame */
    /* 0x06C */ f32       odometer; /* total distance */
    /* 0x070 */ f32       roadOffset; /* signed distance sideways from the route line */
    /* 0x074 */ s32       unk74;
    /* 0x078 */ s32       rpm; /* engine revs (tachometer units) */
    /* 0x07C */ u8        unk7C;
    /* 0x07D */ u8        unk7D;
    /* 0x07E */ u8        hitFlag; /* traffic: set when hit, makes sparks next frame */
    /* 0x07F */ u8        pad7F;
    /* 0x080 */ s32       unk80;
    /* 0x084 */ s32       unk84;
    /* 0x088 */ CarProbe  probes[5]; /* road contact probes */
    /* 0x18C */ RoadPos   road; /* where the car is on the route (roadLocate); also the respawn point */
    /* 0x1A8 */ s32       unk1A8;
    /* 0x1AC */ u8        unk1AC;
    /* 0x1AD */ u8        boosting;
    /* 0x1AE */ u8        pad1AE[2];
    /* 0x1B0 */ f32       speedScale;
    /* 0x1B4 */ f32       boostPower;
    /* 0x1B8 */ Vec3f     pos;
    /* 0x1C4 */ Mtx3      orient; /* orientation (rows: right, up, forward) */
    /* 0x1E8 */ Vec3f     vel; /* velocity per frame */
    /* 0x1F4 */ f32       bound; /* collision pre-check size (model +0xF4): pairs closer than boundA+boundB (squared distance) are tested */
    /* 0x1F8 */ Vec3f     corners[8]; /* collision box corners in world space (carModelPose); 0,1,4,5 = front */
    /* 0x258 */ u8        pad258[4];
} Car; /* size = 0x25C */

/* Main-segment helpers */
f32 sqrtf(f32 x);                        /* 0x80115720 */
s32 tblSin(s32 angle);                   /* 0x80113E80  sin * 16384 */
s32 tblCos(s32 angle);                   /* 0x80113EDC  cos * 16384 */
s32 atan2s(f32 y, f32 x);                /* 0x80114100  binary angle */

/* Per-player camera (0xA0 bytes, at 0x80046FA0 in the race overlay); see camera.c.
 * Position is in drawing units (world * 0.2, y negated). */
typedef struct RaceCamera {
    /* 0x00 */ f32 posX;
    /* 0x04 */ f32 posY;
    /* 0x08 */ f32 posZ;
    /* 0x0C */ f32 baseX;                    /* position without the finish-line orbit */
    /* 0x10 */ f32 baseY;
    /* 0x14 */ f32 baseZ;
    /* 0x18 */ f32 yaw;
    /* 0x1C */ f32 unk1C;                    /* pitch-like: in-car 0.7 * the car's, plus a 2-player tilt */
    /* 0x20 */ f32 pitch;
    /* 0x24 */ u8  pad24[4];
    /* 0x28 */ s32 rot[4][4];                /* last view matrix (16.16), in-car view smooths towards it */
    /* 0x68 */ CarProbe probe;               /* ground test at the camera position */
    /* 0x9C */ u8  pad9C[4];
} RaceCamera; /* size = 0xA0 */
extern RaceCamera gRaceCameras[2];       /* 0x80046FA0 ovl2 */

/* Per-racer bookkeeping (0x70 bytes each, 0x80046E90 ovl2) */
typedef struct RacerInfo {
    /* 0x00 */ s32 position;                 /* 0 = leading */
    /* 0x04 */ s32 framesLeading;
    /* 0x08 */ s32 offroadTime;              /* builds up while off the track; respawn at 600 */
    /* 0x0C */ f32 engineVolume;
    /* 0x10 */ s32 hornTimer;
    /* 0x14 */ s32 rpmShown;
    /* 0x18 */ u8  wasAirborne;
    /* 0x19 */ u8  kerbRumble;
    /* 0x1A */ u8  pad1A;
    /* 0x1B */ u8  unk1B;
    /* 0x1C */ s32 section;                  /* last road section reached */
    /* 0x20 */ u32 barrierSfxFrame;
    /* 0x24 */ u8  revSfx;
    /* 0x25 */ u8  rumbleSfx;
    /* 0x26 */ u8  gravelSfx;
    /* 0x27 */ u8  unk27;
    /* 0x28 */ u8  brakeSfx;
    /* 0x29 */ u8  skidSfx;
    /* 0x2A */ u8  skidSfxPrev;
    /* 0x2B */ u8  pad2B;
    /* 0x2C */ f32 flipRollRate;
    /* 0x30 */ f32 flipPitchRate;
    /* 0x34 */ f32 flipRoll;
    /* 0x38 */ f32 flipPitch;
    /* 0x3C */ f32 flipHeight;
    /* 0x40 */ s32 spinFrames;
    /* 0x44 */ s32 spinReversals;
    /* 0x48 */ f32 lastYawRate;
    /* 0x4C */ s32 respawns;
    /* 0x50 */ s32 pathAhead;
    /* 0x54 */ s32 pathBehind;
    /* 0x58 */ f32 lastX;
    /* 0x5C */ f32 lastZ;
    /* 0x60 */ f32 unk60;
    /* 0x64 */ f32 unk64;
    /* 0x68 */ s32 allowReverse;
    /* 0x6C */ s32 noSpin;
} RacerInfo; /* size = 0x70 */
extern RacerInfo gRacers[];              /* 0x80046E90 ovl2 */

/* AI / traffic state per vehicle (0x90 bytes, 0x80065830 ovl2) */
typedef struct VehicleState {
    /* 0x00 */ f32 finishDist;                /* AI has finished: coasts this far then parks (0 = racing) */
    /* 0x04 */ f32 laneOffset;
    /* 0x08 */ f32 unk08;
    /* 0x0C */ f32 laneTarget;                /* sideways target (units from the road centre) */
    /* 0x10 */ f32 throttle;
    /* 0x14 */ f32 throttleVar;               /* random variation */
    /* 0x18 */ f32 rubberBand;                /* speed factor from distance to the player */
    /* 0x1C */ f32 rubberGain;
    /* 0x20 */ s32 hornCooldown;
    /* 0x24 */ s32 pathTarget;                /* path point the car is heading for */
    /* 0x28 */ s32 varTimer;
    /* 0x2C */ s32 rubberTimer;
    /* 0x30 */ s32 type;
    /* 0x34 */ s32 vehicle;
    /* 0x38 */ s32 timer;
    /* 0x3C */ u8  offscreen;                 /* simplified movement while no player can see it */
    /* 0x3D */ u8  braking;
    /* 0x3E */ u8  roadSide;                  /* which player's road (split roads) */
    /* 0x3F */ u8  onSplitRoad;
    /* 0x40 */ u8  pad40[4];
    /* 0x44 */ f32 crashPitchRate;
    /* 0x48 */ f32 crashYawRate;
    /* 0x4C */ f32 crashPitch;
    /* 0x50 */ f32 crashYaw;
    /* 0x54 */ u8  remove;                    /* despawn at the end of the frame */
    /* 0x55 */ u8  weaving;                   /* traffic: oncoming car that drifts from side to side */
    /* 0x56 */ u8  pad56[2];
    /* 0x58 */ f32 playerDist2[2];            /* traffic: squared distance to each player */
    /* 0x60 */ f32 prevPlayerDist2[2];
    /* 0x68 */ s32 hornSfx[2];                /* traffic: horn sound, per player, 0 once used */
    /* 0x70 */ s32 passSfx[2];                /* oncoming traffic: near-miss sound, per player */
    /* 0x78 */ u8  offroadTimer;              /* traffic: counts up once all wheels are off the road */
    /* 0x79 */ u8  pad79;
    /* 0x7A */ s8  unk7A;
    /* 0x7B */ u8  pad7B;
    /* 0x7C */ f32 lane;                      /* traffic: sideways position to drive at */
    /* 0x80 */ f32 weaveCentre;
    /* 0x84 */ f32 weavePhase;
    /* 0x88 */ f32 weaveSpeed;
    /* 0x8C */ f32 weaveSize;
} VehicleState; /* size = 0x90 */
extern VehicleState gVehicleStates[];    /* 0x80065830 ovl2 */


/* Controller state per player (0x14 bytes, 0x80046F70 ovl2) */
typedef struct PlayerInput {
    /* 0x00 */ u32 buttons;                  /* RACEBTN_* */
    /* 0x04 */ f32 steer;                    /* -1..1 */
    /* 0x08 */ f32 gas;                      /* 0..1 */
    /* 0x0C */ f32 brake;                    /* 0..1 */
    /* 0x10 */ s32 steerSetting;             /* steering sensitivity option */
} PlayerInput;
extern PlayerInput gPlayerInput[2];      /* 0x80046F70 ovl2 */
#define RACEBTN_SHIFT_UP   0x00001
#define RACEBTN_SHIFT_DOWN 0x00002
#define RACEBTN_QUIT       0x00020
#define RACEBTN_RESET      0x10000

/* A puff of tyre smoke (0x1C bytes); two sets of 12 per vehicle */
typedef struct SmokePuff {
    /* 0x00 */ s32 angle[3];                 /* the car's angles when it was made */
    /* 0x0C */ s32 pos[3];                   /* car-relative, 16.16, drifts each frame */
    /* 0x18 */ s16 frame;                    /* animation step, -1 = free */
    /* 0x1A */ u8  flags;                    /* 1 short animation, 2 dark palette, 4 makes another */
    /* 0x1B */ u8  pad1B;
} SmokePuff;

/* 3D object for every vehicle (0x2F0 bytes, 0x8013FA08); drawn by renderObjDraw */
typedef struct RenderObj {
    /* 0x000 */ s32 model;                   /* car model 0-15 (gCarModelInfo) */
    /* 0x004 */ s32 palette;                 /* colour slot in the model's palette file */
    /* 0x008 */ u16 flags;                   /* RO_* */
    /* 0x00A */ u8  smoke[2];                /* per side: 1 one puff, 2 continuous; 0x80 dark */
    /* 0x00C */ s32 x, y, z;                  /* position relative to the camera, 16.16 drawing units */
    /* 0x018 */ s32 angleZ;
    /* 0x01C */ s32 angleY;
    /* 0x020 */ s32 angleX;
    /* 0x024 */ s32 bodyLift;                /* body raised (suspension), 16.16 */
    /* 0x028 */ s32 angleYAdd;               /* extra tilt (bumps, crashes) */
    /* 0x02C */ s32 angleXAdd;
    /* 0x030 */ s32 shadowDrop;              /* RO_SHADOW_FLAT: the shadow is this far below */
    /* 0x034 */ s32 wheelSpin[2];            /* front / rear wheel rotation (binary angles) */
    /* 0x03C */ s32 steerAngle;              /* front wheels (binary angle) */
    /* 0x040 */ s32 camDist;                 /* cameraDistance2 to the viewing player (picks the detail level) */
    /* 0x044 */ s16 backfire;                /* exhaust flame animation step, -1 = none */
    /* 0x046 */ u8  pad046[2];
    /* 0x048 */ SmokePuff puffs[2][12];
    /* 0x2E8 */ s32 lightTimer;              /* RO_LIGHTS_FLASH countdown */
    /* 0x2EC */ s32 lastFrame;               /* gFrameCounter when last updated */
} RenderObj;
#define RO_SHADOW          0x0001            /* rounded shadow under the car */
#define RO_SHADOW_FLAT     0x0002            /* square shadow, shadowDrop below */
#define RO_DIM             0x0004            /* drawn darker */
#define RO_BRAKE           0x0008            /* brake lights on */
#define RO_LOD0            0x0010            /* always the most detailed model */
#define RO_HOOK            0x0020            /* call gDrawHook before the first wheel */
#define RO_BACKFIRE        0x0040            /* start the exhaust flame */
#define RO_LIGHTS_FLASH    0x0080
#define RO_HIDDEN          0x0200
#define RO_SKIP_UPDATE     0x0400            /* smoke etc. not advanced this frame */
#define RO_GLOW_A          0x0800            /* light glows (headlights) */
#define RO_GLOW_B          0x1000
#define RO_FLAMES          0x2000            /* exhaust flames (boost) */

/* Per-model drawing data (0x5C bytes per model, 0x80134100) */
typedef struct CarModelInfo {
    /* 0x00 */ u8  flags;                    /* 1: a second pair of rear wheels */
    /* 0x01 */ u8  wheelMode[3];             /* per detail level: 0 front wheels steer, 1 two-sided, 2 no wheels */
    /* 0x04 */ s32 wheelY[2];                /* front, rear wheel centres (right side), 16.16 drawing units */
    /* 0x0C */ s32 wheelX[2];
    /* 0x14 */ s32 wheelZ[2];
    /* 0x1C */ s32 extraWheelZ;
    /* 0x20 */ s32 bodyHeight;               /* body drawn this much above the object */
    /* 0x24 */ s32 exhaust[3];               /* exhaust pipes (right side) */
    /* 0x30 */ u32 lodDist[3];               /* detail level used up to this camera distance (0 = none) */
    /* 0x3C */ u32 *bodyDL[3];               /* per detail level: slot in gCarModels */
    /* 0x48 */ s32 unk48;
    /* 0x4C */ u32 *wheelDL[2][2];           /* [two-sided][front, rear] */
} CarModelInfo;
extern CarModelInfo gCarModelInfo[];     /* 0x80134100 */

extern RenderObj gRenderObjs[];          /* 0x8013FA08 */

extern Car gCars[];                      /* 0x800470E0 ovl2 - players, then AI racers, then traffic */
extern s32 gNumPlayers;                  /* 0x8004A010 ovl2 - human players (1 or 2); AI racers and traffic follow them in gCars */
extern s32 gNumAiRacers;                 /* 0x8004A014 ovl2 */
extern s32 gNumVehicles;                 /* 0x8004A01C ovl2 - players + AI + traffic */
extern s32 gRaceState;                   /* 0x80046E5C ovl2 - 0 starting, 1 racing, 2 finished */
extern s32 gRaceStateB;                  /* 0x80046E60 ovl2 */
extern s32 gNumPlayersM1;                /* 0x8015095C - number of players minus 1 */
extern s32 gCurrentTrack;                /* 0x8013F9B0 - 0..13 */
extern f32 gTorqueCurve[20];             /* 0x800438FC ovl2 */
extern f32 gGearPower[5];                /* 0x800438C0 ovl2 */
extern f32 gGearRevDrag[5];              /* 0x800438D4 ovl2 */
extern f32 gGearRpmRatio[5];             /* 0x800438E8 ovl2 */

/* math helpers (race/mathutil.c) */
f32  angleWrap(f32 a);
f32  angleWrapPositive(f32 a);
f32  dist2D(f32 x1, f32 y1, f32 x2, f32 y2);
f32  sinf_tbl(f32 a);
f32  cosf_tbl(f32 a);
void bytesClear(u8 *p, s32 n);
void mtx3Identity(Mtx3 m);
void mtx3RotY(Mtx3 m, f32 a);
void mtx3RotX(Mtx3 m, f32 a);
void mtx3Mul(Mtx3 dst, Mtx3 a, Mtx3 b);
void mtx3Transpose(Mtx3 dst, Mtx3 src);
void vec3MulMtx(Vec3f out, Vec3f v, Mtx3 m);
void vec3Forward(Vec3f out, f32 len, Mtx3 m);
void vec3Add(Vec3f out, Vec3f a, Vec3f b);
void vec3Normalize(Vec3f v);
void vec3TriNormal(Vec3f out, Vec3f a, Vec3f b, Vec3f c);
void mtx3ToAngles(Mtx3 m, s32 *outX, s32 *outY, s32 *outZ);
f32  tableLerp(f32 *table, f32 pos, s32 count);
f32  cameraSideOffset(f32 x, f32 z, s32 player);
s32  angleDiff(s16 a, s16 b);
f32  safeDiv(f32 a, f32 b);

#endif
