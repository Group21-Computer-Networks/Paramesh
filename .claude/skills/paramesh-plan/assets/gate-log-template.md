# {{MILESTONE}} gate

- Milestone: {{MILESTONE}}
- Status: open
- Closed by:
- Closed on:

<!--
Only the person closes a gate. An agent may prepare this file and fill in checks it ran,
but sets "Status: closed", "Closed by" and "Closed on" only when the person says the gate is
closed, using the name and date they give.
plan.py check refuses a closed gate while any check is fail or pending or any task in this
milestone is not done.

The plan's gate text:
{{GATE}}
-->

## Checks

<!--
One row per acceptance check named in the gate text. Result is one of: pass, fail, pending, n/a.
Evidence: the command, the log or result file, the trace, or "seen by <who>".
-->

| Check | Setup | Result | Evidence |
| --- | --- | --- | --- |
|  | three simulated nodes | pending |  |
|  | two cabled laptops | pending |  |

## Hardware runs

<!-- Which laptops, kernel versions, the cable or switch, and anything unusual. -->

## Answers to open points

<!--
Each "Still open" point or flagged question the person answered at this gate:
the point's number, the answer, and where it is now recorded (the frozen file, AGENTS.md, docs/TODO.md).
"None" if none.
-->

## Follow-ups

<!-- Items for docs/TODO.md or for the next milestone that came out of the review. "None" if none. -->
