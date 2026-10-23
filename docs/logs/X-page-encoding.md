# X-page-encoding: Adaptive page compression and differential transfer

- Task: X-page-encoding
- Milestone: none
- Stream: -
- Status: done
- Branch: x-page-encoding
- Started: 2026-10-23
- Finished: 2026-10-24

<!--
Status is one of: in-progress, blocked, done.
  blocked  -> at least one [open] item under "Questions and blockers".
  done     -> the Done-when check passed and is recorded under Verification, Finished has a date,
              and Decisions, Approvals, Deviations and Deferred each say something (or "None").
HTML comments like this one are ignored by plan.py check. Leave them or delete them.
-->

## Card

<!-- Copied from docs/PLAN.md when this log was created. Do not edit; the plan is the source. -->
No task card. Approved by Amirishetty Sai Vignesh on 2026-10-23 ("do 6, then 2 and 5"): item 6
of the group's feature list, "Implement an Adaptive Memory Page Compression and Transfer
Mechanism": reduce the page data sent between nodes, choosing compression or differential page
transfer by the page's modifications and the network's conditions, with reliable rebuilding
and little extra processing; evaluate transferred bytes, CPU use, page-transfer latency and
application performance.

## Sessions

<!--
One entry per working session, newest last. Write it as you go, not at the end:
another agent (or you, after a context reset) resumes from this log alone.
Each entry: what you did, what state the code is in, the commands you ran and what they printed,
and the next step.
-->

### 2026-10-23 — design, the frozen-file approval, the encoder

- Read PROTOCOL.md 3 and 7.2: `COMPRESSED` (0x0002) is reserved, never set (the plan's Changes
  row 21: LZ4 in the HLD, no library this semester). Page frames: READ_DATA, WRITE_GRANT,
  FETCH_DATA, WRITEBACK, 4,104 bytes. The home machine drops its copy on a write grant
  (`kDropCopy`), so a home has no base for a diff unless it keeps one.
- Design: an encoding of my own, no library: a 64-byte map of the 8-byte words with a non-zero
  byte, and for each such word a byte mask and its non-zero bytes. A diff is the same encoding
  of page XOR base. The base: a home granting a page for writing to another node keeps a copy
  of what it grants, outside the home store and the state machines, and sets a new flag,
  `BASE_KEPT`; the owner keeps the same bytes (its twin) and sends its next FETCH_DATA or
  WRITEBACK to that home as a diff. Every encoded page carries a CRC-32C of the rebuilt page.
- Asked the person for the frozen-file changes (PROTOCOL.md 3 and 7.2, INTERNAL_API.md 3);
  approved 2026-10-23 (Approvals).
- `src/wire/`: `kFlagBaseKept`; `EncodedPage`, `wire_encode_page`, `wire_decode_encoded_page`.
  An encoding no shorter than a plain page is refused (sent plain). `unit.wire_test`: two cases
  (round trip, sizes, a diff of one changed word is 82 bytes; seven malformed payloads).
- `src/lib/job.cpp`: `post()` encodes page frames to other nodes; `on_frame` rebuilds and
  checks the CRC before handling (a page that does not rebuild aborts the job, PROTOCOL);
  `home_reply` keeps the base on WRITE_GRANT and UPGRADE_GRANT (UPGRADE_GRANT: the home copy,
  only if in RAM); with `BASE_KEPT` the owner keeps its twin (UPGRADE_GRANT: its read copy).
  Settings `page.encoding` (off, on, auto), `page.slow_wait_us`, `page.fast_min_saving`,
  `page.diff_pages`. `job_pages` gains page_bytes, page_bytes_plain, encoded, diffs, cpu_ms.
- `ctest --preset dev`: 16 of 16; again with `PARAMESH_CFG_PAGE_ENCODING=on`: 16 of 16.

### 2026-10-24 — the comparison, the test, the dissector

- `tools/bench/evaluate.sh`: columns page_kb, page_kb_plain, cpu_s, diffs; `impair` takes a
  rate; an `encoding` experiment (matmul 256 and sweep 16 MB x 4 rounds, FIFO placement, off /
  auto / on, on a plain link, 1 ms delay and 20 Mbit/s).
- First run on the Debug build: bytes cut 55-75%, faster at 20 Mbit/s, but up to twice as slow
  on a plain link: the byte loop at -O0. Added a fast path for words equal to the base; the
  comparison is from a Release build (`-b build/rel`) from here on.
- Release, two runs, run s / page KB sent of KB plain / CPU s (summed over the processes):

  | link | app | off | auto | on |
  | --- | --- | --- | --- | --- |
  | plain | matmul | 0.12 / 16,183 / 0.28 | 0.12 / 5,153 / 0.30 | 0.12 / 4,324 / 0.29 |
  | plain | sweep | 0.85 / 99,145 / 1.86 | 0.83 / 84,745 / 1.89 | 0.92 / 43,234 / 1.98 |
  | 1 ms | matmul | 1.66 / 9,863 / 0.38 | 1.67 / 3,401 / 0.42 | 1.68 / 3,037 / 0.40 |
  | 1 ms | sweep | 17.05 / 93,614 / 3.49 | 17.12 / 44,107 / 3.84 | 21.64 / 45,086 / 4.58 |
  | 20 Mbit/s | matmul | 3.71 / 10,211 / 0.69 | 1.11 / 3,207 / 0.41 | 1.10 / 2,935 / 0.41 |
  | 20 Mbit/s | sweep | 23.50 / 95,017 / 4.53 | 12.25 / 42,390 / 3.76 | 12.70 / 46,822 / 4.34 |

  Second run: the same within a few percent, except 1 ms sweep: off 17.0, auto 21.6, on 17.2.
  Six more 1 ms sweep runs, off / on alternately: 19.0, 21.4, 17.2, 17.2, 17.3, 17.3 s; the slow
  ones received 14,438 and 15,634 pages, the others 15,338: FIFO placement, not the encoding.
  Fault p50 at 20 Mbit/s: 16,384 -> 8,192 us (sweep), p99 unchanged or lower.
