# X-lock-pmd: Only a node of the pool starts a job here

- Task: X-lock-pmd
- Milestone: none
- Stream: -
- Status: done
- Branch: x-lock-pmd
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
Approved by Amirishetty Sai Vignesh in chat, 2026-11-02: a stranger on the LAN must not be able to ask `pmd` to start a program. Done when: unit tests show an unsigned or wrongly signed SPAWN_REQ is declined and nodes that share a token run a job.

## Sessions

<!--
One entry per working session, newest last. Write it as you go, not at the end:
another agent (or you, after a context reset) resumes from this log alone.
Each entry: what you did, what state the code is in, the commands you ran and what they printed,
and the next step.
-->

### 2026-11-02 — session 1

- SPAWN_REQ carries `pool_tag`, made from the token in `playground.token`. A node with a token declines a
  request without the right tag, before it reads the program. It covers job, request, hash, path, cwd and
  arguments. A node with no token takes any request, as tests and `--peers` pools without the site do.
- Three new `pmd_test` cases pass; `unit.wire` passes; the tidy build of `pmd` links.
- The invite's own lockout is in X-accounts.

## Files touched

<!--
Every file this task's branch changes, one bullet each, starting with the path in backticks.
A directory entry ends with "/" and covers everything under it. plan.py check --base fails
if the branch changes a file not covered here.
Example:  - `src/net/epoll_loop.cpp` — epoll wrapper and timer wheel
-->

- `src/pmd/pool_key.h` — the tag
- `src/pmd/pool_key.cpp` — the tag
- `src/pmd/pmd.cpp` — signs and checks
- `src/wire/payloads.h` — the field
- `src/wire/wire.cpp` — the field
- `tests/unit/pmd_test.cpp` — checks
- `tests/unit/wire_test.cpp` — checks
- `docs/PROTOCOL.md` — the field
- `docs/TODO.md` — what stays open
- `docs/logs/X-lock-pmd.md` — this log

## Decisions

<!--
Choices you made inside the card's scope, each with the reason. Not for points listed under
"Still open" in the plan: those are asked, never decided here. Write "None" if there were none.
-->

- A SHA-256 with the token on both sides, in place of a new HMAC, with `Hasher` as it is.
- `pmd` reads the token at each request, since an invite replaces it while `pmd` runs.

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
- docs/PROTOCOL.md: `pool_tag` in SPAWN_REQ, approved by Amirishetty Sai Vignesh in chat, 2026-11-02.

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

Dev build, 2026-11-02: `unit_pmd_test` token and tag cases 3/3, `unit.wire`, `unit.lib` and `unit.net` by ctest. CI runs the rest and the sanitizers.

## Deferred

<!--
Anything left undone, each also added to docs/TODO.md in this same pull request. "None" if nothing.
-->
- Known limits, in PROTOCOL.md: a copied request can be replayed, and the invite carries the token in plain HTTP. Job data ports have no lock. See `docs/TODO.md`.

