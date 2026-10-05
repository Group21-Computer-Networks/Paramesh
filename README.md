# ParaMesh

A distributed shared-memory runtime for a small pool of Linux laptops: an ordinary C or C++
program calls `paramesh.h`, and its shared region is kept coherent across the nodes by the page
fault handler, with no socket code in the application.

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
