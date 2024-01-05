/*
 * QEMU AtariST Keyboard/Mouse emulation.
 *
 * Looks a lot like the IKBD ACIA.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/fifo8.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/m68k/atarist.h"
#include "hw/qdev-properties.h"
#include "ui/console.h"
#include "migration/vmstate.h"
#include "qom/object.h"

OBJECT_DECLARE_SIMPLE_TYPE(KBDState, ATARISTKBD)

#define KBD_QUEUE_SIZE 256

#define CSR_RXINT_ENABLE    0x80
#define CSR_TXINT_ENABLE    0x20

#define CSR_INTERRUPT       0x80
#define CSR_TXRDY           0x02
#define CSR_RXRDY           0x01

#define CMD_RESET1          0x80
#define CMD_RESET2          0x01
#define RSP_RESET           0xf0

#define CMD_SET_RELATIVE    0x08
#define CMD_RESUME          0x11
#define CMD_DISABLE_MOUSE   0x12
#define CMD_PAUSE           0x13
#define CMD_GET_TIME        0x1c

struct KBDState {
    SysBusDevice sbd;
    MemoryRegion mr;
    qemu_irq irq;

    uint8_t ctrl_reg;
    uint8_t status_reg;

    Fifo8 fifo;
    uint8_t buttons;
    int dx;
    int dy;
    bool reset_pending;
    bool paused;
    bool mouse_disabled;
};

static void kbd_update_interrupt(KBDState *s)
{
    /* always ready to transmit */
    s->status_reg = CSR_TXRDY;

    /* data ready to read? */
    if (!fifo8_is_empty(&s->fifo) && !s->paused) {
        s->status_reg |= CSR_RXRDY;
    }

    /* RX interrupt pending? */
    if ((s->ctrl_reg & CSR_RXINT_ENABLE) && (s->status_reg & CSR_RXRDY)) {
        s->status_reg |= CSR_INTERRUPT;
        qemu_irq_raise(s->irq);
    } else {
        qemu_irq_lower(s->irq);
    }
}

static void kbd_do_reset(KBDState *s)
{
    fifo8_reset(&s->fifo);
    s->buttons = 3;
    s->dx = 0;
    s->dy = 0;
    s->reset_pending = false;
    kbd_update_interrupt(s);
}

static uint64_t kbd_readfn(void *opaque, hwaddr addr, unsigned size)
{
    KBDState *s = ATARISTKBD(opaque);
    uint64_t val = 0;

    if (size <= 2) {
        switch (addr) {
        case 0:
            return s->status_reg;
            break;
        case 2:
            if (!fifo8_is_empty(&s->fifo) && !s->paused) {
                val = fifo8_pop(&s->fifo);
                kbd_update_interrupt(s);
            }
            break;
        default:
            break;
        }
    }
    return val;
}

static void kbd_cmd(KBDState *s, uint8_t cmd)
{
    qemu_log("ikbd: cmd 0x%02x\n", cmd);
    switch (cmd) {
    case CMD_RESET1:
        s->reset_pending = true;
        return;
    case CMD_RESET2:
        if (s->reset_pending) {
            kbd_do_reset(s);
            fifo8_push(&s->fifo, RSP_RESET);
            kbd_update_interrupt(s);
        }
        break;
    case CMD_RESUME:
        s->paused = false;
        kbd_update_interrupt(s);
        break;
    case CMD_DISABLE_MOUSE:
        s->mouse_disabled = true;
        break;
    case CMD_SET_RELATIVE:
        s->mouse_disabled = false;
        break;
    }
}

static void kbd_writefn(void *opaque, hwaddr addr, uint64_t value,
                        unsigned size)
{
    KBDState *s = ATARISTKBD(opaque);

    if (size <= 2) {
        switch (addr) {
        case 0:
            s->ctrl_reg = value;
            kbd_update_interrupt(s);
            break;
        case 2:
            kbd_cmd(s, value & 0xff);
            break;
        default:
            break;
        }
    }
}

