/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * QEMU M68K Atari-ish Machine
 *
 * Derived from hw/m68k/virt.c, marked (c) 2020 Laurent Vivier <laurent@vivier.eu>
 */

/*
 * TODO:
 *
 * - mc146818rtc.c - needs a sysbus interface
 * - atarifb.c - more plane formats, resolutions, shared memory operation
 * - work out how to deal with less machine RAM
 * - map MMIO ranges for emulation
 * - map the MMIO-IDE to match the Atari mapping (shift = 2?)
 * - what's with the UI hangs (disk wait?) when -serial mon:stdio is not specified?
 * - why does clicking into the SDL window the first time leave you dragging a rubber band?
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/guest-random.h"
#include "sysemu/sysemu.h"
#include "cpu.h"
#include "hw/boards.h"
#include "hw/qdev-properties.h"
#include "hw/loader.h"
#include "ui/console.h"
#include "hw/sysbus.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "sysemu/qtest.h"
#include "sysemu/runstate.h"
#include "sysemu/reset.h"

#include "hw/intc/m68k_irqc.h"
#include "hw/misc/virt_ctrl.h"
#include "hw/char/goldfish_tty.h"
#include "hw/rtc/goldfish_rtc.h"
#include "hw/intc/goldfish_pic.h"
#include "hw/display/ataristfb.h"
#include "hw/m68k/atarist.h"
#include "hw/ide/mmio.h"

#define ATARI_ROM_BASE      0x00e00000

/*
 * IRQ 0-7 map to M68K IPL0-7
 * IRQ 8+ map to one of 6 Goldfish PICs, each in turn routing to IPL1-7 respectively.
 * All I/O interrupts route via the GF PICs.
 */
#define PIC_IRQ_BASE(num)     (8 + (num - 1) * 32)
#define PIC_IRQ(num, irq)     (PIC_IRQ_BASE(num) + irq - 1)
#define PIC_GPIO(pic_irq)     (qdev_get_gpio_in(pic_dev[(pic_irq - 8) / 32], (pic_irq - 8) % 32))

#define GF_PIC_MMIO_BASE 0xff000000         /* MMIO: 0xff000000 - 0xff005fff */
#define GF_PIC_IRQ_BASE  1                  /* IRQ: #1 -> #6 */
#define GF_PIC_NB        6

/* 2 goldfish RTC (really timers), one at IPL6 like Timer C, one at IPL4 like VBL */
#define GF_RTC1_MMIO_BASE 0xff006000        /* MMIO: 0xff006000 - 0xff006fff */
#define GF_RTC1_IRQ_BASE PIC_IRQ(6, 1)      /* PIC: #6, IRQ: #1 */
#define GF_RTC2_MMIO_BASE 0xff007000        /* MMIO: 0xff007000 - 0xff007fff */
#define GF_RTC2_IRQ_BASE PIC_IRQ(4, 1)      /* PIC: #4, IRQ: #1 */

/* 1 goldfish-tty - no interrupt, used for logging only */
#define GF_TTY_MMIO_BASE 0xff008000         /* MMIO: 0xff008000 - 0xff008fff */

/* 1 virt-ctrl - no interrupt */
#define VIRT_CTRL_MMIO_BASE 0xff009000      /* MMIO: 0xff009000 - 0xff009fff */

/* 1 ataristfb - VBL interrupt at IPL4 but not used */
#define ATARISTFB_DAFB_BASE  0xff70e000    /* DAFB: 0xff70e000 - 0xff70efff */
#define ATARISTFB_MMIO_BASE  0xff800000    /* MMIO: 0xff800000 - 0xffbfffff (4MiB) */
#define ATARISTFB_IRQ_BASE   PIC_IRQ(4, 2) /* PIC: 4, IRQ: 2 #*/

/* 1 atarist-kbd - at the IKBD ACIA address and IPL6 */
#define ATARI_KBD_MMIO_BASE 0xfffffc00
#define ATARI_KBD_IRQ_BASE  PIC_IRQ(6, 2)

