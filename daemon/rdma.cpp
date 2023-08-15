#include <iostream>
#include <thread>

#include <rdma/rdma_cma.h>
#include <cassert>
#include <unistd.h>

#include "rdma.hpp"
#include "kv.hpp"
#include "stats.hpp"
#include "util/crc8.hpp"
#include "util/fast_mono_clock.hpp"
#include "cpu-stats/CPUMonitor.hpp"
#include "config.hpp"
#include "breakdown.hpp"

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
static Stats *stats;

extern struct dcc_clients **clients;
extern KV *kv;
CPUMonitor *cpu_monitor;


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
    struct ibv_sge sge = {};
    struct ibv_recv_wr wr = {};
    struct ibv_recv_wr* bad_wr;
    int ret;

    wr.wr_id   = !addr ? 0 : (uint64_t) addr;
    wr.next    = NULL;
    wr.sg_list = &sge;
    wr.num_sge = !addr ? 0 : 1;

    if (addr) {
        sge.addr   = (uint64_t) addr;
        sge.length = sizeof(uint64_t) + PAGE_SIZE;
        sge.lkey   = q->ctrl->mm->GetDataMMPool()->mr->lkey;
    }

    ret = ibv_post_recv(q->qp, &wr, &bad_wr);
    if (ret)
        fprintf(stderr, "failed to ibv_post_recv: %d\n", ret);

    return ret;
}

inline int send_reply(struct rdma_queue *q, uint32_t imm_data, void *value, 
        uint64_t raddr) {
    unsigned int send_inline = !value ? IBV_SEND_INLINE : 0;
    struct ibv_sge sge = {};
    struct ibv_send_wr wr = {};
    struct ibv_send_wr *bad_wr;
    int ret;
    struct mm_pool *data_mm_pool = q->ctrl->mm->GetDataMMPool(); 
    struct mm_region *client_mr = q->ctrl->mm->GetClientMR();

    wr.wr_id                = 0;
    wr.next                 = NULL;
    wr.sg_list              = &sge;
    wr.num_sge              = !value ? 0 : 1;
    wr.opcode               = IBV_WR_RDMA_WRITE_WITH_IMM;
    wr.send_flags           = IBV_SEND_SIGNALED | send_inline;
    wr.imm_data             = imm_data;
    wr.wr.rdma.remote_addr  = raddr;
    wr.wr.rdma.rkey         = client_mr->info.key; 

    if (value) {
        sge.addr    = (uint64_t) value;
        sge.length  = PAGE_SIZE;
        sge.lkey    = data_mm_pool->mr->lkey;
    }

    ret = ibv_post_send(q->qp, &wr, &bad_wr);
    if (ret) {
        fprintf(stderr, "ibv_post_send failed with %d\n", ret);
        assert(0);
    }

    return ret;
}

int send_bf(struct rdma_queue *q) {
    struct dcc_rdma_ctrl *ctrl = q->ctrl;
    int cid = ctrl->id;
    struct ibv_sge sge = {};
    struct ibv_send_wr wr = {};
    struct ibv_send_wr *bad_wr = NULL; 
    int ret;

    //printf("%s\n", __func__);

    /* Send entire updated bf to the client */
    sge.addr   = kv->GetCBF(cid)->GetBFAddr();
    sge.length = kv->GetCBF(cid)->GetNumLongs() * sizeof(unsigned long);
    sge.lkey   = ctrl->mm->GetFilterMMPool()->mr->lkey;

    wr.wr_id               = 0;
    wr.next                = NULL;
    wr.sg_list             = &sge;
    wr.num_sge             = 1;
    wr.opcode              = IBV_WR_RDMA_WRITE_WITH_IMM;
    wr.send_flags          = IBV_SEND_SIGNALED;
    wr.imm_data            = (uint32_t) FILTER_UPDATE_END;
    wr.wr.rdma.remote_addr = ctrl->mm->GetClientMR()->info.baseaddr;
    wr.wr.rdma.rkey        = ctrl->mm->GetClientMR()->info.key;

    ret = ibv_post_send(q->qp, &wr, &bad_wr);
    if (ret) {
        fprintf(stderr, "ibv_post_send failed with %d\n", ret);
        assert(0);
    }

    return ret;
}

