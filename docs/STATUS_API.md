# ParaMesh status API

Each `pmd` serves its own view of the pool over HTTP on `127.0.0.1`, port `--http-port`
(default 47002; 0 lets the system choose, and the port is in the `pmd_start` log line). Only
`GET`. Every answer is JSON except `/v1/events`, which is a stream of server-sent events.
What a daemon shows is its own records; what peers announce is shown as theirs and used for
nothing else.

The JSON is remade once a second, so a change in the pool appears within a second of the
daemon learning of it.

## `GET /v1/peers` (M4-1)

This node and the peers it hears.

```json
{"node": 1, "cap_cores": 4,
 "peers": [{"node": 2, "address": "10.0.0.2", "control_port": 47001, "state": "alive",
            "leaving": false, "cap_cores": 4, "free_cores": 4, "cap_ram": 0, "free_ram": 0,
            "jobs": 1, "silent_ms": 310}],
 "clashes": [{"node": 3, "address": "10.0.0.9"}]}
```

`state` is `alive` or `suspect` (three beacons missed; a peer is dropped after five).
`clashes` lists another daemon using this node's ID.

## `GET /v1/ledger` (M5-4)

For each peer in this node's ledger or that has announced totals: this node's raw counts with
it, its balance and weight, the peer's announcement and whether they differ by more than 10%
(and by more than 1, which is rounding); the same for each job.

```json
{"node": 1,
 "peers": [{"peer": 2,
            "totals": {"chunks_for_peer": 12, "chunks_by_peer": 30,
                       "segsec_for_peer": 40, "segsec_by_peer": 95},
            "balance": 18.4, "weight": 1.005, "pending_indexes": 0,
            "announced": {"chunks_for_peer": 30, "chunks_by_peer": 12,
                          "segsec_for_peer": 95, "segsec_by_peer": 40},
            "disagree": false,
            "jobs": [{"job": 65537,
                      "counts": {"chunks_for_peer": 0, "chunks_by_peer": 30,
                                 "segsec_for_peer": 0, "segsec_by_peer": 95},
                      "credit_given": 18.4, "credit_taken": 0,
                      "announced": null, "disagree": false}]}]}
```

`*_for_peer`: this node's work for the peer's jobs; `*_by_peer`: the peer's work for this
node's jobs. `segsec`: segments hosted x seconds. Credits are in core-seconds (M5-2).
`announced` is `null` until the peer has said something.

## `GET /v1/jobs` (M6-1)

The job processes on this node, launchers and workers, from their last two `L_STATS`
(docs/PROTOCOL.md, section 9).

```json
{"node": 2,
 "jobs": [{"job": 65537, "role": "worker", "launcher": 1, "slots": 2,
           "faults": 1830, "pages_in": 1790, "pages_out": 412, "retries": 3,
           "faults_per_s": 210.0, "mb_in_per_s": 0.84, "mb_out_per_s": 0.2,
           "fault_latency": {"buckets": [{"below_us": 1, "count": 0}, ...,
                                         {"below_us": 16384, "count": 2},
                                         {"below_us": -1, "count": 0}],
                             "p50_us": 256, "p99_us": 2048}}]}
```

| Field | Meaning |
| --- | --- |
| `role` | `launcher` or `worker`. |
| `launcher` | The job's launcher node. |
| `slots` | Worker threads this node gives the job now (M5-3). |
| `faults`, `pages_in`, `pages_out`, `retries` | Counts since the process started. |
| `faults_per_s`, `mb_in_per_s`, `mb_out_per_s` | Over the last second (a page is 4,096 bytes). |
| `fault_latency.buckets` | 16 counts of faults by the time from the fault to the page being installed or woken: below 1, 2, 4, ... 16,384 µs, and the last (`below_us` -1) for longer. |
| `p50_us`, `p99_us` | The upper bound of the bucket holding the 50th and 99th percentile. |

## `GET /v1/jobs/{id}/pages` (M6-1)

For one job on this node, each segment it is home for or holds a page of. 404 for a job not on
this node.

```json
{"job": 65537, "node": 2,
 "segments": [{"segment": 0, "home": 2, "home_here": true, "shared": 0, "modified": 512},
              {"segment": 3, "home": 1, "home_here": false, "shared": 40, "modified": 0}]}
```

`shared` and `modified`: pages of the segment this node holds read-only (S) or writable (M).

## `GET /v1/events` (M6-1)

`text/event-stream`: from the moment of the request, one event a line pair, until the client
goes. It starts with `retry: 1000`.

| Event | Data | When |
| --- | --- | --- |
| `stats` | `{"node", "time_ms", "jobs": [{"job", "faults_per_s", "mb_in_per_s", "mb_out_per_s", "slots"}]}` | Every second. `time_ms` is wall-clock time. |
| `join`, `leave` | `{"peer": N}` | A peer has appeared in, or gone from, this node's peer table. |
| `job_start`, `job_end` | `{"job": N}` | A job process has started or ended on this node. |

```
event: job_start
data: {"job":65537}

event: stats
data: {"jobs":[{"faults_per_s":210.0,"job":65537,"mb_in_per_s":0.84,"mb_out_per_s":0.2,"slots":2}],"node":2,"time_ms":1792245576226}
```

Joins, leaves, starts and ends are found by comparing with the second before, so they appear
within a second. A stream that falls more than 600 events behind skips to the oldest kept.