- Kept `auto` the default: on a plain link it costs nothing measurable and sends a sixth to two
  thirds less; on a slow link it is as good as `on`.
- `unit.lib_test`: the neighbours program (one page changing hands, no hold window, FIFO) with
  encoding on and off: on, 1,131 diffs, 310,887 bytes sent of 9,295,560 plain, every write
  arrived; off, no diff and bytes sent equal to plain.
- `tools/dissector/paramesh.lua`: `BASE_KEPT` named; a COMPRESSED page shows mode, page_crc,
  word map and words. `tools/dissector/capture.sh -b build/rel` with encoding on: malformed 0;
  2,643 COMPRESSED frames, e.g. mode 2, 1,241 bytes of words.
- clang-format: clean. clang-tidy: a nested conditional and a missing const in `job.cpp`;
  fixed, clean.
- Release builds (gcc, clang), `ctest -j2`: 16 of 16 each. Debug matrix: all eight green
  (below). Next: the plan check and the pull request.

## Files touched

<!--
Every file this task's branch changes, one bullet each, starting with the path in backticks.
A directory entry ends with "/" and covers everything under it. plan.py check --base fails
if the branch changes a file not covered here.
Example:  - `src/net/epoll_loop.cpp` — epoll wrapper and timer wheel
-->
- `src/wire/frame.h` — `kFlagBaseKept`
- `src/wire/payloads.h` — the encoded page
- `src/wire/wire.cpp` — its encoder and decoder
- `src/lib/job.cpp` — encoding, rebuilding, the kept copies, the settings, the counts
- `tests/unit/wire_test.cpp` — encoder and decoder cases
- `tests/unit/lib_test.cpp` — diffs in a three-process job
- `tools/bench/evaluate.sh` — new columns, a rate for `impair`, the `encoding` experiment
- `tools/dissector/paramesh.lua` — encoded pages, `BASE_KEPT`
- `docs/PROTOCOL.md` — `COMPRESSED`, `BASE_KEPT`, the encoded page payload (approved)
- `docs/INTERNAL_API.md` — the two flags (approved)
- `docs/TODO.md` — the `page.*` row
- `docs/logs/X-page-encoding.md` — this log

## Decisions

<!--
Choices you made inside the card's scope, each with the reason. Not for points listed under
"Still open" in the plan: those are asked, never decided here. Write "None" if there were none.
-->
- An encoding of my own instead of LZ4: no library (the plan's reason for row 21), one pass,
  and pages in these workloads are mostly zero bytes (small numbers as doubles or integers),
  which it removes; a diff makes the unchanged bytes zero too.
- The base lives outside the home store and the state machines: they keep their meaning
  (STATE_MACHINES.md unchanged), and the home copy is still dropped on a write grant. The cost
  is bounded memory outside the caps (TODO).
- `BASE_KEPT` instead of the owner guessing: the owner diffs only against a copy the home
  promised to keep until the owner's data comes back, so a diff can always be rebuilt; the
  page CRC catches any mistake (the job aborts, nothing is zero-filled).
- "Network conditions": this process's page waits, averaged over the last few. On a slow link
  any shorter encoding is sent; on a fast one only a saving of half or more, so the extra
  work is spent where bytes cost time. No transport change (INTERNAL_API.md 7 unchanged).
- CPU is the job processes' user and system time (`getrusage`), logged at the end.

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
- The work itself: Amirishetty Sai Vignesh, 2026-10-23 ("do 6, then 2 and 5").
- `docs/PROTOCOL.md` (3, flags; 7.2, the encoded page payload) and `docs/INTERNAL_API.md` (3,
  `kFlagCompressed`, `kFlagBaseKept`): Amirishetty Sai Vignesh, 2026-10-23, asked in chat with
  the change described ("Approve").

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
No card, so no Done-when check of its own. On this machine (Ubuntu 24.04, gcc 13, clang 18):

- Matrix, `build/nc-<compiler>-<sanitizer>`, Debug, `ctest -j2`: gcc none 16/16, address
  16/16, undefined 16/16, thread 15/15; clang none 16/16, address 16/16, undefined 16/16,
  thread 15/15 (`app.m0_demo` is not registered under ThreadSanitizer, as on `main`).
- Release, warnings as errors: gcc 16/16, clang 16/16.
- `ctest --preset dev` with `PARAMESH_CFG_PAGE_ENCODING=on`: 16/16.
- The comparison the item asks for (bytes, CPU, page-wait latency, run time),
  `tools/bench/evaluate.sh -b build/rel encoding`, in the session above.
- `unit.wire_test`'s two cases and `unit.lib_test`'s encoding case fail without the encoder
  (no such function; no diffs logged).
- `tools/dissector/capture.sh` with encoding on: no malformed frame.
- `cmake --build --preset tidy`: no finding. clang-format `--dry-run --Werror`: clean.

## Deferred

<!--
Anything left undone, each also added to docs/TODO.md in this same pull request. "None" if nothing.
-->
- The `page.*` keys in the plan's Tunables table and row 21 of its Changes table; the kept
  copies outside the caps; a home's view of the link; `SEG_MIGRATE` pages plain (one TODO
  row).
