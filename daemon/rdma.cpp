#include <iostream>
#include <thread>

#include <rdma/rdma_cma.h>
#include <cassert>
#include <unistd.h>

#include "rdma.hpp"
#include "config.hpp"

using namespace std;

//#define DCC_RDMA_DEBUG
#ifdef DCC_RDMA_DEBUG
#define dcc_rdma_debug(str, ...) printf("%s: " str, __func__, __VA_ARGS__)
#else 
#define dcc_rdma_debug(str, ...) do {} while (0)  
#endif

struct dcc_rdma_config_t rdma_config;
struct rdma_event_channel *ec;
struct rdma_cm_id *listener;

static int client_ctr = 0;
static int queue_ctr = 0;

extern struct dcc_clients **clients;


inline int __poll_cq(struct ibv_cq *cq, struct ibv_wc *wc, bool global) {
    int ne;

    do {
        ne = ibv_poll_cq(cq, 1, wc);
        if (ne < 0) {
            fprintf(stderr, "ibv_poll_cq failed\n");
            assert(0);
        }

        if (global && ne == 0)
            goto out;
    } while (ne < 1);

    if (wc->status != IBV_WC_SUCCESS) {
        fprintf(stderr, "wc_id: %lu failed status: %s (%d)\n", 
                wc->wr_id, ibv_wc_status_str(wc->status), wc->status);
        assert(0);
    }

out:
    return ne;
}

inline void poll_send_cq(struct rdma_queue *q) {
    struct ibv_wc wc = {};
    __poll_cq(q->qp->send_cq, &wc, false);
}

inline int post_recv(struct rdma_queue *q, void *addr) {
    struct ibv_sge sges[2] = {};
    struct ibv_recv_wr wr = {};
    struct ibv_recv_wr* bad_wr;
    int ret;

    wr.wr_id   = !addr ? 0 : (uint64_t) addr;
    wr.next    = NULL;
    wr.sg_list = sges;
    wr.num_sge = !addr ? 0 : 2;

    if (addr) {
        sges[0].addr   = (uint64_t) addr;
        sges[0].length = PAGE_SIZE;
        sges[0].lkey   = q->ctrl->mm->GetDataMMPool()->mr->lkey;

        sges[1].addr   = (uint64_t) ((uint64_t) addr + PAGE_SIZE);
        sges[1].length = sizeof(uint64_t);
        sges[1].lkey   = q->ctrl->mm->GetDataMMPool()->mr->lkey;
    }

    ret = ibv_post_recv(q->qp, &wr, &bad_wr);
    if (ret)
        fprintf(stderr, "failed to ibv_post_recv: %d\n", ret);

    return ret;
}

/************************* Setup rdma connection *************************/
static rdma_dev *get_rdma_device(struct rdma_queue *q) {
    if (!q->ctrl->rdev) {
        struct rdma_dev *dev;
        /* TODO: allocation failed out_error */
        dev = (struct rdma_dev *) malloc(sizeof(*dev));
        TEST_Z(dev);
        dev->verbs = q->cm_id->verbs;
        TEST_Z(dev->verbs);
        dev->pd = ibv_alloc_pd(dev->verbs);
        TEST_Z(dev->pd);

        q->ctrl->rdev = dev;
    }

    return q->ctrl->rdev;
}

static void destroy_device(struct dcc_rdma_ctrl *ctrl, bool last) {
    ctrl->mm->Deregister(ctrl, last); 

    ibv_dealloc_pd(ctrl->rdev->pd);
    free(ctrl->rdev);
    ctrl->rdev = NULL;
}

static void create_qp(struct rdma_queue *q) {
    struct ibv_qp_init_attr qp_attr = {};
    struct ibv_cq *send_cq, *recv_cq;

    send_cq = ibv_create_cq(q->cm_id->verbs, CQ_NUM_CQES, NULL, NULL, 
            0);
    recv_cq = ibv_create_cq(q->cm_id->verbs, CQ_NUM_CQES, NULL, NULL, 
            0);

    /* separate send_cq and recv_cq */
    qp_attr.send_cq = send_cq; 
    qp_attr.recv_cq = recv_cq;
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.cap.max_send_wr = QP_MAX_SEND_WR;
    qp_attr.cap.max_recv_wr = QP_MAX_RECV_WR;
    qp_attr.cap.max_send_sge = QP_MAX_SEND_SGE;
    qp_attr.cap.max_recv_sge = QP_MAX_RECV_SGE;

    TEST_NZ(rdma_create_qp(q->cm_id, q->ctrl->rdev->pd, &qp_attr));
    q->qp = q->cm_id->qp;
}

