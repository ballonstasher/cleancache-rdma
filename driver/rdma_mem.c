#include <linux/slab.h>

#include "rdma_mem.h"
#include "rdma.h"

extern bool dma_mem_for_filter;

struct rdma_mm *rdma_mem_init(void)
{
	struct rdma_mm *mm;

	mm = kzalloc(sizeof(struct rdma_mm), GFP_KERNEL);
	if (!mm) {
		pr_err("failed to allocate mm");
		return ERR_PTR(-ENOMEM);
	}

	return mm;
}

void rdma_mem_exit(struct rdma_mm *mm)
{
	if (mm->mr)
		kfree(mm->mr);
	if (mm)
		kfree(mm);	
}

int rdma_mem_register_region(struct dcc_rdma_ctrl *ctrl)
{
	struct rdma_dev *rdev = ctrl->rdev;
	int access_flags = IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_WRITE | 
		IB_ACCESS_REMOTE_READ;
	struct ib_mr *mr;

	MM_MR(ctrl) = rdev->pd->device->ops.get_dma_mr(rdev->pd, access_flags);
	mr = MM_MR(ctrl);
	if (IS_ERR(mr)) {
		pr_err("failed to get_dma_mr");
		return PTR_ERR(mr);
	}
		
	mr->pd = rdev->pd;

	return 0;
}

int rdma_mem_alloc_region(struct dcc_rdma_ctrl *ctrl)
{
	struct mm_pool *key_mm_pool = &KEY_MM_POOL(ctrl);
	struct mm_pool *pagebuf_mm_pool = &PAGEBUF_MM_POOL(ctrl);
	struct mm_pool *keypage_mm_pool = &KEYPAGE_MM_POOL(ctrl);
	struct mm_info *filter_mm_info = &FILTER_MM_INFO(ctrl);
	struct rdma_dev *rdev = ctrl->rdev;
	int err;
	size_t size;

	err = dma_set_mask_and_coherent(rdev->dev->dma_device,
			DMA_BIT_MASK(64));
	if (err) {
		pr_err("dma_set_mask returned: %d", err);
		return -EINVAL;
	}

	if (dma_mem_for_filter) {
		unsigned int num_bits = SVR_MM_INFO(ctrl).info[1];
		size_t bitmap_size = BITS_TO_LONGS(num_bits) * 
			sizeof(unsigned long);
		struct mm_pool *filter_mm_pool = &FILTER_MM_POOL(ctrl); 

		/* alloc and map at once */
		filter_mm_pool->ptr = ib_dma_alloc_coherent(rdev->dev,
				bitmap_size, &filter_mm_pool->dma_addr,
				GFP_KERNEL);
		if (!filter_mm_pool->dma_addr) {
			pr_err("failed to allocate coherent buffer for"
					" filter_mm_pool");
			return -ENOMEM;
		}
		
		filter_mm_info->baseaddr = (u64) filter_mm_pool->dma_addr;
	}
	filter_mm_info->key = MM_MR(ctrl)->rkey;
	
	/* key buffer */
	key_mm_pool->ptr = ib_dma_alloc_coherent(rdev->dev,
			sizeof(u64) * rdma_config.num_msgs, 
			&key_mm_pool->dma_addr,
			GFP_KERNEL);
	if (!key_mm_pool->dma_addr) {
		pr_err("failed to allocate coherent buffer for key_mm_pool");
		return -ENOMEM;
	}
	ib_dma_sync_single_for_device(rdev->dev, key_mm_pool->dma_addr,
			sizeof(u64) * rdma_config.num_msgs, 
			DMA_TO_DEVICE);

	pagebuf_mm_pool->ptr = ib_dma_alloc_coherent(rdev->dev,
			PAGE_SIZE, &pagebuf_mm_pool->dma_addr, GFP_KERNEL);
	if (!pagebuf_mm_pool->dma_addr) {
		pr_err("failed to allocate coherent buffer for pagebuf_mm_pool");
		return -ENOMEM;
	}
	ib_dma_sync_single_for_device(rdev->dev, pagebuf_mm_pool->dma_addr,
			PAGE_SIZE, DMA_TO_DEVICE);

	/* keypage put buffer */
	size = (sizeof(u64) + PAGE_SIZE) * rdma_config.num_msgs;
	keypage_mm_pool->ptr = ib_dma_alloc_coherent(rdev->dev,
			size, &keypage_mm_pool->dma_addr, GFP_KERNEL);
	if (!keypage_mm_pool->dma_addr) {
		pr_err("failed to allocate coherent buffer for keypage_mm_pool");
		return -ENOMEM;
	}
	ib_dma_sync_single_for_device(rdev->dev, keypage_mm_pool->dma_addr,
			size, DMA_TO_DEVICE);
	return 0;
}

void rdma_mem_free_region(struct dcc_rdma_ctrl *ctrl)
{
	struct mm_pool *key_mm_pool = &KEY_MM_POOL(ctrl);
	struct mm_pool *pagebuf_mm_pool = &PAGEBUF_MM_POOL(ctrl);
	struct mm_pool *keypage_mm_pool = &KEYPAGE_MM_POOL(ctrl);
	size_t size;

	if (dma_mem_for_filter) {
		struct mm_pool *filter_mm_pool = &FILTER_MM_POOL(ctrl);
		unsigned int num_bits = SVR_MM_INFO(ctrl).info[1];
		size_t bitmap_size = BITS_TO_LONGS(num_bits) * 
			sizeof(unsigned long);

		ib_dma_free_coherent(ctrl->rdev->dev, bitmap_size, 
				filter_mm_pool->ptr, filter_mm_pool->dma_addr);	
	}

	ib_dma_free_coherent(ctrl->rdev->dev, 
			sizeof(u64) * rdma_config.num_msgs, 
			key_mm_pool->ptr, key_mm_pool->dma_addr);

	ib_dma_free_coherent(ctrl->rdev->dev,
			PAGE_SIZE, pagebuf_mm_pool->ptr, 
			pagebuf_mm_pool->dma_addr);
	
	size = (sizeof(u64) + PAGE_SIZE) * rdma_config.num_msgs;
	ib_dma_free_coherent(ctrl->rdev->dev, size, 
			keypage_mm_pool->ptr, 
			keypage_mm_pool->dma_addr);

	pr_err("%s: id=%d", __func__, ctrl->id);
}

