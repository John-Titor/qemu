/*
 * Atari ST-friendly framebuffer.
 *
 * Derived from:
 *
 * QEMU Motorola 680x0 Macintosh Video Card Emulation
 *                 Copyright (c) 2012-2018 Laurent Vivier
 *
 * some parts from QEMU G364 framebuffer Emulator.
 *                 Copyright (c) 2007-2011 Herve Poussineau
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/sysbus.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "hw/display/ataristfb.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "trace.h"

#define VIDEO_BASE 0x0

#define ATARISTFB_VRAM_SIZE (4 * MiB)

#define DAFB_MODE_VADDR1    0x0
#define DAFB_MODE_VADDR2    0x4
#define DAFB_MODE_CTRL1     0x8
#define DAFB_MODE_CTRL2     0xc
#define DAFB_INTR_MASK      0x104
#define DAFB_INTR_STAT      0x108
#define DAFB_INTR_CLEAR     0x10c
#define DAFB_LUT_INDEX      0x200
#define DAFB_LUT            0x210

#define DAFB_INTR_VBL   0x4

/* Vertical Blank period (60.15Hz) */
#define DAFB_INTR_VBL_PERIOD_NS 16625800

static AtariFbMode atarifb_mode_table[] = {
    { 1, 1280, 960, 0xa0 },
    { 2, 1280, 960, 0x140 },
    { 4, 1280, 960, 0x280 },
    { 8, 1280, 960, 0x500 },
};

/* default 2-plane ST palette */
static const uint8_t st_2_palette[256][3] = {
    {0xff, 0xff,  0xff},
    {0xff,    0,     0},
    { 0,   0xff,     0},
    { 0,      0,     0},
};

/* default 4-plane ST palette */
static const uint8_t st_4_palette[256][3] = {
    {0xff, 0xff,  0xff},
    {0xff,    0,     0},
    {   0, 0xff,     0},
    {0xff, 0xff,     0},
    {   0,    0,  0xff},
    {0xff,    0,  0xff},
    {   0, 0xff,  0xff},
    {0x40, 0x40,  0x40},
    {0x7f, 0x7f,  0x7f},
    {0xff, 0x7f,  0x7f},
    {0x7f, 0xff,  0x7f},
    {0xff, 0xff,  0x7f},
    {0x7f, 0x7f,  0xff},
    {0xff, 0x7f,  0xff},
    {0x7f, 0xff,  0xff},
    {   0,    0,     0}
};

