# X-required-checks: format and tidy as required checks

- Task: X-required-checks
- Milestone: none
- Stream: -
- Status: in-progress
- Branch: x-required-checks
- Started: 2026-10-30
- Finished:

<!--
Status is one of: in-progress, blocked, done.
  blocked  -> at least one [open] item under "Questions and blockers".
  done     -> the Done-when check passed and is recorded under Verification, Finished has a date,
              and Decisions, Approvals, Deviations and Deferred each say something (or "None").
HTML comments like this one are ignored by plan.py check. Leave them or delete them.
-->

## Card

<!-- Copied from docs/PLAN.md when this log was created. Do not edit; the plan is the source. -->
No task card. Approved by the person (in chat) on 2026-10-30. They asked for `format` and `tidy` to be required for merging into `main`, after the 10 build-and-test jobs were made required on 2026-10-29 (branch protection, no repo change).

## Sessions

<!--
One entry per working session, newest last. Write it as you go, not at the end:
another agent (or you, after a context reset) resumes from this log alone.
Each entry: what you did, what state the code is in, the commands you ran and what they printed,
and the next step.
-->

### 2026-10-30 — session 1

- `main` requires the 10 build-and-test checks; the `format` and `tidy` jobs could not simply
  be added. On a pull request from a branch of this repo they are skipped, and the skipped run
  reported under the same name. Commit `4b04b57` has `format | skipped` and `format | success`,
  and the same for `tidy`. GitHub counts a skipped required check as passing, so requiring them
  as they were would let a PR merge before they ran. The build-and-test matrix job does not have
  this problem: skipped, it reports under its unexpanded name.
- `.github/workflows/ci.yml`: `format` and `tidy` get a `name` that is the job's own `if`
  condition: `format`/`tidy` when the job runs, `format (not run)`/`tidy (not run)` when it is
  skipped. Repeating the condition keeps a pull request from a fork right, since there the jobs
  do run on `pull_request`. If GitHub leaves the name unevaluated for a skipped job (as it does
  for the matrix job), the name is the raw expression, which also differs.
- `python3 -c "import yaml; ..."`: both names parse as the intended expression.
- Next: push the branch and check the names on the push run and the PR run. Then add `format`
  and `tidy` (app 15368) to `main`'s required checks.

## Files touched

<!--
Every file this task's branch changes, one bullet each, starting with the path in backticks.
A directory entry ends with "/" and covers everything under it. plan.py check --base fails
if the branch changes a file not covered here.
Example:  - `src/net/epoll_loop.cpp` — epoll wrapper and timer wheel
-->
- `.github/workflows/ci.yml` — `format` and `tidy` report under another name when skipped

## Decisions

<!--
Choices you made inside the card's scope, each with the reason. Not for points listed under
"Still open" in the plan: those are asked, never decided here. Write "None" if there were none.
-->
- The skipped copies are renamed rather than removed (`if` dropped from the pull_request event,
  say): the jobs must still run for pull requests from forks, which have no push run here.

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
None

## Verification

<!--
The card's Done-when check: the exact commands run, where (local, Actions, simulated nodes,
cabled laptops), and the actual result, pasted or summarised with numbers. Also the quality gate
(clang-format, clang-tidy, sanitizer runs). Required before Status: done.
-->

## Deferred

<!--
Anything left undone, each also added to docs/TODO.md in this same pull request. "None" if nothing.
-->
None
