#!/usr/bin/env python3
"""ParaMesh plan and implementation-log tool.

Reads the task cards from docs/PLAN.md and the logs in docs/logs/.

  plan.py status              milestones, gates, task states, what is ready to start
  plan.py card M1-2           the task card plus its milestone's Streams and Gate text
  plan.py new M1-2            create docs/logs/M1-2.md (refused if its needs are not met)
  plan.py new X-fix-crc       create a log for approved work outside any task card
  plan.py new-gate M1         create docs/logs/M1-gate.md
  plan.py check               validate every log; exit 1 on any error
  plan.py check --base origin/main --pr-title "M1-2: transport"
                              also check that the changes on this branch are logged

Python 3.8+, standard library only, so it runs unchanged in GitHub Actions.
"""

import argparse
import datetime
import re
import subprocess
import sys
from pathlib import Path

SKILL_DIR = Path(__file__).resolve().parent.parent
ASSETS = SKILL_DIR / "assets"

TASK_ID = r"(?:M[0-6]-\d+|AT-\d+)"
EXTRA_ID = r"X-[a-z0-9][a-z0-9-]*"
TASK_FILE = re.compile(rf"^({TASK_ID})\.md$")
EXTRA_FILE = re.compile(rf"^({EXTRA_ID})\.md$")
GATE_FILE = re.compile(r"^(M[0-6])-gate\.md$")
DATE = re.compile(r"^\d{4}-\d{2}-\d{2}$")
STATUSES = ("in-progress", "blocked", "done")
GATE_STATUSES = ("open", "closed")
CHECK_RESULTS = ("pass", "fail", "pending", "n/a")
FROZEN = ("include/paramesh.h", "docs/PROTOCOL.md", "docs/STATE_MACHINES.md", "docs/INTERNAL_API.md")

TASK_FIELDS = ("Task", "Milestone", "Stream", "Status", "Branch", "Started", "Finished")
TASK_SECTIONS = ("Card", "Sessions", "Files touched", "Decisions", "Questions and blockers",
                 "Approvals", "Deviations", "Verification", "Deferred")
GATE_FIELDS = ("Milestone", "Status", "Closed by", "Closed on")
GATE_SECTIONS = ("Checks", "Hardware runs", "Answers to open points", "Follow-ups")


# ---------------------------------------------------------------- plan parsing

class Task:
    def __init__(self, tid, title, files, needs_text, stream, done_when, milestone):
        self.id = tid
        self.title = title
        self.files = files
        self.needs_text = needs_text
        self.stream = stream
        self.done_when = done_when
        self.milestone = milestone
        self.needs = re.findall(rf"{TASK_ID}|M[0-6] gate", needs_text)

    @property
    def post_gate(self):
        # AT tasks follow a gate rather than belonging to a milestone's own work.
        return self.id.startswith("AT-")


class Plan:
    def __init__(self, path):
        self.path = path
        self.text = path.read_text(encoding="utf-8")
        self.tasks = {}
        self.order = []
        self.milestones = {}   # "M1" -> {"heading", "intro", "streams", "gate", "extra"}
        self.still_open = []   # (number, point, settled_in)
        self._parse()

    def _parse(self):
        milestone = None
        section = None
        prev_blank_para = []
        for line in self.text.splitlines():
            h2 = re.match(r"^## (.+)$", line)
            if h2:
                section = h2.group(1).strip()
                m = re.match(r"^(M[0-6]):", section)
                milestone = m.group(1) if m else None
                if milestone:
                    self.milestones[milestone] = {"heading": section, "intro": "", "streams": "",
                                                  "gate": "", "extra": []}
                continue
            if not line.startswith("|"):
                if milestone and line.strip():
                    ms = self.milestones[milestone]
                    if line.startswith("**Streams.**"):
                        ms["streams"] = line
                    elif line.startswith("**Gate.**"):
                        ms["gate"] = line
                    elif not ms["intro"] and not line.startswith("#"):
                        ms["intro"] = line
                    elif not line.startswith("#"):
                        ms["extra"].append(line)
                continue
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            if milestone and len(cells) == 6 and re.fullmatch(TASK_ID, cells[0]):
                t = Task(cells[0], cells[1], cells[2], cells[3], cells[4], cells[5], milestone)
                self.tasks[t.id] = t
                self.order.append(t.id)
            elif section == "Still open" and len(cells) == 4 and cells[0].isdigit():
                self.still_open.append((cells[0], cells[1], cells[3]))

    def milestone_tasks(self, ms):
        return [self.tasks[i] for i in self.order if self.tasks[i].milestone == ms and not self.tasks[i].post_gate]


