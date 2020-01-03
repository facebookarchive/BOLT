FROM ubuntu:18.04 AS builder

RUN apt-get update && \
    apt-get install -y --no-install-recommends ca-certificates git \
      build-essential cmake ninja-build python libjemalloc-dev && \
    rm -rf /var/lib/apt/lists

WORKDIR /home/bolt

RUN git clone https://github.com/llvm-mirror/llvm llvm && \
    cd llvm/tools && \
    git checkout -b llvm-bolt f137ed238db11440f03083b1c88b7ffc0f4af65e && \
    git clone https://github.com/facebookincubator/BOLT llvm-bolt && \
    cd .. && \
    patch -p 1 < tools/llvm-bolt/llvm.patch

RUN mkdir build && \
    cd build && \
    cmake -G Ninja ../llvm \
      -DLLVM_TARGETS_TO_BUILD="X86;AArch64" \
      -DCMAKE_BUILD_TYPE=Release \
      -DLLVM_ENABLE_ASSERTIONS=ON \
      -DCMAKE_EXE_LINKER_FLAGS="-Wl,--push-state -Wl,-whole-archive -ljemalloc_pic -Wl,--pop-state -lpthread" \
      -DCMAKE_INSTALL_PREFIX=/home/bolt/install && \
    ninja install-llvm-bolt install-perf2bolt install-merge-fdata \
      install-llvm-boltdiff install-bolt_rt

FROM ubuntu:18.04

COPY --from=builder /home/bolt/install /usr/local
