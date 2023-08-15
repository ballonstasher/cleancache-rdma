#ifndef __CPU_STATS_HPP__
#define __CPU_STATS_HPP__

#include <iostream>
#include <unistd.h>
#include <sys/types.h>
#include <cstring>
#include <string>
#include <thread>

struct cpu_stat {
	unsigned long utime_ticks;
	long cutime_ticks;
	unsigned long stime_ticks;
	long cstime_ticks;
    
    unsigned long cpu_total_time;
};

class CPUStats {
    public:
        CPUStats(int _interval) {
            pid_t pid = getpid();
           
            pid_stat_path = "/proc/";
            pid_stat_path += std::to_string(pid);
            pid_stat_path += "/stat";
            
            util = 0;
            interval = _interval;
            cumm_cnt = 0;

            printf("%s: pid=%d, %s\n", __func__, pid, pid_stat_path.c_str());

            if (interval) {
                thd = std::thread(&CPUStats::Monitor, this);
                thd.detach();
            }
        }

        ~CPUStats() {}
    
        void Monitor();
        
        void CalcUtil();
        double GetUtil() {
            return util;
        }
        
        void Start() {
            if (Profile(&res[0])) {
                fprintf(stderr, "%s\n", __func__);
            }
        }
        void End() {
            if (Profile(&res[1])) {
                fprintf(stderr, "%s\n", __func__);
            }
        }

    private:
        int Profile(struct cpu_stat *);

    private:
        pid_t pid;
        FILE *pid_stat_fp;
        FILE *stat_fp;
        std::string pid_stat_path; 
        struct cpu_stat res[2];
        double util;
        int interval;
        uint64_t cumm_cnt;

        std::thread thd;
};

inline void CPUStats::Monitor() {
    while (1) {
        Start();
        std::this_thread::sleep_for(std::chrono::seconds(interval));
        End();
        if (cumm_cnt != ULONG_MAX)
            cumm_cnt++;
        CalcUtil();
    }
}

static inline double calc_avg_filter(double avg, double new_val, uint64_t cnt)
{
    return (avg * (cnt - 1) / cnt) + (new_val / cnt);
}

/* calculates the elapsed CPU usage between 2 measuring points. in percent */
inline void CPUStats::CalcUtil() {
    double ucpu_usage = 0, scpu_usage = 0;
    struct cpu_stat *last_usage = &res[0];
    struct cpu_stat *cur_usage = &res[1];
	unsigned long total_time_diff = cur_usage->cpu_total_time -
		last_usage->cpu_total_time;

    if (!total_time_diff)
        return;

	ucpu_usage = 100 * (((cur_usage->utime_ticks + cur_usage->cutime_ticks)
				- (last_usage->utime_ticks + last_usage->cutime_ticks))
			/ (double) total_time_diff);

	scpu_usage = 100 * ((((cur_usage->stime_ticks + cur_usage->cstime_ticks)
					- (last_usage->stime_ticks + last_usage->cstime_ticks))) /
			(double) total_time_diff);

    //util = ucpu_usage + scpu_usage;
    util = calc_avg_filter(util, ucpu_usage + scpu_usage, cumm_cnt);
}

/*
 * read /proc data into the passed struct cpu_stat
 * returns 0 on success, -1 on error
 */
inline int CPUStats::Profile(struct cpu_stat *result) {

	pid_stat_fp = fopen(pid_stat_path.c_str(), "r");
	if (!pid_stat_fp) {
		perror("FOPEN ERROR");
		return -1;
	}

	stat_fp = fopen("/proc/stat", "r");
	if (!stat_fp) {
		perror("FOPEN ERROR ");
		fclose(stat_fp);
		return -1;
	}

	/* read values from /proc/pid/stat */
    memset(result, 0x00, sizeof(struct cpu_stat));
	if (fscanf(pid_stat_fp, 
                "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u"
                " %lu %lu %ld %ld %*d %*d %*d %*d %*u %*u %*d",
				&result->utime_ticks, &result->stime_ticks,
				&result->cutime_ticks, &result->cstime_ticks) == EOF) {
		fclose(pid_stat_fp);
		return -1;
	}
	fclose(pid_stat_fp);
    
    /*
    printf("%lu %lu %ld %ld\n", 
            result->utime_ticks, result->stime_ticks, 
            result->cutime_ticks, result->cstime_ticks);
    */
	
    /* read+calc cpu total time from /proc/stat */
	unsigned long cpu_time[10] = {};
	if (fscanf(stat_fp, "%*s %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
				&cpu_time[0], &cpu_time[1], &cpu_time[2], &cpu_time[3],
				&cpu_time[4], &cpu_time[5], &cpu_time[6], &cpu_time[7],
				&cpu_time[8], &cpu_time[9]) == EOF) {
		fclose(stat_fp);
		return -1;
	}

	fclose(stat_fp);
    
	for (int i = 0; i < 10; i++)
		result->cpu_total_time += cpu_time[i];

    //printf("%lu\n", result->cpu_total_time);

	return 0;
}

#endif // __CPU_STATS_HPP__