/* default 8-plane TT palette */
static const uint8_t st_8_palette[256][3] = {
    { 0xff, 0xff, 0xff }, { 0xff, 0x00, 0x00 }, { 0x00, 0xff, 0x00 }, { 0xff, 0xff, 0x00 },
    { 0x00, 0x00, 0xff }, { 0xff, 0x00, 0xff }, { 0x00, 0xff, 0xff }, { 0xaa, 0xaa, 0xaa },
    { 0x66, 0x66, 0x66 }, { 0xff, 0x99, 0x99 }, { 0x99, 0xff, 0x99 }, { 0xff, 0xff, 0x99 },
    { 0x99, 0x99, 0xff }, { 0xff, 0x99, 0xff }, { 0x99, 0xff, 0xff }, { 0x00, 0x00, 0x00 },
    { 0xff, 0xff, 0xff }, { 0xee, 0xee, 0xee }, { 0xdd, 0xdd, 0xdd }, { 0xcc, 0xcc, 0xcc },
    { 0xbb, 0xbb, 0xbb }, { 0xaa, 0xaa, 0xaa }, { 0x99, 0x99, 0x99 }, { 0x88, 0x88, 0x88 },
    { 0x77, 0x77, 0x77 }, { 0x66, 0x66, 0x66 }, { 0x55, 0x55, 0x55 }, { 0x44, 0x44, 0x44 },
    { 0x33, 0x33, 0x33 }, { 0x22, 0x22, 0x22 }, { 0x11, 0x11, 0x11 }, { 0x00, 0x00, 0x00 },
    { 0xff, 0x00, 0x00 }, { 0xff, 0x00, 0x11 }, { 0xff, 0x00, 0x22 }, { 0xff, 0x00, 0x33 },
    { 0xff, 0x00, 0x44 }, { 0xff, 0x00, 0x55 }, { 0xff, 0x00, 0x66 }, { 0xff, 0x00, 0x77 },
    { 0xff, 0x00, 0x88 }, { 0xff, 0x00, 0x99 }, { 0xff, 0x00, 0xaa }, { 0xff, 0x00, 0xbb },
    { 0xff, 0x00, 0xcc }, { 0xff, 0x00, 0xdd }, { 0xff, 0x00, 0xee }, { 0xff, 0x00, 0xff },
    { 0xee, 0x00, 0xff }, { 0xdd, 0x00, 0xff }, { 0xcc, 0x00, 0xff }, { 0xbb, 0x00, 0xff },
    { 0xaa, 0x00, 0xff }, { 0x99, 0x00, 0xff }, { 0x88, 0x00, 0xff }, { 0x77, 0x00, 0xff },
    { 0x66, 0x00, 0xff }, { 0x55, 0x00, 0xff }, { 0x44, 0x00, 0xff }, { 0x33, 0x00, 0xff },
    { 0x22, 0x00, 0xff }, { 0x11, 0x00, 0xff }, { 0x00, 0x00, 0xff }, { 0x00, 0x11, 0xff },
    { 0x00, 0x22, 0xff }, { 0x00, 0x33, 0xff }, { 0x00, 0x44, 0xff }, { 0x00, 0x55, 0xff },
    { 0x00, 0x66, 0xff }, { 0x00, 0x77, 0xff }, { 0x00, 0x88, 0xff }, { 0x00, 0x99, 0xff },
    { 0x00, 0xaa, 0xff }, { 0x00, 0xbb, 0xff }, { 0x00, 0xcc, 0xff }, { 0x00, 0xdd, 0xff },
    { 0x00, 0xee, 0xff }, { 0x00, 0xff, 0xff }, { 0x00, 0xff, 0xee }, { 0x00, 0xff, 0xdd },
    { 0x00, 0xff, 0xcc }, { 0x00, 0xff, 0xbb }, { 0x00, 0xff, 0xaa }, { 0x00, 0xff, 0x99 },
    { 0x00, 0xff, 0x88 }, { 0x00, 0xff, 0x77 }, { 0x00, 0xff, 0x66 }, { 0x00, 0xff, 0x55 },
    { 0x00, 0xff, 0x44 }, { 0x00, 0xff, 0x33 }, { 0x00, 0xff, 0x22 }, { 0x00, 0xff, 0x11 },
    { 0x00, 0xff, 0x00 }, { 0x11, 0xff, 0x00 }, { 0x22, 0xff, 0x00 }, { 0x33, 0xff, 0x00 },
    { 0x44, 0xff, 0x00 }, { 0x55, 0xff, 0x00 }, { 0x66, 0xff, 0x00 }, { 0x77, 0xff, 0x00 },
    { 0x88, 0xff, 0x00 }, { 0x99, 0xff, 0x00 }, { 0xaa, 0xff, 0x00 }, { 0xbb, 0xff, 0x00 },
    { 0xcc, 0xff, 0x00 }, { 0xdd, 0xff, 0x00 }, { 0xee, 0xff, 0x00 }, { 0xff, 0xff, 0x00 },
    { 0xff, 0xee, 0x00 }, { 0xff, 0xdd, 0x00 }, { 0xff, 0xcc, 0x00 }, { 0xff, 0xbb, 0x00 },
    { 0xff, 0xaa, 0x00 }, { 0xff, 0x99, 0x00 }, { 0xff, 0x88, 0x00 }, { 0xff, 0x77, 0x00 },
    { 0xff, 0x66, 0x00 }, { 0xff, 0x55, 0x00 }, { 0xff, 0x44, 0x00 }, { 0xff, 0x33, 0x00 },
    { 0xff, 0x22, 0x00 }, { 0xff, 0x11, 0x00 }, { 0xbb, 0x00, 0x00 }, { 0xbb, 0x00, 0x11 },
    { 0xbb, 0x00, 0x22 }, { 0xbb, 0x00, 0x33 }, { 0xbb, 0x00, 0x44 }, { 0xbb, 0x00, 0x55 },
    { 0xbb, 0x00, 0x66 }, { 0xbb, 0x00, 0x77 }, { 0xbb, 0x00, 0x88 }, { 0xbb, 0x00, 0x99 },
    { 0xbb, 0x00, 0xaa }, { 0xbb, 0x00, 0xbb }, { 0xaa, 0x00, 0xbb }, { 0x99, 0x00, 0xbb },
    { 0x88, 0x00, 0xbb }, { 0x77, 0x00, 0xbb }, { 0x66, 0x00, 0xbb }, { 0x55, 0x00, 0xbb },
    { 0x44, 0x00, 0xbb }, { 0x33, 0x00, 0xbb }, { 0x22, 0x00, 0xbb }, { 0x11, 0x00, 0xbb },
    { 0x00, 0x00, 0xbb }, { 0x00, 0x11, 0xbb }, { 0x00, 0x22, 0xbb }, { 0x00, 0x33, 0xbb },
    { 0x00, 0x44, 0xbb }, { 0x00, 0x55, 0xbb }, { 0x00, 0x66, 0xbb }, { 0x00, 0x77, 0xbb },
    { 0x00, 0x88, 0xbb }, { 0x00, 0x99, 0xbb }, { 0x00, 0xaa, 0xbb }, { 0x00, 0xbb, 0xbb },
    { 0x00, 0xbb, 0xaa }, { 0x00, 0xbb, 0x99 }, { 0x00, 0xbb, 0x88 }, { 0x00, 0xbb, 0x77 },
    { 0x00, 0xbb, 0x66 }, { 0x00, 0xbb, 0x55 }, { 0x00, 0xbb, 0x44 }, { 0x00, 0xbb, 0x33 },
    { 0x00, 0xbb, 0x22 }, { 0x00, 0xbb, 0x11 }, { 0x00, 0xbb, 0x00 }, { 0x11, 0xbb, 0x00 },
    { 0x22, 0xbb, 0x00 }, { 0x33, 0xbb, 0x00 }, { 0x44, 0xbb, 0x00 }, { 0x55, 0xbb, 0x00 },
    { 0x66, 0xbb, 0x00 }, { 0x77, 0xbb, 0x00 }, { 0x88, 0xbb, 0x00 }, { 0x99, 0xbb, 0x00 },
    { 0xaa, 0xbb, 0x00 }, { 0xbb, 0xbb, 0x00 }, { 0xbb, 0xaa, 0x00 }, { 0xbb, 0x99, 0x00 },
    { 0xbb, 0x88, 0x00 }, { 0xbb, 0x77, 0x00 }, { 0xbb, 0x66, 0x00 }, { 0xbb, 0x55, 0x00 },
    { 0xbb, 0x44, 0x00 }, { 0xbb, 0x33, 0x00 }, { 0xbb, 0x22, 0x00 }, { 0xbb, 0x11, 0x00 },
    { 0x77, 0x00, 0x00 }, { 0x77, 0x00, 0x11 }, { 0x77, 0x00, 0x22 }, { 0x77, 0x00, 0x33 },
    { 0x77, 0x00, 0x44 }, { 0x77, 0x00, 0x55 }, { 0x77, 0x00, 0x66 }, { 0x77, 0x00, 0x77 },
    { 0x66, 0x00, 0x77 }, { 0x55, 0x00, 0x77 }, { 0x44, 0x00, 0x77 }, { 0x33, 0x00, 0x77 },
    { 0x22, 0x00, 0x77 }, { 0x11, 0x00, 0x77 }, { 0x00, 0x00, 0x77 }, { 0x00, 0x11, 0x77 },
    { 0x00, 0x22, 0x77 }, { 0x00, 0x33, 0x77 }, { 0x00, 0x44, 0x77 }, { 0x00, 0x55, 0x77 },
    { 0x00, 0x66, 0x77 }, { 0x00, 0x77, 0x77 }, { 0x00, 0x77, 0x66 }, { 0x00, 0x77, 0x55 },
    { 0x00, 0x77, 0x44 }, { 0x00, 0x77, 0x33 }, { 0x00, 0x77, 0x22 }, { 0x00, 0x77, 0x11 },
    { 0x00, 0x77, 0x00 }, { 0x11, 0x77, 0x00 }, { 0x22, 0x77, 0x00 }, { 0x33, 0x77, 0x00 },
    { 0x44, 0x77, 0x00 }, { 0x55, 0x77, 0x00 }, { 0x66, 0x77, 0x00 }, { 0x77, 0x77, 0x00 },
    { 0x77, 0x66, 0x00 }, { 0x77, 0x55, 0x00 }, { 0x77, 0x44, 0x00 }, { 0x77, 0x33, 0x00 },
    { 0x77, 0x22, 0x00 }, { 0x77, 0x11, 0x00 }, { 0x44, 0x00, 0x00 }, { 0x44, 0x00, 0x11 },
    { 0x44, 0x00, 0x22 }, { 0x44, 0x00, 0x33 }, { 0x44, 0x00, 0x44 }, { 0x33, 0x00, 0x44 },
    { 0x22, 0x00, 0x44 }, { 0x11, 0x00, 0x44 }, { 0x00, 0x00, 0x44 }, { 0x00, 0x11, 0x44 },
    { 0x00, 0x22, 0x44 }, { 0x00, 0x33, 0x44 }, { 0x00, 0x44, 0x44 }, { 0x00, 0x44, 0x33 },
    { 0x00, 0x44, 0x22 }, { 0x00, 0x44, 0x11 }, { 0x00, 0x44, 0x00 }, { 0x11, 0x44, 0x00 },
    { 0x22, 0x44, 0x00 }, { 0x33, 0x44, 0x00 }, { 0x44, 0x44, 0x00 }, { 0x44, 0x33, 0x00 },
    { 0x44, 0x22, 0x00 }, { 0x44, 0x11, 0x00 }, { 0xff, 0xff, 0xff }, { 0x00, 0x00, 0x00 },
};

