# The website and the C playground

`tools/site/` is the website: a home page and a playground where a visitor writes a C program
and runs it on the pool. `tools/playground/server.py` serves it, with nothing but the Python
standard library. The dashboard on port 47002 does not change.

## Running it

The installer puts both under `/opt/paramesh`, and runs the server on every node as a systemd user
service, `paramesh-site`, open to the LAN. Allow TCP 47080 between the nodes. A node needs `g++` to
compile for the playground (`sudo apt install g++`), and Linux 6.7 or later for the sandbox.

Open `http://127.0.0.1:47080/dashboard` on the first node and join. It shows an invite code for the
other nodes and the playground's access code; give that to the people you are supervising. The
site is `http://ADDRESS:47080/` from any machine of the LAN. To keep it to one machine, stop the
service and run `python3 /opt/paramesh/tools/playground/server.py`, which answers on 127.0.0.1 only.

## Plain C, split for you

With Nodes at 2 or more, a program that does not call `pm_init()` goes through
`tools/playground/translate.py` first. Every `for` loop in `main()` of the form
`for (i = first; i < last; i++)` is checked, and one shown safe runs on the pool. Everything else
stays on the laptop that started the program. If an outer loop cannot be split, the loops inside it
are tried. Output shows where each loop ran and why. Totals are found by themselves: a variable the
loop only adds to, or only multiplies.

Convert shows the converted program without running it, and Run shows it first when Nodes is 2 or more.

A loop stays if it prints or allocates, uses `rand()` or `break`, depends on an earlier step, writes
one place from two steps, uses a struct, or calls a function that may have side effects. Loops in
other functions stay. If the translated program does not compile, the original runs on one laptop.

- A split program's shared memory is `--region-mb` of the server, 512 by default. A program that
  needs more stops with a message.
- A `double` total adds up in any order, so its last digits can differ between runs.
- The checks read C with patterns. They see no aliasing they cannot find in `main()`, such as a
  pointer changed through another pointer.
- A loop that updates an array (`c[i][j] += x`) counts twice if a laptop leaves in the middle.

## What a run does

1. The server checks the request: C only, 64 KiB at most, 1 to 8 nodes, one run at a time.
2. It compiles a static binary in `/var/tmp/paramesh-play/<id>/`, under `pm sandbox`. Static means
   the file runs on every node whatever its C library. Build the installer on the oldest system.
3. It posts the binary to the same path on every live peer, checked by SHA-256 and the node token.
4. It runs `pmrun -n N` with the real runtime. `pmd` checks the hash, as for any job.
5. It stops the job after 30 s (`--timeout`). Stop does the same: `pmrun` lets go of `pmd`, which
   ends the job on every node.
6. It collects each worker's log, then deletes the job's directory everywhere.

## The sandbox

`src/platform/sandbox.cpp`, applied by `pmrun` and `pmd` to any program under
`/var/tmp/paramesh-play/`, before `exec`. Neither containers nor a wrapper can do this: `pmd` keeps
the process ID it started, and each process hashes its own executable. With Landlock, seccomp and
rlimits the program stays the process `pmd` started, and userfaultfd works. It fails closed.

| Limit | How |
| --- | --- |
| Files: its own directory only; nothing else read or written | Landlock |
| No new processes, no exec of anything outside its directory | seccomp, Landlock |
| TCP: ports 32768 to 60999 only, and never 47000 to 47099 | Landlock (ABI 4) |
| No UDP, no raw sockets | seccomp |
| No signals or memory access to other processes | seccomp |
| No ptrace, namespaces, BPF, io_uring, key ring, modules | seccomp |
| 6 GiB address space, 120 s of CPU, 64 MiB a file, 512 open files, no core dump | rlimits |
| Cannot ask `pmd` for a job, or to leave | `pmd` refuses a peer whose executable is under the path |

The compiler runs under Landlock too, seeing the toolchain and the job's directory, with no TCP.

## What the sandbox does not stop

Read this before letting anyone run code.

- **It is for a supervised LAN. Never put it on the internet.** There is no user isolation
  between runs beyond their directories, and no rate limit beyond one run at a time.
