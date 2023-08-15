#include <linux/slab.h>
#include <linux/cpumask.h>

#include "rdma.h"
#include "config.h"
#include "util/crc8.h"

//#define DCC_RDMA_DEBUG
#ifdef DCC_RDMA_DEBUG
#define dcc_rdma_debug(fmt, ...)     \
	pr_err("%s: "fmt, __func__, ##__VA_ARGS__)
#else
#define dcc_rdma_debug(fmt, ...)     do {} while (0)
#endif

struct dcc_rdma_ctrl **ctrls;
struct kmem_cache *req_cache;
struct dcc_rdma_config_t rdma_config;

extern int dcc_backend_init(int, struct dcc_rdma_ctrl *); 

static inline void poll_send_cq(struct rdma_queue *q) 
{
	struct ib_wc wc = {};
	__poll_cq(&wc, q->qp->send_cq);
}

static inline int poll_recv_cq(struct rdma_queue *q, int *etc, int *crc)
{
	struct ib_wc wc = {};
	__poll_cq(&wc, q->qp->recv_cq);
	return 0;
}

/* per-cpu qp doesn't have to lock for one-sided read/write */
int dcc_rdma_write_page(struct dcc_rdma_ctrl *ctrl, struct page *page, u64 key, 
		u64 roffset)
{
	//int cpu_id = smp_processor_id();
	int ret;
	struct rdma_req *req;
	struct rdma_queue *q;
	struct ib_sge sge = {};
	struct ib_rdma_wr rdma_wr = {};
	const struct ib_send_wr *bad_wr;
	struct rdma_dev *rdev = ctrl->rdev; 
	unsigned long flags;

	q = get_rdma_queue(ctrl, 0);

	ret = get_req_for_page(&req, rdev->dev, page, DMA_TO_DEVICE);
	if (unlikely(ret)) {
		pr_err("get_req_for_page failed: %d", ret);
	}

	sge.addr   = req->dma;
	sge.length = PAGE_SIZE;
	sge.lkey   = rdev->pd->local_dma_lkey;

	rdma_wr.wr.next       = NULL;
	rdma_wr.wr.wr_id      = 0;
	rdma_wr.wr.sg_list    = &sge;
	rdma_wr.wr.num_sge    = 1;
	rdma_wr.wr.opcode     = IB_WR_RDMA_WRITE;
	rdma_wr.wr.send_flags = IB_SEND_SIGNALED;
	rdma_wr.remote_addr   = SVR_MM_INFO(ctrl).baseaddr + roffset; 
	rdma_wr.rkey          = SVR_MM_INFO(ctrl).key;

	spin_lock_irqsave(&q->lock, flags);
	ret = ib_post_send(q->qp, &rdma_wr.wr, &bad_wr);
	if (unlikely(ret)) {
		pr_err("%s: ib_post_send failed: %d", __func__, ret);
	}
	poll_send_cq(q);
	spin_unlock_irqrestore(&q->lock, flags);

	ib_dma_unmap_page(rdev->dev, req->dma, PAGE_SIZE, DMA_TO_DEVICE);
	kmem_cache_free(req_cache, req);

	return ret;
}

int dcc_rdma_read_page(struct dcc_rdma_ctrl *ctrl, struct page *page, u64 key, 
		u64 roffset)
{
	int cpu_id = smp_processor_id();
	int ret;
	struct rdma_req *req;
	struct rdma_queue *q;
	struct ib_sge sge = {};
	struct ib_rdma_wr rdma_wr = {};
	const struct ib_send_wr *bad_wr;
	struct rdma_dev *rdev = ctrl->rdev; 
	unsigned long flags;

	q = get_rdma_queue(ctrl, cpu_id % (rdma_config.num_data_qps - 1) + 1);
	
	ret = get_req_for_page(&req, rdev->dev, page, DMA_FROM_DEVICE);
	if (unlikely(ret)) {
		pr_err("get_req_for_page failed: %d", ret);
	}

	sge.addr   = (u64)req->dma;
	sge.length = PAGE_SIZE;
	sge.lkey   = rdev->pd->local_dma_lkey;

	rdma_wr.wr.next        = NULL;
	rdma_wr.wr.wr_id       = 0;
	rdma_wr.wr.sg_list     = &sge;
	rdma_wr.wr.num_sge     = 1;
	rdma_wr.wr.opcode      = IB_WR_RDMA_READ;
	rdma_wr.wr.send_flags  = IB_SEND_SIGNALED;
	rdma_wr.remote_addr    = SVR_MM_INFO(ctrl).baseaddr + roffset;
	rdma_wr.rkey           = SVR_MM_INFO(ctrl).key;

	spin_lock_irqsave(&q->lock, flags);
	ret = ib_post_send(q->qp, &rdma_wr.wr, &bad_wr);
	if (unlikely(ret)) {
		pr_err("%s: ib_post_send failed: %d", __func__, ret);
	}
	poll_send_cq(q);
	spin_unlock_irqrestore(&q->lock, flags);

	ib_dma_unmap_page(rdev->dev, req->dma, PAGE_SIZE, DMA_FROM_DEVICE);
	kmem_cache_free(req_cache, req);

	return ret;
}

int dcc_rdma_send_msg(struct rdma_queue *q, u64 addr, u32 bufsize, u32 imm) 
{
	const struct ib_send_wr *bad_wr;
	struct ib_send_wr wr = {};
	struct ib_sge sge = {};
	int ret;
	
	if (addr) {
		sge.addr   = addr;	
		sge.length = bufsize;
		sge.lkey   = q->ctrl->rdev->pd->local_dma_lkey;
	}
	
	wr.next        = NULL;
	wr.wr_id       = 0;
	wr.sg_list     = &sge;
	wr.num_sge     = !addr ? 0 : 1;
	wr.opcode      = !imm ? IB_WR_SEND : IB_WR_SEND_WITH_IMM;
	wr.send_flags  = IB_SEND_SIGNALED | IB_SEND_INLINE;
	wr.ex.imm_data = imm;

	ret = ib_post_send(q->qp, &wr, &bad_wr);
	if (unlikely(ret)) {
		pr_err("ib_post_send failed: %d", ret);
	}
	return ret;
}

/************************* Setup rdma connection *************************/

static int dcc_rdma_recv_remotemr(struct dcc_rdma_ctrl *ctrl)
{
	struct rdma_req *req;
	struct ib_device *dev = ctrl->rdev->dev;
	int ret;
	struct mm_info *server_mm_info = &SVR_MM_INFO(ctrl);

	ret = get_req_for_buf(&req, dev, (void *) server_mm_info, 
			sizeof(struct mm_info), DMA_FROM_DEVICE);
	if (unlikely(ret))
		goto out;

	ret = post_recv(&ctrl->queues[0], req->dma, sizeof(struct mm_info));
	if (unlikely(ret))
		goto out_free_req;

	udelay(10); /* this delay doesn't really matter, only happens once */
	poll_recv_cq(&ctrl->queues[0], NULL, NULL);

	ib_dma_unmap_single(dev, req->dma, sizeof(struct mm_info),
			DMA_FROM_DEVICE);
	complete_all(&req->done);

	pr_info("server_mm_info addr=0x%llx, key=%u, len=%u", 
			server_mm_info->baseaddr, server_mm_info->key,
			server_mm_info->len);

out_free_req:
	kmem_cache_free(req_cache, req);
out:
	return ret;
}

static struct rdma_dev *dcc_rdma_get_device(struct rdma_queue *q)
{
	struct rdma_dev *rdev = NULL;

	if (!q->ctrl->rdev) {
		rdev = kzalloc(sizeof(*rdev), GFP_KERNEL);
		if (!rdev) {
			pr_err("failed to allocate rdev");
			goto out_err;
		}

		rdev->dev = q->cm_id->device;

		//pr_info("selecting device %s", rdev->dev->name);

		rdev->pd = ib_alloc_pd(rdev->dev, 0);
		if (IS_ERR(rdev->pd)) {
			pr_err("failed to allocate pd");
			goto out_free_dev;
		}
#if 0
		if (!(rdev->dev->attrs.device_cap_flags &
					IB_DEVICE_MEM_MGT_EXTENSIONS)) {
			pr_err("memory registrations not supported\n");
			goto out_free_pd;
		}
#endif
		q->ctrl->rdev = rdev;
	}

	return q->ctrl->rdev;

//out_free_pd:
	ib_dealloc_pd(rdev->pd);
out_free_dev:
	kfree(rdev);
out_err:
	return NULL;
}

static void dcc_rdma_qp_event(struct ib_event *e, void *c)
{
	pr_info("dcc_rdma_qp_event");
}

static int dcc_rdma_create_qp(struct rdma_queue *q)
{
	struct rdma_dev *rdev = q->ctrl->rdev;
	struct ib_qp_init_attr init_attr = {
		.event_handler = dcc_rdma_qp_event,
		.cap.max_send_wr = QP_MAX_SEND_WR,
		.cap.max_recv_wr = QP_MAX_RECV_WR,
		.cap.max_recv_sge = QP_MAX_RECV_SGE,
		.cap.max_send_sge = QP_MAX_SEND_SGE,
		.sq_sig_type = IB_SIGNAL_REQ_WR,
		.qp_type = IB_QPT_RC,
	};
	struct ib_device *ibdev = q->ctrl->rdev->dev;
	int ret;

	/* TODO: allocation failure handling */
	q->send_cq = ib_alloc_cq(ibdev, q, CQ_NUM_CQES, 0, 
			IB_POLL_DIRECT);
	q->recv_cq = ib_alloc_cq(ibdev, q, CQ_NUM_CQES, 0, 
			IB_POLL_DIRECT);

	init_attr.send_cq = q->send_cq; 
	init_attr.recv_cq = q->recv_cq;

	/* just to check if we are compiling against the right headers */
	//init_attr.create_flags = IB_QP_EXP_CREATE_ATOMIC_BE_REPLY & 0;

	ret = rdma_create_qp(q->cm_id, rdev->pd, &init_attr);
	if (ret) {
		pr_err("rdma_create_qp failed: %d", ret);
	}

	q->qp = q->cm_id->qp;
	return ret;
}

static void dcc_rdma_destroy_queue_ib(struct rdma_queue *q)
{
	//rdma_destroy_qp(q->ctrl->cm_id);
}

static int dcc_rdma_create_queue_ib(struct rdma_queue *q)
{
	int ret;

	ret = dcc_rdma_create_qp(q);
	if (ret)
		return -1;

	return 0;
}

static int dcc_rdma_addr_resolved(struct rdma_queue *q)
{
	struct rdma_dev *rdev = NULL;
	int ret;

	rdev = dcc_rdma_get_device(q);
	if (!rdev) {
		pr_err("no device found");
		return -ENODEV;
	}

	ret = dcc_rdma_create_queue_ib(q);
	if (ret) {
		return ret;
	}

	ret = rdma_resolve_route(q->cm_id, CONNECTION_TIMEOUT_MS);
	if (ret) {
		pr_err("rdma_resolve_route failed");
		dcc_rdma_destroy_queue_ib(q);
	}

	return 0;
}

static int dcc_rdma_route_resolved(struct rdma_queue *q,
		struct rdma_conn_param *conn_params)
{
	struct rdma_conn_param param = {};
	int ret;

	param.qp_num = q->qp->qp_num;
	param.flow_control = 1;
	param.responder_resources = 16;
	param.initiator_depth = 16;
	param.retry_count = 7;
	param.rnr_retry_count = 7;
	param.private_data = NULL;
	param.private_data_len = 0;

	//pr_info("max_qp_rd_atom=%d max_qp_init_rd_atom=%d",
	//		q->ctrl->rdev->dev->attrs.max_qp_rd_atom,
	//		q->ctrl->rdev->dev->attrs.max_qp_init_rd_atom);

	ret = rdma_connect(q->cm_id, &param);
	if (ret) {
		pr_err("rdma_connect failed (%d)", ret);
		dcc_rdma_destroy_queue_ib(q);
	}

	return 0;
}

static int dcc_rdma_conn_established(struct rdma_queue *q)
{
	pr_info("connection established: %d", q->id);
	return 0;
}

static int dcc_rdma_cm_handler(struct rdma_cm_id *cm_id,
		struct rdma_cm_event *ev)
{
	struct rdma_queue *queue = cm_id->context;
	int cm_error = 0;

	//pr_info("cm_handler msg: %s (%d) status %d id %p", 
	//		rdma_event_msg(ev->event),
	//		ev->event, ev->status, cm_id);

	switch (ev->event) {
		case RDMA_CM_EVENT_ADDR_RESOLVED:
			cm_error = dcc_rdma_addr_resolved(queue);
			break;
		case RDMA_CM_EVENT_ROUTE_RESOLVED:
			cm_error = dcc_rdma_route_resolved(queue, 
					&ev->param.conn);
			break;
		case RDMA_CM_EVENT_ESTABLISHED:
			queue->cm_error = dcc_rdma_conn_established(queue);
			/* complete cm_done regardless of success/failure */
			complete(&queue->cm_done);
			return 0;
		case RDMA_CM_EVENT_REJECTED:
			pr_err("Q%d: connection rejected", queue->id);
			break;
		case RDMA_CM_EVENT_ADDR_ERROR:
		case RDMA_CM_EVENT_ROUTE_ERROR:
		case RDMA_CM_EVENT_CONNECT_ERROR:
		case RDMA_CM_EVENT_UNREACHABLE:
			pr_err("Q%d: CM error event %d", queue->id, ev->event);
			cm_error = -ECONNRESET;
			break;
		case RDMA_CM_EVENT_DISCONNECTED:
		case RDMA_CM_EVENT_ADDR_CHANGE:
		case RDMA_CM_EVENT_TIMEWAIT_EXIT:
			pr_err("Q%d: CM connection closed %d", queue->id, 
					ev->event);
			break;
		case RDMA_CM_EVENT_DEVICE_REMOVAL:
			/* device removal is handled via the ib_client API */
			break;
		default:
			pr_err("Q%d: CM unexpected event: %d", queue->id, 
					ev->event);
			break;
	}

	if (cm_error) {
		queue->cm_error = cm_error;
		complete(&queue->cm_done);
	}

	return 0;
}

static inline int dcc_rdma_wait_for_cm(struct rdma_queue *queue)
{
	wait_for_completion_interruptible_timeout(&queue->cm_done,
			msecs_to_jiffies(CONNECTION_TIMEOUT_MS) + 1);
	return queue->cm_error;
}


static int __dcc_rdma_init_queue(struct dcc_rdma_ctrl *ctrl, int idx)
{
	struct rdma_queue *queue;
	int ret;
	
	queue = &ctrl->queues[idx];

	queue->id = idx;
	spin_lock_init(&queue->lock);

	queue->ctrl = ctrl;

	queue->cm_error = -ETIMEDOUT;
	init_completion(&queue->cm_done);
	queue->cm_id = rdma_create_id(&init_net, dcc_rdma_cm_handler, queue,
			RDMA_PS_TCP, IB_QPT_RC);
	if (IS_ERR(queue->cm_id)) {
		pr_err("failed to create cm id: %ld\n", PTR_ERR(queue->cm_id));
		return -ENODEV;
	}

	ret = rdma_resolve_addr(queue->cm_id, &ctrl->src_addr, &ctrl->addr,
			CONNECTION_TIMEOUT_MS);
	if (ret) {
		pr_err("rdma_resolve_addr failed: %d", ret);
		goto out_destroy_cm_id;
	}

	ret = dcc_rdma_wait_for_cm(queue);
	if (ret) {
		pr_err("dcc_rdma_wait_for_cm failed");
		goto out_destroy_cm_id;
	}
	
	return 0;

out_destroy_cm_id:
	rdma_destroy_id(queue->cm_id);
	return ret;
}

static int dcc_rdma_init_queue(struct dcc_rdma_ctrl *ctrl)
{
	int ret, i;

	for (i = 0; i < rdma_config.num_qps; ++i) {
		ret = __dcc_rdma_init_queue(ctrl, i);
		if (ret) {
			pr_err("failed to alloc queue: %d", i);
			goto out_free_queues;
		}
	}

	return 0;

out_free_queues:
	dcc_rdma_stopandfree_queues(ctrl);

	return ret;
}

static struct dcc_rdma_ctrl *dcc_rdma_alloc_control(int id)
{
	struct dcc_rdma_ctrl *ctrl;
	int ret;

	ctrl = kzalloc(sizeof(struct dcc_rdma_ctrl), GFP_KERNEL);
	if (!ctrl) {
		pr_err("failed to allocate ctrl");
		goto out_err;
		return ERR_PTR(-ENOMEM);
	}
	
	ctrl->id = id;

	ctrl->queues = kzalloc(sizeof(struct rdma_queue) * rdma_config.num_qps, 
			GFP_KERNEL);
	if (!ctrl->queues) {
		pr_err("failed to allocate queues");
		goto out_free_ctrl;
		ctrl = ERR_PTR(-ENOMEM);
	}
	
	ctrl->mm = rdma_mem_init();
	if (IS_ERR(ctrl->mm)) {
		goto out_free_queues;
	}
	
	ctrl->ht = hashtable_init(1024*1024);
	if (!ctrl->ht) {
		goto out_free_mm;	
	}
	
	ret = dcc_rdma_parse_ipaddr(&ctrl->addr_in, config.svr_ips[id]);
	if (ret) {
		pr_err("dcc_rdma_parse_ipaddr failed: %d", ret);
		goto out_free_ht;
		ctrl = ERR_PTR(-EINVAL);
	}
	ctrl->addr_in.sin_port = cpu_to_be16(config.svr_port);

	ret = dcc_rdma_parse_ipaddr(&ctrl->src_addr_in, config.cli_ip);
	if (ret) {
		pr_err("dcc_rdma_parse_ipaddr failed: %d", ret);
		goto out_free_ht;
		ctrl = ERR_PTR(-EINVAL);
	}
	/* no need to set the port on the src_addr */
	
	pr_info("will try to connect to %s:%d", config.svr_ips[id], 
			config.svr_port);

	return ctrl;

out_free_ht:
	hashtable_exit(ctrl->ht);
out_free_mm:
	rdma_mem_exit(ctrl->mm);
out_free_queues:
	kfree(ctrl->queues);
out_free_ctrl:
	kfree(ctrl);
out_err:
	return ctrl;
}

static void dcc_rdma_free_control(struct dcc_rdma_ctrl *ctrl)
{
	pr_err("%s: id=%d", __func__, ctrl->id);
	rdma_mem_exit(ctrl->mm);
	ib_dealloc_pd(ctrl->rdev->pd);
	kfree(ctrl->queues);
	kfree(ctrl->rdev);
	kfree(ctrl);
}

static void dcc_rdma_addone(struct ib_device *dev)
{
	pr_info("dcc_rdma_addone() = %s", dev->name);
}

static void dcc_rdma_removeone(struct ib_device *ib_device, void *client_data)
{
	pr_err("%s", __func__);
}

static struct ib_client dcc_rdma_ib_client = {
	.name   = "dcc_rdma",
	.add    = dcc_rdma_addone,
	.remove = dcc_rdma_removeone
};

void dcc_rdma_set_default_config(void) 
{
	rdma_config.num_data_qps = num_online_cpus() - 1;
	rdma_config.num_qps = rdma_config.num_data_qps;
	rdma_config.num_msgs = 512;
}

int dcc_rdma_init(void) 
{
	int i, ret;

	pr_info("* RDMA BACKEND *");

	dcc_rdma_set_default_config();

	ib_register_client(&dcc_rdma_ib_client);
	
	req_cache = kmem_cache_create("dcc_req_cache", 
			sizeof(struct rdma_req), 0,
			SLAB_TEMPORARY | SLAB_HWCACHE_ALIGN, NULL);
	if (!req_cache) {
		pr_err("no memory for cache allocation");
		ret = -ENOMEM;
		goto out_unregister_client;
	}
	
	ctrls = (struct dcc_rdma_ctrl **) kzalloc(
			sizeof(struct dcc_rdma_ctrl *) * config.num_svrs,
			GFP_KERNEL);
	if (!ctrls) {
		pr_err("failed to allocate ctrls");
		ret = -ENOMEM;
		goto out_free_req_cache;
	}

	/* TODO: allocation failure handling */
	for (i = 0; i < config.num_svrs; i++) {
		struct dcc_rdma_ctrl *ctrl;

		ctrl = dcc_rdma_alloc_control(i);
		ctrls[i] = ctrl;
		if (IS_ERR(ctrl)) {
			pr_err("could not init ctrl");
			ret = PTR_ERR(ctrl);
			goto out_free_ctrl;
		}

		ret = dcc_rdma_init_queue(ctrl);
		if (ret) {
			pr_err("could not init queue");
			ret = -ENOMEM;
			goto out_free_queues;
		}
		
		ret = dcc_rdma_recv_remotemr(ctrl);
		if (ret) {
			/* TODO: allocation failed handle */
			pr_err("could not setup remote memory region");
			return -ENOMEM;
		}

		ret = dcc_backend_init(i, ctrl);
		if (ret) {
			pr_err("could not init backend");
			return -ENOMEM;
		}

		pr_info("ctrl[%d] is ready for reqs", i);
	}
	
	return 0;

out_free_queues:
	for (i = 0; i < config.num_svrs; i++) {
		if (ctrls[i])
			dcc_rdma_stopandfree_queues(ctrls[i]);
	}
out_free_ctrl:
	for (i = 0; i < config.num_svrs; i++) {
		if (ctrls[i])
			kfree(ctrls[i]);
	}
	kfree(ctrls);
out_free_req_cache:
	if (req_cache)
		kmem_cache_destroy(req_cache);
out_unregister_client:
	ib_unregister_client(&dcc_rdma_ib_client);

	return -1;
}

void dcc_rdma_exit(struct dcc_rdma_ctrl *ctrl, bool last_flag) 
{
	dcc_rdma_stopandfree_queues(ctrl);
	
	hashtable_exit(ctrl->ht);

	dcc_rdma_free_control(ctrl);
	
	if (last_flag) {
		kfree(ctrls);
		kmem_cache_destroy(req_cache);
		ib_unregister_client(&dcc_rdma_ib_client);
	}
}
