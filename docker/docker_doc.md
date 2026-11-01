# Backend Development Docker Guide

This directory contains the Docker configuration for isolating the backend Clang C++20 build environment alongside tests, profiling tools, and debuggers.

We use **Docker Compose** to bridge local source-code access dynamically without requiring constant image rebuilds, while delivering identical execution spaces for debuggers across different OS architectures (Windows, macOS, Linux).

## Prerequisites
- Docker Engine & Docker Compose

## Recommended IDE Setup
For test debugging, open the repository locally in **VS Code** with Microsoft's **C/C++** extension
and select **Docker: debug unit tests** or **Docker: debug integration tests**. The build, GDB, and
tests run inside the existing `dev` image. See the [debugging guide](../docs/debugging.md) for
step-by-step instructions and variable inspection.

The existing Dev Containers configuration can also open the editor workspace inside `dev`.
The Docker test launch profiles above expect a local VS Code window with host Docker access.

---

## Command Line Usage

The easiest way to work with this environment is using the pre-configured scripts in the `scripts/` directory. These scripts automatically handle the Docker Compose context and common flags.

Every script that builds or checks the code **starts from an empty build folder**, so a local pass
means a CI pass. No script builds the dev image on its own: when it is missing, the script stops
with the hint `scripts/build.sh image`.

### 1. Build the development image
Builds only the dev image (`taxbrokerreport-dev`). This is the only way to build it besides
`docker-build`.
```bash
./scripts/build.sh image
```

### 2. Build the Backend
Deletes `build/dev`, then compiles the backend and the debug tools from scratch with Clang+Ninja
inside the existing dev image (Debug, `BUILD_DEBUG_TOOLS=ON`).
```bash
./scripts/build.sh
```
`docker-build` builds the dev image first and then runs the same build. `prod` builds the
production image instead:
```bash
./scripts/build.sh docker-build
./scripts/build.sh prod
```

The production image is a Clang Release build with `TAXBROKER_STATIC=ON`: `taxbroker` (the CLI)
and `taxbroker_server` are linked fully static and stripped, so they run on any x86-64 Linux and
the image needs no runtime libraries. `prod` also copies both binaries to `build/static`. The
image and the CI artifact ship `LICENSE` and `THIRD_PARTY_NOTICES.txt` (the licences of the
code linked into the binaries) next to them.

The CMake configure downloads the dependencies from GitHub (FetchContent). CMake gives up at once
on an HTTP error such as a 504, so `build.sh dev`, `cppcheck.sh`, `coverage.sh` and the Dockerfile
run the configure up to three times, 30 s and then 90 s apart. A real CMake error therefore takes
about two minutes to fail.

### 3. Run Tests
Execute the entire test suite via CTest inside the container. This builds `build/dev` from
scratch first:
```bash
./scripts/test.sh
```
To run specific tests matching a pattern:
```bash
./scripts/test.sh "tax_processor"
```
`--reuse-build` (first argument) runs the existing `build/dev` as it is. It configures and compiles
nothing, and fails with a hint when `build/dev` does not exist:
```bash
./scripts/test.sh --reuse-build --suite unit
```
`valgrind.sh` and `cppcheck.sh` take the same flag. Without it, `cppcheck.sh` configures
`build/cppcheck` from scratch; with it, it uses the compile database of `build/dev`.
`valgrind.sh --part regular|large|all` (default `all`) runs everything but the
`*LargeInputTest` unit test suites, only those suites, or both.

### 4. Run the Server
Builds `build/dev` from scratch, then starts the backend server and maps port 8080 to your host
machine:
```bash
./scripts/run.sh
```

### Build folders

`build/` only holds sibling folders. Each script owns one of them and deletes only that one:

| Folder | Used by |
| --- | --- |
| `build/dev` | `build.sh dev`, `test.sh`, `valgrind.sh`, `run.sh`, the CI build job |
| `build/cppcheck` | `cppcheck.sh` (configure only) |
| `build/debug-tools` | the `dump_*.sh` scripts |
| `build/coverage` | `coverage.sh` |
| `build/fuzz` | `fuzz.sh` (Clang with sanitizers, only the fuzz target) |
| `build/test-debug` | `debug_tests.sh` (VS Code), incremental on purpose |
| `build/static` | `build.sh prod` (the static binaries copied out of the production image) |

---

## CI

The `CI` workflow (`.github/workflows/ci.yml`) builds the dev image and the Debug backend once per
run. Later jobs load them and compile nothing:

```text
Dev image ─┬─ Development build ─┬─ Required tests   (test.sh --reuse-build)
           │                     ├─ Valgrind         (valgrind.sh --reuse-build --part regular)
           │                     ├─ Valgrind (large inputs)  (--part large, only when related
           │                     │                            files change, not required)
           │                     └─ Cppcheck         (cppcheck.sh --reuse-build)
           ├─ format-check                  (pull requests only)
           ├─ Fuzz (Trade Republic parser)  (fuzz.sh, 60 s, own sanitizer build, not required yet)
           └─ Coverage (current) ─┬─ coverage      (pull requests only)
              Coverage (baseline) ┘
Production image                   (own Release build inside the Dockerfile)
```

- `Dev image` runs `build.sh image` and uploads the image as a compressed artifact. A build tree
  must run in the exact image that compiled it, so the image is shared, not rebuilt per job.
- `Development build` loads the image, runs `build.sh dev`, and uploads `build/dev` as a tar
  archive (it keeps the executable bits; the artifact upload alone does not).
- Artifacts are kept for one day, except the static binaries (30 days). A new push to a pull
  request cancels its older run.
- The coverage jobs measure the pull-request commit and its base commit in parallel, each from a
  clean GCC build, and `coverage` compares the two reports. lcov runs `gcov` on one core, so
  `coverage.sh` splits the coverage files into one list per core, captures the lists in parallel
  and merges them; the result is the same as one capture.
- The `Production image` job is its own Release build and shares nothing; it is the shipped image.
  It checks the static binaries on the Ubuntu runner (so outside Alpine): both are statically
  linked, the CLI passes `tests/support/check_cli_report.cmake` (golden XML) and the server exits
  with 0. On a push to `main` it uploads `taxbroker-linux-x86_64.tar.gz` (both binaries,
  `LICENSE` and `THIRD_PARTY_NOTICES.txt`) as the artifact
  `taxbroker-linux-x86_64-<short commit>`, kept for 30 days.

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
