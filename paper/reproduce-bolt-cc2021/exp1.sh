#!/bin/bash

#
# This script run experiments to collect wall time and peak memory consumption
# of BOLT when optimizing clang-11 and gcc-10.
#

# Opensource:
# clang-11 built with clang-11 (fdata collected building clang-11)
# GCC-10 bootstrapped with GCC-10 (fdata collected building clang-11)

function run_one() {
    make clean_side_b
    make INPUTBIN="${INPUTBIN}" BOLTOPTS="${BOLT}" all
    mkdir -p ./results-${1}-${2}
    cp aggregate.txt measurements* comparison* results.txt -v ./results-${1}-${2}
}

function run() {
    make clean
    BOLT="-reorder-blocks=cache+ -reorder-functions=hfsort \
-split-eh -frame-opt=hot -eliminate-unreachable=0 \
-split-functions=3 -split-all-cold -dyno-stats -icf=1 -use-gnu-stack \
-lite=1 -time-opts -time-rewrite -time-build"
    run_one ${1} all
    BOLT="-reorder-blocks=cache+ -reorder-functions=hfsort \
-split-eh -frame-opt=hot -eliminate-unreachable=0 \
-split-functions=3 -split-all-cold -dyno-stats -icf=1 -use-gnu-stack \
-lite=1 -no-threads -time-opts -time-rewrite -time-build"
    run_one ${1} lite
    BOLT="-reorder-blocks=cache+ -reorder-functions=hfsort \
-split-functions=3 -split-all-cold -dyno-stats -icf=1 -use-gnu-stack \
-split-eh -frame-opt=hot -eliminate-unreachable=0 \
-lite=0 -time-opts -time-rewrite -time-build"
    run_one ${1} threads
}

function run_suite() {
    rm -rf results-clang*
    INPUTBIN="clang"
    run clang

    rm -rf results-gcc*
    INPUTBIN="gcc"
    run gcc
}

function gen_csv_line_full_run() {
    threads=$(cat results-${2}-threads/results.txt | grep ${3} | cut -d ' ' -f 2)
    thr_err=$(cat results-${2}-threads/results.txt | grep ${3} | cut -d ' ' -f 5)
    lite=$(cat results-${2}-lite/results.txt | grep ${3} | cut -d ' ' -f 2)
    lite_err=$(cat results-${2}-lite/results.txt | grep ${3} | cut -d ' ' -f 5)
    all=$(cat results-${2}-all/results.txt | grep ${3} | cut -d ' ' -f 2)
    all_err=$(cat results-${2}-all/results.txt | grep ${3} | cut -d ' ' -f 5)
    echo -ne "${4} ${1} ${threads} ${thr_err} ${lite} ${lite_err} ${all} ${all_err}\n"
}

function gen_csv_full_run() {
    echo -ne "\n** Ellapsed wall time **\n\n"
    echo -ne "X Input NoLiteThreads ErrA LiteNoThreads ErrB LiteThreads ErrC\n"
    gen_csv_line_full_run Clang clang runtime 2
    gen_csv_line_full_run GCC gcc runtime 3
    echo -ne "\n** RSS Memory **\n\n"
    echo -ne "X Input NoLiteThreads ErrA LiteNoThreads ErrB LiteThreads ErrC\n"
    gen_csv_line_full_run Clang clang mem 2
    gen_csv_line_full_run GCC gcc mem 3
}

AWK_SCRIPT="                                                               \
	BEGIN                                                                    \
 	{                                                                        \
	  sum = 0;                                                               \
	  sumsq = 0;                                                             \
	};                                                                       \
	{                                                                        \
    sum += \$1;                                                            \
    sumsq += (\$1)^2;                                                      \
	  printf \"Data point %s: %f\n\", NR, \$1                                \
  }                                                                        \
  END                                                                      \
	{                                                                        \
	  printf \"Mean: %f StdDev: %f\n\", sum/NR, sqrt((sumsq - sum^2/NR)/(NR-1))\
	};"

function process_one_optimization() {
    DEBUG_DATA_POINTS=0
    echo -n '' > temp.txt
    for step in $(seq 1 5); do
        cat results-"${2}"-threads/measurements.base.log.${step} | grep "${1}" | tail -n 1 | cut -d'(' -f 4 | cut -d')' -f 2 &>> temp.txt
    done
    cat temp.txt | awk "${AWK_SCRIPT}" | tail -n 1  &> temp2.txt
    if [[ $DEBUG_DATA_POINTS -ne 0 ]]; then
        echo 'Data points side A:'
        cat temp.txt
        cat temp2.txt
    fi
    side_a=$(cat temp2.txt | cut -d ' ' -f 2)
    side_a_err=$(cat temp2.txt | cut -d ' ' -f 4)
    echo -n '' > temp.txt
    for step in $(seq 1 5); do
        cat results-"${2}"-threads/measurements.test.log.${step} | grep "${1}" | tail -n 1 | cut -d'(' -f 4 | cut -d')' -f 2 &>> temp.txt
    done
    cat temp.txt | awk "${AWK_SCRIPT}" | tail -n 1  &> temp2.txt
    if [[ $DEBUG_DATA_POINTS -ne 0 ]]; then
        echo 'Data points side B:'
        cat temp.txt
        cat temp2.txt
    fi
    side_b=$(cat temp2.txt | cut -d ' ' -f 2)
    side_b_err=$(cat temp2.txt | cut -d ' ' -f 4)
    rm temp.txt temp2.txt
	  ratio=$(echo "scale=8;($side_a / $side_b)" | bc);
    ratio_err=$(echo "scale=8;($side_a / $side_b) * sqrt (($side_a_err/$side_a)^2 + ($side_b_err/$side_b)^2)" | bc);
}

function gen_csv_line_individual_opts() {
    # could be "frame analysis" to focus just on analysis
    process_one_optimization "frame analysis" ${2}
    frame=$ratio
    frame_err=$ratio_err
    process_one_optimization identical-code-folding ${2}
    icf=$ratio
    icf_err=$ratio_err
    process_one_optimization buildCFG ${2}
    buildcfg=$ratio
    buildcfg_err=$ratio_err
    process_one_optimization reorder-blocks ${2}
    cacheplus=$ratio
    cacheplus_err=$ratio_err
    echo -ne "${3} ${1} ${frame} ${frame_err} ${icf} ${icf_err} ${buildcfg} ${buildcfg_err} ${cacheplus} ${cacheplus_err}\n"

}

function gen_csv_individual_opts() {
    echo -ne "X Input FrameOpts ErrA ICF ErrB BuildCFG ErrC ReorderBlocks ErrD\n"
    gen_csv_line_individual_opts Clang clang 2
    gen_csv_line_individual_opts GCC gcc 3
}

run_suite
gen_csv_full_run
gen_csv_individual_opts
