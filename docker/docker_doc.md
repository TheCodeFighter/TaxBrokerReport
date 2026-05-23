# Backend Development Docker Guide

This directory contains the Docker configuration for isolating the backend Clang C++20 build environment alongside tests, profiling tools, and debuggers.

We use **Docker Compose** to bridge local source-code access dynamically without requiring constant image rebuilds, while delivering identical execution spaces for debuggers across different OS architectures (Windows, macOS, Linux).

## Prerequisites
- Docker Engine & Docker Compose

## Recommended IDE Setup
If you use **VS Code**, the easiest way to develop is using the DevContainers extension.
Click **"Reopen in Container"** when prompted. The IDE will automatically map into the `dev` stage container, initialize the CMake Ninja build generator, and configure LLDB properly for C++ debugging.

---

## Command Line Usage

The easiest way to work with this environment is using the pre-configured scripts in the `scripts/` directory. These scripts automatically handle the Docker Compose context and common flags.

### 1. Build the Backend (no image rebuild)
By default, this script only compiles the C++ code using Clang+Ninja inside the existing dev container image.
```bash
./scripts/build.sh
```

### 2. Build the Development Image and Compile Code
If you need to rebuild the dev image, pass `docker-build` and it will rebuild the image before compiling.
```bash
./scripts/build.sh docker-build
```
If you want to build the highly-optimized production image instead, pass the `prod` parameter:
```bash
./scripts/build.sh prod
```

### 2. Run Tests
Execute the entire test suite via CTest inside the container:
```bash
./scripts/test.sh
```
To run specific tests matching a pattern:
```bash
./scripts/test.sh "tax_processor"
```

### 3. Run the Server
Starts the compiled backend server and maps port 8080 to your host machine:
```bash
./scripts/run.sh
```

---

## Native Docker Compose usage

Because of the `.env` file in this directory, you can run compose from here without specifying the file path with `-f`. If you run compose from the repo root, keep using `-f docker/docker-compose.yml`.

* **Build**: `docker compose build dev`
* **Dev**: `docker compose up --build dev`
* **Production Test**: `docker compose up prod`
* **Cleanup**: `docker compose down -v`

---

## UID/GID mapping (permissions)

The `dev` container runs as `${UID}:${GID}` to keep file ownership aligned with your host user for bind mounts like `/workspace`.

Before starting the dev container, export your current UID and GID:
```bash
export UID=$(id -u)
export GID=$(id -g)
docker compose up dev
```

The `.env` file provides a default fallback (UID=1000, GID=1000) if you do not export them.

If files were already created as root, fix ownership once on the host:
```bash
sudo chown -R $(id -un):$(id -gn) .
```

---

## Logs

The dev service bind-mounts `../logs` into `/var/log/taxbroker` and sets `TBR_LOG_FILE` to `/var/log/taxbroker/taxbroker.log` by default. Logs end up in the repo `logs/` folder and still honor `TBR_LOG_FILE_MODE` (append or truncate).

The prod service mounts a named volume (`taxbroker_logs`) at `/var/log/taxbroker` so logs stay outside the container filesystem.

To view logs from the container:
```bash
docker compose run --rm dev sh -lc 'tail -f /var/log/taxbroker/taxbroker.log'
```
