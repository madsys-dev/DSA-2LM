#!/bin/bash

BIN=/data2/memtis-userspace/bench_dir/polars-benchmark
BENCH_RUN="${BIN}/run-pandas.sh"
BENCH_DRAM=""


if [[ "x${NVM_RATIO}" == "x16GB" ]]; then
    BENCH_DRAM="16384MB"
elif [[ "x${NVM_RATIO}" == "x32GB" ]]; then
    BENCH_DRAM="32768MB"
elif [[ "x${NVM_RATIO}" == "x1:16" ]]; then
    BENCH_DRAM="3850MB"
elif [[ "x${NVM_RATIO}" == "x1:8" ]]; then
    BENCH_DRAM="7200MB"
elif [[ "x${NVM_RATIO}" == "x1:4" ]]; then
    BENCH_DRAM="13107MB"
elif [[ "x${NVM_RATIO}" == "x1:2" ]]; then
    BENCH_DRAM="21800MB"
elif [[ "x${NVM_RATIO}" == "x1:1" ]]; then
    BENCH_DRAM="32768MB"
elif [[ "x${NVM_RATIO}" == "x1:0" ]]; then
    BENCH_DRAM="75000MB"
fi


export BENCH_RUN
export BENCH_DRAM
