#KDIR ?= /lib/modules/`uname -r`/build
KDIR ?= /root/linux-5.4-VM
NCPUS=$(shell grep -c processor /proc/cpuinfo)

default:
	$(MAKE) -j $(NCPUS) -C $(KDIR) M=$$PWD

clean:
	$(MAKE) -j $(NCPUS) -C $(KDIR) M=$$PWD clean

obj-m := dcc.o rdma_backend.o rdma_bench.o
dcc-objs := main.o stats.o debugfs.o
rdma_backend-objs := backend.o worker.o \
	rdma.o rdma_mem.o \
	filter/bloom_filter.o filter/counting_bloom_filter.o hashtable.o \
	util/crc8.o breakdown.o 
rdma_bench-objs := rdma_test.o
	
#ccflags-y += -DDCC_BREAKDOWN
