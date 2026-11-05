# X-package: a container image on GitHub Packages

- Task: X-package
- Milestone: none
- Stream: -
- Status: in-progress
- Branch: x-package
- Started: 2026-11-06
- Finished:

## Card

Approved by Amirishetty Sai Vignesh in chat, 2026-11-06: "make this as package", pointing at the repository's empty Packages panel.

## Sessions

### 2026-11-06 — session 1

- GitHub Packages hosts containers, npm, Maven, NuGet and RubyGems; for a C++ runtime with a daemon and a website, a container image on ghcr.io is the fitting kind.
- `Dockerfile`: builds with `tools/setup/package.sh` on Ubuntu 24.04 (the release's base) and unpacks its payload to `/opt/paramesh`, so the image holds exactly what the installer installs. The runtime image adds python3, gcc and pkg-config for the playground and for building programs, links `pmd`, `pm`, `pmrun` and `paramesh.pc` as the installer does.
- `tools/setup/docker-entrypoint.sh`: with no arguments starts `pmd --standby` and the website, as the installer's two services do; with arguments runs them.
- `.github/workflows/package.yml`: on a `v*` tag or a manual run, builds, tests (`pmd --help`, a program built with `pkg-config`, the services answering on 47002 and 47080, `pm status`, and `pmrun -n 1 quickstart`), then pushes `latest`, `sha-<commit>` and, for a tag, the version.
- README: a "Container image" section.
- No Docker on this machine, so the image is built and tested only in Actions. Next: pull request, then a manual run of `package` on `main` once merged.

### 2026-11-06 — session 2

- First `package` run on the pull request (run 42098556890): build passed, tools and library test passed; the one-node job was refused, "this node has not joined the pool: join on the dashboard", because the image starts `pmd --standby` as the installer does. The test now joins first with `POST /v1/join` (2 cores, 1 GB), as the dashboard's button does.

### 2026-11-06 — session 3

- Second run (42098968121): the join answered 403, because the status API's POSTs need the header `X-ParaMesh: 1` (the dashboard's guard against other pages posting). Added the header to the test.

## Files touched

- `Dockerfile` — the image
- `.dockerignore` — keeps local build output out of the image
- `tools/setup/docker-entrypoint.sh` — the container's start
- `.github/workflows/package.yml` — build, test and push to ghcr.io
- `README.md` — Container image section
- `docs/logs/X-package.md` — this log

## Decisions

- A container image, because GitHub Packages has no format for a native Linux runtime; the image reuses `package.sh` so it cannot drift from the installer.
- Host networking documented as required: multicast discovery and the job mesh's own ports do not pass through Docker's port mapping.

## Questions and blockers

None

## Approvals

None

## Deviations

None

## Verification

Pending: the `package` workflow's test steps, run in Actions after merge.

## Deferred

None
