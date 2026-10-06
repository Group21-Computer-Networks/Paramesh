# AGENTS.md

Guide for coding agents working on ParaMesh. Read it before changing anything.

ParaMesh is a distributed shared-memory runtime: a C API (`include/paramesh.h`) backed by a C++20
library, a per-node daemon (`pmd`) and two tools (`pm`, `pmrun`). It is built milestone by
milestone, M0 to M6, by one to three agents in parallel branches, with a person closing each
milestone gate.

## Sources of truth

- **`docs/PLAN.md`** is the implementation plan. Every task is a card in it: goal, files it may
  touch, what it needs, its stream, and the check that proves it done. Where the plan differs from
  the SRS, the HLD or Doc 1, the plan wins; its "Changes from the SRS and HLD" table lists every
  difference. Only the person edits the plan.
- **Frozen files.** After the M0 gate, `include/paramesh.h`, `docs/PROTOCOL.md`,
  `docs/STATE_MACHINES.md` and `docs/INTERNAL_API.md` change only with the person's approval,
  recorded in the task log, unless the task card lists the file.
- **`docs/TODO.md`** holds everything deferred.
- **No guessing.** A task that meets a point listed under "Still open" in the plan, or anything the
  card, the plan and the frozen files leave ambiguous, stops and asks the person. It does not choose.

## Build and test

Needs CMake 3.25+, Ninja, and GCC or Clang with C++20 (Ubuntu 24.04:
`sudo apt install cmake ninja-build clang clang-format clang-tidy`).

```bash
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```

| Preset | What it is |
| --- | --- |
| `dev` | Debug build, no sanitizer. |
| `asan`, `ubsan`, `tsan` | The same with AddressSanitizer, UndefinedBehaviorSanitizer or ThreadSanitizer. |
| `tidy` | Clang build with clang-tidy run on every compiled file; any finding fails the build. |

Formatting check, as CI runs it:

```bash
git ls-files -- '*.c' '*.h' '*.cpp' '*.hpp' ':!:third_party/**' | xargs clang-format --dry-run --Werror
```

GitHub Actions (`.github/workflows/ci.yml`) builds and runs the unit tests with GCC and Clang,
without a sanitizer and under each of the three, plus the format, tidy and log checks, on every
push and pull request. Multi-node tests run from local scripts in `tests/multi/`, not in Actions.

## Layout and build conventions

The layout and the one-way dependency direction are in `docs/PLAN.md`, "Repository layout and
toolchain". A source directory never includes headers from one that depends on it.

- Each `src/<dir>/` is one CMake target, `paramesh::<dir>`, defined by one `paramesh_module()`
  call in its own `CMakeLists.txt` (see `cmake/ParameshModule.cmake`). Sources are found by glob:
  **add `.cpp` files, do not edit the shared build files.** A directory with no sources yet is an
  INTERFACE target; it becomes a static library when its first `.cpp` appears.
- Headers are included relative to `src/`: `#include "wire/frame.h"`.
- Directories that make up `libparamesh` (`platform`, `wire`, `coh`, `mem`, `store`, `net`, `rt`,
  `lib`) are compiled with `-fno-exceptions`. `pmd` and `tools` allow exceptions.
- Every target gets the same warnings, as errors (`paramesh_target_settings()`).
- Unit tests: each `tests/unit/<name>_test.cpp` becomes its own executable and ctest entry,
  `unit.<name>`. One file per source directory (`wire_test.cpp`, `coh_test.cpp`, ...); `main.cpp`
  supplies `main()`. Tests use doctest.
- Applications: each `apps/<name>/` has its own `CMakeLists.txt` and is picked up automatically.
  Applications use `paramesh.h` only: link `paramesh::api` and `paramesh::lib`.
- A test registered with `add_test()` anywhere uses `${PARAMESH_TEST_LAUNCHER}` as its command
  prefix. Under ThreadSanitizer it runs the test with ASLR off (`setarch -R`), which GCC 13's TSan
  runtime needs on recent kernels.
- Third-party code is vendored header-only in `third_party/`, unmodified, with its licence and a
  README giving version, source and checksums. Only `src/platform/` adapters and tests include it.

## Engineering rules

These are the plan's rules and apply to every pull request.

| Rule | What it requires |
| --- | --- |
| Scope | A task touches only the files its card lists. A change to a frozen file needs a person's approval first. |
| Ownership of memory | No raw `new` or `delete`. Resources are held by RAII types: `std::unique_ptr`, containers, and small wrappers for file descriptors and mappings. |
| Errors in `libparamesh` | Functions return a `Result<T>`. The library is compiled with exceptions off. A fatal condition goes through one abort-the-job function that logs the reason. |
| Errors in `pmd` and tools | Exceptions are allowed and are caught at every thread entry point. |
| Task bodies | The one file that calls application task bodies is compiled with exceptions on and catches everything. |
| C boundary | No exception, C++ type or allocation ownership crosses `paramesh.h`. |
| Blocking | The fault-handler thread waits only on the userfaultfd and an eventfd. The network thread waits only on epoll. Neither takes a lock that a blocking thread can hold. |
| State machines | Code in `src/coh/` makes no system calls and reads no clock. Time arrives as an event. |
| Trust | No value received from another node is an input to a weight, a quota or an admission decision. A unit test feeds forged announcements and checks the weights do not move. |
| Never zero-fill | No code path installs zeros or a stale copy to wake a thread. If data is lost, the job aborts. |
| Tests | Every task adds unit tests for what it adds. A bug fix adds the test that would have caught it. |
| Quality gate | clang-format, clang-tidy and warnings as errors pass. Unit tests pass under address, undefined-behaviour and thread sanitizers in GitHub Actions. |
| Logging | One JSON object per line with time, node, job, level, event and fields, through the logging interface. |
| Configuration | Command-line flags for caps and peers; all tunables from a key=value file. Settings reach job processes as environment variables set by `pmd`. |
| Commits | Small, one purpose each, with the task ID in the pull-request title. |
| Deferred work | Anything left undone is added to `docs/TODO.md` in the same pull request. |

Numbers come from the configuration keys in the plan's "Tunables" table, never from literals.

## Implementation logs

Every change is logged in `docs/logs/`, and CI fails a pull request whose changes are not.
Details and commands are in `docs/logs/README.md`; the tool is
`python3 .claude/skills/paramesh-plan/scripts/plan.py` (`status`, `card`, `new`, `check`).

- One task is one branch, one log (`docs/logs/<ID>.md`) and one pull request whose title starts
  with the task ID (`M1-2: ...`). Work that fits no card needs the person's approval first and
  uses an `X-<slug>` log.
- Start the log with `plan.py new <ID>` and commit it before any code. Only start a task that
  `plan.py status` lists as ready; within a stream, tasks go in order.
- Write a session entry after each meaningful step: what changed, the commands run and what they
  actually printed, and what comes next. Append; never rewrite earlier entries. Keep
  "Files touched" current; CI compares it with the branch's diff.
- A question you cannot settle from the plan goes under "Questions and blockers" as `[open]`,
  the log goes to `Status: blocked`, and you ask the person. M0 drafting tasks flag Still-open
  points as `[for gate]` instead.
- Before `Status: done`: run the card's Done-when check and the quality gate and record the actual
  results under "Verification"; fill every section or write `None`; run
  `plan.py check --base origin/main --pr-title "<title>"` and fix what it reports.
- Never edit another task's log. A fix to merged work adds a session to that task's log and is
  titled `<ID> fix: ...`. Gates are closed only by the person.
