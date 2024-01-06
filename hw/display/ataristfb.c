/*
 * Atari ST-friendly framebuffer.
 *
 * Originally derived from:
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


/* Vertical Blank period (60.15Hz) */
#define VBL_PERIOD_NS 16625800

/* palette values are pre-processed outputs from rgb_to_pixel32(), i.e. x8r8g8b8 */

/* mono palette */
static const uint32_t st_1_palette[256] = {
    0x00000000,
    0x00ffffff,
};

/* default 2-plane ST palette */
static const uint32_t st_2_palette[256] = {
    0x00ffffff,
    0x00ff0000,
    0x0000ff00,
    0x00000000,
};

/* default 4-plane ST palette */
static const uint32_t st_4_palette[256] = {
    0x00ffffff,
    0x00ff0000,
    0x0000ff00,
    0x00ffff00,
    0x000000ff,
    0x00ff00ff,
    0x0000ffff,
    0x00404040,
    0x007f7f7f,
    0x00ff7f7f,
    0x007fff7f,
    0x00ffff7f,
    0x007f7fff,
    0x00ff7fff,
    0x007fffff,
    0x00000000
};

/* default 8-plane TT palette */
static const uint32_t tt_8_palette[256] = {
    0x00ffffff, 0x00ff0000, 0x0000ff00, 0x00ffff00,
    0x000000ff, 0x00ff00ff, 0x0000ffff, 0x00aaaaaa,
    0x00666666, 0x00ff9999, 0x0099ff99, 0x00ffff99,
    0x009999ff, 0x00ff99ff, 0x0099ffff, 0x00000000,
    0x00ffffff, 0x00eeeeee, 0x00dddddd, 0x00cccccc,
    0x00bbbbbb, 0x00aaaaaa, 0x00999999, 0x00888888,
    0x00777777, 0x00666666, 0x00555555, 0x00444444,
    0x00333333, 0x00222222, 0x00111111, 0x00000000,
    0x00ff0000, 0x00ff0011, 0x00ff0022, 0x00ff0033,
    0x00ff0044, 0x00ff0055, 0x00ff0066, 0x00ff0077,
    0x00ff0088, 0x00ff0099, 0x00ff00aa, 0x00ff00bb,
    0x00ff00cc, 0x00ff00dd, 0x00ff00ee, 0x00ff00ff,
    0x00ee00ff, 0x00dd00ff, 0x00cc00ff, 0x00bb00ff,
    0x00aa00ff, 0x009900ff, 0x008800ff, 0x007700ff,
    0x006600ff, 0x005500ff, 0x004400ff, 0x003300ff,
    0x002200ff, 0x001100ff, 0x000000ff, 0x000011ff,
    0x000022ff, 0x000033ff, 0x000044ff, 0x000055ff,
    0x000066ff, 0x000077ff, 0x000088ff, 0x000099ff,
    0x0000aaff, 0x0000bbff, 0x0000ccff, 0x0000ddff,
    0x0000eeff, 0x0000ffff, 0x0000ffee, 0x0000ffdd,
    0x0000ffcc, 0x0000ffbb, 0x0000ffaa, 0x0000ff99,
    0x0000ff88, 0x0000ff77, 0x0000ff66, 0x0000ff55,
    0x0000ff44, 0x0000ff33, 0x0000ff22, 0x0000ff11,
    0x0000ff00, 0x0011ff00, 0x0022ff00, 0x0033ff00,
    0x0044ff00, 0x0055ff00, 0x0066ff00, 0x0077ff00,
    0x0088ff00, 0x0099ff00, 0x00aaff00, 0x00bbff00,
    0x00ccff00, 0x00ddff00, 0x00eeff00, 0x00ffff00,
    0x00ffee00, 0x00ffdd00, 0x00ffcc00, 0x00ffbb00,
    0x00ffaa00, 0x00ff9900, 0x00ff8800, 0x00ff7700,
    0x00ff6600, 0x00ff5500, 0x00ff4400, 0x00ff3300,
    0x00ff2200, 0x00ff1100, 0x00bb0000, 0x00bb0011,
    0x00bb0022, 0x00bb0033, 0x00bb0044, 0x00bb0055,
    0x00bb0066, 0x00bb0077, 0x00bb0088, 0x00bb0099,
    0x00bb00aa, 0x00bb00bb, 0x00aa00bb, 0x009900bb,
    0x008800bb, 0x007700bb, 0x006600bb, 0x005500bb,
    0x004400bb, 0x003300bb, 0x002200bb, 0x001100bb,
    0x000000bb, 0x000011bb, 0x000022bb, 0x000033bb,
    0x000044bb, 0x000055bb, 0x000066bb, 0x000077bb,
    0x000088bb, 0x000099bb, 0x0000aabb, 0x0000bbbb,
    0x0000bbaa, 0x0000bb99, 0x0000bb88, 0x0000bb77,
    0x0000bb66, 0x0000bb55, 0x0000bb44, 0x0000bb33,
    0x0000bb22, 0x0000bb11, 0x0000bb00, 0x0011bb00,
    0x0022bb00, 0x0033bb00, 0x0044bb00, 0x0055bb00,
    0x0066bb00, 0x0077bb00, 0x0088bb00, 0x0099bb00,
    0x00aabb00, 0x00bbbb00, 0x00bbaa00, 0x00bb9900,
    0x00bb8800, 0x00bb7700, 0x00bb6600, 0x00bb5500,
    0x00bb4400, 0x00bb3300, 0x00bb2200, 0x00bb1100,
    0x00770000, 0x00770011, 0x00770022, 0x00770033,
    0x00770044, 0x00770055, 0x00770066, 0x00770077,
    0x00660077, 0x00550077, 0x00440077, 0x00330077,
    0x00220077, 0x00110077, 0x00000077, 0x00001177,
    0x00002277, 0x00003377, 0x00004477, 0x00005577,
    0x00006677, 0x00007777, 0x00007766, 0x00007755,
    0x00007744, 0x00007733, 0x00007722, 0x00007711,
    0x00007700, 0x00117700, 0x00227700, 0x00337700,
    0x00447700, 0x00557700, 0x00667700, 0x00777700,
    0x00776600, 0x00775500, 0x00774400, 0x00773300,
    0x00772200, 0x00771100, 0x00440000, 0x00440011,
    0x00440022, 0x00440033, 0x00440044, 0x00330044,
    0x00220044, 0x00110044, 0x00000044, 0x00001144,
    0x00002244, 0x00003344, 0x00004444, 0x00004433,
    0x00004422, 0x00004411, 0x00004400, 0x00114400,
    0x00224400, 0x00334400, 0x00444400, 0x00443300,
    0x00442200, 0x00441100, 0x00ffffff, 0x00000000,
};

