# X-accounts: Self-registered people, and a queue that takes turns

- Task: X-accounts
- Milestone: none
- Stream: -
- Status: done
- Branch: x-accounts
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
Approved by Amirishetty Sai Vignesh in chat, 2026-11-02: people register themselves, and take turns. Done when: the playground checks show the line, turns, privacy and lockout.

## Sessions

<!--
One entry per working session, newest last. Write it as you go, not at the end:
another agent (or you, after a context reset) resumes from this log alone.
Each entry: what you did, what state the code is in, the commands you ran and what they printed,
and the next step.
-->

### 2026-11-02 — session 1

- `server.py`: register and sign in (PBKDF2, salted, file mode 600), sessions in memory, one line per node.
  One place each, for a person and for a LAN address, so a person who runs again goes behind those waiting.
  Runs, past runs and Stop are the owner's only. Wrong passwords and invite codes are counted by address,
  which closes the parallel guessing the old 1 s sleep allowed.
- `playground_checks.py` first failed in every example: my test instance on port 47087 held its ports.
  With that stopped, `tests/multi/run_playground.sh build/dev` printed PASS.
- The sign-in dialog itself has not been clicked through in a browser.

## Files touched

<!--
Every file this task's branch changes, one bullet each, starting with the path in backticks.
A directory entry ends with "/" and covers everything under it. plan.py check --base fails
if the branch changes a file not covered here.
Example:  - `src/net/epoll_loop.cpp` — epoll wrapper and timer wheel
-->

- `tools/playground/server.py` — people and the line
- `tools/site/playground.html` — the sign-in dialog
- `tools/site/playground.js` — sign-in, place in line
- `tools/site/site.css` — its look
- `tests/multi/playground_checks.py` — checks
- `docs/PLAYGROUND.md` — how it behaves
- `docs/logs/X-accounts.md` — this log

## Decisions

<!--
Choices you made inside the card's scope, each with the reason. Not for points listed under
"Still open" in the plan: those are asked, never decided here. Write "None" if there were none.
-->

- Self-registration, the person's choice: it fits a pool with no one in charge.
- One queue per node, as the plan has no central queue.
- First in line, with one place each, already gives turns: no separate rotation is kept.

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

`bash tests/multi/run_playground.sh build/dev`: PASS, 2026-11-02, loopback nodes.

## Deferred

<!--
Anything left undone, each also added to docs/TODO.md in this same pull request. "None" if nothing.
-->
None: the limits are in `docs/PLAYGROUND.md`.

