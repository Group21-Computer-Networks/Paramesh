# X-cache-policy: Cost-aware local cache replacement

- Task: X-cache-policy
- Milestone: none
- Stream: -
- Status: done
- Branch: x-cache-policy
- Started: 2026-10-21
- Finished: 2026-10-22

<!--
Status is one of: in-progress, blocked, done.
  blocked  -> at least one [open] item under "Questions and blockers".
  done     -> the Done-when check passed and is recorded under Verification, Finished has a date,
              and Decisions, Approvals, Deviations and Deferred each say something (or "None").
HTML comments like this one are ignored by plan.py check. Leave them or delete them.
-->

## Card

<!-- Copied from docs/PLAN.md when this log was created. Do not edit; the plan is the source. -->
No task card. Approved by Amirishetty Sai Vignesh on 2026-10-21 ("lets finish off 7, 3, 1"):
item 7 of the group's feature list, an adaptive cache replacement that weighs page reuse,
modification and retrieval cost, compared against the existing FIFO policy.

## Sessions

<!--
One entry per working session, newest last. Write it as you go, not at the end:
another agent (or you, after a context reset) resumes from this log alone.
Each entry: what you did, what state the code is in, the commands you ran and what they printed,
and the next step.
-->

### 2026-10-21 — session 1

- Read `src/store/local_cache.{h,cpp}` (first in, first out) and how `src/lib/job.cpp` uses it
  (`make_room`, installs, discards). Asked the person to approve an interface change and two
  tunables (below).
- `src/store/local_cache.cpp`: one class for both policies. `next_victim` takes, of the
  `window` oldest pages not yet offered, the cheapest to lose; a window of 1 is FIFO, so the
  old behaviour is kept exactly (the old FIFO test passes unchanged in substance). Pages given
  up and discarded are remembered (as many as the budget), and one installed again counts as
  reused. `note_installed(page, writable)`: an upgrade marks a page writable in place.
- First score: last fetch time x (1 + reuse) x 2 if writable. `evaluate.sh cache` (matmul 512,
  1 MB caps): FIFO 17.0 s, cost 180.1 s; same faults, p50 fault latency 128 us against 2,048.
  At matmul 256 (`-s 128 outofcore`), window 1/2/4: 1.50/3.99/3.55 s.
- Factors one at a time, window 2: all equal 1.60 s (the window costs nothing); fetch time
  alone 3.30 s; writable alone 1.73 s, 34,047 faults against 34,560; reuse alone 1.70 s. The
  fetch time is the culprit: a page's last fetch time is mostly noise. A per-home running mean
  instead: window 1/2/8 1.70/2.67/4.37 s. Reuse and writable only: window 8 2.46 s (33,649
  faults, 17,523 pages in, against 34,560 and 18,515), window 16 4.20 s.
- Told the person: no score beat FIFO on run time. Answer: ship it as an option, FIFO the
  default, with the comparison on record. Fetch time removed from the score and the
  interface (`note_installed(page, writable)`); `cache.policy = cost` turns it on, with
  `cache.victim_window` (8).
- Tests: `unit.store_test`, the FIFO test (calls updated to the new signature) and a new
  cost test: writable twice, an upgrade in place, reuse, the window bound, window 1 is FIFO.
  `unit.internal_api_test`'s fake cache follows the signature.
- `tools/bench/evaluate.sh`: a `cache` experiment (fifo and cost at 1M and 2M, matrix size
  doubled); rows gain `pages_in` and `pages_out`, so `note_abort` reads the abort column by
  position.
- `evaluate.sh cache`, final: fifo 1M 17.071 s, 273,024 faults, 179,817 pages in, 183,953 out;
  cost 1M 33.820 s, 265,335, 157,266, 157,265; fifo 2M 16.344 s, 268,928, 159,689, 161,743;
  cost 2M 35.147 s, 265,335, 157,232, 157,265.
- With `PARAMESH_CFG_CACHE_POLICY=cost` set, the cap and spill tests of `unit.pmd_test` pass
  (most region held 1,136 KB; largest spill file 10,240 KB).
- clang-tidy: `read_settings` passed the complexity limit; the cache keys moved into
  `read_waits_and_grain`. Clean. clang-format: clean.
- Next: the matrix, the plan check, the pull request.

### 2026-10-22 — session 2

- Matrix, GCC and Clang, no sanitizer and each of the three (`cmake -B build/nc-<cc>-<san>
  -DPARAMESH_SANITIZER=<san>`, `ctest`): all pass, 16 of 16, 15 of 15 under thread (the count
  M6-2 had). `unit.pmd_test` under address takes about 15 minutes, as before.
