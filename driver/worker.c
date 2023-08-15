#include <linux/slab.h>

#include "worker.h"
#include "backend.h"
#include "rdma.h"

static bool filter_update_done = false;
extern int filter_update_interval;

void do_update_filter(void *arg) 
{
	struct dcc_worker_arg *my_arg = (struct dcc_worker_arg *) arg;
	struct dcc_rdma_ctrl *ctrl = my_arg->ctrl;
	struct completion *comp = my_arg->comp;
	int accum_sleep_sec = 0;

	pr_info("Spawning worker for filter, interval(sec)=%d",
			filter_update_interval);

	while (!kthread_should_stop()) {
		//pr_err("%s", __func__);
		if (filter_update_interval > accum_sleep_sec) {
			ssleep(filter_update_interval - accum_sleep_sec);
		}
		accum_sleep_sec = 0;

		/* 1. blocking filter */
		metadata_blocking(backends[ctrl->id]->meta);
		
		/* 2. Send msg to server, to update local filter */
		dcc_rdma_send_msg(&ctrl->queues[rdma_config.num_data_qps], 0, 0,
				FILTER_UPDATE_BEGIN);
		
		while (!filter_update_done) {
			ssleep(1); /* usually 1sec is enough */
			accum_sleep_sec++;
			if (kthread_should_stop()) /* disconnected state */
				goto out;
		}
		filter_update_done = false;
		//BUG_ON(accum_sleep_sec >= filter_update_interval);
	}
out:
	complete(comp);
}

int dcc_backend_update_filter(struct dcc_rdma_ctrl *ctrl)
{
	filter_update_done = true;
	metadata_unblocking(backends[ctrl->id]->meta);
	//pr_info("%s", __func__);
	return 0;
}


struct dcc_worker *dcc_start_worker(struct dcc_backend *backend, 
		struct dcc_rdma_ctrl *ctrl, int (*threadfn)(void *data),
		struct dcc_worker_arg *arg,
		const char *namefmt)
{
	struct dcc_worker *thread;

	/* Initialize background migration kthread */
	thread = (struct dcc_worker *) 
		kzalloc(sizeof(struct dcc_worker), GFP_KERNEL);
	if (!thread) {
		pr_err("%s: failed to allocate thread", __func__);
		return ERR_PTR(-ENOMEM);
	}
	
	init_completion(&thread->comp);

	arg->backend = backend;
	arg->ctrl = ctrl;
	arg->comp = &thread->comp;
	
	thread->task = kthread_create(threadfn, (void *) arg, namefmt);

	wake_up_process(thread->task);
	
	return thread;
}

int dcc_backend_stop_worker(struct dcc_worker *worker)
{
	kthread_stop(worker->task);
	wait_for_completion(&worker->comp);
	return 0;
}
