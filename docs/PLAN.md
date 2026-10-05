# ParaMesh Detailed Implementation Plan

Oct 6, 2026 · @Amirishetty Sai Vignesh

ParaMesh is built in seven milestones, M0 to M6, by one to three coding agents working on a Linux laptop, with a person approving each milestone gate. Nothing after M0 starts until the frozen interface files are approved. This is document 3 of 3; it follows ParaMesh Scope and System Behaviors, the High-Level Design and the SRS, and records every place it departs from them.

## How to use this plan

The work is done by one to three coding agents on a Linux 5.19+ laptop, and a person closes each milestone.

- **Task card.** Every row in a milestone table is one task: its goal, the files it may touch, what it depends on, its stream, and the check that proves it is done. One task is one branch and one pull request into `main`.
- **Streams.** Tasks marked A, B or C in the same milestone can run at the same time in separate branches, because they touch different directories and meet only at interfaces frozen in M0. Tasks marked `seq` are done by one agent, alone, in the order listed.
- **Gate.** A milestone ends with a gate. A person reviews the merged work and runs the acceptance tests on three simulated nodes on one machine and on two laptops joined by one Ethernet cable. The next milestone does not start before the gate is closed.
- **Three-laptop runs.** Tests that the SRS words as "3 laptops" are repeated and recorded when a gigabit switch is available. Getting the switch is an item in `docs/TODO.md`.
- **No guessing.** A task that meets a point listed under *Still open* stops and asks. It does not choose.
- **Pending work.** Anything deferred lives in `docs/TODO.md` in the repository, which the team keeps updating.

## Decisions this plan rests on

Every row is an answer you gave on 5 and 6 October 2026; nothing here was decided by the plan.

| Area | Decision |
| --- | --- |
| Implementers | One to three coding agents, not a six-person team. The SRS role table is ignored. |
| Where agents run | On a Linux 5.19+ laptop, so they can run every single-machine test. |
| Parallel work | Up to three streams, only where tasks are truly independent. |
| Human steps | A review and hardware test at each milestone gate. |
| Git | GitHub, one pull request per task. |
| CI | GitHub Actions builds and runs unit tests on every push. Multi-node tests run from a local script. |
| Language | C++20 for the library, daemon and tools. |
| Public API | A C-compatible header, exactly as in the SRS, usable from C and C++ applications. |
| Build | CMake. |
| Third-party code | Vendored header-only: nlohmann/json, cpp-httplib, PicoSHA2, doctest. CRC-32C is our own. |
| Swappable parts | Abstract interfaces, with implementations injected at start-up. A new implementation is new code only. |
| Errors | Mixed: `libparamesh` uses result types and is built with exceptions off; `pmd` and the tools may use exceptions, caught at each thread's entry point. |
| Task that throws | Caught by the runtime, and the job is aborted on all nodes with the task name and message. |
| Quality gate | clang-format, clang-tidy, warnings as errors, unit tests under address, undefined-behaviour and thread sanitizers. |
| Agent guidance | A guide file at the repository root, plus the task cards in this plan. |
| Layout | Left to the plan (see Repository layout). |
| Security | Ledger fix only this semester. Everything else is listed as pending in `docs/TODO.md`. |
| Ledger credit | A node values a peer's work at the node's own measured cost for the same work. Nothing a peer reports counts. |
| Memory credit | By segments assigned: segments homed on the peer x 2 MB x time. |
| Balance | One balance; memory credit is converted to compute credit at a fixed, configurable rate. |
| Dominant share | Measured on this node only: the job's share of this node's worker slots and home-store RAM. |
| Announced totals | Shown in `pm status` and used for the disagreement alarm. Never an input to a weight. |
| Disagreement alarm | Compares raw counts (chunks completed, segment-seconds), not credits. |
| Speed score | Removed. |
| Own jobs | A node's own jobs come first on its own cores; the ledger ranks only guests. |
| Node identity | A random ID generated on first run and saved on disk. |
| State | `~/.local/state/paramesh`, ledger stored as JSON, written atomically. |
| Task queue | In the launcher's job process. |
| Local link | A Unix domain socket between each job process and its `pmd`; `pmd` pushes a thread quota. |
| Launcher thread | The thread in `pm_wait_all()` sleeps; worker threads run chunks. |
| Launcher exit | `pm leave` is refused while its job runs; `pm leave --force` and Ctrl+C abort the job. |
| Lock and barrier manager | The launcher. |
| Task registry | A constructor function per `PM_TASK`; ID is the 64-bit FNV-1a hash of the name; a collision stops the program at start-up. |
| Node choice | The launcher plus the N-1 peers with the most free capacity; each asked peer accepts or declines. |
| Disk spill | Kept, at the home. |
| Compression | Flag reserved in the protocol, never set this semester. |
| Stride prefetcher | Deferred to `docs/TODO.md`. |
| Anti-thrashing | Its own task after M3, with the false-sharing demo. |
| Mid-run join | Automatic refill: a job running on fewer nodes than it asked for is offered to a newly seen peer. |
| Workloads | Shared counter and matrix multiply as milestone tests; false-sharing demo with anti-thrashing. The other five are deferred. |
| Multi-node testing | Processes on localhost for everyday work, network namespaces with `tc netem` for delay and loss. |
| Hardware | All six laptops run native Linux 5.19+. Two laptops on one cable first; no switch yet. |
| Gates | Three simulated nodes plus two cabled laptops. |
| Configuration | Command-line flags plus a key=value file. |
| Logging | JSON lines. |
| Monitoring | `pm top` with plain terminal escape codes; dashboard as one static HTML file. |
| M6 "novelty features" | Parked in `docs/TODO.md` until defined. |
| Fine detail | Signatures, payload fields and transition tables are drafted in M0 and approved by you. |
| Tunables | All configurable, with proposed defaults in one table for your review. |
| Earlier documents | Left as they are; this plan records every change in detail. |

## Repository layout and toolchain

One CMake project builds one library, three programs and the tests; each directory below is its own CMake target, created empty in the first task so that later tasks never edit the same build file.

