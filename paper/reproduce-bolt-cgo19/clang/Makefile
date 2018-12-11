# Makefile recipes to reproduce the open-source results reported in
# "BOLT: A Practical Binary Optimizer for Data Centers and Beyond"
# CGO 2019
#
# The open-source workload evaluated on this paper is clang 7. Our goal is
# to demonstrate that building clang with all optimizations, including LTO and
# PGO, still leaves opportunities for a post-link optimizer such as BOLT to do
# a better job at basic block placement and function reordering, significantly
# improving workload performance.
#
# Technical aspects:
#
# You will probably need a machine with at least 64GB RAM. The lower your core
# count, the slower it will be, as this is building a large code base several
# times, which benefits with a higher core count.
#
# These rules are based on the steps described at
#  https://github.com/facebookincubator/BOLT/blob/master/docs/OptimizingClang.md
#
# It is important to adjust NUMCORES to the number of cores in your system as
# it *will* affect results.
#
# Note: This is a regular Makefile. If you want to re-do a step, simply delete
# the rule target or touch one of its prerequisites to be more updated than the
# target.

NUMCORES       := 40
# Smaller if your system doesn't have enough memory to handle several LTO builds
# in parallel
NUMCORESLTO    := 4
TOPLEV         := $(shell pwd)
SOURCES        := $(TOPLEV)/src
BOLTSOURCE     := $(SOURCES)/llvm
BOLT           := $(SOURCES)/install/bin/llvm-bolt
PERF2BOLT      := $(SOURCES)/install/bin/perf2bolt
BENCHMARKS     := $(TOPLEV)/benchmarks
CLANGSOURCE    := $(BENCHMARKS)/llvm
GCCSOURCE      := $(BENCHMARKS)/gcc
CLANGSTAGE1    := $(BENCHMARKS)/stage1/install/bin/clang
CLANGSTAGE2    := $(BENCHMARKS)/stage2/install/bin/clang
PGOPROFILE     := $(BENCHMARKS)/stage2/clang.profdata
CLANGPGO       := $(BENCHMARKS)/clangpgo/install/bin/clang
RAWDATA        := $(BENCHMARKS)/stage2/perf.data
BOLTDATA       := $(BENCHMARKS)/stage2/bolt.fdata
BOLTLOG        := $(TOPLEV)/bolt.log
MEASUREMENTS   := $(TOPLEV)/measurements
COMPARISON     := $(TOPLEV)/comparison.txt
RESULTS        := $(TOPLEV)/results.txt
LOG_TRAIN      := $(TOPLEV)/output_training.txt
USE_NINJA      := true
NUM_EXP        := 3
EXPERIMENTS    := $(shell seq 1 $(NUM_EXP))

# ============================= PERF OPTIONS ===================================

# Measure task-clock, cycles, instructions, branches, branch misses
PERFCOUNTERS   :=

# Measure dcache, dTLB-load-misses
#PERFCOUNTERS   := -e instructions,L1-dcache-load-misses,dTLB-load-misses

# Measure icache, iTLB-load-misses
#PERFCOUNTERS   := -e instructions,L1-icache-load-misses,iTLB-load-misses

# Measure LLC
#PERFCOUNTERS   := -e cycles,instructions,LLC-load-misses

# ============================= BOLT OPTIONS ===================================

# Full options
BOLTOPTS      := -reorder-blocks=cache+ -reorder-functions=hfsort+ \
-split-functions=3 -split-all-cold -dyno-stats -icf=1 -use-gnu-stack

# Evaluating just BB reorder
#BOLTOPTS      := -reorder-blocks=cache+ -dyno-stats -use-gnu-stack

# Evaluating BB reorder and ICF
#BOLTOPTS       := -reorder-blocks=cache+ -dyno-stats -use-gnu-stack -icf=1

ifeq (true, $(USE_NINJA))
CMAKE          := cmake -G Ninja
MAKE_CMD       := ninja
else
CMAKE          := cmake
MAKE_CMD       := make
endif

