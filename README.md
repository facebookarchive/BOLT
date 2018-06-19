# BOLT

BOLT is a post-link optimizer developed to speed up large applications.
It achieves the improvements by optimizing application's code layout based on
execution profile gathered by sampling profiler, such as Linux `perf` tool.
BOLT can operate on any binary with a symbol table, but for maximum gains
it utilizes relocations saved by a linker (`--emit-relocs`).

NOTE: current support is limited to non-PIC/non-PIE X86-64 and AArch64 ELF
binaries.

## INSTALLATION

BOLT heavily uses LLVM libraries and by design it is built as one of LLVM
tools. The build process in not much different from a regular LLVM build.
The following instructions are assuming that you are running under Linux.

Start with cloning LLVM and BOLT repos:

```
> git clone https://github.com/llvm-mirror/llvm llvm
> cd llvm/tools
> git checkout -b llvm-bolt f137ed238db11440f03083b1c88b7ffc0f4af65e
> git clone https://github.com/facebookincubator/BOLT llvm-bolt
> cd ..
> patch -p 1 < tools/llvm-bolt/llvm.patch
```

Proceed to a normal LLVM build:

```
> cd ../..
> mkdir build
> cd build
> cmake -G Ninja ..
> ninja
```

`llvm-bolt` will be available under `bin/`. Add this directory to your path to
ensure the rest of the commands in this tutorial work.

Note that we use a specific revision of LLVM as we currently rely on a set of
patches that are not yet upstreamed.

## USAGE

### Step 0

In order to allow BOLT to re-arrange functions (in addition to re-arranging
code within functions) in your program, it needs a little help from the linker.
Add `--emit-relocs` to the final link step of your application. You can verify
the presence of relocations by checking for `.rela.text` section in the binary.
BOLT will also report if it detects relocations while processing the binary.

### Step 1: Collect Profile

This step is different for different kinds of executables. If you can invoke
your program to run on a representative input from a command line, then check
**For Applications** section below. If your programs typically runs as a
server/service, then skip to **For Services** section.

#### For Applications

This assumes you can run your program from a command line with a typical input.
In this case, simply prepend the command line invocation with `perf`:
```
$ perf record -e cycles:u -j any,u -o perf.data -- <executable> <args> ...
```

#### For Services

Once you get the service deployed and warmed-up, it is time to collect perf
data with LBR (branch information). The exact perf command to use will depend
on the service. E.g. to collect the data for all processes running on the
server for the next 3 minutes use:
```
$ perf record -e cycles:u -j any,u -a -o perf.data -- sleep 180
```

Depending on the application, you may need more samples to be included with
your profile. It's hard to tell upfront what would be a sweet spot for your
application. We recommend the profile to cover 1B instructions as reported
by BOLT `-dyno-stats` option. If you need to increase the number of samples
in the profile, you can either run the `sleep` command for longer, and/or use
`-F<N>` option with `perf` to increase sampling frequency.

Note that for profile collection we recommend using cycle events and not
`BR_INST_RETIRED.*`. Empirically we found it to produce better results.

### Step 2: Convert Profile to BOLT Format

NOTE: you can skip this step and feed `perf.data` directly to BOLT using
experimental `-p perf.data` option.

For this step you will need `perf.data` file collected from the previous step and
a copy of the binary that was running. The binary has to be either
unstripped, or should have a symbol table intact (i.e. running `strip -g` is
okay).

Execute `perf2bolt`:
```
$ perf2bolt -p perf.data -o perf.fdata <executable>
```

This command will aggregate branch data from `perf.data` and store it in a
format that is both more compact and more resilient to binary modifications.

### Step 3: Optimize with BOLT

Once you have `perf.fdata` ready, you can use it for optimizations with
BOLT. Assuming your environment is setup to include the right path, execute
`llvm-bolt`:
```
$ llvm-bolt <executable> -o <executable>.bolt -data=perf.fdata -reorder-blocks=cache+ -reorder-functions=hfsort+ -split-functions=3 -split-all-cold -split-eh -dyno-stats
```

If you do need an updated debug info, then add `-update-debug-sections` option
to the command above. The processing time will be slightly longer.

For a full list of options see `-help`/`-help-hidden` output.

The input binary for this step does not have to 100% match the binary used for
profile collection in **Step 1**. This could happen when you are doing an active
development, and the source code constantly changes, yet you want to benefit
from profile-guided optimizations. However, since the binary is not exactly the
same, the profile information could become invalid or stale, and BOLT will
report the number of functions with stale profile. The higher the
number, the less performance improvement should be expected. Thus, it is
important to update `.fdata` for important releases.

## Multiple Profiles

Suppose your application can run in different modes, and you can generate
multiple profiles for each one of them. To generate a single binary that can
benefit all modes (assuming the profiles don't contradict each other) you can
use `merge-fdata` tool:
```
$ merge-fdata *.fdata > combined.fdata
```
Use `combined.fdata` for **Step 3** above to generate a universally optimized
binary.
