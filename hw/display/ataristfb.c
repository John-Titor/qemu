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

/*
 * Guest operation model:
 *
 * Turn display off:
 *  - write 0 to REG_VADDR
 *
 * Select or change resolution and turn display on:
 *  - write REG_DEPTH, REG_WIDTH, REG_HEIGHT with valid values
 *  - write REG_VADDR with a valid, nonzero value
 *  - check REG_VADDR; if zero resolution or address is invalid and display is off
 *
 * Move framebuffer:
 *  - Write REG_VADDR with a valid, nonzero value.
 *  - check REG_VADDR; if zero address is invalid and display is off
 *
 * Constraints:
 *  - REG_DEPTH must be 1, 2, 4, or 8
 *  - REG_WIDTH must be a multiple of 16 due to planar video format
 *  - REG_HEIGHT must be at least 1
 *  - REG_VADDR must be a multiple of 2
 *  - There must be RAM backing the span from REG_VADDR..REG_VADDR + (REG_WIDTH * REG_HEIGHT * REG_DEPTH / 8)
 *
 * Line stride is always (REG_WIDTH * REG_DEPTH / 8). The display buffer is expected to be packed Atari-style,
 * i.e. as groups of big-endian 16-bit planes.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "exec/address-spaces.h"
#include "hw/sysbus.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "hw/display/ataristfb.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "trace.h"

/* mono */
static void fb_draw_line1(void *opaque, uint8_t *dest, const uint8_t *src, int cols, int dest_col_pitch)
{
    AtariSTFBState * restrict s = ATARISTFB(opaque);
    uint32_t * restrict d = (uint32_t *)dest;
    const uint16_t * restrict p = (const uint16_t *)src;

    for (int x = 0; x < cols; x += 16) {
        for (int i = 8; i < 24; i++) {
            int mask = 0x8000 >> (i % 16);

            *d++ = (*p & mask) ? s->palette[0] : s->palette[1];
        }
        p++;
    }
}

static void fb_draw_line2(void *opaque, uint8_t *dest, const uint8_t *src, int cols, int dest_col_pitch)
{
    AtariSTFBState * restrict s = ATARISTFB(opaque);
    uint32_t * restrict d = (uint32_t *)dest;
    const uint16_t * restrict p = (const uint16_t *)src;

    for (int x = 0; x < cols; x += 16) {
        for (int i = 8; i < 24; i++) {
            int mask = 0x8000 >> (i % 16);
            int idx = (((p[1] & mask) ? 2 : 0) |
                       ((p[0] & mask) ? 1 : 0));

            *d++ = s->palette[idx];
        }
        p += 2;
    }
}

static void fb_draw_line4(void *opaque, uint8_t *dest, const uint8_t *src, int cols, int dest_col_pitch)
{
    AtariSTFBState * restrict s = ATARISTFB(opaque);
    uint32_t * restrict d = (uint32_t *)dest;
    const uint16_t * restrict p = (const uint16_t *)src;

    for (int x = 0; x < cols; x += 16) {
        for (int i = 8; i < 24; i++) {
            int mask = 0x8000 >> (i % 16);
            int idx = (((p[3] & mask) ? 8 : 0) |
                       ((p[2] & mask) ? 4 : 0) |
                       ((p[1] & mask) ? 2 : 0) |
                       ((p[0] & mask) ? 1 : 0));

            *d++ = s->palette[idx];
        }
        p += 4;
    }
}

static void fb_draw_line8(void *opaque, uint8_t *dest, const uint8_t *src, int cols, int dest_col_pitch)
{
    AtariSTFBState * restrict s = ATARISTFB(opaque);
    uint32_t * restrict d = (uint32_t *)dest;
    const uint16_t * restrict p = (const uint16_t *)src;

    for (int x = 0; x < cols; x += 16) {
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
        p += 8;
    }
}

static void fb_draw_framebuffer(AtariSTFBState *s)
{
    DisplaySurface *surface = qemu_console_surface(s->con);
    int first_dirty = -1;
    int last_dirty = 0;

    framebuffer_update_display(surface,                 /* surface to render to */
                               &s->fb_section,          /* source memory section */
                               s->width,                /* cols */
                               s->height,               /* rows */
                               s->width * s->depth / 8, /* source row stride */
                               surface_stride(surface), /* destination row stride */
                               4,                       /* destination column stride */
                               s->fb_redraw,            /* full redraw? */
                               s->draw_fn,              /* line draw function */
                               s,                       /* instance pointer */
                               &first_dirty,            /* dirty region start */
                               &last_dirty);            /*      "       end */

    s->fb_redraw = false;
    if (first_dirty != -1) {
        dpy_gfx_update(s->con, 0, first_dirty, s->width, last_dirty);
    }
}

