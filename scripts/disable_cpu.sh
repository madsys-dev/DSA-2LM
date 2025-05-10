#!/bin/bash

# Check the number of arguments
if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <NUMA node>"
    exit 1
fi

NUMA_NODE=$1

# Identify all CPU cores on the specified NUMA node
cpu_list=$(numactl --hardware | grep "node ${NUMA_NODE} cpus" | cut -d ':' -f2)

for cpu in $cpu_list; do
    echo "Disabling CPU core: $cpu"
    echo 0 > /sys/devices/system/cpu/cpu${cpu}/online
done

echo "All CPU cores on NUMA node $NUMA_NODE have been disabled."