static const MemoryRegionOps kbd_ops = {
    .read = kbd_readfn,
    .write = kbd_writefn,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static uint8_t qcode_to_ikbd[] = {
    [Q_KEY_CODE_ESC]            = 0x01,
    [Q_KEY_CODE_1]              = 0x02,
    [Q_KEY_CODE_2]              = 0x03,
    [Q_KEY_CODE_3]              = 0x04,
    [Q_KEY_CODE_4]              = 0x05,
    [Q_KEY_CODE_5]              = 0x06,
    [Q_KEY_CODE_6]              = 0x07,
    [Q_KEY_CODE_7]              = 0x08,
    [Q_KEY_CODE_8]              = 0x09,
    [Q_KEY_CODE_9]              = 0x0a,
    [Q_KEY_CODE_0]              = 0x0b,
    [Q_KEY_CODE_MINUS]          = 0x0c,
    [Q_KEY_CODE_EQUAL]          = 0x0d,
    [Q_KEY_CODE_BACKSPACE]      = 0x0e,
    [Q_KEY_CODE_TAB]            = 0x0f,
    [Q_KEY_CODE_Q]              = 0x10,
    [Q_KEY_CODE_W]              = 0x11,
    [Q_KEY_CODE_E]              = 0x12,
    [Q_KEY_CODE_R]              = 0x13,
    [Q_KEY_CODE_T]              = 0x14,
    [Q_KEY_CODE_Y]              = 0x15,
    [Q_KEY_CODE_U]              = 0x16,
    [Q_KEY_CODE_I]              = 0x17,
    [Q_KEY_CODE_O]              = 0x18,
    [Q_KEY_CODE_P]              = 0x19,
    [Q_KEY_CODE_BRACKET_LEFT]   = 0x1a,
    [Q_KEY_CODE_BRACKET_RIGHT]  = 0x1b,
    [Q_KEY_CODE_RET]            = 0x1c,
    [Q_KEY_CODE_CTRL]           = 0x1d,
    [Q_KEY_CODE_A]              = 0x1e,
    [Q_KEY_CODE_S]              = 0x1f,
    [Q_KEY_CODE_D]              = 0x20,
    [Q_KEY_CODE_F]              = 0x21,
    [Q_KEY_CODE_G]              = 0x22,
    [Q_KEY_CODE_H]              = 0x23,
    [Q_KEY_CODE_J]              = 0x24,
    [Q_KEY_CODE_K]              = 0x25,
    [Q_KEY_CODE_L]              = 0x26,
    [Q_KEY_CODE_SEMICOLON]      = 0x27,
    [Q_KEY_CODE_APOSTROPHE]     = 0x28,
    [Q_KEY_CODE_GRAVE_ACCENT]   = 0x29,
    [Q_KEY_CODE_SHIFT]          = 0x2a,
    [Q_KEY_CODE_BACKSLASH]      = 0x2b,
    [Q_KEY_CODE_Z]              = 0x2c,
    [Q_KEY_CODE_X]              = 0x2d,
    [Q_KEY_CODE_C]              = 0x2e,
    [Q_KEY_CODE_V]              = 0x2f,
    [Q_KEY_CODE_B]              = 0x30,
    [Q_KEY_CODE_N]              = 0x31,
    [Q_KEY_CODE_M]              = 0x32,
    [Q_KEY_CODE_COMMA]          = 0x33,
    [Q_KEY_CODE_DOT]            = 0x34,
    [Q_KEY_CODE_SLASH]          = 0x35,
    [Q_KEY_CODE_SHIFT_R]        = 0x36,
    /* 0x37 middle mouse button */
    [Q_KEY_CODE_ALT]            = 0x38,
    [Q_KEY_CODE_SPC]            = 0x39,
    [Q_KEY_CODE_CAPS_LOCK]      = 0x3a,
    [Q_KEY_CODE_F1]             = 0x3b,
    [Q_KEY_CODE_F2]             = 0x3c,
    [Q_KEY_CODE_F3]             = 0x3d,
    [Q_KEY_CODE_F4]             = 0x3e,
    [Q_KEY_CODE_F5]             = 0x3f,
    [Q_KEY_CODE_F6]             = 0x40,
    [Q_KEY_CODE_F7]             = 0x41,
    [Q_KEY_CODE_F8]             = 0x42,
    [Q_KEY_CODE_F9]             = 0x43,
    [Q_KEY_CODE_F10]            = 0x44,
    /* 0x45 unused */
    [Q_KEY_CODE_HOME]           = 0x47,
    [Q_KEY_CODE_UP]             = 0x48,
    /* 0x49 unused */
    [Q_KEY_CODE_KP_SUBTRACT]    = 0x4a,
    [Q_KEY_CODE_LEFT]           = 0x4b,
    /* 0x4c unused */
    [Q_KEY_CODE_RIGHT]          = 0x4d,
    [Q_KEY_CODE_KP_ADD]         = 0x4e,
    /* 0x4f unused */
    [Q_KEY_CODE_DOWN]           = 0x50,
    /* 0x51 unused */
    [Q_KEY_CODE_INSERT]         = 0x52,
    [Q_KEY_CODE_DELETE]         = 0x53,
    /* 0x54-58 unused */
    /* 0x59 mouse wheel up */
    /* 0x5a mouse wheel down */
    /* 0x5b unused */
    /* 0x5c mouse wheel left */
    /* 0x5d mouse wheel right */
    /* 0x5e mouse button 4 */
    /* 0x5f mouse button 5 */
    /* 0x60 "ISO Key" */
    [Q_KEY_CODE_F12]            = 0x61, /* "Undo" */
    [Q_KEY_CODE_HELP]           = 0x62,
    /* 0x63 "KP (" */
    /* 0x64 "KP )" */
    [Q_KEY_CODE_KP_DIVIDE]      = 0x65,
    [Q_KEY_CODE_KP_MULTIPLY]    = 0x66,
    [Q_KEY_CODE_KP_7]           = 0x67,
    [Q_KEY_CODE_KP_8]           = 0x68,
    [Q_KEY_CODE_KP_9]           = 0x69,
    [Q_KEY_CODE_KP_4]           = 0x6a,
    [Q_KEY_CODE_KP_5]           = 0x6b,
    [Q_KEY_CODE_KP_6]           = 0x6c,
    [Q_KEY_CODE_KP_1]           = 0x6d,
    [Q_KEY_CODE_KP_2]           = 0x6e,
    [Q_KEY_CODE_KP_3]           = 0x6f,
    [Q_KEY_CODE_KP_0]           = 0x70,
    [Q_KEY_CODE_KP_DECIMAL]     = 0x71,
    [Q_KEY_CODE_KP_ENTER]       = 0x72,
    [0xff]                      = 0
};

static void kbd_input(DeviceState *dev, QemuConsole *src, InputEvent *evt)
{
    KBDState *s = (KBDState *)dev;
    InputKeyEvent *key = evt->u.key.data;
    int qcode = qemu_input_key_value_to_qcode(key->key);
    uint8_t ikbdcode = qcode_to_ikbd[qcode];

    if (ikbdcode != 0) {
        if (!key->down) {
            ikbdcode |= 0x80;
        }

        if (!fifo8_is_full(&s->fifo)) {
            fifo8_push(&s->fifo, ikbdcode);
        }

        kbd_update_interrupt(s);
    }
}

static const QemuInputHandler kbd_input_handler = {
    .name = "AtariST keyboard",
    .mask = INPUT_EVENT_MASK_KEY,
    .event = kbd_input,
};

static void mouse_input(DeviceState *dev, QemuConsole *src, InputEvent *evt)
{
    KBDState *s = (KBDState *)dev;
    InputMoveEvent *move;
    InputBtnEvent *btn;
    uint8_t ikbdcode = 0;

    switch (evt->type) {
    case INPUT_EVENT_KIND_REL:
        move = evt->u.rel.data;
        if (move->axis == INPUT_AXIS_X) {
            s->dx += move->value;
        } else if (move->axis == INPUT_AXIS_Y) {
            s->dy += move->value;
        }
        break;

    case INPUT_EVENT_KIND_BTN:
        btn = evt->u.btn.data;
        switch (btn->button) {
        case INPUT_BUTTON_LEFT:
            if (btn->down) {
                s->buttons |= 0x02;
            } else {
                s->buttons &= ~0x02;
            }
            break;
        case INPUT_BUTTON_RIGHT:
            if (btn->down) {
                s->buttons |= 0x01;
            } else {
                s->buttons &= ~0x01;
            }
            break;
        case INPUT_BUTTON_MIDDLE:
            ikbdcode = 0x37;
            break;
        case INPUT_BUTTON_SIDE:
            ikbdcode = 0x5e;
            break;
        case INPUT_BUTTON_EXTRA:
            ikbdcode = 0x5f;
            break;
        case INPUT_BUTTON_WHEEL_UP:
            ikbdcode = 0x59;
            break;
        case INPUT_BUTTON_WHEEL_DOWN:
            ikbdcode = 0x5a;
            break;
        case INPUT_BUTTON_WHEEL_LEFT:
            ikbdcode = 0x5c;
            break;
        case INPUT_BUTTON_WHEEL_RIGHT:
            ikbdcode = 0x5d;
            break;
        default:
            break;
        }
        break;
    default:
        break;
    }

    if (ikbdcode) {
        if (!btn->down) {
            ikbdcode |= 0x80;
        }
        if (!fifo8_is_full(&s->fifo)) {
            fifo8_push(&s->fifo, ikbdcode);
            kbd_update_interrupt(s);
        }
    }
}

static void mouse_sync(DeviceState *dev)
{
    KBDState *s = (KBDState *)dev;

    /* always send at least one packet, as long as there's room */
    while (!s->paused && (fifo8_num_free(&s->fifo) >= 3)) {
        int dx = s->dx;
        int dy = s->dy;

        if (dx > INT8_MAX) {
            dx = INT8_MAX;
        } else if (dx < INT8_MIN) {
            dx = INT8_MIN;
        }
        if (dy > INT8_MAX) {
            dy = INT8_MAX;
        } else if (dy < INT8_MIN) {
            dy = INT8_MIN;
        }

        /* send a relative motion report */
        fifo8_push(&s->fifo, 0xf8 | s->buttons);
        fifo8_push(&s->fifo, (int8_t)dx);
        fifo8_push(&s->fifo, (int8_t)dy);
        s->dx -= dx;
        s->dy -= dy;

        /* if no more motion to report, stop */
        if ((s->dx == 0) && (s->dy == 0)) {
            break;
        }
    }
    kbd_update_interrupt(s);
}

static const QemuInputHandler mouse_input_handler = {
    .name = "AtariST mouse",
    .mask = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_REL,
    .event = mouse_input,
    .sync = mouse_sync,
};

static void kbd_reset(DeviceState *dev)
{
    KBDState *s = ATARISTKBD(dev);

    kbd_do_reset(s);
}
static void kbd_realize(DeviceState *dev, Error **errp)
{
    KBDState *s = ATARISTKBD(dev);

    fifo8_create(&s->fifo, 256);

    memory_region_init_io(&s->mr, OBJECT(dev), &kbd_ops, s, "atarist.kbd", 0x4);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mr);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    qemu_input_handler_register(dev, &kbd_input_handler);
    qemu_input_handler_register(dev, &mouse_input_handler);
}

static const VMStateDescription kbd_vmstate = {
    .name = TYPE_ATARISTKBD,
    .unmigratable = 1,
};

static void kbd_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
    dc->vmsd = &kbd_vmstate;
    dc->realize = kbd_realize;
    dc->desc = "AtariST IKBD";
    dc->reset = kbd_reset;
}

static const TypeInfo kbd_info = {
    .name          = TYPE_ATARISTKBD,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(KBDState),
    .class_init    = kbd_class_init,
};

static void kbd_register_types(void)
{
    type_register_static(&kbd_info);
}

type_init(kbd_register_types)