# ---------------------------------------------------------------- log parsing

def strip_comments(text):
    return re.sub(r"<!--.*?-->", "", text, flags=re.S)


class Log:
    def __init__(self, path):
        self.path = path
        self.name = path.name
        raw = path.read_text(encoding="utf-8")
        text = strip_comments(raw)
        head, _, _ = text.partition("\n## ")
        self.fields = {}
        for line in head.splitlines():
            m = re.match(r"^- ([A-Za-z ]+):\s*(.*)$", line)
            if m:
                self.fields[m.group(1).strip()] = m.group(2).strip()
        self.sections = {}
        parts = re.split(r"^## (.+)$", text, flags=re.M)
        for i in range(1, len(parts), 2):
            self.sections[parts[i].strip()] = parts[i + 1].strip()

    def field(self, key):
        return self.fields.get(key, "")

    def section(self, key):
        return self.sections.get(key, "")

    def bullets(self, key):
        return [l.strip()[2:].strip() for l in self.section(key).splitlines() if l.strip().startswith("- ")]

    def files_touched(self):
        paths = []
        for b in self.bullets("Files touched"):
            m = re.match(r"^`([^`]+)`", b)
            if m:
                p = m.group(1).strip()
                paths.append("." if p in (".", "./") else p[2:] if p.startswith("./") else p)
        return paths


def is_none(body):
    return body.strip().rstrip(".").lower() in ("none", "")


# ---------------------------------------------------------------- repo helpers

def repo_root(arg):
    if arg:
        return Path(arg).resolve()
    try:
        out = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True, check=True)
        return Path(out.stdout.strip())
    except (subprocess.CalledProcessError, FileNotFoundError):
        return Path.cwd()


def load(args):
    root = repo_root(args.root)
    plan_path = root / (args.plan or "docs/PLAN.md")
    if not plan_path.exists():
        sys.exit(f"error: plan not found at {plan_path}. M0-1 copies the plan there; pass --plan to override.")
    plan = Plan(plan_path)
    if not plan.tasks:
        sys.exit(f"error: no task rows found in {plan_path}; the table format may have changed.")
    logs_dir = root / "docs" / "logs"
    logs = {}
    if logs_dir.is_dir():
        for p in sorted(logs_dir.iterdir()):
            if p.is_file() and p.suffix == ".md" and p.name != "README.md":
                logs[p.name] = Log(p)
    return root, plan, logs_dir, logs


def task_status(logs, tid):
    log = logs.get(f"{tid}.md")
    return log.field("Status") if log else "not started"


def gate_status(logs, ms):
    log = logs.get(f"{ms}-gate.md")
    return log.field("Status") if log else "not started"


def need_met(logs, need):
    if need.endswith(" gate"):
        return gate_status(logs, need.split()[0]) == "closed"
    return task_status(logs, need) == "done"


def prev_gate(ms):
    n = int(ms[1:])
    return f"M{n - 1}" if n > 0 else None


def blockers_to_start(plan, logs, task):
    """Reasons this task may not be started yet; empty when it may."""
    needs = list(task.needs)
    pg = prev_gate(task.milestone)
    if not task.post_gate and pg and f"{pg} gate" not in needs:
        needs.insert(0, f"{pg} gate")
    reasons = [f"{n} not closed" if n.endswith("gate") else f"needs {n} (not done)"
               for n in needs if not need_met(logs, n)]
    # A lettered stream is one agent working through its tasks in the listed order.
    if task.stream != "seq":
        for tid in plan.order:
            if tid == task.id:
                break
            t = plan.tasks[tid]
            if (t.milestone, t.stream) == (task.milestone, task.stream) and tid not in task.needs \
                    and task_status(logs, tid) != "done":
                reasons.append(f"stream {task.stream} runs in order: {tid} not done")
                break
    return reasons


