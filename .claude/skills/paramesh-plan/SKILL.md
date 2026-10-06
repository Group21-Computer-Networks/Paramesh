---
name: paramesh-plan
description: How to work on the ParaMesh repository from its implementation plan (docs/PLAN.md, milestones M0 to M6, task cards like M1-2 or AT-1, streams, gates) and how to keep the required implementation logs in docs/logs/. Use this whenever you write or change any code, test, script or document in the ParaMesh repo; whenever someone says "start", "continue", "resume" or "pick up" a task or milestone, asks what to work on next, mentions a task ID, a milestone, a gate, a stream or the plan; before opening a pull request; and whenever docs/logs, the TODO file or a "Still open" point comes up. Use it even for a small fix to code that is already merged, because every change in this repo has to be logged against a task. It takes precedence over generic phased-implementation workflows for this repo.
---

# Working from the ParaMesh plan

ParaMesh is built by one to three coding agents in parallel branches, with a person closing each
milestone gate. Two things hold that together: the **plan** (`docs/PLAN.md`), which says exactly what
each task may do, and the **logs** (`docs/logs/`), which say exactly what was done. Agents lose context
between sessions, work on branches they cannot see from each other, and are reviewed by a person
who was not watching. The log is the only thing that survives all three, so it is not optional
paperwork: CI fails any pull request whose changes are not logged.

All commands below use the bundled script. Run it from anywhere inside the repo:

```bash
python3 .claude/skills/paramesh-plan/scripts/plan.py <command>
```

| Command | What it does |
| --- | --- |
| `status` | Every milestone, its gate, each task's state, what is waiting on what, and what is ready to start. |
| `card M1-2` | The task's row, whether its needs are met, the milestone's Streams and Gate text, and the "Still open" points it owns. |
| `new M1-2` | Creates `docs/logs/M1-2.md` from the template, prefilled from the card. Refuses if the task cannot start yet. |
| `new X-slug --title "..."` | Creates a log for approved work that fits no task card. |
| `new-gate M1` | Creates `docs/logs/M1-gate.md`. |
| `check [--base origin/main --pr-title "..."]` | Validates every log; with `--base`, also checks that this branch's changes are logged. Exit 1 on any problem. |

## Sources of truth

- `docs/PLAN.md` is the implementation plan. Where it differs from the SRS, the HLD or Doc 1, the
  plan wins; its "Changes from the SRS and HLD" table lists every difference. Do not edit the plan.
  Only the person changes it.
- `AGENTS.md` at the repo root holds the engineering rules. They are the same as the plan's
  "Engineering rules" table and apply to every pull request.
- After the M0 gate, four files are **frozen**: `include/paramesh.h`, `docs/PROTOCOL.md`,
  `docs/STATE_MACHINES.md` and `docs/INTERNAL_API.md`. Code is written against them. A change to
  one needs the person's approval first, unless the task card lists that file (M3-1 lists
  `paramesh.h` for the macro body only). Record the approval under `## Approvals` in the log.
- `docs/TODO.md` holds everything deferred.

## Doing a task

### 1. Orient

Run `status`. If a log for the task you were asked about is already `in-progress` or `blocked`,
you are resuming. Read that log fully before anything else: the last session entry says where
the work stopped and what comes next. Do not restart from scratch, and do not open a second log.

`status` only sees the current checkout. Run it on an up-to-date `main` (or after merging `main`)
so that tasks merged from other streams show as done.

### 2. Pick the task

If the person named a task, use it. If they said "next" or "continue the milestone", choose from
the **Ready** list, and say which one you picked and why before starting. Respect the plan's
stream rules. Within a stream, tasks go in the listed order, done by one agent. A `seq` task is
done alone, after the milestone's parallel streams have joined. A task that is not ready is not
started, even if it looks independent: `new` refuses it, and `check` fails any log for a task whose
needs are not done or whose previous gate is not closed.

