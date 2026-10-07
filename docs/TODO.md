# Pending work

Everything deferred lives here. A pull request that leaves something undone adds it here and
lists it under "Deferred" in its log. Remove an item in the pull request that does it, and say so
in that log.

## From the plan

These are deferred by `docs/PLAN.md` itself. The "Plan reference" column names the row that defers them.

| Item | Plan reference | Revisit when |
| --- | --- | --- |
| Get a gigabit switch so three or more laptops can be tested together. | How to use this plan, "Three-laptop runs" | Now; it gates the items below that need three laptops. |
| Repeat and record the tests the SRS words as "3 laptops", including the 2 to 2.5 times matrix-multiply speed-up (FR-9.2). | How to use this plan; M3 gate; Changes #28 | A switch is available. |
| Security work beyond the ledger fix (the ledger fix itself is M5). | Decisions, "Security" | After this semester, or when the person schedules it. |
| Probation weight for a peer seen for the first time: start at balance 0 (weight 1) or lower. | Still open #16 | With the security work above. |
| Stride prefetcher after 3 sequential faults. | Decisions, "Stride prefetcher"; Changes #22 | After the M3 speed-up is measured. |
| The five demo workloads beyond counter, matrix multiply and the false-sharing demo (named in Doc 1). | Decisions, "Workloads"; Changes #29 | When the person schedules them. |
| UC-3 stencil workload and its test. | Changes #29; Requirement traceability | With the workloads above. |
| MPI comparison, and so a test for NFR-1 (transparency). | Changes #29; Requirement traceability | With the workloads above. |
| FR-10.2 crash recovery (Future in the SRS). | Requirement traceability | Not planned this semester. |
| M6 "novelty features" from the SRS. | Decisions, "M6 novelty features"; M6 gate | Once the person defines them. |

## Added by tasks

| Item | Added by | Notes |
| --- | --- | --- |
| Run `tools/check_env.sh` on the other five laptops, and have the person see `m0_demo` run. | M0 gate | Waived at the gate on 2026-10-07; only `ouroboros` has run it. Record the results in `docs/logs/M0-gate.md`. |
| Run `readshare` on two cabled laptops (the M1 gate's hardware check for FR-4.1 and FR-4.2). | M1 gate | Waived at the gate on 2026-10-08. The commands are in `docs/logs/M1-gate.md`, "Hardware runs"; record the result there. |
