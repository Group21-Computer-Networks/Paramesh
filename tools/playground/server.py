#!/usr/bin/env python3
"""The ParaMesh website and C playground (X-playground). Standard library only.

Run one on every node of the pool:

    python3 tools/playground/server.py --public

Without --public it serves 127.0.0.1 only. With it, the site is open to the LAN, Run needs the
access code printed at start, and the nodes reach each other with the token in
~/.local/state/paramesh/playground.token, which must be the same file on every node.

A run: compile as a static binary under `pm sandbox`, stage it to the same path on every peer,
run it with the real pmrun, collect what each process logged, and clean up. pmd and pmrun confine
anything under JOBS (src/platform/sandbox.h). Every number shown comes from a process's own
report or a measurement here; what nothing measures is null, shown as "Not available".
See docs/PLAYGROUND.md for what the sandbox does not stop.
"""
import argparse
import base64
import hashlib
import hmac
import ipaddress
import json
import os
import re
import secrets
import shutil
import signal
import socket
import struct
import subprocess
import threading
import time
import urllib.error
import urllib.request
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from translate import translate

JOBS = Path("/var/tmp/paramesh-play")  # kSandboxRoot in src/platform/sandbox.h
PORT = 47080  # within what a job cannot reach (kFirstDaemonPort)
MAX_SOURCE = 64 * 1024
MAX_OUTPUT = 256 * 1024
MAX_BINARY = 64 * 1024 * 1024
COMPILE_SECONDS = 20
RUN_SECONDS = 30
KEPT_RUNS = 32
SITE = Path(__file__).resolve().parent.parent / "site"
PAGES = {
    "/": ("index.html", "text/html; charset=utf-8"),
    "/playground": ("playground.html", "text/html; charset=utf-8"),
    "/dashboard": ("dashboard.html", "text/html; charset=utf-8"),
    "/dashboard.js": ("dashboard.js", "text/javascript; charset=utf-8"),
    "/site.css": ("site.css", "text/css; charset=utf-8"),
    "/home.js": ("home.js", "text/javascript; charset=utf-8"),
    "/playground.js": ("playground.js", "text/javascript; charset=utf-8"),
    "/examples.js": ("examples.js", "text/javascript; charset=utf-8"),
}


def http_json(url, data=None, headers=None, timeout=5):
    request = urllib.request.Request(url, data=data, headers=headers or {})
    with urllib.request.urlopen(request, timeout=timeout) as answer:
        return json.load(answer)


