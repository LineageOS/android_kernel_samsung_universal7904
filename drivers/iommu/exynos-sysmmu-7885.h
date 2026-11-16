/*
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Data structure definition for Exynos IOMMU driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef _EXYNOS_SYSMMU_7885_H
#define _EXYNOS_SYSMMU_7885_H
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/iommu.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>

typedef u32 sysmmu_iova_t;
typedef u32 sysmmu_pte_t;

#define IOVM_NUM_PAGES(vmsize) (vmsize / PAGE_SIZE)
#define IOVM_BITMAP_SIZE(vmsize) \
		((IOVM_NUM_PAGES(vmsize) + BITS_PER_BYTE) / BITS_PER_BYTE)

#define SECT_ORDER 20
#define LPAGE_ORDER 16
#define SPAGE_ORDER 12

#define SECT_SIZE (1 << SECT_ORDER)
#define LPAGE_SIZE (1 << LPAGE_ORDER)
#define SPAGE_SIZE (1 << SPAGE_ORDER)

#define SECT_MASK (~(SECT_SIZE - 1))
#define LPAGE_MASK (~(LPAGE_SIZE - 1))
#define SPAGE_MASK (~(SPAGE_SIZE - 1))

#define SECT_ENT_MASK	~((SECT_SIZE >> PG_ENT_SHIFT) - 1)
#define LPAGE_ENT_MASK	~((LPAGE_SIZE >> PG_ENT_SHIFT) - 1)
#define SPAGE_ENT_MASK	~((SPAGE_SIZE >> PG_ENT_SHIFT) - 1)

#define SPAGES_PER_LPAGE	(LPAGE_SIZE / SPAGE_SIZE)

#define PGBASE_TO_PHYS(pgent)	((phys_addr_t)(pgent) << PG_ENT_SHIFT)

#define NUM_LV1ENTRIES	4096
#define NUM_LV2ENTRIES (SECT_SIZE / SPAGE_SIZE)
#define LV2TABLE_SIZE (NUM_LV2ENTRIES * sizeof(sysmmu_pte_t))

#define lv1ent_offset(iova) ((iova) >> SECT_ORDER)
#define lv2ent_offset(iova) ((iova & ~SECT_MASK) >> SPAGE_ORDER)

#define PG_ENT_SHIFT	4
#define lv1ent_fault(sent)	((*(sent) & 7) == 0)
#define lv1ent_page(sent)	((*(sent) & 7) == 1)

#define FLPD_FLAG_MASK	7
#define SLPD_FLAG_MASK	3

#define SECT_FLAG	2
#define SLPD_FLAG	1

#define LPAGE_FLAG	1
#define SPAGE_FLAG	2

#define ENT_TO_PHYS(ent) (phys_addr_t)(*(ent))
#define section_phys(sent) PGBASE_TO_PHYS(ENT_TO_PHYS(sent) & SECT_ENT_MASK)
#define section_offs(iova) ((iova) & (SECT_SIZE - 1))
#define lpage_phys(pent) PGBASE_TO_PHYS(ENT_TO_PHYS(pent) & LPAGE_ENT_MASK)
#define lpage_offs(iova) ((iova) & (LPAGE_SIZE - 1))
#define spage_phys(pent) PGBASE_TO_PHYS(ENT_TO_PHYS(pent) & SPAGE_ENT_MASK)
#define spage_offs(iova) ((iova) & (SPAGE_SIZE - 1))

#define lv1ent_section(sent) ((*(sent) & FLPD_FLAG_MASK) == SECT_FLAG)
#define lv2table_base(sent)	((phys_addr_t)(*(sent) & ~0x3F) << PG_ENT_SHIFT)
#define lv2ent_fault(pent) ((*(pent) & SLPD_FLAG_MASK) == 0)
#define lv2ent_small(pent) ((*(pent) & SLPD_FLAG_MASK) == SPAGE_FLAG)
#define lv2ent_large(pent) ((*(pent) & SLPD_FLAG_MASK) == LPAGE_FLAG)

#define mk_lv1ent_sect(pa) ((sysmmu_pte_t) ((pa) >> PG_ENT_SHIFT) | 2)
#define mk_lv1ent_page(pa) ((sysmmu_pte_t) ((pa) >> PG_ENT_SHIFT) | 1)
#define mk_lv2ent_lpage(pa) ((sysmmu_pte_t) ((pa) >> PG_ENT_SHIFT) | 1)
#define mk_lv2ent_spage(pa) ((sysmmu_pte_t) ((pa) >> PG_ENT_SHIFT) | 2)
#define set_lv1ent_shareable(sent) (*(sent) |= (1 << 6))
#define set_lv2ent_shareable(sent) (*(sent) |= (1 << 4))
#define mk_lv2ent_pfnmap(pent) (*(pent) |= (1 << 5)) /* unused field */
#define lv2ent_pfnmap(pent) ((*(pent) & (1 << 5)) == (1 << 5))

