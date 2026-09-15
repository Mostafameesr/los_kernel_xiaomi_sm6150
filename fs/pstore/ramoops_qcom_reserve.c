// SPDX-License-Identifier: GPL-2.0
/*
 * Qualcomm downstream ramoops reservation compatibility.
 *
 * Older Qualcomm/Xiaomi SM6150 kernels reserve a persistent 4 MiB region
 * from the early command line parameter "ramoops_memreserve=" and expose it
 * as one ramoops platform device.  Keep that platform-specific reservation
 * separate from the generic ramoops driver so the latter can stay aligned
 * with the newer pstore backport.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/platform_device.h>
#include <linux/pstore_ram.h>

#define QCOM_RAMOOPS_PHYS_ADDR	0xB0000000ULL

static struct ramoops_platform_data qcom_ramoops_data;

static struct platform_device qcom_ramoops_device = {
	.name = "ramoops",
	.id = -1,
	.dev = {
		.platform_data = &qcom_ramoops_data,
	},
};

static int __init qcom_ramoops_memreserve(char *p)
{
	unsigned long size;
	int ret;

	if (!p)
		return 1;

	size = memparse(p, &p) & PAGE_MASK;
	if (!size)
		return 1;

	qcom_ramoops_data.mem_size = size;
	qcom_ramoops_data.mem_address = QCOM_RAMOOPS_PHYS_ADDR;
	qcom_ramoops_data.console_size = size / 2;
	qcom_ramoops_data.pmsg_size = size - qcom_ramoops_data.console_size;

	ret = memblock_reserve(qcom_ramoops_data.mem_address,
			       qcom_ramoops_data.mem_size);
	if (ret) {
		pr_err("ramoops: failed to reserve 0x%lx@0x%llx: %d\n",
		       qcom_ramoops_data.mem_size,
		       (unsigned long long)qcom_ramoops_data.mem_address, ret);
		qcom_ramoops_data.mem_size = 0;
		qcom_ramoops_data.console_size = 0;
		qcom_ramoops_data.pmsg_size = 0;
		return 1;
	}

	pr_info("ramoops: msm_reserve_ramoops_memory addr=%llx,size=%lx\n",
		(unsigned long long)qcom_ramoops_data.mem_address,
		qcom_ramoops_data.mem_size);
	pr_info("ramoops: msm_reserve_ramoops_memory console_size=%lx,pmsg_size=%lx\n",
		qcom_ramoops_data.console_size,
		qcom_ramoops_data.pmsg_size);

	return 0;
}
early_param("ramoops_memreserve", qcom_ramoops_memreserve);

static int __init qcom_register_ramoops_device(void)
{
	int ret;

	if (!qcom_ramoops_data.mem_size)
		return 0;

	ret = platform_device_register(&qcom_ramoops_device);
	if (ret)
		pr_err("ramoops: unable to register Qualcomm ramoops device: %d\n",
		       ret);
	return ret;
}
core_initcall(qcom_register_ramoops_device);