/* mono */
static void fb_draw_line1(AtariSTFBState *s, uint32_t *d, uint32_t addr, int width)
{
    for (int x = 0; x < width; x += 16) {
        const uint16_t *p = (const uint16_t *)&s->vram[addr/* & s->vram_bit_mask */];
        for (int i = 8; i < 24; i++) {
            int mask = 0x8000 >> (i % 16);

            *d++ = (*p & mask) ? 0 : 0x00ffffff;
        }
        addr += 2;
    }
}

/* 2-bit color */
static void fb_draw_line2(AtariSTFBState *s, uint32_t *d, uint32_t addr, int width)
{
    for (int x = 0; x < width; x += 16) {
        const uint16_t *p = (const uint16_t *)&s->vram[addr/* & s->vram_bit_mask */];
        for (int i = 8; i < 24; i++) {
            int mask = 0x8000 >> (i % 16);
            int idx = (((p[1] & mask) ? 2 : 0) |
                       ((p[0] & mask) ? 1 : 0));

            *d++ = s->palette[idx];
        }
        addr += 4;
    }
}

/* 4-bit color */
static void fb_draw_line4(AtariSTFBState *s, uint32_t *d, uint32_t addr, int width)
{
    for (int x = 0; x < width; x += 16) {
        const uint16_t *p = (const uint16_t *)&s->vram[addr/* & s->vram_bit_mask */];
        for (int i = 8; i < 24; i++) {
            int mask = 0x8000 >> (i % 16);
            int idx = (((p[3] & mask) ? 8 : 0) |
                       ((p[2] & mask) ? 4 : 0) |
                       ((p[1] & mask) ? 2 : 0) |
                       ((p[0] & mask) ? 1 : 0));

            *d++ = s->palette[idx];
        }
        addr += 8;
    }
}

