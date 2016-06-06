/*
 * Persistent Memory Driver
 *
 * Copyright (c) 2014-2015, Intel Corporation.
 * Copyright (c) 2015, Christoph Hellwig <hch@lst.de>.
 * Copyright (c) 2015, Boaz Harrosh <boaz@plexistor.com>.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <asm/cacheflush.h>
#include <linux/blkdev.h>
#include <linux/hdreg.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/badblocks.h>
#include <linux/memremap.h>
#include <linux/vmalloc.h>
#include <linux/pfn_t.h>
#include <linux/slab.h>
#include <linux/pmem.h>
#include <linux/nd.h>
#include "pfn.h"
#include "nd.h"

struct pmem_device {
	struct request_queue	*pmem_queue;
	struct gendisk		*pmem_disk;
	struct nd_namespace_common *ndns;

	/* One contiguous memory region per device */
	phys_addr_t		phys_addr;
	/* when non-zero this device is hosting a 'pfn' instance */
	phys_addr_t		data_offset;
	u64			pfn_flags;
	void __pmem		*virt_addr;
	size_t			size;
	struct badblocks	bb;
};

static int pmem_major;

static bool is_bad_pmem(struct badblocks *bb, sector_t sector, unsigned int len)
{
	if (bb->count) {
		sector_t first_bad;
		int num_bad;

		return !!badblocks_check(bb, sector, len / 512, &first_bad,
				&num_bad);
	}

	return false;
}

static int pmem_do_bvec(struct pmem_device *pmem, struct page *page,
			unsigned int len, unsigned int off, int rw,
			sector_t sector)
{
	int rc = 0;
	void *mem = kmap_atomic(page);
	phys_addr_t pmem_off = sector * 512 + pmem->data_offset;
	void __pmem *pmem_addr = pmem->virt_addr + pmem_off;

	if (rw == READ) {
		if (unlikely(is_bad_pmem(&pmem->bb, sector, len)))
			rc = -EIO;
		else {
			memcpy_from_pmem(mem + off, pmem_addr, len);
			flush_dcache_page(page);
		}
	} else {
		flush_dcache_page(page);
		memcpy_to_pmem(pmem_addr, mem + off, len);
	}

	kunmap_atomic(mem);
	return rc;
}

static blk_qc_t pmem_make_request(struct request_queue *q, struct bio *bio)
{
	int rc = 0;
	bool do_acct;
	unsigned long start;
	struct bio_vec bvec;
	struct bvec_iter iter;
	struct block_device *bdev = bio->bi_bdev;
	struct pmem_device *pmem = bdev->bd_disk->private_data;

	do_acct = nd_iostat_start(bio, &start);
	bio_for_each_segment(bvec, bio, iter) {
		rc = pmem_do_bvec(pmem, bvec.bv_page, bvec.bv_len,
				bvec.bv_offset, bio_data_dir(bio),
				iter.bi_sector);
		if (rc) {
			bio->bi_error = rc;
			break;
		}
	}
	if (do_acct)
		nd_iostat_end(bio, start);

	if (bio_data_dir(bio))
		wmb_pmem();

	bio_endio(bio);
	return BLK_QC_T_NONE;
}

static int pmem_rw_page(struct block_device *bdev, sector_t sector,
		       struct page *page, int rw)
{
	struct pmem_device *pmem = bdev->bd_disk->private_data;
	int rc;

	rc = pmem_do_bvec(pmem, page, PAGE_CACHE_SIZE, 0, rw, sector);
	if (rw & WRITE)
		wmb_pmem();

	/*
	 * The ->rw_page interface is subtle and tricky.  The core
	 * retries on any error, so we can only invoke page_endio() in
	 * the successful completion case.  Otherwise, we'll see crashes
	 * caused by double completion.
	 */
	if (rc == 0)
		page_endio(page, rw & WRITE, 0);

	return rc;
}

static long pmem_direct_access(struct block_device *bdev, sector_t sector,
		      void __pmem **kaddr, pfn_t *pfn)
{
	struct pmem_device *pmem = bdev->bd_disk->private_data;
	resource_size_t offset = sector * 512 + pmem->data_offset;

	*kaddr = pmem->virt_addr + offset;
	*pfn = phys_to_pfn_t(pmem->phys_addr + offset, pmem->pfn_flags);

	return pmem->size - offset;
}

static const struct block_device_operations pmem_fops = {
	.owner =		THIS_MODULE,
	.rw_page =		pmem_rw_page,
	.direct_access =	pmem_direct_access,
	.revalidate_disk =	nvdimm_revalidate_disk,
};

static struct pmem_device *pmem_alloc(struct device *dev,
		struct resource *res, int id)
{
	struct pmem_device *pmem;
	struct request_queue *q;

