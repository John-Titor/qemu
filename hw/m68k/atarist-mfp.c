/*
 * Partial emulation of the 68901 MFP as used in the Atari ST.
 * Timers A / B / C in countdown mode only.
 *
 * As the m68k core does not support peripheral vectoring, software
 * on the Atari side needs to handle vectoring.
 *
 * GPIP and USART operations are ignored.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "ui/console.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define TYPE_ATARIST_MFP "atarist-mfp"
OBJECT_DECLARE_SIMPLE_TYPE(MFPState, ATARIST_MFP)

/* registers */
enum {
    MFP_REG_GPDR,
    MFP_REG_AER,
    MFP_REG_DDR,
    MFP_REG_IERA,
    MFP_REG_IERB,
    MFP_REG_IPRA,
    MFP_REG_IPRB,
    MFP_REG_ISRA,
    MFP_REG_ISRB,
    MFP_REG_IMRA,
    MFP_REG_IMRB,
    MFP_REG_VR,
    MFP_REG_TACR,
    MFP_REG_TBCR,
    MFP_REG_TCDCR,
    MFP_REG_TADR,
    MFP_REG_TBDR,
    MFP_REG_TCDR,
    MFP_REG_TDDR,
    MFP_REG_SCR,
    MFP_REG_UCR,
    MFP_REG_RSR,
    MFP_REG_TSR,
    MFP_REG_UDR
};

#define IRA_TIMER_B  (1<<0)
#define IRA_TIMER_A  (1<<5)
#define IRA_GPIP_6   (1<<6)
#define IRA_GPIP_7   (1<<7)
#define IRB_GPIP_0   (1<<0)
#define IRB_GPIP_1   (1<<1)
#define IRB_GPIP_2   (1<<2)
#define IRB_GPIP_3   (1<<3)
#define IRB_TIMER_C  (1<<5)
#define IRB_GPIP_4   (1<<6)
#define IRB_GPIP_5   (1<<7)

struct MFPState {
    SysBusDevice    sbd;
    MemoryRegion    mr;
    qemu_irq        irq;

    uint32_t        clock;
    uint8_t         regs[24];

    QEMUTimer       *timer_a;
    QEMUTimer       *timer_b;
    QEMUTimer       *timer_c;
};

static unsigned int mfp_prescale_table[] = { 0, 4, 10, 16, 50, 64, 100, 200 };

static uint64_t mfp_read(void *opaque, hwaddr addr, unsigned int size)
{
    MFPState *s = ATARIST_MFP(opaque);

    if (!(addr & 1)) {
        return 0xff;
    }
    addr >>= 1;

    switch(addr) {
    case MFP_REG_GPDR:
    case MFP_REG_AER:
    case MFP_REG_DDR:
    case MFP_REG_VR:
    case MFP_REG_SCR:
    case MFP_REG_UCR:
    case MFP_REG_IERA:
    case MFP_REG_IERB:
    case MFP_REG_IMRA:
    case MFP_REG_IMRB:
    case MFP_REG_IPRA:
    case MFP_REG_IPRB:
    case MFP_REG_ISRA:
    case MFP_REG_ISRB:
    case MFP_REG_TACR:
    case MFP_REG_TBCR:
    case MFP_REG_TCDCR:
    case MFP_REG_TADR:
    case MFP_REG_TBDR:
    case MFP_REG_TCDR:
    case MFP_REG_TDDR:
        return s->regs[addr];

    case MFP_REG_RSR:
    case MFP_REG_TSR:
    case MFP_REG_UDR:
        break;
    }
    return 0x00;
}

static void mfp_update_irq(MFPState *s)
{
    if ((s->regs[MFP_REG_IMRA] & s->regs[MFP_REG_IPRA]) ||
        (s->regs[MFP_REG_IMRB] & s->regs[MFP_REG_IPRB])) {
        qemu_irq_raise(s->irq);
    } else {
        qemu_irq_lower(s->irq);
    }
}

