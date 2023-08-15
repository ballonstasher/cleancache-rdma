/* 
 * refer: 
 * https://modoocode.com/285 
 * https://github.com/progschj/ThreadPool
 */ 
#ifndef __WORKER_THREAD_HPP__
#define __WORKER_THREAD_HPP__

#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#define WORKER_POLLING

class WorkerThread {
	public:
		WorkerThread(int);
		~WorkerThread();

		// job 을 추가한다.
#if 0
		template <class F, class... Args>
			std::future<typename std::result_of<F(Args...)>::type> EnqueueJob(
					F&& f, Args&&... args);
#else 
        template <class F, class... Args> 
            void EnqueueJob(F&& f, Args&&... args);
#endif
	private:
		// 총 Worker 쓰레드의 개수.
		int num_threads_;
		// Worker 쓰레드를 보관하는 벡터.
		std::vector<std::thread> worker_threads_;
		// 할일들을 보관하는 job 큐.
		std::queue<std::function<void()>> jobs_;
		// 위의 job 큐를 위한 cv 와 m.
		std::condition_variable cv_job_q_;
		std::mutex m_job_q_;

		// 모든 쓰레드 종료
		bool stop_all;

		// Worker 쓰레드
		void Work();
};

inline WorkerThread::WorkerThread(int num_threads)
	: num_threads_(num_threads), stop_all(false) {
		worker_threads_.reserve(num_threads_);
		for (int i = 0; i < num_threads_; ++i) {
			worker_threads_.emplace_back([this]() { this->Work(); });
		}
	}

inline void WorkerThread::Work() {
	while (true) {
RETRY:
        std::this_thread::sleep_for(std::chrono::seconds(2));
RETRY2:
		//std::unique_lock<std::mutex> lock(m_job_q_);
		if (!m_job_q_.try_lock())
            goto RETRY;
#if 0
        cv_job_q_.wait(lock, 
                [this]() { return !this->jobs_.empty() || stop_all; });
#else 
        if (!stop_all && this->jobs_.empty()) {
            m_job_q_.unlock();
            goto RETRY;
        }
#endif
        if (stop_all && this->jobs_.empty()) {
            m_job_q_.unlock();
			return;
        }

		// 맨 앞의 job 을 뺀다.
		std::function<void()> job = std::move(jobs_.front());
		jobs_.pop();
        m_job_q_.unlock();

		// 해당 job 을 수행한다 :)
		job();
        
        {
            std::unique_lock<std::mutex> lock(m_job_q_);
            if (!stop_all && !this->jobs_.empty()) {
                goto RETRY2;
            }
        }

	}
}

inline WorkerThread::~WorkerThread() {
	stop_all = true;
	cv_job_q_.notify_all();

	for (auto& t : worker_threads_)
		t.join();
}

#if 0
template <class F, class... Args>
inline std::future<typename std::result_of<F(Args...)>::type> 
WorkerThread::EnqueueJob(F&& f, Args&&... args) {
	if (stop_all)
		throw std::runtime_error("WorkerThread 사용 중지됨");

	using return_type = typename std::result_of<F(Args...)>::type;
	auto job = std::make_shared<std::packaged_task<return_type()>>(
			std::bind(std::forward<F>(f), std::forward<Args>(args)...));
	std::future<return_type> job_result_future = job->get_future();
	{
		std::lock_guard<std::mutex> lock(m_job_q_);
		jobs_.push([job]() { (*job)(); });
	}
	cv_job_q_.notify_one();
	
    return job_result_future;
}
#else
template <class F, class... Args>
inline void WorkerThread::EnqueueJob(F&& f, Args&&... args) {
	if (stop_all)
		throw std::runtime_error("WorkerThread 사용 중지됨");

	using return_type = typename std::result_of<F(Args...)>::type;
	auto job = std::make_shared<std::packaged_task<return_type()>>(
			std::bind(std::forward<F>(f), std::forward<Args>(args)...));
	std::future<return_type> job_result_future = job->get_future();
	{
		std::lock_guard<std::mutex> lock(m_job_q_);
		jobs_.push([job]() { (*job)(); });
	}
#if 0
	cv_job_q_.notify_one();
#endif
}
#endif

#endif // __WORKER_THREAD_HPP__
