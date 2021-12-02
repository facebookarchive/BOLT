FROM ubuntu:20.04 AS builder

ARG DEBIAN_FRONTEND=noninteractive
ENV TZ=UTC

RUN apt-get update && \
    apt-get install -y --no-install-recommends ca-certificates git \
      build-essential cmake ninja-build python3 libjemalloc-dev \
      python3-psutil && \
    rm -rf /var/lib/apt/lists

WORKDIR /home/bolt

COPY . llvm-bolt

RUN git clone https://github.com/rafaelauler/bolt-tests bolt-tests

WORKDIR build

RUN cmake -G Ninja ../llvm-bolt/llvm \
      -DLLVM_ENABLE_PROJECTS="bolt;clang;lld" \
      -DLLVM_TARGETS_TO_BUILD="X86;AArch64" \
      -DCMAKE_BUILD_TYPE=Release \
      -DLLVM_ENABLE_ASSERTIONS=ON \
      -DCMAKE_EXE_LINKER_FLAGS="-Wl,--push-state -Wl,-whole-archive -ljemalloc_pic -Wl,--pop-state -lpthread -lstdc++ -lm -ldl" \
      -DCMAKE_INSTALL_PREFIX=/home/bolt/install \
      -DLLVM_EXTERNAL_PROJECTS="bolttests" \
      -DLLVM_EXTERNAL_BOLTTESTS_SOURCE_DIR=$(pwd)/../bolt-tests

RUN ninja check-bolt check-large-bolt && \
    ninja install-llvm-bolt install-perf2bolt install-merge-fdata \
      install-llvm-boltdiff install-bolt_rt

FROM ubuntu:20.04

COPY --from=builder /home/bolt/install /usr/local
