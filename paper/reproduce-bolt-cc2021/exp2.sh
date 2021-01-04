#!/bin/bash

#
# Experiment section 5.4 - tradeoff between BOLT processing time and clang perf
# Explore the impact on clang performance by reducing the number of functions
# BOLT optimizes on it.
#

# Benchmarks:
# clang-11 built with clang-11 (fdata collected building clang-11)
# input.cpp preprocessed off LLVM's lib/Target/X86/X86ISelLowering.cpp

function run_one() {
    make clean_side_b
    make NUM_EXP=3 INPUTBIN="clang" BOLTOPTS="${BOLT}" BOLTOPTSSEQ="${BOLTBASE}" all
    mkdir -p ./results-bolt-exp2-pct-${1}-${2}
    mkdir -p ./results-clang-exp2-pct-${1}-${2}
    mv measurements.test.exe clang.test
    cp -v measurents.test measurements.test.log.* measurements.base measurements.base.log.* results.txt ./results-bolt-exp2-pct-${1}-${2}
    make -f Makefile.exp2 clean_side_b
    make -f Makefile.exp2 INPUTBIN=./clang.test NUM_EXP=3 all
    cp data.test data.test.log.* data.base data.base.log.* results.exp2.txt ./results-clang-exp2-pct-${1}-${2}
}

function run() {
    make clean
    make -f Makefile.exp2 clean
    BOLTBASE="-reorder-blocks=cache+ -reorder-functions=hfsort \
-split-functions=1 -dyno-stats -eliminate-unreachable=0 \
-lite=1 -time-opts -time-rewrite -time-build"
    for curpct in $(seq 5 5 95); do
        BOLT="${BOLTBASE} -lite-threshold-pct=${curpct}"
        run_one ${curpct} normal
    done
}

function run_noscan() {
    make clean
    make -f Makefile.exp2 clean
    BOLTBASE="-reorder-blocks=cache+ -reorder-functions=hfsort \
-split-functions=1 -dyno-stats -eliminate-unreachable=0 \
-lite=1 -time-opts -time-rewrite -time-build -no-scan"
    for curpct in $(seq 5 5 95); do
        BOLT="${BOLTBASE} -lite-threshold-pct=${curpct}"
        run_one ${curpct} noscan
    done
}

function run_suite() {
    rm -rf results-clang-exp2*
    rm -rf results-bolt-exp2*
    run
    run_noscan
}

function gen_csv_line_full_run() {
    time=$(cat results-bolt-exp2-pct-${1}-normal/results.txt | grep runtime | cut -d ' ' -f 2)
    time_err=$(cat results-bolt-exp2-pct-${1}-normal/results.txt | grep runtime | cut -d ' ' -f 5)
    memory=$(cat results-bolt-exp2-pct-${1}-normal/results.txt | grep mem | cut -d ' ' -f 2)
    memory_err=$(cat results-bolt-exp2-pct-${1}-normal/results.txt | grep mem | cut -d ' ' -f 5)
    runtime=$(cat results-clang-exp2-pct-${1}-normal/results.exp2.txt | grep runtime | cut -d ' ' -f 2)
    runtime_err=$(cat results-clang-exp2-pct-${1}-normal/results.exp2.txt | grep runtime | cut -d ' ' -f 5)
    noscan_time=$(cat results-bolt-exp2-pct-${1}-noscan/results.txt | grep runtime | cut -d ' ' -f 2)
    noscan_time_err=$(cat results-bolt-exp2-pct-${1}-noscan/results.txt | grep runtime | cut -d ' ' -f 5)
    noscan_memory=$(cat results-bolt-exp2-pct-${1}-noscan/results.txt | grep mem | cut -d ' ' -f 2)
    noscan_memory_err=$(cat results-bolt-exp2-pct-${1}-noscan/results.txt | grep mem | cut -d ' ' -f 5)
    noscan_runtime=$(cat results-clang-exp2-pct-${1}-noscan/results.exp2.txt | grep runtime | cut -d ' ' -f 2)
    noscan_runtime_err=$(cat results-clang-exp2-pct-${1}-noscan/results.exp2.txt | grep runtime | cut -d ' ' -f 5)
    echo -ne "${1} ${time} ${time_err} ${memory} ${memory_err} ${runtime} ${runtime_err} ${noscan_time} ${noscan_time_err} ${noscan_memory} ${noscan_memory_err} ${noscan_runtime} ${noscan_runtime_err}\n"
}

function gen_csv_full_run() {
    echo -ne "\n** Ellapsed wall time **\n\n"
    echo -ne "Pct Time TimeErr Memory MemoryErr Runtime RuntimeErr NoScanTime NSTErr NoScanMemory NSMErr NoScanRuntime NSRErr\n"
    for curpct in $(seq 5 5 95); do
        gen_csv_line_full_run $curpct
    done
}

run_suite
gen_csv_full_run
