# 🛠️ Build Instructions

We use a **Persistent Docker Container** workflow. This allows you to compile, run, and debug the Linux GUI application on any host (Windows/Mac/Linux) without installing Qt6 or C++ toolchains locally.

---

## 🐳 Quick Start (The "Happy Path")

### 1. Start the Engine

This creates a persistent container (`edavki-container`) with X11 forwarding enabled so the GUI can pop up on your screen. Dev works fine on linux (Ubuntu 24.04).

```bash
make dev-up
```

>[!Note]
 If you get a "permission denied" error regarding the docker socket, run `sudo usermod -aG docker $USER and relogin`.

### 2. Configure CMake

Initializes the build system inside the container.

```Bash
make configure
```

### 3. Build & Run

Compiles the app and launches the GUI.

```Bash
make build
make run
```

## 🍎 Running the GUI on macOS (Docker + XQuartz)

The app runs in a Linux container and draws to your Mac via X11. You need **XQuartz** and one-time setup:

1. **Install XQuartz** (if needed):
   `brew install --cask xquartz`
   Then **log out and back in** (or reboot) so the X11 app is properly registered.

2. **Allow network clients** (required for Docker to connect):
   - Open **XQuartz** (Applications → Utilities → XQuartz).
   - In the menu bar: **XQuartz → Preferences → Security**.
   - Check **"Allow connections from network clients"**.
   - **Quit XQuartz completely** (XQuartz → Quit) and open it again.

3. **Allow localhost** (in Terminal, each time after you start XQuartz):
   ```bash
   xhost +localhost
   ```

4. **Start the dev container** (so it gets `DISPLAY=host.docker.internal:0`):
   ```bash
   make dev-down
   make dev-up
   ```

5. **Build and run**:
   ```bash
   make run
   ```

The app window should appear in XQuartz. If you see `could not connect to display host.docker.internal:0`, re-check steps 2 and 3 and ensure XQuartz is running.

## 🧪 Testing & Quality

- Run Unit Tests: Executes all GoogleTest suites (headless).

  ```Bash
  make test
  ```

- Granular Testing: To save time, you can build and run specific test suites:

  ```Bash
  make build-test-report-loader
  make build-test-xml-generator
  make build-test-application-service
  make build-test-gui
  ```

- Code Coverage: Generates an HTML report of test coverage.

  ```Bash
  make coverage
  # Open tests/coverage_report/index.html to view
  ```

---

## 💻 VS Code Integration (Recommended)

1. This project is optimized for the Dev Containers extension.

2. Run `make dev-up` in your terminal.

3. In VS Code, press F1 and select "Dev Containers: Attach to Running Container".

4. Select edavki-container.

5. Press F5: The launch.json is configured to build and debug automatically. If we got launch error, just re-run.

---

## 🧹 Cleanup

- Stop for the day:

  ```Bash
  make dev-down
  ```

  Stops container, keeps build cache.

- Reset Build Files:

  ```Bash
  make clean
  ```

- Nuclear Reset: clean (Wipes build cache) or manually remove the edavki-dev-cache volume to start fresh.

  ```Bash
  make clean-rebuild
  ```

## ⌨️ Makefile Command Reference

| Command | Description |
| :--- | :--- |
| `make build-image` | Builds the base dev Docker image. |
| `make dev-up` | Starts the persistent dev container. |
| `make configure` | Runs CMake initialization (Debug mode). |
| `make build-main` | Compiles only the main application binary. |
| `make build` | Compiles everything (App + Tests). |
| `make run` | Builds and launches the GUI on the host display. |
| `make test` | Runs all tests using the offscreen Qt plugin. |
| `make coverage` | Captures and filters coverage data into an HTML report. |
| `make shell` | Drops you into an interactive bash shell inside the container. |

## 📦 Production & Release

### Linux AppImage Build

To manufacture a standalone Linux binary (AppImage) for distribution:

```Bash
# Optional: specify version, defaults to 1.0.0
make release-linux VERSION=1.1.0
```

This command builds a specialized bundler image, compiles the code in Release mode, and runs the bundle.sh script to create a portable package in production/linux/v[VERSION]/bin.
