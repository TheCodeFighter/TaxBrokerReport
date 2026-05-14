# Stage 1: Development Environment
FROM alpine:3.19 AS dev

RUN apk update && apk add --no-cache \
    bash \
    git \
    clang \
    clang-extra-tools \
    compiler-rt \
    libc++-dev \
    cmake \
    ninja \
    gdb \
    lldb \
    valgrind \
    linux-headers \
    make \
    perl

# Build and install lcov locally since it's not packaged directly in Alpine 3.19
RUN git clone https://github.com/linux-test-project/lcov.git -b v1.16 /tmp/lcov && \
    cd /tmp/lcov && \
    make install && \
    rm -rf /tmp/lcov

WORKDIR /workspace

CMD ["tail", "-f", "/dev/null"]

# Stage 2: Builder
FROM dev AS builder
COPY . /workspace

RUN CC=clang CXX=clang++ cmake -B build -G Ninja -S . -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build -j$(nproc)

# Stage 3: Production
FROM alpine:3.19 AS prod
# Install runtime libraries
RUN apk update && apk add --no-cache libc++ libstdc++

WORKDIR /app
COPY --from=builder /workspace/build/src/taxbroker_server /app/taxbroker_server

EXPOSE 8080
CMD ["./taxbroker_server"]
