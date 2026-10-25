/*
 * plat_dc.c - the Dreamcast platform for the port (KallistiOS): files from
 * the disc, controller input, and the PowerVR backend that draws the
 * triangles port/gfx.c makes from the game's display lists.
 *
 * Draw order: the N64 draws in display-list order with or without depth
 * testing; the PowerVR draws opaque, then punch-through, then translucent
 * polygons. So that the order still holds:
 *   - polygons drawn without depth test before any depth-tested one this
 *     frame (sky, backdrop) are put at the far end of the depth range and
 *     tested GEQUAL, so later scenery covers them whatever list it is in;
 *   - polygons drawn without depth test but writing depth (the road) keep
 *     their real depth, compare ALWAYS and write it;
 *   - polygons drawn without depth test after that (HUD) are put at the near
 *     end and write depth, so nothing later in another list covers them;
 *   - translucent polygons keep their order (autosort off).
 */
#include <kos.h>
#include <dc/pvr.h>
#include <dc/vblank.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/maple/keyboard.h>
#include <dc/sound/stream.h>
#include <dc/sound/sound.h>
#include <dc/sound/aica_comm.h>
#include <dc/spu.h>
#include <dc/g2bus.h>
#include <dc/vmufs.h>
#include <dc/vmu_pkg.h>
#include <dc/maple/vmu.h>
#include <kos/mutex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
time_t rtc_unix_secs(void);              /* KallistiOS (arch/rtc.h, which kos.h brings in on most versions) */
#include <malloc.h>
#include "port.h"

void port_main(s32 track);

/* ------------------------------------------------------------------ */
/* Files, logging                                                     */
/* ------------------------------------------------------------------ */
void *plat_load_file(const char *name, u32 *size) {
    char path[64];
    FILE *f;
    long n;
    void *buf;

    snprintf(path, sizeof(path), "/cd/%s", name);
    f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(n);
    if (buf != NULL) {
        fread(buf, 1, n, f);
    }
    fclose(f);
    *size = n;
    return buf;
}

void plat_write(const char *s, u32 n) {
    fwrite(s, 1, n, stdout);
}

void plat_exit(int code) {
    arch_exit();
}

void port_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

void port_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    arch_exit();
}

void port_stub(const char *name) {
    printf("stub: %s\n", name);
}

/* ------------------------------------------------------------------ */
/* Input: Dreamcast pad -> N64 pad                                    */
/* ------------------------------------------------------------------ */

/* the date by the console's clock (set in its own settings; a flat clock battery puts it back to
 * 1999 or so); 0 if it isn't sensible */
int plat_date(int *year, int *month, int *day) {
    time_t t = rtc_unix_secs();
    struct tm *tm = gmtime(&t);          /* (the console keeps local time: no time zone to apply) */
    if (tm == NULL || tm->tm_year + 1900 < 2000) {
        return 0;
    }
    *year = tm->tm_year + 1900;
    *month = tm->tm_mon + 1;
    *day = tm->tm_mday;
    printf("date: %04d-%02d-%02d\n", *year, *month, *day);
    return 1;
}

/* a key typed on a keyboard (any port; the cheats on Choose Race): its character, Enter as '\n',
 * 0 if none waiting */
int plat_key(void) {
    maple_device_t *kb = maple_enum_type(0, MAPLE_FUNC_KEYBOARD);
    int k;

    if (kb == NULL) {
        return 0;
    }
    for (;;) {
        k = kbd_queue_pop(kb, 1);        /* translated: the character, or the key's code << 8 */
        if (k < 0) {
            return 0;
        }
        if (k == '\r' || k == (0x28 << 8) || k == (0x58 << 8)) {    /* Enter, keypad Enter */
            return '\n';
        }
        if (k == (0x2A << 8)) {          /* Backspace */
            return 8;
        }
        if (k > 0 && k < 128) {
            return k;
        }
    }
}

