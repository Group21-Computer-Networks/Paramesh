# X-m0-freeze: mark the four interface files approved and frozen

- Task: X-m0-freeze
- Milestone: none
- Stream: -
- Status: done
- Branch: m0-gate
- Started: 2026-10-07
- Finished: 2026-10-07

<!--
Status is one of: in-progress, blocked, done.
  blocked  -> at least one [open] item under "Questions and blockers".
  done     -> the Done-when check passed and is recorded under Verification, Finished has a date,
              and Decisions, Approvals, Deviations and Deferred each say something (or "None").
HTML comments like this one are ignored by plan.py check. Leave them or delete them.
-->

## Card

<!-- Copied from docs/PLAN.md when this log was created. Do not edit; the plan is the source. -->
No task card. Approved by Amirishetty Sai Vignesh on 2026-10-07: closing the M0 gate means the four interface files
must say they are approved, and the one answer without a home (G13) must be written down.

## Sessions

<!--
One entry per working session, newest last. Write it as you go, not at the end:
another agent (or you, after a context reset) resumes from this log alone.
Each entry: what you did, what state the code is in, the commands you ran and what they printed,
and the next step.
-->

### 2026-10-07 — session 1

- The person closed the M0 gate in chat: all four files approved, no exceptions to the drafted
  answers, G13 = abort the job, the five other laptops and the demo viewing waived.
- Changed the status line of each of the four files from "DRAFT" to "approved and frozen", added
  G13's answer to `pm_lock` in `include/paramesh.h`, and added the waived hardware runs to
  `docs/TODO.md`. The `[GATE ...]` tags were left in place as the record.
- `cmake --build --preset dev && ctest --preset dev` → 6/6 pass; format clean.

## Files touched

<!--
Every file this task's branch changes, one bullet each, starting with the path in backticks.
A directory entry ends with "/" and covers everything under it. plan.py check --base fails
if the branch changes a file not covered here.
Example:  - `src/net/epoll_loop.cpp` — epoll wrapper and timer wheel
-->
- `include/paramesh.h` — status line; one sentence at `pm_lock` (G13)
- `docs/PROTOCOL.md` — status line
- `docs/STATE_MACHINES.md` — status line
- `docs/INTERNAL_API.md` — status line
- `docs/TODO.md` — the waived laptop runs

## Decisions

<!--
Choices you made inside the card's scope, each with the reason. Not for points listed under
"Still open" in the plan: those are asked, never decided here. Write "None" if there were none.
-->
- Left the `[GATE ...]` tags in the files and changed only the status lines: fewer edits to
  frozen files, and the tags show what was a proposal.

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
- `include/paramesh.h`, `docs/PROTOCOL.md`, `docs/STATE_MACHINES.md`, `docs/INTERNAL_API.md`:
  approved as drafted, and this change to them approved, by Amirishetty Sai Vignesh on 2026-10-07.

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
Local: comment and Markdown changes only. `cmake --build --preset dev && ctest --preset dev` →
6/6 pass; `clang-format --dry-run --Werror` clean; `plan.py check` with the PR title passes and
`plan.py status` shows the M0 gate closed and M1-1 ready.

## Deferred

<!--
Anything left undone, each also added to docs/TODO.md in this same pull request. "None" if nothing.
-->
- `tools/check_env.sh` on the other five laptops, and the person seeing the demo: waived at the
  gate, added to `docs/TODO.md`.
