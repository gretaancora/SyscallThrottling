#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#include <linux/ioctl.h>

#define MONITOR_IOC_MAGIC 'M'
#define MAX_COMM_LEN 16

struct monitor_prog_config {
    char comm[MAX_COMM_LEN];
};

// Comandi driver
#define MONITOR_IOC_SET_MAX    _IOW(MONITOR_IOC_MAGIC, 1, int)
#define MONITOR_IOC_ADD_PROG   _IOW(MONITOR_IOC_MAGIC, 2, struct monitor_prog_config)
#define MONITOR_IOC_DEL_PROG   _IOW(MONITOR_IOC_MAGIC, 3, struct monitor_prog_config)
#define MONITOR_IOC_ADD_UID    _IOW(MONITOR_IOC_MAGIC, 4, int)
#define MONITOR_IOC_DEL_UID    _IOW(MONITOR_IOC_MAGIC, 5, int)
#define MONITOR_IOC_ADD_SYS    _IOW(MONITOR_IOC_MAGIC, 6, int)
#define MONITOR_IOC_DEL_SYS    _IOW(MONITOR_IOC_MAGIC, 7, int)
#define MONITOR_IOC_SET_STATE  _IOW(MONITOR_IOC_MAGIC, 8, int) // 1 = ON, 0 = OFF

#endif
