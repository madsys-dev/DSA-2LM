#!/bin/bash

if [ -z $NTHREADS ]; then
    NTHREADS=$(grep -c processor /proc/cpuinfo)
fi
export NTHREADS
NCPU_NODES=$(cat /sys/devices/system/node/has_cpu | awk -F '-' '{print $NF+1}')
NMEM_NODES=$(cat /sys/devices/system/node/has_memory | awk -F '-' '{print $NF+1}')
MEM_NODES=($(ls /sys/devices/system/node | grep node | awk -F 'node' '{print $NF}'))

CGROUP_NAME="htmm"
###### update DIR!
DIR=/data2/atc25-dsa2lm-artifact/userspace
FlameGraph=/data2/FlameGraph

CONFIG_PERF=off
CONFIG_NS=on
CONFIG_NW=off
CONFIG_CXL_MODE=on
STATIC_DRAM=""
DATE=""
VER=""

function func_cache_flush() {
    echo 3 > /proc/sys/vm/drop_caches
    free
    return
}

function func_tpp_setting() {
    echo 2 | tee /proc/sys/kernel/numa_balancing
    echo 1 | tee /sys/kernel/mm/numa/demotion_enabled
    swapoff -a
    echo 1000 | tee /proc/sys/vm/demote_scale_factor # 200 -> 1000 -> 2000, 200 is defaut for TPP

	echo 1 | tee /proc/sys/vm/use_concur_to_demote
	echo 1 | tee /proc/sys/vm/use_concur_to_compact

    echo "always" | tee /sys/kernel/mm/transparent_hugepage/enabled
    echo "always" | tee /sys/kernel/mm/transparent_hugepage/defrag
}

function func_prepare() {
    echo "Preparing benchmark start..."

	# set configs
	func_tpp_setting
	
	DATE=$(date +%Y%m%d%H%M)

	export BENCH_NAME
	export NVM_RATIO

	if [[ "x${NVM_RATIO}" == "xstatic" ]]; then
	    if [[ "x${STATIC_DRAM}" != "x" ]]; then
		export STATIC_DRAM
	    fi
	fi

	if [[ -e ${DIR}/bench_cmds/${BENCH_NAME}.sh ]]; then
	    source ${DIR}/bench_cmds/${BENCH_NAME}.sh
	else
	    echo "ERROR: ${BENCH_NAME}.sh does not exist."
	    exit -1
	fi
}

function func_main() {
    TIME="/usr/bin/time"

    # make directory for results
    mkdir -p ${DIR}/results_tpp/${BENCH_NAME}/${VER}/${NVM_RATIO}
    LOG_DIR=${DIR}/results_tpp/${BENCH_NAME}/${VER}/${NVM_RATIO}
	cp ${DIR}/scripts/run_bench_tpp.sh ${LOG_DIR}/run_bench_tpp_${DATE}.sh


    if [[ "x${CONFIG_PERF}" == "xon" ]]; then
		# PERF="perf stat -e dtlb_store_misses.walk_pending,dtlb_load_misses.walk_pending,dTLB-store-misses,dTLB-load-misses,cycle_activity.stalls_total"
		PERF="perf record -o ${LOG_DIR}/perf_${DATE}.data -F 99 -ag -C 44-45,47 --"
    else
		PERF=""
    fi
    
    # use 32 threads
	PINNING="taskset -c 0-31"
    # PINNING="taskset -c 0-47"
	# PINNING="taskset -c 0-47,96-143"
	# PINNING="taskset -c 48-95,144-191"

    echo "-----------------------"
    echo "NVM RATIO: ${NVM_RATIO}"
    echo "${DATE}"
    echo "-----------------------"

    sleep 2

    # flush cache
    func_cache_flush
    sleep 2

    cat /proc/vmstat >> ${LOG_DIR}/before_vmstat.log 

    echo 0 > /proc/timer
    echo 1 > /proc/timer

    ${TIME} -f "execution time %e (s)" ${PINNING} ${BENCH_RUN} 2>&1 | tee -a "${LOG_DIR}"/output.log

    echo 0 > /proc/timer
    cat /proc/timer | tee -a ${LOG_DIR}/timer.log

    cat /proc/vmstat >> ${LOG_DIR}/after_vmstat.log
    sleep 2

    sudo dmesg -c >> ${LOG_DIR}/dmesg.txt

	if [[ "x${CONFIG_PERF}" == "xon" ]]; then
		perf script -i ${LOG_DIR}/perf_${DATE}.data | ${FlameGraph}/stackcollapse-perf.pl > ${LOG_DIR}/out.perf-folded
		cat ${LOG_DIR}/out.perf-folded | ${FlameGraph}/flamegraph.pl > ${LOG_DIR}/perf-kernel_${DATE}.svg
	fi
}

function func_usage() {
    echo
    echo -e "Usage: $0 [-b benchmark name] [-s socket_mode] [-w GB] ..."
    echo
    echo "  -B,   --benchmark   [arg]    benchmark name to run. e.g., graph500, Liblinear, etc"
    echo "  -R,   --ratio       [arg]    fast tier size vs. capacity tier size: \"1:16\", \"1:8\", or \"1:2\""
    echo "  -D,   --dram        [arg]    static dram size [MB or GB]; only available when -R is set to \"static\""
    echo "  -V,   --version     [arg]    a version name for results"
    echo "  -NS,  --nosplit              disable skewness-aware page size determination"
    echo "  -NW,  --nowarm               disable the warm set"
    echo "        --cxl                  enable cxl mode [default: disabled]"
    echo "  -?,   --help"
    echo "        --usage"
    echo
}


################################ Main ##################################

if [ "$#" == 0 ]; then
    echo "Error: no arguments"
    func_usage
    exit -1
fi

# get options:
while (( "$#" )); do
    case "$1" in
	-B|--benchmark)
	    if [ -n "$2" ] && [ ${2:0:1} != "-" ]; then
		BENCH_NAME=( "$2" )
		shift 2
	    else
		echo "Error: Argument for $1 is missing" >&2
		func_usage
		exit -1
	    fi
	    ;;
	-V|--version)
	    if [ -n "$2" ] && [ ${2:0:1} != "-" ]; then
		VER=( "$2" )
		shift 2
	    else
		func_usage
		exit -1
	    fi
	    ;;
	-P|--perf)
	    CONFIG_PERF=on
	    shift 1
	    ;;
	-R|--ratio)
	    if [ -n "$2" ] && [ ${2:0:1} != "-" ]; then
		NVM_RATIO="$2"
		shift 2
	    else
		func_usage
		exit -1
	    fi
	    ;;
	-D|--dram)
	    if [ -n "$2" ] && [ ${2:0:1} != "-" ]; then
		STATIC_DRAM="$2"
		shift 2
	    else
		func_usage
		exit -1
	    fi
	    ;;
	-NS|--nosplit)
	    CONFIG_NS=on
	    shift 1
	    ;;
	-NW|--nowarm)
	    CONFIG_NW=on
	    shift 1
	    ;;
	--cxl)
	    CONFIG_CXL_MODE=on
	    shift 1
	    ;;
	-H|-?|-h|--help|--usage)
	    func_usage
	    exit
	    ;;
	*)
	    echo "Error: Invalid option $1"
	    func_usage
	    exit -1
	    ;;
    esac
done

if [ -z "${BENCH_NAME}" ]; then
    echo "Benchmark name must be specified"
    func_usage
    exit -1
fi

func_prepare
func_main
