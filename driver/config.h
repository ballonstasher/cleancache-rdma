#ifndef __CONFIG_H__
#define __CONFIG_H__

#include <linux/inet.h>

#define MAX_NUM_SVRS 8

struct dcc_config_t {
	char cli_ip[INET_ADDRSTRLEN];
	char svr_ips[MAX_NUM_SVRS][INET_ADDRSTRLEN];
	int svr_port;
	int num_svrs;
	bool exclusive_policy;
	int metadata_type;
	int filter_update_interval;
	bool af_enable;
};

extern struct dcc_config_t config;

#endif // __CONFIG_H__
