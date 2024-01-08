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
#include "hw/pci-host/gpex.h"
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
#include "hw/display/ataristfb.h"
#include "hw/m68k/atarist.h"
#include "hw/ide/mmio.h"

/*
 * System memory map
 *
 * 0x0000_0000-0x8000_0000  2G RAM
 * 0xd000_0000-0xfe00_0000  PCIe MMIO space
 * 0xfe00_0000-0xff00_0000  PCIe ECAM space
 * 0xff00_0000-0xff01_0000  PCIe IO space
 * 0xfff0_0000-0xfff0_007f  Falcon IDE controllers
 * 0xffff_b400-0xffff_b43f  Goldfish TTY device
 * 0xffff_b500-0xffff_b50f  Virt control device
 * 0xffff_c000-0xffff_c3ff  Framebuffer control registers
 * 0xffff_c400-0xffff_c4ff  Framebuffer palette registers
 * 0xffff_fc00-0xffff_ffc3  IKBD
 */

#define ATARI_ROM_BASE      0x00e00000

#define ATARI_MFP_BASE      0xfffffa00  /* MFP emulator */
#define ATARI_MFP_IRQ_LEVEL 6

#define ATARI_PCI_MMIO_BASE 0xd0000000
#define ATARI_PCI_MMIO_SIZE 0x1fd00000
#define ATARI_PCI_ECAM_BASE 0xffd00000
#define ATARI_PCI_ECAM_SIZE 0x00100000  /* 1 bus, 32 devices, 8 functions */
#define ATARI_PCI_IO_BASE   0xffe00000
#define ATARI_PCI_IRQ_LEVEL 5

#define ATARI_IKBD_BASE     0xfffffc00  /* IKBD emulator */
#define ATARI_IKBD_MFP_IRQ  4           /* GPIP 4 -> MFP irq 6 */

#define ATARI_IDE_BASE      0xfff00000  /* Falcon IDE address */
#define ATARI_IDE_OFFSET    0x10        /* alt status reg offset */
#define ATARI_IDE_STRIDE    0x20
#define ATARI_IDE_COUNT     2           /* 2 controllers */
/* XXX TODO - set shift to 4 and use standard Falcon settings */

#define ATARI_FB_REGS_BASE  0xffffc000  /* framebuffer */
#define ATARI_FB_PAL_BASE   0xffffc400
#define ATARI_FB_IRQ_LEVEL  3           /* VBL shim */

#define GF_TTY_BASE         0xffffb400  /* logging pipe */
#define VIRT_CTRL_BASE      0xffffb500  /* system control */

typedef struct {
    M68kCPU *cpu;
    hwaddr  initial_pc;
} ResetInfo;

static void main_cpu_reset(void *opaque)
{
    ResetInfo *reset_info = (ResetInfo *)opaque;
    M68kCPU *cpu = reset_info->cpu;
    CPUState *cs = CPU(cpu);

    cpu_reset(cs);
    cpu->env.pc = reset_info->initial_pc;
}

static void create_pci(DeviceState *irqc)
{
    DeviceState *dev;
    MemoryRegion *mmio_alias;
    MemoryRegion *mmio_reg;
    MemoryRegion *ecam_alias;
    MemoryRegion *ecam_reg;
    int i;

    /* PCIe host bridge */
    dev = qdev_new(TYPE_GPEX_HOST);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    /* map a limited ECAM since we only have one bus */
    ecam_alias = g_new0(MemoryRegion, 1);
    ecam_reg = sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0);
    memory_region_init_alias(ecam_alias, OBJECT(dev), "pcie-ecam",
                             ecam_reg, 0, ATARI_PCI_ECAM_SIZE);
    memory_region_add_subregion(get_system_memory(), ATARI_PCI_ECAM_BASE, ecam_alias);

    /*
     * Map the PCI window 1:1, i.e. host address maps directly to bus address.
     */
    mmio_alias = g_new0(MemoryRegion, 1);
    mmio_reg = sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 1);
    memory_region_init_alias(mmio_alias, OBJECT(dev), "pcie-mmio",
                             mmio_reg, ATARI_PCI_MMIO_BASE, ATARI_PCI_MMIO_SIZE);
    memory_region_add_subregion(get_system_memory(), ATARI_PCI_MMIO_BASE, mmio_alias);

    /*
     * Map I/O port space.
     */
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 2, ATARI_PCI_IO_BASE);

    /*
     * Wire all PCI interrupts to level 5.
     */
    for (i = 0; i < GPEX_NUM_IRQS; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), i, qdev_get_gpio_in(irqc, ATARI_MFP_IRQ_LEVEL - 1));
        gpex_set_irq_num(GPEX_HOST(dev), i, 5); /* not clear what this gets used for... */
    }
}