class Service:
    def __init__(self, args):
        self.args = args
        self.state = Path(args.state_dir).expanduser()
        self.bin = Path(args.bin)
        self.prefix = Path(args.prefix)
        self.runs = {}
        self.order = []
        self.lock = threading.Lock()
        self.busy = False
        self.code = self.load_code() if args.public else None
        self.token = self.load_token()
        self.history = self.state / "playground.jsonl"
        self.usage_lock = threading.Lock()
        self.usage_at = 0
        self.usage = {}
        self.ticks = {}
        self.machine_ticks = None

    def load_code(self):
        """The access code: the same across restarts, and the dashboard shows it."""
        if self.args.code:
            return self.args.code
        path = self.state / "playground.code"
        if not path.exists():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch(0o600)
            path.write_text(secrets.token_hex(3) + "\n")
        return path.read_text().strip()

    def load_token(self):
        path = self.state / "playground.token"
        if self.args.token:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(self.args.token + "\n")
            path.chmod(0o600)
        if not path.exists():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch(0o600)
            path.write_text(secrets.token_hex(16) + "\n")
        return path.read_text().strip()

    # ---- invite codes: a way in for a node that only gives its cores ---------------------------

    def invite_secret(self):
        return hashlib.sha256(self.token.encode()).digest()[:4]

    def address(self):
        """This machine's address on the network it would reach others by. Sends nothing."""
        if self.args.advertise:
            return self.args.advertise
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            try:
                probe.connect(("10.255.255.255", 1))
                return probe.getsockname()[0]
            except OSError:
                return "127.0.0.1"

    def invite_code(self):
        """Where to find this node, and the proof of having been told: 16 letters, as XXXX-XXXX-..."""
        raw = socket.inet_aton(self.address()) + struct.pack(">H", self.args.port) + self.invite_secret()
        text = base64.b32encode(raw).decode()
        return "-".join(text[i:i + 4] for i in range(0, 16, 4))

    def lead(self):
        return self.address() + (f":{self.args.pmd_ports}" if self.args.pmd_ports else "")

    def enter(self, code, cap_cores, cap_ram):
        """On the node that was given the code: find the inviter, take the nodes' token, join."""
        try:
            raw = base64.b32decode(code.replace("-", "").replace(" ", "").upper())
            if len(raw) != 10:
                raise ValueError
        except (ValueError, AttributeError):
            return 400, {"error": "that is not an invite code"}
        address = socket.inet_ntoa(raw[:4])
        port = struct.unpack(">H", raw[4:6])[0]
        url = f"http://{address}:{port}/node/invite?secret={raw[6:].hex()}"
        try:
            with urllib.request.urlopen(url, timeout=5) as answer:
                invite = json.load(answer)
        except urllib.error.HTTPError:
            return 403, {"error": "that code is not right for this pool"}
        except (OSError, ValueError):
            return 502, {"error": f"cannot reach the node at {address}: check the network and its firewall"}
        self.token = invite["token"]
        path = self.state / "playground.token"
        path.write_text(self.token + "\n")
        return self.pool_request("/v1/join", json.dumps({"cap_cores": cap_cores, "cap_ram": cap_ram,
                                                         "lead": invite["lead"]}).encode())

    # ---- what this machine is doing --------------------------------------------------------

    def local_usage(self):
        """CPU and RAM from /proc, for the machine and for each job's process here. At most once
        a second, however many pages are open: two readings make a rate."""
        with self.usage_lock:
            now = time.monotonic()
            if now - self.usage_at >= 0.9:
                self.usage = self.read_usage(now - self.usage_at)
                self.usage_at = now
            return self.usage

    def read_usage(self, elapsed):
        page = os.sysconf("SC_PAGE_SIZE")
        tick = os.sysconf("SC_CLK_TCK")
        memory = {}
        with open("/proc/meminfo") as lines:
            for line in lines:
                name, value = line.split(":", 1)
                memory[name] = int(value.split()[0]) * 1024
        with open("/proc/stat") as lines:
            fields = [int(x) for x in lines.readline().split()[1:]]
        idle, total = fields[3] + fields[4], sum(fields[:8])
        busy = None
        if self.machine_ticks and total > self.machine_ticks[1]:
            busy = 100 * (1 - (idle - self.machine_ticks[0]) / (total - self.machine_ticks[1]))
        self.machine_ticks = (idle, total)
        jobs = {}
        ticks = {}
        for entry in os.scandir("/proc"):
            if not entry.name.isdigit():
                continue
            try:
                with open(f"/proc/{entry.name}/environ", "rb") as source:
                    found = re.search(rb"(?:^|\0)PARAMESH_JOB_ID=(\d+)", source.read())
                if found is None:
                    continue
                with open(f"/proc/{entry.name}/stat") as source:
                    stat = source.read().rsplit(")", 1)[1].split()
            except OSError:
                continue  # gone, or not this user's
            used = int(stat[11]) + int(stat[12])
            job = jobs.setdefault(found[1].decode(), {"rss_bytes": 0, "cpu_percent": None})
            job["rss_bytes"] += int(stat[21]) * page
            ticks[entry.name] = used
            if entry.name in self.ticks and elapsed > 0:
                job["cpu_percent"] = (job["cpu_percent"] or 0) + 100 * (used - self.ticks[entry.name]) / tick / elapsed
        self.ticks = ticks
        return {"cores": os.cpu_count(), "cpu_percent": busy, "ram_total": memory["MemTotal"],
                "ram_used": memory["MemTotal"] - memory["MemAvailable"],
                "paramesh_rss": sum(j["rss_bytes"] for j in jobs.values()),
                "paramesh_cpu_percent": sum(j["cpu_percent"] or 0 for j in jobs.values()), "jobs": jobs}

    def pmd(self, path):
        return http_json(f"http://127.0.0.1:{self.args.pmd_http}{path}", timeout=2)

    def alive_peers(self):
        peers = self.pmd("/v1/peers")
        return [p for p in peers.get("peers", []) if p.get("state") == "alive" and not p.get("leaving")]

    def cluster(self):
        """Real figures from this node's pmd, or offline. Nothing is made up when it is down."""
        try:
            peers = self.pmd("/v1/peers")
            jobs = self.pmd("/v1/jobs")
        except (OSError, ValueError):
            return {"online": False}
        alive = [p for p in peers.get("peers", []) if p.get("state") == "alive"]
        joined = peers.get("member") == "joined"
        return {
            "online": True,
            "node": peers.get("node"),
            "member": peers.get("member"),
            "nodes": (1 if joined else 0) + len(alive),
            "cores": (peers.get("cap_cores", 0) if joined else 0) + sum(p.get("cap_cores", 0) for p in alive),
            "free_cores": sum(p.get("free_cores", 0) for p in alive),
            "jobs": len(jobs.get("jobs", [])),
            "peers": [{"node": p["node"], "cores": p.get("cap_cores"), "state": p["state"],
                       "jobs": p.get("jobs")} for p in peers.get("peers", [])],
            "access_code": self.code is not None,
        }

    def node(self):
        """All the dashboard shows, in one answer, as this node's pmd reports it."""
        try:
            peers, jobs, ledger = self.pmd("/v1/peers"), self.pmd("/v1/jobs"), self.pmd("/v1/ledger")
        except (OSError, ValueError):
            return {"online": False}
        pages = []
        for job in jobs.get("jobs", []):
            try:
                pages.append(self.pmd(f"/v1/jobs/{int(job['job'])}/pages"))
            except (OSError, ValueError, KeyError):
                pass  # the job ended between the two questions
        return {"online": True, "peers": peers, "jobs": jobs, "ledger": ledger, "pages": pages,
                "local": self.local_usage()}

    def pool_request(self, path, body):
        """Join or leave, passed to pmd with the marks it asks of its own dashboard."""
        request = urllib.request.Request(f"http://127.0.0.1:{self.args.pmd_http}{path}", body or b"{}",
                                         {"X-ParaMesh": "1", "Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(request, timeout=10) as answer:
                return answer.status, json.load(answer)
        except urllib.error.HTTPError as answer:
            return answer.code, json.load(answer)
        except (OSError, ValueError):
            return 502, {"error": "no answer from pmd: is it running?"}

    # ---- a run ------------------------------------------------------------------------------

    def start(self, source, nodes):
        with self.lock:
            if self.busy:
                return None
            self.busy = True
            run = {"id": uuid.uuid4().hex, "state": "compiling", "stop": False, "started": time.time()}
            self.runs[run["id"]] = run
            self.order.append(run["id"])
            while len(self.order) > KEPT_RUNS:
                self.runs.pop(self.order.pop(0), None)
        threading.Thread(target=self.execute, args=(run, source, nodes), daemon=True).start()
        return run

    def execute(self, run, source, nodes):
        directory = JOBS / run["id"]
        peers = []
        try:
            JOBS.mkdir(mode=0o755, exist_ok=True)
            directory.mkdir(mode=0o700)
            translated, notes = translate(source, self.args.region_mb << 20) if nodes > 1 else (source, [])
            (directory / "main.c").write_text(translated)
            run["program"] = hashlib.sha256(source.encode()).hexdigest()[:16]
            run["nodes_asked"] = nodes
            compiled = self.compile(run, directory)
            if not compiled and translated != source:  # the user's C comes first: it may be our fault
                (directory / "main.c").write_text(source)
                notes = ["the split program did not compile, so this ran on one laptop"]
                compiled = self.compile(run, directory)
            run["translator"] = notes
            if compiled and not run["stop"]:
                peers = self.stage(run, directory, nodes)
                if peers is not None and not run["stop"]:
                    self.launch(run, directory, nodes)
                    self.collect(run, directory, peers)
            if run["stop"] and "run_ms" not in run:
                run["stopped"] = True
        except Exception as failure:  # the page must always get an answer
            run["internal_error"] = str(failure)
        finally:
            for peer in peers or []:
                self.peer_post(peer, f"/node/clean?job={run['id']}", b"")
            shutil.rmtree(directory, ignore_errors=True)
            run["state"] = "done"
            run["finished"] = time.time()
            self.record(run)
            with self.lock:
                self.busy = False

    def compile(self, run, directory):
        begin = time.monotonic()
        shutil.copy(self.prefix / "include/paramesh.h", directory)  # the sandbox sees nothing else
        shutil.copy(self.prefix / "lib/libparamesh.a", directory)
        command = [str(self.bin / "pm"), "sandbox", str(directory), self.args.cc, "-x", "c", "-std=gnu17",
                   "-O2", "-static", "-fdiagnostics-color=never", "main.c", "-o", "app", "-I.", "-L.",
                   "-lparamesh", "-lstdc++", "-lm", "-lpthread"]
        environment = {"PATH": "/usr/bin:/bin", "TMPDIR": str(directory), "LC_ALL": "C", "HOME": str(directory)}
        try:
            done = subprocess.run(command, cwd=directory, env=environment, capture_output=True,
                                  text=True, errors="replace", timeout=COMPILE_SECONDS)
            text, ok = done.stderr[:MAX_OUTPUT], done.returncode == 0
        except subprocess.TimeoutExpired:
            text, ok = f"the compiler was stopped after {COMPILE_SECONDS} s\n", False
        run["compile_ms"] = round((time.monotonic() - begin) * 1000, 1)
        # A link warning about getaddrinfo in a static binary comes from the runtime's HTTP code.
        run["compiler_output"] = "\n".join(l for l in text.splitlines() if "statically linked" not in l
                                           and "in function `httplib" not in l)
        run["compile_ok"] = ok
        run["diagnostics"] = [{"line": int(m[1]), "column": int(m[2]), "severity": m[3], "message": m[4]}
                              for m in re.finditer(r"^main\.c:(\d+):(\d+): (error|warning): (.*)$", text, re.M)]
        return ok

    def peer_url(self, peer, path):
        return f"http://{peer['address']}:{self.args.port}{path}"

    def peer_post(self, peer, path, body):
        try:
            return http_json(self.peer_url(peer, path), body, {"X-Token": self.token}, timeout=30)
        except (OSError, ValueError):
            return None

    def stage(self, run, directory, nodes):
        """The binary, at the same path on every node a job could land on: pmd picks the peers."""
        run["state"] = "staging"
        begin = time.monotonic()
        peers = [] if nodes == 1 else self.alive_peers()
        if nodes > 1 + len(peers):
            run["refused"] = f"{nodes} nodes asked for, {1 + len(peers)} in the pool"
            return None
        data = (directory / "app").read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        staged = []
        for peer in peers:
            ipaddress.ip_address(peer["address"])  # from pmd, but it goes in a URL
            if ipaddress.ip_address(peer["address"]).is_loopback:  # same machine: already there
                staged.append(peer)
                continue
            answer = self.peer_post(peer, f"/node/stage?job={run['id']}&sha256={digest}", data)
            if answer is not None and answer.get("ok"):
                staged.append(peer)
        run["stage_ms"] = round((time.monotonic() - begin) * 1000, 1)
        run["binary_bytes"] = len(data)
        if nodes > 1 + len(staged):
            run["refused"] = (f"{nodes} nodes asked for, but only {1 + len(staged)} could take the "
                              "program: is the playground server running on every node?")
        return staged

    def launch(self, run, directory, nodes):
        if "refused" in run:
            return
        run["state"] = "running"
        environment = {"PATH": "/usr/bin:/bin", "HOME": str(directory), "LC_ALL": "C",
                       "PARAMESH_CFG_LOG_LEVEL": "info", "PARAMESH_CFG_SPILL_DIR": str(directory)}
        command = [str(self.bin / "pmrun"), "-n", str(nodes), "--state-dir", str(self.state),
                   str(directory / "app")]
        begin = time.monotonic()
        process = subprocess.Popen(command, cwd=directory, env=environment, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
        outputs = {"stdout": bytearray(), "stderr": bytearray()}

        def drain(pipe, into):
            for chunk in iter(lambda: pipe.read(65536), b""):
                if len(into) < MAX_OUTPUT:
                    into += chunk
                else:
                    run["output_cut"] = True

        stopping = 0
        readers = [threading.Thread(target=drain, args=(process.stdout, outputs["stdout"]), daemon=True),
                   threading.Thread(target=drain, args=(process.stderr, outputs["stderr"]), daemon=True)]
        for reader in readers:
            reader.start()
        run["slots"] = {}
        usage = None
        while usage is None:
            elapsed = time.monotonic() - begin
            if elapsed > self.args.timeout:
                run["timed_out"] = True
            if (run["stop"] or run.get("timed_out")) and not stopping:
                stopping = time.monotonic()
                os.kill(process.pid, signal.SIGTERM)  # pmrun lets go of pmd, which ends the job
            if stopping and time.monotonic() - stopping > 8:
                os.killpg(process.pid, signal.SIGKILL)
            pid, status, usage = os.wait4(process.pid, os.WNOHANG)
            if pid == 0:
                usage = None
                run["live_stdout"] = bytes(outputs["stdout"]).decode(errors="replace")
                self.sample(run)
                time.sleep(0.02)
        run["run_ms"] = round((time.monotonic() - begin) * 1000, 1)
        process.returncode = os.waitstatus_to_exitcode(status)
        for reader in readers:
            reader.join(timeout=2)
        run["exit_code"] = process.returncode
        run["launcher_peak_kib"] = usage.ru_maxrss
        run["launcher_cpu_ms"] = round((usage.ru_utime + usage.ru_stime) * 1000, 1)
        run["stdout"] = bytes(outputs["stdout"]).decode(errors="replace")
        stderr, run["events"] = split_events(bytes(outputs["stderr"]).decode(errors="replace"))
        run["stderr"] = stderr
        run.pop("live_stdout", None)

    def sample(self, run):
        if time.monotonic() - run.get("sampled", 0) < 0.3:
            return
        run["sampled"] = time.monotonic()
        try:
            for job in self.pmd("/v1/jobs").get("jobs", []):
                run["slots"][str(job.get("job"))] = job.get("slots")
        except (OSError, ValueError):
            pass

    def collect(self, run, directory, peers):
        reports = []
        for peer in peers:
            if ipaddress.ip_address(peer["address"]).is_loopback:
                log = directory / "worker.log"
                text = log.read_text(errors="replace") if log.exists() else ""
            else:
                try:
                    url = self.peer_url(peer, f"/node/log?job={run['id']}")
                    with urllib.request.urlopen(urllib.request.Request(url, headers={"X-Token": self.token}),
                                                timeout=5) as answer:
                        text = answer.read().decode(errors="replace")
                except OSError:
                    continue
            if text:
                reports.append(text)
        worker_output = []
        for text in reports:
            out, events = split_events(text)
            run["events"] = run.get("events", []) + events
            if out:
                worker_output.append(out)
        run["worker_output"] = "".join(worker_output)
        run["metrics"] = metrics(run, self.ledger(run))

    def ledger(self, run):
        """Chunks each peer ran for this job, from this node's ledger."""
        jobs = {e.get("job") for e in run.get("events", []) if e.get("job")}
        counts = {}
        if run.get("nodes_asked", 1) > 1:
            time.sleep(1.2)  # the status JSON is remade once a second
        try:
            for peer in self.pmd("/v1/ledger").get("peers", []):
                for job in peer.get("jobs", []):
                    if job.get("job") in jobs:
                        counts[str(peer["peer"])] = job.get("counts", {}).get("chunks_by_peer")
        except (OSError, ValueError):
            pass
        return counts

    def record(self, run):
        """One line for each run, for later study. Never the source: only its hash."""
        metric = run.get("metrics") or {}
        line = {"id": run["id"], "time": run["started"], "program": run.get("program"),
                "nodes_asked": run.get("nodes_asked"), "compile_ok": run.get("compile_ok"),
                "compile_ms": run.get("compile_ms"), "stage_ms": run.get("stage_ms"), "run_ms": run.get("run_ms"),
                "exit_code": run.get("exit_code"), "timed_out": run.get("timed_out", False),
                "refused": run.get("refused"), "internal_error": run.get("internal_error"),
                "metrics": metric}
        try:
            with self.history.open("a") as out:
                out.write(json.dumps(line) + "\n")
        except OSError:
            pass

    def public(self, run):
        hidden = {"stop", "events", "sampled"}
        return {k: v for k, v in run.items() if k not in hidden}

    def recent(self, limit=50):
        try:
            lines = self.history.read_text().splitlines()[-limit:]
        except OSError:
            return []
        return [json.loads(line) for line in lines]


def split_events(text):
    """The runtime's JSON log lines, from what the program itself printed."""
    kept, events = [], []
    for line in text.splitlines(keepends=True):
        event = None
        if line.startswith('{"event":'):
            try:
                event = json.loads(line)
            except ValueError:
                event = None
        if isinstance(event, dict) and "fields" in event:
            events.append(event)
        else:
            kept.append(line)
    return "".join(kept), events


def metrics(run, chunks):
    """From job_pages, which each process logs as the job ends, and task_on_node."""
    events = run.get("events", [])
    processes = {}
    for event in events:
        if event.get("event") == "job_pages":
            processes[str(event.get("node"))] = event["fields"]
    jobs = {e.get("job") for e in events}
    participants = sorted({e["fields"]["on"] for e in events if e.get("event") == "task_on_node"})
    def total(name):
        return sum(p.get(name, 0) for p in processes.values()) if processes else None
    latency = [0] * 16
    for process in processes.values():
        latency = [a + b for a, b in zip(latency, process.get("latency", latency))]
    return {
        "processes_reported": len(processes),
        "nodes_ran_tasks": participants,
        "worker_threads": sum(run.get("slots", {}).get(str(job)) or 0 for job in jobs) or None,
        "cpu_ms": total("cpu_ms"),
        "page_faults": total("faults"),
        "pages_received": total("received"),
        "pages_sent": total("sent"),
        "pages_prefetched": total("prefetched"),
        "requests_sent_again": total("sent_again"),
        "page_bytes": total("page_bytes"),
        "page_bytes_plain": total("page_bytes_plain"),
        "pages_encoded": total("encoded"),
        "pages_as_diffs": total("diffs"),
        "fault_latency": latency if processes else None,
        "chunks_by_peer": chunks,
        "by_node": processes,
    }


class Handler(BaseHTTPRequestHandler):
    server_version = "ParaMesh"
    sys_version = ""
    protocol_version = "HTTP/1.0"

    def log_message(self, *args):
        pass

    def reply(self, status, body, kind="application/json"):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.end_headers()
        self.wfile.write(data)

    def path_and_query(self):
        path, _, query = self.path.partition("?")
        return path, dict(p.split("=", 1) for p in query.split("&") if "=" in p)

    def token_ok(self):
        given = self.headers.get("X-Token", "")
        return hmac.compare_digest(given.encode(), self.server.service.token.encode())

    def code_ok(self):
        code = self.server.service.code
        return code is None or hmac.compare_digest(self.headers.get("X-Access-Code", "").encode(), code.encode())

    def is_local(self):
        """This machine, by its own address, not through a name that a web page could point here."""
        host = self.headers.get("Host", "").rsplit(":", 1)[0]
        return ipaddress.ip_address(self.client_address[0]).is_loopback and host in ("127.0.0.1", "localhost")

    def body(self, limit):
        length = int(self.headers.get("Content-Length", "0"))
        if length < 0 or length > limit:
            return None
        return self.rfile.read(length)

    def do_GET(self):
        service = self.server.service
        path, query = self.path_and_query()
        if path in PAGES:
            name, kind = PAGES[path]
            try:
                self.reply(200, (SITE / name).read_bytes(), kind)
            except OSError:
                self.reply(404, {"error": "no such page"})
        elif path == "/api/cluster":
            self.reply(200, service.cluster())
        elif path == "/api/node":
            answer = {**service.node(), "can_manage": self.is_local()}
            if self.is_local():
                answer["invite"] = {"code": service.invite_code(), "address": service.address(),
                                    "access_code": service.code}
            self.reply(200, answer)
        elif path == "/api/runs":
            self.reply(200, {"runs": service.recent()})
        elif path.startswith("/api/run/"):
            run = service.runs.get(path.rsplit("/", 1)[1])
            self.reply(200, service.public(run)) if run else self.reply(404, {"error": "no such run"})
        elif path == "/node/invite":
            given = bytes.fromhex(query.get("secret", "")) if re.fullmatch(r"[0-9a-f]{8}", query.get("secret", "")) else b""
            if hmac.compare_digest(given, service.invite_secret()):
                self.reply(200, {"token": service.token, "lead": service.lead()})
            else:
                time.sleep(1)  # four bytes: this is what makes guessing slow
                self.reply(403, {"error": "not this pool's code"})
        elif path == "/node/log" and self.token_ok() and valid_id(query.get("job")):
            log = JOBS / query["job"] / "worker.log"
            self.reply(200, log.read_bytes()[-MAX_OUTPUT:] if log.exists() else b"", "text/plain")
        else:
            self.reply(404, {"error": "not found"})

    def do_POST(self):
        service = self.server.service
        path, query = self.path_and_query()
        if path == "/api/run":
            if not self.code_ok():
                self.reply(403, {"error": "the access code is not right"})
                return
            raw = self.body(MAX_SOURCE * 2)
            try:
                request = json.loads(raw) if raw else None
                source, nodes = request["source"], int(request.get("nodes", 1))
            except (TypeError, ValueError, KeyError):
                self.reply(400, {"error": "send JSON with source and nodes"})
                return
            if request.get("language", "c") != "c":
                self.reply(400, {"error": "the playground runs C only"})
            elif not isinstance(source, str) or not source.strip() or len(source.encode()) > MAX_SOURCE or "\0" in source:
                self.reply(400, {"error": f"the program must be text, at most {MAX_SOURCE} bytes"})
            elif not 1 <= nodes <= 8:
                self.reply(400, {"error": "nodes must be 1 to 8"})
            else:
                run = service.start(source, nodes)
                if run is None:
                    self.reply(409, {"error": "another program is running; try again in a moment"})
                else:
                    self.reply(202, {"id": run["id"]})
        elif path in ("/api/join", "/api/leave"):
            # Only the person at this machine, never a visitor on the LAN. The header stops
            # another web page in their browser, which cannot send it without asking first.
            if not self.is_local() or self.headers.get("X-ParaMesh") != "1":
                self.reply(403, {"error": "join and leave work only on the machine itself"})
                return
            raw = self.body(1024)
            if raw is None:
                self.reply(400, {"error": "too long"})
                return
            status, answer = service.pool_request(path.replace("/api/", "/v1/"), raw)
            self.reply(status, answer)
        elif path == "/api/enter":
            if not self.is_local() or self.headers.get("X-ParaMesh") != "1":
                self.reply(403, {"error": "this works only on the machine itself"})
                return
            try:
                request = json.loads(self.body(1024) or b"")
                status, answer = service.enter(request["code"], int(request["cap_cores"]), int(request["cap_ram"]))
            except (ValueError, KeyError, TypeError):
                status, answer = 400, {"error": "send a code, cores and RAM"}
            self.reply(status, answer)
        elif path.startswith("/api/stop/"):
            run = service.runs.get(path.rsplit("/", 1)[1])
            if not self.code_ok() or run is None:
                self.reply(403, {"error": "no"})
            else:
                run["stop"] = True
                self.reply(202, {"ok": True})
        elif path == "/node/stage" and self.token_ok() and valid_id(query.get("job")):
            data = self.body(MAX_BINARY)
            if data is None or hashlib.sha256(data).hexdigest() != query.get("sha256"):
                self.reply(400, {"error": "that is not the binary"})
                return
            JOBS.mkdir(mode=0o755, exist_ok=True)
            directory = JOBS / query["job"]
            directory.mkdir(mode=0o700, exist_ok=True)
            app = directory / "app"
            app.write_bytes(data)
            app.chmod(0o700)
            self.reply(200, {"ok": True})
        elif path == "/node/clean" and self.token_ok() and valid_id(query.get("job")):
            shutil.rmtree(JOBS / query["job"], ignore_errors=True)
            self.reply(200, {"ok": True})
        else:
            self.reply(404, {"error": "not found"})


def valid_id(job):
    return job is not None and re.fullmatch(r"[0-9a-f]{32}", job) is not None


def main():
    parser = argparse.ArgumentParser(description="The ParaMesh website and C playground")
    parser.add_argument("--public", action="store_true", help="serve the LAN; Run needs the access code")
    parser.add_argument("--port", type=int, default=PORT)
    parser.add_argument("--code", help="the access code (default: a new one, printed at start)")
    parser.add_argument("--token", help="set the token the nodes share, and keep it")
    parser.add_argument("--pmd-http", type=int, default=47002)
    parser.add_argument("--pmd-ports", help="control:discovery ports, if not the defaults")
    parser.add_argument("--advertise", help="the address to put in invite codes (default: found)")
    parser.add_argument("--state-dir", default="~/.local/state/paramesh")
    parser.add_argument("--bin", default="/opt/paramesh/bin", help="where pm and pmrun are")
    parser.add_argument("--prefix", default="/opt/paramesh", help="where paramesh.h and the library are")
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--region-mb", type=int, default=512, help="shared memory of a program split for the pool")
    parser.add_argument("--timeout", type=int, default=RUN_SECONDS, help="seconds a job may run")
    args = parser.parse_args()
    server = ThreadingHTTPServer(("0.0.0.0" if args.public else "127.0.0.1", args.port), Handler)
    server.service = Service(args)
    server.daemon_threads = True
    shutil.rmtree(JOBS, ignore_errors=True)  # what a killed server left
    print(f"ParaMesh site: http://127.0.0.1:{args.port}/", flush=True)
    if args.public:
        print(f"access code: {server.service.code}", flush=True)
        print(f"node token (the same on every node): {server.service.token}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
