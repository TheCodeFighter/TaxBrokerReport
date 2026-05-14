#!/usr/bin/env bash
set -e

if [ "$1" == "prod" ]; then
    echo "==> Building Docker prod image..."
    docker compose build prod
else
    echo "==> Building Docker dev image (if needed)..."
    docker compose build dev

    echo "==> Compiling backend code using Clang + Ninja..."
    docker compose run --rm dev sh -c "CC=clang CXX=clang++ cmake -B build -S . -G Ninja && cmake --build build -j$(nproc)"
fi
