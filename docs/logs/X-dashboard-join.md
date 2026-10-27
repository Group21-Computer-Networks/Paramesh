# X-dashboard-join: Dashboard: compute given and received, join and leave the pool

- Task: X-dashboard-join
- Milestone: none
- Stream: -
- Status: done
- Branch: x-dashboard-join
- Started: 2026-10-27
- Finished: 2026-10-28

<!--
Status is one of: in-progress, blocked, done.
  blocked  -> at least one [open] item under "Questions and blockers".
  done     -> the Done-when check passed and is recorded under Verification, Finished has a date,
              and Decisions, Approvals, Deviations and Deferred each say something (or "None").
HTML comments like this one are ignored by plan.py check. Leave them or delete them.
-->

## Card

<!-- Copied from docs/PLAN.md when this log was created. Do not edit; the plan is the source. -->
No task card. Asked for by Amirishetty Sai Vignesh on 2026-10-27: "a dashboard in which the user
can monitor how much compute he has given and he has received if he is part of the cluster that
is connected to him via LAN, and also shows the choice to join a lead cluster, and while joining
provide him the options to decide how much compute he is going to provide".

## Sessions

<!--
One entry per working session, newest last. Write it as you go, not at the end:
another agent (or you, after a context reset) resumes from this log alone.
Each entry: what you did, what state the code is in, the commands you ran and what they printed,
and the next step.
-->

### 2026-10-27 — standby, join and leave; the dashboard

- Read the M6-3 dashboard, `docs/STATUS_API.md`, `src/pmd/pmd.cpp` and the ledger. The ledger
  already has compute both ways in core-seconds (`/v1/ledger`, per job `credit_taken` =
  given by this node, `credit_given` = received). Joining is new: a `pmd` was always in the
  pool, with its caps fixed by flags, and left by stopping. No frozen file is touched
  (`http_server.h` is not in INTERNAL_API.md; nothing new on the wire).
- `src/platform/`: `HttpServer::post`, with the guard in the adapter: `X-ParaMesh: 1` and a
  Host of `127.0.0.1:<port>` or `localhost:<port>`, else 403 (another web page cannot press
  the buttons; DNS rebinding gets the wrong Host). `unit.platform_test`: a POST case.
- `src/pmd/`: `--standby` (hear the pool, no beacon, SPAWN_REQ declined, `pmrun` refused);
  `POST /v1/join` (cores, RAM, optional lead added to the seeds; checked on the HTTP thread,
  done on the main loop through the existing eventfd); `POST /v1/leave` (the graceful leave,
  then back to standby instead of stopping; refused while a job launched here runs). `/v1/peers`
  gains `member`, `note`, `cap_ram`, `machine_cores`, `machine_ram`. `unit.pmd_test`: the
  command line, and standby -> join (bad bodies 400, twice 409, no header 403) -> heard ->
  leave -> standby, still running -> join again.
- `tools/dashboard/index.html`: "This node" (state, join form: cores slider, RAM, lead node
  with the nodes heard offered; leave button), "Your account with the pool" (given, received,
  net), "Given and received, by node".
- Tried in the browser pane with two daemons (node 11 `--standby`, node 12 joined): joined with
  4 cores and 2,048 MB; node 12 listed it; `pmrun -n 2 matmul 384` from node 12: node 11 ran
  10 chunks, the dashboard showed 0.6 core-seconds given, node 12's account agreeing; left:
  back to standby, history kept.
- Polish (impeccable): the two equal number cards became one account card with the net
  balance; buttons say "Joining…"/"Leaving…" while a request is out and errors show in a
  contrast-checked colour; themed focus ring and selection; tables scroll inside their own
  box (375 px: page width 375, no sideways scroll); server errors in plain words.
- clang-tidy: the three new bools padded `Pmd`; moved beside the others, clean. clang-format:
  clean. `ctest --preset dev`: 16 of 16. Next: the matrix, the pull request.

## Files touched

<!--
Every file this task's branch changes, one bullet each, starting with the path in backticks.
A directory entry ends with "/" and covers everything under it. plan.py check --base fails
if the branch changes a file not covered here.
Example:  - `src/net/epoll_loop.cpp` — epoll wrapper and timer wheel
-->
- `src/platform/http_server.h` — `post`
- `src/platform/adapters/http_httplib.cpp` — it, with the guard
- `src/pmd/pmd.h` — `--standby`, the membership state
- `src/pmd/pmd.cpp` — standby, join, leave back to standby, `/v1/peers` fields
- `tests/unit/platform_test.cpp` — the POST guard
- `tests/unit/pmd_test.cpp` — the command line; standby, join and leave
- `tools/dashboard/index.html` — membership, join form, compute given and received
- `docs/STATUS_API.md` — the new fields and endpoints
- `docs/logs/X-dashboard-join.md` — this log

## Decisions

<!--
Choices you made inside the card's scope, each with the reason. Not for points listed under
"Still open" in the plan: those are asked, never decided here. Write "None" if there were none.
-->
- "A lead cluster": ParaMesh has one pool per LAN (every daemon heard by multicast), with no
  leader. Joining makes this node a member; the optional "lead node" is any member's address,
  for a network without multicast. A named or led cluster would need a field in HELLO
  (PROTOCOL.md, frozen): not done.
- Caps change only between a leave and a join: running jobs keep the commitments they were
  given (X-node-commit).
- Leave from the dashboard ends in standby; Ctrl+C and `pm leave` still stop the daemon.

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
- The work itself: Amirishetty Sai Vignesh, 2026-10-27, in chat. No frozen file changes.

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
No card. On this machine (Ubuntu 24.04, gcc 13, clang 18):

- Release, warnings as errors: gcc 16/16, clang 16/16. Debug: gcc none 16/16, address 16/16,
  undefined 16/16, thread 15/15; clang none 16/16, address 16/16, undefined 16/16, thread
  15/15. (A first run was disturbed by a second copy of the script on the same build
  directories; the rest was run again alone.)
- `unit.platform_test` POST guard and `unit.pmd_test` standby/join/leave fail on `main`.
- In the browser pane: join, a two-node job, compute given shown, leave; 375 px wide with no
  sideways scroll; the error path.
- `cmake --build --preset tidy`: no finding. clang-format `--dry-run --Werror`: clean.

## Deferred

<!--
Anything left undone, each also added to docs/TODO.md in this same pull request. "None" if nothing.
-->
None.
