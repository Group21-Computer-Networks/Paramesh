# AT-1: Hold window at the home

- Task: AT-1
- Milestone: M3
- Stream: seq
- Status: done
- Branch: at-1-hold
- Started: 2026-10-10
- Finished: 2026-10-10

<!--
Status is one of: in-progress, blocked, done.
  blocked  -> at least one [open] item under "Questions and blockers".
  done     -> the Done-when check passed and is recorded under Verification, Finished has a date,
              and Decisions, Approvals, Deviations and Deferred each say something (or "None").
HTML comments like this one are ignored by plan.py check. Leave them or delete them.
-->

## Card

<!-- Copied from docs/PLAN.md when this log was created. Do not edit; the plan is the source. -->
- Goal: Hold window at the home: count ownership transfers per page; above the threshold, hold the current writer's grant for a window that doubles while the fight continues and resets when it stops; report the page and the two tasks in the log. The false-sharing demo: two tasks writing neighbouring integers.
- Files: `src/coh/` (home files), `apps/falseshare`, `tests/unit/`
- Needs: M3 gate
- Stream: seq
- Done when: The demo is slow without the hold window and recovers with it; the report names the page and both tasks; the M2 fuzz run still passes.

## Sessions

<!--
One entry per working session, newest last. Write it as you go, not at the end:
another agent (or you, after a context reset) resumes from this log alone.
Each entry: what you did, what state the code is in, the commands you ran and what they printed,
and the next step.
-->

### 2026-10-10 — session 1

- Started the task. Read the card, the milestone's Streams and Gate text, and AGENTS.md.
- The card's check needs the job process to carry out two new home actions, and `src/lib/`
  is not on the card. Asked the person (see Questions).
- Next: the hold window in the home machine with unit tests; the two actions and the
  tunables in `src/lib/job.cpp`; the demo; a three-process test.

### 2026-10-10 — session 2: the window, the wiring, the demo

- `src/coh/home_machine.cpp`, section 2.6 of `docs/STATE_MACHINES.md`:
  - every grant of write access to a node other than the last writer is counted, in periods
    of `thrash.threshold`'s length; above the count the page is thrashing: the hold starts at
    `thrash.hold_initial` and doubles up to `thrash.hold_max` with every further transfer in
    the period, and one `kReportThrash` names the page and the two nodes;
  - while the window runs and the page is exclusive, a `WRITE_REQ`, an `UPGRADE_REQ` and an
    `ATOMIC_OP` from another node stay in the queue, in order; readers and the owner are
    served ahead of them; one `kArmHoldTimer` asks for `HOLD_EXPIRED` at the window's end;
  - the window is over at its end time or as soon as the page is no longer exclusive.
  Every request now goes through the queue, and the home takes "the first one the window
  does not hold back", which is the document's rule for the next request.
- `src/lib/job.cpp`: the home gets the plan's tunables (8 transfers in 100 ms; hold 1 ms,
  doubling to 64 ms); `PARAMESH_CFG_THRASH_THRESHOLD` sets the count and 0 turns the window
  off. `kArmHoldTimer` starts a transport timer whose firing delivers `HOLD_EXPIRED`;
  `kReportThrash` is logged as `page_thrash`. The launcher logs `task_on_node` the first
  time a node finishes a chunk of a task.
- `apps/falseshare/`: the demo. Chunk i of `write_left` writes number 2i and chunk i of
  `write_right` number 2i + 1, two million times each; all the numbers are in one page.
- Tests: five unit cases for the window; one three-process case that runs the same program
  with the window on and off.
- One wrong expectation of mine in a unit case (the owner's own atomic operation is not
  held, as the table says); corrected in the test.
- State: Done-when met.
## Files touched

<!--
Every file this task's branch changes, one bullet each, starting with the path in backticks.
A directory entry ends with "/" and covers everything under it. plan.py check --base fails
if the branch changes a file not covered here.
Example:  - `src/net/epoll_loop.cpp` — epoll wrapper and timer wheel
-->
- `src/coh/home_machine.cpp` — the hold window
- `apps/falseshare/` — the false-sharing demo and its build file
- `src/lib/job.cpp` — the hold timer, the two log lines, the thrash tunables (approved)
- `tests/unit/coh_home_test.cpp` — the window's rows and its counting
- `tests/unit/lib_test.cpp` — three processes, window on and off (approved)
- `docs/TODO.md` — one item

## Decisions

<!--
Choices you made inside the card's scope, each with the reason. Not for points listed under
"Still open" in the plan: those are asked, never decided here. Write "None" if there were none.
-->
- **"Both tasks" in the report are named through the nodes,** as the M0 gate approved
  ([GATE S6]): a page request carries no task, so the home reports the page and the two nodes
  (`page_thrash`), and the launcher's log says which tasks ran on which node
  (`task_on_node`). When both tasks ran on both nodes, as in the demo, the log shows all four.
- **Transfers are counted in periods laid end to end,** not in a window that slides. Marked
  `ponytail:`. A new period starts the count at 1, which is below any threshold, so the hold
  resets there: this is the document's "resets when it stops".
- **One report for each fight:** when the hold goes from nothing to its initial value, not at
  every doubling.
- **The report is logged at `info`.** The job process logs at `warn` by default (a row in
  `docs/TODO.md` since M3-3), so the lines show with `PARAMESH_CFG_LOG_LEVEL=info`.
