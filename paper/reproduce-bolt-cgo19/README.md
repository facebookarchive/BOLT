# reproduce-bolt-cgo19

The open-source workload evaluated on this paper is clang 7 and gcc 8.2. Our goal is
to demonstrate that building clang/gcc with all optimizations, including LTO and
PGO, still leaves opportunities for a post-link optimizer such as BOLT to do
a better job at basic block placement and function reordering, significantly
improving workload performance.

In a nutshell, the paper advocates for a two-step profiling
pipeline (PGO and BOLT) evaluated on a data-center environment, showing
that doing a single pass of profile collection is not
enough to leverage the full potential of profile-guide optimizations.
In this two-step approach, PGO or AutoFDO can be used to feed the
compiler with profile information, which is an important enabler of better
inlining decisions, while a second pass is used to collect profile for a
post-link optimizer such as BOLT, enabling us to improve basic block order
and function order in the final binary and further increase performance.

Notice that even though a PGO-enabled compiler already uses profile information
to enhance layout decisions, the reason why BOLT can still
get performance wins on top of it is
because BOLT profile is more accurate at the final steps of the compilation
and excels at such tasks. BOLT profile
is collected and applied directly at binary level and there is no
imperfect conversion step trying to map PC addresses back to source code
that relies on the accuracy of debug information. Since BOLT doesn't rely
on source code, it can also optimize assembly-written code or library code
you are statically linking for which there are no sources available.

Furthermore, profiles used in compilers and BOLT,
for space-efficiency reasons, are not traces but an aggregation of execution
counts. This aggregation loses information: a given function accumulates
the superposition of many traces, each one possibly exercising a different path
of basic blocks, e.g. depending on its callee. Thus, it has limited
applicability and significant code
changes may render it stale. For example, after the compiler decides to inline a function that
was not previously inlined in the code where the profile was originally collected,
it now lacks the correct profile for this function when called exclusively
at that call site. BOLT, by operating at the final binary after
all compilation decisions that substantially change code have been taken, is
in a better position to do code layout and low-level optimizations suitable
to a lower-level IR.

## Usage

Clone this repo, cd to either the clang or the gcc folder, depending on the workload
you want to evaluate, and run make as in the following commands:

```
> cd clang      # or gcc
> vim Makefile  # edit NUMCORES according to your system, customize Makefile
> make
> cat results.txt
```

Check the results.txt file with the numbers for the clang-build bars in
Figures 7 and 8 of the paper.

These Makefile rules are based on the steps described at
https://github.com/facebookincubator/BOLT/blob/master/docs/OptimizingClang.md

# Hardware prerequisites

You will need a machine with a fair amount of RAM (32GB RAM is OK for the GCC
evaluation, but more is needed for Clang because of the expensive LTO tasks
running in parallel) and around 120GB of free disk space.
This machine needs an Intel processor with LBR support for profile data
collection. By now, LBR is pretty established on Intel processors -
microarchitectures Sandy Bridge (2011) and later supports LBR.
The lower your core count, the slower it will be, as this is building a large
code base several times (adjust the NUMCORES Makefile variable). The whole
process (evaluating GCC and Clang) takes about 6 hours using 40 threads running
simultaneously on our Broadwell setup (see below for specs).

# Software prerequisites

We present next a brief list of software prerequisites along with the
corresponding CentOS 7 package install command:

```
> git -- yum install git
> cmake -- yum install cmake
> ninja -- yum install ninja-build
> flex -- yum install flex
```

Since we build Clang/LLVM, check here for its own list
of requirements: http://llvm.org/docs/GettingStarted.html#requirements

In general, for building Clang/LLVM, you should be fine if your system has a
relatively modern C++ compiler such as gcc version 4.8.0 or higher.

# Troubleshooting

Make sure you understand the rules in the Makefile before diagnosing an issue
and check the log files.
If one of the steps to build a compiler failed, it is best to wipe the compiler
build folder entirely before running make again.

There are 5 compilers installed for clang and 4 for gcc:

```
> benchmarks/stage1
> benchmarks/stage2       # (clang only)
> benchmarks/clangbolt    # or gccbolt
> benchmarks/clangpgo     # or gccpgo
> benchmarks/clangpgobolt $ or gccpgobolt
```

You may want to delete one of these folders if the rules failed to make the
compiler. The next time you run make, it will restart the build process for
the compiler you deleted. Once these 5 (or 4) compilers are built, the Makefile will limit
itself to measure the speed of 4 configurations and report them to you.

## Out of memory

If your system freezes, you may have ran out of memory when doing the expensive
full LTO step for clang when building benchmarks/clangpgo. Edit the Makefile
in Step 6 and change make install -j $(NUMCORES) to a lower number (remove
$(NUMCORES) and use the number of threads you believe your system will
handle).

## Downloading sources

If your machine uses a proxy and you run into trouble with the default Makefile
rules to download sources, it is easier to download the sources yourself and
put them into the designated folders, so make can proceed to the build steps by
using your manually downloaded sources. These are the source folders used:

```
> benchmarks/llvm     # llvm repo with Clang, LLD and compiler-rt (check Step 1)
> benchmarks/gcc      # gcc sources after running ./contrib/download_prerequisites
>                     # (check step 9)
> src/llvm            # llvm repo with BOLT (check step 7)
```

If you wish to run the Makefile steps organized in separate download, build and
experimental phases, you can use special rules to do so. This can be
useful if you need to separately download all source files in a machine that has internet
connection, then transfer the files to a builder machine with restricted connection
where you will resume the build and experimental steps there. These special rules are:

```
> make download_sources
> make build_all
> make results
```

## makeinfo failures

When building gcc, makeinfo failures can happen if the last modified dates of source files
are inconsistent. In this case, you will see a "missing makeinfo" failure in
a gcc build. Notice that this message may be present in logs, but it is not
a fatal error. It is only fatal if make thinks it needs to update the
documentation files. This may happen if you manually copied the gcc source files
without preserving their original file dates, causing make to conclude it needs
to regenerate tex files. To avoid this, always copy gcc sources by
packing them first with a tool such as tar/gzip.

# Results on different Intel microarchitectures

Our 2-node Ivy Bridge test machine (Xeon E5-2680 v2) @ 2.8GHz, 40 logical cores
with 32GB RAM finished the GCC evaluation in 3 hours and 55 minutes. GCC
results were the following:

```
> gccpgo is 16.65% faster than baseline
> gccbolt is 25.04% faster than baseline
> gccpgobolt is 28.40% faster than baseline
```

The Clang evaluation took 3 hours and 45 minutes. Clang results were the
following:

```
> clangpgo is 36.72% faster than baseline
> clangbolt is 40.74% faster than baseline
> clangpgobolt is 61.96% faster than baseline
```

Our Broadwell test machine (Xeon E5-2680 v4) @ 2.4GHz, 56 logical cores with
256GB RAM finished the clang evaluation in 2 hours and 9 minutes. Clang
results were the following:

```
> clangpgo is 34.65% faster than baseline
> clangbolt is 30.72% faster than baseline
> clangpgobolt is 50.55% faster than baseline
```

The GCC evaluation took 2 hours and 57 minutes on our Broadwell test machine
and the results are the following:
```
> gccpgo is 10.57% faster than baseline
> gccbolt is 11.57% faster than baseline
> gccpgobolt is 14.44% faster than baseline
```