| Path | Holds | Depends on |
| --- | --- | --- |
| `include/paramesh.h` | The public C API. Frozen at the M0 gate. | Nothing |
| `src/platform/` | Interfaces for hashing, checksum, JSON, HTTP server, logging, clock and configuration, each with its adapter. | `third_party/` |
| `src/wire/` | Frame header, opcodes, payload encode and decode. No sockets. | `platform` |
| `src/coh/` | Requester and home state machines and the directory. Pure: events in, actions out. | `wire` |
| `src/mem/` | Region mapping, userfaultfd, handler thread, install, zap, write-protect, wake. | `platform` |
| `src/store/` | Home store, CLOCK eviction, spill file, local-cache accounting. | `platform` |
| `src/net/` | epoll loop, TCP mesh, timers, heartbeats. | `wire` |
| `src/rt/` | Task registry, queue, worker threads, locks, barriers, atomics, allocator. | `wire`, `net` |
| `src/lib/` | Wires the above into `libparamesh` and implements `paramesh.h`. | All of the above |
| `src/pmd/` | Daemon: discovery, control channel, admission, spawn, ledger, slot allocation, status API, local socket. | `wire`, `net`, `platform` |
| `src/tools/` | `pm` and `pmrun`. | `platform`, `wire` |
| `apps/` | Example programs: counter, matrix multiply, false-sharing demo. | `paramesh.h` only |
| `tests/unit/` | doctest unit tests, one file per source directory. |  |
| `tests/sim/` | Fake message bus, fuzz harness, invariant checker. | `coh`, `wire` |
| `tests/multi/` | Scripts that start several nodes on localhost or in network namespaces. | Built binaries |
| `tools/` | Wireshark dissector, dashboard page, environment check, benchmark scripts. |  |
| `third_party/` | Vendored headers, each with its licence file and a `README` giving version and source. |  |
| `docs/` | `PROTOCOL.md`, `STATE_MACHINES.md`, `INTERNAL_API.md`, `TODO.md`. |  |
| `AGENTS.md` | The guide file for coding agents. |  |

**Toolchain.** C++20, CMake with Ninja, GCC or Clang as shipped with the laptops' distribution. Dependency direction is one-way as listed; a source directory never includes headers from one that depends on it.

**Vendored versions.** I could not confirm the libraries' current versions and licences from here. Task M0-1 records the exact version and licence text of each when it copies them in, and stops if any is not a permissive licence.

**Swappable parts.** Each capability in `src/platform/` is a pure-virtual interface in its own header, with adapters in a subdirectory and one factory that maps a configured name to an implementation. Code elsewhere includes only the interface header. Replacing a vendored library later means adding an adapter class and one registration line.

## Engineering rules

These rules go into `AGENTS.md` and apply to every pull request.

| Rule | What it requires |
| --- | --- |
| Scope | A task touches only the files its card lists. A change to a frozen file needs a person's approval first. |
| Ownership of memory | No raw `new` or `delete`. Resources are held by RAII types: `std::unique_ptr`, containers, and small wrappers for file descriptors and mappings. |
| Errors in `libparamesh` | Functions return a `Result<T>`. The library is compiled with exceptions off. A fatal condition goes through one abort-the-job function that logs the reason. |
| Errors in `pmd` and tools | Exceptions are allowed and are caught at every thread entry point. |
| Task bodies | The one file that calls application task bodies is compiled with exceptions on and catches everything. |
| C boundary | No exception, C++ type or allocation ownership crosses `paramesh.h`. |
| Blocking | The fault-handler thread waits only on the userfaultfd and an eventfd. The network thread waits only on epoll. Neither takes a lock that a blocking thread can hold. |
| State machines | Code in `src/coh/` makes no system calls and reads no clock. Time arrives as an event. |
| Trust | No value received from another node is an input to a weight, a quota or an admission decision. A unit test feeds forged announcements and checks the weights do not move. |
| Never zero-fill | No code path installs zeros or a stale copy to wake a thread. If data is lost, the job aborts. |
| Tests | Every task adds unit tests for what it adds. A bug fix adds the test that would have caught it. |
| Quality gate | clang-format, clang-tidy and warnings as errors pass. Unit tests pass under address, undefined-behaviour and thread sanitizers in GitHub Actions. |
| Logging | One JSON object per line with time, node, job, level, event and fields, through the logging interface. |
| Configuration | Command-line flags for caps and peers; all tunables from a key=value file. Settings reach job processes as environment variables set by `pmd`. |
| Commits | Small, one purpose each, with the task ID in the pull-request title. |
| Deferred work | Anything left undone is added to `docs/TODO.md` in the same pull request. |

## Milestones and streams

The seven milestones run strictly in order, and inside each one at most three streams run side by side between a shared start and a shared finish.

&#91;embedded content: milestones M0 to M6 · streams and gates\]

Read each row left to right: one agent prepares what the streams share, the streams work in separate directories, one agent joins the results, and a person closes the gate. AT-1 follows the M3 gate and must merge before stream B of M4 starts, because both change the home side.

## M0: foundations and interface freeze

M0 ends with a program running on memory it never allocated, on one machine, and with four interface files approved.

| ID | Task | Files | Needs | Stream | Done when |
| --- | --- | --- | --- | --- | --- |
| M0-1 | Repository skeleton: CMake project with every directory as an empty target, C++20, warnings as errors, clang-format and clang-tidy settings, vendored headers with licences and versions recorded, `AGENTS.md`, `docs/TODO.md`, GitHub Actions workflow for build and unit tests under the three sanitizers. | Root, `third_party/`, `.github/` | None | seq | A clean clone builds and `ctest` passes locally and in Actions. |
| M0-2 | Environment check script: kernel version, user-mode userfaultfd without root, write-protect mode on anonymous memory, the region base address is free, sanitizers coexist with a fixed-address 4 GB mapping, network namespaces and `tc netem` available. | `tools/check_env.sh` | M0-1 | C | The script prints pass or fail per item and exits non-zero on any fail. |
| M0-3 | Platform interfaces and adapters: hasher (PicoSHA2), checksum (own table-based CRC-32C), JSON codec (nlohmann/json), HTTP server (cpp-httplib), logger (JSON lines), clock (real and fake), configuration loader (key=value), and the factory. | `src/platform/`, `tests/unit/` | M0-1 | A | CRC-32C and SHA-256 match published test vectors; a second dummy adapter is selected by configuration name with no change to calling code. |
| M0-4 | Draft the public header in full: every function signature, the configuration struct, the task function signature, `pm_lock_t`, error codes. | `include/paramesh.h` | M0-1 | B | Compiles as C11 and as C++20; every *Still open* point that belongs here is answered or flagged for the gate. |
| M0-5 | Draft the protocol document: header, every opcode with its payload fields and sizes, `HELLO` contents, the local Unix-socket messages, timeouts, error codes. | `docs/PROTOCOL.md` | M0-1 | B | Every opcode in the HLD has a payload definition; byte order and alignment are stated. |
| M0-6 | Draft the state-machine document: node page states (three stable, three in flight), home directory states, every event and action, the full transition tables. | `docs/STATE_MACHINES.md` | M0-5 | B | Every state and event pair has a defined outcome or is marked impossible with a reason. |
| M0-7 | Draft the internal interfaces between directories: `net` send and receive callbacks, `mem` install, zap, write-protect and wake, `coh` step functions, `store` get and put. | `docs/INTERNAL_API.md`, header stubs in each `src/` directory | M0-5 | B | Headers compile; no directory needs another's private types. |
| M0-8 | userfaultfd on one machine: map the 4 GB region at the fixed base, register missing and write-protect modes, a handler thread that fills pages from a local callback, and the four `mem` helpers. | `src/mem/`, `apps/m0_demo`, `tests/unit/` | M0-2 | C | The demo reads and writes memory it never allocated; unit tests cover install, zap, write-protect, wake and the `EEXIST` case. |

