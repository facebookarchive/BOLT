#!/bin/bash -x

function run_one() {
    make clean_measurements
    make PERFCOUNTERS="${PERF}" BOLTOPTS="${BOLT}" results_bolt
    mkdir -p ../results-${1}-${2}
    cp * -v ../results-${1}-${2}
}

function run() {
    make clean_bolted_builds
    PERF=" "
    run_one ${1} 1
    PERF="-e instructions,L1-dcache-load-misses,dTLB-load-misses "
    run_one ${1} 2
    PERF="-e instructions,L1-icache-load-misses,iTLB-load-misses "
    run_one ${1} 3
    PERF="-e cycles,instructions,LLC-load-misses "
    run_one ${1} 4
}

function run_suite() {
    cd ${1}

    BOLT="-reorder-blocks=cache+ -dyno-stats -use-gnu-stack"
    run bb-reorder

    BOLT="-reorder-blocks=cache+ -dyno-stats -use-gnu-stack -icf=1"
    run bb-reorder-icf

    BOLT="-reorder-blocks=cache+ -dyno-stats -use-gnu-stack -icf=1 -split-functions=3 -split-all-cold"
    run bb-reorder-icf-split

    BOLT="-reorder-blocks=cache+ -reorder-functions=hfsort+ -split-functions=3 -split-all-cold -dyno-stats -icf=1 -use-gnu-stack"
    run bb-func

    BOLT="-reorder-blocks=cache+ -reorder-functions=hfsort+ -split-functions=3 -split-all-cold -dyno-stats -icf=1 -use-gnu-stack -simplify-rodata-loads -frame-opt=hot -indirect-call-promotion=jump-tables -indirect-call-promotion-topn=3 -plt=all"
    run bb-all
}

run_suite clang

