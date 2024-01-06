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

enum {
    REG_VBL_ACK,
    REG_VBL_CTRL,
    REG_MODE,
    REG_DEPTH,
    REG_WIDTH,
    REG_HEIGHT,
    REG_VADDR,
    REG_PALETTE_BASE = 0x40
};

#define ATARISTFB_NUM_REGS      REG_PALETTE_BASE
#define ATARISTFB_PALETTE_SIZE  0x100

#define TYPE_ATARISTFB "ataristfb"
OBJECT_DECLARE_SIMPLE_TYPE(AtariSTFBState, ATARISTFB)

struct AtariSTFBState {
    SysBusDevice busdev;
    MemoryRegion    mem_vram;
    MemoryRegion    mem_regs;
    QemuConsole     *con;

    uint8_t         *vram;
//    uint32_t        vram_bit_mask;

    uint32_t        regs[ATARISTFB_NUM_REGS];
    uint32_t        palette[ATARISTFB_PALETTE_SIZE];
    int             mode;

    QEMUTimer       *vbl_timer;
    uint64_t        next_vbl;
    qemu_irq        irq;
};

typedef void ataristfb_draw_func(AtariSTFBState *s, uint32_t *d, uint32_t addr, int width);

typedef struct {
    uint8_t         depth;
    uint32_t        width;
    uint32_t        height;
    uint32_t        stride;
    const uint32_t  *default_palette;
    ataristfb_draw_func *draw_fn;
} AtariSTFBMode;

#endif
