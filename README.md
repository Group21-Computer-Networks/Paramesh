# ParaMesh

A distributed shared-memory runtime for a small pool of Linux laptops: an ordinary C or C++
program calls `paramesh.h`, and its shared region is kept coherent across the nodes by the page
fault handler, with no socket code in the application.

**Using it:** [`docs/API.md`](docs/API.md), the user guide and API reference: install, join
the pool from the dashboard, write and run a program, every function of `paramesh.h`. To give
others an installer, run `tools/setup/package.sh` and share the one file it writes.

**Seeing it:** [`docs/PLAYGROUND.md`](docs/PLAYGROUND.md): the website and the C playground, what
a run does, and what its sandbox does not stop.

**How it is built** (for contributors):

- Plan and task cards: [`docs/PLAN.md`](docs/PLAN.md)
- Guide for contributors and coding agents: [`AGENTS.md`](AGENTS.md)
- What has been built and how it was checked: [`docs/logs/`](docs/logs/)
- Deferred work: [`docs/TODO.md`](docs/TODO.md)
- Vendored libraries, versions and licences: [`third_party/`](third_party/)

## What it is for

A few laptops on one network become one computer for a C program. You write ordinary C, and
loops that are safe to split run across the laptops, while the rest of the program stays on the
laptop that started it. It suits work made of large independent pieces, such as matrix work or
number crunching. Small loops run slower, because a network hop costs far more than memory.

## How a run works

```mermaid
flowchart LR
    A[Write C in the playground] --> C[Convert: safe loops become tasks]
    C --> D[Wait your turn in line]
    D --> E[Compile in the sandbox]
    E --> F[Copy the program to the other laptops]
    F --> G[pmrun: pmd admits the job]
    G --> H[Laptops pull chunks and fetch pages on fault]
    H --> I[Output and Analytics]
```

## Run it on two laptops

Both laptops need Linux 6.7 or later for the playground, and must reach each other on the network.

1. On every laptop, install or update with one line:

   ```bash
   curl -fsSL https://github.com/Group21-Computer-Networks/Paramesh/releases/latest/download/install.sh | bash
   ```

   It asks for your password, and checks the download's SHA-256.

2. Every laptop must run the same release, because a job checks that its program is identical
   everywhere. After a new release, run the line above on all of them.

3. For the playground, install a compiler on each laptop:

   ```bash
   sudo apt install -y g++
   ```

4. On the first laptop, open `http://127.0.0.1:47080/dashboard` and join. It shows an invite code
   and the playground's access code.
5. On each other laptop, open the same address, enter the invite code and join.
6. Check that every node is `alive`, then run a job:

   ```bash
   pm status
   ```

   ```bash
   cd /opt/paramesh/apps && pmrun -n 2 /opt/paramesh/apps/matmul 512
   ```

7. Open `http://127.0.0.1:47080/playground`, or `http://ADDRESS:47080/` from another machine.
   From another machine, enter the access code. Set Nodes to 2 and run an example.

If nodes cannot see each other, allow the pool through the firewall, for example
`sudo ufw allow from 10.0.0.0/8`. To remove everything:

```bash
bash ~/.local/share/paramesh/paramesh-setup.sh --uninstall
```

To package your own build instead, run `tools/setup/package.sh` once and give that one file to every
laptop.

## Features

| Area | What you get |
| --- | --- |
| Shared memory | One address space across nodes, kept coherent by page faults through userfaultfd. |
| Tasks | `PM_TASK`, `pm_parallel_for`, `pm_wait_all`, locks, barriers and atomic add. |
| Plain C | Convert shows your program split into tasks, and where each loop runs and why. |
| Pool | Discovery, invite codes across subnets, caps on cores and RAM, and spill to disk. |
| Changing pool | A planned `pm leave`, joining a running job, and optional re-homing of segments. |
| Fairness | A ledger per node weights free slots, and what a peer claims never changes a weight. |
| Sharing | An access code, one line per laptop, turns between machines, and private runs. |
| Sandbox | Compiles and jobs run under Landlock, a system-call filter and limits. |
| Seeing it | A website, a playground, a live dashboard, Analytics, `pm status` and `pm top`. |

## Known limits

- **Trusted network only.** The access code and the pool token cross the LAN in plain HTTP, and a
  captured start request can be replayed. Never put this on the internet.
- **Turns are per machine,** so anyone with several machines can hold more places.
- **Fairness between laptops is by node,** not by person, and each laptop has its own line.
- **A laptop that crashes ends every job using it.** Only a planned leave is survived.
- **The translator reads C with patterns,** not a parser. It handles counted `for` loops in `main()`,
  and leaves anything it cannot show safe on one laptop. It has not been run on two real laptops.
- **A split program gets 512 MB** of shared memory by default (`--region-mb`).
- **`double` totals** can differ in their last digits between runs.
- **Speedup on two real laptops has not been measured:** that gate check was waived. See `docs/logs/M3-gate.md`.
- Rare unexplained failures are recorded in [`docs/TODO.md`](docs/TODO.md).

## Build

To build from source: Linux 5.19 or later, CMake 3.25+, Ninja, GCC or Clang with C++20.

```bash
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```

The vendored headers are committed, so a clean clone builds with nothing to download.