typedef void atarifb_draw_line_func(AtarifbState *s, uint8_t *d, uint32_t addr, int width);

/* mono */
static void atarifb_draw_line1(AtarifbState *s, uint8_t *d, uint32_t addr, int width)
{
    for (int x = 0; x < width; x += 16) {
        const uint16_t *p = (const uint16_t *)&s->vram[addr & s->vram_bit_mask];
        for (int i = 8; i < 24; i++) {
            int mask = 0x8000 >> (i % 16);

            if (*p & mask) {
                *(uint32_t *)d = rgb_to_pixel32(0, 0, 0);
            } else {
                *(uint32_t *)d = rgb_to_pixel32(0xff, 0xff, 0xff);
            }
            d += 4;
        }
        addr += 2;
    }
}

/* 2-bit color */
static void atarifb_draw_line2(AtarifbState *s, uint8_t *d, uint32_t addr, int width)
{
    for (int x = 0; x < width; x += 16) {
        const uint16_t *p = (const uint16_t *)&s->vram[addr & s->vram_bit_mask];
        for (int i = 8; i < 24; i++) {
            int mask = 0x8000 >> (i % 16);
            int idx = (((p[1] & mask) ? 2 : 0) |
                       ((p[0] & mask) ? 1 : 0));

            *(uint32_t *)d = rgb_to_pixel32(s->color_palette[idx * 3 + 0] << 2,
                                            s->color_palette[idx * 3 + 1] << 2,
                                            s->color_palette[idx * 3 + 2] << 2);
            d += 4;
        }
        addr += 4;
    }
}