void process_put(struct rdma_queue *q, int cid, int qid, int mid, 
        uint64_t recv_addr) { 
    uint64_t meta_region = (uint64_t) q->ctrl->mm->GetMetaMMPool()->mr->addr;
    uint64_t key = *(uint64_t *) (recv_addr + PAGE_SIZE);
    Value_t value;
    uint32_t imm_data;
    int ret;
    dcc_declare_ts(all);

    dcc_start_ts(all);
    value = kv->Allocate(cid, -1);
    if (value == (void *) INVALID) {
        post_recv(q, (Value_t) recv_addr);
        goto out;
    } 
    post_recv(q, value);

    kv->Insert(key, (Value_t) recv_addr, cid, qid);

out:
    if (mid == rdma_config.num_msgs - 1) {
        int etc = cpu_monitor->GetCPUUsage() > 50 ? 1 : 0; /* XXX: */

        imm_data = htonl(bit_mask(mid, MSG_WRITE_REPLY, TX_WRITE_COMMITTED, 
                    etc, 0));
        if (send_reply(q, imm_data, NULL, 0)) {
            fprintf(stderr, "cannot reply put request\n");
            assert(0);       
        }
        dcc_end_ts(BR_PUTS, all);
        poll_send_cq(q);
    } else {
        dcc_end_ts(BR_PUTS, all);
    }

    dcc_rdma_debug("(%d, %d, %d) key=%lu, recv_addr=0x%lx\n", 
            cid, qid, mid, key, recv_addr);
}

static void process_get(struct rdma_queue *q, int cid, int qid, int mid) {
    uint64_t meta_region = (uint64_t) q->ctrl->mm->GetMetaMMPool()->mr->addr;
    Key_t key = *(uint64_t *) KEY_META_REGION(meta_region, qid, mid, MSG_READ);
    Value_t value;
    uint64_t raddr = *(uint64_t *) ADDR_META_REGION(meta_region, qid, mid);
    uint32_t imm_data;
    int ret;
    dcc_declare_ts(all);

    dcc_start_ts(all);
    if (config.exclusive_policy) {
        value = kv->Remove(key, cid, qid);
    } else {
        value = kv->GetWithFlag(key, cid, qid, true);
    }

    imm_data = htonl(bit_mask(mid, MSG_READ_REPLY, 
                value ? TX_READ_COMMITTED : TX_READ_ABORTED,
                0, value ? CRC8(value) : 0));

    if (send_reply(q, imm_data, value, raddr)) {
        fprintf(stderr, "cannot reply get request\n");
        assert(0);
    }
    dcc_end_ts(BR_GETS, all);
    poll_send_cq(q);

    dcc_start_ts(all);
    if (value) {
        if (config.exclusive_policy)
            kv->Free(value, cid, -1); 
        else
            kv->GetWithFlag(key, cid, qid, false); 
    }
    dcc_end_ts(BR_GETS, all);

    dcc_rdma_debug("(%d, %d, %d) key=%lu, value=%lu, raddr=0x%lx\n", 
            cid, qid, mid, key, value ? *(uint64_t *)value : 0, raddr);
}

