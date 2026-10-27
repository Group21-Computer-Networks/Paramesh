# X-setup: A setup script to share: install ParaMesh at one path on every node

- Task: X-setup
- Milestone: none
- Stream: -
- Status: done
- Branch: x-setup
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
No task card. Asked for by Amirishetty Sai Vignesh on 2026-10-27: "a setup.bat or setup
executable or bash script, which can be shared directly to anyone ... that automatically places
the libraries in the right path, so that it functions correctly in a multi-computer
environment".

## Sessions

<!--
One entry per working session, newest last. Write it as you go, not at the end:
another agent (or you, after a context reset) resumes from this log alone.
Each entry: what you did, what state the code is in, the commands you ran and what they printed,
and the next step.
-->

### 2026-10-27 — the package and the installer

- What "the right path" means here: a worker runs the launcher's program from the same
  absolute path and checks its SHA-256 (PROTOCOL.md, SPAWN_REQ), and starts in the launcher's
  working directory (`chdir`, else exit 127). So one fixed prefix, `/opt/paramesh`, on every
  node, with one prebuilt package for all of them (a rebuild per machine gives the bundled
  apps different hashes). No `setup.bat`: ParaMesh needs Linux userfaultfd.
- `tools/setup/package.sh`: a Release build; `pmd`, `pm`, `pmrun`, `paramesh.h`, the
  dashboard (at `tools/dashboard/`, where pmd already looks beside its binary), every app, the
  guide if present; the eight module archives merged into one `libparamesh.a` (`ar -M`); a
  `paramesh.pc`; all packed below `tools/setup/install.sh` as `paramesh-setup-<version>.sh`.
- `tools/setup/install.sh`: checks Linux, the architecture it was built for and kernel 5.19;
  unpacks beside the old install then swaps (a failed unpack leaves the old one; the previous
  stays as `/opt/paramesh.old`); `/opt/paramesh/apps` owned by the user; links in
  `/usr/local/bin` and `/usr/local/lib/pkgconfig`; a systemd user unit, `pmd --standby`,
  enabled and started, then a health check on `/v1/peers` (fails with the journal command);
  `--no-service`; `--uninstall` (the rollback: unit, files, links; the ledger stays).
- `package.sh` here: `paramesh-setup-b1ef21a.sh`, 4.3 MB. Its payload unpacked to a
  scratch directory: an outside program built with `pkg-config --cflags --libs paramesh`
  failed to link (`log@@GLIBC_2.29`): the library needs libm; `-lm` added, then it linked and
  ran (`v[999] = 998001`). pmd from the unpacked tree served the dashboard (found beside its
  binary) and ran `matmul 256` over two nodes (chunks 4 and 124).
- Not run: the install itself (sudo into `/opt` and `/usr/local`, a user service) on this
  machine: sudo asks for a password, and no container is available. The person runs it.
- Next: rebase on X-dashboard-join (`--standby`), repackage, the gate, the PR.

### 2026-10-28 — repackaged on X-dashboard-join

- Merged `origin/x-dashboard-join` (PR #80) for `--standby`; `package.sh`:
  `paramesh-setup-500f0f9-dirty.sh`, 4.3 MB. Unpacked: `pmd --standby` (the service's command)
  says `"member":"standby"`; `pmrun -n 2 matmul 128` from the other node ran on that node
  alone (22:32); `POST /v1/join` -> `{"member":"joining"}`; `matmul 256` then ran on both
  (21:4 22:124).
- PR #80 merged; rebased on `main`. No C++ change here: CI's build jobs cover the tree. Next: the pull request.
## Files touched

<!--
Every file this task's branch changes, one bullet each, starting with the path in backticks.
A directory entry ends with "/" and covers everything under it. plan.py check --base fails
if the branch changes a file not covered here.
Example:  - `src/net/epoll_loop.cpp` — epoll wrapper and timer wheel
-->
- `tools/setup/install.sh` — the installer, the head of the shared file
- `tools/setup/package.sh` — makes the shared file
- `docs/logs/X-setup.md` — this log

## Decisions

<!--
Choices you made inside the card's scope, each with the reason. Not for points listed under
"Still open" in the plan: those are asked, never decided here. Write "None" if there were none.
-->
- One fixed prefix, not a choice: the same path on every node is what makes a job start.
- A prebuilt package, built once and given to every node, not a script that builds from source
  on each: the bundled programs must hash the same everywhere. It suits machines with the same
  distribution as the builder or a newer one (glibc).
- One merged static library and a pkg-config file: users link `-lparamesh` with no CMake.
- The installer copies files out of the build directory; no CMake install rules (one script, no
  build-system change).

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
- No `setup.bat`, as asked among the options: ParaMesh runs on Linux only.

## Verification

<!--
The card's Done-when check: the exact commands run, where (local, Actions, simulated nodes,
cabled laptops), and the actual result, pasted or summarised with numbers. Also the quality gate
(clang-format, clang-tidy, sanitizer runs). Required before Status: done.
-->
No card. On this machine:

- `bash -n` on both scripts: clean. `tools/setup/package.sh`: a 4.3 MB file; its payload, unpacked,
  compiles and links an outside program through `pkg-config`, serves the dashboard, stands by,
  joins on `POST /v1/join`, and runs a two-node job (sessions above).
- The install into `/opt` with sudo and the systemd user service: not run here (sudo needs a
  password the agent does not enter). The person runs it once before sharing the file.

## Deferred

<!--
Anything left undone, each also added to docs/TODO.md in this same pull request. "None" if nothing.
-->
None.
