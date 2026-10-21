# X-config: Tunables from a key=value file in the state directory

- Task: X-config
- Milestone: none
- Stream: -
- Status: done
- Branch: x-config
- Started: 2026-10-20
- Finished: 2026-10-21

<!--
Status is one of: in-progress, blocked, done.
  blocked  -> at least one [open] item under "Questions and blockers".
  done     -> the Done-when check passed and is recorded under Verification, Finished has a date,
              and Decisions, Approvals, Deviations and Deferred each say something (or "None").
HTML comments like this one are ignored by plan.py check. Leave them or delete them.
-->

## Card

<!-- Copied from docs/PLAN.md when this log was created. Do not edit; the plan is the source. -->
No task card. Approved by Amirishetty Sai Vignesh on 2026-10-21 ("1 then 2, go ahead"), from
the list of open TODO items after X-node-commit: (1) G13, (2) the Configuration rule
("all tunables from a key=value file"; `pmd` read no file, and five tunables were read by
nothing).

## Sessions

<!--
One entry per working session, newest last. Write it as you go, not at the end:
another agent (or you, after a context reset) resumes from this log alone.
Each entry: what you did, what state the code is in, the commands you ran and what they printed,
and the next step.
-->

### 2026-10-20 — session 1

- G13 (item 1) was built in M4-5: `Tasks::return_chunks` ends the job when a chunk to be run
  again belongs to a thread that holds a lock, and `unit.rt_test` "a chunk that would run again
  while its thread holds a lock ends the job" covers it. It is the only place a chunk is put
  back. Only the TODO row was stale; removed here (a TODO-only change is not committed alone).
- Item 2. Read the plan's Configuration rule and Tunables table, PROTOCOL.md section 10
  (`PARAMESH_CFG_<KEY>`), the key=value loader of M0-3. Not read anywhere: `task.no_task_backoff`,
  `task.prefetch`, `task.default_grain`, `thrash.hold_initial`, `thrash.hold_max`. Asked the
  person where the file lives and what wins, and what a `task.default_grain` value means
  (below).
- `platform_load_settings(state_dir)` in `src/platform/factory.{h,cpp}`: reads
  `<state_dir>/paramesh.conf` with the existing loader and sets each key as
  `PARAMESH_CFG_<KEY>`, not overwriting a variable already set. `pmd`'s `main()` calls it before
  anything starts (so its own tunables and its workers' environment come from it) and takes
  `log.level` for its logger; `pmrun` calls it with the socket's directory before it starts the
  launcher. A bad line: `pmd` and `pmrun` exit 2 with the loader's reason.
- `src/lib/job.cpp`: the five keys, in `read_waits_and_grain()`; `Settings` keeps a
  `TasksConfig` that set-up starts from. Backoffs in ms as `coh.busy_retry_backoff`; the
  maximum is never below the first wait.
- Tests: `unit.platform_test`, the file becomes variables, a set one wins, a bad line is
  refused; `unit.pmd_test`, through `pmrun`, matmul 256 runs 12 chunks by default and 26 with
  `task.default_grain = 8` in the file, 12 again with the variable set to 4, and exit 2 on a
  bad file. By hand, the `pmd` binary: one `pmd_start` line with no file, none with
  `log.level = warn` in the file, and "pmd: cannot parse configuration: a line is not 'key =
  value'", exit 2, with a bad line.
- clang-tidy: `read_settings` passed the complexity limit (the five keys moved into
  `read_waits_and_grain`), and the test made strings from `getenv` unchecked; fixed, clean.
- Matrix: all eight builds green (below). Next: the plan check and the pull request.

## Files touched

<!--
Every file this task's branch changes, one bullet each, starting with the path in backticks.
A directory entry ends with "/" and covers everything under it. plan.py check --base fails
if the branch changes a file not covered here.
Example:  - `src/net/epoll_loop.cpp` — epoll wrapper and timer wheel
-->
- `src/platform/factory.h` — `platform_load_settings`
- `src/platform/factory.cpp` — the same
- `src/pmd/main.cpp` — loads the file; `log.level` for the daemon's logger
- `src/tools/pmrun.cpp` — loads the file for the launcher
- `src/lib/job.cpp` — reads the task and hold tunables
- `tests/unit/platform_test.cpp` — the loader
- `tests/unit/pmd_test.cpp` — through `pmrun`
- `docs/TODO.md` — the G13, task tunables and minimal `pmd` rows
- `docs/logs/X-config.md` — this log

## Decisions

<!--
Choices you made inside the card's scope, each with the reason. Not for points listed under
"Still open" in the plan: those are asked, never decided here. Write "None" if there were none.
-->
- The loader sits beside `platform_config_loader()` in the factory: `pmd` and `tools` both
  link `platform`, and the factory is outside INTERNAL_API.md (section 2), so no frozen file
  changes.
- `pmrun` takes the state directory from its socket's path, so `PARAMESH_PMD_SOCKET` and
  `--state-dir` both lead to the right file.
- `pmd`'s `log.level` accepts `warn` too; the job processes keep their existing parsing.

## Questions and blockers

<!--
Each item starts with [open] or [answered YYYY-MM-DD by <who>], followed by the question and,
once answered, the answer. M0 drafting tasks may also use [for gate] for a Still-open point
drafted for the person to decide at the M0 gate. Write "None" if there are none.
-->
- [answered 2026-10-21 by Amirishetty Sai Vignesh] Where does the file live, and what wins when
  a setting is given twice? Answer: `paramesh.conf` in the state directory, read by `pmd` and
  `pmrun`; a variable already set wins.
- [answered 2026-10-21 by Amirishetty Sai Vignesh] What does a `task.default_grain` value mean:
  chunks per slot (the 4 in the plan's formula), or a fixed chunk size? Answer: chunks per
  slot.

## Approvals

<!--
Approvals from the person for anything the rules reserve to them, such as a change to a frozen
file (include/paramesh.h, docs/PROTOCOL.md, docs/STATE_MACHINES.md, docs/INTERNAL_API.md) that
this card does not list. Name the file, who approved, and the date. "None" if there are none.
-->
- The work itself, outside any card: Amirishetty Sai Vignesh, 2026-10-21 ("1 then 2, go
  ahead"). No frozen file changes.

## Deviations

<!--
Anything that differs from the card: a file outside its Files list, a changed approach, a Done-when
check run differently. Each with the reason and who agreed. "None" if the card was followed exactly.
-->
None.

## Verification

<!--
The card's Done-when check: the exact commands run, where (local, Actions, simulated nodes,
cabled laptops), and the actual result, pasted or summarised with numbers. Also the quality gate
(clang-format, clang-tidy, sanitizer runs). Required before Status: done.
-->
No card, so no Done-when check of its own. On this machine (Ubuntu 24.04, gcc 13, clang 18):

- Matrix, `build/nc-<compiler>-<sanitizer>`, Debug, `ctest -j2`: gcc none 16/16, address
  16/16, undefined 16/16, thread 15/15; clang none 16/16, address 16/16, undefined 16/16,
  thread 15/15 (`app.m0_demo` is not registered under ThreadSanitizer, as on `main`).
- The `pmrun` test fails without the change: `pmrun` read no file and the job read no
  `PARAMESH_CFG_TASK_DEFAULT_GRAIN`, so both runs would cut 12 chunks, and a bad file would
  not stop `pmrun`.
- The `pmd` binary by hand, as in the session above.
- `cmake --build --preset tidy`: no finding. clang-format `--dry-run --Werror`: clean.

## Deferred

<!--
Anything left undone, each also added to docs/TODO.md in this same pull request. "None" if nothing.
-->
None.