# ---------------------------------------------------------------- commands

def cmd_status(args):
    _, plan, _, logs = load(args)
    ready = []
    for ms in sorted(plan.milestones):
        print(f"\n{plan.milestones[ms]['heading']}    [gate: {gate_status(logs, ms)}]")
        for tid in plan.order:
            t = plan.tasks[tid]
            if t.milestone != ms:
                continue
            st = task_status(logs, tid)
            note = ""
            if st == "not started":
                b = blockers_to_start(plan, logs, t)
                if b:
                    note = "  waiting: " + "; ".join(b)
                else:
                    note = "  READY"
                    ready.append(t)
            print(f"  {tid:<6} {st:<12} stream {t.stream:<4} {t.title[:60]}{note}")
    extras = sorted(n for n in logs if EXTRA_FILE.match(n))
    if extras:
        print("\nWork outside task cards:")
        for n in extras:
            print(f"  {n[:-3]:<24} {logs[n].field('Status')}")
    active = [n[:-3] for n, l in logs.items() if l.field("Status") in ("in-progress", "blocked")]
    print("\nIn progress or blocked (resume these first):", ", ".join(sorted(active)) or "none")
    print("Ready to start:", ", ".join(f"{t.id} ({t.stream})" for t in ready) or "none")
    print("\nThis reflects the current checkout only. Work on other branches is not visible until merged.")


def cmd_card(args):
    _, plan, _, logs = load(args)
    tid = args.id
    if tid not in plan.tasks:
        sys.exit(f"error: {tid} is not a task in {plan.path}")
    t = plan.tasks[tid]
    ms = plan.milestones[t.milestone]
    print(f"{t.id}  ({ms['heading']})")
    print(f"Status: {task_status(logs, tid)}\n")
    print(f"Task:      {t.title}")
    print(f"Files:     {t.files}")
    print(f"Needs:     {t.needs_text}")
    for n in t.needs:
        print(f"           - {n}: {'met' if need_met(logs, n) else 'NOT MET'}")
    print(f"Stream:    {t.stream}")
    print(f"Done when: {t.done_when}\n")
    print(f"Milestone goal: {ms['intro']}\n")
    if ms["streams"]:
        print(ms["streams"] + "\n")
    if ms["gate"]:
        print(ms["gate"] + "\n")
    for line in ms["extra"]:
        if tid in line or t.post_gate:
            print(line + "\n")
    owned = [s for s in plan.still_open if tid in s[2] or (t.milestone + " gate") in s[2]]
    if owned:
        print("Still-open points settled by this task or its gate (do not choose; draft and flag, or ask):")
        for num, point, settled in owned:
            print(f"  #{num} [{settled}] {point}")
    b = blockers_to_start(plan, logs, t)
    if b and task_status(logs, tid) == "not started":
        print("\nNOT READY: " + "; ".join(b))


def render(template, values):
    text = (ASSETS / template).read_text(encoding="utf-8")
    for k, v in values.items():
        text = text.replace("{{" + k + "}}", v)
    return text


