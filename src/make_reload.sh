#!/bin/bash

set -e
set -x

make
if [ "$UID" -eq 0 ]; then
    cpupower frequency-set -g schedutil
    rmmod memutil.ko
    insmod memutil.ko
    cpupower frequency-set -g memutil
else
    echo "Run this script as root to refresh the kernel module"
fi
