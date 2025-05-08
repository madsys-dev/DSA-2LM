#!/bin/bash
# Author: LRL

set -e

VERSION="4e1w-d"
# VERSION="movdir64b"

WQs=1
WQ_MODE="d" # d for dedicated, s for shared
ENGINES=4
DSA_ID=0

DSA_CONF="dsa${DSA_ID}-${ENGINES}e${WQs}w-${WQ_MODE}"
# timestamp=$(date +"%Y%m%d%H%M")
LOG_DIR_PREFIX="results/${VERSION}"
[[ ! -d ${LOG_DIR_PREFIX} ]] && mkdir -p ${LOG_DIR_PREFIX}

readonly LOG_DIR_PREFIX DSA_CONF DSA_ID ENGINES WQ_MODE WQs VERSION

function setup_dsa() {
    local wq_size=$1
    if [[ -z ${wq_size} ]]; then
        local wq_size=128
        local conf="${DSA_CONF}.conf"
    else
        local conf="${DSA_CONF}-wqsz${wq_size}.conf"
    fi
    echo -e "\n==> Setting DSA <==\n"
    ./scripts/setup_dsa.sh -d dsa${DSA_ID} -w ${WQs} -m ${WQ_MODE} -e ${ENGINES} -s "${wq_size}"
    accel-config list | tee "${LOG_DIR}/${conf}"
}

function func_cache_flush() {
    echo 3 > /proc/sys/vm/drop_caches
    # free -h
    sync
    # free -h
}

function set_args() {
    if [[ -z ${transfer_size} || -z ${iterations} || -z ${buffer_count} || -z ${mode} || ("${mode}" != "cpu" && -z ${batch_size})]]; then
        echo -e "\nERROR: some args are empty\n"
        exit 1
    fi

    args="-s${transfer_size} -i${iterations} -n${buffer_count}"
    
    # set mode
    if [[ "${mode}" == "async" ]]; then
        args+=" -x 0x000100"
    elif [[ "${mode}" == "cpu" ]]; then
        args+=" -m"
    fi
     
    # set batch size
    if [[ "${mode}" != "cpu" && -n ${batch_size} ]]; then
        args+=" -b${batch_size}"
    fi

    # set NUMA node
    if [[ -n ${lr} ]]; then
        [[ ${lr} == "L,L" ]] && args+=" -S${LOCAL_NODE},${LOCAL_NODE}"
        [[ ${lr} == "L,R" ]] && args+=" -S${LOCAL_NODE},${REMOTE_NODE}"
        [[ ${lr} == "R,L" ]] && args+=" -S${REMOTE_NODE},${LOCAL_NODE}"
        [[ ${lr} == "R,R" ]] && args+=" -S${REMOTE_NODE},${REMOTE_NODE}"
    fi

    [[ -n ${other_args} ]] && args+=" ${other_args}"
}

# remember to set $filename 
function run_test() {
    if [[ -z ${filename} ]]; then
        echo -e "\nERROR: filename is empty\n"
        exit 1
    fi
    
    func_cache_flush
    set_args

    PREFIX_COMMAND=""
    [[ -n ${LOCAL_NODE} ]] && PREFIX_COMMAND="numactl -N ${LOCAL_NODE}"

    local command
    if [[ "${mode}" != "cpu" ]]; then
        # shellcheck disable=SC2086
        DSA_PERF_MICROS_LOG_LEVEL=info ${PREFIX_COMMAND} ./src/dsa_perf_micros ${args} 2>&1 | tee ${LOG_DIR}/"${filename}"
        command="DSA_PERF_MICROS_LOG_LEVEL=info ${PREFIX_COMMAND} ./src/dsa_perf_micros ${args} 2>&1 | tee ${LOG_DIR}/${filename}"
    else
        # shellcheck disable=SC2086
        ${PREFIX_COMMAND} ./src/dsa_perf_micros ${args} 2>&1 | tee ${LOG_DIR}/"${filename}"
        command="${PREFIX_COMMAND} ./src/dsa_perf_micros ${args} 2>&1 | tee ${LOG_DIR}/${filename}"
    fi
    
    echo "${command}" | tee -a ${LOG_DIR}/"${filename}"
    unset filename
}

# Run about 45min for memcpy and DSA
# Run about 39min for movdir64b
function figure6() {
    # Figure 6
    LOG_DIR="${LOG_DIR_PREFIX}"
    [[ ! -d ${LOG_DIR} ]] && mkdir -p ${LOG_DIR}

    LR="L,L L,R R,L R,R"
    MODE="cpu sync" # memcpy, DSA
    # MODE="cpu" # movdir64b
    TRANSFER_SIZE="64 128 256 512 1K 2K 4K 8K 16K 64K 128K 256K 512K 1M 2M"
    
    LOCAL_NODE=0
    REMOTE_NODE=1
    batch_size=1
    buffer_count=128
    other_args="-c -w0 -zF,F -o3"

    setup_dsa ""

    for lr in ${LR}; do
        for mode in ${MODE}; do
            if [[ ${mode} == "cpu" ]]; then
                iterations=25 # cpu test is too slow
            else
                iterations=1000
            fi
            for transfer_size in ${TRANSFER_SIZE}; do
                filename="${mode}-${lr}-${transfer_size}.log"
                run_test
                # if [[ ${lr} == "L,R" ]]; then
                    filename="${mode}-${lr}-${transfer_size}-latency.log"
                    other_args_bak=${other_args}
                    other_args="${other_args} -q1"
                    run_test
                    other_args=${other_args_bak}
                # fi
            done
        done
    done

    unset lr LR LOCAL_NODE REMOTE_NODE
}

function main() {
    figure6
}

main "$@"