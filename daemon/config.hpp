#ifndef __CONFIG_HPP__
#define __CONFIG_HPP__

struct daemon_config_t {
    int tcp_port;
    int num_clients;
    bool exclusive_policy;
    size_t mem_pool_size;
};
extern struct daemon_config_t config;

#endif // __CONFIG_HPP__