	pmem = devm_kzalloc(dev, sizeof(*pmem), GFP_KERNEL);
	if (!pmem)
		return ERR_PTR(-ENOMEM);

	pmem->phys_addr = res->start;
	pmem->size = resource_size(res);
	if (!arch_has_wmb_pmem())
		dev_warn(dev, "unable to guarantee persistence of writes\n");

	if (!devm_request_mem_region(dev, pmem->phys_addr, pmem->size,
			dev_name(dev))) {
		dev_warn(dev, "could not reserve region [0x%pa:0x%zx]\n",
				&pmem->phys_addr, pmem->size);
		return ERR_PTR(-EBUSY);
	}

	q = blk_alloc_queue_node(GFP_KERNEL, dev_to_node(dev));
	if (!q)
		return ERR_PTR(-ENOMEM);

	pmem->pfn_flags = PFN_DEV;
	if (pmem_should_map_pages(dev)) {
		pmem->virt_addr = (void __pmem *) devm_memremap_pages(dev, res,
				&q->q_usage_counter, NULL);
		pmem->pfn_flags |= PFN_MAP;
	} else
		pmem->virt_addr = (void __pmem *) devm_memremap(dev,
				pmem->phys_addr, pmem->size,
				ARCH_MEMREMAP_PMEM);

	if (IS_ERR(pmem->virt_addr)) {
		blk_cleanup_queue(q);
		return (void __force *) pmem->virt_addr;
	}

	pmem->pmem_queue = q;
	return pmem;
}

static void pmem_detach_disk(struct pmem_device *pmem)
{
	if (!pmem->pmem_disk)
		return;

	del_gendisk(pmem->pmem_disk);
	put_disk(pmem->pmem_disk);
	blk_cleanup_queue(pmem->pmem_queue);
}

static int pmem_attach_disk(struct device *dev,
		struct nd_namespace_common *ndns, struct pmem_device *pmem)
{
	int nid = dev_to_node(dev);
	struct gendisk *disk;

	blk_queue_make_request(pmem->pmem_queue, pmem_make_request);
	blk_queue_physical_block_size(pmem->pmem_queue, PAGE_SIZE);
	blk_queue_max_hw_sectors(pmem->pmem_queue, UINT_MAX);
	blk_queue_bounce_limit(pmem->pmem_queue, BLK_BOUNCE_ANY);
	queue_flag_set_unlocked(QUEUE_FLAG_NONROT, pmem->pmem_queue);

	disk = alloc_disk_node(0, nid);
	if (!disk) {
		blk_cleanup_queue(pmem->pmem_queue);
		return -ENOMEM;
	}

	disk->major		= pmem_major;
	disk->first_minor	= 0;
	disk->fops		= &pmem_fops;
	disk->private_data	= pmem;
	disk->queue		= pmem->pmem_queue;
	disk->flags		= GENHD_FL_EXT_DEVT;
	nvdimm_namespace_disk_name(ndns, disk->disk_name);
	disk->driverfs_dev = dev;
	set_capacity(disk, (pmem->size - pmem->data_offset) / 512);
	pmem->pmem_disk = disk;
	devm_exit_badblocks(dev, &pmem->bb);
	if (devm_init_badblocks(dev, &pmem->bb))
		return -ENOMEM;
	nvdimm_namespace_add_poison(ndns, &pmem->bb, pmem->data_offset);

	disk->bb = &pmem->bb;
	add_disk(disk);
	revalidate_disk(disk);

	return 0;
}

static int pmem_rw_bytes(struct nd_namespace_common *ndns,
		resource_size_t offset, void *buf, size_t size, int rw)
{
	struct pmem_device *pmem = dev_get_drvdata(ndns->claim);

	if (unlikely(offset + size > pmem->size)) {
		dev_WARN_ONCE(&ndns->dev, 1, "request out of range\n");
		return -EFAULT;
	}

	if (rw == READ) {
		unsigned int sz_align = ALIGN(size + (offset & (512 - 1)), 512);

		if (unlikely(is_bad_pmem(&pmem->bb, offset / 512, sz_align)))
			return -EIO;
		memcpy_from_pmem(buf, pmem->virt_addr + offset, size);
	} else {
		memcpy_to_pmem(pmem->virt_addr + offset, buf, size);
		wmb_pmem();
	}

	return 0;
}