int on_connect_request(struct rdma_cm_id *id, struct rdma_conn_param *param) {
    struct rdma_conn_param cm_params = {};
    struct ibv_device_attr attrs = {};
    struct rdma_queue *q;
    struct rdma_dev *dev;
    int qid = queue_ctr++;

    q = &clients[client_ctr]->ctrl->queues[qid];

    q->id = qid;
    q->cm_id = id;
    id->context = q;

    TEST_Z(q->state == rdma_queue::INIT);

    dev = get_rdma_device(q); 
    /* only once per client */
    if (qid == 0)
        q->ctrl->mm->Register(q);

    create_qp(q);

    TEST_NZ(ibv_query_device(dev->verbs, &attrs));

    //printf("attrs: max_qp=%d, max_qp_wr=%d, max_cq=%d max_cqe=%d "
    //        "max_qp_rd_atom=%d, max_qp_init_rd_atom=%d\n", attrs.max_qp,
    //        attrs.max_qp_wr, attrs.max_cq, attrs.max_cqe,
    //        attrs.max_qp_rd_atom, attrs.max_qp_init_rd_atom);

    //printf("ctrl attrs: initiator_depth=%d responder_resources=%d\n",
    //        param->initiator_depth, param->responder_resources);

    // the following should hold for initiator_depth:
    // initiator_depth <= max_qp_init_rd_atom, and
    // initiator_depth <= param->initiator_depth
    cm_params.initiator_depth = param->initiator_depth;
    // the following should hold for responder_resources:
    // responder_resources <= max_qp_rd_atom, and
    // responder_resources >= param->responder_resources
    cm_params.responder_resources = param->responder_resources;
    cm_params.rnr_retry_count = param->rnr_retry_count;
    cm_params.flow_control = param->flow_control;

    TEST_NZ(rdma_accept(q->cm_id, &cm_params));

    return 0;
}

void send_mr(struct rdma_queue *q) 
{
    struct dcc_rdma_ctrl *ctrl = q->ctrl;
    struct ibv_send_wr wr = {};
    struct ibv_send_wr *bad_wr;
    struct ibv_sge sge = {};
    struct mm_info server_mm_info = {};
    struct ibv_mr *mr;

    printf("connected. sending memory region info.\n");

    mr = ctrl->mm->GetDataMMPool()->mr;

    printf("servermr addr=%p, key=%u, length=%lu\n", mr->addr, mr->rkey, 
            mr->length);

    server_mm_info.baseaddr = (uint64_t) mr->addr;
    server_mm_info.key      = mr->rkey;
    server_mm_info.len      = mr->length / PAGE_SIZE;
    server_mm_info.cli_id   = q->ctrl->id;

    wr.opcode = IBV_WR_SEND;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;

    sge.addr = (uint64_t) &server_mm_info;
    sge.length = sizeof(struct mm_info);

    TEST_NZ(ibv_post_send(q->qp, &wr, &bad_wr));
}

void exchange_mr(struct rdma_queue *q) {
    send_mr(q);
}

/* 
 * TODO: 
 * client -> server: request memory registration 
 * server -> client: allocate requirements or not
 */
int on_connection(struct rdma_queue *q) {
    TEST_Z(q->state == rdma_queue::INIT);

    if (q == &q->ctrl->queues[0]) {
        exchange_mr(q);
        // XXX: polling at init_rdma()
    }

    q->state = rdma_queue::CONNECTED;
    return q->state;
}

int on_disconnect(struct rdma_queue *q) {
    if (q->state == rdma_queue::CONNECTED) {
        q->state = rdma_queue::INIT;

        rdma_destroy_qp(q->cm_id);
        if (q->id < rdma_config.num_data_qps) {
            /* data qp */
            ibv_destroy_cq(q->qp->send_cq);
            ibv_destroy_cq(q->qp->recv_cq);       
        } else {
            /* filter qp */
            //ibv_destroy_cq(q->event_cq);
#if 0
            int err = ibv_destroy_comp_channel(q->channel);
            if (err) {
                fprintf(stderr, "Q%d: ibv_destroy_comp_channel() failed, %d\n", 
                        q->id, err);
                return -1;
            }
#endif
        }
#if 1
        if (!(q->id < rdma_config.num_data_qps))
            ibv_destroy_cq(q->event_cq);
#endif
        rdma_destroy_id(q->cm_id);
    }
    printf("Q%d: CM connection closed\n", q->id);

    return 0;
}