# ================================= RULES ======================================

.PHONY: all clean distclean clean_measurements

all: print_results_clangpgo print_results_clangbolt print_results_clangpgobolt

download_sources: $(CLANGSOURCE) $(GCCSOURCE) $(BOLTSOURCE)

build_all: $(BOLT) $(CLANGSTAGE1) $(CLANGPGO) \
  $(BENCHMARKS)/clangbolt/install/bin/clang \
  $(BENCHMARKS)/clangpgobolt/install/bin/clang

results: print_results_clangpgo print_results_clangbolt print_results_clangpgobolt

results_bolt: print_results_clangbolt print_results_clangpgobolt

# Step 1: Download clang sources
$(CLANGSOURCE):
	mkdir -p $(BENCHMARKS)
	cd $(BENCHMARKS)               && git clone -q --depth=1 --branch=release_70 \
	  https://git.llvm.org/git/llvm.git/ llvm
	cd $(BENCHMARKS)/llvm/tools    && git clone -q --depth=1 --branch=release_70 \
	  https://git.llvm.org/git/clang.git/
	cd $(BENCHMARKS)/llvm/projects && git clone -q --depth=1 --branch=release_70 \
	  https://git.llvm.org/git/lld.git/
	cd $(BENCHMARKS)/llvm/projects && git clone -q --depth=1 --branch=release_70 \
	  https://git.llvm.org/git/compiler-rt.git/

# Step 2: Building stage1 clang compiler so we use the same compiler used in the
# paper. Our goal is to improve our workload on top of this compiler.
$(CLANGSTAGE1): $(BENCHMARKS)/llvm
	mkdir -p $(BENCHMARKS)/stage1
	export LDFLAGS="-Wl,-q,-znow"  && cd $(BENCHMARKS)/stage1 && $(CMAKE) \
	  $(CLANGSOURCE) -DLLVM_TARGETS_TO_BUILD=X86 -DCMAKE_BUILD_TYPE=Release \
	  -DLLVM_ENABLE_ASSERTIONS=OFF \
	  -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_ASM_COMPILER=gcc \
	  -DCMAKE_INSTALL_PREFIX=$(BENCHMARKS)/stage1/install \
	  -DENABLE_LINKER_BUILD_ID=ON
	cd $(BENCHMARKS)/stage1 && $(MAKE_CMD) install -j $(NUMCORES)

# Step 3: Building stage2 clang with instrumentation capability. This is our
# workload (clang itself). We have to enable instrumentation in order to collect
# profile data for it, which will enable us to build a faster version of it
# named clangpgo.
$(CLANGSTAGE2): $(CLANGSTAGE1)
	mkdir -p $(BENCHMARKS)/stage2
	cd $(BENCHMARKS)/stage2 && $(CMAKE) $(CLANGSOURCE) \
	  -DLLVM_TARGETS_TO_BUILD=X86 -DCMAKE_BUILD_TYPE=Release \
	  -DLLVM_ENABLE_ASSERTIONS=OFF \
	  -DCMAKE_C_COMPILER=$(CLANGSTAGE1) \
	  -DCMAKE_CXX_COMPILER=$(CLANGSTAGE1)++ \
	  -DLLVM_USE_LINKER=lld -DLLVM_BUILD_INSTRUMENTED=ON \
	  -DCMAKE_INSTALL_PREFIX=$(BENCHMARKS)/stage2/install
	cd $(BENCHMARKS)/stage2 && $(MAKE_CMD) install -j $(NUMCORES)