static int nd_pfn_init(struct nd_pfn *nd_pfn)
{
	struct nd_pfn_sb *pfn_sb = kzalloc(sizeof(*pfn_sb), GFP_KERNEL);
	struct pmem_device *pmem = dev_get_drvdata(&nd_pfn->dev);
	struct nd_namespace_common *ndns = nd_pfn->ndns;
	struct nd_region *nd_region;
	unsigned long npfns;
	phys_addr_t offset;
	u64 checksum;
	int rc;

	if (!pfn_sb)
		return -ENOMEM;

	nd_pfn->pfn_sb = pfn_sb;
	rc = nd_pfn_validate(nd_pfn);
	if (rc == -ENODEV)
		/* no info block, do init */;
	else
		return rc;

	nd_region = to_nd_region(nd_pfn->dev.parent);
	if (nd_region->ro) {
		dev_info(&nd_pfn->dev,
				"%s is read-only, unable to init metadata\n",
				dev_name(&nd_region->dev));
		goto err;
	}

	memset(pfn_sb, 0, sizeof(*pfn_sb));
	npfns = (pmem->size - SZ_8K) / SZ_4K;
	/*
	 * Note, we use 64 here for the standard size of struct page,
	 * debugging options may cause it to be larger in which case the
	 * implementation will limit the pfns advertised through
	 * ->direct_access() to those that are included in the memmap.
	 */
	if (nd_pfn->mode == PFN_MODE_PMEM) {
		unsigned long memmap_size;

		/*
		 * vmemmap_populate_hugepages() allocates the memmap array in
		 * HPAGE_SIZE chunks.
		 */
		memmap_size = ALIGN(64 * npfns, PMD_SIZE);
		offset = ALIGN(SZ_8K + memmap_size, nd_pfn->align);
	} else if (nd_pfn->mode == PFN_MODE_RAM)
		offset = ALIGN(SZ_8K, nd_pfn->align);
	else
		goto err;

	npfns = (pmem->size - offset) / SZ_4K;
	pfn_sb->mode = cpu_to_le32(nd_pfn->mode);
	pfn_sb->dataoff = cpu_to_le64(offset);
	pfn_sb->npfns = cpu_to_le64(npfns);
	memcpy(pfn_sb->signature, PFN_SIG, PFN_SIG_LEN);
	memcpy(pfn_sb->uuid, nd_pfn->uuid, 16);
	memcpy(pfn_sb->parent_uuid, nd_dev_to_uuid(&ndns->dev), 16);
	pfn_sb->version_major = cpu_to_le16(1);
	checksum = nd_sb_checksum((struct nd_gen_sb *) pfn_sb);
	pfn_sb->checksum = cpu_to_le64(checksum);

	rc = nvdimm_write_bytes(ndns, SZ_4K, pfn_sb, sizeof(*pfn_sb));
	if (rc)
		goto err;

	return 0;
 err:
	nd_pfn->pfn_sb = NULL;
	kfree(pfn_sb);
	return -ENXIO;
}

static int nvdimm_namespace_detach_pfn(struct nd_namespace_common *ndns)
{
	struct nd_pfn *nd_pfn = to_nd_pfn(ndns->claim);
	struct pmem_device *pmem;

	/* free pmem disk */
	pmem = dev_get_drvdata(&nd_pfn->dev);
	pmem_detach_disk(pmem);

	/* release nd_pfn resources */
	kfree(nd_pfn->pfn_sb);
	nd_pfn->pfn_sb = NULL;

	return 0;
}

static int nvdimm_namespace_attach_pfn(struct nd_namespace_common *ndns)
{
	struct nd_namespace_io *nsio = to_nd_namespace_io(&ndns->dev);
	struct nd_pfn *nd_pfn = to_nd_pfn(ndns->claim);
	struct device *dev = &nd_pfn->dev;
	struct nd_region *nd_region;
	struct vmem_altmap *altmap;
	struct nd_pfn_sb *pfn_sb;
	struct pmem_device *pmem;
	struct request_queue *q;
	phys_addr_t offset;
	int rc;
	struct vmem_altmap __altmap = {
		.base_pfn = __phys_to_pfn(nsio->res.start),
		.reserve = __phys_to_pfn(SZ_8K),
	};

	if (!nd_pfn->uuid || !nd_pfn->ndns)
		return -ENODEV;

	nd_region = to_nd_region(dev->parent);
	rc = nd_pfn_init(nd_pfn);
	if (rc)
		return rc;

	pfn_sb = nd_pfn->pfn_sb;
	offset = le64_to_cpu(pfn_sb->dataoff);
	nd_pfn->mode = le32_to_cpu(nd_pfn->pfn_sb->mode);
	if (nd_pfn->mode == PFN_MODE_RAM) {
		if (offset < SZ_8K)
			return -EINVAL;
		nd_pfn->npfns = le64_to_cpu(pfn_sb->npfns);
		altmap = NULL;
	} else if (nd_pfn->mode == PFN_MODE_PMEM) {
		nd_pfn->npfns = (resource_size(&nsio->res) - offset)
			/ PAGE_SIZE;
		if (le64_to_cpu(nd_pfn->pfn_sb->npfns) > nd_pfn->npfns)
			dev_info(&nd_pfn->dev,
					"number of pfns truncated from %lld to %ld\n",
					le64_to_cpu(nd_pfn->pfn_sb->npfns),
					nd_pfn->npfns);
		altmap = & __altmap;
		altmap->free = __phys_to_pfn(offset - SZ_8K);
		altmap->alloc = 0;
	} else {
		rc = -ENXIO;
		goto err;
	}

	/* establish pfn range for lookup, and switch to direct map */
	pmem = dev_get_drvdata(dev);
	q = pmem->pmem_queue;
	devm_memunmap(dev, (void __force *) pmem->virt_addr);
	pmem->virt_addr = (void __pmem *) devm_memremap_pages(dev, &nsio->res,
			&q->q_usage_counter, altmap);
	pmem->pfn_flags |= PFN_MAP;
	if (IS_ERR(pmem->virt_addr)) {
		rc = PTR_ERR(pmem->virt_addr);
		goto err;
	}

	/* attach pmem disk in "pfn-mode" */
	pmem->data_offset = offset;
	rc = pmem_attach_disk(dev, ndns, pmem);
	if (rc)
		goto err;

	return rc;
 err:
	nvdimm_namespace_detach_pfn(ndns);
	return rc;
}