int on_event(struct rdma_cm_event *event) {
    struct rdma_queue *q = (struct rdma_queue *) event->id->context;

    switch (event->event) {
        case RDMA_CM_EVENT_CONNECT_REQUEST:
            return on_connect_request(event->id, &event->param.conn);
        case RDMA_CM_EVENT_ESTABLISHED:
            return on_connection(q);
        case RDMA_CM_EVENT_DISCONNECTED:
            on_disconnect(q);
            return -1;
        default:
            printf("unknown event: %s\n", rdma_event_str(event->event));
            return -2;
    }
}

int handle_rdma_cm_event(struct rdma_event_channel *ec,
        struct rdma_cm_event *event) {
    int ret;

    while (!rdma_get_cm_event(ec, &event)) {
        struct rdma_cm_event event_copy;

        memcpy(&event_copy, event, sizeof(*event));
        rdma_ack_cm_event(event);

        ret = on_event(&event_copy);
        if (ret) 
            break;
    }

    return ret;
}

int run_rdma(int tcp_port, int num_clients) {
    struct rdma_cm_event *event;
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(tcp_port)
    };

    TEST_Z(ec = rdma_create_event_channel());
    TEST_NZ(rdma_create_id(ec, &listener, NULL, RDMA_PS_TCP));
    TEST_NZ(rdma_bind_addr(listener, (struct sockaddr *)&addr));
    TEST_NZ(rdma_listen(listener, 10));

    printf("listening on port %d\n\n", ntohs(rdma_get_src_port(listener)));

    for (int cli_id = 0; cli_id < num_clients; cli_id++) {
        struct dcc_rdma_ctrl *ctrl = clients[cli_id]->ctrl;

        ctrl->id = cli_id;

        queue_ctr = 0;
        for (int i = 0; i < rdma_config.num_qps; ++i) {
            printf("waiting for queue connection: %d:%d\n", cli_id, i);
            struct rdma_queue *q = &ctrl->queues[i];
            /* handle connection requests */
            if (handle_rdma_cm_event(ec, event) < 0) {
                fprintf(stderr, "not conn event: %d", event->event);
                assert(0);
            }
        }

        /* cq check for sending mr here: client might not be connected yet */
        poll_send_cq(&ctrl->queues[0]);
        client_ctr++;
    }

    printf("done connecting all queues\n");

    return 0;
    // TODO: allocation failed out_err
//out_err:
//    rdma_destroy_event_channel(ec);
//    rdma_destroy_id(listener);
//    destroy_device(ctrl;
}

struct dcc_rdma_ctrl *alloc_control() {
    struct dcc_rdma_ctrl *ctrl;

    ctrl = (struct dcc_rdma_ctrl *) malloc(sizeof(struct dcc_rdma_ctrl));
    TEST_Z(ctrl);
    memset(ctrl, 0, sizeof(struct dcc_rdma_ctrl));

    /* TODO: allocation failed out_error */
    ctrl->queues = (struct rdma_queue *) malloc(sizeof(struct rdma_queue) *
            rdma_config.num_qps);
    TEST_Z(ctrl->queues);
    memset(ctrl->queues, 0, sizeof(struct rdma_queue) * rdma_config.num_qps);
    for (int i = 0; i < rdma_config.num_qps; ++i) {
        ctrl->queues[i].ctrl = ctrl;
        ctrl->queues[i].state = rdma_queue::INIT;
    }

    return ctrl;
}

void dcc_rdma_set_default_config() {
    rdma_config.num_data_qps = 8;
    rdma_config.num_qps = rdma_config.num_data_qps;
    rdma_config.num_msgs = 512;
}

struct dcc_rdma_ctrl *init_rdma(int cid) {
    struct dcc_rdma_ctrl *ctrl;

    if (cid == 0) {
        dcc_rdma_set_default_config();
    }

    ctrl = alloc_control();
    if (!ctrl)
        return NULL;

    ctrl->mm = new RdmaMem();

    ctrl->mm->Allocate(cid, ctrl);

    return ctrl;
}

/* XXX */
int exit_rdma(void) {
    struct rdma_cm_event *event;
    struct dcc_rdma_ctrl *ctrl;

    for (int i = 0; i < client_ctr; i++) {
        for (int j = 0; j < rdma_config.num_qps; j++) {
            /* handle disconnects */
            if (handle_rdma_cm_event(ec, event) != -1) {
                fprintf(stderr, "not disconn event: %d", event->event);
                assert(0);
            }
        }

        /* suppose disconnect order is same as connection order */
        ctrl = clients[i]->ctrl;
        destroy_device(ctrl, i == client_ctr - 1 ? true : false);

        free(ctrl->queues);
        free(ctrl);
    }

    rdma_destroy_event_channel(ec);
    rdma_destroy_id(listener);

    return 0;
}
