#include <linux/module.h>
#define INCLUDE_VERMAGIC
#include <linux/build-salt.h>
#include <linux/elfnote-lto.h>
#include <linux/export-internal.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

#ifdef CONFIG_UNWINDER_ORC
#include <asm/orc_header.h>
ORC_HEADER;
#endif

BUILD_SALT;
BUILD_LTO_INFO;

MODULE_INFO(vermagic, VERMAGIC_STRING);
MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

#ifdef CONFIG_RETPOLINE
MODULE_INFO(retpoline, "Y");
#endif



static const char ____versions[]
__used __section("__versions") =
	"\x18\x00\x00\x00\xb7\xe5\x8c\xa7"
	"usb_kill_urb\0\0\0\0"
	"\x18\x00\x00\x00\xb0\x4c\x1f\x93"
	"usb_free_urb\0\0\0\0"
	"\x1c\x00\x00\x00\x2c\xe2\xee\xc5"
	"usb_free_coherent\0\0\0"
	"\x20\x00\x00\x00\xe2\xe3\x83\xfd"
	"input_unregister_device\0"
	"\x10\x00\x00\x00\xba\x0c\x7a\x03"
	"kfree\0\0\0"
	"\x10\x00\x00\x00\x7e\x3a\x2c\x12"
	"_printk\0"
	"\x14\x00\x00\x00\x06\x35\x74\xec"
	"input_event\0"
	"\x18\x00\x00\x00\xe6\x7b\x4e\x81"
	"usb_submit_urb\0\0"
	"\x18\x00\x00\x00\x9e\x63\xf0\x3c"
	"usb_deregister\0\0"
	"\x1c\x00\x00\x00\x63\xa5\x03\x4c"
	"random_kmalloc_seed\0"
	"\x18\x00\x00\x00\x46\xe9\x04\x10"
	"kmalloc_caches\0\0"
	"\x18\x00\x00\x00\x04\xf1\x55\xbf"
	"kmalloc_trace\0\0\0"
	"\x1c\x00\x00\x00\x9e\xa4\xf3\xf0"
	"usb_alloc_coherent\0\0"
	"\x18\x00\x00\x00\xe2\x27\xf4\x57"
	"usb_alloc_urb\0\0\0"
	"\x20\x00\x00\x00\xe7\xad\xb9\x77"
	"input_allocate_device\0\0\0"
	"\x20\x00\x00\x00\x18\x94\xb6\x79"
	"input_register_device\0\0\0"
	"\x1c\x00\x00\x00\x95\x06\x08\xcd"
	"input_free_device\0\0\0"
	"\x28\x00\x00\x00\xb3\x1c\xa2\x87"
	"__ubsan_handle_out_of_bounds\0\0\0\0"
	"\x14\x00\x00\x00\xbb\x6d\xfb\xbd"
	"__fentry__\0\0"
	"\x1c\x00\x00\x00\x47\x30\x5f\x80"
	"usb_register_driver\0"
	"\x1c\x00\x00\x00\xca\x39\x82\x5b"
	"__x86_return_thunk\0\0"
	"\x18\x00\x00\x00\x2e\x9f\xe7\xf6"
	"module_layout\0\0\0"
	"\x00\x00\x00\x00\x00\x00\x00\x00";

MODULE_INFO(depends, "");

MODULE_ALIAS("usb:v04D9p1603d*dc*dsc*dp*ic*isc*ip*in*");

MODULE_INFO(srcversion, "8FF4CD4D53A209F76A86277");
