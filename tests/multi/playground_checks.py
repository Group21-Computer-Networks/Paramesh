"""What tests/multi/run_playground.sh checks, through the same requests the page sends."""
import json
import os
import shutil
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

BASE = "http://127.0.0.1:47081"
CODE = {"X-Access-Code": "letmein", "Content-Type": "application/json"}
failures = 0


def check(name, good, detail=""):
    global failures
    print(("ok   " if good else "FAIL ") + name, detail if not good else "")
    failures += 0 if good else 1


def post(path, body, headers=CODE):
    request = urllib.request.Request(BASE + path, json.dumps(body).encode(), headers)
    try:
        with urllib.request.urlopen(request) as answer:
            return answer.status, json.load(answer)
    except urllib.error.HTTPError as answer:
        return answer.code, json.load(answer)


def get(path):
    with urllib.request.urlopen(BASE + path) as answer:
        return json.load(answer)


def run(source, nodes=1):
    status, body = post("/api/run", {"language": "c", "source": source, "nodes": nodes})
    assert status == 202, body
    while True:
        state = get("/api/run/" + body["id"])
        if state["state"] == "done":
            return state
        time.sleep(0.05)


examples = subprocess.run(["node", "-e", 'global.window={};eval(require("fs").readFileSync("tools/site/examples.js","utf8"));'
                           "console.log(JSON.stringify(window.PM_EXAMPLES))"], capture_output=True, text=True, check=True)
for example in json.loads(examples.stdout):
    for nodes in (1, 2):
        state = run(example["source"], nodes)
        check(f"example: {example['name']}, {nodes} node(s)", state.get("exit_code") == 0 and "WRONG" not in state.get("stdout", ""), state)

long_job = """#include <paramesh.h>
#include <stdint.h>
#include <stdio.h>
struct A { uint64_t* v; };
PM_TASK(w) { const struct A* a = arg; for (uint64_t i = lo; i < hi; i++) a->v[i] = i * 3; }
int main(int c, char** v) {
    pm_config f = {.region_bytes = 32 << 20};
    if (pm_init(&c, &v, &f) != PM_OK) return 1;
    struct A a = {pm_malloc(3000000 * 8)};
    pm_parallel_for_data("w", 0, 3000000, 0, &a, sizeof a, a.v, 8);
    pm_wait_all();
    printf("%llu\\n", (unsigned long long)a.v[2999999]);
    return pm_finalize();
}
"""
state = run(long_job, 2)
metric = state.get("metrics", {})
check("two nodes both ran tasks and reported", sorted(metric.get("nodes_ran_tasks", [])) == [1, 2] and metric.get("processes_reported") == 2, metric)
check("the output is right", state.get("stdout") == "8999997\n", state.get("stdout"))
check("page counts come from the processes", metric.get("page_faults", 0) > 0 and metric.get("cpu_ms", 0) > 0, metric)
check("no figure is invented", all(k not in metric for k in ("invalidations", "tasks_scheduled")))
check("chunks by peer come from the ledger", int(metric.get("chunks_by_peer", {}).get("2", 0)) > 0, metric.get("chunks_by_peer"))

state = run("int main(void) { return x }")
check("a compile error is reported, with its line", state["compile_ok"] is False and state["diagnostics"][0]["line"] == 1 and "run_ms" not in state)
state = run('#include "/etc/passwd"\nint main(void) {}')
check("the compiler cannot read host files", state["compile_ok"] is False and "Permission denied" in state["compiler_output"], state)
state = run('#include <stdio.h>\nint main(void) { FILE* f = fopen("/etc/passwd", "r"); puts(f ? "READ" : "blocked"); return 0; }')
check("a job cannot read host files", state.get("stdout") == "blocked\n", state)
state = run('#include <unistd.h>\n#include <stdio.h>\nint main(void) { puts(fork() < 0 ? "blocked" : "FORKED"); return 0; }')
check("a job cannot start a process", state.get("stdout") == "blocked\n", state)
state = run('#include <unistd.h>\n#include <stdio.h>\nint main(void) { execl("/bin/sh", "sh", "-c", "id", (char*)0); puts("blocked"); return 0; }')
check("a job cannot run a shell", state.get("stdout") == "blocked\n", state)
state = run("""#include <stdio.h>
#include <arpa/inet.h>
#include <sys/socket.h>
int main(void) {
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(47001), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    puts(connect(fd, (struct sockaddr*)&to, sizeof to) ? "blocked" : "CONNECTED");
    return 0;
}
""")
check("a job cannot reach pmd's control port", state.get("stdout") == "blocked\n", state)
state = run("int main(void) { int* p = 0; return *p; }")
check("a crash is reported as a signal", state.get("exit_code") == 139, state.get("exit_code"))
state = run("int main(void) { for (;;); }")
check("the time limit ends a job", state.get("timed_out") is True and state.get("exit_code") != 0, state)
state = run("#include <paramesh.h>\nint main(int c, char** v) { pm_config f = {.region_bytes = 1ULL << 32}; return pm_init(&c, &v, &f) == PM_OK ? pm_finalize() : 7; }")
check("a runtime refusal shows in the exit status", state.get("exit_code") == 7, state.get("exit_code"))