static void process_invalidate_page(struct rdma_queue *q, int cid, int qid, 
        int mid) { 
    uint64_t meta_region = (uint64_t) q->ctrl->mm->GetMetaMMPool()->mr->addr;
    Key_t key = *(uint64_t *) KEY_META_REGION(meta_region, qid, mid,
            MSG_INV_PAGE);
    Value_t value;
    uint32_t imm_data;
    dcc_declare_ts(all);

    dcc_start_ts(all);
    value = kv->Remove(key, cid, qid); 
    if (value)
        kv->Free(value, cid, -1); 

    if (mid == rdma_config.num_msgs - 1) {
        imm_data = htonl(bit_mask(mid, MSG_INV_PAGE_REPLY, 
                    value ? TX_INV_PAGE_COMMITED : TX_INV_PAGE_ABORTED, 0, 0));
        if (send_reply(q, imm_data, NULL, 0)) {
            fprintf(stderr, "cannot reply inv page request\n");
            assert(0);
        }
        dcc_end_ts(BR_INVS, all);
        poll_send_cq(q);
    } else {
        dcc_end_ts(BR_INVS, all);
    }

    dcc_rdma_debug("(%d, %d, %d) key=%lu, value=%lu\n", 
            cid, qid, mid, key, value ? *(uint64_t *)value : 0);
}

void process_send(struct rdma_queue *q, struct ibv_wc *wc) {
    if (wc->wr_id == FILTER_UPDATE_END) {}
    //printf("%s: qid=%d\n", __func__, q->id);
}

void process_imm(struct rdma_queue *q, struct ibv_wc &wc) {
    int cli_id = q->ctrl->id;
    int qid = q->id;
    int mid, type, tx_state;
    uint64_t wr_id = wc.wr_id;
    uint32_t imm_data = wc.imm_data;

    bit_unmask(ntohl(imm_data), &mid, &type, &tx_state, NULL, NULL);

    switch (type) {
        case MSG_WRITE:
            assert(0);
            break;
        case MSG_READ:
            post_recv(q, NULL);	
            process_get(q, cli_id, qid, mid);
            break;
        case MSG_INV_PAGE:
            post_recv(q, NULL);	
            process_invalidate_page(q, cli_id, qid, mid);
            break;
        default:
            fprintf(stderr, "Unknown msg type: %d\n", type);
    }
}

void process_recv(struct rdma_queue *q, struct ibv_wc &wc) {
    uint64_t wr_id = wc.wr_id;
    uint32_t imm_data = wc.imm_data;

    /* Filter */
    if (q->id == rdma_config.num_data_qps && imm_data == FILTER_UPDATE_BEGIN) {
        kv->GetCBF(q->ctrl->id)->ToOrdinaryBF();
        send_bf(q);

        post_recv(q, NULL);	
        return;
    }

    /* Data */
    if (!wr_id) {
        struct mm_region *client_mr = q->ctrl->mm->GetClientMR();
        /* only for the connection */
        printf("clientmr(filter) addr=0x%lx, key=%u, len=%u\n",
                client_mr->info.baseaddr, client_mr->info.key, 
                client_mr->info.len);
    } else {
        int cli_id = q->ctrl->id;
        int qid = q->id;
        int mid, type, tx_state;
        dcc_declare_ts(all);

        bit_unmask(ntohl(imm_data), &mid, &type, &tx_state, NULL, NULL); 

        process_put(q, cli_id, qid, mid, wr_id);
    }
}

void data_cq_poller(struct rdma_queue *queue, int qid) {
    uint64_t idx = 0;

    while (!client_ctr)
        sleep(1);

    //printf("Q[%d] %s\n", qid, __func__);

    while(1) {
        struct ibv_wc wc;
        struct rdma_queue *q = queue;
        int ret;

        while (1) {
            wc = {};

            switch (config.poller_type) {
                case PER_QP_POLLER:
                    ret = __poll_cq(q->qp->recv_cq, &wc, false);
                    if (ret > 0)
                        goto out_process;
                    break;
                case GLOBAL_QP_POLLER:
                    /* 
                     * For fairness, poll the client next to the successful
                     * client that you previously polled 
                     */
                    while (1) {
                        q = &clients[idx]->ctrl->queues[qid];
                        ret = __poll_cq(q->qp->recv_cq, &wc, true);
                        idx = ++idx % client_ctr;
                        if (ret > 0) {
                            goto out_process;
                        }
                    }
                default:
                    fprintf(stderr, "Unknown poller_type: %d\n", 
                            config.poller_type);
            }
        }

out_process:
        assert(q != NULL);

        switch (wc.opcode) {
            case IBV_WC_RECV_RDMA_WITH_IMM:
                process_imm(q, wc);
                break;
            case IBV_WC_RECV:
                process_recv(q, wc);
                break;
            case IBV_WC_RDMA_WRITE:
            case IBV_WC_RDMA_READ:
                break;
            default:
                fprintf(stderr, "%s: Received a weired opcode (%d)\n", 
                        __func__, (int)wc.opcode);
        }
    }
}

