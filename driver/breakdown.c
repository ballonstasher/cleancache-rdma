#include "breakdown.h"

struct dcc_breakdown_t dcc_breakdown = {};
EXPORT_SYMBOL_GPL(dcc_breakdown);

const char *dcc_breakdown_names[] = {
	"br_puts",
	"br_gets",
	"br_invs",
#if 0
	"br_meta_puts",
	"br_meta_gets",
	"br_meta_invs",
#endif
	"br_comm_puts",
	"br_comm_gets",
	"br_comm_invs"
};
EXPORT_SYMBOL_GPL(dcc_breakdown_names);
