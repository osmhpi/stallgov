#!/bin/sh

make
cpupower frequency-set -g schedutil
rmmod memutil.ko
insmod memutil.ko
cpupower frequency-set -g memutil