- **`PARAMESH_CFG_THRASH_THRESHOLD=0` means off.** The demo has to be run without the window
  to show what it is worth. The period and the two hold values are the plan's defaults and
  are not read from the environment yet (the same row).
- **The fuzz harness does not exercise the window:** it has no clock. The card asks only that
  the M2 fuzz run still passes, which it does with the window's code in place.
- **The window is on by default.** Measured on the programs that pass one page around under a
  lock: no real cost (table under Verification).

## Questions and blockers

<!--
Each item starts with [open] or [answered YYYY-MM-DD by <who>], followed by the question and,
once answered, the answer. M0 drafting tasks may also use [for gate] for a Still-open point
drafted for the person to decide at the M0 gate. Write "None" if there are none.
-->
- [answered 2026-10-10 by Amirishetty Sai Vignesh] The check ("the demo is slow without the hold window
  and recovers with it") needs `src/lib/` to carry out the hold timer and the thrash report,
  and the card does not list it. May AT-1 touch `src/lib/`? Answer: yes, AT-1 wires its own
  part.

## Approvals

<!--
Approvals from the person for anything the rules reserve to them, such as a change to a frozen
file (include/paramesh.h, docs/PROTOCOL.md, docs/STATE_MACHINES.md, docs/INTERNAL_API.md) that
this card does not list. Name the file, who approved, and the date. "None" if there are none.
-->
- `src/lib/job.cpp` and `tests/unit/lib_test.cpp`: approved by Amirishetty Sai Vignesh in chat, 2026-10-10.
  Not frozen files; the approval is for files the card does not list.

## Deviations

<!--
Anything that differs from the card: a file outside its Files list, a changed approach, a Done-when
check run differently. Each with the reason and who agreed. "None" if the card was followed exactly.
-->
- The branch changes `src/lib/job.cpp`, which the card does not list. Approved; see Approvals.
- The report names the two nodes, and the tasks through a second log line, not the two tasks
  in one line: the M0 gate's answer to [GATE S6].

## Verification

<!--
The card's Done-when check: the exact commands run, where (local, Actions, simulated nodes,
cabled laptops), and the actual result, pasted or summarised with numbers. Also the quality gate
(clang-format, clang-tidy, sanitizer runs). Required before Status: done.
-->
Local, 2026-10-10, host `ouroboros` (16 processors).

**Done-when 1: the demo is slow without the hold window and recovers with it.**
`tests/multi/run_local.sh build/dev/apps/falseshare/falseshare`, three nodes, 32 numbers in
one page, two million writes each; the second column with `PARAMESH_CFG_THRASH_THRESHOLD=0`:

| | With the hold window | Without |
| --- | --- | --- |
| Time, three runs | 0.151 s, 0.097 s, 0.091 s | 0.456 s, 0.499 s, 0.269 s |
| One more run with the log on: time | 0.152 s | 0.876 s |
| The same run: page transfers, the three processes together | 145 | 7,611 |
| Result | every number 2,000,000 | every number 2,000,000 |

About four times faster, with about fifty times fewer page transfers.

**Done-when 2: the report names the page and both tasks.** From the run with
`PARAMESH_CFG_LOG_LEVEL=info`, with the window on:

```text
{"event":"page_thrash","fields":{"node_a":1,"node_b":2,"page":0},"job":1,"level":"info","node":2,...}
{"on":1,"task":"write_left"} {"on":1,"task":"write_right"} {"on":2,"task":"write_left"} {"on":2,"task":"write_right"}
```

The first line is the home's (node 2 is page 0's home); the second is the `fields` of the
launcher's four `task_on_node` lines. With the window off there is no `page_thrash` line.

**Done-when 3: the M2 fuzz run still passes.** `sim_fuzz --schedules 100000`:
`100000 schedules passed`.

Tests:

- `unit_coh_home_test`, 21 cases (5 new), 520 assertions: off by default; held after too many
  transfers, with the report, the timer, the doubling to the most, and the queue's order;
  readers and the owner not held, and a reader ends the window; a timer that fires early is
  asked for again and a quiet period ends the hold; one node writing again and again is no
  transfer. With `held_back` made to return false, 15 assertions fail.
- `unit_lib_test`, "the hold window: neighbours in one page cost far fewer page transfers
  with it": three processes, window on and off. Six runs: 17 to 23 transfers with it, 1,165
  to 3,099 without; the `page_thrash` line and both tasks' names are in the log with it and
  no `page_thrash` line without.

**The window is on by default; what it costs programs that are not thrashing by mistake.**
The counter passes one page from node to node under a lock, 3,000 times:

| | Window on | Window off |
| --- | --- | --- |
| `run_local.sh counter 3000`, two measurements | 2.97 s, 2.73 s | 2.73 s, 2.72 s |
| `unit_lib_test`, the M2 write-sharing case | 3.01 s | 2.91 s |

`run_pmrun.sh falseshare` passes too.

Quality gate: all eight compiler and sanitizer builds pass, 16/16 tests (15/15 under TSan), no
warnings; clang-tidy exit 0; clang-format clean.

## Deferred

<!--
Anything left undone, each also added to docs/TODO.md in this same pull request. "None" if nothing.
-->
- The fuzz harness has no clock and so never holds a request back. In `docs/TODO.md`.
