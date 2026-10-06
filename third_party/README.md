# Third-party code

Header-only libraries, vendored unmodified. Each directory holds the headers under `include/`,
the upstream licence file, and a `README.md` with the exact version, source and checksums.
All four are MIT-licensed. CRC-32C is our own code and lives in `src/platform/`.

| Directory | Library | Version | CMake target |
| --- | --- | --- | --- |
| `nlohmann_json/` | nlohmann/json | 3.12.0 | `paramesh::json` |
| `cpp-httplib/` | cpp-httplib | 0.59.0 | `paramesh::httplib` |
| `picosha2/` | PicoSHA2 | 1.0.1 | `paramesh::picosha2` |
| `doctest/` | doctest | 2.5.3 | `paramesh::doctest` |

The include directories are marked `SYSTEM`, so our warning flags and clang-tidy do not apply
to these headers. To update one: replace the files from the new upstream tag, update its
`README.md` (version, date, checksums), check the licence is still permissive, and log it.
Code outside `src/platform/` and `tests/` never includes these headers; it goes through the
interfaces in `src/platform/`.
