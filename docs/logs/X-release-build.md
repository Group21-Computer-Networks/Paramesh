# X-release-build: Release builds with warnings as errors, in CI

- Task: X-release-build
- Milestone: none
- Stream: -
- Status: done
- Branch: x-release-build
- Started: 2026-10-22
- Finished: 2026-10-22

<!--
Status is one of: in-progress, blocked, done.
  blocked  -> at least one [open] item under "Questions and blockers".
  done     -> the Done-when check passed and is recorded under Verification, Finished has a date,
              and Decisions, Approvals, Deviations and Deferred each say something (or "None").
HTML comments like this one are ignored by plan.py check. Leave them or delete them.
-->

## Card

<!-- Copied from docs/PLAN.md when this log was created. Do not edit; the plan is the source. -->
No task card. Approved by Amirishetty Sai Vignesh on 2026-10-22 ("2 then 3, go ahead"): the
TODO item from M3-6, a Release build failed with warnings as errors; fix or silence each
warning, and add a Release build to CI.

## Sessions

<!--
One entry per working session, newest last. Write it as you go, not at the end:
another agent (or you, after a context reset) resumes from this log alone.
Each entry: what you did, what state the code is in, the commands you ran and what they printed,
and the next step.
-->

### 2026-10-22 — session 1

- `cmake -B build/rel-gcc -DCMAKE_BUILD_TYPE=Release` (gcc 13) and `cmake --build -- -k 0`:
  about 40 sites. Two are `-Wunused-result` (`read` in `src/pmd/pmd.cpp`, `write` in
  `tests/unit/net_test.cpp`: GCC does not accept a cast to void). The rest are
  `-Wnull-dereference`, several pointing at plain struct definitions (`NodeId` in
  `src/platform/ids.h:25`, `HelloPayload` in `src/wire/payloads.h:141`): inlined library code.
  GCC runs that check only with optimisation, so the Debug builds of CI never ran it. One
  report is a real unchecked `calls_.find` in `Tasks::done` (`src/rt/tasks.cpp`), safe by
  the code's invariant but not checked.
- clang 18 Release: builds with no warning.
- Fixes: the two results kept in `[[maybe_unused]]` variables; `Tasks::done` ends the job if
  the call is gone; `cmake/ParameshModule.cmake` adds `-Wno-null-dereference` for GCC in
  builds other than Debug, with the reason. `tools/bench/matmul.sh` no longer turns
  warnings-as-errors off for its Release build. `.github/workflows/ci.yml`: two more
  build-and-test jobs, gcc and clang Release with no sanitizer; the existing jobs keep their
  names.
- Release, gcc: builds, `ctest` 16 of 16 (72.8 s). Release, clang: builds, 16 of 16 (65.3 s).
  clang-format and `cmake --build --preset tidy`: clean.
- Debug matrix: seven builds green; `gcc address` printed "Terminated" with `pmd_test` still
  running, a SIGTERM from outside (another session's matrix ran beside it, and a ctest
  timeout prints "Timeout" instead). Run again alone: 16 of 16 (1,335 s). Next: the plan check
  and the pull request.

## Files touched

<!--
Every file this task's branch changes, one bullet each, starting with the path in backticks.
A directory entry ends with "/" and covers everything under it. plan.py check --base fails
if the branch changes a file not covered here.
Example:  - `src/net/epoll_loop.cpp` — epoll wrapper and timer wheel
-->
- `cmake/ParameshModule.cmake` — no `-Wnull-dereference` for optimised GCC builds
- `.github/workflows/ci.yml` — Release jobs for gcc and clang
- `src/pmd/pmd.cpp` — the `read` result kept
- `src/rt/tasks.cpp` — `Tasks::done` checks its call
- `tests/unit/net_test.cpp` — the `write` result kept
- `tools/bench/matmul.sh` — warnings are errors in its Release build too
- `docs/TODO.md` — the Release-build row removed
- `docs/logs/X-release-build.md` — this log

## Decisions

<!--
Choices you made inside the card's scope, each with the reason. Not for points listed under
"Still open" in the plan: those are asked, never decided here. Write "None" if there were none.
-->
- `-Wnull-dereference` off for optimised GCC builds, not silenced site by site: about 40
  sites, mostly inside inlined library code, would each need a pragma; the check never ran
  in CI before (Debug), and clang-tidy's `clang-analyzer-core.NullDereference` runs on every
  push. Clang keeps the flag in every build.
- Release jobs without a sanitizer only: the sanitizer jobs already cover those checks.

## Questions and blockers

<!--
Each item starts with [open] or [answered YYYY-MM-DD by <who>], followed by the question and,
once answered, the answer. M0 drafting tasks may also use [for gate] for a Still-open point
drafted for the person to decide at the M0 gate. Write "None" if there are none.
-->
None

## Approvals

<!--
Approvals from the person for anything the rules reserve to them, such as a change to a frozen
file (include/paramesh.h, docs/PROTOCOL.md, docs/STATE_MACHINES.md, docs/INTERNAL_API.md) that
this card does not list. Name the file, who approved, and the date. "None" if there are none.
-->
- The work itself: Amirishetty Sai Vignesh, 2026-10-22 ("2 then 3, go ahead"). No frozen file
  changes.

## Deviations

<!--
Anything that differs from the card: a file outside its Files list, a changed approach, a Done-when
check run differently. Each with the reason and who agreed. "None" if the card was followed exactly.
-->
- `cmake/ParameshModule.cmake` is a shared build file, which AGENTS.md asks tasks not to edit;
  the warning flags live only there, and this task is about them.

## Verification

<!--
The card's Done-when check: the exact commands run, where (local, Actions, simulated nodes,
cabled laptops), and the actual result, pasted or summarised with numbers. Also the quality gate
(clang-format, clang-tidy, sanitizer runs). Required before Status: done.
-->
No card, so no Done-when check of its own. On this machine (Ubuntu 24.04, gcc 13, clang 18):

- Release builds with warnings as errors: gcc builds clean and `ctest` 16/16; clang builds
  clean and 16/16. Before this task the gcc Release build failed at about 40 sites.
- Debug matrix, `build/nc-<compiler>-<sanitizer>`: gcc none 16/16, address 16/16 (run again
  alone, above), undefined 16/16, thread 15/15; clang none 16/16, address 16/16, undefined
  16/16, thread 15/15 (`app.m0_demo` is not registered under ThreadSanitizer, as on `main`).
- `cmake --build --preset tidy`: no finding. clang-format `--dry-run --Werror`: clean.
- The two Release jobs in `.github/workflows/ci.yml` run in Actions on this pull request.

## Deferred

<!--
Anything left undone, each also added to docs/TODO.md in this same pull request. "None" if nothing.
-->
None.
