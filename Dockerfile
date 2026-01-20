# ================================
# Stage 1: Development Base
# ================================
FROM ubuntu:22.04 AS dev-base

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    git \
    pkg-config \
    gdb \
    gdbserver \
    curl \
    wget \
    file \
    valgrind \
    ninja-build \
    ccache \
    lcov \
    cmake \
    libgtest-dev \
    nlohmann-json3-dev \
    libpugixml-dev \
    libxml2-dev \
    libxml2-utils \
    libpoppler-cpp-dev \
    qt6-base-dev \
    libgl1-mesa-dev \
    libxkbcommon-x11-0 \
    libegl1-mesa-dev \
    && rm -rf /var/lib/apt/lists/*

# Ccache
ENV PATH="/usr/lib/ccache:$PATH"

ARG USER_ID=1000
ARG GROUP_ID=1000
RUN groupadd -g ${GROUP_ID} appuser && \
    useradd -u ${USER_ID} -g ${GROUP_ID} -s /bin/bash -m appuser

RUN cd /usr/src/gtest && \
    cmake . && \
    make && \
    cp lib/*.a /usr/lib

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

# Use Ninja for speed; CMAKE_BUILD_TYPE=Release strips debug symbols
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSAVE_GENERATED_FILES=0 && \
    cmake --build build --parallel $(nproc)

# ================================
# Stage 4: Production Runtime
# ================================
FROM ubuntu:22.04 AS prod

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    libpoppler-cpp0v5 \
    libpugixml1v5 \
    libxml2 \
    libqt6widgets6 \
    libqt6gui6 \
    libqt6core6 \
    libqt6opengl6 \
    libxkbcommon-x11-0 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /app/build/EdavkiXmlMaker .

CMD ["./EdavkiXmlMaker"]