static void virt_init(MachineState *machine)
{
    M68kCPU         *cpu = M68K_CPU(cpu_create(machine->cpu_type));
    DeviceState     *irqc_dev;
    DeviceState     *mfp_dev;
    DeviceState     *dev;
    SysBusDevice    *sysbus;
    ResetInfo       *reset_info = g_new0(ResetInfo, 1);
    const char      *rom_filename = machine->kernel_filename;
    hwaddr          io_base;

    /* needs to cover ROM space? */
    if (machine->ram_size < (15 * MiB)) {
        error_report("memory size must be at least 15M");
        exit(1);
    }

    /* Wire up reset */
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

    /* m68k interrupt controller */
    irqc_dev = qdev_new(TYPE_M68K_IRQC);
    object_property_set_link(OBJECT(irqc_dev), "m68k-cpu", OBJECT(cpu), &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(irqc_dev), &error_fatal);

    /* MFP */
    mfp_dev = qdev_new("atarist-mfp");
    sysbus = SYS_BUS_DEVICE(mfp_dev);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    sysbus_mmio_map(sysbus, 0, ATARI_MFP_BASE);
    sysbus_connect_irq(sysbus, 0, qdev_get_gpio_in(irqc_dev, ATARI_MFP_IRQ_LEVEL - 1));

    /* IKBD */
    sysbus_create_simple(TYPE_ATARISTKBD, ATARI_IKBD_BASE, qdev_get_gpio_in(mfp_dev, ATARI_IKBD_MFP_IRQ));

    /* IDE */
    io_base = ATARI_IDE_BASE;
    for (int i = 0; i < ATARI_IDE_COUNT; i++, io_base += ATARI_IDE_STRIDE) {
        dev = qdev_new(TYPE_MMIO_IDE);
        sysbus = SYS_BUS_DEVICE(dev);
        /* sysbus_connect_irq(sysbus, 0, XXX + i)); - no interrupt? */
        qdev_prop_set_uint32(dev, "shift", 1);
        sysbus_realize_and_unref(sysbus, &error_fatal);
        sysbus_mmio_map(sysbus, 0, io_base);
        sysbus_mmio_map(sysbus, 1, io_base + ATARI_IDE_OFFSET);
        mmio_ide_init_drives(dev, drive_get(IF_IDE, i, 0), drive_get(IF_IDE, i, 1));
    }

    /* framebuffer */
    dev = qdev_new(TYPE_ATARISTFB);
    sysbus = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    sysbus_connect_irq(sysbus, 0, qdev_get_gpio_in(irqc_dev, ATARI_FB_IRQ_LEVEL - 1));
    sysbus_mmio_map(sysbus, 0, ATARI_FB_REGS_BASE);
    sysbus_mmio_map(sysbus, 1, ATARI_FB_PAL_BASE);

    /* goldfish-tty for console logging, output only */
    dev = qdev_new(TYPE_GOLDFISH_TTY);
    sysbus = SYS_BUS_DEVICE(dev);
    qdev_prop_set_chr(dev, "chardev", serial_hd(0));
    sysbus_realize_and_unref(sysbus, &error_fatal);
    sysbus_mmio_map(sysbus, 0, GF_TTY_BASE);

    /* virt controller */
    sysbus_create_simple(TYPE_VIRT_CTRL, VIRT_CTRL_BASE, 0);

    /* PCI bus */
    create_pci(irqc_dev);
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