/* 4-bit color */
static void atarifb_draw_line4(AtarifbState *s, uint8_t *d, uint32_t addr, int width)
{
    for (int x = 0; x < width; x += 16) {
        const uint16_t *p = (const uint16_t *)&s->vram[addr & s->vram_bit_mask];
        for (int i = 8; i < 24; i++) {
            int mask = 0x8000 >> (i % 16);
            int idx = (((p[3] & mask) ? 8 : 0) |
                       ((p[2] & mask) ? 4 : 0) |
                       ((p[1] & mask) ? 2 : 0) |
                       ((p[0] & mask) ? 1 : 0));

            *(uint32_t *)d = rgb_to_pixel32(s->color_palette[idx * 3 + 0] << 2,
                                            s->color_palette[idx * 3 + 1] << 2,
                                            s->color_palette[idx * 3 + 2] << 2);
            d += 4;
        }
        addr += 8;
    }
}

/* 8-bit color */
static void atarifb_draw_line8(AtarifbState *s, uint8_t *d, uint32_t addr, int width)
{
    for (int x = 0; x < width; x += 16) {
        const uint16_t *p = (const uint16_t *)&s->vram[addr & s->vram_bit_mask];
        for (int i = 8; i < 24; i++) {
            int mask = 0x8000 >> (i % 16);
            int idx = (((p[7] & mask) ? 0x80 : 0) |
                       ((p[6] & mask) ? 0x40 : 0) |
                       ((p[5] & mask) ? 0x20 : 0) |
                       ((p[4] & mask) ? 0x10 : 0) |
                       ((p[3] & mask) ? 0x08 : 0) |
                       ((p[2] & mask) ? 0x04 : 0) |
                       ((p[1] & mask) ? 0x02 : 0) |
                       ((p[0] & mask) ? 0x01 : 0));

            *(uint32_t *)d = rgb_to_pixel32(s->color_palette[idx * 3 + 0] << 2,
                                            s->color_palette[idx * 3 + 1] << 2,
                                            s->color_palette[idx * 3 + 2] << 2);
            d += 4;
        }
        addr += 16;
    }
}