/* 2 ide-mmio - interrupts assigned but not used */
#define IDE_MMIO1_BASE 0xff00b000
#define IDE_MMIO2_OFFSET 0x10
#define IDE_MMIO_STRIDE 0x20
#define IDE_IRQ_BASE  PIC_IRQ(2, 1)
#define IDE_NB 2

typedef struct {
    M68kCPU *cpu;
    hwaddr initial_pc;
    hwaddr initial_stack;
} ResetInfo;

static void main_cpu_reset(void *opaque)
{
    ResetInfo *reset_info = opaque;
    M68kCPU *cpu = reset_info->cpu;
    CPUState *cs = CPU(cpu);

    cpu_reset(cs);
    cpu->env.aregs[7] = reset_info->initial_stack;
    cpu->env.pc = reset_info->initial_pc;
}

static void virt_init(MachineState *machine)
{
    M68kCPU *cpu;
    const char *rom_filename = machine->kernel_filename;
    DeviceState *dev;
    DeviceState *irqc_dev;
    DeviceState *pic_dev[GF_PIC_NB];
    SysBusDevice *sysbus;
    hwaddr io_base;
    ResetInfo *reset_info;

    /* needs to cover ROM space for ... reasons */
    if (machine->ram_size < (15 * MiB)) {
        /* need allocated to host ROM at this point */
        error_report("memory size must be at least 15M");
        exit(1);
    }

    /* CPU */
    cpu = M68K_CPU(cpu_create(machine->cpu_type));
    reset_info = g_new0(ResetInfo, 1);
    reset_info->cpu = cpu;
    qemu_register_reset(main_cpu_reset, reset_info);

    /* RAM */
    memory_region_add_subregion(get_system_memory(), 0, machine->ram);

    /* ROM */
    if (rom_filename) {
        if (rom_add_file_fixed(rom_filename, ATARI_ROM_BASE, 0) < 0) {
            error_report("could not load ROM '%s'", rom_filename);
            exit(1);
        }
        reset_info->initial_pc = ATARI_ROM_BASE;
    }

    /* M68K IRQ Controller */
    irqc_dev = qdev_new(TYPE_M68K_IRQC);
    object_property_set_link(OBJECT(irqc_dev), "m68k-cpu", OBJECT(cpu), &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(irqc_dev), &error_fatal);

    /* goldfish-tty */
    dev = qdev_new(TYPE_GOLDFISH_TTY);
    sysbus = SYS_BUS_DEVICE(dev);
    qdev_prop_set_chr(dev, "chardev", serial_hd(0));
    sysbus_realize_and_unref(sysbus, &error_fatal);
    sysbus_mmio_map(sysbus, 0, GF_TTY_MMIO_BASE);


    /*
     * 6 goldfish-pic
     *
     * map: 0xff000000 - 0xff006fff = 28 KiB
     * IRQ: #1 (lower priority) -> #6 (higher priority)
     *
     */
    io_base = GF_PIC_MMIO_BASE;
    for (int i = 0; i < GF_PIC_NB; i++, io_base += 0x1000) {
        pic_dev[i] = qdev_new(TYPE_GOLDFISH_PIC);
        sysbus = SYS_BUS_DEVICE(pic_dev[i]);
        qdev_prop_set_uint8(pic_dev[i], "index", i);
        sysbus_realize_and_unref(sysbus, &error_fatal);

        sysbus_mmio_map(sysbus, 0, io_base);
        sysbus_connect_irq(sysbus, 0, qdev_get_gpio_in(irqc_dev, i));
    }

    /* goldfish-rtc 1 */
    dev = qdev_new(TYPE_GOLDFISH_RTC);
    qdev_prop_set_bit(dev, "big-endian", true);
    sysbus = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    sysbus_mmio_map(sysbus, 0, GF_RTC1_MMIO_BASE);
    sysbus_connect_irq(sysbus, 0, PIC_GPIO(GF_RTC1_IRQ_BASE));

    /* goldfish-rtc 2 */
    dev = qdev_new(TYPE_GOLDFISH_RTC);
    qdev_prop_set_bit(dev, "big-endian", true);
    sysbus = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    sysbus_mmio_map(sysbus, 0, GF_RTC2_MMIO_BASE);
    sysbus_connect_irq(sysbus, 0, PIC_GPIO(GF_RTC2_IRQ_BASE));

    /* virt controller */
    sysbus_create_simple(TYPE_VIRT_CTRL, VIRT_CTRL_MMIO_BASE, 0);

    /* keyboard / mouse */
    sysbus_create_simple(TYPE_ATARISTKBD, ATARI_KBD_MMIO_BASE,
                         PIC_GPIO(ATARI_KBD_IRQ_BASE));

    /* IDE */
    io_base = IDE_MMIO1_BASE;
    for (int i = 0; i < IDE_NB; i++, io_base += IDE_MMIO_STRIDE) {
        dev = qdev_new(TYPE_MMIO_IDE);
        sysbus = SYS_BUS_DEVICE(dev);
        sysbus_connect_irq(sysbus, 0, PIC_GPIO(IDE_IRQ_BASE + i));
        qdev_prop_set_uint32(dev, "shift", 1);
        sysbus_realize_and_unref(sysbus, &error_fatal);
        sysbus_mmio_map(sysbus, 0, io_base);
        sysbus_mmio_map(sysbus, 1, io_base + IDE_MMIO2_OFFSET);
        mmio_ide_init_drives(dev, drive_get(IF_IDE, i, 0), drive_get(IF_IDE, i, 1));
    }

    /* framebuffer */
    dev = qdev_new(TYPE_ATARISTFB);
    sysbus = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    sysbus_connect_irq(sysbus, 0, PIC_GPIO(ATARISTFB_IRQ_BASE));
    sysbus_mmio_map(sysbus, 0, ATARISTFB_DAFB_BASE);
    sysbus_mmio_map(sysbus, 1, ATARISTFB_MMIO_BASE);
}

