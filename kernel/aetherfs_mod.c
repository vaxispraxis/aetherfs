#include "aetherfs.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("AetherFS Team");
MODULE_DESCRIPTION("AetherFS - Next-generation filesystem for 8 EiB storage");
MODULE_VERSION("1.0.0");

int aetherfs_init(void)
{
	pr_info("AetherFS: initializing v1.0.0\n");
	return 0;
}

void aetherfs_exit(void)
{
	pr_info("AetherFS: module unloaded\n");
}

module_init(aetherfs_init);
module_exit(aetherfs_exit);