# Step 4: Collect profile data for our workload. Remember our workload is clang,
# and since it is a compiler, we have to build something to collect profile. We
# build clang itself again for this.
$(BENCHMARKS)/stage2/profiles: $(CLANGSTAGE2)
	mkdir -p $(BENCHMARKS)/train
	cd $(BENCHMARKS)/train && $(CMAKE) $(CLANGSOURCE) \
	  -DLLVM_TARGETS_TO_BUILD=X86 -DCMAKE_BUILD_TYPE=Release \
	  -DCMAKE_C_COMPILER=$(CLANGSTAGE2) \
	  -DCMAKE_CXX_COMPILER=$(CLANGSTAGE2)++ \
	  -DLLVM_USE_LINKER=lld \
	  -DCMAKE_INSTALL_PREFIX=$(BENCHMARKS)/train/install
	cd $(BENCHMARKS)/train && $(MAKE_CMD) clang -j $(NUMCORES)

# Step 5: Merge profiles. Intermediate step to generate the PGO data to build
# a faster workload (clang + lto + pgo).
$(PGOPROFILE): $(BENCHMARKS)/stage2/profiles
	cd $(BENCHMARKS)/stage2/profiles && \
	  $(BENCHMARKS)/stage1/install/bin/llvm-profdata merge \
	  -output=$(PGOPROFILE) *.profraw

# Step 6: Build the fastest version of our open-source workload: PGO- and LTO-
# enabled. We will show that BOLT can further speedup this binary (which is
# clang the compiler driver and C++ frontend).
$(CLANGPGO): $(PGOPROFILE)
	mkdir -p $(BENCHMARKS)/clangpgo
	export LDFLAGS="-Wl,-q,-znow" && cd $(BENCHMARKS)/clangpgo && $(CMAKE) \
	  $(CLANGSOURCE) \
	  -DLLVM_TARGETS_TO_BUILD=X86 -DCMAKE_BUILD_TYPE=Release \
	  -DCMAKE_C_COMPILER=$(CLANGSTAGE1) \
	  -DCMAKE_CXX_COMPILER=$(CLANGSTAGE1)++ \
	  -DLLVM_ENABLE_ASSERTIONS=OFF \
	  -DLLVM_USE_LINKER=lld \
	  -DLLVM_ENABLE_LTO=Full \
	  -DENABLE_LINKER_BUILD_ID=ON \
	  -DLLVM_PROFDATA_FILE=$< \
	  -DCMAKE_INSTALL_PREFIX=$(BENCHMARKS)/clangpgo/install
	cd $(BENCHMARKS)/clangpgo && $(MAKE_CMD) clang -j $(NUMCORES)
	cd $(BENCHMARKS)/clangpgo && $(MAKE_CMD) install -j $(NUMCORESLTO)

# Step 7: Download the open-source BOLT tool (which is being evaluated here)
# This is using BOLT rev dd94222, which was tested during this artifact
# submission. Feel free to use master.
$(BOLTSOURCE):
	mkdir -p $(SOURCES)
	cd $(SOURCES)            && git clone https://github.com/llvm-mirror/llvm \
	  llvm -q --single-branch
	cd $(SOURCES)/llvm/tools && git checkout -b llvm-bolt \
	  f137ed238db11440f03083b1c88b7ffc0f4af65e
	cd $(SOURCES)/llvm/tools && git clone \
	  https://github.com/facebookincubator/BOLT llvm-bolt
	cd $(SOURCES)/llvm/tools/llvm-bolt && git checkout \
	  dd94222dabf6f8942c0fb6eb122bbfa60569dd5e
	cd $(SOURCES)/llvm && patch -p1 < tools/llvm-bolt/llvm.patch

# Step 8: Build BOLT
$(BOLT): $(BOLTSOURCE)
	mkdir -p $(SOURCES)/build
	cd $(SOURCES)/build && cmake $(BOLTSOURCE) \
	  -DLLVM_TARGETS_TO_BUILD="X86;AArch64" \
	  -DCMAKE_BUILD_TYPE=Release \
	  -DCMAKE_INSTALL_PREFIX=$(SOURCES)/install
	cd $(SOURCES)/build && make install -j $(NUMCORES)