static void fb_set_vaddr(AtariSTFBState *s, uint32_t vaddr)
{
    /* sanity-check registers */
    if (s->regs[REG_VADDR] % 2) {
        s->regs[REG_VADDR] = 0;
    }
    if ((s->regs[REG_WIDTH] < 320) ||
        (s->regs[REG_WIDTH] > 2048) ||
        (s->regs[REG_WIDTH] % 16)) {
        s->regs[REG_VADDR] = 0;
    }
    if ((s->regs[REG_HEIGHT] < 1) ||
        (s->regs[REG_HEIGHT] > 2048)) {
        s->regs[REG_VADDR] = 0;
    }
    switch (s->regs[REG_DEPTH]) {
    case 1:
    case 2:
    case 4:
    case 8:
        break;
    default:
        s->regs[REG_VADDR] = 0;
    }

    /* display off */
    if (s->regs[REG_VADDR] == 0) {
        s->regs[REG_DEPTH] = 0;
        s->regs[REG_WIDTH] = 0;
        s->regs[REG_HEIGHT] = 0;
    }

    /* cache display geometry since regs are writable */
    s->depth = s->regs[REG_DEPTH];
    s->width = s->regs[REG_WIDTH];
    s->height = s->regs[REG_HEIGHT];

    /* select draw function */
    switch (s->depth) {
    case 1:
        s->draw_fn = fb_draw_line1;
        break;
    case 2:
        s->draw_fn = fb_draw_line2;
        break;
    case 4:
        s->draw_fn = fb_draw_line4;
        break;
    case 8:
        s->draw_fn = fb_draw_line8;
        break;
    default:
        /* can't draw this format */
        return;
    }

    /* invalidate display subregion and force full redraw */
    s->fb_valid = false;
    s->fb_redraw = true;
}

static void fb_invalidate_display(void *opaque)
{
    AtariSTFBState *s = ATARISTFB(opaque);

    s->fb_redraw = true;
}

static void fb_update_display(void *opaque)
{
    AtariSTFBState *s = ATARISTFB(opaque);
    DisplaySurface *surface = qemu_console_surface(s->con);

    if (s->regs[REG_VADDR] == 0) {
        return;
    }

    qemu_flush_coalesced_mmio_buffer();

    if (s->width != surface_width(surface) ||
        s->height != surface_height(surface)) {
        qemu_console_resize(s->con, s->width, s->height);
    }

    if (!s->fb_valid) {
        framebuffer_update_memory_section(&s->fb_section,
                                          get_system_memory(),
                                          s->regs[REG_VADDR],
                                          s->width,
                                          s->width * s->depth / 8);
        s->fb_valid = true;
    }

    fb_draw_framebuffer(s);
}

static void fb_vbl_timer(void *opaque)
{
    AtariSTFBState *s = ATARISTFB(opaque);

    qemu_irq_raise(s->irq);

    s->next_vbl += s->regs[REG_VBL_PERIOD];
    timer_mod(s->vbl_timer, s->next_vbl);
}

static uint64_t fb_reg_read(void *opaque, hwaddr addr, unsigned int size)
{
    AtariSTFBState *s = ATARISTFB(opaque);

    addr /= 4;
    if (addr < ATARISTFB_NUM_REGS) {
        return s->regs[addr];
    }
    return 0;
}

static void fb_reg_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    AtariSTFBState *s = ATARISTFB(opaque);

    addr /= 4;
    if (addr < ATARISTFB_NUM_REGS) {
        s->regs[addr] = val;
    }

    switch (addr) {
    case REG_VBL_PERIOD:
        if (val > 1000000) {
            /* enable VBL interrupt given a sane period */
            s->next_vbl = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->regs[REG_VBL_PERIOD];
            timer_mod(s->vbl_timer, s->next_vbl);
        } else {
            /* disable VBL interrupt */
            timer_del(s->vbl_timer);
            s->regs[REG_VBL_PERIOD] = 0;
        }
        break;

    case REG_VBL_ACK:
        /* clear VBL interrupt */
        qemu_irq_lower(s->irq);
        break;

    case REG_VADDR:
        /* set new display mode */
        fb_set_vaddr(s, val);
        break;
    }
}

static uint64_t fb_palette_read(void *opaque, hwaddr addr, unsigned int size)
{
    AtariSTFBState *s = ATARISTFB(opaque);

    addr /= 4;
    if (addr < ATARISTFB_PALETTE_SIZE) {
        return s->palette[addr];
    }
    return 0;
}

static void fb_palette_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    AtariSTFBState *s = ATARISTFB(opaque);

    addr /= 4;
    if (addr < ATARISTFB_PALETTE_SIZE) {
        s->palette[addr] = val & 0x00ffffffff;
    }
}

static const MemoryRegionOps regs_ops = {
    .read = fb_reg_read,
    .write = fb_reg_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static const MemoryRegionOps palette_ops = {
    .read = fb_palette_read,
    .write = fb_palette_write,
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

    s->con = graphic_console_init(dev, 0, &ops, s);
    surface = qemu_console_surface(s->con);

    if (surface_bits_per_pixel(surface) != 32) {
        error_setg(errp, "unsupported host display depth %d", surface_bits_per_pixel(surface));
        return;
    }

    s->vbl_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, fb_vbl_timer, s);

    memory_region_init_io(&s->mem_regs,
                          OBJECT(dev),
                          &regs_ops,
                          s,
                          "ataristfb-regs",
                          sizeof(uint32_t) * ATARISTFB_NUM_REGS);
    memory_region_init_io(&s->mem_palette,
                          OBJECT(dev),
                          &palette_ops,
                          s,
                          "ataristfb-palette",
                          sizeof(uint32_t) * ATARISTFB_PALETTE_SIZE);

    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mem_regs);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mem_palette);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
}

static void fb_reset(DeviceState *d)
{
    AtariSTFBState *s = ATARISTFB(d);

    fb_set_vaddr(s, 0);
    fb_invalidate_display(s);
}

static Property fb_properties[] = {
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
