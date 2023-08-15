#ifndef __WORKER_H__
#define __WORKER_H__

#include <linux/sched.h>

struct dcc_backend;
struct dcc_rdma_ctrl;

struct dcc_worker_arg {
	struct dcc_backend *backend;
	struct dcc_rdma_ctrl *ctrl;
	struct completion *comp;
};

struct dcc_worker {
	struct task_struct *task;
	struct completion comp;
};


void do_monitor_latency(void *arg);
void do_update_filter(void *arg);
struct dcc_worker *dcc_start_worker(struct dcc_backend *backend, 
		struct dcc_rdma_ctrl *ctrl, int (*threadfn)(void *data),
		struct dcc_worker_arg *arg,
		const char *namefmt);
int dcc_backend_stop_worker(struct dcc_worker *worker);

#endif // __WORKER_H__
