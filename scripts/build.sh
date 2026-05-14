#!/usr/bin/env bash
set -e

echo "==> Building Docker dev image (if needed)..."
docker compose build dev

echo "==> Compiling backend code using Clang + Ninja..."
docker compose run --rm dev sh -c "CC=clang CXX=clang++ cmake -B build -S . -G Ninja && cmake --build build -j$(nproc)"
