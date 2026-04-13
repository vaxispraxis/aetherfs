#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

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



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0xd710adbf, "__kmalloc_noprof" },
	{ 0xfbe7861b, "memmove" },
	{ 0x494c552b, "_find_first_zero_bit" },
	{ 0xfbe7861b, "memcpy" },
	{ 0xcb8b6ec6, "kfree" },
	{ 0xde338d9a, "_raw_spin_lock" },
	{ 0xd272d446, "__fentry__" },
	{ 0xe8213e80, "_printk" },
	{ 0xbd03ed67, "__ref_stack_chk_guard" },
	{ 0xd272d446, "__stack_chk_fail" },
	{ 0x12c25e2e, "bdev_getblk" },
	{ 0x90a48d82, "__ubsan_handle_out_of_bounds" },
	{ 0x684812dd, "__bitmap_set" },
	{ 0xab7a2e8b, "__brelse" },
	{ 0xe25a916e, "sync_dirty_buffer" },
	{ 0xbd03ed67, "random_kmalloc_seed" },
	{ 0x9e3a8e47, "_raw_write_lock" },
	{ 0xe981281f, "kmem_cache_free" },
	{ 0x9e3a8e47, "_raw_read_unlock" },
	{ 0x9e3a8e47, "_raw_write_unlock" },
	{ 0x12c25e2e, "__bread_gfp" },
	{ 0xd272d446, "__x86_return_thunk" },
	{ 0xdd0e0f53, "kmem_cache_alloc_noprof" },
	{ 0x7fd710be, "__kmem_cache_create_args" },
	{ 0x684812dd, "__bitmap_clear" },
	{ 0x82fd7238, "__ubsan_handle_shift_out_of_bounds" },
	{ 0x86632fd6, "_find_next_zero_bit" },
	{ 0x957c6137, "__kmalloc_cache_noprof" },
	{ 0x9e3a8e47, "_raw_read_lock" },
	{ 0xce03c8c3, "__mark_inode_dirty" },
	{ 0xde338d9a, "_raw_spin_unlock" },
	{ 0x78339609, "kmalloc_caches" },
	{ 0x6da5974d, "kmem_cache_destroy" },
	{ 0x984622ae, "module_layout" },
};

static const u32 ____version_ext_crcs[]
__used __section("__version_ext_crcs") = {
	0xd710adbf,
	0xfbe7861b,
	0x494c552b,
	0xfbe7861b,
	0xcb8b6ec6,
	0xde338d9a,
	0xd272d446,
	0xe8213e80,
	0xbd03ed67,
	0xd272d446,
	0x12c25e2e,
	0x90a48d82,
	0x684812dd,
	0xab7a2e8b,
	0xe25a916e,
	0xbd03ed67,
	0x9e3a8e47,
	0xe981281f,
	0x9e3a8e47,
	0x9e3a8e47,
	0x12c25e2e,
	0xd272d446,
	0xdd0e0f53,
	0x7fd710be,
	0x684812dd,
	0x82fd7238,
	0x86632fd6,
	0x957c6137,
	0x9e3a8e47,
	0xce03c8c3,
	0xde338d9a,
	0x78339609,
	0x6da5974d,
	0x984622ae,
};
static const char ____version_ext_names[]
__used __section("__version_ext_names") =
	"__kmalloc_noprof\0"
	"memmove\0"
	"_find_first_zero_bit\0"
	"memcpy\0"
	"kfree\0"
	"_raw_spin_lock\0"
	"__fentry__\0"
	"_printk\0"
	"__ref_stack_chk_guard\0"
	"__stack_chk_fail\0"
	"bdev_getblk\0"
	"__ubsan_handle_out_of_bounds\0"
	"__bitmap_set\0"
	"__brelse\0"
	"sync_dirty_buffer\0"
	"random_kmalloc_seed\0"
	"_raw_write_lock\0"
	"kmem_cache_free\0"
	"_raw_read_unlock\0"
	"_raw_write_unlock\0"
	"__bread_gfp\0"
	"__x86_return_thunk\0"
	"kmem_cache_alloc_noprof\0"
	"__kmem_cache_create_args\0"
	"__bitmap_clear\0"
	"__ubsan_handle_shift_out_of_bounds\0"
	"_find_next_zero_bit\0"
	"__kmalloc_cache_noprof\0"
	"_raw_read_lock\0"
	"__mark_inode_dirty\0"
	"_raw_spin_unlock\0"
	"kmalloc_caches\0"
	"kmem_cache_destroy\0"
	"module_layout\0"
;

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "1804B876CEA23044E0A97E8");
