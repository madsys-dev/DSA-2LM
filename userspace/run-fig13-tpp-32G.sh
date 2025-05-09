#!/bin/bash

BENCHMARKS="graph500 btree XSBench pandas"
NVM_RATIO="32GB"

dmesg -c

for BENCH in ${BENCHMARKS}; do
    for NR in ${NVM_RATIO}; do
	echo 0 | tee /proc/sys/vm/use_dsa_copy_pages
	./scripts/run_bench_tpp.sh -B "${BENCH}" -R "${NR}" -V "fig13-tpp-baseline"
	echo 1 | tee /proc/sys/vm/use_dsa_copy_pages
	./scripts/run_bench_tpp.sh -B "${BENCH}" -R "${NR}" -V "fig13-tpp-dsa"
    done
done
