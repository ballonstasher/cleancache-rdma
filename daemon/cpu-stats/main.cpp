/* 
 * refer: https://github.com/vivaladav/cpu-stat 
 */
#include "CPUSnapshot.hpp"

#include <chrono>
#include <thread>
#include <iostream>

using namespace std;

void monitor_cpu_usage() {
    while (1) {
        CPUSnapshot previousSnap;
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        CPUSnapshot curSnap;

        const float ACTIVE_TIME = curSnap.GetActiveTimeTotal() - previousSnap.GetActiveTimeTotal();
        const float IDLE_TIME   = curSnap.GetIdleTimeTotal() - previousSnap.GetIdleTimeTotal();
        const float TOTAL_TIME  = ACTIVE_TIME + IDLE_TIME;
        int usage = 100.f * ACTIVE_TIME / TOTAL_TIME;

        std::cout << "total cpu usage: " << usage << " %" << std::endl;
    }
}

int main() {
    thread monitor;

    monitor = thread(monitor_cpu_usage);

    monitor.join();

    return 0;
}
