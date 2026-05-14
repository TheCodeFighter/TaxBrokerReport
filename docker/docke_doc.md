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

### 1. Build the Development Image and Compile Code
This script builds the Docker image (if changed) and compiles the C++ code using Clang+Ninja inside the container.
```bash
./scripts/build.sh
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
./scripts/run_server.sh
```

---

## Native Docker Compose usage

Because of the `.env` file in the root, you no longer need to specify the file path with `-f`. You can use standard compose commands directly:

* **Build**: `docker compose build dev`
* **Production Test**: `docker compose up prod`
* **Cleanup**: `docker compose down -v`
