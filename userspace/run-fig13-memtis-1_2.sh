#!/bin/bash

BENCHMARKS="graph500 btree gapbs-pr XSBench pandas"
NVM_RATIO="1:2"

dmesg -c

for BENCH in ${BENCHMARKS}; do
    for NR in ${NVM_RATIO}; do
	echo 0 | tee /proc/sys/vm/use_dsa_copy_pages
	./scripts/run_bench_memtis.sh -B "${BENCH}" -R "${NR}" -V "fig13-memtis-1_2-baseline"
	echo 1 | tee /proc/sys/vm/use_dsa_copy_pages
	./scripts/run_bench_memtis.sh -B "${BENCH}" -R "${NR}" -V "fig13-memtis-1_2-dsa"
    done
done
