# Makefile recipes to reproduce the open-source results reported in
# "BOLT: A Practical Binary Optimizer for Data Centers and Beyond"
# CGO 2019
#
# The open-source workload evaluated on this paper is gcc 8.2.#
# It is important to adjust NUMCORES to the number of cores in your system as
# it *will* affect results.
#
# Note: This is a regular Makefile. If you want to re-do a step, simply delete
# the rule target or touch one of its prerequisites to be more updated than the
# target.

NUMCORES       := 40
TOPLEV         := $(shell pwd)
SOURCES        := $(TOPLEV)/src
BOLTSOURCE     := $(SOURCES)/llvm
BOLT           := $(SOURCES)/install/bin/llvm-bolt
PERF2BOLT      := $(SOURCES)/install/bin/perf2bolt
BENCHMARKS     := $(TOPLEV)/benchmarks
CLANGSOURCE    := $(BENCHMARKS)/llvm
GCCSOURCE      := $(BENCHMARKS)/gcc
GCCSTAGE1      := $(BENCHMARKS)/stage1/install/bin/gcc
GCCPGO         := $(BENCHMARKS)/gccpgo/install/bin/gcc
RAWDATA        := $(BENCHMARKS)/perf.data
BOLTDATA       := $(BENCHMARKS)/bolt.fdata
BOLTLOG        := $(TOPLEV)/bolt.log
MEASUREMENTS   := $(TOPLEV)/measurements
COMPARISON     := $(TOPLEV)/comparison.txt
RESULTS        := $(TOPLEV)/results.txt
LOG_TRAIN      := $(TOPLEV)/output_training.txt
USE_NINJA      := true
NUM_EXP        := 3
EXPERIMENTS    := $(shell seq 1 $(NUM_EXP))

ifeq (true, $(USE_NINJA))
CMAKE          := cmake -G Ninja
MAKE_CMD       := ninja
else
CMAKE          := cmake
MAKE_CMD       := make
endif

.PHONY: all clean distclean clean_measurements

all: print_results_gccpgo print_results_gccbolt print_results_gccpgobolt

download_sources: $(CLANGSOURCE) $(GCCSOURCE) $(BOLTSOURCE)

build_all: $(BOLT) $(GCCSTAGE1) $(GCCPGO) \
  $(BENCHMARKS)/gccbolt/install/bin/gcc \
  $(BENCHMARKS)/gccpgobolt/install/bin/gcc

results: print_results_gccpgo print_results_gccbolt print_results_gccpgobolt

# Step 1: Download GCC sources.
$(GCCSOURCE):
	mkdir -p $(BENCHMARKS)
	cd $(BENCHMARKS)    && git clone -q --depth=1 --branch=gcc-8_2_0-release \
	  https://github.com/gcc-mirror/gcc gcc
	cd $(BENCHMARKS)/gcc && ./contrib/download_prerequisites

# STEP 2: Building baseline compiler
$(GCCSTAGE1): $(GCCSOURCE)
	mkdir -p $(BENCHMARKS)/stage1
	cd $(BENCHMARKS)/stage1 && \
	  $(GCCSOURCE)/configure --enable-bootstrap \
	  --enable-linker-build-id --enable-languages=c,c++ \
	  --with-gnu-as --with-gnu-ld --disable-multilib \
	  --prefix=$(BENCHMARKS)/stage1/install
	cd $(BENCHMARKS)/stage1 && make -j $(NUMCORES)
	cd $(BENCHMARKS)/stage1 && make install -j $(NUMCORES)

# Step 3: Building pgo gcc
$(GCCPGO): $(GCCSOURCE)
	mkdir -p $(BENCHMARKS)/gccpgo
	cd $(BENCHMARKS)/gccpgo && \
	  $(GCCSOURCE)/configure --enable-bootstrap \
	  --enable-linker-build-id --enable-languages=c,c++ \
	  --with-gnu-as --with-gnu-ld --disable-multilib \
	  --prefix=$(BENCHMARKS)/gccpgo/install
	cd $(BENCHMARKS)/gccpgo && make profiledbootstrap -j $(NUMCORES)
	cd $(BENCHMARKS)/gccpgo && make install -j $(NUMCORES)

# Step 4: Download the open-source BOLT tool (which is being evaluated here)
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

