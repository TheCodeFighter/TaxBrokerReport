# ================================
# Stage 1: Development Base
# ================================
FROM ubuntu:22.04 AS dev-base

ENV DEBIAN_FRONTEND=noninteractive

# Install apt-utils first to avoid debconf warning
RUN apt-get update && apt-get install -y apt-utils && rm -rf /var/lib/apt/lists/*

# Install other dependencies
RUN apt-get update && apt-get install -y \
    build-essential git pkg-config gdb gdbserver vim curl wget \
    libpoppler-cpp-dev libgtest-dev nlohmann-json3-dev file \
    valgrind \
    && rm -rf /var/lib/apt/lists/*

# Create user and group matching host UID/GID
ARG USER_ID=1000
ARG GROUP_ID=1000
RUN groupadd -g ${GROUP_ID} appuser && \
    useradd -u ${USER_ID} -g ${GROUP_ID} -s /bin/bash -m appuser

# Install CMake 3.30
RUN wget https://github.com/Kitware/CMake/releases/download/v3.30.2/cmake-3.30.2-linux-x86_64.tar.gz && \
    tar --strip-components=1 -xzvf cmake-3.30.2-linux-x86_64.tar.gz -C /usr/local && \
    rm cmake-3.30.2-linux-x86_64.tar.gz

# Build Google Test static libraries
RUN cd /usr/src/gtest && cmake . && make && cp lib/*.a /usr/lib

WORKDIR /app
USER appuser

# ================================
# Stage 2: Development Container
# ================================
FROM dev-base AS dev
WORKDIR /app
CMD ["/bin/bash"]

# ================================
# Stage 3: Production Build
# ================================
FROM dev-base AS builder
WORKDIR /app
COPY . .
RUN rm -rf build && mkdir build && chown appuser:appuser build  # Reset build directory
RUN cmake -S . -B build -CMAKE_BUILD_TYPE=Release && cmake --build build

# ================================
# Stage 4: Production Runtime
# ================================
FROM ubuntu:22.04 AS prod
RUN apt-get update && apt-get install -y libpoppler-cpp-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=builder /app/build/EdavkiXmlMaker .
CMD ["./EdavkiXmlMaker"]