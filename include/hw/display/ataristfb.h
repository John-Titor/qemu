/*
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

#ifndef ATARISTFB_H
#define ATARISTFB_H

#include "exec/memory.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "ui/console.h"
#include "qemu/timer.h"
#include "hw/display/framebuffer.h"

enum {
    REG_VBL_ACK,
    REG_VBL_PERIOD,
    REG_DEPTH,
    REG_WIDTH,
    REG_HEIGHT,
    REG_VADDR,
};

#define ATARISTFB_NUM_REGS      0x10
#define ATARISTFB_PALETTE_SIZE  0x100

#define TYPE_ATARISTFB "ataristfb"
OBJECT_DECLARE_SIMPLE_TYPE(AtariSTFBState, ATARISTFB)

struct AtariSTFBState {
    SysBusDevice        busdev;
    MemoryRegion        mem_regs;
    MemoryRegion        mem_palette;
    QemuConsole         *con;

    uint8_t             *vram;

    uint32_t            regs[ATARISTFB_NUM_REGS];
    uint32_t            palette[ATARISTFB_PALETTE_SIZE];

    uint32_t            depth;
    uint32_t            width;
    uint32_t            height;

    bool                fb_valid;
    bool                fb_redraw;
    drawfn              draw_fn;
    MemoryRegionSection fb_section;

    QEMUTimer           *vbl_timer;
    uint64_t            next_vbl;
    qemu_irq            irq;
};

#endif