# Step 5: Build BOLT
$(BOLT): $(BOLTSOURCE)
	mkdir -p $(SOURCES)/build
	cd $(SOURCES)/build && cmake $(BOLTSOURCE) \
	  -DLLVM_TARGETS_TO_BUILD="X86;AArch64" \
	  -DCMAKE_BUILD_TYPE=Release \
	  -DCMAKE_INSTALL_PREFIX=$(SOURCES)/install
	cd $(SOURCES)/build && make install -j $(NUMCORES)

# Step 6: Download clang sources (used as our input to test gcc speed)
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

# Step 7: Create new gcc installations with gcc binaries processed by BOLT.
# We have two bolted versions of gcc: stage1+bolt and pgo+bolt, and we
# evaluate the effect of bolt on both.
# In order to be processed by BOLT, these gcc setups are built differently.
# We add the -q linker flag to add relocation metadata to binaries, and
# we also use -fno-reorder-blocks-and-partition to disable a gcc 8
# optimization that renders the binary unsupported by BOLT. This is related
# to function splitting. BOLT does function splitting by itself, but
# can't read binaries with split functions.
$(BENCHMARKS)/gccbolt: $(GCCSOURCE)
	mkdir -p $(BENCHMARKS)/gccbolt
	cd $(BENCHMARKS)/gccbolt && \
	  $(GCCSOURCE)/configure --enable-bootstrap \
	  --enable-linker-build-id --enable-languages=c,c++ \
	  --with-gnu-as --with-gnu-ld --disable-multilib \
	  --with-boot-ldflags='-Wl,-q,-znow -static-libstdc++ -static-libgcc' \
	  --with-stage1-ldflags='-Wl,-q,-znow' \
	  --prefix=$(BENCHMARKS)/gccbolt/install
	cd $(BENCHMARKS)/gccbolt && \
	  make -j $(NUMCORES) BOOT_CFLAGS='-O2 -g -fno-reorder-blocks-and-partition'
	cd $(BENCHMARKS)/gccbolt && make install -j $(NUMCORES)

$(BENCHMARKS)/gccpgobolt: $(GCCSOURCE)
	mkdir -p $(BENCHMARKS)/gccpgobolt
	cd $(BENCHMARKS)/gccpgobolt && \
	  $(GCCSOURCE)/configure --enable-bootstrap \
	  --with-boot-ldflags='-Wl,-q,-znow -static-libstdc++ -static-libgcc' \
	  --with-stage1-ldflags='-Wl,-q,-znow' \
	  --enable-linker-build-id --enable-languages=c,c++ \
	  --with-gnu-as --with-gnu-ld --disable-multilib \
	  --prefix=$(BENCHMARKS)/gccpgobolt/install
	cd $(BENCHMARKS)/gccpgobolt && make profiledbootstrap -j $(NUMCORES) \
	  BOOT_CFLAGS='-O2 -g -fno-reorder-blocks-and-partition'
	cd $(BENCHMARKS)/gccpgobolt && make install -j $(NUMCORES)

# Step 8: Collect BOLT data for a gcc installation (when building gcc itself)
# BOLT data is collected with Linux perf.
$(RAWDATA).gccbolt $(RAWDATA).gccpgobolt: \
$(RAWDATA).%: $(BENCHMARKS)/% $(BOLT) $(GCCSOURCE)
	-rm -rf $(BENCHMARKS)/train
	mkdir -p $(BENCHMARKS)/train
	cd $(BENCHMARKS)/train && CC=$(<)/install/bin/gcc \
	  CXX=$(<)/install/bin/g++ \
	  $(GCCSOURCE)/configure --disable-bootstrap \
	  --enable-languages=c,c++ --with-gnu-as --with-gnu-ld --disable-multilib
	cd $(BENCHMARKS)/train && perf record -e cycles:u -j any,u -o $@ \
	  -- make maybe-all-gcc -j $(NUMCORES) &> $(LOG_TRAIN).$*

