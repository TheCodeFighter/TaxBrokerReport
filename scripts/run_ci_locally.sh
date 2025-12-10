#!/bin/bash
set -e

# -----------------------------
# 1. Install act if not present
# -----------------------------
if ! command -v act &> /dev/null
then
    echo "act not found. Installing..."
    curl https://raw.githubusercontent.com/nektos/act/master/install.sh | sudo bash
fi

# -----------------------------
# 2. Create persistent Ubuntu container
# -----------------------------
CONTAINER_NAME="ci-edavki-ubuntu"
IMAGE_NAME="ubuntu:22.04"
CACHE_VOLUME="ci-cache"

# Check if container exists
if [ "$(docker ps -aq -f name=$CONTAINER_NAME)" ]; then
    echo "Container $CONTAINER_NAME exists, starting it..."
    docker start $CONTAINER_NAME
else
    echo "Creating persistent CI container $CONTAINER_NAME..."
    docker run -d --name $CONTAINER_NAME \
        -v "$(pwd)/..":/github/workspace \
        -v $CACHE_VOLUME:/github/workspace/build \
        --hostname ci-edavki-ubuntu \
        $IMAGE_NAME sleep infinity

    echo "Installing build dependencies inside container..."
    docker exec -it $CONTAINER_NAME bash -c "
        set -e
        apt-get update -qq
        DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
            build-essential \
            cmake \
            git \
            bash \
            wget \
            python3 \
            python3-pip \
            libpoppler-dev \
            libpoppler-cpp-dev \
            nlohmann-json3-dev \
            libpugixml-dev \
            ca-certificates \
            unzip \
            pkg-config \
            libgtest-dev
        # Upgrade CMake to 3.30+
        wget -qO- https://cmake.org/files/v3.30/cmake-3.30.3-linux-x86_64.tar.gz | tar --strip-components=1 -xz -C /usr/local

        # Build and install GTest
        cd /usr/src/gtest
        cmake .
        make -j\$(nproc)
        cp lib/*.a /usr/lib

        apt-get clean
        rm -rf /var/lib/apt/lists/*
    "
fi

# -----------------------------
# 3. Run CI workflow locally inside container
# -----------------------------
docker exec -it $CONTAINER_NAME bash -c "
    set -e
    cd /github/workspace/EdavkiXmlMaker
    echo 'Cleaning old build directory...'
    rm -rf ../build/* ../build/.[!.]* 2>/dev/null || true
    echo 'Configuring project...'
    cmake -S . -B ../build -DCMAKE_BUILD_TYPE=Debug -DSAVE_GENERATED_FILES=0
    echo 'Building project...'
    cmake --build ../build -- -j\$(nproc)
    echo 'Running tests...'
    ctest --test-dir ../build --output-on-failure
"

# -----------------------------
# 4. Stop container after run
# -----------------------------
echo "Stopping container $CONTAINER_NAME..."
docker stop $CONTAINER_NAME

echo "To remove container: docker rm $CONTAINER_NAME"
