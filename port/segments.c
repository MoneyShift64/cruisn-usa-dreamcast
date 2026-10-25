/*
 * segments.c - the game's global variables: copy each segment's image
 * (tools/gen_data.py) into place and relocate the pointers it holds from
 * N64 addresses to the port's.
 */
#include "port.h"

struct PortSeg {
    const char *name;
    u32 base, size;
    u8 *seg;
    const u8 *image;
    const u32 *relocs;
    const struct { u32 off; void (*fn)(void); } *fnrelocs;
};
extern const struct PortSeg gPortSegs[];

static const struct PortSeg *seg_named(const char *name) {
    const struct PortSeg *s;
    const char *a, *b;

    for (s = gPortSegs; s->name != NULL; s++) {
        for (a = s->name, b = name; *a && *a == *b; a++, b++) {
        }
        if (*a == 0 && *b == 0) {
            return s;
        }
    }
    return NULL;
}

/* N64 address of a global -> the port's (NULL when it isn't in a segment);
 * of the two overlays sharing addresses, the one loaded now */
extern s32 gCurrentOverlay;              /* 0x801466D0 */
void *port_n64_to_ptr(u32 a) {
    const struct PortSeg *s;

    for (s = gPortSegs; s->name != NULL; s++) {
        if (s->name[0] == 'o' && (s->name[3] == '2' || s->name[3] == '3')
            && s->name[3] - '0' != gCurrentOverlay && (gCurrentOverlay == 2 || gCurrentOverlay == 3)) {
            continue;
        }
        if (a >= s->base && a < s->base + s->size) {
            return s->seg + (a - s->base);
        }
    }
    return NULL;
}

/* as port_n64_to_ptr, for a pointer stored in segment 'from': its own segment
 * first, and never the other overlay (ovl2 and ovl3 share N64 addresses) */
static void *seg_target(const struct PortSeg *from, u32 a) {
    const struct PortSeg *s;

    if (a >= from->base && a < from->base + from->size) {
        return from->seg + (a - from->base);
    }
    for (s = gPortSegs; s->name != NULL; s++) {
        if ((from->name[0] == 'o' && from->name[3] != '1') && (s->name[0] == 'o' && s->name[3] != '1')
            && s != from) {
            continue;
        }
        if (a >= s->base && a < s->base + s->size) {
            return s->seg + (a - s->base);
        }
    }
    return NULL;
}

static void seg_load(const struct PortSeg *s) {
    const u32 *r;
    u32 i;

    for (i = 0; i < s->size; i++) {
        s->seg[i] = s->image[i];
    }
    for (r = s->relocs; *r != 0xFFFFFFFF; r++) {
        u32 *p = (u32 *)(s->seg + *r);
        void *to = seg_target(s, *p);
        if (to == NULL) {
            port_log("segment %s: pointer %08x at +%x has no target\n", s->name, *p, *r);
        } else {
            *p = (u32)to;
        }
    }
    for (i = 0; s->fnrelocs[i].off != 0xFFFFFFFF; i++) {
        *(u32 *)(s->seg + s->fnrelocs[i].off) = (u32)s->fnrelocs[i].fn;
    }
}

void port_segments_init(void) {
    seg_load(seg_named("main"));
    seg_load(seg_named("ovl1"));
}

/* overlayCopyIn: the N64 copies an overlay's code and data from the ROM and
 * clears its bss, so its variables start fresh each time it is loaded */
void port_overlay_reset(s32 id) {
    static const char *names[] = { "ovl0", "ovl1", "ovl2", "ovl3" };
    const struct PortSeg *s;

    if (id < 0 || id > 3) {
        return;
    }
    s = seg_named(names[id]);
    if (s != NULL) {
        seg_load(s);
    }
}
