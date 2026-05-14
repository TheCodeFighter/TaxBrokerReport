#!/usr/bin/env bash
set -e

echo "==> Running taxbroker_server in development mode on port 8080..."
# Use --service-ports to ensure the 8080 mapping in docker-compose.yml is respected during `run`
docker compose run --rm --service-ports dev ./build/src/taxbroker_server
