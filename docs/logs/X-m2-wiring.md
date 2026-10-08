# X-m2-wiring: work outside the task cards

- Task: X-m2-wiring
- Milestone: none
- Stream: -
- Status: done
- Branch: x-m2-wiring
- Started: 2026-10-09
- Finished: 2026-10-09

<!--
Status is one of: in-progress, blocked, done.
  blocked  -> at least one [open] item under "Questions and blockers".
  done     -> the Done-when check passed and is recorded under Verification, Finished has a date,
              and Decisions, Approvals, Deviations and Deferred each say something (or "None").
HTML comments like this one are ignored by plan.py check. Leave them or delete them.
-->

## Card

<!-- Copied from docs/PLAN.md when this log was created. Do not edit; the plan is the source. -->
No task card. Approved by Amirishetty Sai Vignesh on 2026-10-09 because M2-1 to M2-4 built the two state
machines, the write side of the memory engine and the lock manager, and no M2 card lists
`src/lib/`, where they have to be connected to a running job.

- Goal: `src/lib/job.cpp` carries out the M2 page actions of the node and home machines
  (upgrade, `INV`, `FETCH_INV`), and the C API gets `pm_lock_create`, `pm_lock`, `pm_unlock`,
  `pm_barrier_create` and `pm_barrier_wait`, through `Sync`.
- Files: `src/lib/`, `tests/unit/lib_test.cpp`
- Done when: three processes on localhost write the same shared page and lose no update, with
  and without a lock.
- Not in it: `pm_atomic_add` (M2-5), `pm_wait_all` (M3-2), eviction and spill (M4).

## Sessions

<!--
One entry per working session, newest last. Write it as you go, not at the end:
another agent (or you, after a context reset) resumes from this log alone.
Each entry: what you did, what state the code is in, the commands you ran and what they printed,
and the next step.
-->

### 2026-10-09 — session 1: the wiring and its test

- Asked the person which task connects M2-1 to M2-4 to a running job; the answer is this
  task (see Approvals).
- `src/lib/job.cpp`:
  - Frames: `UPGRADE_REQ` and `INV_ACK` go to the home machine; `UPGRADE_GRANT`, `INV` and
    `FETCH_INV` to the node machine; the five lock and barrier opcodes to `Sync`.
  - Node actions: allow writes (`write_protect` off), discard (`zap`), and a reply without a
    page (`INV_ACK` is the page ID alone).
  - Home actions: a reply without bytes (`UPGRADE_GRANT`). `INV` and `FETCH_INV` needed
    nothing new: they are sends like `FETCH`.
  - `Job` is now the `RuntimeHost` of `src/rt/` and owns a `Sync`.
  - The C API gets `pm_lock_create`, `pm_lock`, `pm_unlock`, `pm_barrier_create` and
    `pm_barrier_wait`. Each thread the test hook starts gets the next thread index; the
    launcher's `main()` is `0xFFFF`.
- `tests/unit/lib_test.cpp`: the M2 program, run by three processes. `start_job` now takes the
  program to run.
- It passed on the first run and on every repeat; no fault in M2-1 to M2-4 came to light.
- A slip in committing: the first commit on the branch is titled "the implementation log" but
  also holds `job.cpp` and the test, because they were already staged. Left as it is rather
  than rewrite a pushed branch.
- State: Done-when met.

## Files touched

<!--
Every file this task's branch changes, one bullet each, starting with the path in backticks.
A directory entry ends with "/" and covers everything under it. plan.py check --base fails
if the branch changes a file not covered here.
Example:  - `src/net/epoll_loop.cpp` — epoll wrapper and timer wheel
-->
- `src/lib/job.cpp` — the M2 page actions, `Sync`, and the lock and barrier functions of the C API
- `tests/unit/lib_test.cpp` — three processes write one shared page
- `docs/TODO.md` — the row M2-4 added, cut down to what is still open

## Decisions

<!--
Choices you made inside the card's scope, each with the reason. Not for points listed under
"Still open" in the plan: those are asked, never decided here. Write "None" if there were none.
-->
- **Only what M2 can raise is wired.** `WRITEBACK`, `BUSY_RETRY`, `REDIRECT`, the resend timer
  and the load of a spilled page have rows in the machines but nothing raises them before M4;
  a job that met one would end with "does not handle yet". `kApplyAtomic` is M2-5.
- **An abort asked for by `src/rt/` on the network thread exits at once,** without `JOB_END`:
  that thread cannot wait for its own frames to leave, and `RuntimeHost::abort_job` may not
  return. The other processes end the job as "node lost". Every such call reports a peer
  that broke the protocol. From any other thread the usual path is taken. Marked `ponytail:`.
- **Thread indexes** come from a counter per process, one for each thread the test hook
  starts. M3-2's worker threads replace this.
- **The test puts everything in one page** (the lock handle, the counter, each node's own
  number), so the page changes owner under the writers all the time: upgrade, `INV`,
  `FETCH` and `FETCH_INV` all occur in one run.

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
- The task itself: approved by Amirishetty Sai Vignesh in chat, 2026-10-09, as "a separate task first":
  `job.cpp` carries out the M2 page actions and the C API gets `pm_lock`, `pm_unlock` and the
  barrier calls, with tests in `tests/unit/lib_test.cpp`. M2-5 then adds only
  `pm_atomic_add` and may also touch `src/lib/` and `tests/unit/`.

## Deviations

<!--
Anything that differs from the card: a file outside its Files list, a changed approach, a Done-when
check run differently. Each with the reason and who agreed. "None" if the card was followed exactly.
-->
None

## Verification

<!--
The card's Done-when check: the exact commands run, where (local, Actions, simulated nodes,
cabled laptops), and the actual result, pasted or summarised with numbers. Also the quality gate
(clang-format, clang-tidy, sanitizer runs). Required before Status: done.
-->
Local, 2026-10-09.

**Done-when: three processes on localhost write the same shared page and lose no update, with
and without a lock.** `unit_lib_test`, case "three processes write one shared page and lose
no update, with and without a lock": passed, about 2.5 s. In order, the launcher checks:

| Step | Expected | Result |
| --- | --- | --- |
| Before `pm_init()` | `pm_lock_create()` gives id 0; `pm_lock` gives `PM_ERR_STATE` | pass |
| Wrong calls on a worker (ten of them) | what `paramesh.h` says for each | pass |
| Two workers add 1,000 times each under the lock | counter = 2,000 | pass |
| The launcher adds 1,000 times under the lock | counter = 3,000 | pass |
| Each worker adds 1,000 times to its own number in the same page, no lock | 1,000 and 1,000; counter still 3,000 | pass |
| Two workers meet at a barrier of 2 | each sees the other's mark after the wait | pass |
| All three processes | exit 0, nothing on the launcher's stderr | pass |

Repeats: 20 times in the dev build before and 20 after the tidy fixes; 10 times each under
gcc ASan, clang ASan and clang UBSan: 0 failures.

M1 still works: `run_local.sh readshare` and `run_netns.sh -d 5 -l 1 readshare` both PASS.

Quality gate: all eight compiler and sanitizer builds pass, 15/15 tests (14/14 under TSan), no
warnings; clang-tidy exit 0; clang-format clean.

Not checked: `job.cpp` under ThreadSanitizer. The lib tests map the region, which TSan cannot,
so they are skipped there, as before.

## Deferred

<!--
Anything left undone, each also added to docs/TODO.md in this same pull request. "None" if nothing.
-->
- `pm_wait_all` is not connected to `Sync::wait_all`: it belongs with `pm_parallel_for` in
  M3-2. The row M2-4 added to `docs/TODO.md` now says only that.
