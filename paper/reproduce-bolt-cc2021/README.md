# Artifact for paper Lightning BOLT: Powerful, Fast and Scalable Binary Optimization

The open-source workload evaluated in this paper is clang 11 and gcc 10. These
two workloads need to be bootstrapped (built with themselves). Our goal is
to demonstrate reductions in wall time and memory consumption when running
BOLT on these workloads with different techniques: parallelization and
selective optimizations. This is accomplished with the first experiment
(exp1.sh script, Figures 3, 4 and 5 in the paper). The second experiment
(exp2.sh script, Figures 6 and 7 in the paper) shows how can we trade
BOLT speed for output binary performance.

## Docker image usage

For ease of reproduction, we supply a docker image. The image created by the
Dockerfile in this folder is available at dockerhub, alias
rafaelauler/bolt_cc2021:latest.
Simply pull this image and create a container based on it:

```
$ docker run -it rafaelauler/bolt_cc2021:latest /bin/bash
```

Then, inside the container, run:

```
# ./exp1.sh
```

to reproduce results for Figures 3, 4 and 5. Run:

```
# ./exp2.sh
```

to reproduce results for Figures 6 and 7.

### Hardware prerequisites

Please allocate at least 10GB of RAM for your docker container and a few GB of
disk. Having less than 10GB will cause Linux to silently kill your experiments.

### Experiment configuration

Edit Makefile and change NUM\_EXP to control how many repetitions of the same run
will be used to reduce noise. Default is 5. Recommended is 20 depending on your
environment, but will significantly increase experiment total run time. The same
should be done at exp2.sh by changing both lines containing NUM\_EXP=. For exp2,
the default is 3. Recommended is 20, but may take several hours to complete if
set up in this way.

If you are familiar with bash, feel free to change exp1.sh in any way you want.

### Troubleshooting

Experiment logs are stored in "results-*" folders. If trial runs are not
running, inspect log files to check for the error message. If the experiment is
being killed by the system OOM killer (out of memory), please note that no
special message will be logged. Copy BOLT's command line from Makefile output,
run in the shell and verify that the process exits with error code zero.

## Reading and interpreting experiment results

### Output of exp1.sh

The script prints CSS files with the results of the graphs. These CSS files
are used by pgfplots in our paper LaTeX source, but can be used to feed any
plotting software. The data for the 3 graphs collected by exp1.sh appear below:

```
** Ellapsed wall time **

X Input NoLiteThreads ErrA LiteNoThreads ErrB LiteThreads ErrC
2 Clang 1.46358970 .04859227 1.81566721 .05150352 2.83230438 .04442293
3 GCC 1.51034248 .08512436 1.49332796 .08979905 1.67936053 .08357952
```

Here, NoLiteThreads corresponds to  "Parallelization", LiteNoThreads corresponds
to "Selective Optimizations" and LiteThreads, "Both" in Figure 3. Columns ErrA,
ErrB and ErrC presents the error margin for each respective mode.

```
** RSS Memory **

X Input NoLiteThreads ErrA LiteNoThreads ErrB LiteThreads ErrC
2 Clang -1.19663600 .02862245 58.29191000 0 57.71797900 .01035655
3 GCC -3.93468300 .05596986 45.07975200 0 16.52376700 .13943202
```

Here, we have the same correspondence, but for Figure 4.

```
X Input FrameOpts ErrA ICF ErrB BuildCFG ErrC ReorderBlocks ErrD
2 Clang 4.72632786 .22332385 2.41650290 .10167965 2.45481867 .15541132 17.18659521 1.26428873
3 GCC 3.49823118 1.08338715 2.31376928 .23628329 2.75669947 .13830857 17.37811004 .96494371
```

Here, FrameOpts corresponds to "Frame Analysis" in Figure 5, while the other
columns have the same name of their labels in the original graph.

### Output of exp2.sh

This script will also print a CSV file, but the data is used to render Figures
6 and 7 of the paper.

Example:

