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

## Build

Linux 5.19 or later, CMake 3.25+, Ninja, GCC or Clang with C++20.

```bash
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```

The vendored headers are committed, so a clean clone builds with nothing to download.