# Step 9: Download GCC sources. The profile collected during a GCC build will be
# used as our training data for BOLT when optimizing clang.
# We use a different project to build so our training set is different than our
# evaluation set.
$(GCCSOURCE):
	mkdir -p $(BENCHMARKS)
	cd $(BENCHMARKS)    && git clone -q --depth=1 --branch=gcc-8_2_0-release \
	  https://github.com/gcc-mirror/gcc gcc
	cd $(BENCHMARKS)/gcc && ./contrib/download_prerequisites

# Step 10: Create new clang installations with clang binaries processed by BOLT.
# We have two bolted versions of clang: stage1+bolt and pgo+bolt, and we
# evaluate the effect of bolt on both.
$(BENCHMARKS)/clangbolt: $(CLANGSTAGE1)
	mkdir -p $(BENCHMARKS)/clangbolt
	cd $(BENCHMARKS)/clangbolt && cp -r $(BENCHMARKS)/stage1/install .

$(BENCHMARKS)/clangpgobolt: $(CLANGPGO)
	mkdir -p $(BENCHMARKS)/clangpgobolt
	cd $(BENCHMARKS)/clangpgobolt && cp -r $(BENCHMARKS)/clangpgo/install .

# Step 11: Collect BOLT data for a clang installation (when building gcc)
# BOLT data is collected with Linux perf.
$(RAWDATA).clangbolt $(RAWDATA).clangpgobolt: \
$(RAWDATA).%: $(BENCHMARKS)/% $(BOLT) $(GCCSOURCE)
	-rm -rf $(BENCHMARKS)/train
	mkdir -p $(BENCHMARKS)/train
	cd $(BENCHMARKS)/train && CC=$(<)/install/bin/clang \
	  CXX=$(<)/install/bin/clang++ \
	  $(GCCSOURCE)/configure --disable-bootstrap \
	  --enable-linker-build-id --enable-languages=c,c++ \
	  --with-gnu-as --with-gnu-ld --disable-multilib
	cd $(BENCHMARKS)/train && perf record -e cycles:u -j any,u -o $@ \
	  -- make maybe-all-gcc -j $(NUMCORES) &> $(LOG_TRAIN).$*

# Step 12: Aggregate data. This is a data conversion step, reading perf.data
# generated by Linux perf and creating the profile file used by BOLT. This needs
# to read every sample recorded at each hardware performance counter event, read
# the LBR for this event (16 branches or 32 addresses) and convert them to
# aggregated edge counts.
$(BOLTDATA).clangbolt $(BOLTDATA).clangpgobolt: \
$(BOLTDATA).%: $(RAWDATA).% $(PERF2BOLT)
	cd $(BENCHMARKS)/stage2 && \
	  $(PERF2BOLT) $(BENCHMARKS)/$(*)/install/bin/clang-7 -p $< -o $@ -w $@.yaml \
	  |& tee $(BOLTLOG).$*

# Step 13: Run BOLT now that we have both inputs: the profile data collected by
# perf and the input binary (clang). BOLT should provide a log of the work it
# did and output a faster binary (faster clang, for this case).
$(BENCHMARKS)/clangbolt/install/bin/clang \
$(BENCHMARKS)/clangpgobolt/install/bin/clang:\
$(BENCHMARKS)/%bolt/install/bin/clang: $(BOLTDATA).%bolt
	$(BOLT) $(@)-7 -o $(@)-7.bolt -b $(<).yaml $(BOLTOPTS) |& \
	  tee -a $(BOLTLOG).$(*)bolt
	cp $(@)-7.bolt $(@)-7

