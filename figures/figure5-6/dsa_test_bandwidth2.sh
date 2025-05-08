#!/bin/bash
# Author: LRL

set -e

VERSION="bandwidth"
DSA_CONFIG="dsas-4e1w-d"
MAX_CHAN=8
MAX_BATCH_SIZE=128
MAX_QUEUE_SIZE=128
MAX_ORDER=9
CODE_DIR="dsa_test_bandwidth2"
SOURCE="${CODE_DIR}/dsa_test.c"
LOG_DIR_PREFIX="results/${VERSION}"

[[ ! -d ${LOG_DIR_PREFIX} ]] && mkdir -p ${LOG_DIR_PREFIX}

function setup_dsa() {
    local res
    res=$(accel-config list)
    if [[ ${#res} -gt 3 ]]; then
        set +e
        accel-config disable-device dsa0 dsa2 dsa4 dsa6 dsa8 dsa10 dsa12 dsa14
        set -e
    fi
    accel-config load-config -c ./"${DSA_CONFIG}.conf" -e
}

function dsa_channel_test() {
    setup_dsa

    dmesg -C
    method="dsa_multi_copy_pages"
    order="9"
    sed -i "s/#define ORDER [0-9]\+/#define ORDER ${order}/g" ${SOURCE}
    for (( chan = 1; chan <= MAX_CHAN; chan *= 2 )); do
        sed -i "s/#define MAX_CHAN [0-9]\+/#define MAX_CHAN ${chan}/g" ${SOURCE}
        
        (cd ${CODE_DIR} && make clean && make -j && make run)
        dmesg > "${LOG_DIR_PREFIX}/${method}-${chan}-${order}.log"
        dmesg -C
    done

    order="0" # simulate batch_size is 1 for batch bandwidth results
    chan="1"
    sed -i "s/#define ORDER [0-9]\+/#define ORDER ${order}/g" ${SOURCE}
    sed -i "s/#define MAX_CHAN [0-9]\+/#define MAX_CHAN ${chan}/g" ${SOURCE}
    (cd ${CODE_DIR} && make clean && make -j && make run)
    dmesg > "${LOG_DIR_PREFIX}/${method}-${chan}-${order}.log"
    dmesg -C
}

function dsa_batch_size_test() {
    setup_dsa

    dmesg -C
    method="dsa_batch_copy_pages"
    chan="1"
    sed -i "s/#define MAX_CHAN [0-9]\+/#define MAX_CHAN ${chan}/g" ${SOURCE}
    for (( order = 0; order <= MAX_ORDER; ++order )); do
        if [[ ${order} == 0 ]]; then
            continue
        fi
        if (( (1 << order) > MAX_BATCH_SIZE )); then
            break
        fi
        sed -i "s/#define ORDER [0-9]\+/#define ORDER ${order}/g" ${SOURCE}
        
        (cd ${CODE_DIR} && make clean && make -j && make run)
        dmesg > "${LOG_DIR_PREFIX}/${method}-${chan}-${order}.log"
        dmesg -C
    done
}

function main() {
    # Don't run dsa_channel_test and dsa_batch_size_test at the same time
    # We don't use copy_pages_wrapper so you need to modify the source code manually
    # line 430-436 for dsa_channel_test, line 437-443 for dsa_batch_size_test

    # dsa_channel_test
    
    # For some code implementation reasons, 
    # require num_online_cpus >= 128 (as work_queue_siz is 128).
    # So we need to enable hyper-threading.
    state=$(cat /sys/devices/system/cpu/smt/control)
    if [[ ${state} == "off" ]]; then
        echo on > /sys/devices/system/cpu/smt/control
    fi

    dsa_batch_size_test # We only synchronize for DSA
                        # completion when the work_queue is full.

    if [[ ${state} == "off" ]]; then
        echo off > /sys/devices/system/cpu/smt/control
    fi
}

main "$@"