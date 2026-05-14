obj-m += syscall_monitor.o
syscall_monitor-objs := src/main.o src/gatekeeper.o src/config.o src/hook.o src/driver.o lib/vtpmo.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all: module user_tools

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

user_tools:
	gcc user/cli.c -o user/monitor_cli
	gcc -o test/test_suite test/test_suite.c -pthread

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f user/monitor_cli
	rm -f test/test_suite
