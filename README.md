# Syscall Throttling LKM
A high-performance Linux Kernel Module (LKM) designed to monitor and rate-limit system calls on a per-UID and per-program basis.
This repository contains the final project for the course of Advanced Operating Systems of the University of Rome Tor Vergata (faculty Computer Engineering).  
For an in-depth architectural analysis please refer to the full documentation: ./Relazione_SyscallThrottling.pdf

## Build and Load the Module
$ make  
$ sudo insmod syscall_monitor.ko

## Unload the Module
$ sudo rmmod syscall_monitor

## View Configurations and Statistics
$ cat /dev/syscall_monitor
