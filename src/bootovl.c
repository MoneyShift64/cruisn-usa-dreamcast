/*
 * bootovl.c - parts of the boot overlay (overlay 0, ROM 0x3D860-0x3E6F0, RAM
 * 0x80025C80), which runs once at power-on. The port does most of its set-up
 * in port/port_main.c (boot_game); this is the part with data of its own:
 * the default high-score tables.
 */
#include "common.h"
#include "frontend.h"

typedef struct HiscoreDefault {          /* 8 bytes, table 0x80133DC8 */
    /* 0x00 */ char *initials;           /* "SER", "GGN", ... */
    /* 0x04 */ f32 time;                 /* 105 s, 106 s, ... */
} HiscoreDefault;

extern HiscoreDefault gHiscoreDefaults[10]; /* 0x80133DC8 */
extern HiscoreEntry gHiscores[][15][10]; /* 0x801625C0 - per save device */
extern s32 gTrackRecords[][14][10];      /* 0x80150A00 - per save device */

/* from 0x80026574 (in 0x80025C80): both devices' tables. Each race starts from the
 * same ten (a hundredth of a second slower per race number); Cruise the USA
 * from fourteen times them. No track records yet (-1). */
void hiscoresDefault(void) {
    HiscoreDefault *d;
    HiscoreEntry *e;
    s32 dev, t, i, k;
    f64 add;

    for (dev = 0; dev < 2; dev++) {
        for (t = 0; t < 15; t++) {
            add = (f64)(f32)t / 100.0;
            e = gHiscores[dev][t];
            for (i = 0, d = gHiscoreDefaults; i < 10; i++, d++, e++) {
                for (k = 0; k < 3; k++) {
                    e->initials[k] = d->initials[k];
                }
                e->time = (f32)((f64)d->time + add);
            }
        }
        for (i = 0; i < 10; i++) {
            gHiscores[dev][14][i].time = (f32)((f64)gHiscoreDefaults[i].time * 14.0 + 0.14);
        }
        for (t = 0; t < 14; t++) {
            for (i = 0; i < 10; i++) {
                gTrackRecords[dev][t][i] = -1;
            }
        }
    }
}
