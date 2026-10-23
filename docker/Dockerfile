# Stage 1: Development Environment
FROM alpine:3.24 AS dev

# libxml2-dev is only for the XSD validation tests; the production image does not get it.
RUN apk add --no-cache \
    bash \
    boost-dev \
    ca-certificates \
    git \
    clang \
    clang-extra-tools \
    compiler-rt \
    cppcheck \
    g++ \
    libc++-dev \
    libxml2-dev \
    cmake \
    ninja \
    gdb \
    lldb \
    valgrind \
    linux-headers \
    make \
    perl

# lcov 1.16 comes from its release archive, checked by SHA-256. lcov 2 (up to 2.6) rejects GCC 15
# coverage data: it expects a taken branch on every line that ran, but GCC puts exception cleanup
# paths that never run on closing braces.
RUN wget -q -O /tmp/lcov.tar.gz \
        https://github.com/linux-test-project/lcov/releases/download/v1.16/lcov-1.16.tar.gz && \
    echo "987031ad5528c8a746d4b52b380bc1bffe412de1f2b9c2ba5224995668e3240b  /tmp/lcov.tar.gz" | \
        sha256sum -c - && \
    tar -xzf /tmp/lcov.tar.gz -C /tmp && \
    make -C /tmp/lcov-1.16 install && \
    rm -rf /tmp/lcov.tar.gz /tmp/lcov-1.16

WORKDIR /workspace

CMD ["/bin/bash"]

# Stage 2: Builder
FROM dev AS builder
COPY . /workspace

RUN CC=clang CXX=clang++ cmake -B build -G Ninja -S . -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build -j$(nproc)

# Stage 3: Production
FROM alpine:3.24 AS prod
# Install runtime libraries
RUN apk add --no-cache libc++ libstdc++ ca-certificates && \
    addgroup -S app && adduser -S app -G app && \
    mkdir -p /app /var/log/taxbroker && \
    chown -R app:app /app /var/log/taxbroker

WORKDIR /app
COPY --from=builder --chown=app:app /workspace/build/src/taxbroker_server /app/taxbroker_server

USER app

EXPOSE 8080
CMD ["./taxbroker_server"]