- clang-format: clean.
- Next: the pull request.

## Files touched

<!--
Every file this task's branch changes, one bullet each, starting with the path in backticks.
A directory entry ends with "/" and covers everything under it. plan.py check --base fails
if the branch changes a file not covered here.
Example:  - `src/net/epoll_loop.cpp` — epoll wrapper and timer wheel
-->
- `src/store/local_cache.h` — the interface: `writable`, a window
- `src/store/local_cache.cpp` — the cost-aware choice, ghosts of pages given up
- `docs/INTERNAL_API.md` — section 6, `LocalCache` and `cache_open`
- `src/lib/job.cpp` — passes writable, reads `cache.policy` and `cache.victim_window`
- `tests/unit/store_test.cpp` — the cost policy; the FIFO test's calls
- `tests/unit/internal_api_test.cpp` — the fake cache's signature
- `tools/bench/evaluate.sh` — the `cache` experiment, `pages_in`/`pages_out` columns
- `docs/TODO.md` — the two keys and the slowdown
- `docs/logs/X-cache-policy.md` — this log

## Decisions

<!--
Choices you made inside the card's scope, each with the reason. Not for points listed under
"Still open" in the plan: those are asked, never decided here. Write "None" if there were none.
-->
- FIFO is the window of 1, not a second class: one code path for both policies.
- Retrieval cost is left out of the score: both forms measured slowed faults (session above).

## Questions and blockers

<!--
Each item starts with [open] or [answered YYYY-MM-DD by <who>], followed by the question and,
once answered, the answer. M0 drafting tasks may also use [for gate] for a Still-open point
drafted for the person to decide at the M0 gate. Write "None" if there are none.
-->
- [answered 2026-10-21 by Amirishetty Sai Vignesh] May the cache interface in INTERNAL_API.md
  section 6 change, with `cache.policy` and `cache.victim_window`? Answer: approve.
- [answered 2026-10-22 by Amirishetty Sai Vignesh] The cost policy is slower than FIFO on
  matmul: ship it with FIFO the default, dig into the slowdown, or drop it? Answer: ship it,
  FIFO default.

## Approvals

<!--
Approvals from the person for anything the rules reserve to them, such as a change to a frozen
file (include/paramesh.h, docs/PROTOCOL.md, docs/STATE_MACHINES.md, docs/INTERNAL_API.md) that
this card does not list. Name the file, who approved, and the date. "None" if there are none.
-->
- The work itself: Amirishetty Sai Vignesh, 2026-10-21 ("lets finish off 7, 3, 1").
- `docs/INTERNAL_API.md` section 6, `LocalCache::note_installed` and `cache_open`: Amirishetty
  Sai Vignesh in chat, 2026-10-21 ("Approve"). Approved with a fetch-time argument, which the
  measurements removed; the approved change is otherwise as made.

## Deviations

<!--
Anything that differs from the card: a file outside its Files list, a changed approach, a Done-when
check run differently. Each with the reason and who agreed. "None" if the card was followed exactly.
-->
- The approved score had the fetch time in it, and the cost policy was to be the default. The
  fetch time is out (measured harmful), and FIFO stays the default, as the person chose.

## Verification

<!--
The card's Done-when check: the exact commands run, where (local, Actions, simulated nodes,
cabled laptops), and the actual result, pasted or summarised with numbers. Also the quality gate
(clang-format, clang-tidy, sanitizer runs). Required before Status: done.
-->
- Local, Ubuntu 24.04. `cmake -S . -B build/nc-<cc>-<san> -G Ninja -DCMAKE_BUILD_TYPE=Debug
  -DPARAMESH_SANITIZER=<san>` then `ctest --output-on-failure -j2`:
  gcc none/address/undefined 16 of 16 passed, gcc thread 15 of 15; clang none/address/undefined
  16 of 16, clang thread 15 of 15.
- `unit.store_test`: the FIFO test and the cost test (writable twice, upgrade in place, reuse,
  the window bound, window 1 is FIFO) pass.
- `tools/bench/evaluate.sh cache`: fifo 1M 17.071 s, cost 1M 33.820 s; fifo 2M 16.344 s, cost
  2M 35.147 s (session 1). Cost is slower; FIFO stays the default.
- clang-format `--dry-run --Werror` over all sources: clean. clang-tidy (tidy preset): clean.

## Deferred

<!--
Anything left undone, each also added to docs/TODO.md in this same pull request. "None" if nothing.
-->
- The two keys in the plan's Tunables table, and why the cost policy makes each fault wait
  longer (docs/TODO.md).
