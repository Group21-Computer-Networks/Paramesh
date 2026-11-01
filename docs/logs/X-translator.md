# X-translator: Plain C with a pragma, translated for the playground

- Task: X-translator
- Milestone: none
- Stream: -
- Status: done
- Branch: x-translator
- Started: 2026-11-02
- Finished: 2026-11-02

<!--
Status is one of: in-progress, blocked, done.
  blocked  -> at least one [open] item under "Questions and blockers".
  done     -> the Done-when check passed and is recorded under Verification, Finished has a date,
              and Decisions, Approvals, Deviations and Deferred each say something (or "None").
HTML comments like this one are ignored by plan.py check. Leave them or delete them.
-->

## Card

<!-- Copied from docs/PLAN.md when this log was created. Do not edit; the plan is the source. -->
Approved by Amirishetty Sai Vignesh in chat, 2026-11-02: write plain C, and have whatever loops can be parallelised run on the pool while the rest stays on the laptop that started it. Done when: plain C programs print the same on two nodes as gcc's own build prints alone, and unsafe loops stay.

## Sessions

<!--
One entry per working session, newest last. Write it as you go, not at the end:
another agent (or you, after a context reset) resumes from this log alone.
Each entry: what you did, what state the code is in, the commands you ran and what they printed,
and the next step.
-->

### 2026-11-02 — session 1

- First built on a `#pragma paramesh` marker. The person then asked for no marker: every loop safe to
  split is split. Rewritten: each `for` in `main()` is studied, a loop that stays has its inner loops
  tried, and totals are found from how a variable is used. `Stay` replaces refusing the program.
- A job asking for the default region was refused: 4 GB needed, 966 MB free. The size now comes from
  the server's `--region-mb`, and a full region stops with a message.
- `tests/multi/translate_checks.py` runs each case both ways. It caught `x = x*2 + i` taken for a
  product: fixed, with cases for a modulus and a shift.
- Result: `tests/multi/run_playground.sh build/dev` printed PASS, all playground checks and every
  translator case, on 2 local nodes. NOT tested on the two real laptops.

## Files touched

<!--
Every file this task's branch changes, one bullet each, starting with the path in backticks.
A directory entry ends with "/" and covers everything under it. plan.py check --base fails
if the branch changes a file not covered here.
Example:  - `src/net/epoll_loop.cpp` — epoll wrapper and timer wheel
-->

- `tools/playground/translate.py` — the translator
- `tools/playground/server.py` — translates, falls back, `--region-mb`
- `tools/site/playground.js` — shows where each loop ran
- `tools/site/examples.js` — a plain C example
- `tools/setup/package.sh` — packs the translator
- `tests/multi/translate_checks.py`, `tests/multi/run_playground.sh` — checks
- `docs/PLAYGROUND.md`, `docs/logs/X-translator.md`, `docs/TODO.md`

## Decisions

<!--
Choices you made inside the card's scope, each with the reason. Not for points listed under
"Still open" in the plan: those are asked, never decided here. Write "None" if there were none.
-->

- No marker, at the person's request, so safety rests on the checks, and a doubtful loop stays.
- Totals go through a shared cell and a lock, not `pm_atomic_add`, so `double` and products work.
- Only used with 2 or more nodes: 1 node runs the program exactly as written.

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
None: no frozen file changed.

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

`bash tests/multi/run_playground.sh build/dev`: PASS, 2 local nodes, 2026-11-02. No C++ changed, so the quality gate does not apply. Two real laptops not run.

## Deferred

<!--
Anything left undone, each also added to docs/TODO.md in this same pull request. "None" if nothing.
-->
- See the row in `docs/TODO.md`: limits of the checks, and loops outside `main()`.