/* 8-bit color */
static void fb_draw_line8(AtariSTFBState *s, uint32_t *d, uint32_t addr, int width)
{
    for (int x = 0; x < width; x += 16) {
        const uint16_t *p = (const uint16_t *)&s->vram[addr/* & s->vram_bit_mask */];
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

            *d++ = s->palette[idx];
        }
        addr += 16;
    }
}

static const AtariSTFBMode fb_mode_table[] = {
    { 1, 1280, 960,  0xa0,  st_1_palette, fb_draw_line1 },
    { 2, 1280, 960, 0x140,  st_2_palette, fb_draw_line2 },
    { 4, 1280, 960, 0x280,  st_4_palette, fb_draw_line4 },
    { 8, 1280, 960, 0x500,  tt_8_palette, fb_draw_line8 },
};

static int fb_check_dirty(AtariSTFBState *s,
                               DirtyBitmapSnapshot *snap,
                               ram_addr_t addr,
                               int len)
{
    return memory_region_snapshot_get_dirty(&s->mem_vram, snap, addr, len);
}

static void fb_draw_graphic(AtariSTFBState *s)
{
    DisplaySurface *surface = qemu_console_surface(s->con);
    DirtyBitmapSnapshot *snap = memory_region_snapshot_and_clear_dirty(
                                    &s->mem_vram,
                                    0x0,
                                    memory_region_size(&s->mem_vram),
                                    DIRTY_MEMORY_VGA);
    const AtariSTFBMode *mode = &fb_mode_table[s->mode];
    uint32_t dest_stride = surface_stride(surface);
    ram_addr_t page = 0;
    int ymin = -1;

    for (int y = 0; y < mode->height; y++, page += mode->stride) {
        if (fb_check_dirty(s, snap, page, mode->stride)) {

            uint32_t *data_display = surface_data(surface) + y * dest_stride;
            mode->draw_fn(s, data_display, page, mode->width);

            if (ymin < 0) {
                ymin = y;
            }
        } else {
            if (ymin >= 0) {
                dpy_gfx_update(s->con, 0, ymin, mode->width, y - ymin);
                ymin = -1;
            }
        }
    }

    if (ymin >= 0) {
        dpy_gfx_update(s->con, 0, ymin, mode->width, mode->height - ymin);
    }

    g_free(snap);
}

static void fb_invalidate_display(void *opaque)
{
    AtariSTFBState *s = ATARISTFB(opaque);

    memory_region_set_dirty(&s->mem_vram, 0, ATARISTFB_VRAM_SIZE);
}

static void fb_update_mode(AtariSTFBState *s)
{
    s->regs[REG_MODE] = s->mode;
    if (s->mode < 0) {
        s->regs[REG_DEPTH] = 0;
        s->regs[REG_WIDTH] = 0;
        s->regs[REG_HEIGHT] = 0;
    } else {
        s->regs[REG_DEPTH] = fb_mode_table[s->mode].depth;
        s->regs[REG_WIDTH] = fb_mode_table[s->mode].width;
        s->regs[REG_HEIGHT] = fb_mode_table[s->mode].height;
    }
    memcpy(s->palette, fb_mode_table[s->mode].default_palette, sizeof(s->palette));
    fb_invalidate_display(s);
}

static int fb_find_mode(int mode)
{
    if ((mode >= ARRAY_SIZE(fb_mode_table)) || (fb_mode_table[mode].depth == 0)) {
        return -1;
    }
    return mode;
}

static gchar *fb_mode_list(void)
{
    GString *list = g_string_new("");

    for (int i = 0; i < ARRAY_SIZE(fb_mode_table); i++) {

        g_string_append_printf(list, "    %dx%dx%d\n", fb_mode_table[i].width,
                               fb_mode_table[i].height, fb_mode_table[i].depth);
    }

    return g_string_free(list, FALSE);
}

static void fb_update_display(void *opaque)
{
    AtariSTFBState *s = ATARISTFB(opaque);
    DisplaySurface *surface = qemu_console_surface(s->con);

    qemu_flush_coalesced_mmio_buffer();

    if (s->mode < 0) {
        return;
    }

    if (fb_mode_table[s->mode].width != surface_width(surface) ||
        fb_mode_table[s->mode].height != surface_height(surface)) {
        qemu_console_resize(s->con, fb_mode_table[s->mode].width, fb_mode_table[s->mode].height);
    }

    fb_draw_graphic(s);
}