**Streams.** M0-1 runs alone first. Then A (M0-3), B (M0-4 to M0-7, one agent, in order) and C (M0-2 then M0-8) run in parallel. M0-8 uses only the `mem` helper names from the HLD; if M0-7 renames them, M0-8 is adjusted before the gate.

**Gate.** A person approves `paramesh.h`, `PROTOCOL.md`, `STATE_MACHINES.md` and `INTERNAL_API.md`, answers every flagged point, runs `check_env.sh` on all six laptops, and sees the demo run. After this gate the four files change only with approval.

## M1: read sharing over TCP

M1 ends with node B reading an array that node A wrote, with no socket code in the application (FR-4).

| ID | Task | Files | Needs | Stream | Done when |
| --- | --- | --- | --- | --- | --- |
| M1-1 | Wire format: 36-byte header encode and decode, opcode table, payload structs for the M1 opcodes, CRC-32C check, magic and version checks. | `src/wire/`, `tests/unit/` | M0 gate | seq | Round-trip tests pass; fixed byte sequences decode to the expected fields; a corrupted payload is rejected. |
| M1-2 | Transport: epoll loop, non-blocking TCP, one connection per process pair, frame reassembly, `writev` of header and payload, `TCP_NODELAY`, heartbeats every 1 s, a 2 s reply timer with one retry. | `src/net/`, `tests/unit/` | M1-1 | A | Two processes on localhost exchange 100,000 frames of mixed size with none lost or reordered; a killed peer is reported within 3 s. |
| M1-3 | Requester state machine, read path: page states for invalid and shared, the in-flight read state, de-duplication of faults on one page. | `src/coh/`, `tests/unit/` | M1-1 | B | Unit tests cover every transition M1 uses, as listed in `STATE_MACHINES.md`. |
| M1-4 | Home state machine, M1 subset: directory entry, per-page serialisation with busy flag and wait queue, `READ_REQ` answered from the home copy or with the zero-page flag, `READ_REQ` on a page another node holds for writing (fetch from the owner), and `WRITE_REQ` on a page nobody else holds. Any other request stops the job with "not supported before M2". | `src/coh/`, `tests/unit/` | M1-3 | B | Unit tests cover those cases and the refusal. |
| M1-5 | Fake message bus and fuzz harness: delivers, delays and reorders messages between simulated nodes from a seed; an invariant checker runs after every step. | `tests/sim/` | M1-4 | B | A failing seed reproduces exactly; 2,000 schedules pass in Actions. |
| M1-6 | Home store in RAM (no eviction yet), the bump allocator behind `pm_malloc`, and RAM-weighted rendezvous hashing of segments to homes. | `src/store/`, `src/rt/`, `tests/unit/` | M1-1 | C | Hashing is identical on every node for the same inputs; removing one node moves only that node's segments. |
| M1-7 | Job bring-up without `pmd`: role, job ID and peer addresses from environment variables; `pm_init`, `JOIN_JOB` with the binary hash, `SEG_MAP` at epoch 1, `pm_finalize`, `JOB_END`; the handler, requester, transport, home and memory engine wired together. A lost node or second timeout makes every process exit with a message. | `src/lib/`, `src/mem/` | M1-2, M1-5, M1-6 | seq | Three processes on localhost complete the M1 test program. |
| M1-8 | `pm_touch`, and the check that a system call given a missing shared page returns `EFAULT`. | `src/lib/`, `tests/multi/` | M1-7 | seq | `read()` into an untouched shared buffer returns `EFAULT`; after `pm_touch` it succeeds. |
| M1-9 | Multi-node test scripts: start N nodes on localhost; start N nodes in network namespaces with a chosen delay and loss. A read-sharing test program. | `tests/multi/`, `apps/` | M1-7 | seq | Both scripts run the test program and report pass or fail. |

**Streams.** M1-1 alone first. Then A (M1-2), B (M1-3, M1-4, M1-5 in order) and C (M1-6) in parallel: they share no files and meet only at the frozen interfaces. M1-7 onwards is one agent.

**Gate.** FR-4.1: B reads what A wrote. FR-4.2: the message trace shows one round trip from the home and two when another node holds the page for writing. FR-4.3 as in M1-8. Run on three simulated nodes and on the two cabled laptops.

**Running code on workers before M3.** The task runtime arrives in M3, so the M1 and M2 test programs use a test-only hook, outside the public header, that lets each process run a function chosen by its role. M3-7 moves the counter onto `PM_TASK` and removes the hook from the applications.

## M2: write-invalidate, locks and atomics

M2 ends with a shared counter that stays exact when three nodes increment it (FR-5, UC-2).

