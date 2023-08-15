#include <linux/slab.h>
#include <linux/cpumask.h>

#include "rdma.h"
#include "backend.h"
#include "util/crc8.h"

struct dcc_rdma_ctrl **ctrls;
struct kmem_cache *req_cache;
struct dcc_rdma_config_t rdma_config;

extern int dcc_backend_init(int, struct dcc_rdma_ctrl *); 
extern int dcc_backend_update_filter(struct dcc_rdma_ctrl *); 

//#define DCC_RDMA_DEBUG
#ifdef DCC_RDMA_DEBUG
#define dcc_rdma_debug(fmt, ...)     \
	pr_err("%s: "fmt, __func__, ##__VA_ARGS__)
#else
#define dcc_rdma_debug(fmt, ...)     do {} while (0)
#endif

static inline void poll_send_cq(struct rdma_queue *q) 
{
	struct ib_wc wc = {};
	__poll_cq(&wc, q->qp->send_cq);
}

static inline int poll_recv_cq(struct rdma_queue *q, int *etc, int *crc)
{
	struct ib_wc wc = {};
	int ret;
	int mid, msg_type, tx_state;
	
	__poll_cq(&wc, q->qp->recv_cq);
	
	if (!wc.ex.imm_data)
		return 0;

	bit_unmask(ntohl(wc.ex.imm_data), &mid, &msg_type, &tx_state, 
			etc, crc);

	switch (tx_state) {
		case TX_WRITE_COMMITTED:
		case TX_INV_PAGE_COMMITED:
			ret = 0;
			break;
		case TX_READ_COMMITTED:
			ret = 0;
			break;
		case TX_READ_ABORTED:
		case TX_INV_PAGE_ABORTED:
			ret = 1;
			break;
		default:
			pr_err("Unknown tx_state: %d", tx_state);
			ret = -1;
	}

	return ret;
}

void send_page_work_handler(struct work_struct *work)
{
	struct work_data *data = (struct work_data *) work;
	struct rdma_queue *q = data->q;
	struct dcc_rdma_ctrl *ctrl = q->ctrl;
	struct ib_sge *sges;
	struct ib_rdma_wr *rdma_wrs;
	const struct ib_send_wr *bad_wr;
	int msg_id = data->msg_id;
	int ret;
	int etc = 0;
	int i;
	u32 imm;
	
	sges = kzalloc(sizeof(struct ib_sge) * rdma_config.num_put_batch, 
			GFP_ATOMIC);
	rdma_wrs = kzalloc(sizeof(struct ib_rdma_wr) * rdma_config.num_put_batch, 
			GFP_ATOMIC);

	for (i = 0; i < rdma_config.num_put_batch; i++) {
		size_t len = sizeof(u64) + PAGE_SIZE;
		int sending_msg_id = msg_id - (rdma_config.num_put_batch - 1) + i;

		/* setup imm data */
		imm = htonl(bit_mask(sending_msg_id, MSG_WRITE, TX_WRITE_BEGIN, 
					0, 0));

		sges[i].addr   = KEYPAGE_MM_POOL(ctrl).dma_addr + 
			len * sending_msg_id;
		sges[i].length = len;
		sges[i].lkey   = ctrl->rdev->pd->local_dma_lkey;

		rdma_wrs[i].wr.next        = i < (rdma_config.num_put_batch - 1) ? 
			&rdma_wrs[i + 1].wr : NULL;
		rdma_wrs[i].wr.wr_id       = 0;
		rdma_wrs[i].wr.sg_list     = &sges[i];
		rdma_wrs[i].wr.num_sge     = 1;
		rdma_wrs[i].wr.opcode      = IB_WR_SEND_WITH_IMM;
		rdma_wrs[i].wr.send_flags  = i < (rdma_config.num_put_batch - 1) ? 
			0 : IB_SEND_SIGNALED;
		rdma_wrs[i].wr.ex.imm_data = imm;
	}

	mutex_lock(&q->mtx);
	
	ret = ib_post_send(q->qp, &rdma_wrs[0].wr, &bad_wr);
	if (unlikely(ret))
		pr_err("%s: ib_post_send failed: %d", __func__, ret);

	poll_send_cq(q);

	clear_bit(msg_id / rdma_config.num_put_batch, &ctrl->wait);

	if (msg_id == rdma_config.num_msgs - 1) { 
		ret = poll_recv_cq(q, &etc, NULL);
		post_recv(q, 0, 0);
	}
	
	mutex_unlock(&q->mtx);

	kfree(sges);
	kfree(rdma_wrs);

	kfree(data);
}

/* (Page, key) (Page, key) */
static inline void copy_to_send_buffer(struct dcc_rdma_ctrl *ctrl, u64 key, 
		struct page *page, int msg_id) 
{
	u64 off = msg_id * (PAGE_SIZE + sizeof(u64));
	u64 page_loc = (u64) KEYPAGE_MM_POOL(ctrl).ptr + off;
	u64 key_loc = page_loc + PAGE_SIZE;
	
	memcpy((void *) page_loc, page_address(page), PAGE_SIZE);
	memcpy((void *) key_loc, &key, sizeof(u64));
}

int dcc_rdma_send_page(struct dcc_rdma_ctrl *ctrl, struct page *page, u64 key)
{
	int queue_id, msg_id;
	int ret = 0;
	struct rdma_queue *q;
	int num_msgs = rdma_config.num_msgs;
	unsigned long flags;
	struct work_data *data;
	int num_retry = 0;

	q = get_rdma_queue(ctrl, 0);
	queue_id = q->id;

	spin_lock_irqsave(&q->lock, flags);

	msg_id = q->msg_id++ % num_msgs;
	
	while (test_bit(msg_id / rdma_config.num_put_batch, &ctrl->wait)) {
		//pr_info("key=%llu, msg_id=%d wait!!", key, msg_id);
		if (num_retry++ > 5) { 
			q->msg_id--;
			spin_unlock_irqrestore(&q->lock, flags);
			return -6;
		}
		udelay(1);
	}

	copy_to_send_buffer(ctrl, key, page, msg_id);

	if (msg_id % rdma_config.num_put_batch != 
			rdma_config.num_put_batch - 1) {
		spin_unlock_irqrestore(&q->lock, flags);
		return 0;
	}
	
	data = kmalloc(sizeof(struct work_data), GFP_ATOMIC);
	INIT_WORK(&data->work, send_page_work_handler);
	data->q = q;
	data->msg_id = msg_id;

	set_bit(msg_id / rdma_config.num_put_batch, &ctrl->wait);

	spin_unlock_irqrestore(&q->lock, flags);

	if (!schedule_work(&data->work))
		pr_err("%s: failed to schedule_work", __func__);
	
	return ret;
}

/* for get_page2 */
int dcc_rdma_write_msg(struct dcc_rdma_ctrl *ctrl, struct page *page, u64 key)
{
	int msg_id = 0;
	int ret;
	struct dcc_metadata meta = {};
	u32 imm;
	struct rdma_req *req;
	struct rdma_queue *q;
	struct ib_sge sge = {};
	struct ib_rdma_wr rdma_wr = {};
	const struct ib_send_wr *bad_wr;
	struct rdma_dev *rdev = ctrl->rdev; 
	int msg_type = MSG_READ;
	int tx_type = TX_READ_BEGIN;
	int crc;
	unsigned long flags;

	/* 0: put, 1: inv, 2 ~ N: get */
#if 1
	q = get_rdma_queue(ctrl, smp_processor_id() % rdma_config.num_get_qps + 2);
#else
	q = get_rdma_queue(ctrl, key % rdma_config.num_get_qps + 2);
#endif
	/* DMA PAGE */
	ret = get_req_for_page(&req, rdev->dev, page, DMA_FROM_DEVICE);
	if (unlikely(ret)) {
		pr_err("get_req_for_page failed: %d", ret);
	}

	/* set metadata */
	meta.key = key;
	meta.raddr = req->dma;

	/* setup imm data */
	imm = htonl(bit_mask(msg_id, msg_type, tx_type, 0, 0));

	/* request (key, raddr) */
	sge.addr   = (u64) &meta;
	sge.length = sizeof(struct dcc_metadata);
	sge.lkey   = rdev->pd->local_dma_lkey;

	rdma_wr.wr.next        = NULL;
	rdma_wr.wr.wr_id       = 0;
	rdma_wr.wr.sg_list     = &sge;
	rdma_wr.wr.num_sge     = 1;
	rdma_wr.wr.opcode      = IB_WR_RDMA_WRITE_WITH_IMM;

	rdma_wr.wr.send_flags  = IB_SEND_SIGNALED | IB_SEND_INLINE;
	rdma_wr.wr.ex.imm_data = imm;
	rdma_wr.remote_addr    = SVR_MM_INFO(ctrl).baseaddr + 
		GET_METADATA_OFFSET(q->id, msg_id, msg_type);
	rdma_wr.rkey           = SVR_MM_INFO(ctrl).key;

	//mutex_lock(&q->mtx);
	spin_lock_irqsave(&q->lock, flags);

	/* to keep key order with invalidation */
	//while (hashtable_get(ctrl->ht, key) == key) { }

	/* request => reply */
	ret = ib_post_send(q->qp, &rdma_wr.wr, &bad_wr);
	if (unlikely(ret)) {
		pr_err("ib_post_send failed: %d", ret);
	}
	poll_send_cq(q);
	
	ret = poll_recv_cq(q, NULL, &crc);
	
	//mutex_unlock(&q->mtx);
	spin_unlock_irqrestore(&q->lock, flags);

	post_recv(q, 0, 0);

	if (!ret && ((uint8_t) crc != CRC8(page_address(page)))) {
		ret = -4;
	}
	
	ib_dma_unmap_page(rdev->dev, req->dma, PAGE_SIZE,
			DMA_FROM_DEVICE);
	kmem_cache_free(req_cache, req);

	dcc_rdma_debug("key=%llu, qid=%d, ret=%d", key, queue_id, ret);
	
	return ret;
}

/* for inv_page2 */
int dcc_rdma_write_msg2(struct dcc_rdma_ctrl *ctrl, struct page *page, u64 key)
{
	int msg_id;
	int ret;
	u32 imm;
	struct rdma_queue *q;
	struct ib_sge sge = {};
	struct ib_rdma_wr rdma_wr = {};
	const struct ib_send_wr *bad_wr;
	struct rdma_dev *rdev = ctrl->rdev; 
	int num_msgs = rdma_config.num_msgs;
	unsigned long flags;

	q = get_rdma_queue(ctrl, 1);

	//hashtable_insert(ctrl->ht, key, key);
	
	/* request (key, raddr) */
	sge.addr   = (u64) &key;
	sge.length = sizeof(u64);
	sge.lkey   = rdev->pd->local_dma_lkey;

	rdma_wr.wr.next        = NULL;
	rdma_wr.wr.wr_id       = 0;
	rdma_wr.wr.sg_list     = &sge;
	rdma_wr.wr.num_sge     = 1;
	rdma_wr.wr.opcode      = IB_WR_RDMA_WRITE_WITH_IMM;
	
	spin_lock_irqsave(&q->lock, flags);
	msg_id = q->msg_id++ % num_msgs;
	/* setup imm data */
	imm = htonl(bit_mask(msg_id, MSG_INV_PAGE, TX_INV_PAGE_BEGIN, 0, 0));
	rdma_wr.wr.ex.imm_data = imm;
	rdma_wr.wr.send_flags  = msg_id < num_msgs - 1 ? 
		IB_SEND_INLINE : IB_SEND_SIGNALED | IB_SEND_INLINE;
	rdma_wr.remote_addr    = SVR_MM_INFO(ctrl).baseaddr + 
		GET_METADATA_OFFSET(q->id, msg_id, MSG_INV_PAGE);
	rdma_wr.rkey           = SVR_MM_INFO(ctrl).key;

	/* request => reply */
	ret = ib_post_send(q->qp, &rdma_wr.wr, &bad_wr);
	if (unlikely(ret)) {
		pr_err("ib_post_send failed: %d", ret);
	}

	if (msg_id < num_msgs - 1) {
		spin_unlock_irqrestore(&q->lock, flags);
	} else {
		poll_send_cq(q);
		ret = poll_recv_cq(q, NULL, NULL);
		spin_unlock_irqrestore(&q->lock, flags);
		post_recv(q, 0, 0);
	}

	//hashtable_remove(q->ctrl->ht, key);

	dcc_rdma_debug("key=%llu, qid=%d, ret=%d", key, q->id, ret);

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

static inline void process_send(struct ib_wc *wc) 
{
	//pr_err("%s", __func__);
}

/* Update client BF */
static inline void process_imm(struct rdma_queue *q, struct ib_wc *wc) {
	switch (wc->ex.imm_data) {
		case FILTER_UPDATE_END:
			dcc_backend_update_filter(q->ctrl);
			break;
		default:
			pr_err("Unknown imm: %d", wc->ex.imm_data);
	}
}

static void filter_event_handler(struct ib_cq *cq, void *arg) {
	struct rdma_queue *q = (struct rdma_queue *)arg;
	struct ib_wc wc = {};
	int ret, err;

retry:
	while ((ret = ib_poll_cq(q->event_cq, 1, &wc)) > 0) {
		//pr_err("%s", __func__);
		if (wc.status) {
			pr_err("%s: failed status %s(%d) for wr_id %llu", 
					__func__, ib_wc_status_msg(wc.status), 
					wc.status, wc.wr_id);
			goto error;
		}

		switch (wc.opcode) {
			case IB_WC_RDMA_WRITE:
			case IB_WC_SEND:
				process_send(&wc);
				break;
			case IB_WC_RECV:
			case IB_WC_RECV_RDMA_WITH_IMM:
				process_imm(q, &wc);
				post_recv(q, 0, 0);
				break;
			default:
				pr_err("Unexpected opcode %d, Shutting down", 
						wc.opcode);
				goto error;	/* TODO for rmmod */
			//wake_up_interruptible(&cb->sem);
			//ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);
		}
	}

	err = ib_req_notify_cq(q->event_cq, 
			IB_CQ_NEXT_COMP | IB_CQ_REPORT_MISSED_EVENTS);
	BUG_ON(err < 0);
	if (err > 0)
		goto retry;

error:
	return;
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
	pr_info("server_mm_info num_hash=%u, num_bits=%u, num_rmem_pages=%u", 
			server_mm_info->info[0], server_mm_info->info[1], 
			server_mm_info->info[2]);

out_free_req:
	kmem_cache_free(req_cache, req);
out:
	return ret;
}

static int dcc_rdma_send_localmr(struct dcc_rdma_ctrl *ctrl) 
{
	int ret;
	struct mm_info *filter_mm_info = &FILTER_MM_INFO(ctrl);

	ret = dcc_rdma_send_msg(&ctrl->queues[0], (u64) filter_mm_info, 
			sizeof(struct mm_info), 0);
	/* this delay doesn't really matter, only happens once */
	poll_send_cq(&ctrl->queues[0]);

	pr_info("filter_mm_info addr=0x%llx, len=%u, key=%u", 
			filter_mm_info->baseaddr, 
			filter_mm_info->len, filter_mm_info->key);

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
	if (q->id < rdma_config.num_data_qps) {
		q->send_cq = ib_alloc_cq(ibdev, q, CQ_NUM_CQES, 0, 
				IB_POLL_DIRECT);
		q->recv_cq = ib_alloc_cq(ibdev, q, CQ_NUM_CQES, 0, 
				IB_POLL_DIRECT);
		
		init_attr.send_cq = q->send_cq; 
		init_attr.recv_cq = q->recv_cq;
	} else {
		struct ib_cq_init_attr cq_init_attr = { 
			.cqe = CQ_NUM_CQES,
			.comp_vector = 0
		};

		q->event_cq = ib_create_cq(ibdev, filter_event_handler, NULL,
				q, &cq_init_attr);
		if (IS_ERR(q->event_cq)) {
			pr_err("ib_create_cq failed");
		}

		ret = ib_req_notify_cq(q->event_cq, IB_CQ_NEXT_COMP);
		if (ret) {
			pr_err("ib_req_notify_cq failed");
		}

		init_attr.send_cq = q->event_cq;
		init_attr.recv_cq = q->event_cq;
	}

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
	if (!(q->id < rdma_config.num_data_qps))
		ib_free_cq(q->event_cq);
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
	mutex_init(&queue->mtx);

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
	
	ctrl->ht = hashtable_init(128);
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
	rdma_config.num_get_qps = 6;
	rdma_config.num_data_qps = 2 + rdma_config.num_get_qps;
	rdma_config.num_filter_qps = 1;
	rdma_config.num_qps = rdma_config.num_data_qps + 
		rdma_config.num_filter_qps;
	rdma_config.num_msgs = 512;	// 1 MB
	rdma_config.num_put_batch = 32; // 128 KB
	rdma_config.get_metadata_size = sizeof(uint64_t) * 2;
	rdma_config.inv_metadata_size = sizeof(uint64_t) * rdma_config.num_msgs;
	rdma_config.metadata_size = rdma_config.get_metadata_size + 
		rdma_config.inv_metadata_size;
	rdma_config.metadata_mr_size = rdma_config.num_data_qps * 
		rdma_config.metadata_size;
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
		
		ret = rdma_mem_register_region(ctrl);
		if (ret) {
			pr_err("could not register mr");
			ret = -ENOMEM;
			goto out_free_queues;
		}

		ret = dcc_rdma_recv_remotemr(ctrl);
		if (ret) {
			/* TODO: allocation failed handle */
			pr_err("could not setup remote memory region");
			return -ENOMEM;
		}
		
		ret = rdma_mem_alloc_region(ctrl);
		if (ret) {
			pr_err("could not allocate filter mem pool");
			return -ENOMEM;
		}

		ret = dcc_rdma_send_localmr(ctrl);
		if (unlikely(ret)) {
			pr_err("could not send local memory region");
			return -ENODEV;
		}

		pre_post_recvs(ctrl);

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

	rdma_mem_free_region(ctrl);
	
	dcc_rdma_free_control(ctrl);
	
	if (last_flag) {
		kfree(ctrls);
		kmem_cache_destroy(req_cache);
		ib_unregister_client(&dcc_rdma_ib_client);
	}
}