- A job can open TCP connections to high ports on any machine it can reach, because job processes
  need those ports. Anything listening there, on the LAN or the internet, can be reached.
- A job holds CPU and memory for up to 30 s, and the address-space limit is not a RAM cap.
- A job shares `pmd.sock`, so a hostile program can send its own daemon false chunk counts, which
  touches the ledger. Its own job is the only one it can end.
- `pmd` trusts its peers: anyone on the LAN who can reach TCP 47001 can already ask it to start a
  program. That is true of ParaMesh with or without the playground. Run it on a network you trust.
- The access code and token cross the LAN in plain HTTP.
- On kernels below 6.7, or with Landlock off, jobs do not run: the sandbox fails closed.

## Statistics

Every number comes from a measurement. What nothing measures reads "Not available": tasks
scheduled and completed, invalidations, CPU utilisation, shared memory allocated, and workers'
peak memory.

| Figure | Source |
| --- | --- |
| Compile, staging and wall-clock time | Timed by the server around each step |
| CPU, page faults, pages in and out, prefetches, encodings, fault latency | Each process's `job_pages` log line |
| Launcher CPU and peak memory | `wait4` on `pmrun` |
| Nodes that ran tasks | The launcher's `task_on_node` lines |
| Chunks by peer | This node's ledger, `GET /v1/ledger` |
| Worker threads | `slots` in `GET /v1/jobs`, sampled on this node |

A job too short for a node to take a chunk shows only the nodes that really did.

Each run adds one line to `~/.local/state/paramesh/playground.jsonl`: its ID, time, a hash of the
source (never the source), node count, timings, exit status and the figures above. That file is
what to analyse for time against node count, overhead and stability. `GET /api/runs` returns the
last 50 lines.

## The dashboard

`/dashboard` shows what the page on port 47002 shows, in the site's design: this node, its account
with the pool, given and received by node, the nodes heard, a leave in progress, the jobs here and
page ownership. All of it comes from this node's `pmd`, once a second. That page stays as it was.

Anyone who can open the site can read this, addresses included. Join and Leave work only on the
machine itself: from a browser on `127.0.0.1` or `localhost`, with the `X-ParaMesh` header. A visitor
on the LAN sees no buttons, and the server refuses the request if it is sent anyway.

## Adding a laptop with an invite code

A laptop can join only to give its cores and RAM. On a node in the pool, the dashboard shows an
invite code. On the new laptop, open its own dashboard, enter the code, choose the cores and RAM,
and press the button. Its server asks the inviter for the node token, keeps it, and joins with the
inviter as its lead node. It can then receive and run other nodes' programs, and need run nothing.

The code holds the inviter's address and port and four bytes derived from the token. It is a
convenience, not a lock: `pmd` lets any node on the LAN join with or without it.

The installed service does the rest: it is how the other nodes send this laptop their programs.

## What this machine is doing

The dashboard also shows the machine's CPU and RAM, and the CPU and RAM of ParaMesh's job processes
here, once a second for the last 60 seconds. They come from `/proc`: `/proc/stat`, `/proc/meminfo`,
and each process with `PARAMESH_JOB_ID` in its environment. So jobs started with `pmrun` count too.

## API

| Request | Meaning |
| --- | --- |
| `GET /api/cluster` | Live figures from this node's `pmd`, or `{"online": false}`. |
| `POST /api/run` | `{"language": "c", "source": ..., "nodes": N}`, header `X-Access-Code`. 202 with an `id`; 403 for a wrong code; 409 if a run is going. |
| `GET /api/run/<id>` | The run so far: `state` is `compiling`, `staging`, `running` or `done`. |
| `POST /api/stop/<id>` | Stops it. |
| `GET /api/runs` | Past runs. |
| `GET /api/node` | All the dashboard shows, and `can_manage`. `{"online": false}` without `pmd`. |
| `POST /api/enter` | `{"code", "cap_cores", "cap_ram"}`: join with an invite code. From the machine itself. |
| `POST /api/join`, `POST /api/leave` | Passed to `pmd`, as in docs/STATUS_API.md. 403 unless from the machine itself. |
| `POST /node/stage`, `GET /node/log`, `POST /node/clean` | Between nodes, with `X-Token`. |
