obj-m += memutil.o
memutil-objs := memutil_main.o memutil_ringbuffer_log.o memutil_debugfs.o memutil_debugfs_logfile.o memutil_debugfs_infofile.o pmu_events.o pmu_cpuid_helper.o memutil_perf_read_local.o memutil_perf_counter.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