static uint64_t mfp_timer_deadline_ns(MFPState *s, uint8_t prescale, uint8_t reg)
{
    uint64_t period_ns = (1000000000ULL * mfp_prescale_table[prescale] * s->regs[reg]) / s->clock;
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + period_ns;
}

static void mfp_reset_timer_a(MFPState *s)
{
    uint8_t val = s->regs[MFP_REG_TACR] & 0x07;

    if (mfp_prescale_table[val] == 0) {
        /* set the timer to "forever" */
        timer_mod(s->timer_a, (1ULL<<62));
    } else {
        /* (re)start */
        timer_mod(s->timer_a, mfp_timer_deadline_ns(s, val, MFP_REG_TADR));
    }
}

static void mfp_reset_timer_b(MFPState *s)
{
    uint8_t val = s->regs[MFP_REG_TBCR] & 0x07;

    if (mfp_prescale_table[val] == 0) {
        /* set the timer to "forever" */
        timer_mod(s->timer_b, (1ULL<<62));
    } else {
        /* (re)start */
        timer_mod(s->timer_b, mfp_timer_deadline_ns(s, val, MFP_REG_TBDR));
    }
}

static void mfp_reset_timer_cd(MFPState *s)
{
    uint8_t val = (s->regs[MFP_REG_TCDCR] & 0x70) >> 4;    /* ignore timer D */

    if (mfp_prescale_table[val] == 0) {
        /* set the timer to "forever" */
        timer_mod(s->timer_c, (1ULL<<62));
    } else {
        timer_mod(s->timer_c, mfp_timer_deadline_ns(s, val, MFP_REG_TCDR));
    }
}

static void mfp_timer_a(void *opaque)
{
    MFPState *s = ATARIST_MFP(opaque);

    s->regs[MFP_REG_IPRA] |= IRA_TIMER_A & s->regs[MFP_REG_IERA];
    mfp_update_irq(s);
    mfp_reset_timer_a(s);
}

static void mfp_timer_b(void *opaque)
{
    MFPState *s = ATARIST_MFP(opaque);

    s->regs[MFP_REG_IPRA] |= IRA_TIMER_B & s->regs[MFP_REG_IERA];
    mfp_update_irq(s);
    mfp_reset_timer_b(s);
}

static void mfp_timer_c(void *opaque)
{
    MFPState *s = ATARIST_MFP(opaque);

    s->regs[MFP_REG_IPRB] |= IRB_TIMER_C & s->regs[MFP_REG_IERB];
    mfp_update_irq(s);
    mfp_reset_timer_cd(s);
}

static void mfp_gpip_irq(void *opaque, int irq, int level)
{
    MFPState *s = ATARIST_MFP(opaque);

    /* we ignore DDR */

    uint8_t mask_a = 0;
    uint8_t mask_b = 0;
    switch (irq) {
    case 0:
        mask_b |= IRB_GPIP_0;
        break;
    case 1:
        mask_b |= IRB_GPIP_1;
        break;
    case 2:
        mask_b |= IRB_GPIP_2;
        break;
    case 3:
        mask_b |= IRB_GPIP_3;
        break;
    case 4:
        mask_b |= IRB_GPIP_4;
        break;
    case 5:
        mask_b |= IRB_GPIP_5;
        break;
    case 6:
        mask_a |= IRA_GPIP_6;
        break;
    case 7:
        mask_a |= IRA_GPIP_7;
        break;
    }
    if (level) {
        s->regs[MFP_REG_GPDR] &= ~(1 << irq);
        s->regs[MFP_REG_IPRA] |= (mask_a & s->regs[MFP_REG_IERA]);
        s->regs[MFP_REG_IPRB] |= (mask_b & s->regs[MFP_REG_IERB]);
    } else {
        s->regs[MFP_REG_GPDR] |= (1 << irq);
        s->regs[MFP_REG_IPRA] &= ~mask_a;
        s->regs[MFP_REG_IPRB] &= ~mask_b;
    }
    mfp_update_irq(s);
}