# A job that tries to start a job, which would run outside the sandbox.
directory = Path("/var/tmp/paramesh-play/attack")
directory.mkdir(parents=True, exist_ok=True)
shutil.copy(os.environ["PMRUN"], directory / "pmrun")
done = subprocess.run([str(directory / "pmrun"), "--state-dir", os.environ["STATE"], "/bin/true"], capture_output=True, text=True, cwd="/tmp")
shutil.rmtree(directory)
check("pmd refuses a job asked for from the sandbox", "may not do that" in done.stderr, done.stderr)

status, _ = post("/api/run", {"source": "int main(void){}", "nodes": 1}, {"Content-Type": "application/json"})
check("Run needs the access code", status == 403, status)
for body, why in [({"language": "python", "source": "print(1)"}, "another language"), ({"source": "", "nodes": 1}, "no source"),
                  ({"source": "int main(void){}", "nodes": 99}, "too many nodes"), ({"source": "x" * 70000}, "too long"),
                  ({"source": "a\0b"}, "a NUL")]:
    check(f"refused: {why}", post("/api/run", body)[0] == 400)
status, first = post("/api/run", {"source": "int main(void) { for (;;); }", "nodes": 1})
second, _ = post("/api/run", {"source": "int main(void){}", "nodes": 1})
while get("/api/run/" + first["id"])["state"] != "running":
    time.sleep(0.05)
check("a second run waits its turn", status == 202 and second == 409, second)
post("/api/stop/" + first["id"], {})
begin = time.monotonic()
while get("/api/run/" + first["id"])["state"] != "done" and time.monotonic() - begin < 20:
    time.sleep(0.1)
check("Stop ends the job", get("/api/run/" + first["id"]).get("exit_code") not in (None, 0))
check("no job directory is left", not any(Path("/var/tmp/paramesh-play").iterdir()))
history = (Path(os.environ["STATE"]) / "playground.jsonl").read_text()
check("the history holds no source", "for (;;)" not in history and history.count("\n") > 10)
cluster = get("/api/cluster")
check("the cluster figures are real", cluster["online"] and cluster["nodes"] == 2, cluster)

# The dashboard.
node = get("/api/node")
check("the dashboard has every part", node["online"] and all(k in node for k in ("peers", "jobs", "ledger", "pages")) and node["can_manage"], node)
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
    probe.connect(("10.255.255.255", 1))  # sends nothing: it finds this machine's LAN address
    address = probe.getsockname()[0]
leave = {"X-ParaMesh": "1", "Content-Type": "application/json"}
try:
    urllib.request.urlopen(urllib.request.Request(f"http://{address}:47081/api/leave", b"{}", leave))
    from_lan = 200
except urllib.error.HTTPError as answer:
    from_lan = answer.code
check("Leave is refused from the LAN", from_lan == 403 and get("/api/node")["peers"]["member"] == "joined", from_lan)
with urllib.request.urlopen(f"http://{address}:47081/api/node") as answer:
    check("a LAN visitor can read but not manage", json.load(answer)["can_manage"] is False)
check("Leave needs the header", post("/api/leave", {}, {"Content-Type": "application/json"})[0] == 403)
check("Leave is passed to pmd", post("/api/leave", {}, leave)[0] == 202)
begin = time.monotonic()
while get("/api/node")["peers"]["member"] != "standby" and time.monotonic() - begin < 15:
    time.sleep(0.2)
check("the node stands by after leaving", get("/api/node")["peers"]["member"] == "standby")
check("Join is passed to pmd", post("/api/join", {"cap_cores": 2, "cap_ram": 256 << 20}, leave)[0] == 202)
begin = time.monotonic()
while get("/api/node")["peers"]["member"] != "joined" and time.monotonic() - begin < 15:
    time.sleep(0.2)
check("the node is back in the pool", get("/api/node")["peers"]["member"] == "joined")
sys.exit(1 if failures else 0)
