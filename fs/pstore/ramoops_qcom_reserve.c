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
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/platform_device.h>
#include <linux/pstore_ram.h>

#define QCOM_RAMOOPS_PHYS_ADDR	0xB0000000ULL
#define QCOM_RAMOOPS_SIG	0x43474244U

struct qcom_ramoops_raw_header {
	u32 sig;
	u32 start;
	u32 size;
};

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

static void __init qcom_ramoops_dump_raw_header(phys_addr_t addr,
					 const char *name)
{
	struct qcom_ramoops_raw_header *hdr;
	void *vaddr;

	vaddr = memremap(addr, sizeof(*hdr), MEMREMAP_WB);
	if (!vaddr) {
		pr_err("ramoops_diag: %s memremap failed at 0x%llx\n",
		       name, (unsigned long long)addr);
		return;
	}

	hdr = vaddr;
	pr_info("ramoops_diag: %s addr=0x%llx raw_sig=0x%08x expected=0x%08x raw_start=0x%08x raw_size=0x%08x\n",
		name, (unsigned long long)addr,
		READ_ONCE(hdr->sig), QCOM_RAMOOPS_SIG,
		READ_ONCE(hdr->start), READ_ONCE(hdr->size));

	memunmap(vaddr);
}

static int __init qcom_register_ramoops_device(void)
{
	int ret;

	if (!qcom_ramoops_data.mem_size)
		return 0;

	/*
	 * Diagnostic only: sample both persistent headers before the generic
	 * ramoops driver maps, validates, saves or rewinds either zone.  This
	 * distinguishes firmware/boot-time RAM loss from later pstore cleanup.
	 */
	qcom_ramoops_dump_raw_header(qcom_ramoops_data.mem_address, "console");
	qcom_ramoops_dump_raw_header(qcom_ramoops_data.mem_address +
				     qcom_ramoops_data.console_size, "pmsg");

	ret = platform_device_register(&qcom_ramoops_device);
	if (ret)
		pr_err("ramoops: unable to register Qualcomm ramoops device: %d\n",
		       ret);
	return ret;
}
core_initcall(qcom_register_ramoops_device);
