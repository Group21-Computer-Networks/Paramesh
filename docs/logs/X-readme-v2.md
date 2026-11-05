# X-readme-v2: a detailed README with a diagram for each feature

- Task: X-readme-v2
- Milestone: none
- Stream: -
- Status: done
- Branch: x-readme-v2
- Started: 2026-11-05
- Finished: 2026-11-05

## Card

Approved by Amirishetty Sai Vignesh in chat, 2026-11-05: "make a very good detailed sophisticated read me and push it".

## Sessions

### 2026-11-05 — session 1

- `README.md` rewritten: highlights, quick start, step-by-step description, a section per feature area (shared memory, the pool, running the work, making it fast, safety) with a diagram each, the full `paramesh.h` API with the `apps/quickstart` program, the translator's rules, the website with screenshots of the playground and dashboard, commands and ports, measured results of each speed feature from the X- logs, a comparison with similar systems, known limits, build, layout, testing, documentation and the team's contributions as the group agreed them.
- Diagrams made in the diagram-design style with the report's palette (`report/diagrams/features.py`, not in the repo), copied as PNG to `docs/images/`; screenshots of the installed site to `docs/images/screens/`. The dashboard screenshot has its invite and access codes blurred.
- Every number in the results tables is from a log: X-prefetch, X-page-encoding, X-adaptive-coherence, X-locality-sched, X-rehome, M5 gate (fairness), and the two-laptop runs in X-readme-verified.

## Files touched

- `README.md` — rewritten
- `docs/images/` — feature diagrams and screenshots
- `docs/logs/X-readme-v2.md` — this log

## Decisions

- Known limits kept short and limited to what a user must know before installing: trusted network, crashes, page-heavy work over Wi-Fi, fairness by laptop, translator scope, no per-job memory cap.

## Questions and blockers

None

## Approvals

None

## Deviations

None

## Verification

Documentation only. `plan.py check --base origin/main --pr-title "X-readme-v2: ..."` run before the pull request; result recorded in the next session.

## Deferred

None