static void fb_vbl_timer(void *opaque)
{
    AtariSTFBState *s = ATARISTFB(opaque);
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    qemu_irq_raise(s->irq);

    do {
        s->next_vbl += VBL_PERIOD_NS;
    } while (s->next_vbl <= now);

    timer_mod(s->vbl_timer, s->next_vbl);
}

static uint64_t fb_ctrl_read(void *opaque, hwaddr addr, unsigned int size)
{
    AtariSTFBState *s = ATARISTFB(opaque);
    if (addr & 3) {
        return 0;
    }
    addr >>= 2;
    if (addr > ATARISTFB_NUM_REGS) {
        return 0;
    }

    if (addr < REG_PALETTE_BASE) {
        return s->regs[addr];
    } else {
        addr -= REG_PALETTE_BASE;
        if (addr < ATARISTFB_PALETTE_SIZE) {
            return s->palette[addr];
        }
    }

    return 0;
}

static void fb_ctrl_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    AtariSTFBState *s = ATARISTFB(opaque);
    if (addr & 3) {
        return;
    }
    addr >>= 2;
    if (addr > ATARISTFB_NUM_REGS) {
        return;
    }

    switch (addr) {
    case REG_MODE:
    case REG_VADDR:
    case REG_PALETTE_BASE ... (REG_PALETTE_BASE + 0x100):
        s->regs[addr] = val;
        break;

    case REG_VBL_CTRL:
        if (val) {
            s->next_vbl = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + VBL_PERIOD_NS;
            timer_mod(s->vbl_timer, s->next_vbl);
        } else {
            timer_del(s->vbl_timer);
        }
        break;

    case REG_VBL_ACK:
        qemu_irq_lower(s->irq);
        break;
    }
}

static const MemoryRegionOps ctrl_ops = {
    .read = fb_ctrl_read,
    .write = fb_ctrl_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static const GraphicHwOps ops = {
    .invalidate = fb_invalidate_display,
    .gfx_update = fb_update_display,
};

static void fb_realize(DeviceState *dev, Error **errp)
{
    AtariSTFBState *s = ATARISTFB(dev);
    DisplaySurface *surface;
    int requested_mode = s->mode;

    s->mode = fb_find_mode(requested_mode);
    if (s->mode < 0) {
        gchar *list;
        error_setg(errp, "unknown display mode %d", requested_mode);
        list = fb_mode_list();
        error_append_hint(errp, "Available modes:\n%s", list);
        g_free(list);

        return;
    }

    s->con = graphic_console_init(dev, 0, &ops, s);
    surface = qemu_console_surface(s->con);

    if (surface_bits_per_pixel(surface) != 32) {
        error_setg(errp, "unknown host depth %d",
                   surface_bits_per_pixel(surface));
        return;
    }

    memory_region_init_io(&s->mem_regs, OBJECT(dev), &ctrl_ops, s,
                          "ataristfb-regs", 0x1000);

    memory_region_init_ram(&s->mem_vram, OBJECT(dev), "ataristfb-vram",
                           ATARISTFB_VRAM_SIZE, &error_abort);
    memory_region_set_log(&s->mem_vram, true, DIRTY_MEMORY_VGA);
    s->vram = memory_region_get_ram_ptr(&s->mem_vram);

    s->vbl_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, fb_vbl_timer, s);
    fb_update_mode(s);

    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mem_regs);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mem_vram);

    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
}

static void fb_reset(DeviceState *d)
{
    AtariSTFBState *s = ATARISTFB(d);

    memcpy(s->palette, fb_mode_table[s->mode].default_palette, sizeof(s->palette));
    memset(s->vram, 0, ATARISTFB_VRAM_SIZE);
    fb_invalidate_display(s);
}

static Property fb_properties[] = {
    DEFINE_PROP_INT32("mode", AtariSTFBState, mode, 1),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription vmstate_fb = {
    .name = "ataristfb",
    .version_id = 1,
    .minimum_version_id = 1,
    .unmigratable = 1,
};

static void class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = fb_realize;
    dc->desc = "AtariST framebuffer";
    dc->reset = fb_reset;
    dc->vmsd = &vmstate_fb;
    device_class_set_props(dc, fb_properties);
}

static const TypeInfo sysbus_info = {
    .name          = TYPE_ATARISTFB,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AtariSTFBState),
    .class_init    = class_init,
};

static void register_types(void)
{
    type_register_static(&sysbus_info);
}

type_init(register_types)