| ID | Task | Files | Needs | Stream | Done when |
| --- | --- | --- | --- | --- | --- |
| M2-1 | Home state machine, complete: `WRITE_REQ` and `UPGRADE_REQ` with `INV` to every other holder and a wait for every `INV_ACK`; `FETCH_INV` to an exclusive owner; `WRITEBACK` and `WRITEBACK_ACK`; the page version. | `src/coh/` (home files), `tests/unit/` | M1 gate | A | Unit tests cover every home transition in `STATE_MACHINES.md`. |
| M2-2 | Requester state machine, complete: the modified state, upgrade, and replies to `INV`, `FETCH` and `FETCH_INV` in the fixed order write-protect, send, wait for acknowledgement, zap, wake. | `src/coh/` (node files), `tests/unit/` | M1 gate | B | Unit tests cover every node transition; no test can observe a zap before the acknowledgement. |
| M2-3 | Memory engine, write side: write-protect faults, changing protection on a range, installing a page already write-protected. | `src/mem/`, `tests/unit/` | M2-2 | B | A write to a shared page raises one event and resumes after the grant. |
| M2-4 | Locks and barriers, managed by the launcher: first-in-first-out lock queue, barrier counting, `pm_wait_all` as a phase barrier. | `src/rt/` (sync files), `tests/unit/` | M1 gate | C | Unit tests on the lock and barrier logic with a fake transport. |
| M2-5 | `pm_atomic_add`, executed at the page's home as one message that returns the old value. The home first takes the page back if another node holds it. | `src/rt/`, `src/coh/` | M2-1, M2-4 | seq | Concurrent adds from three simulated nodes sum exactly. |
| M2-6 | Fuzzing the full protocol: invariants are one writer or many readers, no lost update, no stale read, no early discard. | `tests/sim/` | M2-1, M2-2 | seq | 100,000 fuzzed schedules pass locally; 2,000 pass in Actions on every push. |
| M2-7 | The counter application and its multi-node test. | `apps/counter`, `tests/multi/` | M2-3, M2-5, M2-6 | seq | Three nodes each add N times with a lock, then with `pm_atomic_add`; both totals are exact. |

**Streams.** A, B and C run in parallel: the home and node state machines are separate files that agree only through the frozen transition tables, and locks do not touch either. M2-5 onwards is one agent.

**Gate.** FR-5.1 and FR-5.2 by M2-6. FR-5.3 and UC-2 by M2-7, on three simulated nodes and on the two cabled laptops.

## M3: task runtime, launch and speed-up

M3 ends with matrix multiply at n = 4096 running faster on several nodes than on one (FR-3, FR-9, UC-1).

| ID | Task | Files | Needs | Stream | Done when |
| --- | --- | --- | --- | --- | --- |
| M3-1 | Task registry: `PM_TASK` emits a constructor function that registers the task; the ID is the 64-bit FNV-1a hash of the name; two names with one hash stop the program at start-up. The caller of task bodies catches any exception and aborts the job with the task name. | `include/paramesh.h` (macro body only), `src/rt/`, `tests/unit/` | M2 gate | A | A task registered in a C file and one in a C++ file are both found by hash; a forced collision stops start-up with a clear message; a throwing task aborts the job. |
| M3-2 | Queue and workers: `pm_parallel_for` cuts the range into page-aligned chunks and queues them in the launcher's job process; worker threads pull with `TASK_REQ` and receive `TASK_ASSIGN` or `NO_TASK`; one chunk kept prefetched; back-off after `NO_TASK`; `TASK_DONE`; the launcher tracks outstanding chunks; `pm_wait_all` sleeps until all are done. | `src/rt/`, `tests/unit/` | M3-1 | A | Every index in the range is processed exactly once across three simulated nodes. |
| M3-3 | Data affinity: on `TASK_REQ` the launcher prefers a chunk whose pages are homed on the asking node, read from its own segment map, and falls back to any chunk. | `src/rt/` | M3-2 | A | With affinity on, fewer page transfers are logged for the same run than with it off. |
| M3-4 | Minimal `pmd`: daemon control channel on TCP 47001 with a `--peers` list, `SPAWN_REQ` and `SPAWN_OK`, starting the job binary with its role and addresses, the binary-hash check, the Unix socket to job processes, a thread quota fixed at `--cap-cores`. | `src/pmd/`, `tests/unit/` | M2 gate | B | A job process started by `pmd` receives its quota; a binary with a different hash is refused with "binary mismatch". |
| M3-5 | `pmrun -n N ./app`: asks the local `pmd`, which picks the N-1 peers with the most free capacity and asks each; a peer that declines is replaced by the next. Ctrl+C aborts the job on every node. | `src/tools/`, `src/pmd/` | M3-4 | B | A job starts on three simulated nodes from one command; Ctrl+C leaves no process behind. |
| M3-6 | Matrix multiply application and the benchmark script: the plain program on one machine, ParaMesh on one node, ParaMesh on N nodes; five runs each, median and range; chunk counts per node from the log. | `apps/matmul`, `tools/bench/` | M2 gate | C | The application compiles against `paramesh.h`; the script produces a results table. |
| M3-7 | Integration: job processes take their quota from `pmd`, the counter is moved onto `PM_TASK`, and the test-only hook is removed from the applications. | `src/lib/`, `apps/counter` | M3-3, M3-5, M3-6 | seq | Counter and matrix multiply both run through `pmrun` on three simulated nodes. |

**Streams.** A (M3-1 to M3-3, in order), B (M3-4 then M3-5) and C (M3-6) run in parallel. C writes only an application against the frozen header, so it cannot conflict. M3-7 is one agent.

**Gate.** FR-3.1 to FR-3.3 by M3-1, M3-2 and M3-4. FR-9.1: at least two nodes run chunks of one job, shown by the per-node chunk counts. FR-9.2: matrix multiply at n = 4096 is faster on the two cabled laptops than on one. The SRS target of 2 to 2.5 times on three laptops is measured when the switch arrives.

### After the M3 gate: anti-thrashing

| ID | Task | Files | Needs | Stream | Done when |
| --- | --- | --- | --- | --- | --- |
| AT-1 | Hold window at the home: count ownership transfers per page; above the threshold, hold the current writer's grant for a window that doubles while the fight continues and resets when it stops; report the page and the two tasks in the log. The false-sharing demo: two tasks writing neighbouring integers. | `src/coh/` (home files), `apps/falseshare`, `tests/unit/` | M3 gate | seq | The demo is slow without the hold window and recovers with it; the report names the page and both tasks; the M2 fuzz run still passes. |

AT-1 edits the home state machine, as M4-5 does, so it is finished and merged before M4-5 starts.

## M4: discovery, caps, leave, spill and abort

M4 ends with a node leaving in the middle of a job and the job still finishing with the right answer (FR-1, FR-1a, FR-2, FR-8, FR-10.1).

