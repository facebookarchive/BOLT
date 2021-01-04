# Makefile recipes to reproduce BOLT results reported in CC 2021
#
#
# Note: This is a regular Makefile. If you want to re-do a step, simply delete
# the rule target or touch one of its prerequisites to be more updated than the
# target.

SHELL          := /bin/bash

TOPLEV         := $(shell pwd)
INPUTBIN       := $(TOPLEV)/clang

NUMCORES       := 8
SOURCES        := $(TOPLEV)/src
BOLTSOURCE     := $(SOURCES)/bolt/llvm-project/llvm
CLANGSOURCE    := $(SOURCES)/llvm-project/llvm
GCCSOURCE      := $(SOURCES)/gcc
CLANGSTAGE1    := $(SOURCES)/stage1/install/bin/clang
INPUTCLANG     := $(TOPLEV)/clang
INPUTGCC       := $(TOPLEV)/gcc
BOLT           := $(SOURCES)/bolt/install/bin/llvm-bolt
PERF2BOLT      := $(SOURCES)/bolt/install/bin/perf2bolt
MEASUREMENTS   := $(TOPLEV)/measurements
COMPARISON     := $(TOPLEV)/comparison.txt
RESULTS        := $(TOPLEV)/results.txt
USE_NINJA      := true
NUM_EXP        := 5
EXPERIMENTS    := $(shell seq 1 $(NUM_EXP))
FACEBOOK       := false

# ============================= TIMER OPTIONS ==================================

# Measure wall time, use time, and maximum resident set size of the process
# during its lifetime, in Kbytes
TIMER          := /usr/bin/time -f '%e,wall\n%M,mem\n%U,user'

# ============================= BOLT OPTIONS ===================================

# Default base Sequential+NoLite options
BOLTOPTSSEQ    := -reorder-blocks=cache+ -reorder-functions=hfsort \
-split-functions=3 -split-all-cold -dyno-stats -icf=1 -use-gnu-stack \
-split-eh -frame-opt=hot -eliminate-unreachable=0 \
-no-threads -lite=0 -time-opts -time-rewrite -time-build

# Default comparison: Threads+Lite options
BOLTOPTS    := -reorder-blocks=cache+ -reorder-functions=hfsort \
-split-functions=3 -split-all-cold -dyno-stats -icf=1 -use-gnu-stack \
-split-eh -frame-opt=hot -eliminate-unreachable=0 \
-lite=1 -time-opts -time-rewrite -time-build

ifeq (true, $(USE_NINJA))
CMAKE          := cmake -G Ninja
MAKE_CMD       := ninja
else
CMAKE          := cmake
MAKE_CMD       := make
endif

ifeq (true, $(FACEBOOK))
PATCH_GCC := true
JEMALLOC  := /usr/lib/x86_64-linux-gnu/libjemalloc.so
else
PATCH_GCC := true
JEMALLOC  := /usr/lib/x86_64-linux-gnu/libjemalloc.so
endif

# ================================= RULES ======================================

.PHONY: all clean clean_measurements clean_side_b

all: print_results

# Use this rule to build input binaries and their profile (fdata) used when
# evaluating BOLT
inputs: $(INPUTCLANG) $(INPUTGCC) $(INPUTCLANG).fdata $(INPUTGCC).fdata

build_bolt: $(BOLT)

build: inputs build_bolt

# Step 1: Download clang/gcc sources
$(CLANGSOURCE):
	mkdir -p $(SOURCES)
	cd $(SOURCES)               && git clone -q --depth=1 \
	  --branch=release/11.x https://github.com/llvm/llvm-project.git llvm-project

$(GCCSOURCE):
	mkdir -p $(SOURCES)
	cd $(SOURCES)    && git clone -q --depth=1 --branch=releases/gcc-10 \
	  https://github.com/gcc-mirror/gcc gcc
	cd $(SOURCES)/gcc && $(PATCH_GCC)
	cd $(SOURCES)/gcc && ./contrib/download_prerequisites