/* hybrid polling: event-based + polling (timed repeat + backoff) */
//#define DCC_EVENT_ONLY
void hybrid_data_cq_poller(struct rdma_queue *q, int qid) {
    struct ibv_wc *wc;
    struct ibv_wc wc_arr[MAX_POLL_WC] = {};
    void *ev_ctx;
    int ret;
    struct ibv_cq *cq = q->event_cq;
    int num_wc;
    using namespace std::chrono_literals;
    auto min_duration = (1 << 0) * 1ms;
    auto max_duration = (1 << 10) * 1ms;
    auto duration = config.wc_mode == WC_ADAPTIVE_BO ? (1 << 5) * 1ms : min_duration;

    ret = ibv_req_notify_cq(q->event_cq, 0);
    if (ret) {
        fprintf(stderr, "Couldn't request CQ notification\n");
        die("ibv_req_notify_cq");
    }

    while (1) {
event_mode:
        ret = ibv_get_cq_event(q->channel, &cq, &ev_ctx);
        if (ret) {
            fprintf(stderr, "Failed to get CQ event\n");
            die("ibv_get_cq_event");
        }  

poll_mode:
        uint64_t succ_cnt = 0, failed_cnt = 0;
        /* polling during some duration */
#ifndef DCC_EVENT_ONLY
        auto now = std::chrono::steady_clock::now;
        auto start = now();
        while (now() - start < duration)
#endif
        {
retry:
            num_wc = 0;

            for (int i = 0; i < MAX_POLL_WC; i++) {
                ret = ibv_poll_cq(cq, 1, &wc_arr[i]);
                if (ret <= 0)
                    break;
                num_wc++;
            }

            if (num_wc > 0)
                succ_cnt++;
            else
                failed_cnt++;

            for (int i = 0; i < num_wc; i++) {
                wc = &wc_arr[i];
                switch (wc->opcode) {
                    case IBV_WC_SEND:
                        process_send(q, wc);
                        break;
                    case IBV_WC_RECV_RDMA_WITH_IMM:
                        process_imm(q, *wc);
                        break;
                    case IBV_WC_RECV:
                        process_recv(q, *wc);
                        break;
                    case IBV_WC_RDMA_WRITE:
                    case IBV_WC_RDMA_READ:
                        break;
                    default:
                        fprintf(stderr, "%s: Received a weired opcode (%d)\n", 
                                __func__, (int)wc->opcode);
                }
            }
        }

#ifdef DCC_EVENT_ONLY
        /* Request notification upon the next completion event */
        ret = ibv_req_notify_cq(cq, 0);
        if (ret) {
            fprintf(stderr, "Couldn't request CQ notification\n");
            die("ibv_req_notify_cq");
        }
        ibv_ack_cq_events(cq, 1);
        goto event_mode;   
#else 
        if (!(succ_cnt + failed_cnt)) {
            /* XXX: Never checked cq due to duration limit */
            //throw std::runtime_error("succ_cnt + failed_cnt is 0");
            goto retry;
        }

        int succ_rate = 100UL * succ_cnt / (succ_cnt + failed_cnt); 
        if (!succ_rate) {
            if (config.wc_mode == WC_ADAPTIVE_BO && duration > min_duration)
                duration = (duration / 2);
            /* Request notification upon the next completion event */
            ret = ibv_req_notify_cq(cq, 0);
            if (ret) {
                fprintf(stderr, "Couldn't request CQ notification\n");
                die("ibv_req_notify_cq");
            }
            ibv_ack_cq_events(cq, 1);
            goto event_mode;
        } else {
            if (config.wc_mode == WC_ADAPTIVE_BO && 
                    succ_rate > 80 && duration < max_duration)
                duration = (duration * 2);
            /* else predict well => not change the duration */
            goto poll_mode;
        }
#endif
    }
}