| ID | Task | Files | Needs | Stream | Done when |
| --- | --- | --- | --- | --- | --- |
| M4-1 | Discovery: `HELLO` every 1 s and `BYE` on UDP multicast 239.77.77.77:47000 with TTL 1; peer table (three missed beacons suspect, five removed); `--peers` seed list where multicast is blocked; random node ID created on first run and saved in the state directory; a clash of IDs reported. `GET /v1/peers` and `pm status`. | `src/pmd/`, `src/tools/`, `tests/unit/` | M3 gate | A | Three nodes list each other in `pm status` within 2 s; a silent node disappears after 5 s; the seed list works with multicast off. |
| M4-2 | Caps: `--cap-ram` limits the home store and the local cache together; `--cap-cores` is the largest quota `pmd` ever pushes. Local cache evicts in first-in-first-out order; a modified page is written back and zapped only after `WRITEBACK_ACK`. | `src/store/`, `src/lib/`, `src/pmd/` | M3 gate | C | Memory sampled every 1 s during a run never exceeds the cap; worker threads never exceed the core cap. |
| M4-3 | Spill at the home: when the home store is full, CLOCK picks a cold page and writes it to a local spill file; a later request reads it back first; the directory records where each page is. | `src/store/`, `tests/unit/` | AT-1 | B | A job whose region is twice the RAM caps completes with the right answer. |
| M4-4 | Admission: `pmrun` is refused when the region exceeds 90% of the chosen nodes' RAM caps plus spill budgets, with the needed and available amounts in the message; `pm_malloc` returns `NULL` past the region end. | `src/pmd/`, `src/rt/` | M4-1, M4-3 | A | An oversized job is refused before any process starts. |
| M4-5 | Graceful leave of a worker, by `pm leave` or Ctrl+C on its `pmd`: `LEAVE_INTENT`; no new tasks; running tasks get 10 s, then return to the queue; modified pages written back; shared pages dropped; each home segment frozen (`BUSY_RETRY`), drained and sent with `SEG_MIGRATE`; new `SEG_MAP` at a higher epoch; old-epoch requests get `REDIRECT`; `LEAVE_DONE`, then `BYE`. On a launcher, `pm leave` is refused while its job runs and `pm leave --force` aborts the job. | `src/coh/`, `src/rt/`, `src/pmd/`, `src/store/`, `src/tools/` | M4-1, M4-2, M4-3 | seq | A worker leaves during matrix multiply; the job finishes and the result equals the single-machine result; no task is lost. |
| M4-6 | Abort path: 3 s without `HEARTBEAT`, a TCP reset, or a second 2 s reply timeout makes the launcher send `JOB_END` with the error and the lost node; every process prints it and exits; each `pmd` frees the job's home store and spill file. | `src/lib/`, `src/pmd/` | M4-2 | C | Killing a node mid-run ends the job everywhere within 3 s with "job N aborted: node K lost"; nothing hangs. |
| M4-7 | Mid-run join by automatic refill: when a job runs on fewer nodes than it asked for, its launcher's `pmd` offers it to a newly seen peer; the new node receives the segment map, joins the mesh, pulls tasks, and hosts no homes. | `src/pmd/`, `src/lib/`, `src/rt/` | M4-5 | seq | After one node leaves a three-node job, a fourth node joins and its chunk count rises. |
| M4-8 | Leave, spill, abort and join tests under delay and loss in network namespaces. | `tests/multi/` | M4-5, M4-6, M4-7 | seq | The scripts pass at 0, 5 and 25 ms added delay and at 0 and 2% loss. |

**Streams.** A (M4-1 then M4-4), B (M4-3) and C (M4-2 then M4-6) run in parallel. M4-2 and M4-3 both sit in `src/store/` but in different files: the local-cache accounting and the home store. In the daemon's directory, stream A edits the discovery and admission files and stream C edits only the quota and job-cleanup files. M4-5, M4-7 and M4-8 change membership across several directories and are done by one agent, in order.

**Gate.** FR-1.1, FR-1a, FR-2.1 to FR-2.3, FR-8.1, FR-8.2 and FR-10.1 by the checks above, on three simulated nodes and the two cabled laptops. Pulling the cable out mid-run is the hardware test for FR-10.1. FR-1.2 by M4-7.

## M5: ledger and weighted fair sharing

M5 ends with worker slots following contribution: three equal contributors share evenly, and a free-rider's share falls but never reaches zero (FR-6, FR-7, UC-4).

| ID | Task | Files | Needs | Stream | Done when |
| --- | --- | --- | --- | --- | --- |
| M5-1 | Ledger store: per peer and per job, in both directions, the raw counts (chunks completed per task, segment-seconds hosted) and the credits; saved as JSON in the state directory by writing a temporary file and renaming it. Job processes report completions to `pmd` over the Unix socket. | `src/pmd/` (ledger files), `tests/unit/` | M4 gate | seq | The ledger survives a `pmd` restart; a crash during a write leaves the previous file intact. |
| M5-2 | Credit rules. *Given to me by peer P*: each chunk P completed for my job, valued at my own measured cost for a chunk of that task and size (the median over workers until I have run one myself), plus segments of my job homed on P x 2 MB x time, converted at the configured rate. *Taken from me by P*: my own measured CPU time on P's chunks, plus the home-store segments I held for P's job x time, converted at the same rate. Balance = given - taken. | `src/pmd/` (ledger files), `tests/unit/` | M5-1 | A | Unit tests with fixed inputs give the expected balances; no input comes from a received message field. |
| M5-3 | Weight and slot allocation: `w = max(0.25, 1 + balance / scale)`; when a slot frees at a task boundary, it goes to the guest job with the lowest dominant share divided by `w`, where the share is over this node's worker slots and home-store RAM; the node's own jobs come first; `pmd` pushes the new quotas. | `src/pmd/` (scheduler files), `tests/unit/` | M5-1 | B | Unit tests with a mock ledger show slots following weights; a job is never left with zero slots for longer than one task boundary while it has work. |
| M5-4 | Announcements and alarm: raw totals on `HELLO` and in `LEDGER_SYNC`, shown in `pm status` and at `GET /v1/ledger`; an alarm when the two sides' raw counts differ by more than 10%. A test sends forged totals and checks that no weight changes. | `src/pmd/` (status files), `src/tools/` | M5-1 | C | `GET /v1/ledger` matches the task log; the forged-totals test passes. |
| M5-5 | Fairness experiments as scripts: three concurrent jobs from equal contributors (Jain's index); a node contributing no cores (its jobs' share over time); fault latency with and without contention. | `tools/bench/`, `tests/multi/` | M5-2, M5-3, M5-4 | seq | The scripts print Jain's index, the free-rider's share curve and the two latency distributions. |

