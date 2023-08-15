#ifndef __CONFIG_HPP__
#define __CONFIG_HPP__


struct daemon_config_t {
    int tcp_port;
    int num_clients;
    bool exclusive_policy;
    size_t mem_pool_size;
    int ht_type;
    int num_ht_shards;
    bool cbf_on;
    int wc_mode;
    int poller_type;
};
extern struct daemon_config_t config;

enum poller_t {
    PER_QP_POLLER,          // num_clients * num_qps
    GLOBAL_QP_POLLER,      // num_qps
};

enum wc_mode_t {
    WC_BUSY_WAITING,
    WC_EVENT,
    WC_ADAPTIVE,
    WC_ADAPTIVE_BO,

    MAX_WC_MODE
};

const std::string wc_mode_names[] = {
    "busy_waiting", 
    "event",
    "adaptive",
    "adaptive_bo"
};

enum ht_type_t {
	HT_CH,
	HT_CH_APPROX,
    HT_LP,
	HT_LP_APPROX,
};

static inline int __wc_mode(std::string &str) {
    for (int i = 0; i < MAX_WC_MODE; i++) {
        if (wc_mode_names[i] == str)
            return i;
    }
    throw std::runtime_error("plz check wc_mode\n");
    return -1;
}

#endif // __CONFIG_HPP__
