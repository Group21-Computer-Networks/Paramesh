# X-rehome: dynamic home reassignment

- Task: X-rehome
- Milestone: none
- Stream: -
- Status: done
- Branch: x-rehome
- Started: 2026-10-31
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
No task card. Item 5 of the course's extension list, "Develop a Dynamic Memory Home-Node Reassignment Mechanism": monitor page-access frequency and node workload, transfer heavily accessed segments to suitable nodes, preserve correct versions and prevent conflicting access during reassignment, and compare remote access latency, network traffic, migration overhead and load distribution against static placement. Approved by the person in chat on 2026-10-31.

## Sessions

<!--
One entry per working session, newest last. Write it as you go, not at the end:
another agent (or you, after a context reset) resumes from this log alone.
Each entry: what you did, what state the code is in, the commands you ran and what they printed,
and the next step.
-->

### 2026-10-31 — session 1

- Each home counts requests by segment and node. Every `rehome.period_ms`, with no segment of
  its own on the move, it sends `REHOME_HINT` for the segment one other node uses most, if that
  node made at least `rehome.min_requests` of the requests and `rehome.share` percent of them.
- The hint goes home → that node → launcher, which differs from what was first described to the
  person. Only the target knows it has room, and a move into a node without room stalls until
  the reply timer ends the job. So a node that has no room drops the hint.
- The launcher checks the job is up, nobody is joining or leaving, no rehoming map is being
  acknowledged, and `store_may_rehome`, then sends `SEG_MAP` with one home changed. The rest is
  the leave path: freeze, `SEG_MIGRATE`, `REDIRECT`. Versions and entries go with the segment.
- `store_may_rehome`, in `src/store/placement.cpp`: at most twice the even share of segments. A
  hint comes from another node, so this is what keeps hints from gathering the homes on one
  node, and through `L_SEGMENTS` from growing what its `pmd` commits. No hint reaches a weight.
- Off by default (`rehome.period_ms` 0).
- Tests: wire layout in `wire_test.cpp`, the bound in `store_test.cpp`, and in `lib_test.cpp` a
  segment moved during a live read-write fight, 6/6, with the last value written read back.
- `tools/bench/evaluate.sh rehome`, 1 ms added delay, FIFO chunk placement, three namespaces on
  one machine. `apps/falseshare` gained a `skip` argument, so a page can be homed off the fight.

  | Workload | Static | Rehoming every 500 ms |
  | --- | --- | --- |
  | fight, page homed on a third node, 11 static runs | 74.5-98.2 s in 9, 53.7 and 23.9 s in 2; p50 < 8,192 us in 10 | 22.5-28.2 s; 10,131-23,352 pages in; p50 < 4,096 us; 1 move each |
  | fight, page 0 (no move, so most likely homed on a fighter; not confirmed) | 17.9-47.1 s | 15.6-22.2 s; 0 moves |
  | matmul 256 | 0.62-0.64 s, 0 moves | 0.62-0.63 s, 0 moves |
  | sweep 16 MB, 4 rounds | 22.0-25.2 s, 0 moves | 23.2-25.2 s, 0 moves |

  A move took one `SEG_MAP` and the segment's pages, once. An earlier run of matmul, with
  `task.prefetch` at its default, moved one segment in 2 of 3 runs: 1.73-2.06 s against
  2.06-2.23 s static, three runs each, not enough to call a gain.
  One further static fight aborted, with `node 1 lost` and `node 3 lost`: 1 of 12 static runs, 0
  of 3 with rehoming, and no rehoming code runs when it is off. The logs were not kept, so
  `evaluate.sh` now keeps them; 9 more runs did not repeat it. Cause unknown; it is in TODO.md.
  The first attempt (200 ms, 256 requests) moved nothing: sweep's requests are too thin, and
  rows with 0 moves compare nothing. Only a segment one node dominates gains. On these
  workloads that is the third-node fight alone, so this stays off by default.

## Files touched

<!--
Every file this task's branch changes, one bullet each, starting with the path in backticks.
A directory entry ends with "/" and covers everything under it. plan.py check --base fails
if the branch changes a file not covered here.
Example:  - `src/net/epoll_loop.cpp` — epoll wrapper and timer wheel
-->
- `src/lib/job.cpp` — the counting, the hint and the move
- `src/store/placement.cpp` — `store_may_rehome`
- `src/store/placement.h` — `store_may_rehome`
- `src/wire/frame.h` — opcode `REHOME_HINT`
- `src/wire/payloads.h` — its payload
- `src/wire/wire.cpp` — its encoding
- `docs/PROTOCOL.md` — `REHOME_HINT`
- `docs/INTERNAL_API.md` — the opcode and `store_may_rehome`
- `docs/TODO.md` — the new keys
- `tools/dissector/paramesh.lua` — the frame
- `tools/bench/evaluate.sh` — experiment `rehome`; an aborted run keeps its logs
- `apps/falseshare/main.c` — `skip`
- `tests/unit/wire_test.cpp` — the frame
- `tests/unit/store_test.cpp` — the bound
- `tests/unit/lib_test.cpp` — a move during a fight

## Decisions

<!--
Choices you made inside the card's scope, each with the reason. Not for points listed under
"Still open" in the plan: those are asked, never decided here. Write "None" if there were none.
-->
- One opcode, two hops, rather than a reply from the target: the target's room is the only
  thing the launcher cannot know, and the launcher then sees the target as sender.
- One segment a period, and no memory of past moves (marked `ponytail:` in the code).

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
- `docs/PROTOCOL.md` and `docs/INTERNAL_API.md`: the `REHOME_HINT` frame, off by default, steering
  placement only. Approved by the person in chat, 2026-10-31.

## Deviations

<!--
Anything that differs from the card: a file outside its Files list, a changed approach, a Done-when
check run differently. Each with the reason and who agreed. "None" if the card was followed exactly.
-->
- The hint's route and the twice-the-share bound were added after the approval. The person was
  told both in chat. Still one message, off by default.

## Verification

<!--
The card's Done-when check: the exact commands run, where (local, Actions, simulated nodes,
cabled laptops), and the actual result, pasted or summarised with numbers. Also the quality gate
(clang-format, clang-tidy, sanitizer runs). Required before Status: done.
-->
No card. The comparison the item asks for is in the session above: three namespaces on one
machine with 1 ms netem delay, not real hardware. Remote latency as p50 fault wait, traffic as
pages received, overhead as moves, and load distribution through the twice-the-share bound
(not measured separately; only a third-node fight moved anything).
Local, 2026-11-01: clang-format clean; `ctest` with `dev`, `asan` and `ubsan` 16/16 each, `tsan`
15/15; `tidy` found `read_settings` too complex. After moving the `rehome.*` keys to
`read_rehome`: `tidy` exit 0, `dev` 16/16. The sanitizers were not repeated locally after that
move, which changes no logic; CI runs them.

## Deferred

<!--
Anything left undone, each also added to docs/TODO.md in this same pull request. "None" if nothing.
-->
- The three `rehome.*` keys are not in the plan's Tunables table, and there is no cooldown per
  segment. In docs/TODO.md.
- The one static abort above is not explained. In docs/TODO.md.