def cmd_new(args):
    root, plan, logs_dir, logs = load(args)
    tid = args.id
    today = datetime.date.today().isoformat()
    path = logs_dir / f"{tid}.md"
    if path.exists():
        sys.exit(f"error: {path.relative_to(root)} already exists; add a session to it instead.")
    if re.fullmatch(EXTRA_ID, tid):
        values = {"ID": tid, "TITLE": args.title or "work outside the task cards", "MILESTONE": "none",
                  "STREAM": "-", "BRANCH": tid.lower(), "TODAY": today,
                  "CARD": "No task card. Approved by <who> on <date> because <reason>.",
                  "FILES": "", "NEEDS": "", "DONE": ""}
    elif tid in plan.tasks:
        t = plan.tasks[tid]
        b = blockers_to_start(plan, logs, t)
        if b:
            sys.exit(f"error: {tid} cannot start yet: " + "; ".join(b))
        values = {"ID": tid, "TITLE": t.title.split(":")[0].split(".")[0][:70], "MILESTONE": t.milestone,
                  "STREAM": t.stream, "BRANCH": tid.lower(), "TODAY": today,
                  "CARD": f"- Goal: {t.title}\n- Files: {t.files}\n- Needs: {t.needs_text}\n"
                          f"- Stream: {t.stream}\n- Done when: {t.done_when}",
                  "FILES": t.files, "NEEDS": t.needs_text, "DONE": t.done_when}
    else:
        sys.exit(f"error: {tid} is neither a task in the plan nor an X-<slug> id.")
    try:
        branch = subprocess.run(["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=root,
                                capture_output=True, text=True, check=True).stdout.strip()
        if branch not in ("main", "master", "HEAD"):
            values["BRANCH"] = branch
    except (subprocess.CalledProcessError, FileNotFoundError):
        pass
    logs_dir.mkdir(parents=True, exist_ok=True)
    path.write_text(render("task-log-template.md", values), encoding="utf-8")
    print(f"created {path.relative_to(root)}")


def cmd_new_gate(args):
    root, plan, logs_dir, logs = load(args)
    ms = args.milestone
    if ms not in plan.milestones:
        sys.exit(f"error: {ms} is not a milestone in the plan")
    path = logs_dir / f"{ms}-gate.md"
    if path.exists():
        sys.exit(f"error: {path.relative_to(root)} already exists")
    logs_dir.mkdir(parents=True, exist_ok=True)
    path.write_text(render("gate-log-template.md", {"MILESTONE": ms, "GATE": plan.milestones[ms]["gate"],
                                                    "TODAY": datetime.date.today().isoformat()}),
                    encoding="utf-8")
    print(f"created {path.relative_to(root)}")


# ---------------------------------------------------------------- check

def check_task_log(plan, logs, log, errors):
    def err(msg):
        errors.append(f"{log.name}: {msg}")

    stem = log.name[:-3]
    is_extra = bool(EXTRA_FILE.match(log.name))
    missing = [f"missing header field '- {f}:'" for f in TASK_FIELDS if f not in log.fields]
    missing += [f"missing section '## {s}'" for s in TASK_SECTIONS if s not in log.sections]
    for m in missing:
        err(m)
    if missing:
        return  # the rest of the checks assume the template's structure
    if log.field("Task") != stem:
        err(f"Task field '{log.field('Task')}' does not match the file name")
    status = log.field("Status")
    if status not in STATUSES:
        err(f"Status '{status}' must be one of {', '.join(STATUSES)}")
    if not DATE.match(log.field("Started")):
        err("Started must be a date YYYY-MM-DD")
    if not is_extra:
        t = plan.tasks.get(stem)
        if not t:
            err(f"{stem} is not a task in the plan")
            return
        if log.field("Milestone") != t.milestone:
            err(f"Milestone '{log.field('Milestone')}' but the plan puts {stem} in {t.milestone}")
        if log.field("Stream") != t.stream:
            err(f"Stream '{log.field('Stream')}' but the plan says {t.stream}")
        for b in blockers_to_start(plan, logs, t):
            err(f"task is logged but cannot have started: {b}")

    sessions = re.findall(r"^### (\d{4}-\d{2}-\d{2})\b.*$", log.section("Sessions"), flags=re.M)
    if not sessions:
        err("Sessions needs at least one '### YYYY-MM-DD ...' entry")
    for body in re.split(r"^### .*$", log.section("Sessions"), flags=re.M)[1:]:
        if not body.strip():
            err("a session entry is empty")
            break

    for b in log.bullets("Files touched"):
        if not re.match(r"^`[^`]+`", b):
            err(f"Files touched entry must start with a `path`: '{b[:60]}'")

    questions = log.bullets("Questions and blockers")
    open_q = 0
    for q in questions:
        if q.startswith("[open]"):
            open_q += 1
        elif q.startswith("[for gate]"):
            if not log.field("Task").startswith("M0-"):
                err("[for gate] is only for M0 drafting tasks; elsewhere use [open] and ask")
        elif not re.match(r"^\[answered \d{4}-\d{2}-\d{2} by [^\]]+\]", q):
            err(f"question must start with [open], [for gate] or [answered YYYY-MM-DD by who]: '{q[:60]}'")
    if not questions and not is_none(log.section("Questions and blockers")):
        err("Questions and blockers must be 'None' or a list of [open]/[for gate]/[answered ...] items")

    if status == "blocked" and open_q == 0:
        err("Status is blocked but no question is [open]")
    if status == "done":
        if open_q:
            err(f"Status is done but {open_q} question(s) are still [open]")
        if not DATE.match(log.field("Finished")):
            err("Status is done but Finished is not a date YYYY-MM-DD")
        if is_none(log.section("Verification")):
            err("Status is done but Verification is empty; record the Done-when check and its actual result")
        if not log.files_touched():
            err("Status is done but Files touched lists nothing")
        for s in ("Decisions", "Approvals", "Deviations", "Deferred"):
            if not log.section(s).strip():
                err(f"Status is done but '{s}' is empty; write 'None' if there is nothing")


def check_gate_log(plan, logs, log, errors):
    def err(msg):
        errors.append(f"{log.name}: {msg}")

    ms = log.name.split("-")[0]
    for f in GATE_FIELDS:
        if f not in log.fields:
            err(f"missing header field '- {f}:'")
    for s in GATE_SECTIONS:
        if s not in log.sections:
            err(f"missing section '## {s}'")
    if log.field("Milestone") != ms:
        err(f"Milestone field '{log.field('Milestone')}' does not match the file name")
    status = log.field("Status")
    if status not in GATE_STATUSES:
        err(f"Status '{status}' must be open or closed")
    rows = []
    for line in log.section("Checks").splitlines():
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if not line.strip().startswith("|") or len(cells) < 4 or set(cells[0]) <= set("-: ") or cells[0] == "Check":
            continue
        rows.append(cells)
        if cells[2].lower() not in CHECK_RESULTS:
            err(f"check '{cells[0][:40]}' result '{cells[2]}' must be one of {', '.join(CHECK_RESULTS)}")
    if status == "closed":
        if not log.field("Closed by") or "<" in log.field("Closed by"):
            err("gate is closed but 'Closed by' does not name the person who closed it")
        if not DATE.match(log.field("Closed on")):
            err("gate is closed but 'Closed on' is not a date YYYY-MM-DD")
        if not rows:
            err("gate is closed but no checks are recorded")
        bad = [r[0] for r in rows if r[2].lower() in ("fail", "pending")]
        if bad:
            err("gate is closed with failing or pending checks: " + "; ".join(b[:40] for b in bad))
        undone = [t.id for t in plan.milestone_tasks(ms) if task_status(logs, t.id) != "done"]
        if undone:
            err("gate is closed but these tasks are not done: " + ", ".join(undone))


def changed_files(root, base):
    def git(*a):
        return subprocess.run(["git", *a], cwd=root, capture_output=True, text=True, check=True).stdout
    try:
        mb = git("merge-base", base, "HEAD").strip()
        files = set(git("diff", "--name-only", mb).split())
        files |= set(git("ls-files", "--others", "--exclude-standard").split())
    except subprocess.CalledProcessError as e:
        sys.exit(f"error: git failed comparing with {base}: {e.stderr.strip()}")
    return sorted(files)


def covered(path, entries):
    for e in entries:
        if e == "." or path == e or (e.endswith("/") and path.startswith(e)):
            return True
    return False


def check_diff(root, plan, logs, args, errors):
    files = changed_files(root, args.base)
    log_prefix = "docs/logs/"
    code = [f for f in files if not f.startswith(log_prefix)]
    changed_logs = [f[len(log_prefix):] for f in files
                    if f.startswith(log_prefix) and "/" not in f[len(log_prefix):]]
    work_logs = [n for n in changed_logs if (TASK_FILE.match(n) or EXTRA_FILE.match(n)) and n in logs]
    gate_logs = [n for n in changed_logs if GATE_FILE.match(n)]

    if code and not work_logs:
        errors.append("diff: files changed outside docs/logs/ but no task log was updated: "
                      + ", ".join(code[:8]) + (" ..." if len(code) > 8 else ""))
    if args.pr_title is not None:
        ids = re.findall(rf"\b({TASK_ID}|{EXTRA_ID})\b", args.pr_title)
        gate_ids = re.findall(r"\b(M[0-6]) gate\b", args.pr_title, flags=re.I)
        if not ids and not gate_ids:
            errors.append(f"PR title '{args.pr_title}' names no task ID (e.g. 'M1-2: ...', 'X-slug: ...', 'M1 gate: ...')")
        for i in ids:
            if f"{i}.md" not in changed_logs:
                errors.append(f"PR title names {i} but docs/logs/{i}.md was not updated on this branch")
        for g in gate_ids:
            if f"{g.upper()}-gate.md" not in changed_logs:
                errors.append(f"PR title names the {g.upper()} gate but docs/logs/{g.upper()}-gate.md was not updated")

    entries = [e for n in work_logs for e in logs[n].files_touched()]
    uncovered = [f for f in code if not covered(f, entries)]
    if uncovered and work_logs:
        errors.append("diff: changed files missing from 'Files touched' in the updated log(s) "
                      f"({', '.join(work_logs)}): " + ", ".join(uncovered))

    if gate_status(logs, "M0") == "closed":
        for f in code:
            if f not in FROZEN:
                continue
            for n in work_logs:
                stem = n[:-3]
                t = plan.tasks.get(stem)
                if t and f in t.files:
                    continue
                if f not in logs[n].section("Approvals"):
                    errors.append(f"{n}: frozen file {f} changed without an entry under '## Approvals' naming it")
    return files


def cmd_check(args):
    root, plan, logs_dir, logs = load(args)
    errors = []
    if logs_dir.is_dir():
        for p in sorted(logs_dir.iterdir()):
            if p.is_dir():
                errors.append(f"{p.name}/: subdirectories are not allowed in docs/logs/")
            elif p.name != "README.md" and not (TASK_FILE.match(p.name) or GATE_FILE.match(p.name)
                                                 or EXTRA_FILE.match(p.name)):
                errors.append(f"{p.name}: not a valid log name (M1-2.md, AT-1.md, M1-gate.md or X-slug.md)")
    elif any(t for t in plan.order):
        errors.append("docs/logs/ does not exist; M0-1 creates it with README.md")
    for name, log in logs.items():
        if TASK_FILE.match(name) or EXTRA_FILE.match(name):
            check_task_log(plan, logs, log, errors)
        elif GATE_FILE.match(name):
            check_gate_log(plan, logs, log, errors)
    if args.base:
        check_diff(root, plan, logs, args, errors)
    if errors:
        print(f"Implementation log check FAILED ({len(errors)} problem(s)):")
        for e in errors:
            print("  - " + e)
        sys.exit(1)
    print(f"Implementation log check passed: {len(logs)} log(s)" + (f", diff against {args.base} logged" if args.base else ""))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", help="repository root (default: git top level)")
    ap.add_argument("--plan", help="plan path relative to root (default: docs/PLAN.md)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status").set_defaults(fn=cmd_status)
    p = sub.add_parser("card"); p.add_argument("id"); p.set_defaults(fn=cmd_card)
    p = sub.add_parser("new"); p.add_argument("id"); p.add_argument("--title"); p.set_defaults(fn=cmd_new)
    p = sub.add_parser("new-gate"); p.add_argument("milestone"); p.set_defaults(fn=cmd_new_gate)
    p = sub.add_parser("check")
    p.add_argument("--base", help="git ref to diff against, e.g. origin/main")
    p.add_argument("--pr-title", help="pull-request title; must name the task ID")
    p.set_defaults(fn=cmd_check)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