**Streams.** M5-1 alone first. Then A, B and C in parallel, each in its own files, with B using a mock of the ledger interface that M5-1 fixed. M5-5 is one agent.

**Gate.** FR-6 by M5-4. FR-7.1: Jain's index at least 0.9. FR-7.2: the free-rider's share falls and stays above zero. FR-7.3: no change in fault latency under contention. The trust rule: the forged-totals test.

## M6: observability and evaluation

M6 ends with a live view of the pool and every ParaMesh frame readable in Wireshark (UC-5, NFR-6).

| ID | Task | Files | Needs | Stream | Done when |
| --- | --- | --- | --- | --- | --- |
| M6-1 | Status API, complete: `GET /v1/jobs`, `/v1/jobs/{id}/pages` and `/v1/events` (server-sent events with faults per second, bytes per second, joins and leaves), beside the existing `/v1/peers` and `/v1/ledger`. Fault-latency counters exported as a histogram. | `src/pmd/` (status files), `src/lib/` | M5 gate | seq | Each endpoint returns the documented JSON; a change in the pool appears within 1 s. |
| M6-2 | `pm top`: a terminal view of faults per second, MB per second and page ownership, redrawn with terminal escape codes. | `src/tools/` | M6-1 | A | It updates once a second during a run and restores the terminal on exit. |
| M6-3 | Dashboard: one static HTML and JavaScript file served by `pmd`, reading the event stream; peers, rates, page ownership, and a progress bar during segment migration. No external library. | `tools/dashboard/` | M6-1 | B | It opens from `127.0.0.1:7070` and matches `pm top` during a run. |
| M6-4 | Wireshark dissector in Lua for the 36-byte header and every opcode. | `tools/dissector/` | M5 gate | C | A capture of a three-node run shows every frame decoded, with no "malformed" entries. |
| M6-5 | Evaluation runs for matrix multiply: the impairment matrix (added delay 0, 1, 5, 10, 25, 50 ms; jitter 0 and 25%; loss 0, 1, 2, 5%) in network namespaces; the fault-latency histogram on the cabled laptops; an out-of-core run with the region larger than one node's cap. | `tools/bench/` | M6-1 | seq | One results file per experiment with run time, faults per second, MB per second, median and 99th-percentile fault latency, retries and aborts. |

**Streams.** M6-1 first. Then A, B and C in parallel; M6-4 needs only `PROTOCOL.md` and can start at the M5 gate. M6-5 last.

**Gate.** UC-5 and NFR-6 by M6-2 to M6-4. NFR-9 (a fault served by its home in under 1 ms on wired gigabit) by the histogram in M6-5. The "novelty features" the SRS mentions for M6 are not defined and are parked in `docs/TODO.md`.

## Tunables

The values in the first table are my proposals and need your review; none has been measured. Each is a key in the configuration file, and M5-5 and M6-5 are where they get tuned.

| Key | Proposed default | Reasoning |
| --- | --- | --- |
| `ledger.scale` | 3,600 core-seconds | A credit of one core-hour doubles a peer's weight; a debt of 0.75 core-hours reaches the 0.25 floor. |
| `ledger.mem_rate` | 1 GB-hour = 450 core-seconds | Treats 8 GB held for an hour as worth one core-hour. A rough ratio with no measurement behind it. |
| `task.no_task_backoff` | 10 ms, doubling to 200 ms | Short enough that a refilled queue is noticed quickly, long enough not to flood the launcher. |
| `coh.busy_retry_backoff` | 5 ms, doubling to 100 ms | A frozen segment migrates in seconds, so retries need not be faster. |
| `thrash.threshold` | 8 ownership transfers of one page in 100 ms | Well above what ordinary hand-over produces. |
| `thrash.hold_initial` and `thrash.hold_max` | 1 ms, doubling to 64 ms | Starts near one round trip and grows only while the fight lasts. |
| `mem.cache_share` | 25% of `--cap-ram` for the local cache, 75% for the home store | The home store holds the only copy of a page, so it gets most of the cap. |
| `spill.budget` | Equal to `--cap-ram` | Lets a region reach about twice the RAM caps, which is what the FR-8.1 test needs. |
| `task.default_grain` | Range length / (4 x total worker slots), rounded to a page boundary | About four chunks per slot, so fast nodes can take more. Used when the caller passes 0. |
| `task.prefetch` | 1 chunk | As the HLD says. |
| `fuzz.schedules` | 2,000 in Actions, 100,000 at the M2 gate | Keeps each push fast while the gate meets FR-5.1's stated count. |
| `log.level` | `info` |  |

These values are fixed by the SRS and HLD and are not proposals: reply timeout 2 s with one retry; `HEARTBEAT` every 1 s, abort after 3 s; `HELLO` every 1 s, suspect after 3 missed, removed after 5; leave grace 10 s; admission headroom 90%; weight floor 0.25; region 4 GB at `0x600000000000`; segment 2 MB; page 4 KB; at most 8 nodes.

## Still open

No document decides these 17 points and I have not chosen an answer for any of them. Each is put to you in the M0 draft that owns it, and a task that reaches one earlier stops and asks.

| # | Point | Why it is open | Settled in |
| --- | --- | --- | --- |
| 1 | The exact signature of every API function, the fields of the `pm_init` configuration, the task function's parameters, and what `pm_lock_t` contains. | The SRS gives names and behaviour only. | M0-4 |
| 2 | The payload fields of every opcode. | The HLD lists opcodes without payloads, except the page payload. | M0-5 |
| 3 | What `HELLO` carries now that the speed score is gone, and the format of the raw ledger totals on it. | The HLD list included the score. | M0-5 |
| 4 | Whether `TASK_DONE` still carries CPU-seconds. | The HLD says it does; the ledger no longer uses a reported value. | M0-5 |
| 5 | The message set on the Unix socket between a job process and `pmd`. | New in this plan. | M0-5 |
| 6 | The three in-flight node states and every transition. | The HLD names "3 in flight" without listing them in text. | M0-6 |
| 7 | How a 16-bit random node ID maps to a bit in the 64-bit copyset. | The HLD has both and no mapping. | M0-5 |
| 8 | Who generates the 32-bit job ID and how clashes are avoided. | Not stated anywhere. | M0-5 |
| 9 | Which network interface discovery uses when a laptop has several. | Not stated; matters with a cable plus Wi-Fi. | M0-5 |
| 10 | What `pmrun -n N` does when fewer than N nodes accept: run on fewer, or refuse. | Not stated. | M0-5 |
| 11 | What a job does if its local `pmd` dies while the job is running. | Not stated; the quota and ledger reports stop. | M0-5 |
| 12 | Data affinity by "cached at". The HLD says the launcher prefers chunks whose pages are cached at the asking worker, but the launcher only knows the directory of pages it is home for. M3-3 as written uses home placement only. | The HLD rule cannot be met from the launcher's own data. | M0 gate |
| 13 | The unit of work in a chunk for valuing credit: the range length, or something the task declares. | "The same amount of work" needs a measure. | M0-4 |
| 14 | The credit rule for work taken from me: my own measured CPU time on the peer's chunks. This comes from our discussion, not from one of your recorded answers. | Needs your confirmation. | M0 gate |
| 15 | How `--cap-ram` is split between the home store and the local cache, and whether the spill budget is a flag. | The documents give one cap for "shared data". Proposed values are in Tunables. | M0 gate |
| 16 | Whether a peer seen for the first time starts at balance 0 (weight 1) or lower. | A probation weight is listed as pending security work. | `docs/TODO.md` |
| 17 | Whether the sanitizers can run on code that maps the fixed 4 GB region. | Unknown until tried. If not, the kernel-dependent tests run without them. | M0-2 |

