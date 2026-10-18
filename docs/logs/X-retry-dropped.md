# X-retry-dropped: work outside the task cards

- Task: X-retry-dropped
- Milestone: none
- Stream: -
- Status: done
- Branch: x-retry-dropped
- Started: 2026-10-18
- Finished: 2026-10-18

<!--
Status is one of: in-progress, blocked, done.
  blocked  -> at least one [open] item under "Questions and blockers".
  done     -> the Done-when check passed and is recorded under Verification, Finished has a date,
              and Decisions, Approvals, Deviations and Deferred each say something (or "None").
HTML comments like this one are ignored by plan.py check. Leave them or delete them.
-->

## Card

<!-- Copied from docs/PLAN.md when this log was created. Do not edit; the plan is the source. -->
No task card. Approved by Amirishetty Sai Vignesh on 2026-10-18 ("Fix it now") because the M6-5
impairment matrix found a job abort at 50 ms delay and 5% loss: the home machine met an
impossible event.

## Sessions

<!--
One entry per working session, newest last. Write it as you go, not at the end:
another agent (or you, after a context reset) resumes from this log alone.
Each entry: what you did, what state the code is in, the commands you ran and what they printed,
and the next step.
-->

### 2026-10-18 — session 1

- Started the task. Read AGENTS.md, `docs/PROTOCOL.md` section 8 and the transport.
- Cause: a reply that takes over 2 s makes the requester's transport send the request again with
  `RETRY` (`epoll_transport.cpp`, housekeeping). PROTOCOL.md section 8 says the receiver drops
  such a frame, since TCP already delivered the first; nothing did, so the home ran the request
  a second time and met an impossible event.
- Fix: `Job::on_frame` drops every frame with `RETRY`. Test-only setting
  `PARAMESH_TEST_REPLY_DELAY_MS` makes each home send its first page to another node that much
  later; lib_test "a page sent after the reply timeout: the request sent again with RETRY is
  dropped" runs the M1 program with 2,500 ms.
- Without the drop the test fails with the very abort the matrix saw:
  `job 42 aborted: the home machine met an impossible event: page 0, event 1 from node 1, state 2,
  waiting for 0, segment state 1`. With it the test passes (8.5 s, 14 assertions).
- Matrix with that alone: 7 of 8 builds green; clang undefined failed pmd_test "a fourth node
  joins" once (node 3 lost, then "no reply from node 1"); it passed 3 of 3 when run alone.
- The impairment matrix with that alone then aborted most runs from 1 ms delay on: "no reply
  from node 1". A debug print showed the dropped RETRYs were TASK_REQs (0x50) to the launcher.
  Cause, older than this task: the launcher drops a TASK_REQ that comes before its task queue
  exists (`handle_task`, `tasks()` null); until now the RETRY 2 s later rescued it. The
  pmd_test failure above has the same cause.
- Fix: while it has no queue, the launcher answers TASK_REQ with NO_TASK reason 1 ("empty for
  now: ask again after task.no_task_backoff", PROTOCOL.md). An atomic `tasks_up_`, set after
  `tasks_.emplace`, tells the network thread the queue is there; `tasks_` was read there
  without one while the main thread made it. Test-only setting `PARAMESH_TEST_TASKS_DELAY_MS`
  makes the launcher start its queue that much later; lib_test "TASK_REQs that come before the
  launcher has its task queue are answered NO_TASK" uses 2,500 ms, so the RETRY copies come
  before it too. Without the NO_TASK answer it fails: `job 42 aborted: no reply from node 1`;
  with it, it passes.
- `tools/bench/evaluate.sh impairment` again, all 44 settings: 0 aborts, empty aborts.log,
  30 min 18 s (before: 1 abort in 44, the impossible event at 50 ms / 5% loss). Rows: 0 ms
  0.052 s; 5 ms 2.2-7.9 s; 25 ms 10.1-18.3 s; 50 ms 20.1-33.4 s; 856 faults in every run.
- Matrix after both fixes: 8 of 8 builds green. Tidy found one unchecked optional in
  `handle_task`; fixed, tidy clean, lib_test 17 of 17.
- Next: PR.

## Files touched

<!--
Every file this task's branch changes, one bullet each, starting with the path in backticks.
A directory entry ends with "/" and covers everything under it. plan.py check --base fails
if the branch changes a file not covered here.
Example:  - `src/net/epoll_loop.cpp` — epoll wrapper and timer wheel
-->
- `src/lib/job.cpp` — drop frames with RETRY; NO_TASK before the launcher's queue; test delays
- `tests/unit/lib_test.cpp` — tests of a request sent again with RETRY and an early TASK_REQ
- `docs/TODO.md` — the impossible-event entry removed: fixed here
- `docs/logs/X-retry-dropped.md` — this log

## Decisions

<!--
Choices you made inside the card's scope, each with the reason. Not for points listed under
"Still open" in the plan: those are asked, never decided here. Write "None" if there were none.
-->
- The drop is in the job, not the transport: the transport test "a timed request is sent again
  with RETRY" checks the retry reaches the other side, and the job is the receiver the protocol
  means. Every frame the job gets with RETRY is dropped, requests and timed home messages alike.
- No change to docs/STATE_MACHINES.md: the home machine was right to call a second request
  impossible; the transport layer above it must not deliver one.
- NO_TASK reason 1 for a TASK_REQ before the queue, not a queue of early requests: it is the
  protocol's own answer for "nothing now" and the worker already backs off and asks again.

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
None

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
- No card, so no Done-when. The check is the M6-5 finding itself: `tools/bench/evaluate.sh
  impairment` (three namespaced nodes, matmul 256): 44 of 44 settings finished, 0 aborts,
  empty aborts.log.
- New tests, each failing without its fix (local, gcc debug):
  "a page sent after the reply timeout: the request sent again with RETRY is dropped" fails with
  the impossible event of the matrix; "TASK_REQs that come before the launcher has its task
  queue are answered NO_TASK" fails with "no reply from node 1". Both pass with the fixes.
- Matrix, local: gcc and clang, each with no sanitizer, address, undefined and thread:
  `100% tests passed` in all eight (16 programs; 15 under thread).
- clang-format clean; `cmake --build --preset tidy` clean.

## Deferred

<!--
Anything left undone, each also added to docs/TODO.md in this same pull request. "None" if nothing.
-->
None
