# EdavkiXmlMaker - Quick Start (Docker & Dev)

## Overview

EdavkiXmlMaker parses PDF tax documents and generates XML. This guide covers the Persistent Container Workflow, which keeps a development environment running in the background for near-instant builds and debugging.

- Persistent Environment: Start the "engine" once, run commands instantly.

- GUI Support: X11 forwarding allows the Linux GUI to pop up on your host.

- Granular Builds: Compile only the test you are currently working on to save time.

## Folder Structure

```Plaintext
EdavkiXmlMaker/
├── CMakeLists.txt                 # CMake build configuration
├── Dockerfile                     # Multi-stage Docker build
├── Makefile                       # Persistent workflow & dev commands
├── bin/                           # Compiled binaries (ignored by git)
├── build/                         # Build folder (Persistent volume)
├── include/                       # C++ header files
├── src/                           # Main source code (including GUI)
├── tests/                         # Unit tests (Google Test)
└── util/                          # Utility code (XML/Config)
```

- `build/` is managed via a Docker volume (`edavki-dev-cache`) to ensure persistence between container restarts.

---

## Makefile Commands

### 1. The Development Engine

#### Start the environment

```Bash
make dev-up
```

- Checks for the edavki-dev image (builds it if missing).

- Starts a persistent container with X11 permissions for the GUI.

- Run this once at the start of your work session.

#### Stop the environment

```Bash
make dev-down
```

- Stops and removes the container. Use this when you are finished for the day.

### 2. Configuration & Full Builds

#### Initialize CMake

```Bash
make configure
```

- Prepares the build/ directory inside the running container.

#### Build everything

```Bash
make build
```

- Compiles the main app and all test binaries.

### 3. Granular Test Builds (Fastest)

To avoid rebuilding everything, use target-specific commands (matches VS Code tasks):

```Bash
make build-test-report-loader
make build-test-xml-generator
make build-test-application-service
make build-test-gui
```

### 4. Running the Application

#### Run the GUI

```Bash
make run-main
```

- Compiles the main binary and launches the GUI on your host screen.

#### Run all tests (Offscreen)

```Bash
make test
```

- Runs all tests in offscreen mode (perfect for CI or quick verification).

### 5. Advanced Tools

#### Memory Leak Check

```Bash
make valgrind
```

#### Interactive Shell

```Bash
make shell
```

- Drops you into the running container's bash prompt.

---

## VS Code Debugging

### 🛠 Automated Debugging (F5)

This project is optimized for "Attach to Running Container" mode.

#### 1. Setup

Run `make dev-up` on your host.

In VS Code, click the green bottom-left corner and select "Attach to Running Container".

Choose `edavki-container`.

#### 2. Workflow

F5: Compiles only the specific test/app (via `preLaunchTask`) and starts the debugger.

Granular Targets: The `launch.json` is linked to granular Makefile targets so only the necessary code is recompiled.

#### 3. Troubleshooting GUI Windows

If `make run-main` or debugging fails to open a window:

Ensure you ran `xhost +local:docker` on your host (done automatically by `make dev-up`).

If using a remote connection (SSH), ensure X11 forwarding is enabled.

---

## Maintenance & Cleanup

Reset the Build folder: If CMake cache becomes corrupted:

```Bash
make clean
make configure
```

Total Reset (Nuclear Option): To delete all project-related images and volumes:

```Bash
docker stop edavki-container && docker rm edavki-container
docker rmi edavki-dev
docker volume rm edavki-dev-cache
```
