#ifndef __CPU_MONITOR_HPP__
#define __CPU_MONITOR_HPP__

#include "CPUSnapshot.hpp"

#include <thread>
#include <chrono>
#include <atomic>

/* Monitor system wide cpu utilization: 0 <= x <= 100 % */

class CPUMonitor {
    public:
        CPUMonitor(int);
        ~CPUMonitor();
        
        void MonitorCPUUsage();
        int GetCPUUsage() {
            return usage_.load();    
        }

    private:
        std::thread monitor_;
        std::atomic<int> usage_;
        int interval_;
};

inline CPUMonitor::CPUMonitor(int interval) {
    usage_.store(0);
    interval_ = interval;

    monitor_ = std::thread(&CPUMonitor::MonitorCPUUsage, this);
    monitor_.detach();
}

inline void CPUMonitor::MonitorCPUUsage() {
    while (1) {
        CPUSnapshot previousSnap;

        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ * 1000));

        CPUSnapshot curSnap;

        const float ACTIVE_TIME = curSnap.GetActiveTimeTotal() - previousSnap.GetActiveTimeTotal();
        const float IDLE_TIME   = curSnap.GetIdleTimeTotal() - previousSnap.GetIdleTimeTotal();
        const float TOTAL_TIME  = ACTIVE_TIME + IDLE_TIME;
        usage_.store(100 * ACTIVE_TIME / TOTAL_TIME);
        //std::cout << "total cpu usage: " << usage << " %" << std::endl;
    }   
}


#endif // __CPU_MONITOR_HPP__