#define SYSMMU_BLOCK_POLLING_COUNT 4096

#define REG_MMU_CTRL		0x000
#define REG_MMU_CFG		0x004
#define REG_MMU_STATUS		0x008
#define REG_MMU_VERSION		0x034

#define CTRL_ENABLE	0x5
#define CTRL_BLOCK	0x7
#define CTRL_DISABLE	0x0
#define CTRL_BLOCK_DISABLE 0x3

#define CFG_MASK	0x301F1F8C	/* Bit 29-28, 20-16, 12-7, 3-2 */
#define CFG_ACGEN	(1 << 24)
#define CFG_FLPDCACHE	(1 << 20)
#define CFG_QOS_OVRRIDE (1 << 11)
#define CFG_QOS(n)      (((n) & 0xF) << 7)

#define MMU_WAY_CFG_MASK_PREFETCH	(1 << 1)
#define MMU_WAY_CFG_MASK_PREFETCH_DIR	(3 << 2)
#define MMU_WAY_CFG_MASK_MATCH_METHOD	(1 << 4)
#define MMU_WAY_CFG_MASK_FETCH_SIZE	(7 << 5)
#define MMU_WAY_CFG_MASK_TARGET_CH	(3 << 8)

#define MMU_WAY_CFG_ID_MATCHING		(1 << 4)
#define MMU_WAY_CFG_ADDR_MATCHING	(0 << 4)
#define MMU_WAY_CFG_PRIVATE_ENABLE	(1 << 0)

#define MMU_PUBLIC_WAY_MASK	(MMU_WAY_CFG_MASK_PREFETCH |	\
		MMU_WAY_CFG_MASK_PREFETCH_DIR | MMU_WAY_CFG_MASK_FETCH_SIZE)
#define MMU_PRIVATE_WAY_MASK	(MMU_PUBLIC_WAY_MASK |		\
		MMU_WAY_CFG_MASK_MATCH_METHOD | MMU_WAY_CFG_MASK_TARGET_CH)

#define REG_PT_BASE_PPN		0x00C
/* ABOX SYSMMU page table ID #1 (vendor 4.4 exynos-iommu.c) */
#define REG_ABOX_PT1_BASE_PPN	0x1020
#define ABOX_PT1_FLUSH		((1 << 4) | 0x1)
#define REG_MMU_FLUSH		0x010
#define REG_MMU_FLUSH_ENTRY	0x014
#define REG_MMU_FLUSH_RANGE	0x018
#define REG_FLUSH_RANGE_START	0x020
#define REG_FLUSH_RANGE_END	0x024
#define REG_MMU_CAPA		0x030
#define REG_MMU_CAPA_1		0x038
#define REG_INT_STATUS		0x060
#define REG_INT_CLEAR		0x064

#define REG_FAULT_AR_ADDR	0x070
#define REG_FAULT_AR_TRANS_INFO	0x078
#define REG_FAULT_AW_ADDR	0x080
#define REG_FAULT_AW_TRANS_INFO	0x088

#define REG_L2TLB_CFG		0x200

/* For SysMMU v7.x */
#define REG_MMU_CAPA0_V7	0x870
#define REG_MMU_CAPA1_V7	0x874
#define REG_PUBLIC_WAY_CFG	0x120
#define REG_PRIVATE_WAY_CFG(n)		(0x200 + ((n) * 0x10))
#define REG_PRIVATE_ADDR_START(n)	(0x204 + ((n) * 0x10))
#define REG_PRIVATE_ADDR_END(n)		(0x208 + ((n) * 0x10))
#define REG_PRIVATE_ID(n)		(0x20C + ((n) * 0x10))
#define REG_FAULT_ADDR		0x070
#define REG_FAULT_TRANS_INFO	0x078
#define REG_TLB_READ		0x1000
#define REG_TLB_VPN		0x1004
#define REG_TLB_PPN		0x1008
#define REG_TLB_ATTR		0x100C
#define REG_SBB_READ		0x1100
#define REG_SBB_VPN		0x1104
#define REG_SBB_LINK		0x1108
#define REG_SBB_ATTR		0x110C

