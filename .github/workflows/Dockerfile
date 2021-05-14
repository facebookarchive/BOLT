FROM ubuntu:20.04 AS builder

ARG DEBIAN_FRONTEND=noninteractive
ENV TZ=UTC

RUN apt-get update && \
    apt-get install -y --no-install-recommends ca-certificates git \
      build-essential cmake ninja-build python3 libjemalloc-dev && \
    rm -rf /var/lib/apt/lists

WORKDIR /home/bolt

COPY . llvm-bolt

WORKDIR build

RUN cmake -G Ninja ../llvm-bolt/llvm \
      -DLLVM_ENABLE_PROJECTS="bolt" \
      -DLLVM_TARGETS_TO_BUILD="X86;AArch64" \
      -DCMAKE_BUILD_TYPE=Release \
      -DLLVM_ENABLE_ASSERTIONS=ON \
      -DCMAKE_EXE_LINKER_FLAGS="-Wl,--push-state -Wl,-whole-archive -ljemalloc_pic -Wl,--pop-state -lpthread -lstdc++ -lm -ldl" \
      -DCMAKE_INSTALL_PREFIX=/home/bolt/install

RUN ninja check-bolt && \
    ninja install-llvm-bolt install-perf2bolt install-merge-fdata \
      install-llvm-boltdiff install-bolt_rt

FROM ubuntu:20.04

COPY --from=builder /home/bolt/install /usr/local