static int atarifb_check_dirty(AtarifbState *s,
                               DirtyBitmapSnapshot *snap,
                               ram_addr_t addr,
                               int len)
{
    return memory_region_snapshot_get_dirty(&s->mem_vram, snap, addr, len);
}

static void atarifb_draw_graphic(AtarifbState *s)
{
    DisplaySurface *surface = qemu_console_surface(s->con);
    DirtyBitmapSnapshot *snap = memory_region_snapshot_and_clear_dirty(
                                    &s->mem_vram,
                                    0x0,
                                    memory_region_size(&s->mem_vram),
                                    DIRTY_MEMORY_VGA);
    int atarifb_stride = s->mode->stride;
    ram_addr_t page = 0;
    int ymin = -1;

    atarifb_draw_line_func *atarifb_draw_line = NULL;

    switch (s->depth) {
    case 1:
        atarifb_draw_line = atarifb_draw_line1;
        break;
    case 2:
        atarifb_draw_line = atarifb_draw_line2;
        break;
    case 4:
        atarifb_draw_line = atarifb_draw_line4;
        break;
    case 8:
        atarifb_draw_line = atarifb_draw_line8;
        break;
    }

    assert(atarifb_draw_line != NULL);

    for (int y = 0; y < s->height; y++, page += atarifb_stride) {
        if (atarifb_check_dirty(s, snap, page, atarifb_stride)) {
            uint8_t *data_display;

            data_display = surface_data(surface) + y * surface_stride(surface);
            atarifb_draw_line(s, data_display, page, s->width);

            if (ymin < 0) {
                ymin = y;
            }
        } else {
            if (ymin >= 0) {
                dpy_gfx_update(s->con, 0, ymin, s->width, y - ymin);
                ymin = -1;
            }
        }
    }

    if (ymin >= 0) {
        dpy_gfx_update(s->con, 0, ymin, s->width, s->height - ymin);
    }

    g_free(snap);
}

static void atarifb_invalidate_display(void *opaque)
{
    AtarifbState *s = opaque;

    memory_region_set_dirty(&s->mem_vram, 0, ATARISTFB_VRAM_SIZE);
}

static void atarifb_update_mode(AtarifbState *s)
{
    s->width = s->mode->width;
    s->height = s->mode->height;
    s->depth = s->mode->depth;

//    trace_atarifb_update_mode(s->width, s->height, s->depth);
    atarifb_invalidate_display(s);
}

static AtariFbMode *atarifb_find_mode(uint16_t width, uint16_t height, uint8_t depth)
{
    AtariFbMode *atarifb_mode;

    for (int i = 0; i < ARRAY_SIZE(atarifb_mode_table); i++) {
        atarifb_mode = &atarifb_mode_table[i];

        if (width == atarifb_mode->width &&
            height == atarifb_mode->height &&
            depth == atarifb_mode->depth) {
            return atarifb_mode;
        }
    }

    return NULL;
}

static gchar *atarifb_mode_list(void)
{
    GString *list = g_string_new("");
    AtariFbMode *atarifb_mode;

    for (int i = 0; i < ARRAY_SIZE(atarifb_mode_table); i++) {
        atarifb_mode = &atarifb_mode_table[i];

        g_string_append_printf(list, "    %dx%dx%d\n", atarifb_mode->width,
                               atarifb_mode->height, atarifb_mode->depth);
    }

    return g_string_free(list, FALSE);
}


static void atarifb_update_display(void *opaque)
{
    AtarifbState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);

    qemu_flush_coalesced_mmio_buffer();

    if (s->width == 0 || s->height == 0) {
        return;
    }

    if (s->width != surface_width(surface) ||
        s->height != surface_height(surface)) {
        qemu_console_resize(s->con, s->width, s->height);
    }

    atarifb_draw_graphic(s);
}

