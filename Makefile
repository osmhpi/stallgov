obj-m += memutil.o
memutil-objs := memutil_main.o memutil_log.o memutil_debugfs.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
