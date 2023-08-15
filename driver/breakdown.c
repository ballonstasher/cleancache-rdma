#include "breakdown.h"

struct dcc_breakdown_t dcc_breakdown = {};
EXPORT_SYMBOL_GPL(dcc_breakdown);

const char *dcc_breakdown_names[] = {
	"br_puts",
	"br_puts_index",
	"br_puts_pool",
	"br_puts_comm",

	"br_gets",
	"br_gets_index",
	"br_gets_pool",
	"br_gets_comm",

	"br_invs",
	"br_invs_index",
	"br_invs_pool",
	"br_invs_comm",
};
EXPORT_SYMBOL_GPL(dcc_breakdown_names);