static void atarifb_update_irq(AtarifbState *s)
{
    uint32_t irq_state = s->regs[DAFB_INTR_STAT >> 2] &
                         s->regs[DAFB_INTR_MASK >> 2];

    if (irq_state) {
        qemu_irq_raise(s->irq);
    } else {
        qemu_irq_lower(s->irq);
    }
}

static int64_t atarifb_next_vbl(void)
{
    return (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + DAFB_INTR_VBL_PERIOD_NS) /
            DAFB_INTR_VBL_PERIOD_NS * DAFB_INTR_VBL_PERIOD_NS;
}

static void atarifb_vbl_timer(void *opaque)
{
    AtarifbState *s = opaque;
    int64_t next_vbl;

    s->regs[DAFB_INTR_STAT >> 2] |= DAFB_INTR_VBL;
    atarifb_update_irq(s);

    /* 60 Hz irq */
    next_vbl = atarifb_next_vbl();
    timer_mod(s->vbl_timer, next_vbl);
}

static void atarifb_reset(AtarifbState *s)
{
    s->palette_current = 0;
    switch (s->mode->depth) {
    case 2:
        memcpy(s->color_palette, st_2_palette, sizeof(s->color_palette));
        break;
    case 4:
        memcpy(s->color_palette, st_4_palette, sizeof(s->color_palette));
        break;
    case 8:
        memcpy(s->color_palette, st_8_palette, sizeof(s->color_palette));
        break;
    default:
        memset(s->color_palette, 0, sizeof(s->color_palette));
    }
    memset(s->vram, 0, ATARISTFB_VRAM_SIZE);
    atarifb_invalidate_display(s);
}

static uint64_t atarifb_ctrl_read(void *opaque,
                                hwaddr addr,
                                unsigned int size)
{
    AtarifbState *s = opaque;
    uint64_t val = 0;

    switch (addr) {
    case DAFB_MODE_VADDR1:
    case DAFB_MODE_VADDR2:
    case DAFB_MODE_CTRL1:
    case DAFB_MODE_CTRL2:
    case DAFB_INTR_STAT:
        val = s->regs[addr >> 2];
        break;
    case DAFB_LUT ... DAFB_LUT + 3:
        val = s->color_palette[s->palette_current];
        s->palette_current = (s->palette_current + 1) %
                             ARRAY_SIZE(s->color_palette);
        break;
    default:
        if (addr < ATARISTFB_CTRL_TOPADDR) {
            val = s->regs[addr >> 2];
        }
    }

//    trace_atarifb_ctrl_read(addr, val, size);
    return val;
}

static void atarifb_ctrl_write(void *opaque,
                             hwaddr addr,
                             uint64_t val,
                             unsigned int size)
{
    AtarifbState *s = opaque;
    int64_t next_vbl;

    switch (addr) {
    case DAFB_MODE_VADDR1:
    case DAFB_MODE_VADDR2:
    case DAFB_MODE_CTRL1 ... DAFB_MODE_CTRL1 + 3:
    case DAFB_MODE_CTRL2 ... DAFB_MODE_CTRL2 + 3:
        s->regs[addr >> 2] = val;
        break;
    case DAFB_INTR_MASK:
        s->regs[addr >> 2] = val;
        if (val & DAFB_INTR_VBL) {
            next_vbl = atarifb_next_vbl();
            timer_mod(s->vbl_timer, next_vbl);
        } else {
            timer_del(s->vbl_timer);
        }
        break;
    case DAFB_INTR_CLEAR:
        s->regs[DAFB_INTR_STAT >> 2] &= ~DAFB_INTR_VBL;
        atarifb_update_irq(s);
        break;
    case DAFB_LUT_INDEX:
        s->palette_current = (val & 0xff) * 3;
        break;
    case DAFB_LUT ... DAFB_LUT + 3:
        s->color_palette[s->palette_current] = val & 0xff;
        s->palette_current = (s->palette_current + 1) %
                             ARRAY_SIZE(s->color_palette);
        if (s->palette_current % 3) {
            atarifb_invalidate_display(s);
        }
        break;
    default:
        if (addr < ATARISTFB_CTRL_TOPADDR) {
            s->regs[addr >> 2] = val;
        }
    }

//    trace_atarifb_ctrl_write(addr, val, size);
}

