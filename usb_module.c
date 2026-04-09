/*
 * usb_kbd.c — USB HID Keyboard Driver
 *
 * Features:
 *   - Full mechanical keyboard scancode table
 *   - Guided test mode  (toggle: LCtrl + LAlt)
 *   - Hold-to-repeat for all keys including Backspace
 *   - LED control: CapsLock / NumLock / ScrollLock via HID SET_REPORT
 *   - Debug mode: module param  debug=1  (or  insmod usb_kbd.ko debug=1)
 *                 logs every raw HID report + LED state changes
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/bitops.h>

/* ── Module parameters ─────────────────────────────────────────────────── */

static bool debug;
module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "Enable debug logging (0=off, 1=on)");

#define kbd_dbg(fmt, ...) \
    do { if (debug) printk(KERN_DEBUG "usb_kbd: " fmt, ##__VA_ARGS__); } while (0)

/* ── Device IDs ─────────────────────────────────────────────────────────── */

/*
#define USB_KBD_VENDOR_ID  0x062a
#define USB_KBD_PRODUCT_ID 0x4101
*/

/* ── HID LED bits (HID spec, Usage Page 0x08) ───────────────────────────── */
#define HID_LED_NUMLOCK    BIT(0)
#define HID_LED_CAPSLOCK   BIT(1)
#define HID_LED_SCROLLLOCK BIT(2)

/* ── State machine ──────────────────────────────────────────────────────── */

enum test_state {
    TEST_OFF,
    TEST_RUNNING,
    TEST_DONE
};

/* ── Driver private data ────────────────────────────────────────────────── */

struct usb_kbd {
    struct usb_device      *usbdev;

    /* Interrupt IN URB — receives HID reports from keyboard */
    struct urb             *irq;
    unsigned char          *irq_buf;
    dma_addr_t              irq_dma;
    size_t                  irq_size;

    /* OUT URB — sends LED state to keyboard */
    struct urb             *led;
    unsigned char          *led_buf;   /* 1-byte HID LED report */
    dma_addr_t              led_dma;
    unsigned char           led_state; /* current LED bitmask */

    struct input_dev       *input;
    unsigned char           old[8];    /* previous HID report */

    enum test_state         state;
    int                     current_test_index;
    struct usb_ctrlrequest *cr;
    dma_addr_t              cr_dma;
};

/* ── Scancode → Linux keycode table (full mechanical keyboard) ──────────── */

static const unsigned char scancode_to_keycode[256] = {
    /* ── Letters ── */
    [0x04] = KEY_A, [0x05] = KEY_B, [0x06] = KEY_C, [0x07] = KEY_D,
    [0x08] = KEY_E, [0x09] = KEY_F, [0x0a] = KEY_G, [0x0b] = KEY_H,
    [0x0c] = KEY_I, [0x0d] = KEY_J, [0x0e] = KEY_K, [0x0f] = KEY_L,
    [0x10] = KEY_M, [0x11] = KEY_N, [0x12] = KEY_O, [0x13] = KEY_P,
    [0x14] = KEY_Q, [0x15] = KEY_R, [0x16] = KEY_S, [0x17] = KEY_T,
    [0x18] = KEY_U, [0x19] = KEY_V, [0x1a] = KEY_W, [0x1b] = KEY_X,
    [0x1c] = KEY_Y, [0x1d] = KEY_Z,

    /* ── Number row ── */
    [0x1e] = KEY_1, [0x1f] = KEY_2, [0x20] = KEY_3,
    [0x21] = KEY_4, [0x22] = KEY_5, [0x23] = KEY_6,
    [0x24] = KEY_7, [0x25] = KEY_8, [0x26] = KEY_9,
    [0x27] = KEY_0,

    /* ── Core editing ── */
    [0x28] = KEY_ENTER,
    [0x29] = KEY_ESC,
    [0x2a] = KEY_BACKSPACE,
    [0x2b] = KEY_TAB,
    [0x2c] = KEY_SPACE,

    /* ── Punctuation / symbols ── */
    [0x2d] = KEY_MINUS,
    [0x2e] = KEY_EQUAL,
    [0x2f] = KEY_LEFTBRACE,
    [0x30] = KEY_RIGHTBRACE,
    [0x31] = KEY_BACKSLASH,
    [0x32] = KEY_BACKSLASH,   /* non-US # ~ */
    [0x33] = KEY_SEMICOLON,
    [0x34] = KEY_APOSTROPHE,
    [0x35] = KEY_GRAVE,
    [0x36] = KEY_COMMA,
    [0x37] = KEY_DOT,
    [0x38] = KEY_SLASH,

    /* ── Locks ── */
    [0x39] = KEY_CAPSLOCK,

    /* ── Function row ── */
    [0x3a] = KEY_F1,  [0x3b] = KEY_F2,  [0x3c] = KEY_F3,
    [0x3d] = KEY_F4,  [0x3e] = KEY_F5,  [0x3f] = KEY_F6,
    [0x40] = KEY_F7,  [0x41] = KEY_F8,  [0x42] = KEY_F9,
    [0x43] = KEY_F10, [0x44] = KEY_F11, [0x45] = KEY_F12,

    /* ── System keys ── */
    [0x46] = KEY_SYSRQ,
    [0x47] = KEY_SCROLLLOCK,
    [0x48] = KEY_PAUSE,

    /* ── Navigation cluster ── */
    [0x49] = KEY_INSERT,
    [0x4a] = KEY_HOME,
    [0x4b] = KEY_PAGEUP,
    [0x4c] = KEY_DELETE,
    [0x4d] = KEY_END,
    [0x4e] = KEY_PAGEDOWN,

    /* ── Arrow keys ── */
    [0x4f] = KEY_RIGHT,
    [0x50] = KEY_LEFT,
    [0x51] = KEY_DOWN,
    [0x52] = KEY_UP,

    /* ── Numpad ── */
    [0x53] = KEY_NUMLOCK,
    [0x54] = KEY_KPSLASH,
    [0x55] = KEY_KPASTERISK,
    [0x56] = KEY_KPMINUS,
    [0x57] = KEY_KPPLUS,
    [0x58] = KEY_KPENTER,
    [0x59] = KEY_KP1, [0x5a] = KEY_KP2, [0x5b] = KEY_KP3,
    [0x5c] = KEY_KP4, [0x5d] = KEY_KP5, [0x5e] = KEY_KP6,
    [0x5f] = KEY_KP7, [0x60] = KEY_KP8, [0x61] = KEY_KP9,
    [0x62] = KEY_KP0,
    [0x63] = KEY_KPDOT,

    /* ── Extra / media ── */
    [0x65] = KEY_COMPOSE,
    [0x66] = KEY_POWER,
    [0x67] = KEY_KPEQUAL,

    [0x68] = KEY_F13, [0x69] = KEY_F14, [0x6a] = KEY_F15,
    [0x6b] = KEY_F16, [0x6c] = KEY_F17, [0x6d] = KEY_F18,
    [0x6e] = KEY_F19, [0x6f] = KEY_F20, [0x70] = KEY_F21,
    [0x71] = KEY_F22, [0x72] = KEY_F23, [0x73] = KEY_F24,

    [0x7f] = KEY_MUTE,
    [0x80] = KEY_VOLUMEUP,
    [0x81] = KEY_VOLUMEDOWN,
};

/* ── Guided test sequence ───────────────────────────────────────────────── */

static const unsigned int test_keys[] = {
    KEY_A, KEY_B, KEY_C, KEY_D,
    KEY_Q, KEY_W, KEY_E, KEY_R,
    KEY_1, KEY_2, KEY_3,
    KEY_ENTER, KEY_SPACE
};

static const char * const test_key_names[] = {
    "A", "B", "C", "D",
    "Q", "W", "E", "R",
    "1", "2", "3",
    "ENTER", "SPACE"
};

/* ── LED ────────────────────────────────────────────────────────────────── */

static void usb_kbd_led_complete(struct urb *urb)
{
    struct usb_kbd *kbd = urb->context;

    if (urb->status)
        printk(KERN_WARNING "usb_kbd: LED URB status %d\n", urb->status);
    else
        kbd_dbg("LED URB complete OK (state=0x%02x)\n", kbd->led_state);
}

/*
 * usb_kbd_set_leds() — push led_bits to the keyboard hardware.
 * Safe to call from atomic context (IRQ handler).
 */
static void usb_kbd_set_leds(struct usb_kbd *kbd, unsigned char led_bits)
{
    if (kbd->led_state == led_bits)
        return;

    kbd->led_state = led_bits;
    *kbd->led_buf  = led_bits;

    kbd_dbg("LED → NumLock:%d CapsLock:%d ScrollLock:%d (0x%02x)\n",
            !!(led_bits & HID_LED_NUMLOCK),
            !!(led_bits & HID_LED_CAPSLOCK),
            !!(led_bits & HID_LED_SCROLLLOCK),
            led_bits);

    if (usb_submit_urb(kbd->led, GFP_ATOMIC))
        kbd_dbg("LED URB submit failed (busy?)\n");
}

/* ── Modifier handling ──────────────────────────────────────────────────── */

static void handle_modifiers(struct usb_kbd *kbd,
                             unsigned char new_mods, unsigned char old_mods)
{
    static const unsigned int mods[8] = {
        KEY_LEFTCTRL,  KEY_LEFTSHIFT,  KEY_LEFTALT,  KEY_LEFTMETA,
        KEY_RIGHTCTRL, KEY_RIGHTSHIFT, KEY_RIGHTALT, KEY_RIGHTMETA
    };
    static const char * const mod_names[8] = {
        "LCtrl", "LShift", "LAlt", "LMeta",
        "RCtrl", "RShift", "RAlt", "RMeta"
    };
    int i;

    for (i = 0; i < 8; i++) {
        bool n = !!(new_mods & (1 << i));
        bool o = !!(old_mods & (1 << i));
        if (n != o) {
            input_report_key(kbd->input, mods[i], n);
            kbd_dbg("MOD %s %s\n", n ? "DOWN" : "UP  ", mod_names[i]);
        }
    }
}

/* ── Helper ─────────────────────────────────────────────────────────────── */

static bool key_in_report(unsigned char *rep, unsigned char code)
{
    int i;
    for (i = 2; i < 8; i++)
        if (rep[i] == code)
            return true;
    return false;
}

/* ── Guided test prompt ─────────────────────────────────────────────────── */

static void guided_test_prompt(struct usb_kbd *kbd)
{
    if (kbd->state != TEST_RUNNING)
        return;
    if (kbd->current_test_index < ARRAY_SIZE(test_keys))
        printk(KERN_INFO "usb_kbd: TEST [%d/%zu] Press: %s\n",
               kbd->current_test_index + 1,
               ARRAY_SIZE(test_keys),
               test_key_names[kbd->current_test_index]);
}

/* ── IRQ callback ───────────────────────────────────────────────────────── */

static void usb_kbd_irq(struct urb *urb)
{
    struct usb_kbd *kbd  = urb->context;
    unsigned char  *data = kbd->irq_buf;
    unsigned char   new_leds;
    int i;

    if (urb->status)
        goto resubmit;

    /* Raw report dump (debug only) */
    if (debug)
        printk(KERN_DEBUG
               "usb_kbd: RAW mod=%02x rsv=%02x k=%02x %02x %02x %02x %02x %02x\n",
               data[0], data[1],
               data[2], data[3], data[4], data[5], data[6], data[7]);

    handle_modifiers(kbd, data[0], kbd->old[0]);

    /* Toggle guided test: LCtrl + LAlt */
    if ((data[0] & 0x05) == 0x05 && (kbd->old[0] & 0x05) != 0x05) {
        if (kbd->state == TEST_OFF) {
            kbd->state = TEST_RUNNING;
            kbd->current_test_index = 0;
            printk(KERN_INFO "usb_kbd: GUIDED TEST START (%zu keys)\n",
                   ARRAY_SIZE(test_keys));
            guided_test_prompt(kbd);
        } else {
            kbd->state = TEST_OFF;
            printk(KERN_INFO "usb_kbd: GUIDED TEST STOP\n");
        }
    }

    /* Newly pressed keys */
    for (i = 2; i < 8; i++) {
        unsigned char code = data[i];
        unsigned int keycode;

        if (!code || key_in_report(kbd->old, code))
            continue;

        keycode = scancode_to_keycode[code];
        if (!keycode) {
            kbd_dbg("PRESS scancode=0x%02x (no mapping)\n", code);
            continue;
        }

        input_report_key(kbd->input, keycode, 1);
        kbd_dbg("PRESS scancode=0x%02x keycode=%u\n", code, keycode);

        if (kbd->state == TEST_RUNNING) {
            unsigned int expected = test_keys[kbd->current_test_index];
            if (keycode == expected) {
                printk(KERN_INFO "usb_kbd: OK (%s)\n",
                       test_key_names[kbd->current_test_index]);
                kbd->current_test_index++;
                if (kbd->current_test_index >= ARRAY_SIZE(test_keys)) {
                    printk(KERN_INFO "usb_kbd: TEST DONE - all keys passed\n");
                    kbd->state = TEST_DONE;
                } else {
                    guided_test_prompt(kbd);
                }
            } else {
                printk(KERN_INFO "usb_kbd: WRONG KEY - expected: %s\n",
                       test_key_names[kbd->current_test_index]);
            }
        }
    }

    /* Released keys */
    for (i = 2; i < 8; i++) {
        unsigned char code = kbd->old[i];
        unsigned int keycode;

        if (!code || key_in_report(data, code))
            continue;

        keycode = scancode_to_keycode[code];
        if (keycode) {
            input_report_key(kbd->input, keycode, 0);
            kbd_dbg("RELEASE scancode=0x%02x keycode=%u\n", code, keycode);
        }
    }

    input_sync(kbd->input);
    memcpy(kbd->old, data, 8);

    /* Sync LED state with input layer */
    new_leds = 0;
    if (test_bit(LED_CAPSL,   kbd->input->led)) new_leds |= HID_LED_CAPSLOCK;
    if (test_bit(LED_NUML,    kbd->input->led)) new_leds |= HID_LED_NUMLOCK;
    if (test_bit(LED_SCROLLL, kbd->input->led)) new_leds |= HID_LED_SCROLLLOCK;
    usb_kbd_set_leds(kbd, new_leds);

resubmit:
    if (usb_submit_urb(urb, GFP_ATOMIC))
        printk(KERN_ERR "usb_kbd: failed to resubmit IRQ URB\n");
}

/* ── input_dev event handler — receives EV_LED from input layer ─────────── */

static int usb_kbd_event(struct input_dev *dev, unsigned int type,
                         unsigned int code, int value)
{
    struct usb_kbd *kbd = input_get_drvdata(dev);
    unsigned char leds;

    if (type != EV_LED)
        return -1;

    leds = 0;
    if (test_bit(LED_CAPSL,   dev->led)) leds |= HID_LED_CAPSLOCK;
    if (test_bit(LED_NUML,    dev->led)) leds |= HID_LED_NUMLOCK;
    if (test_bit(LED_SCROLLL, dev->led)) leds |= HID_LED_SCROLLLOCK;

    kbd_dbg("EV_LED code=%u value=%d → leds=0x%02x\n", code, value, leds);
    usb_kbd_set_leds(kbd, leds);
    return 0;
}

/* ── probe ──────────────────────────────────────────────────────────────── */

static int usb_kbd_probe(struct usb_interface *iface,
                         const struct usb_device_id *id)
{
    struct usb_device            *dev = interface_to_usbdev(iface);
    struct usb_host_interface    *interface;
    struct usb_endpoint_descriptor *ep_in  = NULL;
    struct usb_endpoint_descriptor *ep_out = NULL;
    struct usb_kbd               *kbd;
    int pipe, maxp, err, i;

    interface = iface->cur_altsetting;

    usb_control_msg(dev,
        usb_sndctrlpipe(dev, 0),
        0x0B, /* SET_PROTOCOL */
        USB_TYPE_CLASS | USB_RECIP_INTERFACE,
        0, /* BOOT protocol */
        interface->desc.bInterfaceNumber,
        NULL, 0,
        USB_CTRL_SET_TIMEOUT);

    for (i = 0; i < interface->desc.bNumEndpoints; i++) {
        struct usb_endpoint_descriptor *ep =
            &interface->endpoint[i].desc;
        if (usb_endpoint_is_int_in(ep)  && !ep_in)  ep_in  = ep;
        if (usb_endpoint_is_int_out(ep) && !ep_out) ep_out = ep;
    }

    if (!ep_in)
        return -ENODEV;

    kbd = kzalloc(sizeof(*kbd), GFP_KERNEL);
    if (!kbd)
        return -ENOMEM;

    kbd->usbdev   = dev;
    kbd->irq_size = le16_to_cpu(ep_in->wMaxPacketSize);

    kbd->irq_buf = usb_alloc_coherent(dev, kbd->irq_size,
                                      GFP_ATOMIC, &kbd->irq_dma);
    if (!kbd->irq_buf) { err = -ENOMEM; goto err_free_kbd; }

    kbd->led_buf = usb_alloc_coherent(dev, 1, GFP_ATOMIC, &kbd->led_dma);
    if (!kbd->led_buf) { err = -ENOMEM; goto err_free_irq_buf; }

    kbd->irq = usb_alloc_urb(0, GFP_KERNEL);
    if (!kbd->irq) { err = -ENOMEM; goto err_free_led_buf; }

    kbd->led = usb_alloc_urb(0, GFP_KERNEL);
    if (!kbd->led) { err = -ENOMEM; goto err_free_irq_urb; }

    kbd->input = input_allocate_device();
    if (!kbd->input) { err = -ENOMEM; goto err_free_led_urb; }

    kbd->input->name       = "USB Lab Keyboard";
    kbd->input->id.bustype = BUS_USB;
    input_set_drvdata(kbd->input, kbd);

    __set_bit(EV_KEY, kbd->input->evbit);
    __set_bit(EV_SYN, kbd->input->evbit);
    __set_bit(EV_REP, kbd->input->evbit);
    __set_bit(EV_LED, kbd->input->evbit);

    __set_bit(LED_CAPSL,   kbd->input->ledbit);
    __set_bit(LED_NUML,    kbd->input->ledbit);
    __set_bit(LED_SCROLLL, kbd->input->ledbit);

    for (i = 0; i < ARRAY_SIZE(scancode_to_keycode); i++)
        if (scancode_to_keycode[i])
            __set_bit(scancode_to_keycode[i], kbd->input->keybit);

    __set_bit(KEY_LEFTSHIFT,  kbd->input->keybit);
    __set_bit(KEY_RIGHTSHIFT, kbd->input->keybit);
    __set_bit(KEY_LEFTCTRL,   kbd->input->keybit);
    __set_bit(KEY_RIGHTCTRL,  kbd->input->keybit);
    __set_bit(KEY_LEFTALT,    kbd->input->keybit);
    __set_bit(KEY_RIGHTALT,   kbd->input->keybit);
    __set_bit(KEY_LEFTMETA,   kbd->input->keybit);
    __set_bit(KEY_RIGHTMETA,  kbd->input->keybit);

    kbd->input->event = usb_kbd_event;

    err = input_register_device(kbd->input);
    if (err) goto err_free_input;

    /* IRQ IN URB */
    pipe = usb_rcvintpipe(dev, ep_in->bEndpointAddress);
    maxp = usb_maxpacket(dev, pipe);
    usb_fill_int_urb(kbd->irq, dev, pipe, kbd->irq_buf,
                     min(maxp, (int)kbd->irq_size),
                     usb_kbd_irq, kbd, ep_in->bInterval);
    kbd->irq->transfer_dma   = kbd->irq_dma;
    kbd->irq->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

    /* LED OUT URB */
    if (ep_out) {
        /* Keyboard has an interrupt OUT endpoint — use it directly */
        pipe = usb_sndintpipe(dev, ep_out->bEndpointAddress);
        usb_fill_int_urb(kbd->led, dev, pipe, kbd->led_buf, 1,
                         usb_kbd_led_complete, kbd, ep_out->bInterval);
        kbd->led->transfer_dma   = kbd->led_dma;
        kbd->led->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
    } else {
        /*
         * No interrupt OUT — use HID SET_REPORT control transfer.
         * Allocate a setup packet and embed it in the URB.
         */
        kbd->cr = usb_alloc_coherent(dev, sizeof(*kbd->cr), GFP_ATOMIC,
                             &kbd->cr_dma);
        if (!kbd->cr) {
            err = -ENOMEM;
            goto err_unregister_input;
        }

        kbd->cr->bRequestType = USB_TYPE_CLASS | USB_RECIP_INTERFACE;
        kbd->cr->bRequest     = 0x09; /* HID SET_REPORT */
        kbd->cr->wValue       = cpu_to_le16(0x0200); /* Output report, ID 0 */
        kbd->cr->wIndex       = cpu_to_le16(interface->desc.bInterfaceNumber);
        kbd->cr->wLength      = cpu_to_le16(1);

        usb_fill_control_urb(kbd->led, dev, usb_sndctrlpipe(dev, 0),
                             (unsigned char *)kbd->cr, kbd->led_buf, 1,
                             usb_kbd_led_complete, kbd);
        kbd->led->setup_dma = kbd->cr_dma;
        kbd->led->transfer_dma   = kbd->led_dma;
        kbd->led->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
    }

    memset(kbd->old, 0, sizeof(kbd->old));
    kbd->state     = TEST_OFF;
    kbd->led_state = 0xff; /* force first sync */

    usb_set_intfdata(iface, kbd);

    err = usb_submit_urb(kbd->irq, GFP_KERNEL);
    if (err) goto err_unregister_input;

    printk(KERN_INFO "usb_kbd: connected (VID=%04x PID=%04x) debug=%s\n",
       le16_to_cpu(dev->descriptor.idVendor),
       le16_to_cpu(dev->descriptor.idProduct),
       debug ? "on" : "off");
    return 0;

err_unregister_input:
    input_unregister_device(kbd->input);
    kbd->input = NULL;
err_free_input:
    if (kbd->input)
        input_free_device(kbd->input);
err_free_led_urb:
    usb_free_urb(kbd->led);
err_free_irq_urb:
    usb_free_urb(kbd->irq);
err_free_led_buf:
    usb_free_coherent(dev, 1, kbd->led_buf, kbd->led_dma);
err_free_irq_buf:
    usb_free_coherent(dev, kbd->irq_size, kbd->irq_buf, kbd->irq_dma);
err_free_kbd:
    kfree(kbd);
    return err;
}

/* ── disconnect ─────────────────────────────────────────────────────────── */

static void usb_kbd_disconnect(struct usb_interface *iface)
{
    struct usb_kbd *kbd = usb_get_intfdata(iface);

    usb_set_intfdata(iface, NULL);
    if (!kbd)
        return;

    if (kbd->cr)
        usb_free_coherent(kbd->usbdev, sizeof(*kbd->cr), kbd->cr, kbd->cr_dma);

    usb_kill_urb(kbd->irq);
    usb_kill_urb(kbd->led);
    usb_free_urb(kbd->irq);
    usb_free_urb(kbd->led);
    usb_free_coherent(kbd->usbdev, kbd->irq_size, kbd->irq_buf, kbd->irq_dma);
    usb_free_coherent(kbd->usbdev, 1,             kbd->led_buf, kbd->led_dma);
    input_unregister_device(kbd->input);
    kfree(kbd);

    printk(KERN_INFO "usb_kbd: disconnected\n");
}

/* ── USB driver registration ────────────────────────────────────────────── */

static const struct usb_device_id usb_kbd_id_table[] = {
    {
        USB_INTERFACE_INFO(
            USB_CLASS_HID,  /* 0x03 */
            1,              /* Boot Interface Subclass */
            1               /* Keyboard */
        )
    },
    {}
};
MODULE_DEVICE_TABLE(usb, usb_kbd_id_table);

static struct usb_driver usb_kbd_driver = {
    .name       = "usb_kbd_driver",
    .probe      = usb_kbd_probe,
    .disconnect = usb_kbd_disconnect,
    .id_table   = usb_kbd_id_table,
};
module_usb_driver(usb_kbd_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("sakata");
MODULE_DESCRIPTION("USB Keyboard Driver — full keymap, LED control, debug mode");