```
Pct Time TimeErr Memory MemoryErr Runtime RuntimeErr NoScanTime NSTErr NoScanMemory NSMErr NoScanRuntime NSRErr
(...)
85 1.79722926 .05465379 47.68480900 .09517911 1.13532441 .06899232 2.31494870 .07823635 49.25582000 .07861237 1.07583090 .05862991
90 1.90698996 .07089054 50.99616500 .08330651 1.10291553 .06897984 2.56187489 .09216306 52.55787800 .06858612 1.06116507 .04085856
95 2.06324807 .06601715 53.86611800 .08213860 1.08555963 .06567814 2.86586029 .11378330 55.42840100 .06412694 1.04035116 .03690108
```

Here, we see that at Pct=95 (optimizing only the hottest 5% functions of clang),
BOLT is running 2.06x faster than the baseline (Fig. 6), consuming 53% less
memory (Fig. 6) and the resulting clang is running 1.08x faster than baseline
(Fig7). Similarly, the no-scan version of BOLT is running 2.86x faster, using
55% less memory but the resulting clang is running only 1.04x faster.

## Building the docker image yourself

Copy the contents of this folder to a local folder, then run:

```
$ docker build . -t bolt_cc2021:latest
```

This will bootstrap clang-11 and gcc-10 and will build BOLT.

### Hardware requirements

Bootstrapping the compilers is quite demanding and may take multiple hours
on most systems. Edit Makefile and adjust NUM\_CORES to the number of cores
available to build your image to speed up the build. Please have at least
30GB of disk available for intermediate build artifacts.

### Troubleshooting

The first thing to check if something goes wrong is to make sure your docker
installation has enough space for building. If you need to free up docker
space, use commands such as "docker system prune" to remove unused containers,
cached build images and more, but please be careful as this will delete data.
The second most common error is running out of RAM because you set NUM\_CORES
too high. Lower NUM\_CORES to meet your RAM availability.

## Running experiments outside of docker

You are free to run this Makefile and scripts in any system that meets the
software prerequisites. You have higher chances of succeeding by doing this
on a system that you already know can build clang-11 and gcc-10. Make sure
you have these ubuntu packages available (or the equivalent package for your
distro):

- git
- build-essential
- linux-tools-generic (Linux perf)
- cmake
- ninja-build
- bc (used by scripts)
- g++-multilib (used by BOLT)
- time (used by scripts)
- flex (used by gcc)
- bison (used by gcc)
- file
- curl (used by gcc)
- python
- libjemalloc-dev (used by BOLT)

Then, run the following make rules to build the experiment environment:

```
$ make $(pwd)/gcc
$ make $(pwd)/clang
$ make build_bolt
```

This will produce two input files used to test BOLT, clang and gcc. However,
you still need to collect the BOLT profile for these files. You can only do this
on an Intel system with LBR, and with perf record -j being able to record
samples. You also need a lot of disk space (~100GB) to make sure you have room
to store the samples (the perf.data file can get quite large). To materialize
the BOLT profile for each input, run:

```
$ make $(pwd)/gcc.fdata
$ make $(pwd)/clang.fdata
$ cp inputs/input.cpp . # available inside inputs.tar.bz2
```

If your system requires you to be root to run perf record, this may fail when
starting sample collection. In this case, edit the Makefile to add a "sudo"
before perf record, and add "sudo chown youruser clang.perf.data" or equivalent
to make the profile available to your user.

### Gotchas

The system where you run the experiments matters. That's why it is easier to run
in Ubuntu 18.04 or inside the docker container based on Ubuntu. The reason for
that is because BOLT profile, in order to be effective, depends on the input binary
used in BOLT being the same one used when the profile was created or at least having
the same instructions in important hot functions. When you change systems, even
though we bootstrap clang-11, clang-11 can end up containing slightly different
instructions than it would in another system, rendering the .fdata files that we
make available quite stale. This does not happen for gcc-10. Therefore, if
you change the system, you need to recollect profile (regenerate the fdata file)
for clang-11 and you need the disk space and time required to do that.