void event_filter_cq_handler(struct rdma_queue *q) {
    struct ibv_wc wc = {};
    void *ev_ctx;
    int ret;
    struct ibv_cq *cq = q->event_cq;

    ret = ibv_req_notify_cq(q->event_cq, 0);
    if (ret) {
        fprintf(stderr, "Couldn't request CQ notification\n");
        die("ibv_req_notify_cq");
    }

    while (1) {
        wc = {};

        ret = ibv_get_cq_event(q->channel, &cq, &ev_ctx);
        if (ret) {
            fprintf(stderr, "Failed to get CQ event\n");
            die("ibv_get_cq_event");
        }  

        ibv_ack_cq_events(cq, 1);

        /* Request notification upon the next completion event */
        ret = ibv_req_notify_cq(cq, 0);
        if (ret) {
            fprintf(stderr, "Couldn't request CQ notification\n");
            die("ibv_req_notify_cq");
        }

        __poll_cq(cq, &wc, false);

        switch (wc.opcode) {
            case IBV_WC_SEND:
                process_send(q, &wc);
                break;
            case IBV_WC_RECV_RDMA_WITH_IMM:
                process_imm(q, wc);
                break;
            case IBV_WC_RECV:
                process_recv(q, wc);
                break;
            case IBV_WC_RDMA_WRITE:
            case IBV_WC_RDMA_READ:
                break;
            default:
                fprintf(stderr, "%s: Received a weired opcode (%d)\n", 
                        __func__, (int)wc.opcode);
        }
    }
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

    if (q->id < rdma_config.num_data_qps) {
        if (config.wc_mode == WC_BUSY_WAITING) {
            send_cq = ibv_create_cq(q->cm_id->verbs, CQ_NUM_CQES, NULL, NULL, 
                    0);
            recv_cq = ibv_create_cq(q->cm_id->verbs, CQ_NUM_CQES, NULL, NULL, 
                    0);  
        } else {
            /* hybrid polling */
            q->channel = ibv_create_comp_channel(q->ctrl->rdev->verbs);
            if (!q->channel) {
                fprintf(stderr, "Error, ibv_create_comp_channel() failed\n");
                die("ibv_create_comp_channel");
            }   
            q->event_cq = ibv_create_cq(q->cm_id->verbs, CQ_NUM_CQES, NULL, 
                    q->channel, 0);
            send_cq = ibv_create_cq(q->cm_id->verbs, CQ_NUM_CQES, NULL, NULL, 
                    0);
            recv_cq = q->event_cq;
        }
    } else {
        q->channel = ibv_create_comp_channel(q->ctrl->rdev->verbs);
        if (!q->channel) {
            fprintf(stderr, "Error, ibv_create_comp_channel() failed\n");
            die("ibv_create_comp_channel");
        }   
        q->event_cq = ibv_create_cq(q->cm_id->verbs, CQ_NUM_CQES, NULL, 
                q->channel, 0);
        send_cq = q->event_cq;
        recv_cq = q->event_cq;
    }

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

inline int set_thread_affinity(thread &t, int cpu) {
    cpu_set_t cpuset;
    int rc;

    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    rc = pthread_setaffinity_np(t.native_handle(), sizeof(cpu_set_t), &cpuset);
    if (rc) {
        fprintf(stderr, "[CPU:%d] pthread_setaffinity_np\n", cpu);
        return -1;
    }

    return 0;
}

inline int set_thread_affinity2(thread &t, int qid) {
    cpu_set_t cpuset;
    int rc;
    vector<int> cpus;

    CPU_ZERO(&cpuset);

    // TODO: config numa api, num_cpus = 8
    for (int i = 0; i < 8; i++) {
        cpus.push_back(qid == 0 ? i : i + 4);
    }

    for (int i = 0; i < cpus.size(); i++) {
        CPU_SET(cpus[i], &cpuset);
    }

    rc = pthread_setaffinity_np(t.native_handle(), sizeof(cpu_set_t), &cpuset);
    if (rc) {
        fprintf(stderr, "[Q:%d] pthread_setaffinity_np\n", qid);
        return -1;
    }

    return 0;
}

int create_data_cq_handler(struct rdma_queue *q, int cli_id, int queue_id) {
    thread p;
    cpu_set_t cpuset;
    int rc;

    if (config.wc_mode == WC_BUSY_WAITING) {
        p = thread(data_cq_poller, q, -1);
        /*
         * Let Data QPS=8
         * CPU: 0: 1~8, 1: 11~18, 2: 21~28, 3: 31~38
         */
        set_thread_affinity(p, (cli_id * 10) + (queue_id + 1));
        if (rc) {
            fprintf(stderr, "[%d:%d pthread_setaffinity_np\n", cli_id, 
                    queue_id);
            return -1;
        }
    } else {
        p = thread(hybrid_data_cq_poller, q, -1);
        //set_thread_affinity(p, queue_id);
        set_thread_affinity2(p, queue_id);
        if (rc) {
            fprintf(stderr, "[%d:%d pthread_setaffinity_np\n", cli_id, 
                    queue_id);
            return -1;
        }
    }

    p.detach();

    return 0;
}

int create_global_data_cq_handler(void) {
    for (int i = 0; i < rdma_config.num_data_qps; i++) {
        thread p = thread(data_cq_poller, nullptr, i);
        set_thread_affinity(p, i);
        p.detach();
    }
    return 0;
}

inline int create_filter_cq_handler(struct rdma_queue *q) {
    thread p = thread(event_filter_cq_handler, q);
    p.detach();
    return 0;
}

int create_cq_handler(struct rdma_queue *q) {
    if (q->id < rdma_config.num_data_qps) {
        if (config.poller_type == PER_QP_POLLER) {
            create_data_cq_handler(q, q->ctrl->id, q->id);
        }
    } else {
        create_filter_cq_handler(q);
    }
    return 0;
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

    create_cq_handler(q);

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

    mr = ctrl->mm->GetMetaMMPool()->mr;

    printf("servermr addr=%p, key=%u, length=%lu\n", mr->addr, mr->rkey, 
            mr->length);
    printf("servermr num_hash=%u, num_bits=%u\n", 
            kv->GetCBF(0)->GetNumHash(), kv->GetCBF(0)->GetNumBits());

    server_mm_info.baseaddr = (uint64_t) mr->addr;
    server_mm_info.key      = mr->rkey;
    server_mm_info.len      = mr->length / PAGE_SIZE;
    server_mm_info.cli_id   = q->ctrl->id;
    server_mm_info.info[0]  = kv->GetCBF(0)->GetNumHash();
    server_mm_info.info[1]  = kv->GetCBF(0)->GetNumBits();
    server_mm_info.info[2]  = ctrl->mm->GetDataMMPool()->mr->length / PAGE_SIZE;

    wr.opcode = IBV_WR_SEND;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;

    sge.addr = (uint64_t) &server_mm_info;
    sge.length = sizeof(struct mm_info);

    TEST_NZ(ibv_post_send(q->qp, &wr, &bad_wr));
}

void recv_mr(struct rdma_queue *q) 
{
    struct dcc_rdma_ctrl *ctrl = q->ctrl;
    struct ibv_recv_wr rwr = {};
    struct ibv_sge sge = {};
    struct ibv_recv_wr *bad_rwr;
    struct mm_info *client_mm_info = &ctrl->mm->GetClientMR()->info;

    sge.addr    = (uint64_t) client_mm_info;
    sge.length  = sizeof(struct mm_info);
    sge.lkey    = ctrl->mm->GetClientMR()->mr->lkey;

    rwr.wr_id   = 0;
    rwr.next    = NULL;
    rwr.sg_list = &sge;
    rwr.num_sge = 1;

    TEST_NZ(ibv_post_recv(q->qp, &rwr, &bad_rwr));
}

void exchange_mr(struct rdma_queue *q) {
    send_mr(q);
    recv_mr(q);
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

void pre_post_recvs(struct rdma_queue *q, int cid, int qid) {
    for (int i = 0; i < MAX_QPS_RECVS; i++)
        post_recv(q, qid < rdma_config.num_data_qps ? 
                kv->Allocate(cid, qid) : NULL);
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
#if 0
        if (config.poller_type == GLOBAL_QP_POLLER) { 
            struct ibv_wc wc;
            __poll_cq((&ctrl->queues[0])->qp->recv_cq, &wc, false);
        }
#endif
        client_ctr++;

        for (int i = 0; i < rdma_config.num_qps; ++i)
            pre_post_recvs(&ctrl->queues[i], cli_id, i);
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
    rdma_config.num_get_qps = 6;
    rdma_config.num_data_qps = 2 + rdma_config.num_get_qps;
    rdma_config.num_filter_qps = 1;
    rdma_config.num_qps = rdma_config.num_data_qps +
        rdma_config.num_filter_qps;
    rdma_config.num_msgs = 512;
    rdma_config.get_metadata_size = sizeof(uint64_t) * 2;
    rdma_config.inv_metadata_size = sizeof(uint64_t) * rdma_config.num_msgs;
    rdma_config.metadata_size = rdma_config.get_metadata_size +
        rdma_config.inv_metadata_size;
    rdma_config.metadata_mr_size = rdma_config.num_data_qps *
        rdma_config.metadata_size;
}

struct dcc_rdma_ctrl *init_rdma(int cid) {
    struct dcc_rdma_ctrl *ctrl;

    if (cid == 0) {
        dcc_rdma_set_default_config();
        cpu_monitor = new CPUMonitor(3);
    }

    ctrl = alloc_control();
    if (!ctrl)
        return NULL;

    ctrl->mm = new RdmaMem();

    ctrl->mm->Allocate(cid, ctrl);

    /* spawn recv cq polling threads */
    if (cid == 0 && config.wc_mode == WC_BUSY_WAITING && 
            config.poller_type == GLOBAL_QP_POLLER) {
        create_global_data_cq_handler();
    }

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

void rdma_show_stats() {
#ifdef DCC_STATS_ENABLE
    printf("**************** RDMA Stats ****************\n");
    printf("Completion: event=%lu, poll=%lu, sleep_wait=%lu\n", 
            stats->GetSum(RDMA_STATS, RDMA_EVENT_COMP), 
            stats->GetSum(RDMA_STATS, RDMA_POLL_COMP), 
            stats->GetSum(RDMA_STATS, RDMA_SLEEP_WAIT_COMP));
    printf("********************************************\n");
#endif

#ifdef DCC_BREAKDOWN
    uint64_t put_elapsed = dcc_breakdown.elapseds[BR_PUTS];
    uint64_t get_elapsed = dcc_breakdown.elapseds[BR_GETS];
    uint64_t inv_elapsed = dcc_breakdown.elapseds[BR_INVS];

    fprintf(stderr, "put=%lu, get=%lu, inv=%lu\n", 
            put_elapsed, get_elapsed, inv_elapsed);
#endif
}