static void virt_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->desc = "QEMU AtariST";
    mc->init = virt_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68040");
    mc->max_cpus = 1;
    mc->no_floppy = 1;
    mc->no_parallel = 1;
    mc->block_default_type = IF_IDE;
    mc->default_ram_id = "atarist_virt.ram";
}

static const TypeInfo virt_machine_info = {
    .name       = MACHINE_TYPE_NAME("atarist"),
    .parent     = TYPE_MACHINE,
    .abstract   = true,
    .class_init = virt_machine_class_init,
};

static void virt_machine_register_types(void)
{
    type_register_static(&virt_machine_info);
}

type_init(virt_machine_register_types)

#define DEFINE_VIRT_MACHINE(major, minor, latest) \
    static void virt_##major##_##minor##_class_init(ObjectClass *oc, \
                                                    void *data) \
    { \
        MachineClass *mc = MACHINE_CLASS(oc); \
        virt_machine_##major##_##minor##_options(mc); \
        mc->desc = "QEMU " # major "." # minor " AtariST"; \
        if (latest) { \
            mc->alias = "atarist"; \
        } \
    } \
    static const TypeInfo machvirt_##major##_##minor##_info = { \
        .name = MACHINE_TYPE_NAME("atarist-" # major "." # minor), \
        .parent = MACHINE_TYPE_NAME("atarist"), \
        .class_init = virt_##major##_##minor##_class_init, \
    }; \
    static void machvirt_machine_##major##_##minor##_init(void) \
    { \
        type_register_static(&machvirt_##major##_##minor##_info); \
    } \
    type_init(machvirt_machine_##major##_##minor##_init);

static void virt_machine_9_0_options(MachineClass *mc)
{
}
DEFINE_VIRT_MACHINE(9, 0, true)

static void virt_machine_8_2_options(MachineClass *mc)
{
    virt_machine_9_0_options(mc);
    compat_props_add(mc->compat_props, hw_compat_8_2, hw_compat_8_2_len);
}
DEFINE_VIRT_MACHINE(8, 2, false)

