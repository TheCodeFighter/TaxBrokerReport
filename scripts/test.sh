#!/usr/bin/env bash
set -e

# If an argument is provided, run specific tests matching the regex. Otherwise, run all.
if [ -n "$1" ]; then
    echo "==> Running tests matching: $1"
    docker compose run --rm dev ctest --test-dir build -R "$1"
else
    echo "==> Running all tests..."
    docker compose run --rm dev ctest --test-dir build --output-on-failure
fi