int plat_pad(int i, u16 *buttons, s8 *x, s8 *y) {
    maple_device_t *dev = maple_enum_type(i, MAPLE_FUNC_CONTROLLER);
    cont_state_t *st;
    u16 b = 0;
    int sx, sy;

    if (dev == NULL || (st = (cont_state_t *)maple_dev_status(dev)) == NULL) {
        return 0;
    }
    /* each Dreamcast button as one of the game's (N64) button bits; what they do is
     * the controls set-up's (the Dreamcast defaults: port_main.c gDcConfigs):
     *   A 0x8000 (the N64's A: menus' select), X 0x4000 (the N64's B: options,
     *   presets; radio), B 0x0040 and Y 0x0080 (bits the N64 pad never sets;
     *   view, display), left trigger 0x0020 (L; brake), right 0x0010 (R; gas) */
    if (st->buttons & CONT_A) b |= 0x8000;
    if (st->buttons & CONT_X) b |= 0x4000;
    if (st->buttons & CONT_B) b |= 0x0040;
    if (st->buttons & CONT_Y) b |= 0x0080;
    if (st->ltrig > 64) b |= 0x0020;
    if (st->rtrig > 64) b |= 0x0010;
    if (st->buttons & CONT_START) b |= 0x1000;
    if (st->buttons & CONT_DPAD_UP) b |= 0x0800;
    if (st->buttons & CONT_DPAD_DOWN) b |= 0x0400;
    if (st->buttons & CONT_DPAD_LEFT) b |= 0x0200;
    if (st->buttons & CONT_DPAD_RIGHT) b |= 0x0100;
    sx = st->joyx * 80 / 128;
    sy = -st->joyy * 80 / 128;
    *buttons = b;
    *x = (s8)sx;
    *y = (s8)sy;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Saves (port/save.c): one file on the VMU in each player's controller */
/* ------------------------------------------------------------------ */
#include "vmu_icon.c"
#define SAVE_FILE "CRUISNUS.SYS"
#define SAVE_DATA_AT (128 + 512)         /* the VMS header and one icon, then the game's data */

static int gSaveVmuPort[2], gSaveVmuUnit[2];
static mutex_t gVmuLock = MUTEX_INITIALIZER;   /* one VMU job at a time: a save, a load or the screen */

/* the VMU in player p's controller (first slot with one) */
static maple_device_t *save_vmu_of_player(int p, int *unit) {
    maple_device_t *c = maple_enum_type(p, MAPLE_FUNC_CONTROLLER);
    maple_device_t *d;
    int u;

    if (c == NULL) {
        return NULL;
    }
    for (u = 1; u <= 2; u++) {
        d = maple_enum_dev(c->port, u);
        if (d != NULL && (d->info.functions & MAPLE_FUNC_MEMCARD)) {
            *unit = u;
            return d;
        }
    }
    return NULL;
}

static maple_device_t *save_vmu(int dev) {
    maple_device_t *d = maple_enum_dev(gSaveVmuPort[dev], gSaveVmuUnit[dev]);
    return (d != NULL && (d->info.functions & MAPLE_FUNC_MEMCARD)) ? d : NULL;
}

int plat_save_devices(int *playerOfDevice) {
    maple_device_t *d;
    int p, u, n = 0;

    for (p = 0; p < 2; p++) {
        d = save_vmu_of_player(p, &u);
        if (d != NULL) {
            gSaveVmuPort[n] = d->port;
            gSaveVmuUnit[n] = u;
            playerOfDevice[n] = p;
            n++;
        }
    }
    printf("save: %d VMU(s)\n", n);
    return n;
}

static void save_wait(void);
/* 1: read; 0: no save there; -1: a save there that couldn't be used (left as it is: see saveBoot).
 * The game's data is found from the file's own header (its icon count and eyecatch), not a fixed
 * place: saves made with a different number of icons (the 3-frame icon of earlier builds) read too. A failed read is tried again (at start-up the
 * VMU may not answer the first time). */
int plat_save_read(int dev, void *buf, int size) {
    maple_device_t *d;
    void *data = NULL;
    int got = 0, ok = -1, tries, icons = 0;

    save_wait();                         /* (a write still going: read what it wrote) */
    for (tries = 0; tries < 4; tries++) {
        d = save_vmu(dev);
        int r = -1;
        if (d != NULL) {
            mutex_lock(&gVmuLock);       /* (not while the screen is being sent) */
            r = vmufs_read(d, SAVE_FILE, &data, &got);
            mutex_unlock(&gVmuLock);
        }
        if (r >= 0 && data != NULL) {
            break;
        }
        data = NULL;
        thd_sleep(150);
    }
    if (data == NULL) {
        printf("save: no file on VMU %c%d\n", 'A' + gSaveVmuPort[dev], gSaveVmuUnit[dev]);
        return 0;
    }
    {   /* the VMS header: icons at 0x40, eyecatch type at 0x44, the data's length at 0x48 */
        const uint8_t *h = (const uint8_t *)data;
        static const int eyecatch[4] = { 0, 72 * 56 * 2, 512 + 72 * 56, 32 + 72 * 56 / 2 };
        icons = got >= 128 ? h[0x40] | (h[0x41] << 8) : 0;
        int ec = got >= 128 ? h[0x44] | (h[0x45] << 8) : 0;
        uint32_t len = got >= 128 ? h[0x48] | (h[0x49] << 8) | (h[0x4A] << 16) | ((uint32_t)h[0x4B] << 24) : 0;
        int at = 128 + icons * 512 + (ec >= 0 && ec < 4 ? eyecatch[ec] : 0);
        if (icons >= 1 && icons <= 3 && len >= (uint32_t)size && at + size <= got) {
            memcpy(buf, h + at, size);
            ok = 1;
        } else if (got >= SAVE_DATA_AT + size) {
            memcpy(buf, h + SAVE_DATA_AT, size);   /* (as before, if the header doesn't make sense) */
            ok = 1;
        }
    }
    printf("save: read VMU %c%d (%d bytes, %d icons) %s\n", 'A' + gSaveVmuPort[dev], gSaveVmuUnit[dev], got,
           icons, ok > 0 ? "ok" : "not usable");
    free(data);
    return ok;
}

/* The VMU screen (port_main.c port_vmu_frame): player 1's VMU with a screen. Sending a
 * picture waits for the VMU's answer, so a thread of its own does it while the game goes on;
 * it checks a few times a second for a new picture or a VMU plugged in. The VMU shows its
 * screen data upside down (as KallistiOS's vmu_set_icon): turned round here. */
static uint8_t gLcdWant[192];
static volatile int gLcdSeq;
static kthread_t *gLcdThread;

static maple_device_t *lcd_vmu(void) {
    maple_device_t *c = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    maple_device_t *d;
    int u;
    if (c == NULL) {
        return NULL;
    }
    for (u = 1; u <= 2; u++) {
        d = maple_enum_dev(c->port, u);
        if (d != NULL && (d->info.functions & MAPLE_FUNC_LCD)) {
            return d;
        }
    }
    return NULL;
}

static void *lcd_thread(void *arg) {
    int done = -1;
    maple_device_t *last = NULL;
    uint8_t raw[192];
    (void)arg;
    for (;;) {
        maple_device_t *d = lcd_vmu();
        int seq = gLcdSeq;
        if (d != NULL && (seq != done || d != last)) {
            int x, y;
            memset(raw, 0, sizeof(raw));
            for (y = 0; y < 32; y++) {
                for (x = 0; x < 48; x++) {
                    if (gLcdWant[y * 6 + (x >> 3)] & (0x80 >> (x & 7))) {
                        int rx = 47 - x, ry = 31 - y;
                        raw[ry * 6 + (rx >> 3)] |= 0x80 >> (rx & 7);
                    }
                }
            }
            mutex_lock(&gVmuLock);
            vmu_draw_lcd(d, raw);
            mutex_unlock(&gVmuLock);
            done = seq;
        }
        last = d;
        thd_sleep(100);
    }
    return NULL;
}

void plat_vmu_lcd(const u8 *bits) {
    memcpy(gLcdWant, bits, sizeof(gLcdWant));
    gLcdSeq++;
    if (gLcdThread == NULL) {
        gLcdThread = thd_create(1, lcd_thread, NULL);   /* (detached: runs for good) */
    }
}

/* Writing to the VMU takes a good part of a second, and the game (pictures and music) would stop
 * while it did: the file is made here, and written by a thread of its own while the game goes on
 * (the maple bus serves both). A save made while the last is still being written waits for it. */
typedef struct SaveJob {
    maple_device_t *dev;
    uint8_t *file;
    int size, port, unit;
} SaveJob;
static kthread_t *gSaveThread;
static SaveJob gSaveJob;

static void *save_thread(void *arg) {
    SaveJob *j = (SaveJob *)arg;
    int r;
    mutex_lock(&gVmuLock);               /* (the screen waits for the save) */
    r = vmufs_write(j->dev, SAVE_FILE, j->file, j->size, VMUFS_OVERWRITE);
    mutex_unlock(&gVmuLock);
    if (r < 0) {                         /* once more, after a moment */
        thd_sleep(200);
        mutex_lock(&gVmuLock);
        r = vmufs_write(j->dev, SAVE_FILE, j->file, j->size, VMUFS_OVERWRITE);
        mutex_unlock(&gVmuLock);
    }
    printf("save: VMU %c%d %s\n", 'A' + j->port, j->unit, r < 0 ? "failed" : "written");
    free(j->file);
    j->file = NULL;
    return NULL;
}

static void save_wait(void) {
    if (gSaveThread != NULL) {
        thd_join(gSaveThread, NULL);
        gSaveThread = NULL;
    }
}

int plat_save_write(int dev, const void *buf, int size) {
    maple_device_t *d = save_vmu(dev);
    vmu_pkg_t pkg;
    uint8_t *file = NULL;
    int fileSize = 0;

    if (d == NULL) {
        return 0;
    }
    memset(&pkg, 0, sizeof(pkg));
    strcpy(pkg.desc_short, "CRUIS'N USA");
    strcpy(pkg.desc_long, "CRUIS'N USA SAVE DATA");
    strcpy(pkg.app_id, "CRUISN USA DC");
    pkg.icon_cnt = 1;
    pkg.icon_anim_speed = 0;
    pkg.eyecatch_type = VMUPKG_EC_NONE;
    pkg.data_len = size;
    memcpy(pkg.icon_pal, gVmuIconPal, sizeof(gVmuIconPal));
    pkg.icon_data = (uint8_t *)gVmuIcon;
    pkg.data = (const uint8_t *)buf;
    if (vmu_pkg_build(&pkg, &file, &fileSize) < 0 || file == NULL) {
        return 0;
    }
    save_wait();                         /* (the last one first) */
    gSaveJob.dev = d;
    gSaveJob.file = file;
    gSaveJob.size = fileSize;
    gSaveJob.port = gSaveVmuPort[dev];
    gSaveJob.unit = gSaveVmuUnit[dev];
    gSaveThread = thd_create(0, save_thread, &gSaveJob);
    if (gSaveThread == NULL) {
        save_thread(&gSaveJob);          /* (no thread: as before, the game waits) */
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Textures: cached in video memory by the key gfx.c gives            */
/* ------------------------------------------------------------------ */
#define TEX_SLOTS 2048
typedef struct TexEntry {
    u32 key;
    pvr_ptr_t ptr;
    u16 w, h;                            /* power-of-two size in video memory */
    u16 srcW, srcH;
    s32 used;                            /* last frame it was drawn with */
} TexEntry;

/* the last header put in each list: the same again is left out (the
 * following triangles carry on with it) */
static struct { s32 valid, zcmp, zwrite, wrapS, wrapT; pvr_ptr_t ptr; u16 w, h; } gLastHdr[3];
static TexEntry gTex[TEX_SLOTS];
static s32 gTexCount;
static s32 gFrameNo;
/* this frame's texture work, for the timing bars */
static s32 gUploads, gEvictions;
static uint32_t gUploadUs;

/* a cheap microsecond clock for the timing bars (called a few thousand times
 * a frame): the uptime timer's counter read directly (TMU2, counting down at
 * 12.5 MHz from TCOR2 to 0) instead of the library's full uptime calculation */
u32 plat_time_us(void) {
    static u32 last, us, rem;
    u32 now = *(volatile u32 *)0xFFD80024;           /* TCNT2 */
    u32 ticks = (last >= now) ? last - now : last + *(volatile u32 *)0xFFD80020 - now;   /* + TCOR2 on wrap */

    last = now;
    rem += ticks * 2;                                /* 80 ns ticks: 2/25 of a microsecond */
    us += rem / 25;
    rem %= 25;
    return us;
}

static TexEntry *tex_find(u32 key) {
    u32 i = (key * 2654435761u) & (TEX_SLOTS - 1);
    while (gTex[i].ptr != NULL) {
        if (gTex[i].key == key) {
            return &gTex[i];
        }
        i = (i + 1) & (TEX_SLOTS - 1);
    }
    return NULL;
}

/* new textures are spread over frames so that a burst of them (a new part of
 * the track coming into view) does not hold up one frame: once this frame has
 * spent its share, the rest wait (their triangles are left out until then) */
void *gb_alloc(u32 n) {
    return malloc(n);
}

void gb_free(void *p) {
    free(p);
}

s32 gb_tex_room(void) {
    return gUploads < 12 && gUploadUs < 4000;
}

s32 gb_tex_known(u32 key) {
    TexEntry *e = tex_find(key);
    if (e != NULL) {
        e->used = gFrameNo;              /* about to be drawn: keep it */
        return 1;
    }
    return 0;
}

/* free the textures not drawn this frame or the last (the PowerVR may still be
 * drawing last frame's), then rebuild the table; returns how many were freed */
static s32 tex_evict(void) {
    static TexEntry keep[TEX_SLOTS];
    s32 i, n = 0, freed = 0;

    for (i = 0; i < TEX_SLOTS; i++) {
        if (gTex[i].ptr == NULL) {
            continue;
        }
        if (gTex[i].used >= gFrameNo - 1) {
            keep[n++] = gTex[i];
        } else {
            pvr_mem_free(gTex[i].ptr);
            freed++;
        }
        gTex[i].ptr = NULL;
    }
    for (i = 0; i < n; i++) {
        u32 j = (keep[i].key * 2654435761u) & (TEX_SLOTS - 1);
        while (gTex[j].ptr != NULL) {
            j = (j + 1) & (TEX_SLOTS - 1);
        }
        gTex[j] = keep[i];
    }
    gTexCount = n;
    gEvictions++;
    gLastHdr[0].valid = gLastHdr[1].valid = gLastHdr[2].valid = 0;   /* (freed video memory may be used again) */
    return freed;
}

/* twiddled order: the bits of x and y interleaved */
static u16 gSpread[256];
static u16 gTwBuf[256 * 256];

static void twiddle_init(void) {
    u32 v, b;
    for (v = 0; v < 256; v++) {
        u32 s = 0;
        for (b = 0; b < 8; b++) {
            s |= ((v >> b) & 1) << (2 * b);
        }
        gSpread[v] = (u16)s;
    }
}

static s32 log2i(u32 v) {
    s32 n = 0;
    while ((1u << n) < v) n++;
    return n;
}

static TexEntry *tex_upload(const GfxTex *t) {
    u32 w = 8, h = 8, x, y, i, m, lm, lw, lh;
    TexEntry *e;
    pvr_ptr_t p;
    uint64_t t0 = timer_us_gettime64();
    static u16 sxs[1024];

    if (gTexCount >= TEX_SLOTS * 3 / 4 && tex_evict() == 0) {
        return NULL;
    }
    while (w < (u32)t->width) w <<= 1;
    while (h < (u32)t->height) h <<= 1;
    p = pvr_mem_malloc(w * h * 2);
    if (p == NULL) {
        if (tex_evict() == 0 || (p = pvr_mem_malloc(w * h * 2)) == NULL) {
            return NULL;
        }
    }
    /* stretched (not padded) to the power-of-two size, so textures that
     * repeat (road markings) tile at the right spacing */
    m = w < h ? w : h;
    lm = log2i(m);
    lw = log2i(w);
    lh = log2i(h);
    for (x = 0; x < w; x++) {
        sxs[x] = (u16)((x * (u32)t->width) >> lw);
    }
    for (y = 0; y < h; y++) {
        const u16 *src = t->argb + ((y * (u32)t->height) >> lh) * t->width;
        u32 ybits = gSpread[y & (m - 1)];
        u32 ybase = (w > h) ? 0 : (y >> lm) << (2 * lm);
        for (x = 0; x < w; x++) {
            u32 idx = ybits | (gSpread[x & (m - 1)] << 1);
            idx += (w > h) ? (x >> lm) << (2 * lm) : ybase;
            gTwBuf[idx] = src[sxs[x]];
        }
    }
    pvr_txr_load(gTwBuf, p, w * h * 2);
    i = (t->key * 2654435761u) & (TEX_SLOTS - 1);
    while (gTex[i].ptr != NULL) {
        i = (i + 1) & (TEX_SLOTS - 1);
    }
    e = &gTex[i];
    e->key = t->key;
    e->ptr = p;
    e->w = w;
    e->h = h;
    e->srcW = t->width;
    e->srcH = t->height;
    e->used = gFrameNo;
    gTexCount++;
    gUploads++;
    gUploadUs += (uint32_t)(timer_us_gettime64() - t0);
    return e;
}

/* ------------------------------------------------------------------ */
/* Polygon lists, filled during the frame, sent at the end            */
/* ------------------------------------------------------------------ */
#define LIST_ITEMS 12288                 /* 32-byte headers and vertices per list */
typedef struct PolyList { pvr_vertex_t *buf; s32 n; } PolyList;
static PolyList gLists[3];               /* opaque, punch-through, translucent */
static const s32 gListId[3] = { PVR_LIST_OP_POLY, PVR_LIST_PT_POLY, PVR_LIST_TR_POLY };
static s32 gSeen3D;                      /* a depth-tested polygon was drawn this frame */

static void *list_alloc(s32 l, s32 n) {
    PolyList *pl = &gLists[l];
    void *p;
    if (pl->n + n > LIST_ITEMS) {
        return NULL;
    }
    p = &pl->buf[pl->n];
    pl->n += n;
    return p;
}

/* The translucent list (nearly everything) is not kept in memory: it is open
 * all frame and each polygon goes to the PowerVR as it is made, through a
 * small staging buffer that stays in the cache (sent with the store queues
 * when full). The opaque list (the depth reset, a few others) is kept and
 * sent at the end of the frame. */
#define STAGE 128
static pvr_vertex_t gStage[STAGE] __attribute__((aligned(32)));
static s32 gStageN, gTrItems;

static void stage_flush(void) {
    if (gStageN > 0) {
        pvr_prim(gStage, gStageN * 32);
        gStageN = 0;
    }
}

/* room for one 32-byte header or vertex in list l */
static inline void *slot(s32 l) {
    if (l != 2) {
        return list_alloc(l, 1);
    }
    if (gStageN == STAGE) {
        stage_flush();
    }
    return &gStage[gStageN++];
}


#define HDR_BITS 8
typedef struct HdrCache {
    pvr_poly_hdr_t hdr;
    pvr_ptr_t ptr;
    u32 sz, mode;
    s32 valid;
} HdrCache;
static HdrCache gHdrCache[1 << HDR_BITS] __attribute__((aligned(32)));
static void hdr_compile(pvr_poly_hdr_t *hdr, s32 l, TexEntry *te, const GfxTex *t, s32 zcmp, s32 zwrite);

static void put_header(s32 l, TexEntry *te, const GfxTex *t, s32 zcmp, s32 zwrite) {
    pvr_poly_hdr_t *hdr;
    pvr_ptr_t ptr = te ? te->ptr : NULL;
    s32 ws = te ? t->wrapS : -1, wt = te ? t->wrapT : -1;

    if (gLastHdr[l].valid && gLastHdr[l].zcmp == zcmp && gLastHdr[l].zwrite == zwrite && gLastHdr[l].ptr == ptr &&
        gLastHdr[l].wrapS == ws && gLastHdr[l].wrapT == wt && (te == NULL || (gLastHdr[l].w == te->w && gLastHdr[l].h == te->h))) {
        return;
    }
    hdr = slot(l);
    if (hdr == NULL) {
        gLastHdr[l].valid = 0;
        return;
    }
    gLastHdr[l].valid = 1;
    gLastHdr[l].zcmp = zcmp;
    gLastHdr[l].zwrite = zwrite;
    gLastHdr[l].ptr = ptr;
    gLastHdr[l].wrapS = ws;
    gLastHdr[l].wrapT = wt;
    gLastHdr[l].w = te ? te->w : 0;
    gLastHdr[l].h = te ? te->h : 0;
    {   /* compiled headers, kept: building one (pvr_poly_cxt_txr + pvr_poly_compile) was
         * ~9% of a frame on San Francisco. A header only says where the texture is,
         * its size, wrapping and the depth test, so a kept one stays right even when
         * the video memory is used for another texture since. */
        u32 sz = te ? ((u32)te->w << 16) | te->h : 0;
        u32 mode = (u32)l | ((u32)(ws + 1) << 2) | ((u32)(wt + 1) << 4) | ((u32)zcmp << 6) | ((u32)(zwrite != 0) << 10);
        u32 hsh = (((u32)ptr >> 3) * 2654435761u ^ sz * 40503u ^ mode * 2246822519u) >> (32 - HDR_BITS);
        HdrCache *c = &gHdrCache[hsh];
        if (!(c->valid && c->ptr == ptr && c->sz == sz && c->mode == mode)) {
            c->valid = 0;
            hdr_compile(&c->hdr, l, te, t, zcmp, zwrite);
            c->ptr = ptr;
            c->sz = sz;
            c->mode = mode;
            c->valid = 1;
        }
        *hdr = c->hdr;
        return;
    }
}

static void hdr_compile(pvr_poly_hdr_t *hdr, s32 l, TexEntry *te, const GfxTex *t, s32 zcmp, s32 zwrite) {
    pvr_poly_cxt_t cxt;

    if (te != NULL) {
        pvr_poly_cxt_txr(&cxt, gListId[l], PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_TWIDDLED,
                         te->w, te->h, te->ptr, PVR_FILTER_BILINEAR);
        cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
        cxt.txr.uv_clamp = (t->wrapS == 0 ? PVR_UVCLAMP_U : 0) | (t->wrapT == 0 ? PVR_UVCLAMP_V : 0);
        cxt.txr.uv_flip = (t->wrapS == 2 ? PVR_UVFLIP_U : 0) | (t->wrapT == 2 ? PVR_UVFLIP_V : 0);
    } else {
        pvr_poly_cxt_col(&cxt, gListId[l]);
    }
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.comparison = zcmp;
    cxt.depth.write = zwrite ? PVR_DEPTHWRITE_ENABLE : PVR_DEPTHWRITE_DISABLE;
    pvr_poly_compile(hdr, &cxt);
}

/* first polygon of the frame: the whole screen, black, at the far end of the
 * depth range (compare ALWAYS, write), so the depth buffer starts from a value
 * we know rather than the hardware's background plane depth */
static void depth_reset(void) {
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t *hdr = list_alloc(0, 1);
    pvr_vertex_t *v = list_alloc(0, 4);

    gLastHdr[0].valid = 0;
    static const float xs[4] = { 0, 0, 640, 640 }, ys[4] = { 480, 0, 480, 0 };
    s32 i;

    if (hdr == NULL || v == NULL) {
        return;
    }
    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write = PVR_DEPTHWRITE_ENABLE;
    pvr_poly_compile(hdr, &cxt);
    for (i = 0; i < 4; i++) {
        v[i].flags = (i == 3) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        v[i].x = xs[i];
        v[i].y = ys[i];
        v[i].z = 0.00000001f;
        v[i].u = v[i].v = 0.0f;
        v[i].argb = 0xFF000000;
        v[i].oargb = 0;
    }
}

void gb_frame_begin(void) {
    s32 i;

    pvr_wait_ready();
    gFrameNo++;
    gUploads = gEvictions = 0;
    gUploadUs = 0;
    for (i = 0; i < 3; i++) {
        gLists[i].n = 0;
    }
    gStageN = gTrItems = 0;
    gLastHdr[0].valid = gLastHdr[1].valid = gLastHdr[2].valid = 0;
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_TR_POLY);    /* open all frame: polygons go straight in */
    gSeen3D = 0;
    depth_reset();
}

/* floor() without the library call */
static inline f32 ffloor(f32 x) {
    f32 t = (f32)(s32)x;
    return (t > x) ? t - 1.0f : t;
}

void gb_draw(GfxVert *v, s32 n, GfxTex *tex, s32 blend, s32 zcmp, s32 zwrite) {
    /* cutout (1-bit alpha) polygons go in the translucent list too: drawn in
     * order with 0/1 alpha they look the same, and the punch-through list drew
     * nothing in the first Flycast test */
    s32 l = (blend == GB_OPAQUE) ? 0 : 2;
    s32 cmp, i;
    f32 fixedZ = 0.0f;
    TexEntry *te = NULL;
    pvr_vertex_t *out;
    f32 us = 0, vs = 0, du = 0, dv = 0;

    if (tex != NULL) {
        te = tex_find(tex->key);
        if (te == NULL && tex->argb != NULL) {
            te = tex_upload(tex);
        }
        if (te != NULL) {
            te->used = gFrameNo;
            us = 1.0f / te->srcW;
            vs = 1.0f / te->srcH;
        }
    }
    if (zcmp) {
        gSeen3D = 1;
        cmp = PVR_DEPTHCMP_GEQUAL;
    } else if (zwrite) {
        /* drawn in order over what is there, but writing depth (the road:
         * scenery and cars behind a hill must stay hidden); its real depth
         * also keeps the texture in perspective */
        gSeen3D = 1;
        cmp = PVR_DEPTHCMP_ALWAYS;
    } else if (!gSeen3D) {
        fixedZ = 0.0000001f;             /* background */
        cmp = PVR_DEPTHCMP_GEQUAL;
        zwrite = 0;
    } else if (n > 0 && v[0].q > 0.0f && v[0].q < 0.5f) {
        /* in perspective, between depth-tested drawing (the garage floor in
         * CHOOSE CAR): drawn in order over what is there, and leaving the depth
         * alone so the cars drawn after it still show (written at the near end
         * it hid their lower halves) */
        cmp = PVR_DEPTHCMP_ALWAYS;
    } else {
        fixedZ = 10.0f;                  /* overlay */
        cmp = PVR_DEPTHCMP_ALWAYS;
        zwrite = 1;
    }
    if (te == NULL && tex != NULL && l == 1) {
        l = 0;
    }
    /* room for the header and the vertices (as much as the lists had before) */
    if (l == 2) {
        if (gTrItems + 1 + n > LIST_ITEMS) {
            return;
        }
        gTrItems += 1 + n;
    } else if (gLists[l].n + 1 + n > LIST_ITEMS) {
        return;
    }
    put_header(l, te, tex, cmp, zwrite);
    for (i = 0; i < n; i++) {
        const GfxVert *s = &v[i];
        out = slot(l);
        if (i % 3 == 0) {
            /* keep each triangle's texture coordinates near 0 by whole repeats
             * (the PowerVR loses precision on big values: smeared road lines) */
            du = dv = 0.0f;
            if (tex != NULL) {
                f32 mu = s->u * us, mv = s->v * vs;
                if (s[1].u * us < mu) mu = s[1].u * us;
                if (s[2].u * us < mu) mu = s[2].u * us;
                if (s[1].v * vs < mv) mv = s[1].v * vs;
                if (s[2].v * vs < mv) mv = s[2].v * vs;
                if (tex->wrapS == 1) du = ffloor(mu);
                else if (tex->wrapS == 2) du = 2.0f * ffloor(mu * 0.5f);
                if (tex->wrapT == 1) dv = ffloor(mv);
                else if (tex->wrapT == 2) dv = 2.0f * ffloor(mv * 0.5f);
            }
        }
        out->flags = (i % 3 == 2) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        out->x = s->x * 2.0f;
        out->y = s->y * 2.0f;
        out->z = (fixedZ != 0.0f) ? fixedZ : s->q;
        out->u = s->u * us - du;
        out->v = s->v * vs - dv;
        out->argb = ((u32)s->a << 24) | ((u32)s->r << 16) | ((u32)s->g << 8) | s->b;
        out->oargb = 0;
    }
}

/* the scenery replay's direct path (port/gfx_replay.h): the same as gb_draw,
 * with the vertices converted once each (they are shared by the triangles)
 * and the triangles written straight into the list */
static struct { s32 l, wrapS, wrapT, tex; f32 fixedZ; } gPrim;

f32 gb_prim_xyscale(void) {
    return 2.0f;                         /* (as gb_prim_begin's xs, ys) */
}

s32 gb_prim_begin(const GfxTex *tex, s32 blend, s32 zcmp, s32 zwrite, GbPrim *p) {
    s32 l = (blend == GB_OPAQUE) ? 0 : 2;
    s32 cmp;
    TexEntry *te = NULL;

    p->xs = p->ys = 2.0f;
    p->us = p->vs = 0.0f;
    gPrim.fixedZ = 0.0f;
    if (tex != NULL) {
        te = tex_find(tex->key);
        if (te == NULL) {
            return 0;                    /* (the replay checks they are loaded) */
        }
        te->used = gFrameNo;
        p->us = 1.0f / te->srcW;
        p->vs = 1.0f / te->srcH;
    }
    if (zcmp) {
        gSeen3D = 1;
        cmp = PVR_DEPTHCMP_GEQUAL;
    } else if (zwrite) {
        gSeen3D = 1;
        cmp = PVR_DEPTHCMP_ALWAYS;
    } else if (!gSeen3D) {
        gPrim.fixedZ = 0.0000001f;
        cmp = PVR_DEPTHCMP_GEQUAL;
        zwrite = 0;
    } else {
        gPrim.fixedZ = 10.0f;
        cmp = PVR_DEPTHCMP_ALWAYS;
        zwrite = 1;
    }
    if (l == 2) {
        if (gTrItems + 1 > LIST_ITEMS) {
            return 0;
        }
        gTrItems += 1;
    } else if (gLists[l].n + 1 > LIST_ITEMS) {
        return 0;
    }
    put_header(l, te, tex, cmp, zwrite);
    gPrim.l = l;
    gPrim.tex = (tex != NULL);
    gPrim.wrapS = tex ? tex->wrapS : 0;
    gPrim.wrapT = tex ? tex->wrapT : 0;
    return 1;
}

static inline void prim_vert(pvr_vertex_t *o, const GbPVert *s, f32 du, f32 dv, u32 flags) {
    o->flags = flags;
    o->x = s->x;
    o->y = s->y;
    o->z = (gPrim.fixedZ != 0.0f) ? gPrim.fixedZ : s->q;
    o->u = s->u - du;
    o->v = s->v - dv;
    o->argb = s->argb;
    o->oargb = 0;
}

void gb_prim_tri(const GbPVert *a, const GbPVert *b, const GbPVert *c) {
    f32 du = 0.0f, dv = 0.0f;
    pvr_vertex_t *o;

    if (gPrim.tex) {
        /* texture coordinates near 0 by whole repeats, as gb_draw */
        f32 mu = a->u, mv = a->v;
        if (b->u < mu) mu = b->u;
        if (c->u < mu) mu = c->u;
        if (b->v < mv) mv = b->v;
        if (c->v < mv) mv = c->v;
        if (gPrim.wrapS == 1) du = ffloor(mu);
        else if (gPrim.wrapS == 2) du = 2.0f * ffloor(mu * 0.5f);
        if (gPrim.wrapT == 1) dv = ffloor(mv);
        else if (gPrim.wrapT == 2) dv = 2.0f * ffloor(mv * 0.5f);
    }
    if (gPrim.l == 2) {
        if (gTrItems + 3 > LIST_ITEMS) {
            return;
        }
        gTrItems += 3;
        if (gStageN + 3 > STAGE) {
            stage_flush();
        }
        o = &gStage[gStageN];
        gStageN += 3;
    } else {
        o = list_alloc(gPrim.l, 3);
        if (o == NULL) {
            return;
        }
    }
    prim_vert(&o[0], a, du, dv, PVR_CMD_VERTEX);
    prim_vert(&o[1], b, du, dv, PVR_CMD_VERTEX);
    prim_vert(&o[2], c, du, dv, PVR_CMD_VERTEX_EOL);
}

void gb_fill(s32 x0, s32 y0, s32 x1, s32 y1, u8 r, u8 g, u8 b, u8 a) {
    GfxVert q[6];
    static const u8 cx[6] = { 0, 1, 1, 0, 1, 0 }, cy[6] = { 0, 0, 1, 0, 1, 1 };
    s32 i;

    for (i = 0; i < 6; i++) {
        q[i].x = cx[i] ? x1 : x0;
        q[i].y = cy[i] ? y1 : y0;
        q[i].z = 0;
        q[i].q = 1;
        q[i].u = q[i].v = 0;
        q[i].r = r; q[i].g = g; q[i].b = b; q[i].a = a;
    }
    gb_draw(q, 6, NULL, a == 255 ? GB_OPAQUE : GB_BLEND, 0, 0);
}

void gb_frame_end(void) {
    stage_flush();
    pvr_list_finish();                   /* translucent: already sent */
    pvr_list_begin(gListId[0]);          /* opaque: kept until now */
    if (gLists[0].n > 0) {
        pvr_prim(gLists[0].buf, gLists[0].n * 32);
    }
    pvr_list_finish();
    pvr_scene_finish();
}

/* the game runs at 30 frames a second, as on the N64: one game frame per two
 * vertical blanks (counted by an interrupt handler). A late frame waits for
 * the next blank only, rather than a whole extra frame. */
static volatile uint32_t gVblanks;


static void vblank_count(uint32_t code, void *data) {
    gVblanks++;
}

/* ------------------------------------------------------------------ */
/* Sound: the port's mixer (port/audio.c) feeds a KallistiOS stream,  */
/* 16-bit stereo at 22050 Hz. The stream asks for its buffer's worth  */
/* in big chunks every few frames; mixing that much at once made the  */
/* frame it fell in late. So a little is mixed every frame, ahead of  */
/* what the stream will ask for, into a ring the stream then reads.   */
/* ------------------------------------------------------------------ */
#define AUD_FRAMES 4096                  /* most the stream asks for at once */
#define RING 8192                        /* samples (stereo pairs), a power of two */
#define RING_AHEAD 2048                  /* most mixed ahead of the stream (its request size and a bit) */
#define MIX_PER_FRAME 1500               /* at most this much a frame (the rest when asked) */
static snd_stream_hnd_t gStream = SND_STREAM_INVALID;
static int16_t gAudBuf[AUD_FRAMES * 2] __attribute__((aligned(32)));
static int16_t gRing[RING * 2];
static uint32_t gRingW, gRingR;          /* samples written / read, ever */

static void ring_mix(uint32_t n) {
    while (n > 0) {
        uint32_t at = gRingW & (RING - 1);
        uint32_t k = RING - at;          /* up to the end of the ring */
        if (k > n) {
            k = n;
        }
        port_audio_mix(gRing + at * 2, k);
        gRingW += k;
        n -= k;
    }
}

static void *audio_cb(snd_stream_hnd_t hnd, int req, int *done) {
    int frames = req / 4, i;

    if (frames > AUD_FRAMES) {
        frames = AUD_FRAMES;
    }
    frames &= ~7;
    if (gRingW - gRingR < (uint32_t)frames) {
        ring_mix(frames - (gRingW - gRingR));    /* not enough ahead: the rest now */
    }
    for (i = 0; i < frames; i++, gRingR++) {
        uint32_t at = gRingR & (RING - 1);
        gAudBuf[i * 2] = gRing[at * 2];
        gAudBuf[i * 2 + 1] = gRing[at * 2 + 1];
    }
    *done = frames * 4;
    return gAudBuf;
}

/* ---- the voices on the sound chip (port/audio.c, gHwVoices) ----
 * KallistiOS's sound driver on the chip's own processor takes channel
 * commands from a queue in sound memory, each with the time (its clock, in
 * ms) to carry it out. */
#define PORT_HW_VOICES 1                 /* 0: mix in software and stream it, as before */
#define SND_UNCACHED 0xA0800000u         /* sound memory, as the main CPU sees it */
#define SND_CMD_QUEUE 0x010000           /* (the driver's aica_cmd_iface.h) */
#define SND_CLOCK 0x021000
/* The driver's clock: not in ms as its notes say, but 4410 a second in the
 * current driver (its timer counts 10 samples at 44100 Hz). Its rate is
 * measured at start-up, and the engine's times (ms) converted. */
static u32 gSndClockRate = 1000;         /* ticks a second */
static u32 hw_clock_raw(void) {
    return g2_read_32(SND_UNCACHED + SND_CLOCK);
}
static u32 hw_clock(void) {
    return (u32)((uint64_t)hw_clock_raw() * 1000 / gSndClockRate);
}
static u32 hw_ts(u32 ms) {               /* the engine's ms as the driver's clock */
    u32 t = (u32)((uint64_t)ms * gSndClockRate / 1000);
    return t ? t : 1;
}
static void hw_clock_measure(void) {
    uint64_t t0 = timer_us_gettime64(), t1;
    u32 c0 = hw_clock_raw(), c1;
    do {
        thd_pass();
        t1 = timer_us_gettime64();
    } while (t1 - t0 < 250000);
    c1 = hw_clock_raw();
    if (c1 > c0) {
        gSndClockRate = (u32)(((uint64_t)(c1 - c0) * 1000000 + (t1 - t0) / 2) / (t1 - t0));
    }
    if (gSndClockRate < 100) {
        gSndClockRate = 1000;            /* (not running?) */
    }
}
/* room left in the command queue (bytes): commands wait in it until their time */
static u32 hw_room(void) {
    u32 q = SND_UNCACHED + SND_CMD_QUEUE;
    u32 head = g2_read_32(q + 0), tail = g2_read_32(q + 4), size = g2_read_32(q + 8);
    if (size == 0 || size > 0x10000) {
        return 0;
    }
    return (tail > head) ? tail - head - 4 : size - (head - tail) - 4;
}
static void hw_send(void *pkt, u32 words) {
    s32 tries = 0;
    while (hw_room() < words * 4 + 64 && tries++ < 2000) {
        thd_pass();                      /* (only if far too many are waiting) */
    }
    snd_sh4_to_aica(pkt, words);
}
u32 hw_snd_alloc(u32 bytes) {
    return snd_mem_malloc((bytes + 31) & ~31u);
}
void hw_snd_free(u32 addr) {
    snd_mem_free(addr);
}
void hw_snd_write(u32 addr, const s16 *pcm, u32 bytes) {
    spu_memload(addr, (void *)pcm, (bytes + 3) & ~3u);
}
void hw_snd_start(s32 ch, u32 addr, s32 loop, u32 loopStart, u32 end, u32 freq, s32 vol, s32 pan, u32 ms) {
    AICA_CMDSTR_CHANNEL(tmp, cmd, chan);
    memset(tmp, 0, sizeof(tmp));
    cmd->cmd = AICA_CMD_CHAN;
    cmd->timestamp = hw_ts(ms);
    cmd->size = AICA_CMDSTR_CHANNEL_SIZE;
    cmd->cmd_id = ch;
    chan->cmd = AICA_CH_CMD_START;
    chan->base = addr;
    chan->type = AICA_SM_16BIT;
    chan->length = end;
    chan->loop = loop;
    chan->loopstart = loop ? loopStart : 0;
    chan->loopend = end;
    chan->freq = freq;
    chan->vol = vol;
    chan->pan = pan;
    hw_send(tmp, cmd->size);
}
void hw_snd_set(s32 ch, s32 what, u32 freq, s32 vol, s32 pan, u32 ms) {
    AICA_CMDSTR_CHANNEL(tmp, cmd, chan);
    memset(tmp, 0, sizeof(tmp));
    cmd->cmd = AICA_CMD_CHAN;
    cmd->timestamp = hw_ts(ms);
    cmd->size = AICA_CMDSTR_CHANNEL_SIZE;
    cmd->cmd_id = ch;
    chan->cmd = AICA_CH_CMD_UPDATE | ((what & 1) ? AICA_CH_UPDATE_SET_FREQ : 0) |
                ((what & 2) ? AICA_CH_UPDATE_SET_VOL : 0) | ((what & 4) ? AICA_CH_UPDATE_SET_PAN : 0);
    chan->freq = freq;
    chan->vol = vol;
    chan->pan = pan;
    hw_send(tmp, cmd->size);
}
void hw_snd_stop(s32 ch, u32 ms) {
    AICA_CMDSTR_CHANNEL(tmp, cmd, chan);
    memset(tmp, 0, sizeof(tmp));
    cmd->cmd = AICA_CMD_CHAN;
    cmd->timestamp = hw_ts(ms);
    cmd->size = AICA_CMDSTR_CHANNEL_SIZE;
    cmd->cmd_id = ch;
    chan->cmd = AICA_CH_CMD_STOP;
    hw_send(tmp, cmd->size);
}

static void audio_init(void) {
    if (PORT_HW_VOICES) {
        snd_init();                      /* the chip's driver and the sound memory allocator */
        hw_clock_measure();
        gHwVoices = 1;
        return;
    }
    snd_stream_init();
    gStream = snd_stream_alloc(audio_cb, 6144);  /* per channel: 3072 samples (139 ms), asked for 1536 at a time */
    if (gStream == SND_STREAM_INVALID) {
        return;
    }
    snd_stream_volume(gStream, 255);
    snd_stream_start(gStream, 22050, 1);
}

void plat_frame_end(void) {
    static uint32_t last;

    if (gHwVoices) {
        port_audio_hw_run(hw_clock());
    } else if (gStream != SND_STREAM_INVALID) {
        static uint64_t lastUs;
        static uint32_t rem;
        uint64_t a0 = timer_us_gettime64();
        uint32_t ahead = gRingW - gRingR, n;
        /* this frame's share of the mixing: as much sound as time has passed
         * (the stream's requests come in lumps; the ring evens them out) */
        uint32_t el = lastUs ? (uint32_t)(a0 - lastUs) : 0;
        lastUs = a0;
        if (el > 200000) {
            el = 200000;
        }
        rem += el * 441;                 /* 22050 a second = 441 per 20000 us */
        n = rem / 20000;
        rem %= 20000;
        if (n > MIX_PER_FRAME) {
            n = MIX_PER_FRAME;
        }
        if (ahead + n > RING_AHEAD) {
            n = ahead < RING_AHEAD ? RING_AHEAD - ahead : 0;
        }
        ring_mix(n);
        snd_stream_poll(gStream);
    }

    while ((uint32_t)(gVblanks - last) < 2) {
        thd_pass();
    }
    last = gVblanks;
    {   /* (the scenery replay's per-frame counts start again) */
        extern s32 gStRepHit, gStRepInterp;
        gStRepHit = gStRepInterp = 0;
    }
}

/* ------------------------------------------------------------------ */
int main(int argc, char **argv) {
    /* nearly everything goes in the translucent list: bigger bins there, and
     * spare sets of bins for the busy tiles near the horizon (with none, a tile
     * that runs out drops its polygons: blue blocks where the sea shows through) */
    pvr_init_params_t params = {
        { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_0 },
        512 * 1024, 0, 0, 1, 4
    };

    params.autosort_disabled = 1;
    twiddle_init();
    pvr_init(&params);
    vblank_handler_add(vblank_count, NULL);
    audio_init();
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);
    gLists[0].buf = memalign(32, LIST_ITEMS * 32);   /* the opaque list (the translucent one is sent as it goes) */
    port_main(0);
    return 0;
}
