FROM ubuntu:18.04 AS builder

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && \
    apt-get install -y --no-install-recommends ca-certificates git \
      build-essential linux-tools-generic cmake ninja-build vim bc \
      g++-multilib time flex bison file curl python libjemalloc-dev

WORKDIR /home/bolt
COPY Makefile .
RUN make $(pwd)/gcc
RUN make $(pwd)/clang
RUN make build_bolt

FROM ubuntu:18.04
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && \
    apt-get install -y --no-install-recommends ca-certificates git \
      build-essential cmake ninja-build vim bc time file curl python \
      libjemalloc-dev

WORKDIR /home/bolt
COPY --from=builder /home/bolt/clang /home/bolt/clang
COPY --from=builder /home/bolt/gcc /home/bolt/gcc
COPY --from=builder /home/bolt/src/bolt/install/bin/llvm-bolt \
                    /home/bolt/src/bolt/install/bin/llvm-bolt
COPY Makefile .
COPY Makefile.exp2 .
COPY exp1.sh .
COPY exp2.sh .
COPY inputs.tar.bz2 .
RUN tar xjvf inputs.tar.bz2