/* For SysMMU v7.1 */
#define MMU_CAPA1_NUM_TLB(reg)	((reg >> 4) & 0xFF)
#define MMU_CAPA1_NUM_PORT(reg)	((reg) & 0xF)
#define MMU_TLB_INFO(n)		(0x2000 + ((n) * 0x20))
#define MMU_CAPA1_NUM_TLB_SET(reg)	((reg >> 16) & 0xFF)
#define MMU_CAPA1_NUM_TLB_WAY(reg)	((reg) & 0xFF)
#define REG_MMU_TLB_CFG(n)		(0x2000 + ((n) * 0x20) + 0x4)
#define REG_MMU_TLB_MATCH_CFG(n)	(0x2000 + ((n) * 0x20) + 0x8)
#define REG_MMU_TLB_MATCH_SVA(n)	(0x2000 + ((n) * 0x20) + 0xC)
#define REG_MMU_TLB_MATCH_EVA(n)	(0x2000 + ((n) * 0x20) + 0x10)
#define REG_MMU_TLB_MATCH_ID(n)		(0x2000 + ((n) * 0x20) + 0x14)
#define REG_CAPA1_TLB_READ		0x8000
#define REG_CAPA1_TLB_VPN		0x8004
#define REG_CAPA1_TLB_PPN		0x8008
#define REG_CAPA1_TLB_ATTR		0x800C
#define REG_CAPA1_SBB_READ		0x8020
#define REG_CAPA1_SBB_VPN		0x8024
#define REG_CAPA1_SBB_LINK		0x8028
#define REG_CAPA1_SBB_ATTR		0x802C
#define REG_SLOT_RSV(n)			(0x4000 + ((n) * 0x20))
#define MMU_CAPA1_SET_TLB_READ_ENTRY(tid, set, way, line)		\
			((set) | ((way) << 8) | ((line) << 16) | ((tid) << 20))
#define MMU_TLB_CFG_MASK(reg)		((reg) & ((0x7 << 5) | (0x3 << 2) | (0x1 << 1)))
#define MMU_TLB_MATCH_CFG_MASK(reg)	((reg) & ((0xFFFF << 16) | (0x3 << 8)))

#define MMU_CAPA_NUM_SBB_ENTRY(reg)	((reg >> 12) & 0xF)
#define MMU_CAPA_NUM_TLB_SET(reg)	((reg >> 8) & 0xF)
#define MMU_CAPA_NUM_TLB_WAY(reg)	((reg) & 0xFF)
#define MMU_SET_TLB_READ_ENTRY(set, way, line)		\
			((set) | ((way) << 8) | ((line) << 16))
#define MMU_TLB_ENTRY_VALID(reg)	((reg) >> 28)
#define MMU_SBB_ENTRY_VALID(reg)	((reg) >> 28)

#define MMU_FAULT_INFO_READ_REQUEST	0
#define MMU_FAULT_INFO_WRITE_REQUEST	1
#define MMU_IS_READ_FAULT(reg)		\
		((((reg) >> 20) & 0x1) == MMU_FAULT_INFO_READ_REQUEST)

#define MMU_HAVE_PB(reg)	(!!((reg >> 20) & 0xF))
#define MMU_IS_TLB_CONFIGURABLE(reg)	(!!((reg >> 16) & 0xFF))

#define MMU_MASK_LINE_SIZE	0x7
#define MMU_DEFAULT_LINE_SIZE	(0x2 << 4)

#define MMU_MAJ_VER(val)	((val) >> 11)
#define MMU_MIN_VER(val)	((val >> 4) & 0x7F)
#define MMU_REV_VER(val)	((val) & 0xF)
#define MMU_RAW_VER(reg)	(((reg) >> 17) & 0x7FFF) /* upper 15 bits */

#define MAKE_MMU_VER(maj, min)	((((maj) & 0xF) << 11) | \
					(((min) & 0x7F) << 4))

#define DEFAULT_QOS_VALUE	-1

#define SYSMMU_FAULT_BITS       4
#define SYSMMU_FAULT_SHIFT      16
#define SYSMMU_FAULT_MASK       ((1 << SYSMMU_FAULT_BITS) - 1)
#define SYSMMU_FAULT_FLAG(id) (((id) & SYSMMU_FAULT_MASK) << SYSMMU_FAULT_SHIFT)
#define SYSMMU_FAULT_ID(fg)   (((fg) >> SYSMMU_FAULT_SHIFT) & SYSMMU_FAULT_MASK)

#define SYSMMU_FAULT_PTW_ACCESS   0
#define SYSMMU_FAULT_PAGE_FAULT   1
#define SYSMMU_FAULT_TLB_MULTIHIT 2
#define SYSMMU_FAULT_ACCESS       3
#define SYSMMU_FAULT_SECURITY     4
#define SYSMMU_FAULT_UNKNOWN      5

#define SYSMMU_FAULTS_NUM         (SYSMMU_FAULT_UNKNOWN + 1)
struct tlb_priv_addr {
	unsigned int cfg;
};

struct tlb_priv_id {
	unsigned int cfg;
	unsigned int id;
};

struct tlb_port_cfg {
	unsigned int cfg;
	unsigned int id;
};

/*
 * flags[7:4] specifies TLB matching types.
 * 0x1 : TLB way dedication
 * 0x2 : TLB port dedication
 */