# Step 9: Aggregate data. This is a data conversion step, reading perf.data
# generated by Linux perf and creating the profile file used by BOLT. This needs
# to read every sample recorded at each hardware performance counter event, read
# the LBR for this event (16 branches or 32 addresses) and convert them to
# aggregated edge counts.
$(BOLTDATA).gccbolt $(BOLTDATA).gccpgobolt: \
$(BOLTDATA).%: $(RAWDATA).% $(PERF2BOLT)
	cd $(BENCHMARKS) && \
	  $(PERF2BOLT) $(BENCHMARKS)/$(*)/install/libexec/gcc/x86_64-pc-linux-gnu/8.2.0/cc1plus \
	  -p $< -o $@ -w $@.yaml |& tee $(BOLTLOG).$*

# Step 10: Run BOLT now that we have both inputs: the profile data collected by
# perf and the input binary (gcc). BOLT should provide a log of the work it
# did and output a faster binary (faster gcc, for this case).
$(BENCHMARKS)/gccbolt/install/bin/gcc $(BENCHMARKS)/gccpgobolt/install/bin/gcc:\
$(BENCHMARKS)/%bolt/install/bin/gcc: $(BOLTDATA).%bolt
	$(BOLT) $(@D)/../libexec/gcc/x86_64-pc-linux-gnu/8.2.0/cc1plus \
	  -o $(@D)/../libexec/gcc/x86_64-pc-linux-gnu/8.2.0/cc1plus.bolt -b $(<).yaml \
	  -reorder-blocks=cache+ -reorder-functions=hfsort+ -split-functions=3 \
	  -split-all-cold -dyno-stats -icf=1 -use-gnu-stack |& \
	  tee -a $(BOLTLOG).$(*)bolt
	cp $(@D)/../libexec/gcc/x86_64-pc-linux-gnu/8.2.0/cc1plus.bolt \
	  $(@D)/../libexec/gcc/x86_64-pc-linux-gnu/8.2.0/cc1plus

# Step 11: Measure compile time to build a large project (clang)
# to evaluate compiler performance.
$(MEASUREMENTS).gccbolt $(MEASUREMENTS).stage1 $(MEASUREMENTS).gccpgo \
$(MEASUREMENTS).gccpgobolt: \
$(MEASUREMENTS).%: $(BENCHMARKS)/%/install/bin/gcc $(CLANGSOURCE)
	for number in $(EXPERIMENTS); do \
	  mkdir -p ${@}.work ; \
	  echo Measuring trial number $${number} for $* ; \
	  cd ${@}.work && $(CMAKE) $(CLANGSOURCE) \
	    -DLLVM_TARGETS_TO_BUILD=X86 -DCMAKE_BUILD_TYPE=Release \
	    -DCMAKE_C_COMPILER=$(<) -DCMAKE_CXX_COMPILER=$(<D)/g++ \
	    -DCMAKE_INSTALL_PREFIX=$(BENCHMARKS)/eval/install \
	    -DCMAKE_CXX_LINK_FLAGS="-Wl,-rpath,$(<D)/../lib64" \
	    &> ${@}.log.$${number}; \
	  perf stat -x , -o ${@}.exp.$${number} \
	    -- $(MAKE_CMD) clang -j $(NUMCORES) \
	    &>> ${@}.log.$${number} ;\
		rm -rf ${@}.work ;\
	done
	cat ${@}.exp.* &> ${@}

# Step 12: Aggregate comparison results in a single file
$(TOPLEV)/gccpgo.txt: $(MEASUREMENTS).stage1 $(MEASUREMENTS).gccpgo
	cat $^ &> $@

$(TOPLEV)/gccbolt.txt: $(MEASUREMENTS).stage1 $(MEASUREMENTS).gccbolt
	cat $^ &> $@

$(TOPLEV)/gccpgobolt.txt: $(MEASUREMENTS).stage1 $(MEASUREMENTS).gccpgobolt
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

# Step 13: Compare and print results;
print_results_gccpgo print_results_gccbolt print_results_gccpgobolt: \
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
	-rm -rf $(MEASUREMENTS).* $(COMPARISON).* $(RESULTS) $(TOPLEV)/gccpgo.txt \
	  $(TOPLEV)/gccbolt.txt $(TOPLEV)/gccpgobolt.txt $(RESULTS)

clean_measurements: clean

distclean: clean
	-rm -rf $(BENCHMARKS) $(SOURCES) $(BOLTLOG).* $(LOG_TRAIN).*
