// SPDX-License-Identifier: GPL-2.0
/* Environment smoke test only; this is not the queue driver project. */
#include <linux/init.h>
#include <linux/module.h>

static int __init envcheck_init(void)
{
	pr_info("envcheck: ARM64 driver environment loaded successfully\n");
	return 0;
}

static void __exit envcheck_exit(void)
{
	pr_info("envcheck: module unloaded successfully\n");
}

module_init(envcheck_init);
module_exit(envcheck_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Minimal module for verifying the QEMU ARM64 build environment");
