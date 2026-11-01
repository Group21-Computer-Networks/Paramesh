# X-playground: the website and the C playground

- Task: X-playground
- Milestone: none
- Stream: -
- Status: in-progress
- Branch: x-website
- Started: 2026-11-01
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
No task card. The person asked for it on 2026-11-02 in a long brief: a redesigned website, and a C playground that runs visitors' programs on the real runtime, with real statistics and isolation. They chose a LAN with an access code, and plain HTML with a Python standard-library server.

## Sessions

<!--
One entry per working session, newest last. Write it as you go, not at the end:
another agent (or you, after a context reset) resumes from this log alone.
Each entry: what you did, what state the code is in, the commands you ran and what they printed,
and the next step.
-->

### 2026-11-01 — session 1

- Read the dashboard, the status API, `pmrun`, `pmd`'s spawn and the job's start-up. A wrapper or
  a container cannot sandbox a job: `pmd` keeps the process ID it forked, compares the hash of the
  file at the path, and each process hashes its own executable. So the sandbox goes in before
  `exec`, in `pmrun` and `pmd`, for programs under `/var/tmp/paramesh-play/`.
- `src/platform/sandbox.cpp`: Landlock, seccomp, rlimits. Found on the way:
  - The sandbox unit test showed the ephemeral range holds pmd's 47001 and 47002, which would
    have let a job send `SPAWN_REQ`. 47000-47099 are now left out.
  - The runtime needs `process_vm_readv` on itself; it is allowed for this process only.
  - The spill file went beside `pmd.sock`. `PARAMESH_CFG_SPILL_DIR` puts a job's in its directory.
  - A job could ask `pmd` to start any program, outside the sandbox. `pmd` now refuses
    `L_RUN_REQ` and `L_LEAVE_REQ` from a peer whose executable is under the path (SO_PEERCRED).
- `tools/playground/server.py`: compile under `pm sandbox`, static, so one file runs on both
  Ubuntu versions; stage to peers; run with `pmrun`; collect `job_pages` from each process.
- `tools/site/`: the home page and the playground. Impeccable's flow was followed (PRODUCT.md,
  concept seed, craft floor). The direction is an exhibition catalogue, within the person's pinned
  palette. The Taste skill the brief names does not exist in this account: not used.
- `tests/multi/run_playground.sh`: 40 checks through the page's own requests: all 7 examples on 1
  and 2 nodes, two nodes running tasks, compile errors, reading host files, fork, exec, pmd's
  port, a crash, the time limit, a refused region, a job asking for a job, the access code, bad
  requests, a second run, Stop, cleanup, history without source, live figures. PASS, twice.
- Seen in a browser at desktop and 375 px: both pages, Monaco, a two-node run, the analytics.
- At the person's request, `/dashboard`: all of the page on port 47002, in the site's design, from
  `GET /api/node`. Join and Leave work only from the machine itself. 8 more checks in the test.
- At the person's request, the site was checked against the plan's "Decisions this plan rests on"
  (there is no file called DECISIONS.md; asked to confirm). Now shown: the launcher holds the
  queue, locks and barriers; peers with the most free capacity, which may decline; a node's own
  jobs first; the ledger's rule that nothing a peer reports counts. Two things differ from those
  decisions, by the person's choice: the server is Python, where the plan says C++ for tools, and
  Monaco and fonts come from a CDN, where the plan vendors third-party code.
- At the person's request, a laptop can join just to give its cores: an invite code on a joined
  node's dashboard, entered on the new laptop's. It brings the node token and the lead address.
  It is a convenience, not a lock on the pool. The live view adds CPU and RAM for the machine and
  for ParaMesh's job processes, from /proc, with the last 60 seconds. 10 more checks.
- The new test found why discovery was one-way on the real laptops: `pmd` beacons to multicast and
  its seeds only, so a node that found this one by address, across subnets, heard nothing back.
  `pmd` now beacons to each new peer's source address too, on a list kept apart from job choice.
- At the person's request: their palette of blue, tan and olive, with the tones chosen here, and the
  fonts Prata and Karla, a pairing from a public Pinterest pin. Pinterest's own search needs a login,
  so it came through a web search. The installer now runs the server as `paramesh-site`, and the
  access code is kept across restarts. `install.sh` itself has not been run: it needs sudo.
