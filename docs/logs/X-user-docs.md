# X-user-docs: The library's user guide and API reference

- Task: X-user-docs
- Milestone: none
- Stream: -
- Status: done
- Branch: x-user-docs
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
No task card. Asked for by Amirishetty Sai Vignesh on 2026-10-27: "a proper README or a
documentation type of page in which we explain all the APIs, what are the parameters, what do
they mean ... separate from the architecture"; in a separate doc too, with a diagram (asked on
2026-10-27: repo and shared doc, the diagram in the dashboard's look).

## Sessions

<!--
One entry per working session, newest last. Write it as you go, not at the end:
another agent (or you, after a context reset) resumes from this log alone.
Each entry: what you did, what state the code is in, the commands you ran and what they printed,
and the next step.
-->

### 2026-10-27 — the guide, its example, the diagram

- Read `include/paramesh.h` whole (frozen, unchanged), `pmrun --help`, `pm --help`, the
  settings loader (`paramesh.conf`, `key = value`, read by pmd, passed as `PARAMESH_CFG_*`) and
  pmd's decline messages, so the guide quotes what the tools print.
- `docs/API.md`: install and join, a first program, concepts, rules, every function (signature,
  who may call it, parameters, errors), status codes, tools, settings, troubleshooting, a
  pointer to the architecture documents. README: "Using it" (this guide) apart from "How it is
  built".
- `apps/quickstart`: the guide's example, built by CI so it keeps compiling. Run: one node,
  `quickstart 10` -> 285 (right); three nodes, default -> 333332833333500000 (right). First
  version segfaulted: it read the shared total after `pm_finalize()`; fixed, the guide now
  says why.
- The lock and `pm_touch` snippets compiled in a scratch file with `-std=c11 -Wall -Wextra
  -Werror`. A script checks the guide's example is `apps/quickstart/main.c` word for word.
- Diagram (diagram-design, architecture type, the dashboard's colours):
  `docs/images/job-across-pool.html`, exported to `.svg` with the skill's `export_svg.py`;
  `self_check.py` OK. First render had connectors through a zone label; rerouted, looked again.
- The same guide published as a shareable doc (docs skill), its diagram redrawn there and
  looked at: https://claude.ai/artifact/Vca98NGv36xqgxx48st4VM
- Rebased on `main` after X-dashboard-join (PR #80); X-setup is PR #81. `quickstart` built again on this base. Next: the pull request.
### 2026-10-28 — CI: tidy finding, thread job hang

- tidy (CI): `apps/quickstart/main.c:30`, `2 * PM_PAGE_SIZE` widened to 64 bits after the
  multiply. Now `2 * (uint64_t)PM_PAGE_SIZE`; clang-format wrapped the line. The guide's code
  block in `docs/API.md` and the shared doc changed to match.
- `clang / thread` (CI): hit the 6-hour job limit in "Install tools" (apt), before configure;
  nothing of ours ran. Runs again with this push.
- `cmake --preset tidy && cmake --build --preset tidy --target quickstart`: 22/22, no findings.
  `clang-format --dry-run --Werror apps/quickstart/main.c`: clean.

## Files touched

<!--
Every file this task's branch changes, one bullet each, starting with the path in backticks.
A directory entry ends with "/" and covers everything under it. plan.py check --base fails
if the branch changes a file not covered here.
Example:  - `src/net/epoll_loop.cpp` — epoll wrapper and timer wheel
-->
- `docs/API.md` — the user guide and API reference
- `docs/images/job-across-pool.html` — its diagram, the source
- `docs/images/job-across-pool.svg` — the same, exported
- `apps/quickstart/CMakeLists.txt` — the guide's example
- `apps/quickstart/main.c` — the same
- `README.md` — using it apart from how it is built
- `docs/logs/X-user-docs.md` — this log

## Decisions

<!--
Choices you made inside the card's scope, each with the reason. Not for points listed under
"Still open" in the plan: those are asked, never decided here. Write "None" if there were none.
-->
- Markdown in the repo is the source: it ships in the installer and is reviewed with the code;
  the shared doc is a copy for reading and comments.
- The example is an app in the tree, so a change to the API that breaks it fails CI.
- Settings listed: those a user would change, with their defaults as the code has them; the
  full list stays in the plan.

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
No card. The guide's example is `apps/quickstart`: built and run on one node (285, right) and
three (333332833333500000, right). The lock and `pm_touch` snippets compile with `-Wall -Wextra
-Werror`. The example in the guide matches the app word for word. `clang-format --dry-run
--Werror apps/quickstart/main.c`: clean. clang-tidy on `quickstart` (tidy preset, 2026-10-28): clean. Diagram: `self_check.py` OK, looked at rendered.

## Deferred

<!--
Anything left undone, each also added to docs/TODO.md in this same pull request. "None" if nothing.
-->
None.