#define TLB_TYPE_MASK(x)	((x) & (0xF << 4))
#define TLB_TYPE_WAY		(0x1 << 4)
#define TLB_TYPE_PORT		(0x2 << 4)
#define IS_TLB_WAY_TYPE(data)	(TLB_TYPE_MASK((data)->tlb_props.flags)	\
				== TLB_TYPE_WAY)
#define IS_TLB_PORT_TYPE(data)	(TLB_TYPE_MASK((data)->tlb_props.flags)	\
				== TLB_TYPE_PORT)

#define TLB_WAY_PRIVATE_ID	(1 << 0)
#define TLB_WAY_PRIVATE_ADDR	(1 << 1)
#define TLB_WAY_PUBLIC		(1 << 2)

struct tlb_way_props {
	int priv_id_cnt;
	int priv_addr_cnt;
	unsigned int public_cfg;
	struct tlb_priv_id *priv_id_cfg;
	struct tlb_priv_addr *priv_addr_cfg;
};

struct tlb_port_props {
	int port_id_cnt;
	int slot_cnt;
	struct tlb_port_cfg *port_cfg;
	unsigned int *slot_cfg;
};

struct tlb_props {
	int flags;
	union {
		struct tlb_way_props way_props;
		struct tlb_port_props port_props;
	};
};

struct exynos_iovmm;
struct exynos_iommu_domain {
	struct iommu_domain domain;
	sysmmu_pte_t *pgtable;
	/* Keep L2 tables until domain destruction: hardware may prefetch them. */
	sysmmu_pte_t *lv2table[NUM_LV1ENTRIES];
	spinlock_t pgtablelock;
	spinlock_t lock;
	struct list_head clients_list;
};
struct sysmmu_drvdata {
	struct device *sysmmu;
	void __iomem *sfrbase;
	struct clk *clk;
	struct iommu_device iommu;
	struct iommu_group *group;
	struct iommu_domain *domain;
	struct mutex mutex;
	spinlock_t lock;
	struct list_head owners;
	unsigned int activations;
	bool active;
	bool legacy_reservation_failed;
	bool suspended;
	unsigned int ptw_faults;
	unsigned int other_faults;
	int irq;
	const char *port_name;
	int version;
	int qos;
	u32 securebase;
	struct tlb_props tlb_props;
	struct exynos_iovmm *vmm;
	void (*release_iovmm)(struct device *dev);
	/*
	 * Bootloader framebuffer handover. 4.4 enabled translation from
	 * iovmm_activate(), after dpp_probe() had identity-mapped the splash
	 * buffer DECON is still scanning. The 5.10 core attaches the default
	 * DMA domain before driver probe, so translation must stay off (and
	 * the SMMU clock on) until that identity mapping exists.
	 */
	phys_addr_t handover_base;
	size_t handover_size;
	bool translation_deferred;
	/* ABOX walks a second page table (ID #1) with its own base/flush. */
	bool is_abox;
	/* ABOX translation kept on across its runtime suspend (clock held). */
	bool rpm_held;
	bool handover_mapped;
	bool boot_clk_held;
};
int exynos_sysmmu_allow_translation(struct sysmmu_drvdata *data);
struct exynos_iommu_owner {
	/* First owner remains the legacy IOVMM handle; followers share its master. */
	struct list_head siblings;
	struct list_head sibling;
	struct sysmmu_drvdata *data;
	struct iommu_domain *domain;
	struct device *master;
	struct device_link *link;
	struct list_head client;
	struct list_head node;
	iommu_fault_handler_t fault_handler;
	void *token;
	bool activated;
};
struct exynos_vm_region {
	struct list_head node;
	u32 start;
	u32 size;
	u32 section_off;
	u32 dummy_size;
	bool one_to_one;
};

struct exynos_iovmm {
	struct iommu_domain *domain;	/* iommu domain for this iovmm */
	size_t iovm_size;		/* iovm bitmap size per plane */
	u32 iova_start;			/* iovm start address per plane */
	unsigned long *vm_map;		/* iovm biatmap per plane */
	struct list_head regions_list;	/* list of exynos_vm_region */
	spinlock_t vmlist_lock;		/* lock for updating regions_list */
	spinlock_t bitmap_lock;		/* lock for manipulating bitmaps */
	struct device *dev;	/* peripheral device that has this iovmm */
	size_t allocated_size;
	int num_areas;
	unsigned int num_map;
	unsigned int num_unmap;
	const char *domain_name;
	struct iommu_group *group;
	struct mutex mutex;
	unsigned int users;
};

void exynos_sysmmu_tlb_invalidate(struct iommu_domain *domain,
				dma_addr_t start, size_t size);
static inline struct exynos_iovmm *exynos_get_iovmm(struct device *dev)
{
	struct exynos_iommu_owner *owner = dev_iommu_priv_get(dev);

	return owner ? owner->data->vmm : NULL;
}
#endif