static void mfp_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    MFPState *s = ATARIST_MFP(opaque);

    if (!(addr & 1) || (size != 1)) {
        return;
    }
    addr >>= 1;

    switch(addr) {
    case MFP_REG_AER:
    case MFP_REG_DDR:
    case MFP_REG_VR:
    case MFP_REG_SCR:
    case MFP_REG_UCR:
    case MFP_REG_TADR:
    case MFP_REG_TBDR:
    case MFP_REG_TCDR:
    case MFP_REG_TDDR:
        s->regs[addr] = val;
        break;

    case MFP_REG_IERA:
    case MFP_REG_IERB:
    case MFP_REG_IMRA:
    case MFP_REG_IMRB:
        s->regs[addr] = val;
        mfp_update_irq(s);
        break;

        /*
         * To allow for vectoring emulation, we make IPRx w0c and set IPRx for the corresponding
         * interrupt. Confusing things may happen if more than one zero is written to IPRx.
         */
    case MFP_REG_IPRA:
    case MFP_REG_IPRB:
        if ((val & s->regs[addr]) != s->regs[addr]) {
            s->regs[addr + 2] |= ~val;
        }
        s->regs[addr] &= val;
        mfp_update_irq(s);
        break;

    case MFP_REG_ISRA:
    case MFP_REG_ISRB:
        s->regs[addr] &= val;
        break;

    case MFP_REG_TACR:
        s->regs[addr] = val;
        mfp_reset_timer_a(s);
        break;
    case MFP_REG_TBCR:
        s->regs[addr] = val;
        mfp_reset_timer_b(s);
        break;
    case MFP_REG_TCDCR:
        s->regs[addr] = val;
        mfp_reset_timer_cd(s);
        break;

    case MFP_REG_GPDR:
    case MFP_REG_RSR:
    case MFP_REG_TSR:
    case MFP_REG_UDR:
        break;
    }
}

static const MemoryRegionOps mfp_ops = {
    .read = mfp_read,
    .write = mfp_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1
};

static void mfp_realize(DeviceState *dev, Error **errp)
{
    MFPState *s = ATARIST_MFP(dev);

    memory_region_init_io(&s->mr, OBJECT(dev), &mfp_ops, s, "atarist.mfp", 0x30);

    s->timer_a = timer_new_ns(QEMU_CLOCK_VIRTUAL, mfp_timer_a, s);
    s->timer_b = timer_new_ns(QEMU_CLOCK_VIRTUAL, mfp_timer_b, s);
    s->timer_c = timer_new_ns(QEMU_CLOCK_VIRTUAL, mfp_timer_c, s);
}

static void mfp_reset(DeviceState *d)
{
    MFPState *s = ATARIST_MFP(d);
    memset(&s->regs, 0, sizeof(s->regs));
    mfp_update_irq(s);
}

static const VMStateDescription mfp_vmstate = {
    .name = TYPE_ATARIST_MFP,
    .unmigratable = 1,
};

static void mfp_instance_init(Object *obj)
{
    SysBusDevice *dev = SYS_BUS_DEVICE(obj);
    MFPState *s = ATARIST_MFP(obj);

    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mr);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    qdev_init_gpio_in(DEVICE(obj), mfp_gpip_irq, 8);
}

static Property mfp_properties[] = {
    DEFINE_PROP_UINT32("clock", MFPState, clock, 2457600),
    DEFINE_PROP_END_OF_LIST(),
};

static void mfp_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->vmsd = &mfp_vmstate;
    dc->realize = mfp_realize;
    dc->desc = "AtariST MFP";
    dc->reset = mfp_reset;
    device_class_set_props(dc, mfp_properties);
}

static const TypeInfo mfp_info = {
    .name          = TYPE_ATARIST_MFP,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MFPState),
    .class_init    = mfp_class_init,
    .instance_init = mfp_instance_init,
};

static void mfp_register_types(void)
{
    type_register_static(&mfp_info);
}

type_init(mfp_register_types)
