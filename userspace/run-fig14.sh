#!/bin/bash

BENCHMARKS="graph500 pandas"
NVM_RATIO="1:1 1:2 1:4 1:8 1:16"

dmesg -c

for BENCH in ${BENCHMARKS}; do
    for NR in ${NVM_RATIO}; do
	echo 0 | tee /proc/sys/vm/use_dsa_copy_pages
	./scripts/run_bench_memtis.sh -B "${BENCH}" -R "${NR}" -V "fig14-baseline"
	echo 1 | tee /proc/sys/vm/use_dsa_copy_pages
	./scripts/run_bench_memtis.sh -B "${BENCH}" -R "${NR}" -V "fig14-dsa"
    done
done
