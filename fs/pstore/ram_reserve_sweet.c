// SPDX-License-Identifier: GPL-2.0
/*
 * Xiaomi Sweet ramoops early reservation helper.
 */

#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/kmsg_dump.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/platform_device.h>
#include <linux/pstore_ram.h>
#include <linux/string.h>

#ifdef CONFIG_MACH_XIAOMI_SWEET

#define SWEET_RAMOOPS_BASE	0xB0000000ULL
#define SWEET_PRZ_SIG		0x43474244U /* DBGC */
#define SWEET_PRZ_HDR_SIZE	12U

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

static void __init sweet_ramoops_log_raw_header(const char *name,
					 phys_addr_t addr, size_t zone_size)
{
	void __iomem *base;
	u32 sig;
	u32 start;
	u32 size;
	bool valid;

	base = ioremap_wc(addr, SWEET_PRZ_HDR_SIZE);
	if (!base) {
		pr_err("ramoops: Sweet pre-probe %s header map failed at %pa\n",
		       name, &addr);
		return;
	}

	sig = readl((u8 __iomem *)base + 0);
	start = readl((u8 __iomem *)base + 4);
	size = readl((u8 __iomem *)base + 8);
	valid = sig == SWEET_PRZ_SIG &&
		size <= zone_size - SWEET_PRZ_HDR_SIZE && start <= size;

	pr_info("ramoops: Sweet pre-probe %s header @%pa sig=0x%08x start=%u size=%u valid=%u\n",
		name, &addr, sig, start, size, valid);

	iounmap(base);
}

static int __init sweet_register_ramoops_device(void)
{
	phys_addr_t console_addr;

	if (!sweet_ramoops_data.mem_size)
		return 0;

	/*
	 * Inspect the previous boot's raw PRZ headers before ramoops probes.
	 * This runs before persistent_ram_post_init() can save/zap them and
	 * before userspace can mount, collect, or unlink anything in pstore.
	 */
	sweet_ramoops_log_raw_header("dump",
		sweet_ramoops_data.mem_address,
		sweet_ramoops_data.record_size);

	console_addr = sweet_ramoops_data.mem_address +
		       sweet_ramoops_data.record_size;
	sweet_ramoops_log_raw_header("console", console_addr,
		sweet_ramoops_data.console_size);

	return platform_device_register(&sweet_ramoops_dev);
}
core_initcall(sweet_register_ramoops_device);

#endif /* CONFIG_MACH_XIAOMI_SWEET */
