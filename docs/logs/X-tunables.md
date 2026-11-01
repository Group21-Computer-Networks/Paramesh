# X-tunables: the plan's Tunables table completed

- Task: X-tunables
- Milestone: none
- Stream: -
- Status: done
- Branch: x-tunables
- Started: 2026-11-01
- Finished: 2026-11-01

<!--
Status is one of: in-progress, blocked, done.
  blocked  -> at least one [open] item under "Questions and blockers".
  done     -> the Done-when check passed and is recorded under Verification, Finished has a date,
              and Decisions, Approvals, Deviations and Deferred each say something (or "None").
HTML comments like this one are ignored by plan.py check. Leave them or delete them.
-->

## Card

<!-- Copied from docs/PLAN.md when this log was created. Do not edit; the plan is the source. -->
No task card. The person asked in chat on 2026-11-01 to complete the Tunables table in `docs/PLAN.md`, a file only they edit.

## Sessions

<!--
One entry per working session, newest last. Write it as you go, not at the end:
another agent (or you, after a context reset) resumes from this log alone.
Each entry: what you did, what state the code is in, the commands you ran and what they printed,
and the next step.
-->

### 2026-11-01 — session 1

- Compared every `PARAMESH_CFG_*` key in `src/` and `tools/` with the table: 14 were missing,
  the 13 TODO.md named and `job.start_timeout`. Added them as a second table, since the first
  says none of its values was measured. Each default was read from `src/lib/job.cpp`.
- Trimmed the "not in the table" wording from the TODO.md rows and removed the `task.affinity`
  row, which said nothing else. Row 21 of the Changes table and the `spill.budget` row stay open.
- Found, not changed: the table says `log.level` is `info`; the job processes default to `warn`. (Changed in session 2.)

### 2026-11-01 — session 2: two corrections to the plan

- At the person's request: `log.level` in the Tunables table now says `warn` for job processes
  and `info` for `pmd`, as `src/lib/job.cpp` and `src/pmd/main.cpp` do, and row 21 of the Changes table says what `COMPRESSED` means since X-page-encoding. The
  TODO.md wording about row 21 is gone. The `spill.budget` row stays open.

## Files touched

<!--
Every file this task's branch changes, one bullet each, starting with the path in backticks.
A directory entry ends with "/" and covers everything under it. plan.py check --base fails
if the branch changes a file not covered here.
Example:  - `src/net/epoll_loop.cpp` — epoll wrapper and timer wheel
-->
- `docs/PLAN.md` — the second Tunables table
- `docs/TODO.md` — rows that asked for it

## Decisions

<!--
Choices you made inside the card's scope, each with the reason. Not for points listed under
"Still open" in the plan: those are asked, never decided here. Write "None" if there were none.
-->
None

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
- `docs/PLAN.md`: edited at the person's request in chat, 2026-11-01.

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
Documents only, so no build. Every key in `src/` and `tools/` is now in a table, checked with
`grep`; `plan.py check` passes.

## Deferred

<!--
Anything left undone, each also added to docs/TODO.md in this same pull request. "None" if nothing.
-->
None
