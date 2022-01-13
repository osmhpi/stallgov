obj-m += memutil.o
memutil-objs := memutil_main.o memutil_log.o memutil_debugfs.o pmu_events.o pmu_cpuid_helper.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
