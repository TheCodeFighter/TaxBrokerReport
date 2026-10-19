# Debug tests in VS Code with Docker

## Setup

1. Start Docker and open the repository root locally in VS Code.
2. Install Microsoft's **C/C++** extension (`ms-vscode.cpptools`). VS Code recommends it for this
   workspace.
3. Open a test under `tests/unit/` or `tests/integration/` and set a breakpoint on an executable line
   with **F9** or by clicking beside the line number.
4. Open **Run and Debug** (`Ctrl+Shift+D`) and select **Docker: debug unit tests** or
   **Docker: debug integration tests**.
5. Press **F5**. When asked for a GoogleTest filter, enter `Suite.Test` for one test, `Suite.*` for a
   group, or `*` for the full selected suite. For example,
   `ExactArithmeticTest.RoundsDirectlyWithoutAnIntermediateDecimalScale` selects one unit test.

VS Code builds the selected executable before starting it. The first build downloads CMake
dependencies and takes longer. Execution pauses at your breakpoint; without a matching breakpoint,
the tests run to completion. Failed GoogleTest assertions also stop the debugger.

## Step through code and inspect values

| Action | Key / location |
| --- | --- |
| Execute the current line and stay in this function | **F10** (Step Over) |
| Enter the called function | **F11** (Step Into) |
| Return to the caller | **Shift+F11** (Step Out) |
| Continue to the next breakpoint | **F5** |
| Stop debugging | **Shift+F5** |
| Inspect local variables and expand objects | **Variables** in Run and Debug |
| Keep an expression visible as you step | Add it to **Watch** |
| Inspect one value | Hover over its name while paused |
| Inspect the caller's variables | Select its frame under **Call Stack** |

Inline values are enabled in the workspace. Use **Variables** and **Watch** for complete object
inspection; expand members and container elements. GDB pretty-printers make standard C++ strings
and containers readable. Watch expressions can include fields such as `record.mUnits` or a value
you select from the current test. Evaluate expressions in the **Debug Console** while paused.

Values belong to the selected stack frame and must be in scope and initialized. C++ debugging
pauses at executable source locations: comments, declarations without executable work, and some
parts of compound expressions do not have a separate step. Debug builds disable optimization to
make stepping predictable and retain variables, but a debugger cannot display variables before
their lifetime starts or after it ends.

## Docker isolation

These profiles are for a local VS Code window. The editor and C/C++ debug adapter run on the host;
CMake, Clang, GDB, and the test executable run inside Docker. No host compiler or GDB is needed.
The [VS Code pipe transport](https://code.visualstudio.com/docs/cpp/pipe-transport) starts GDB
through `scripts/debug_tests.sh`, using `docker compose run --rm --no-deps -T dev`. Each session
gets a temporary container, removed when it exits, with the existing debugger permissions from
`docker/docker-compose.yml`. No debugger port or server needs to be started.

The existing `taxbrokerreport-dev` image already includes the compiler, GDB, and C++ pretty-printers.
The profiles load `docker/gdbinit` explicitly because Alpine does not register these printers for
automatic loading. It also enables standard-library expressions such as indexing a vector in Watch.
There is no separate debugger image. The debug build uses `build/test-debug/`, separate from the
normal `build/dev/` outputs and `build/coverage/`, and explicitly uses Clang with `-g3 -O0`, frame
pointers, and full type debug information. Container source paths under `/workspace` map to the
open workspace so breakpoints match your local files. The working directory matches the CTest
test directory, which also keeps relative test fixture paths working.

The existing **Reopen in Container** configuration is a separate development workflow. These
profiles expect Docker access from the host, so use **Dev Containers: Reopen Folder Locally** if
you opened the repository in a Dev Container.

To build either debug executable manually on the host:

```sh
scripts/debug_tests.sh build unit
scripts/debug_tests.sh build integration
```

The legacy launch configuration selected `/usr/bin/gdb` in VS Code's environment. Its Makefile
built inside Docker, but the launch configuration did not explicitly put GDB or the running test
inside Docker. The new profiles specify that connection directly.

## Troubleshooting

- **Docker connection fails:** ensure Docker is running and your host user can run `docker ps`.
- **A breakpoint stays hollow:** check the suite and filter, place it on an executable line, and
  save changed files before pressing F5 again. The prelaunch task rebuilds the executable.
- **The build task fails:** inspect its terminal output. Fix the error before debugging; choosing
  **Debug Anyway** can run a stale binary.
- **No tests run:** filters are case-sensitive GoogleTest names, not filenames or CTest regular
  expressions. Check the `TEST(Suite, Test)` declaration.
- **A container or string appears as internal fields:** expand it in Variables; in the Debug
  Console, `-exec info pretty-printer` lists the GDB printers available in the container.