static const MemoryRegionOps atarifb_ctrl_ops = {
    .read = atarifb_ctrl_read,
    .write = atarifb_ctrl_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static int atarifb_post_load(void *opaque, int version_id)
{
    return 0;
}

static const VMStateDescription vmstate_atarifb = {
    .name = "atarifb",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = atarifb_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT8_ARRAY(color_palette, AtarifbState, 256 * 3),
        VMSTATE_UINT32(palette_current, AtarifbState),
        VMSTATE_UINT32_ARRAY(regs, AtarifbState, ATARISTFB_NUM_REGS),
        VMSTATE_TIMER_PTR(vbl_timer, AtarifbState),
        VMSTATE_END_OF_LIST()
    }
};

static const GraphicHwOps atarifb_ops = {
    .invalidate = atarifb_invalidate_display,
    .gfx_update = atarifb_update_display,
};

static bool atarifb_common_realize(DeviceState *dev, AtarifbState *s, Error **errp)
{
    DisplaySurface *surface;

    s->mode = atarifb_find_mode(s->width, s->height, s->depth);
    if (!s->mode) {
        gchar *list;
        error_setg(errp, "unknown display mode: width %d, height %d, depth %d",
                   s->width, s->height, s->depth);
        list = atarifb_mode_list();
        error_append_hint(errp, "Available modes:\n%s", list);
        g_free(list);

        return false;
    }

    s->con = graphic_console_init(dev, 0, &atarifb_ops, s);
    surface = qemu_console_surface(s->con);

    if (surface_bits_per_pixel(surface) != 32) {
        error_setg(errp, "unknown host depth %d",
                   surface_bits_per_pixel(surface));
        return false;
    }

    memory_region_init_io(&s->mem_ctrl, OBJECT(dev), &atarifb_ctrl_ops, s,
                          "atarifb-ctrl", 0x1000);

    memory_region_init_ram(&s->mem_vram, OBJECT(dev), "atarifb-vram",
                           ATARISTFB_VRAM_SIZE, &error_abort);
    memory_region_set_log(&s->mem_vram, true, DIRTY_MEMORY_VGA);
    s->vram = memory_region_get_ram_ptr(&s->mem_vram);
    s->vram_bit_mask = ATARISTFB_VRAM_SIZE - 1;

    s->vbl_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, atarifb_vbl_timer, s);
    atarifb_update_mode(s);
    return true;
}

static void atarifb_sysbus_realize(DeviceState *dev, Error **errp)
{
    AtarifbSysBusState *s = ATARISTFB(dev);
    AtarifbState *ms = &s->atarifb;

    if (!atarifb_common_realize(dev, ms, errp)) {
        return;
    }

    sysbus_init_mmio(SYS_BUS_DEVICE(s), &ms->mem_ctrl);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &ms->mem_vram);

    sysbus_init_irq(SYS_BUS_DEVICE(s), &ms->irq);
}

static void atarifb_sysbus_reset(DeviceState *d)
{
    AtarifbSysBusState *s = ATARISTFB(d);
    atarifb_reset(&s->atarifb);
}

static Property atarifb_sysbus_properties[] = {
    DEFINE_PROP_UINT32("width", AtarifbSysBusState, atarifb.width, 1280),
    DEFINE_PROP_UINT32("height", AtarifbSysBusState, atarifb.height, 960),
    DEFINE_PROP_UINT8("depth", AtarifbSysBusState, atarifb.depth, 4),   /* 8-plane mode crashes EmuTOS */
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription vmstate_atarifb_sysbus = {
    .name = "atarifb-sysbus",
    .version_id = 1,
    .minimum_version_id = 1,
    .unmigratable = 1,
    .fields = (VMStateField[]) {
        VMSTATE_STRUCT(atarifb, AtarifbSysBusState, 1, vmstate_atarifb, AtarifbState),
        VMSTATE_END_OF_LIST()
    }
};

static void atarifb_sysbus_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = atarifb_sysbus_realize;
    dc->desc = "AtariST framebuffer";
    dc->reset = atarifb_sysbus_reset;
    dc->vmsd = &vmstate_atarifb_sysbus;
    device_class_set_props(dc, atarifb_sysbus_properties);
}

static const TypeInfo atarifb_sysbus_info = {
    .name          = TYPE_ATARISTFB,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AtarifbSysBusState),
    .class_init    = atarifb_sysbus_class_init,
};

static void atarifb_register_types(void)
{
    type_register_static(&atarifb_sysbus_info);
}

type_init(atarifb_register_types)