static int nd_pmem_probe(struct device *dev)
{
	struct nd_region *nd_region = to_nd_region(dev->parent);
	struct nd_namespace_common *ndns;
	struct nd_namespace_io *nsio;
	struct pmem_device *pmem;

	ndns = nvdimm_namespace_common_probe(dev);
	if (IS_ERR(ndns))
		return PTR_ERR(ndns);

	nsio = to_nd_namespace_io(&ndns->dev);
	pmem = pmem_alloc(dev, &nsio->res, nd_region->id);
	if (IS_ERR(pmem))
		return PTR_ERR(pmem);

	pmem->ndns = ndns;
	dev_set_drvdata(dev, pmem);
	ndns->rw_bytes = pmem_rw_bytes;
	if (devm_init_badblocks(dev, &pmem->bb))
		return -ENOMEM;
	nvdimm_namespace_add_poison(ndns, &pmem->bb, 0);

	if (is_nd_btt(dev)) {
		/* btt allocates its own request_queue */
		blk_cleanup_queue(pmem->pmem_queue);
		pmem->pmem_queue = NULL;
		return nvdimm_namespace_attach_btt(ndns);
	}

	if (is_nd_pfn(dev))
		return nvdimm_namespace_attach_pfn(ndns);

	if (nd_btt_probe(ndns, pmem) == 0 || nd_pfn_probe(ndns, pmem) == 0) {
		/*
		 * We'll come back as either btt-pmem, or pfn-pmem, so
		 * drop the queue allocation for now.
		 */
		blk_cleanup_queue(pmem->pmem_queue);
		return -ENXIO;
	}

	return pmem_attach_disk(dev, ndns, pmem);
}

static int nd_pmem_remove(struct device *dev)
{
	struct pmem_device *pmem = dev_get_drvdata(dev);

	if (is_nd_btt(dev))
		nvdimm_namespace_detach_btt(pmem->ndns);
	else if (is_nd_pfn(dev))
		nvdimm_namespace_detach_pfn(pmem->ndns);
	else
		pmem_detach_disk(pmem);

	return 0;
}

MODULE_ALIAS("pmem");
MODULE_ALIAS_ND_DEVICE(ND_DEVICE_NAMESPACE_IO);
MODULE_ALIAS_ND_DEVICE(ND_DEVICE_NAMESPACE_PMEM);
static struct nd_device_driver nd_pmem_driver = {
	.probe = nd_pmem_probe,
	.remove = nd_pmem_remove,
	.drv = {
		.name = "nd_pmem",
	},
	.type = ND_DRIVER_NAMESPACE_IO | ND_DRIVER_NAMESPACE_PMEM,
};

static int __init pmem_init(void)
{
	int error;

	pmem_major = register_blkdev(0, "pmem");
	if (pmem_major < 0)
		return pmem_major;

	error = nd_driver_register(&nd_pmem_driver);
	if (error) {
		unregister_blkdev(pmem_major, "pmem");
		return error;
	}

	return 0;
}
module_init(pmem_init);

static void pmem_exit(void)
{
	driver_unregister(&nd_pmem_driver.drv);
	unregister_blkdev(pmem_major, "pmem");
}
module_exit(pmem_exit);

MODULE_AUTHOR("Ross Zwisler <ross.zwisler@linux.intel.com>");
MODULE_LICENSE("GPL v2");