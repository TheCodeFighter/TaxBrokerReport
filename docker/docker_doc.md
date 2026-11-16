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

The production image is a Clang Release build with `TAXBROKER_STATIC=ON` and
`TAXBROKER_BUILD_TESTS=OFF`. Its builder stage starts from plain Alpine with only the compile
packages (no debuggers, Valgrind, Cppcheck, lcov or libxml2) and builds only the two executables.
The downloaded dependencies are kept in a BuildKit cache folder, so a local rebuild does not
download them again (a fresh CI runner has an empty cache). `taxbroker` (the CLI) and
`taxbroker_server` are linked fully static and stripped, so they run on any x86-64 Linux and
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
Builds `build/dev` from scratch, then starts the backend server and publishes port 8080 on the
loopback address `127.0.0.1` of your host only, so no other computer on the network can reach it.
Inside the container the server listens on `0.0.0.0` (`TBR_SERVER_HOST`), because Docker forwards
the published port to the container's own network address; this is safe only because of the
`127.0.0.1` in the port mapping of `docker/docker-compose.yml`. Do not remove it. This needs
Docker Engine 28.0 or later: older versions let other computers on the same network reach a port
published on `127.0.0.1`, so `scripts/run.sh` stops with an error on them. A browser cannot start
inside the container, so the server writes a launch file (`runtime/launch/launch.html`,
`TBR_LAUNCH_FILE`) and prints the one-time link; `scripts/run.sh` waits on the host for the file
and opens it with `xdg-open` (when the host has it). The link works once, for 2 minutes. Start the
server with:
```bash
./scripts/run.sh
```
`./scripts/run.sh --no-browser` does not open the file, so the link stays unused and can be
redeemed by hand (see "Manual API test" in `docs/debugging.md`). With no build flag, `run.sh`
builds `build/dev` and the frontend (`scripts/frontend.sh build`) and embeds the frontend in the
server. `--reuse-build-backend` keeps `build/dev` (the frontend and the server are rebuilt),
`--reuse-build-frontend` keeps `build/frontend/dist`, `--reuse-build` keeps both and builds
nothing, and `--help` lists the options.

### 5. Build and check the frontend
The dev image has Node and npm. `scripts/frontend.sh` runs in the container: `install`
(`npm ci --ignore-scripts`), `typecheck`, `test`, `build` (to `build/frontend/dist`) and `check`
(size budget, no foreign address, no inline script; with a self-test). With no argument it runs
all five. `--rebuild-all` runs `npm ci` first even when `node_modules` looks current; `build` also
installs on its own when `package-lock.json` is newer than `node_modules`.
`docs/development.md` describes the frontend.
```bash
./scripts/frontend.sh
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
| `build/frontend` | `frontend.sh build` (`dist` holds the four files the server embeds) |

---

## CI

The `CI` workflow (`.github/workflows/ci.yml`) builds the dev image and the Debug backend once per
run. Later jobs load them and compile nothing:

```text
Dev image ─┬─ Development build ─┬─ Required tests   (test.sh --reuse-build)
           │                     ├─ Valgrind         (valgrind.sh --reuse-build --part regular)
           │                     ├─ Valgrind (csv|ecb|reference|messages)  (--part <part>, only
           │                     │                   when related files change, not required)
           │                     └─ Cppcheck         (cppcheck.sh --reuse-build)
           ├─ format-check                  (pull requests only)
           ├─ Frontend                      (frontend.sh, only when frontend files change)
           ├─ Fuzz (Trade Republic parser)  (fuzz.sh, 240 s, own sanitizer build)
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
- The `Frontend` job runs `scripts/frontend.sh` in the dev image. A pull request runs it only when
  `frontend/`, the two frontend scripts, `scripts/lib.sh`, `docker/`, `resources/i18n/` or the
  workflow change; a push
  to `main` always runs it. The job always reports a result (it succeeds when the steps are
  skipped), so it is a required check like the others.
- The coverage jobs measure the pull-request commit and its base commit in parallel, each from a
  clean GCC build, and `coverage` compares the two reports. lcov runs `gcov` on one core, so
  `coverage.sh` splits the coverage files into one list per core, captures the lists in parallel
  and merges them; the result is the same as one capture.
- The `Production image` job is its own Release build and shares nothing; it is the shipped image.
  Its `frontend` stage builds the frontend (`npm ci --ignore-scripts`, `npm run build`), and the
  builder stage embeds the four files (`TAXBROKER_FRONTEND_DIR`). It checks the static binaries
  on the Ubuntu runner (so outside Alpine): both are statically linked, the CLI passes
  `tests/support/check_cli_report.cmake` (golden XML), the server serves `/` and its three
  assets, and it exits with 0. On a push to `main` it uploads `taxbroker-linux-x86_64.tar.gz`
  (both binaries, `LICENSE` and `THIRD_PARTY_NOTICES.txt`) as the artifact
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
