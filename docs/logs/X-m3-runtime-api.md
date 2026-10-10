# X-m3-runtime-api: work outside the task cards

- Task: X-m3-runtime-api
- Milestone: none
- Stream: -
- Status: done
- Branch: x-m3-runtime-api
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
No task card. Approved by Amirishetty Sai Vignesh on 2026-10-10, at the M3 gate, because the frozen
`src/rt/runtime.h` describes one `Runtime` class behind `rt_open()` that was never built.

- Goal: `src/rt/runtime.h` and section 8 of `docs/INTERNAL_API.md` describe the code as it is.
  No behaviour changes.
- Files: `src/rt/runtime.h`, `docs/INTERNAL_API.md`, `tests/unit/internal_api_test.cpp`
- Done when: the header and the document agree, nothing in them names a thing that does not
  exist, and every test still passes.

## Sessions

<!--
One entry per working session, newest last. Write it as you go, not at the end:
another agent (or you, after a context reset) resumes from this log alone.
Each entry: what you did, what state the code is in, the commands you ran and what they printed,
and the next step.
-->

### 2026-10-10 — session 1

- `src/rt/runtime.h`: removed the `Runtime` class, `RuntimeConfig` and `rt_open()`, and the
  two includes only they needed. Kept as they were: `RuntimeHost`, `ParallelFor`,
  `rt_register_task`, `rt_task_id`, `rt_check_registry`. The header comment now names the
  pieces the runtime is made of.
- `docs/INTERNAL_API.md`, section 8: the same declarations, a paragraph marked as changed at
  the M3 gate, and a table of the four headers the job process uses. In the list of function
  name prefixes, `rt_open` became `rt_task_id`.
- `tests/unit/internal_api_test.cpp`: one `static_assert` named the removed class.
- `docs/TODO.md`: the row about the unbuilt class is gone; one row added (see Deferred).
- State: done.

## Files touched

<!--
Every file this task's branch changes, one bullet each, starting with the path in backticks.
A directory entry ends with "/" and covers everything under it. plan.py check --base fails
if the branch changes a file not covered here.
Example:  - `src/net/epoll_loop.cpp` — epoll wrapper and timer wheel
-->
- `src/rt/runtime.h` — without the class that was never built (frozen; approved)
- `docs/INTERNAL_API.md` — section 8 brought in line (frozen; approved)
- `tests/unit/internal_api_test.cpp` — one assertion
- `docs/TODO.md` — one row done, one added

## Decisions

<!--
Choices you made inside the card's scope, each with the reason. Not for points listed under
"Still open" in the plan: those are asked, never decided here. Write "None" if there were none.
-->
- **The four headers of the pieces are named in the document but not copied into it,** and it
  says they are not frozen. Copying their declarations would freeze `Sync` and `Tasks` just
  before M4 and M5 add to them (a changing quota, chunks put back in the queue).
- **`RuntimeHost` is unchanged,** including `chunk_finished`, which nothing uses before the
  ledger (M5).
- **Only section 8 was touched.** Two other places where the document is behind the code are
  outside what was approved; see Deferred.

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
- `src/rt/runtime.h` and `docs/INTERNAL_API.md`, both frozen: approved by Amirishetty Sai Vignesh in chat
  on 2026-10-10, at the M3 gate, in answer to "what should happen to the frozen
  `rt/runtime.h`?": "Trim it to what exists". Recorded in `docs/logs/M3-gate.md`.

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
Local, 2026-10-10.

- The declarations in section 8 of the document, from `class RuntimeHost` to
  `rt_check_registry`, are the header's, line for line (`diff` of the two: no difference).
- `grep` for `rt_open`, `RuntimeConfig` and `class Runtime` in `src/`, `tests/` and
  `docs/INTERNAL_API.md`: only the paragraph that says they are gone.
- `ctest --preset dev`: 16 of 16 pass. The gcc build and the clang build with clang-tidy both
  compile with no warning; clang-format clean.
- Not run locally: the six sanitizer builds. Nothing that runs changed; Actions runs them on
  the pull request.

## Deferred

<!--
Anything left undone, each also added to docs/TODO.md in this same pull request. "None" if nothing.
-->
- `docs/INTERNAL_API.md` is behind the code in two more places, both from M3-7:
  `src/net/frame_io.h` is not in section 7, and the dependency table gives `tools` no `net`.
  In `docs/TODO.md` for the person to approve or leave.
