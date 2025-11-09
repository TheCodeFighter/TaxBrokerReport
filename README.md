<p align="center">
  <img src="Manuals/icon.jpg" alt="Project Icon" width="150"/>
</p>

# EdavkiXmlMaker

EdavkiXmlMaker is a C++ project for generating XML files from financial reports, with support for Poppler PDF parsing and Google Test-based unit tests. The project is containerized using Docker for reproducible builds and easy development.

## Quick Start

### 1. Build Dev Docker Image

```bash
make build-image
```

### 2. Configure and Build (Debug)

```bash
make configure build
```

If error is:
>`docker: permission denied while trying to connect to the Docker daemon socket at unix`

Run:

``` bash
sudo usermod -aG docker $USER
```

### 3. Run Tests

```bash
make test
```

### 4. Debugging

- Debug with GDB inside container:

  ```bash
  make debug-test-report-loader
  ```

- Remote debug with gdbserver:

  ```bash
  make gdbserver BIN=build/EdavkiXmlMaker PORT=1234
  # Then attach with gdb or VS Code
  ```

### 5. Build Production Image

```bash
make build-prod
```

### 6. Run Production Binary

```bash
make run-prod
```

## 🧩 GitHub Pipelines

### 🧠 `pr-prep.yml`

This workflow validates Pull Requests to enforce proper issue linking:

- Respects the default PR template (`.github/PULL_REQUEST_TEMPLATE.md`).
- Ensures each PR references an issue (e.g., `Fixes #123`) or explicitly states `None`.
- Will **comment on the PR** if the check fails, alerting the author.
- Fails the workflow if no valid issue reference is found, preventing merges until fixed.

---

### ⚙️ `ci.yml`

The continuous integration (CI) workflow runs automatically on each commit or pull request.
It handles:

- Building the project
- Running unit tests  
- Checking code formatting or linting (if configured)
- Running Valgrind for memory checks

This ensures that all code merged into the main branch passes basic quality and functionality checks before integration.

## Documentation

- See the [Manuals/Instructions.md](Manuals/Instructions.md) for detailed usage, setup, and troubleshooting.

## VS Code Integration

- Launch and task configurations are provided in `.vscode/` for building, testing, and debugging inside Docker.

## License

This project is licensed under the MIT License.