## Changes from the SRS and HLD

This plan departs from the earlier documents in 30 places; the earlier documents are unchanged, and this table is the reference for bringing them in line. "Doc 1" is ParaMesh Scope and System Behaviors, "HLD" is the High-Level Design, "SRS" is v0.5.

### Ledger and fairness

| # | Earlier documents said | The plan does | Why | Where to update |
| --- | --- | --- | --- | --- |
| 1 | Compute credit = CPU-seconds reported in `TASK_DONE` x the provider's speed score (HLD, Scheduling and fairness, Ledger row). | A node values a peer's chunk at the node's own measured cost for the same work. | Both inputs were self-reported, so a node could inflate its own credit. | HLD ledger row and Tasks opcode row; SRS FR-6. |
| 2 | A CPU speed score from a 1 s start-up benchmark is announced on `HELLO` (HLD, Job lifecycle step 1; SRS workflow figure step 1; SRS FR-6 "scaled by a speed score"). | The speed score is removed. | Nothing uses it once credit is valued locally. | HLD step 1; SRS FR-6 and the workflow figure; viva notes. |
| 3 | Balance = credits earned - credits spent, not defined further; "totals ride on `HELLO`" (HLD, Balance row). | Balance at node N for peer P = what P gave N - what P took from N, from N's own records only. | An announced total could be forged to gain priority. | HLD Balance row; SRS FR-6; viva notes line on totals. |
| 4 | Totals on `HELLO` and `LEDGER_SYNC`, purpose unstated. | Display and alarm only; never an input to a weight. | Same as above. | HLD Balance row and opcode table note. |
| 5 | "Flag a disagreement over 10% between two sides" (SRS FR-6), implicitly on credits. | The alarm compares raw counts: chunks completed and segment-seconds. | Each side now values work at its own cost, so credits differ for honest reasons. | SRS FR-6 wording. |
| 6 | Dominant share = the job's share of pool cores and pool memory (HLD, Slot allocation row). | The job's share of this node's worker slots and home-store RAM. | Pool-wide usage is only known from other nodes' announcements. | HLD Slot allocation row. |
| 7 | Memory credit = GB-hours of home store hosted, measurement unstated. | Segments assigned x 2 MB x time, computed from the node's own segment map. | Only the host knows actual pages stored, and its report would be self-reported. | HLD ledger row; SRS FR-6. |
| 8 | One `balance` in the weight formula, two resources tracked. | Memory credit is converted to compute credit at a fixed, configurable rate, then summed. | The formula needs one number. | HLD Weight row; SRS FR-7.1. |
| 9 | Enforcement point not stated as a rule. | Stated as a rule and tested: no received value feeds a weight, quota or admission decision. | Makes the ledger fix checkable. | SRS, as a new requirement beside FR-7. |

### Membership, launch and scheduling

| # | Earlier documents said | The plan does | Why | Where to update |
| --- | --- | --- | --- | --- |
| 10 | "The launcher cannot leave while its job runs" (HLD, Graceful leave); `pm leave` described for any node (Doc 1, Operator CLI). | On a launcher, `pm leave` is refused, `pm leave --force` aborts the job, and Ctrl+C on `pmrun` aborts the job. Ctrl+C on a worker's `pmd` is still a graceful leave. | The launcher case had no defined user-facing behaviour. | HLD Graceful leave; Doc 1 CLI table and failure table; SRS FR-2. |
| 11 | A 16-bit node ID with no stated origin. | A random ID created on first run and saved on disk; clashes reported. | The ledger needs an identity that survives restarts. | HLD frame header and step 1. |
| 12 | Peers "chosen by fair share" (HLD step 2; Doc 1 CLI). | The launcher plus the N-1 peers with the most free capacity; each accepts or declines. | "Fair share" was never defined for choosing nodes. | HLD step 2; Doc 1 CLI table. |
| 13 | "A central pull queue" (SRS 7.5). | The queue is in the launcher's job process, as the HLD already says. | Removes the ambiguity in the SRS. | SRS 7.5. |
| 14 | No description of how a job process gets its worker-slot count. | A Unix socket to the local `pmd`, which pushes a thread quota; a thread over quota parks at its next task boundary. | Needed to make "slots at task boundaries" buildable. | HLD Node components and Channels. |
| 15 | "The launcher runs chunks too" (HLD step 5), thread unstated. | The thread in `pm_wait_all()` sleeps; the launcher's worker threads run chunks. | One code path for launcher and workers. | HLD step 5. |
| 16 | Locks at "a manager node" (Doc 1, API table). | The launcher manages locks and barriers. | The launcher never leaves mid-job, so lock state never migrates. | Doc 1 API table; HLD step 7. |
| 17 | "The wire carries a hash of the name" (Doc 1; SRS FR-3.1). | A constructor function per task; 64-bit FNV-1a; a collision stops start-up. | Fixes the mechanism and the hash. | Doc 1 API table; SRS FR-3.1. |
| 18 | Data affinity by pages "homed on, or cached at" the worker (HLD, Inside one job; SRS 7.5). | Home placement only, pending your decision (Still open, point 12). | The launcher does not hold other homes' directories. | HLD and SRS 7.5, once decided. |
| 19 | A node "may join a running job" (SRS FR-1.2), mechanism unstated. | Automatic refill when a job runs on fewer nodes than it asked for. | Gives FR-1.2 a trigger. | SRS FR-1.2; Doc 1 FR-1 row. |
| 20 | Anti-thrashing in the HLD home rules, in no SRS milestone. | Its own task after the M3 gate, with the false-sharing demo. | Coherence is correct without it. | SRS build milestones. |