# Step 2: Building stage1 clang compiler so we use the same compiler used in the
# paper. This will be used to build BOLT and also clang (again, bootstrapped).
$(CLANGSTAGE1): $(CLANGSOURCE)
	mkdir -p $(SOURCES)/stage1
	export LDFLAGS="-Wl,-q,-znow"  && cd $(SOURCES)/stage1 && $(CMAKE) \
	  $(CLANGSOURCE) -DLLVM_TARGETS_TO_BUILD=X86 -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_ENABLE_ASSERTIONS=OFF \
    -DLLVM_ENABLE_PROJECTS="clang;compiler-rt;lld" \
    -DCMAKE_INSTALL_PREFIX=$(SOURCES)/stage1/install \
    -DENABLE_LINKER_BUILD_ID=ON
	cd $(SOURCES)/stage1 && $(MAKE_CMD) install -j $(NUMCORES)

#                         ~  ~  ~   Build Inputs  ~  ~  ~
# 4 run-once steps - Building clang and gcc used as inpust to run BOLT to
# optimize them.
# Create BOLT profile data  (fdata file) for each one of them by using them
# to build clang. These 4 rules will take a while since they involve building
# full compilers multiple times, but they will run only once.
$(INPUTCLANG): $(CLANGSTAGE1)
	mkdir -p $(SOURCES)/inputclang
	export LDFLAGS="-Wl,-q,-znow" && cd $(SOURCES)/inputclang && $(CMAKE) $(CLANGSOURCE) \
	  -DLLVM_TARGETS_TO_BUILD=X86 -DCMAKE_BUILD_TYPE=Release \
	  -DLLVM_ENABLE_ASSERTIONS=OFF \
    -DLLVM_ENABLE_PROJECTS="clang;compiler-rt;lld" \
	  -DCMAKE_C_COMPILER=$(CLANGSTAGE1) \
	  -DCMAKE_CXX_COMPILER=$(CLANGSTAGE1)++ \
	  -DLLVM_USE_LINKER=lld \
	  -DCMAKE_INSTALL_PREFIX=$(SOURCES)/inputclang/install
	cd $(SOURCES)/inputclang && $(MAKE_CMD) install -j $(NUMCORES) && \
    cp $(SOURCES)/inputclang/install/bin/clang-11 $(INPUTCLANG)

$(INPUTGCC): $(GCCSOURCE)
	mkdir -p $(SOURCES)/inputgcc
	cd $(SOURCES)/inputgcc && \
	  $(GCCSOURCE)/configure --enable-bootstrap \
	  --enable-linker-build-id --enable-languages=c,c++ \
	  --with-gnu-as --with-gnu-ld --disable-multilib \
	  --with-boot-ldflags='-Wl,-q,-znow -static-libstdc++ -static-libgcc' \
	  --with-stage1-ldflags='-Wl,-q,-znow' \
    --prefix=$(SOURCES)/inputgcc/install && \
     make -j $(NUMCORES) BOOT_CFLAGS='-O2 -g -fno-reorder-blocks-and-partition' && make install -j $(NUMCORES)
	cp $(SOURCES)/inputgcc/install/libexec/gcc/x86_64-pc-linux-gnu/10.2.1/cc1plus $(INPUTGCC)

$(INPUTCLANG).fdata: $(INPUTCLANG) $(BOLT)
	-rm -rf $(SOURCES)/train
	mkdir -p $(SOURCES)/train
	cd $(SOURCES)/train && $(CMAKE) $(CLANGSOURCE) \
	  -DLLVM_TARGETS_TO_BUILD=X86 -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_ENABLE_PROJECTS="clang;compiler-rt;lld" \
	  -DCMAKE_C_COMPILER=$(SOURCES)/inputclang/install/bin/clang \
	  -DCMAKE_CXX_COMPILER=$(SOURCES)/inputclang/install/bin/clang++ \
	  -DLLVM_USE_LINKER=lld \
	  -DCMAKE_INSTALL_PREFIX=$(SOURCES)/train/install
	cd $(SOURCES)/train && perf record -e cycles:u -j any,u -o clang.perf.data \
	  -- ninja &> $(SOURCES)/train.clang.log
	cd $(SOURCES)/train && ln -s $(INPUTCLANG) clang-11
	cd $(SOURCES)/train && $(PERF2BOLT) clang-11 -p clang.perf.data -o $(INPUTCLANG).fdata |& tee $(SOURCES)/clang.perf2bolt.log

