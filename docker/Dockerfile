# Stage 1: Development Environment
FROM alpine:3.19 AS dev

RUN apk add --no-cache \
    bash \
    ca-certificates \
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
RUN git clone --depth 1 --branch v1.16 https://github.com/linux-test-project/lcov.git /tmp/lcov && \
    cd /tmp/lcov && \
    make install && \
    rm -rf /tmp/lcov

WORKDIR /workspace

CMD ["/bin/bash"]

# Stage 2: Builder
FROM dev AS builder
COPY . /workspace

RUN CC=clang CXX=clang++ cmake -B build -G Ninja -S . -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build -j$(nproc)

# Stage 3: Production
FROM alpine:3.19 AS prod
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