- The person's reference, a Cosmos gallery, moved the "What is ParaMesh?" wall to drawings in full-bleed
  frames with captions below, near-square corners and no shadows.
- CI's tidy failed on `Pmd::hear` at cognitive complexity 41: `answer_beacons` now holds the list.
  `cmake --build --preset tidy --target pmd` printed `[7/7] Linking CXX executable src/pmd/pmd`.
- At the person's request, motion: the headline rises, the hero's code types itself, content rises as
  it scrolls in, the live figures count up to their real value, pages fade. Off under reduced motion.
- NOT tested: the real two laptops. The new `pmd` and server need installing on both, which needs
  the person's sudo. Running on 127.0.0.1, peers skip staging; POST /node/stage is unexercised.
- Next: the person installs on both laptops and runs a two-node job.

## Files touched

<!--
Every file this task's branch changes, one bullet each, starting with the path in backticks.
A directory entry ends with "/" and covers everything under it. plan.py check --base fails
if the branch changes a file not covered here.
Example:  - `src/net/epoll_loop.cpp` — epoll wrapper and timer wheel
-->
- `src/platform/sandbox.cpp` — the sandbox
- `src/platform/sandbox.h` — the sandbox
- `src/pmd/pmd.cpp` — sandboxes jobs, refuses requests from them, answers new peers' beacons
- `src/pmd/pmd.h` — `from_sandbox`
- `src/tools/pmrun.cpp` — sandboxes the launcher
- `src/tools/pm.cpp` — `pm sandbox`
- `src/lib/job.cpp` — `PARAMESH_CFG_SPILL_DIR`
- `tests/unit/platform_test.cpp` — the sandbox's limits
- `tests/multi/run_playground.sh` — end to end
- `tests/multi/playground_checks.py` — end to end
- `tools/playground/server.py` — the server
- `tools/site/` — the website
- `tools/setup/package.sh` — packs them
- `tools/setup/install.sh` — says where they are
- `docs/PLAYGROUND.md` — how to run it, and its limits
- `docs/TODO.md` — what is left
- `README.md` — a link
- `PRODUCT.md` — product context for design work

## Decisions

<!--
Choices you made inside the card's scope, each with the reason. Not for points listed under
"Still open" in the plan: those are asked, never decided here. Write "None" if there were none.
-->
- Landlock, seccomp and rlimits rather than containers: see the session.
- A static binary, so a build on Ubuntu 26.04 runs on 24.04.
- Monaco and the fonts come from CDNs, with a plain text box as the editor offline. Vendoring
  Monaco would add about 15 MB to the repository.
- One run at a time: a second gets 409.
- Runs are recorded with a hash of the source, never the source.

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
None. No frozen file changed.

## Deviations

<!--
Anything that differs from the card: a file outside its Files list, a changed approach, a Done-when
check run differently. Each with the reason and who agreed. "None" if the card was followed exactly.
-->
- The brief's Taste skill was not available.
- Code formatting in the editor is left out: Monaco has none for C. In docs/TODO.md.

## Verification

<!--
The card's Done-when check: the exact commands run, where (local, Actions, simulated nodes,
cabled laptops), and the actual result, pasted or summarised with numbers. Also the quality gate
(clang-format, clang-tidy, sanitizer runs). Required before Status: done.
-->
Local, 2026-11-02: clang-format clean; `ctest` with `dev`, `asan` and `ubsan` 16/16 each, `tsan`
15/15; `tidy` first found 6 style points in the new code, fixed, then exit 0. Those fixes came after
the sanitizer runs: `dev` and `tests/multi/run_playground.sh` were repeated, and CI runs the rest.
`tests/multi/run_playground.sh`: PASS, 48 checks, on two daemons on one machine.
Not done, so this log stays in-progress: a run on the two real laptops.

## Deferred

<!--
Anything left undone, each also added to docs/TODO.md in this same pull request. "None" if nothing.
-->
- A run on the two real laptops; figures nothing reports yet; a C formatter; a service unit.
  All in docs/TODO.md.