$(INPUTGCC).fdata: $(INPUTGCC) $(BOLT)
	-rm -rf $(SOURCES)/train
	mkdir -p $(SOURCES)/train
	cd $(SOURCES)/train && $(CMAKE) $(CLANGSOURCE) \
	  -DLLVM_TARGETS_TO_BUILD=X86 -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_ENABLE_PROJECTS="clang" \
	  -DCMAKE_C_COMPILER=$(SOURCES)/inputgcc/install/bin/gcc \
	  -DCMAKE_CXX_COMPILER=$(SOURCES)/inputgcc/install/bin/g++ \
    -DCMAKE_C_FLAGS="-Wl,-rpath,'$(SOURCES)/inputgcc/install/lib64'" \
    -DCMAKE_CXX_FLAGS="-Wl,-rpath,'$(SOURCES)/inputgcc/install/lib64'" \
	  -DCMAKE_INSTALL_PREFIX=$(SOURCES)/train/install
	cd $(SOURCES)/train && perf record -e cycles:u -j any,u -o gcc.perf.data \
	  -- ninja &> $(SOURCES)/train.gcc.log
	cd $(SOURCES)/train && ln -s $(INPUTGCC) cc1plus
	cd $(SOURCES)/train && $(PERF2BOLT) cc1plus -p gcc.perf.data -o $(INPUTGCC).fdata |& tee $(SOURCES)/gcc.perf2bolt.log

# Step 3: Download the open-source BOLT tool (which is being evaluated here)
# This is using BOLT rev fab6ae4a, which was tested during this artifact
# submission. Feel free to use master.
$(BOLTSOURCE):
	mkdir -p $(SOURCES)/bolt
	cd $(SOURCES)/bolt         && git clone \
	  https://github.com/llvm/llvm-project.git llvm-project -q --single-branch
	cd $(SOURCES)/bolt/llvm-project && git checkout -b llvm-bolt \
	  2fa1436206177291edb2d78c84d5822bb6e58cc9
	cd $(SOURCES)/bolt/llvm-project/llvm/tools && git clone \
	  https://github.com/facebookincubator/BOLT llvm-bolt
	cd $(SOURCES)/bolt/llvm-project/llvm/tools/llvm-bolt && git checkout \
	  fab6ae4a50186cd67d6bf733b7d21e48a157033c
	cd $(SOURCES)/bolt/llvm-project/llvm && patch -p1 < tools/llvm-bolt/llvm.patch

# Step 4: Build BOLT with Clang stage1
$(BOLT): $(BOLTSOURCE) $(CLANGSTAGE1)
	mkdir -p $(SOURCES)/bolt/build
	cd $(SOURCES)/bolt/build && cmake $(BOLTSOURCE) \
	  -DLLVM_TARGETS_TO_BUILD="X86;AArch64" \
    -DCMAKE_C_COMPILER=$(CLANGSTAGE1) \
    -DCMAKE_CXX_COMPILER=$(CLANGSTAGE1)++ \
    -DCMAKE_ASM_COMPILER=$(CLANGSTAGE1) \
	  -DCMAKE_BUILD_TYPE=Release \
	  -DLLVM_ENABLE_ASSERTIONS=OFF \
	  -DCMAKE_INSTALL_PREFIX=$(SOURCES)/bolt/install
	cd $(SOURCES)/bolt/build && make install-llvm-bolt \
                              install-perf2bolt \
                              install-merge-fdata -j $(NUMCORES)


# Step 5: Measure bolt run time to process a large input
# to evaluate performance - baseline
$(MEASUREMENTS).base:
	for number in $(EXPERIMENTS); do \
	  echo Measuring trial number $${number} for base ; \
	  LD_PRELOAD=$(JEMALLOC) \
      $(TIMER) -o ${@}.exp.$${number} -- \
		  $(BOLT) $(INPUTBIN) -o $(@).exe -data $(INPUTBIN).fdata $(BOLTOPTSSEQ) \
        &>> ${@}.log.$${number} ;\
	done
	cat ${@}.exp.* &> ${@}

