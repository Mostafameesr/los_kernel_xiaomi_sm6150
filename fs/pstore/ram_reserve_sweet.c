// SPDX-License-Identifier: GPL-2.0
/*
 * Xiaomi Sweet ramoops early reservation helper.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kmsg_dump.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/platform_device.h>
#include <linux/pstore_ram.h>
#include <linux/string.h>

#ifdef CONFIG_MACH_XIAOMI_SWEET

#define SWEET_RAMOOPS_BASE	0xB0000000ULL

static struct ramoops_platform_data sweet_ramoops_data;

static struct platform_device sweet_ramoops_dev = {
	.name = "ramoops",
	.id = -1,
	.dev = {
		.platform_data = &sweet_ramoops_data,
	},
};

static int __init sweet_ramoops_memreserve(char *p)
{
	unsigned long size;
	int ret;

	if (!p)
		return 1;

	/* Be safe if the bootloader and CONFIG_CMDLINE both provide it. */
	if (sweet_ramoops_data.mem_size)
		return 0;

	size = memparse(p, &p) & PAGE_MASK;
	if (!size)
		return 1;

	sweet_ramoops_data.mem_size = size;
	sweet_ramoops_data.mem_address = SWEET_RAMOOPS_BASE;
	sweet_ramoops_data.record_size = size / 2;
	sweet_ramoops_data.console_size = size / 2;
	sweet_ramoops_data.max_reason = KMSG_DUMP_RESTART;

	ret = memblock_reserve(sweet_ramoops_data.mem_address,
			       sweet_ramoops_data.mem_size);
	if (ret) {
		pr_err("ramoops: Sweet early reserve failed at %pa size 0x%lx: %d\n",
		       &sweet_ramoops_data.mem_address,
		       sweet_ramoops_data.mem_size, ret);
		memset(&sweet_ramoops_data, 0, sizeof(sweet_ramoops_data));
		return 1;
	}

	pr_info("ramoops: Sweet early reserved 0x%lx@%pa (record=0x%lx console=0x%lx max_reason=%d)\n",
		sweet_ramoops_data.mem_size,
		&sweet_ramoops_data.mem_address,
		sweet_ramoops_data.record_size,
		sweet_ramoops_data.console_size,
		sweet_ramoops_data.max_reason);

	return 0;
}
early_param("ramoops_memreserve", sweet_ramoops_memreserve);

static int __init sweet_register_ramoops_device(void)
{
	if (!sweet_ramoops_data.mem_size)
		return 0;

	return platform_device_register(&sweet_ramoops_dev);
}
core_initcall(sweet_register_ramoops_device);

#endif /* CONFIG_MACH_XIAOMI_SWEET */
