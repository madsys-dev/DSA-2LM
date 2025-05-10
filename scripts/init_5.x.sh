#!/bin/bash
set -e

# Disable hyper-threading
echo off > /sys/devices/system/cpu/smt/control

# TPP and MEMTIS kernel do not support this feature
# echo 3200000 | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_min_freq

# Enable Intel DSA in kernel mode
accel-config load-config -c ./dsas-4e1w-d.conf -e

# Initialize DSA for page migration
echo 1 > /proc/sys/vm/dsa_state

dmesg -C
