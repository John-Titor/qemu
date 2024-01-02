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

typedef struct AtariFbMode {
    uint8_t depth;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
} AtariFbMode;

#define ATARISTFB_CTRL_TOPADDR  0x200
#define ATARISTFB_NUM_REGS      (ATARISTFB_CTRL_TOPADDR / sizeof(uint32_t))

typedef struct AtarifbState {
    MemoryRegion mem_vram;
    MemoryRegion mem_ctrl;
    QemuConsole *con;

    uint8_t *vram;
    uint32_t vram_bit_mask;
    uint32_t palette_current;
    uint8_t color_palette[256 * 3];
    uint32_t width, height; /* in pixels */
    uint8_t depth;

    uint32_t regs[ATARISTFB_NUM_REGS];
    AtariFbMode *mode;

    QEMUTimer *vbl_timer;
    qemu_irq irq;
} AtarifbState;

#define TYPE_ATARISTFB "sysbus-atarifb"
OBJECT_DECLARE_SIMPLE_TYPE(AtarifbSysBusState, ATARISTFB)

struct AtarifbSysBusState {
    SysBusDevice busdev;

    AtarifbState atarifb;
};

#endif