# Step 6: Measure bolt run time to process a large input
# to evaluate performance - comparison
$(MEASUREMENTS).test:
	for number in $(EXPERIMENTS); do \
	  echo Measuring trial number $${number} for test ; \
	  LD_PRELOAD=$(JEMALLOC) \
      $(TIMER) -o ${@}.exp.$${number} -- \
		  $(BOLT) $(INPUTBIN) -o $(@).exe -data $(INPUTBIN).fdata $(BOLTOPTS) \
        &>> ${@}.log.$${number} ;\
	done
	cat ${@}.exp.* &> ${@}

# Step 7: Aggregate comparison results in a single file
$(TOPLEV)/aggregate.txt: $(MEASUREMENTS).base $(MEASUREMENTS).test
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

# Final step: Compare and print results for both wall time and memory
print_results: $(TOPLEV)/aggregate.txt
	echo "WALL TIME SIDE A:"
	cat $< | grep wall | head -n $(NUM_EXP) | awk -F',' \
	  $(AWK_SCRIPT) |& tee $(COMPARISON).a
	echo "WALL TIME SIDE B:"
	cat $< | grep wall | tail -n $(NUM_EXP) | awk -F',' \
	  $(AWK_SCRIPT) |& tee $(COMPARISON).b
	echo "MEMORY SIDE A:"
	cat $< | grep mem | head -n $(NUM_EXP) | awk -F',' \
	  $(AWK_SCRIPT) |& tee $(COMPARISON).mem.a
	echo "MEMORY SIDE B:"
	cat $< | grep mem | tail -n $(NUM_EXP) | awk -F',' \
	  $(AWK_SCRIPT) |& tee $(COMPARISON).mem.b
	ASIDE=`cat $(COMPARISON).a | tail -n 1 | awk '{print $$2}'` \
	  BSIDE=`cat $(COMPARISON).b | tail -n 1 | awk '{print $$2}'` \
	  AERR=`cat $(COMPARISON).a | tail -n 1 | awk '{print $$4}'` \
	  BERR=`cat $(COMPARISON).b | tail -n 1 | awk '{print $$4}'` \
	  bash <<< 'COMP=$$(echo "scale=8;($$ASIDE / $$BSIDE)" | bc); \
	          COMPERR=$$(echo "scale=8;($$ASIDE / $$BSIDE) * sqrt (($$AERR/$$ASIDE)^2 + ($$BERR/$$BSIDE)^2)" | bc); \
	          echo -ne "runtime: $${COMP} % +- $${COMPERR} faster than \
	          baseline, average of $(NUM_EXP) experiments\n"' |& \
	  tee -a $(RESULTS)
	ASIDE=`cat $(COMPARISON).mem.a | tail -n 1 | awk '{print $$2}'` \
	  BSIDE=`cat $(COMPARISON).mem.b | tail -n 1 | awk '{print $$2}'` \
	  AERR=`cat $(COMPARISON).mem.a | tail -n 1 | awk '{print $$4}'` \
	  BERR=`cat $(COMPARISON).mem.b | tail -n 1 | awk '{print $$4}'` \
	  bash <<< 'COMP=$$(echo "scale=8;(1 - $$BSIDE / $$ASIDE) * 100" | bc); \
	          COMPERR=$$(echo "scale=8;100 * ($$BSIDE / $$ASIDE) * sqrt (($$AERR/$$ASIDE)^2 + ($$BERR/$$BSIDE)^2)" | bc); \
	          echo -ne "memory: $${COMP} % +- $${COMPERR} reduction in memory wrt \
	          baseline, average of $(NUM_EXP) experiments\n"' |& \
	  tee -a $(RESULTS)


# Cleaning steps
# clean deletes final results, so experiments can be restarted
#   without rebuilding everything
clean:
	-rm $(MEASUREMENTS).* $(COMPARISON).* $(RESULTS) $(TOPLEV)/aggregate.txt

clean_measurements: clean

distclean: clean
	-rm -rf $(TOPLEV)/results-clang-lite $(TOPLEV)/results-clang-all $(TOPLEV)/results-clang-threads $(TOPLEV)/results-gcc-lite $(TOPLEV)/results-gcc-all $(TOPLEV)/results-gcc-threads $(TOPLEV)/src

clean_side_b:
	-rm $(TOPLEV)/aggregate.txt $(RESULTS) $(MEASUREMENTS).test $(MEASUREMENTS).test.log.* $(MEASUREMENTS).test.exe