### 3. Read before writing code

Run `card <ID>` and read all of it. Then read, from `docs/PLAN.md`:

- the "Engineering rules" table (also in `AGENTS.md`);
- every row of "Decisions this plan rests on" that touches your area (errors, trust, logging,
  configuration, and so on);
- "Tunables", if you need any number: use the configuration key, never a literal;
- the "Still open" table;
- the rows of "Changes from the SRS and HLD" that touch your area, so you don't build what the
  HLD said instead of what the plan says.

After M0, also read the frozen files your task codes against.

### 4. Start the log, then the branch

```bash
git switch -c m1-2-transport
```

```bash
python3 .claude/skills/paramesh-plan/scripts/plan.py new M1-2
```

Fill in the first session entry and commit the log on its own before you change any code. The
work is now visible to anyone who looks at the branch, even if you stop halfway.

### 5. Stay inside the card

The card's **Files** column is the task's scope. If you need to touch something outside it,
stop and decide. A small, obviously necessary change (a missing include in a sibling header, say)
goes under `## Deviations` with the reason. Anything larger, or anything another stream owns,
is a question for the person. Never edit another task's log.

### 6. Never guess. Stop and ask

The plan's rule is that a task which meets a "Still open" point stops and asks; it does not
choose. The same goes for anything the card, the plan and the frozen files leave genuinely
ambiguous. When that happens:

1. Add `- [open] <the question, the options you see, and what each would mean>` under
   `## Questions and blockers`.
2. Set `Status: blocked`, write a session entry, and commit the log.
3. Ask the person in chat. Don't keep building on an assumed answer while you wait.
4. When they answer, change the item to `- [answered 2026-10-08 by <who>] ... Answer: ...`, set the
   status back to `in-progress`, and carry on.

M0 drafting tasks work differently. M0-4 to M0-7 exist to draft answers to the Still-open
points they own (`card` lists them), and the M0 gate is where the person approves or changes
those answers. Draft the proposal in the document, mark it there as awaiting approval, and list
it in the log as `- [for gate] #<point number>: <what you proposed and the alternatives>`. A
`[for gate]` item does not block `done`, because the card itself says "answered or flagged for
the gate". It is only accepted in M0 logs. Every other task uses `[open]` and waits.

### 7. Log as you go

Write a session entry whenever you finish a meaningful step, after every test run that matters,
before you end a turn, and before anything that might lose your context. Append; never rewrite
or delete earlier entries. If an earlier entry was wrong, say so in a new one. Each entry
answers: what changed, what state the code is in, which commands ran and what they actually
printed, and what comes next. Keep `## Files touched` current as you go. CI compares it against
the branch's diff.

A useful entry looks like this:

```markdown
### 2026-10-09 — frame reassembly

- Added `FrameReader` in `src/net/frame_reader.cpp`: buffers partial reads, yields whole frames,
  rejects a length field over 2 MB + 36.
- `ctest -R net` → 14/14 pass. Under TSan: 14/14 pass. ASan flagged a read past the end on a
  0-byte payload; fixed by checking length before `memcpy`, and added `frame_reader_empty_payload`.
- Not done yet: heartbeats and the 2 s reply timer.
- Next: timer wheel in `src/net/timers.cpp`, then the 100,000-frame localhost test.
```

A bad entry says "worked on networking, tests pass". It cannot be resumed from or reviewed.

### 8. Verify against the card

Run the card's **Done when** check exactly as written, plus the quality gate (clang-format,
clang-tidy, warnings as errors, unit tests under ASan, UBSan and TSan). Under `## Verification`,
record the commands, where they ran, and the actual results with numbers. If you could not run
part of it (it needs two laptops, say), write exactly that and leave the task `in-progress`, or
ask the person whether it may be verified at the gate. Don't mark it done on the strength of
something you didn't see.

### 9. Finish