### Protocol and memory

| # | Earlier documents said | The plan does | Why | Where to update |
| --- | --- | --- | --- | --- |
| 21 | The `COMPRESSED` flag means LZ4 page data (HLD, frame header). | The flag is reserved and never set. | Avoids another library this semester. | HLD page-payload sentence. |
| 22 | A stride prefetcher after 3 sequential faults (Doc 1, rejected-mechanisms table). | Deferred to `docs/TODO.md`. | Decided once the M3 speed-up is measured. | Doc 1 table. |
| 23 | Spill at the home or at the origin listed as open (Doc 1, Open questions). | At the home. | Closes the open question. | Doc 1 Open questions. |

### Process, testing and scope

| # | Earlier documents said | The plan does | Why | Where to update |
| --- | --- | --- | --- | --- |
| 24 | Six modules owned by six named members; interfaces "frozen in week 1" (SRS 8.1; HLD Module boundaries). | One to three coding agents in streams; four interface files approved at the M0 gate. `INTERNAL_API.md` is added to the three files the SRS lists. | The implementers changed. | SRS 8.1; HLD Module boundaries. |
| 25 | Directory layout `src/mem`, `coh`, `net`, `pmd`, `rt` (HLD). | Adds `src/platform`, `wire`, `store`, `lib`, `tools` and `apps`. | Separates swappable parts and pure code. | HLD Module boundaries table. |
| 26 | "A developer writes one ordinary C program" (Doc 1); implementation language unstated. | The library, daemon and tools are C++20 behind the same C header. | Your choice; the API is unchanged. | Doc 1 and SRS, one sentence each. |
| 27 | Integration tests "nightly in CI" in network namespaces (SRS 9.1). | Unit tests in GitHub Actions; multi-node tests from a local script. | Hosted runners may not allow userfaultfd or namespaces. | SRS 9.1. |
| 28 | System tests "on 3 to 6 laptops on the switch" (SRS 9.1); wired switch assumed (Doc 1). | Gates pass on three simulated nodes and two laptops on one cable; three-laptop runs when a switch is available. | No switch yet. | SRS 9.1; Doc 1 Scope table. |
| 29 | Six demo workloads and an MPI comparison (Doc 1; SRS 9.2); UC-3 stencil in M6. | Counter, matrix multiply and the false-sharing demo are built; the rest are in `docs/TODO.md`. NFR-1 and UC-3 have no test until they are. | Scope. | Doc 1 Demo workloads; SRS 9.2. |
| 30 | Dates: 30 Sep to an assumed 24 Nov demo, M3 checkpoint 3 Nov (Doc 1, Scope table); M6 "novelty features" (SRS 8.3). | No dates. Novelty features parked until defined. | Your instruction. | Doc 1 Scope table and Open questions; SRS 8.3. |

New in this plan with nothing to contradict: the error-handling policy, swappable interfaces, quality gate, JSON-lines logging, key=value configuration, the state directory, and the single-file dashboard.

## Requirement traceability

Every Must requirement in the SRS has a task and a gate; three items have no test in this plan: NFR-1, UC-3 and FR-10.2.

| Requirement | Built by | Shown at |
| --- | --- | --- |
| FR-1.1 Join and appear within 2 s | M4-1 | M4 gate |
| FR-1.2 Join a running job (Should) | M4-7 | M4 gate |
| FR-1a Caps never exceeded | M3-4, M4-2 | M4 gate |
| FR-2.1 to FR-2.3 Graceful leave | M4-5 | M4 gate |
| FR-3.1 Tasks by name hash | M3-1 | M3 gate |
| FR-3.2 `pm_parallel_for` | M3-2 | M3 gate |
| FR-3.3 Same binary, hash check | M1-7, M3-4, M3-5 | M3 gate |
| FR-3.4 Idempotent tasks, requeue | M3-2, M4-5 | M4 gate |
| FR-4.1, FR-4.2 Automatic fetch | M1-1 to M1-7 | M1 gate |
| FR-4.3 `EFAULT` and `pm_touch` | M1-8 | M1 gate |
| FR-5.1, FR-5.2 One writer or many readers | M2-1, M2-2, M2-3, M2-6 | M2 gate |
| FR-5.3 Locks, barriers, atomics | M2-4, M2-5, M2-7 | M2 gate |
| FR-6 Ledger (Should) | M5-1, M5-2, M5-4 | M5 gate |
| FR-7.1 to FR-7.3 Weighted fair sharing | M5-3, M5-5 | M5 gate |
| FR-8.1 Spill to disk | M4-3 | M4 gate |
| FR-8.2 Admission headroom | M4-4 | M4 gate |
| FR-9.1, FR-9.2 Parallel run and speed-up | M3-2, M3-6, M3-7 | M3 gate; three-laptop figure when a switch is available |
| FR-10.1 Clean abort within 3 s | M1-7, M4-6 | M4 gate |
| FR-10.2 Crash recovery (Future) | Not planned | `docs/TODO.md` |
| UC-1 Matrix multiply | M3-6 | M3 gate |
| UC-2 Shared counter | M2-7 | M2 gate |
| UC-3 Stencil (Should) | Not planned | `docs/TODO.md` |
| UC-4 Free-rider | M5-5 | M5 gate |
| UC-5 Teaching testbed | M6-1 to M6-4 | M6 gate |
| NFR-1 Transparency | No socket code in `apps/` by construction; the MPI comparison is not planned | `docs/TODO.md` |
| NFR-2 Fairness | M5-5 | M5 gate |
| NFR-3 Portability, no root | M0-2 | M0 gate |
| NFR-4 Modularity | M0-7, M1-5 | M1 gate |
| NFR-5 Elasticity | M4-5, M4-7 | M4 gate |
| NFR-6 Observability | M6-1 to M6-4 | M6 gate |
| NFR-7 Local safety | M4-2 | M4 gate |
| NFR-8 Reliability | M1-2, M2-2, M4-6 | M2 and M4 gates |
| NFR-9 Fault under 1 ms | M6-5 | M6 gate |
