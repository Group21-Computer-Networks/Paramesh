# Implementation logs

This directory is the record of what has been built, by which task, and how it was checked.
It is how agents on separate branches hand work to each other, and how the person reviews a
milestone at its gate. CI fails any pull request whose changes are not logged here.

## Files

| File | What it records | Written by |
| --- | --- | --- |
| `M1-2.md`, `AT-1.md` | One task card from `docs/PLAN.md`: sessions, files touched, decisions, questions, verification. | The agent doing the task, in the task's own pull request. |
| `M1-gate.md` | One milestone gate: the acceptance checks, hardware runs, answers to open points. | Prepared by an agent; closed only by the person. |
| `X-short-slug.md` | Work the person approved that fits no task card. | The agent doing it. |

One file per task means agents in parallel streams never edit the same log, so their branches
never conflict here. There is no shared index; run the status command instead.

## Commands

```bash
python3 .claude/skills/paramesh-plan/scripts/plan.py status
```

```bash
python3 .claude/skills/paramesh-plan/scripts/plan.py card M1-2
```

```bash
python3 .claude/skills/paramesh-plan/scripts/plan.py new M1-2
```

```bash
python3 .claude/skills/paramesh-plan/scripts/plan.py check --base origin/main --pr-title "M1-2: transport"
```

`new` refuses a task whose needs are not done or whose previous gate is not closed.
`check` validates every log, and with `--base` also checks that every file changed on the branch
is listed under "Files touched" in an updated log and that the pull-request title names the task.

The templates and the full rules live in `.claude/skills/paramesh-plan/`.
