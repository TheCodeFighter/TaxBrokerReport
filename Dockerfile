# ================================
# Stage 1: Development Base
# ================================
FROM ubuntu:22.04 AS dev-base

ENV DEBIAN_FRONTEND=noninteractive

# Install apt-utils first
RUN apt-get update && apt-get install -y apt-utils && rm -rf /var/lib/apt/lists/*

# Install dependencies
RUN apt-get update && apt-get install -y \
    build-essential git pkg-config gdb gdbserver \
    vim curl wget libpoppler-cpp-dev libgtest-dev \
    nlohmann-json3-dev file libpugixml-dev \
    libxml2-dev libxml2-utils valgrind \
    ninja-build ccache \
    && rm -rf /var/lib/apt/lists/*

# Configure Ccache (Enable it nicely for dev)
ENV PATH="/usr/lib/ccache:$PATH"

# Create user and group
ARG USER_ID=1000
ARG GROUP_ID=1000
RUN groupadd -g ${GROUP_ID} appuser && \
    useradd -u ${USER_ID} -g ${GROUP_ID} -s /bin/bash -m appuser

# Install CMake 3.30
RUN wget https://github.com/Kitware/CMake/releases/download/v3.30.2/cmake-3.30.2-linux-x86_64.tar.gz && \
    tar --strip-components=1 -xzvf cmake-3.30.2-linux-x86_64.tar.gz -C /usr/local && \
    rm cmake-3.30.2-linux-x86_64.tar.gz

# Build Google Test static libraries (Legacy but safe method)
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
COPY --chown=appuser:appuser . .

# 1. Use Ninja Generator (-G Ninja) for speed
# 2. Fixed typo: -DCMAKE_BUILD_TYPE (added 'D')
# 3. Explicitly set Release mode
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build

# ================================
# Stage 4: Production Runtime
# ================================
FROM ubuntu:22.04 AS prod

# Runtime dependencies only
RUN apt-get update && apt-get install -y \
    libpoppler-cpp-dev \
    libpugixml1v5 \
    libxml2 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
# Copy only the executable from the builder stage
COPY --from=builder /app/build/EdavkiXmlMaker .

CMD ["./EdavkiXmlMaker"]