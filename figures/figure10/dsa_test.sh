#!/bin/bash
# Author: LRL

set -e

VERSION="results"
DSA_CONFIG="dsas-4e1w-d"
MAX_CHAN=8
MAX_BATCH_SIZE=128
MAX_QUEUE_SIZE=128
MAX_ORDER=9
CODE_DIR="dsa_test"
SOURCE="${CODE_DIR}/dsa_test.c"
LOG_DIR_PREFIX="${VERSION}"

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

function dsa_test() {
    setup_dsa
    
    local DSA_COPY_METHODS=("cpu_copy_page_lists" "mix_copy_page_lists" "dsa_copy_page_lists")

    sed -i "s/#define MAX_BATCH_SIZE [0-9]\+/#define MAX_BATCH_SIZE ${MAX_BATCH_SIZE}/g" ${SOURCE}
    dmesg -C

    workload="workload-a"
    sed -i 's/^static int test_huge_page_idx.*$/static int test_huge_page_idx[] = {5};/g' ${SOURCE}

    for method in "${DSA_COPY_METHODS[@]}"; do
        sed -i "s/#define copy_page_lists_wrapper \(cpu_copy_page_lists\|mix_copy_page_lists\|dsa_copy_page_lists\)/#define copy_page_lists_wrapper ${method}/g" ${SOURCE}
        (cd ${CODE_DIR} && make clean && make -j && make run)
        dmesg > "${LOG_DIR_PREFIX}/${method}-${workload}.log"
        dmesg -C
    done    

    workload="workload-b"
    sed -i 's/^static int test_huge_page_idx.*$/static int test_huge_page_idx[] = {5, 16, 25, 32, 36, 40, 45, 50, 52, 55, 60, 70, 75, 80, 85, 90, 95, 100, 105, 110, 115, 120, 125, 130};/g' ${SOURCE}

    for method in "${DSA_COPY_METHODS[@]}"; do
        sed -i "s/#define copy_page_lists_wrapper \(cpu_copy_page_lists\|mix_copy_page_lists\|dsa_copy_page_lists\)/#define copy_page_lists_wrapper ${method}/g" ${SOURCE}
        (cd ${CODE_DIR} && make clean && make -j && make run)
        dmesg > "${LOG_DIR_PREFIX}/${method}-${workload}.log"
        dmesg -C
    done    
}

function main() {
    dsa_test
}

main "$@"