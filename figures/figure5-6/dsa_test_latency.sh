#!/bin/bash
# Author: LRL

set -e

VERSION="latency"
DSA_CONFIG="dsas-4e1w-d"
MAX_CHAN=8
MAX_BATCH_SIZE=128
MAX_QUEUE_SIZE=128
MAX_ORDER=9
CODE_DIR="dsa_test_latency"
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

function dsa_test() {
    setup_dsa
    
    local DSA_COPY_METHODS=("dsa_multi_copy_pages" "dsa_batch_copy_pages" "dsa_copy_pages")

    sed -i "s/#define MAX_BATCH_SIZE [0-9]\+/#define MAX_BATCH_SIZE ${MAX_BATCH_SIZE}/g" ${SOURCE}
    dmesg -C

    for method in "${DSA_COPY_METHODS[@]}"; do
        sed -i "s/#define copy_pages_wrapper \(dsa_multi_copy_pages\|dsa_batch_copy_pages\|dsa_copy_pages\|cpu_copy_pages\)/#define copy_pages_wrapper ${method}/g" ${SOURCE}
        for (( chan = 1; chan <= MAX_CHAN; chan *= 2 )); do
            sed -i "s/#define MAX_CHAN [0-9]\+/#define MAX_CHAN ${chan}/g" ${SOURCE}
            for (( order = 0; order <= MAX_ORDER; ++order )); do
                if [[ ${method} == "dsa_batch_copy_pages" ]]; then
                    if [[ ${order} == 0 ]]; then
                        continue
                    fi
                    if (( (1 << order) > MAX_BATCH_SIZE )); then
                        break
                    fi
                fi
                if [[ ${method} == "dsa_copy_pages" ]] && (( (1 << order) > MAX_QUEUE_SIZE )); then
                    break
                fi
                sed -i "s/#define ORDER [0-9]\+/#define ORDER ${order}/g" ${SOURCE}
                (cd ${CODE_DIR} && make clean && make -j && make run)
                dmesg > "${LOG_DIR_PREFIX}/${method}-${chan}-${order}.log"
                dmesg -C
            done
            if [[ ${method} != "dsa_multi_copy_pages" ]]; then
                break
            fi
        done
    done    
}

function cpu_test() {
    method="cpu_copy_pages"
    sed -i "s/#define copy_pages_wrapper \(dsa_multi_copy_pages\|dsa_batch_copy_pages\|dsa_copy_pages\|cpu_copy_pages\)/#define copy_pages_wrapper ${method}/g" ${SOURCE}
    sed -i "s/#define MAX_CHAN [0-9]\+/#define MAX_CHAN 1/g" ${SOURCE}
    dmesg -C

    for (( order = 0; order <= MAX_ORDER; ++order )); do
        sed -i "s/#define ORDER [0-9]\+/#define ORDER ${order}/g" ${SOURCE}
        (cd ${CODE_DIR} && make clean && make -j && make run)
        dmesg > "${LOG_DIR_PREFIX}/${method}-1-${order}.log"
        dmesg -C
    done
}

function main() {
    dsa_test
    cpu_test
}

main "$@"