- Anything left undone: add it to `docs/TODO.md` **and** list it under `## Deferred`, in this
  same pull request.
- Fill in `## Decisions`, `## Approvals`, `## Deviations` and `## Deferred`. Write `None` where
  there is nothing to say; an empty section fails the check when the status is done.
- Set `Status: done` and `Finished:` to today's date.
- Run the check exactly as CI will and fix everything it reports:

```bash
python3 .claude/skills/paramesh-plan/scripts/plan.py check --base origin/main --pr-title "M1-2: transport (epoll, TCP mesh, heartbeats)"
```

- Open the pull request with the task ID at the start of the title (`M1-2: ...`). One task is
  one pull request.

## Gates

A gate is closed by the person, never by an agent. When the milestone's tasks are done you may
prepare the gate: run `new-gate M1`, add one row per acceptance check named in the gate text,
run the checks you can run (simulated nodes, scripts), and record each result and its evidence.
Hardware runs, approvals and answers to open points are filled in from what the person reports.
Set `Status: closed`, `Closed by:` and `Closed on:` only when the person tells you the gate is
closed, using the name and date they give. The check refuses a closed gate while any check is
`fail` or `pending` or any task of the milestone is not done. At the M0 gate, record every
Still-open answer under `## Answers to open points` and where it now lives.

## Other kinds of work

- **A fix to a task that is already merged.** Add a new session to that task's log (status stays
  `done`), add any new files to `## Files touched`, and title the pull request `M1-2 fix: ...`.
  A bug fix also adds the test that would have caught it.
- **Work that fits no task card** (tooling, a CI repair, a plan correction the person asked for).
  Ask the person first. If they agree, run `new X-short-slug --title "..."`, record who approved
  it and why in `## Card`, and title the pull request `X-short-slug: ...`.
- **Questions with no code change** need no log. Anything that changes a file in the repo does.

## Bootstrapping in M0-1

Before M0-1 there is no repo, no `docs/PLAN.md` and no `docs/logs/`, so the script cannot run yet.
M0-1 sets them up as its first steps:

1. Copy the plan into the repo as `docs/PLAN.md`, unchanged.
2. Create `docs/logs/` and copy `assets/logs-README.md` (in this skill folder) to
   `docs/logs/README.md`.
3. Run `new M0-1` and continue as for any task.
4. When creating the GitHub Actions workflow, add the job in `assets/ci-logs-job.yml`, so every
   pull request runs the log check.
5. In `AGENTS.md`, include a short "Implementation logs" section that states the rules above in
   brief and points to `docs/logs/README.md`. Not every coding agent loads Claude skills;
   `AGENTS.md` is how the others learn the rule, and CI is how it is enforced for all of them.
6. List the skill folder `.claude/skills/paramesh-plan/` under `## Files touched` of M0-1.

## What the check enforces

It's useful to know exactly what will fail, so you can fix it before CI does:

- Log names are `M1-2.md`, `AT-1.md`, `M1-gate.md`, `X-slug.md` or `README.md`, with no
  subdirectories. Header fields and section headings match the template. The Task, Milestone and
  Stream fields agree with the plan.
- A logged task's needs are done and its previous milestone's gate is closed.
- Every session heading starts with a date. Every question is `[open]`,
  `[answered <date> by <who>]` or, in M0 only, `[for gate]`. A `blocked` log has an open question. A `done` log has none, and
  it has a Finished date, a Verification section, at least one file touched, and every other
  section filled in or marked `None`.
- With `--base`, if any file outside `docs/logs/` changed, a task or X log changed too. Every
  changed file is covered by `## Files touched` in an updated log (a path ending in `/` covers its
  directory). The PR title names the task or gate, and that log was updated. After the M0 gate, a
  frozen file changed by a task whose card doesn't list it needs an `## Approvals` entry naming it.

The check cannot tell whether a log is true. It is your job to make sure it is. Results in a log
are results you saw.
