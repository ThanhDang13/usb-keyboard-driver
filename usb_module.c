#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/input.h>

#define USB_KBD_VENDOR_ID  0x04d9
#define USB_KBD_PRODUCT_ID 0x1603

struct usb_kbd {
    struct usb_device *usbdev;
    struct urb *irq;
    unsigned char *irq_buf;
    dma_addr_t irq_dma;
    size_t irq_size;

    struct input_dev *input; // input subsystem device
};

static const unsigned char scancode_to_keycode[256] = {
    [0x04] = KEY_A, [0x05] = KEY_B, [0x06] = KEY_C, [0x07] = KEY_D,
    [0x08] = KEY_E, [0x09] = KEY_F, [0x0a] = KEY_G, [0x0b] = KEY_H,
    [0x0c] = KEY_I, [0x0d] = KEY_J, [0x0e] = KEY_K, [0x0f] = KEY_L,
    [0x10] = KEY_M, [0x11] = KEY_N, [0x12] = KEY_O, [0x13] = KEY_P,
    [0x14] = KEY_Q, [0x15] = KEY_R, [0x16] = KEY_S, [0x17] = KEY_T,
    [0x18] = KEY_U, [0x19] = KEY_V, [0x1a] = KEY_W, [0x1b] = KEY_X,
    [0x1c] = KEY_Y, [0x1d] = KEY_Z,

    [0x1e] = KEY_1, [0x1f] = KEY_2, [0x20] = KEY_3, [0x21] = KEY_4,
    [0x22] = KEY_5, [0x23] = KEY_6, [0x24] = KEY_7, [0x25] = KEY_8,
    [0x26] = KEY_9, [0x27] = KEY_0,

    [0x2c] = KEY_SPACE,
};

static void usb_kbd_irq(struct urb *urb)
{
    struct usb_kbd *kbd = urb->context;
    int i;
    unsigned char code;
    bool shift;

    if (urb->status)
        goto resubmit;

    shift = kbd->irq_buf[0] & 0x22; // left/right shift

    for (i = 2; i < 8; i++) {
        code = kbd->irq_buf[i];
        if (!code)
            continue;

        if (code >= ARRAY_SIZE(scancode_to_keycode))
            continue;

        input_report_key(kbd->input, scancode_to_keycode[code], 1); // press
        input_report_key(kbd->input, scancode_to_keycode[code], 0); // release
        input_sync(kbd->input);
    }

resubmit:
    usb_submit_urb(urb, GFP_ATOMIC);
}

static int usb_kbd_probe(struct usb_interface *iface,
                         const struct usb_device_id *id)
{
    struct usb_device *dev = interface_to_usbdev(iface);
    struct usb_host_interface *interface;
    struct usb_endpoint_descriptor *endpoint;
    struct usb_kbd *kbd;
    int pipe, maxp, err;

    interface = iface->cur_altsetting;
    endpoint = &interface->endpoint[0].desc;

    kbd = kzalloc(sizeof(struct usb_kbd), GFP_KERNEL);
    if (!kbd)
        return -ENOMEM;

    kbd->usbdev = dev;
    kbd->irq_size = le16_to_cpu(endpoint->wMaxPacketSize);

    kbd->irq_buf = usb_alloc_coherent(dev, kbd->irq_size,
                                      GFP_ATOMIC, &kbd->irq_dma);
    if (!kbd->irq_buf) {
        kfree(kbd);
        return -ENOMEM;
    }

    kbd->irq = usb_alloc_urb(0, GFP_KERNEL);
    if (!kbd->irq) {
        usb_free_coherent(dev, kbd->irq_size, kbd->irq_buf, kbd->irq_dma);
        kfree(kbd);
        return -ENOMEM;
    }

    /* Input device setup */
    kbd->input = input_allocate_device();
    if (!kbd->input) {
        usb_free_urb(kbd->irq);
        usb_free_coherent(dev, kbd->irq_size, kbd->irq_buf, kbd->irq_dma);
        kfree(kbd);
        return -ENOMEM;
    }

    kbd->input->name = "USB Lab Keyboard";
    kbd->input->phys = "usb/input0";
    kbd->input->id.bustype = BUS_USB;
    kbd->input->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_REP);

    for (int i = 0; i < ARRAY_SIZE(scancode_to_keycode); i++)
        if (scancode_to_keycode[i])
            set_bit(scancode_to_keycode[i], kbd->input->keybit);

    err = input_register_device(kbd->input);
    if (err) {
        input_free_device(kbd->input);
        usb_free_urb(kbd->irq);
        usb_free_coherent(dev, kbd->irq_size, kbd->irq_buf, kbd->irq_dma);
        kfree(kbd);
        return err;
    }

    pipe = usb_rcvintpipe(dev, endpoint->bEndpointAddress);
    maxp = usb_maxpacket(dev, pipe);

    usb_fill_int_urb(kbd->irq, dev, pipe, kbd->irq_buf,
                     (maxp > kbd->irq_size ? kbd->irq_size : maxp),
                     usb_kbd_irq, kbd, endpoint->bInterval);

    kbd->irq->transfer_dma = kbd->irq_dma;
    kbd->irq->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

    usb_set_intfdata(iface, kbd);
    usb_submit_urb(kbd->irq, GFP_KERNEL);

    printk(KERN_INFO "usb_kbd: keyboard connected\n");
    return 0;
}

static void usb_kbd_disconnect(struct usb_interface *iface)
{
    struct usb_kbd *kbd = usb_get_intfdata(iface);

    usb_set_intfdata(iface, NULL);
    if (!kbd)
        return;

    usb_kill_urb(kbd->irq);
    usb_free_urb(kbd->irq);
    usb_free_coherent(kbd->usbdev, kbd->irq_size,
                      kbd->irq_buf, kbd->irq_dma);

    input_unregister_device(kbd->input);

    kfree(kbd);
    printk(KERN_INFO "usb_kbd: keyboard disconnected\n");
}

static const struct usb_device_id usb_kbd_id_table[] = {
    { USB_DEVICE(USB_KBD_VENDOR_ID, USB_KBD_PRODUCT_ID) },
    {}
};

MODULE_DEVICE_TABLE(usb, usb_kbd_id_table);

static struct usb_driver usb_kbd_driver = {
    .name = "usb_kbd_driver",
    .probe = usb_kbd_probe,
    .disconnect = usb_kbd_disconnect,
    .id_table = usb_kbd_id_table,
};

module_usb_driver(usb_kbd_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("sakata");
MODULE_DESCRIPTION("USB Keyboard Driver with input subsystem");