# Step 14: Measure compile time to build a large project (clang itself)
# to evaluate a compiler performance.
$(MEASUREMENTS).clangbolt $(MEASUREMENTS).stage1 $(MEASUREMENTS).clangpgo \
$(MEASUREMENTS).clangpgobolt: \
$(MEASUREMENTS).%: $(BENCHMARKS)/%/install/bin/clang
	for number in $(EXPERIMENTS); do \
	  mkdir -p ${@}.work ; \
	  echo Measuring trial number $${number} for $* ; \
	  cd ${@}.work && $(CMAKE) $(CLANGSOURCE) \
	    -DLLVM_TARGETS_TO_BUILD=X86 -DCMAKE_BUILD_TYPE=Release \
	    -DCMAKE_C_COMPILER=${^} -DCMAKE_CXX_COMPILER=${^}++ \
	    -DLLVM_USE_LINKER=lld -DCMAKE_INSTALL_PREFIX=$(BENCHMARKS)/eval/install \
	    &> ${@}.log.$${number}; \
	  perf stat $(PERFCOUNTERS) -x , -o ${@}.exp.$${number} -- \
	    $(MAKE_CMD) clang -j $(NUMCORES) &>> ${@}.log.$${number} ;\
		rm -rf ${@}.work ;\
	done
	cat ${@}.exp.* &> ${@}

# Step 15: Aggregate comparison results in a single file
$(TOPLEV)/clangpgo.txt: $(MEASUREMENTS).stage1 $(MEASUREMENTS).clangpgo
	cat $^ &> $@

$(TOPLEV)/clangbolt.txt: $(MEASUREMENTS).stage1 $(MEASUREMENTS).clangbolt
	cat $^ &> $@

$(TOPLEV)/clangpgobolt.txt: $(MEASUREMENTS).stage1 $(MEASUREMENTS).clangpgobolt
	cat $^ &> $@

AWK_SCRIPT := '                                                               \
	BEGIN                                                                       \
	{                                                                           \
	  sum = 0;                                                                  \
	  sumsq = 0;                                                                \
	};                                                                          \
	{                                                                           \
    sum += $$1;                                                               \
    sumsq += ($$1)^2;                                                         \
	  printf "Data point %s: %f\n", NR, $$1                                     \
  }                                                                           \
  END                                                                         \
	{                                                                           \
	  printf "Mean: %f StdDev: %f\n", sum/NR, sqrt((sumsq - sum^2/NR)/(NR-1))   \
	};  \
'

# Step 16: Compare and print results
print_results_clangpgo print_results_clangbolt print_results_clangpgobolt: \
print_results_%: $(TOPLEV)/%.txt
	echo "SIDE A:"
	cat $< | grep task-clock | head -n $(NUM_EXP) | awk -F',' \
	  $(AWK_SCRIPT) |& tee $(COMPARISON).a
	echo "SIDE B:"
	cat $< | grep task-clock | tail -n $(NUM_EXP) | awk -F',' \
	  $(AWK_SCRIPT) |& tee $(COMPARISON).b
	ASIDE=`cat $(COMPARISON).a | tail -n 1 | awk '{print $$2}'` \
	  BSIDE=`cat $(COMPARISON).b | tail -n 1 | awk '{print $$2}'` \
	  sh <<< 'COMP=$$(echo "scale=4;($$ASIDE / $$BSIDE - 1) * 100" | bc); \
	          echo -ne "\n\n $* is $${COMP}% faster than \
	          baseline, average of $(NUM_EXP) experiments\n\n"' |& \
	  tee -a $(RESULTS)

# Cleaning steps
# clean deletes final results, so experiments can be restarted
#   without rebuilding everything
# distclean further removes benchmarks and BOLT sources
clean:
	-rm -rf $(MEASUREMENTS).* $(COMPARISON).* $(RESULTS) $(TOPLEV)/clangpgo.txt \
	  $(TOPLEV)/clangbolt.txt $(TOPLEV)/clangpgobolt.txt $(RESULTS)

clean_measurements: clean

clean_bolted_builds:
	-rm -rf $(BENCHMARKS)/clangbolt $(BENCHMARKS)/clangpgobolt

distclean: clean
	-rm -rf $(BENCHMARKS) $(SOURCES) $(BOLTLOG).* $(LOG_TRAIN).*
