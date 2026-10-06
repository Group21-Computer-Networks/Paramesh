# ParaMesh wire protocol

**Status: DRAFT from task M0-5, awaiting approval at the M0 gate. Not frozen yet.**

This document defines every byte ParaMesh processes send each other: the frame header, every
opcode and its payload, the discovery beacon, the local socket between a job process and its
`pmd`, the timers, and the error codes. Code in `src/wire/` is written against it, and the
Wireshark dissector (M6-4) needs nothing else.

A paragraph or table tagged **[GATE Pn]** is a proposal for the person to approve or change at
the M0 gate. The same numbers, with the alternatives, are listed in `docs/logs/M0-5.md`. Text
without a tag restates what `docs/PLAN.md` or the HLD already fixes.

## 1. Conventions

- **Byte order.** Every multi-byte integer, on every channel including the local socket, is in
  network byte order (big-endian).
- **Alignment.** There is none. Fields are laid end to end at the offsets given and no padding
  is ever inserted. A payload starts at offset 36 of its frame, so its 8-byte fields are not
  8-byte aligned in memory. Encoders and decoders build and read each value byte by byte (or
  with `memcpy`); they never cast a buffer to a struct.
- **Types.** `u8`, `u16`, `u32`, `u64` are unsigned integers of 1, 2, 4 and 8 bytes.
  `bytes[n]` is `n` raw bytes. `str16` is a `u16` length followed by that many bytes of UTF-8,
  with no terminator.
- **Reserved fields** are sent as zero and ignored on receipt.
- **Sizes.** "Fixed size" under a message is the exact payload length; a receiver rejects any
  other length. A variable message states its minimum and maximum.
- **Numbers that the design fixes:** page 4,096 bytes; segment 2 MiB = 512 pages; region at
  most 4 GiB = 2,048 segments = 1,048,576 pages at base address `0x600000000000`; at most 8
  nodes in a job.
- **Page ID** = (address - `0x600000000000`) / 4096, so it is below 1,048,576.
  **Segment ID** = page ID / 512.

## 2. Channels

| Channel | Transport | Port | Carries |
| --- | --- | --- | --- |
| Discovery | UDP multicast 239.77.77.77, TTL 1, IPv4; `--peers` seed list where multicast is blocked | 47000 | `HELLO` every 1 s, `BYE` |
| Daemon control | TCP, `pmd` to `pmd` | 47001 | Spawn requests and replies, ledger sync, leave intent |
| Job data plane | TCP full mesh between the processes of one job | Ephemeral, announced in `JOIN_JOB` | Pages, coherence, tasks, locks, migration |
| Local link | Unix stream socket between a job process (or a tool) and the `pmd` on the same machine | A socket file, section 9 | Admission, quota, completion reports, leave |

All four use the frame of section 3. On UDP one datagram holds exactly one frame.

**Data-plane connections.** There is one TCP connection per pair of job processes: the process
with the lower node ID connects and the higher accepts. Every data-plane socket has
`TCP_NODELAY` set, and a frame's header and payload are sent with one `writev()`. The first
frame each side sends on a new connection is `JOIN_JOB`.

## 3. Frame header

Every frame starts with this 36-byte header, followed by `payload_len` bytes of payload.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 4 | `magic` | `0x504D5348` ("PMSH"). Detects a stream that has lost framing. |
| 4 | 1 | `version` | Protocol version, 1. |
| 5 | 1 | `opcode` | Message type, section 4. |
| 6 | 2 | `flags` | Bit set, below. |
| 8 | 4 | `job_id` | The job this frame belongs to. 0 on discovery and on daemon-control and local frames that concern no job. |
| 12 | 2 | `src_node` | Node ID of the sender. |
| 14 | 2 | `dst_node` | Node ID of the intended receiver. `0xFFFF` on multicast frames. 0 on local-link frames. |
| 16 | 8 | `req_id` | Matches a reply to its request. A request carries a number its sender has not used before on that connection, starting at 1. A reply repeats the request's number. 0 on a frame that is neither. |
| 24 | 4 | `epoch` | The segment-map version the sender is using. 0 off the data plane and before the first `SEG_MAP`. |
| 28 | 4 | `payload_len` | Number of payload bytes that follow. |
| 32 | 4 | `payload_crc` | CRC-32C (Castagnoli) of the payload. 0 when `payload_len` is 0. |

Node ID 0 means "no node" and `0xFFFF` means "every node"; neither is ever a node's own ID.

**Flags.**

| Bit | Name | Meaning |
| --- | --- | --- |
| `0x0001` | `ZERO_PAGE` | The page has never been written. The frame carries the page ID and no page data; the receiver uses 4,096 zero bytes. |
| `0x0002` | `COMPRESSED` | Reserved for compressed page data. Never set this semester; a receiver that sees it treats the frame as malformed. |
| `0x0004` | `READ_ONLY` | The page data in this frame is a read copy: the receiver installs it write-protected, and the sender of a `FETCH_DATA` kept its own copy. |
| `0x0008` | `RETRY` | This request was sent before, with the same `req_id`, and no reply came within the reply timeout. |

All other bits are reserved.

**Checks on receipt, in this order:** `magic`; `version`; `payload_len` at most 1,048,576 and
legal for the opcode; the payload CRC; `opcode` known and allowed on this channel; `job_id`
and `dst_node` are the receiver's. What follows a failed check:

| Channel | A frame fails a check |
| --- | --- |
| Discovery | The datagram is dropped and counted. |
| Daemon control, local link | The connection is closed and the event logged. |
| Job data plane | The job is aborted with status `PROTOCOL`, naming the sending node. |

**[GATE P14]** The 1 MiB payload limit is a proposal; no earlier document gives one. The
largest ordinary frame is a page (4,104 bytes of payload); only `SEG_MIGRATE` comes near the
limit, and it is sent in batches for that reason.

## 4. Opcodes

The High-Level Design lists 39 opcodes and all of them are here. `SPAWN_DECLINE`,
`SEG_MAP_ACK` and the local-link messages are additions.

| Value | Name | Channel | From → to | Reply |
| --- | --- | --- | --- | --- |
| `0x01` | `HELLO` | Discovery | `pmd` → all | none |
| `0x02` | `BYE` | Discovery | `pmd` → all | none |
| `0x10` | `SPAWN_REQ` | Control | launcher's `pmd` → peer `pmd` | `SPAWN_OK` or `SPAWN_DECLINE` |
| `0x11` | `SPAWN_OK` | Control | peer `pmd` → launcher's `pmd` | is a reply |
| `0x12` | `LEDGER_SYNC` | Control | `pmd` → `pmd` | none |
| `0x13` | `LEAVE_INTENT` | Control | leaver's `pmd` → launcher's `pmd` | none |
| `0x14` | `SPAWN_DECLINE` | Control | peer `pmd` → launcher's `pmd` | is a reply |
| `0x20` | `JOIN_JOB` | Data | each end of a new connection → the other | none |
| `0x21` | `SEG_MAP` | Data | launcher → every process | `SEG_MAP_ACK` |
| `0x22` | `HEARTBEAT` | Data | every process → every other | none |
| `0x23` | `JOB_END` | Data | launcher → every process; worker → launcher | none |
| `0x24` | `SEG_MAP_ACK` | Data | process → launcher | is a reply |
| `0x30` | `READ_REQ` | Data | requester → home | `READ_DATA`, `REDIRECT` or `BUSY_RETRY` |
| `0x31` | `WRITE_REQ` | Data | requester → home | `WRITE_GRANT`, `REDIRECT` or `BUSY_RETRY` |
| `0x32` | `UPGRADE_REQ` | Data | requester → home | `UPGRADE_GRANT`, `WRITE_GRANT`, `REDIRECT` or `BUSY_RETRY` |
| `0x38` | `READ_DATA` | Data | home → requester | is a reply |
| `0x39` | `WRITE_GRANT` | Data | home → requester | is a reply |
| `0x3A` | `UPGRADE_GRANT` | Data | home → requester | is a reply |
| `0x3B` | `REDIRECT` | Data | node → requester | is a reply |
| `0x3C` | `BUSY_RETRY` | Data | node → requester | is a reply |
| `0x40` | `INV` | Data | home → holder | `INV_ACK` |
| `0x41` | `INV_ACK` | Data | holder → home | is a reply |
| `0x42` | `FETCH` | Data | home → owner | `FETCH_DATA` |
| `0x43` | `FETCH_INV` | Data | home → owner | `FETCH_DATA` |
| `0x44` | `FETCH_DATA` | Data | owner → home | is a reply |
| `0x45` | `WRITEBACK` | Data | owner → home | `WRITEBACK_ACK`, `REDIRECT` or `BUSY_RETRY` |
| `0x46` | `WRITEBACK_ACK` | Data | home → owner | is a reply |
| `0x50` | `TASK_REQ` | Data | worker thread → launcher | `TASK_ASSIGN` or `NO_TASK` |
| `0x51` | `TASK_ASSIGN` | Data | launcher → worker | is a reply |
| `0x52` | `NO_TASK` | Data | launcher → worker | is a reply |
| `0x53` | `TASK_DONE` | Data | worker → launcher | none |
| `0x60` | `LOCK_ACQ` | Data | any process → launcher | `LOCK_GRANT` |
| `0x61` | `LOCK_GRANT` | Data | launcher → process | is a reply |
| `0x62` | `LOCK_REL` | Data | process → launcher | none |
| `0x63` | `BARRIER_ENTER` | Data | any process → launcher | `BARRIER_RELEASE` |
| `0x64` | `BARRIER_RELEASE` | Data | launcher → process | is a reply |
| `0x65` | `ATOMIC_OP` | Data | any process → home | `ATOMIC_RESULT`, `REDIRECT` or `BUSY_RETRY` |
| `0x66` | `ATOMIC_RESULT` | Data | home → process | is a reply |
| `0x70` | `SEG_MIGRATE` | Data | leaver → new home | `SEG_MIGRATE_DONE` after the last batch |
| `0x71` | `SEG_MIGRATE_DONE` | Data | new home → leaver | is a reply |
| `0x72` | `LEAVE_DONE` | Data | leaver → launcher | `JOB_END` with status `LEFT` |
| `0x80`–`0x86` | `L_REGISTER` … `L_JOB_END` | Local | job process → `pmd`, and its replies | section 9 |
| `0x90`–`0x94` | `L_QUOTA` … `L_MEMBER` | Local | `pmd` → job process | section 9 |
| `0xA0`–`0xA4` | `L_RUN_REQ` … `L_LEAVE_REPLY` | Local | tool ↔ `pmd` | section 9 |

A process that is the launcher is also a node of its job: where the table says "→ launcher"
and the sender is the launcher itself, the message is handled in-process and never sent.

**[GATE P1]** Still open #2. The numeric values, and every payload in sections 5 to 9, are
proposals: the HLD lists the opcodes without values or payloads, apart from the page payload.
Two opcodes are new, and the reasons are given where they are defined: `SPAWN_DECLINE`
(section 6) and `SEG_MAP_ACK` (section 7.1).

## 5. Discovery

### `HELLO` — 0x01

Sent by every `pmd` every 1 s. A peer is marked suspect after 3 missed beacons and removed
after 5. The sender's IPv4 address is the datagram's source address.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 4 | `instance` | A random number chosen each time `pmd` starts. Two beacons with one node ID but different `instance` and different source addresses are an ID clash, which is reported. |
| 4 | 2 | `control_port` | TCP port of this `pmd`'s control channel, normally 47001. |
| 6 | 2 | `flags` | `0x0001` `LEAVING`: this node is leaving and accepts no new job. |
| 8 | 8 | `cap_ram` | `--cap-ram`, in bytes. |
| 16 | 8 | `free_ram` | Bytes of the RAM cap not committed to a job. |
| 24 | 8 | `spill_budget` | Spill budget, in bytes. |
| 32 | 8 | `free_spill` | Bytes of the spill budget not committed to a job. |
| 40 | 2 | `cap_cores` | `--cap-cores`. |
| 42 | 2 | `free_cores` | Worker slots not in use. |
| 44 | 2 | `jobs` | Jobs this node takes part in now. |
| 46 | 2 | `total_count` | Number of ledger-total entries that follow, 0 to 7. |
| 48 | var | `totals` | `total_count` entries of 36 bytes, below. |

Fixed part: 48 bytes. Size: 48 + 36 × `total_count`, at most 300 bytes.

One ledger-total entry, the sender's own raw counts with one peer, over all jobs since its
ledger began:

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 2 | `peer` | Node ID of the peer the entry is about. |
| 2 | 2 | reserved | |
| 4 | 8 | `chunks_for_peer` | Chunks the sender completed for the peer's jobs. |
| 12 | 8 | `chunks_by_peer` | Chunks the peer completed for the sender's jobs. |
| 20 | 8 | `segsec_for_peer` | Segment-seconds the sender hosted for the peer's jobs. |
| 28 | 8 | `segsec_by_peer` | Segment-seconds the peer hosted for the sender's jobs. |

The totals are shown in `pm status` and compared for the disagreement alarm. They are never
an input to a weight, a quota or an admission decision.

**[GATE P2]** Still open #3. This is the proposed content now that the speed score is gone:
identity, caps and free capacity (the plan chooses peers "with the most free capacity"), and
raw counts only, as chunks and segment-seconds, never credits.

**[GATE P7]** Still open #9. Proposal: `pmd` sends and listens on every IPv4 interface that is
up, is not loopback and supports multicast, and records each peer with the source address it
was first heard from. `pmd --iface <name>` restricts it to one interface.

### `BYE` — 0x02

Sent once, by a `pmd` that is leaving the pool, after its last job has let it go.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 4 | `instance` | As in `HELLO`. |

Fixed size: 4 bytes.

## 6. Daemon control

A `pmd` opens a TCP connection to another `pmd`'s control port when it first needs one and
keeps it. `job_id` in the header names the job a frame is about.

### `SPAWN_REQ` — 0x10

Asks a peer to start one worker process for a job.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 2 | `launcher_node` | Node ID of the launcher. |
| 2 | 2 | `flags` | `0x0001` `MID_RUN`: the job is already running and this node would join it (M4-7). |
| 4 | 4 | `launcher_addr` | IPv4 address of the launcher's data-plane listener. |
| 8 | 2 | `launcher_port` | Its TCP port. |
| 10 | 2 | `threads_per_node` | The job's own upper bound on worker threads per node; 0 for none. |
| 12 | 8 | `region_bytes` | Size of the job's shared region. |
| 20 | 32 | `binary_hash` | SHA-256 of the launcher's executable. |
| 52 | var | `path` | `str16`: absolute path of the executable, the same on every node. |
| var | var | `cwd` | `str16`: the launcher's working directory. |
| var | 2 | `argc` | Number of arguments that follow, not counting the program name. |
| var | var | `argv` | `argc` values of `str16`. |

Fixed part: 52 bytes. Size: 52 bytes plus the variable part, at most 65,536 bytes.

The peer hashes the file at `path`. If the hash differs it replies `SPAWN_DECLINE` with status
`BINARY_MISMATCH`. If it accepts, it starts the process with the environment of section 10,
waits for the process's `L_REGISTER`, and replies `SPAWN_OK`.

### `SPAWN_OK` — 0x11

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 2 | `node` | Node ID of the peer. |
| 2 | 2 | `data_port` | TCP port the new worker process listens on. |
| 4 | 2 | `cores` | The largest thread quota this node will give the job. |
| 6 | 2 | reserved | |
| 8 | 8 | `ram_commit` | Bytes of RAM this node commits to the job. |
| 16 | 8 | `spill_commit` | Bytes of spill this node commits to the job. |

Fixed size: 24 bytes.

### `SPAWN_DECLINE` — 0x14

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 2 | `status` | Why, from section 12: `NO_CAPACITY`, `BINARY_MISMATCH`, `NOT_FOUND`, `LEAVING` or `INTERNAL`. |
| 2 | var | `message` | `str16`: text for the launcher's log. |

Fixed part: 2 bytes. Size: 4 to 516 bytes.

**[GATE P1]** New opcode. The plan has each asked peer "accept or decline", and the HLD has
only `SPAWN_OK`.

**[GATE P8]** Still open #10. Proposal: when fewer than N nodes accept, the job runs on those
that did, provided their commitments still pass the capacity check, and `pmrun` prints how
many it got. The plan's automatic refill (M4-7) then offers the job to peers seen later.
Otherwise the job is refused.

**[GATE P16]** The capacity check sums `ram_commit` and `spill_commit` from other nodes, and
node choice ranks peers by the free capacity in their `HELLO`. Both are values received from
another node, and the Trust rule forbids such a value in an admission decision. The draft
reads the rule as covering the ledger and weights, and treats a peer's commitment as that
peer's own decision about its own resources. This needs the person's ruling.

### `LEDGER_SYNC` — 0x12

Sent to each peer of a job when the job ends on the sender. For display and the disagreement
alarm only.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 2 | `count` | Number of entries that follow, at least 1. |
| 2 | 2 | reserved | |
| 4 | var | `entries` | `count` entries of 40 bytes, below. |

Fixed part: 4 bytes. Size: 4 + 40 × `count`, at most 65,536 bytes.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 4 | `job_id` | The job the counts are for. |
| 4 | 4 | reserved | |
| 8 | 8 | `chunks_for_peer` | As in `HELLO`, for this job only. |
| 16 | 8 | `chunks_by_peer` | |
| 24 | 8 | `segsec_for_peer` | |
| 32 | 8 | `segsec_by_peer` | |

### `LEAVE_INTENT` — 0x13

Sent by a leaving node's `pmd` to the `pmd` of the launcher of every job it serves. That `pmd`
passes it to the launcher process as `L_LEAVE_INTENT`.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 2 | `node` | Node ID of the leaver. |
| 2 | 1 | `reason` | 1 = `pm leave`, 2 = Ctrl+C on `pmd`. |
| 3 | 1 | reserved | |

Fixed size: 4 bytes.

## 7. Job data plane

### 7.1 Membership

### `JOIN_JOB` — 0x20

The first frame each side sends on a new data-plane connection.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 1 | `role` | 1 = launcher, 2 = worker. |
| 1 | 1 | reserved | |
| 2 | 2 | `listen_port` | TCP port this process accepts data-plane connections on. |
| 4 | 4 | `pid` | Process ID, for logs. |
| 8 | 32 | `binary_hash` | SHA-256 of this process's executable. |

Fixed size: 40 bytes.

A process that receives a `JOIN_JOB` whose hash differs from its own reports it: the launcher
ends the job with status `BINARY_MISMATCH`.

### `SEG_MAP` — 0x21

The member list and the home of every segment, at one epoch. The launcher sends it to every
process when the job starts (epoch 1) and each time membership changes (the next epoch). The
header's `epoch` field holds the new epoch.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 2 | `segment_count` | Number of segments in the region, 1 to 2,048. |
| 2 | 1 | `member_count` | Number of members, 1 to 8. |
| 3 | 1 | reserved | |
| 4 | var | `members` | `member_count` entries of 20 bytes, below. |
| var | var | `homes` | `segment_count` bytes: for each segment in order, the `slot` of its home. |

Fixed part: 4 bytes. Size: 4 + 20 × `member_count` + `segment_count`, at most 2,212 bytes.

One member entry:

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 2 | `node` | Node ID. |
| 2 | 1 | `slot` | This member's bit number in every copyset of the job, 0 to 63. |
| 3 | 1 | `flags` | `0x01` `LAUNCHER`. `0x02` `NO_HOMES`: hosts no segment (a node that joined mid-run). `0x04` `LEAVING`: named for the last time; it hosts no segment in this map. |
| 4 | 4 | `addr` | IPv4 address of its data-plane listener. |
| 8 | 2 | `port` | Its TCP port. |
| 10 | 2 | reserved | |
| 12 | 8 | `ram_weight` | The RAM in bytes it contributes to the job, its weight in home placement. |

A process that receives a `SEG_MAP` opens a connection to each member it has none with, where
the lower-ID rule makes it the one to connect, and drops the copyset bit of every member no
longer listed. It then replies `SEG_MAP_ACK`.

**[GATE P5]** Still open #7. Proposal: the launcher gives each member a `slot` when it first
appears in a map, counting up from 0 and never reusing one within a job. Bit `slot` of a
copyset stands for that member. Sixty-four slots allow 56 joins after the first 8 members.

**[GATE P13]** Proposal: the map carries the home of every segment explicitly. The launcher
computes it by RAM-weighted rendezvous hashing over the members that host segments; the other
processes use the table as sent. This keeps a map self-describing for the dissector and makes
it impossible for two nodes to disagree about a home.

### `SEG_MAP_ACK` — 0x24

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 4 | `epoch` | The epoch the sender has applied; it now holds a connection to every member of that map. |

Fixed size: 4 bytes.

**[GATE P1]** New opcode. Without it the launcher cannot know when the mesh is complete at
start-up, and a leaving node cannot know when it is safe to close its sockets: a request sent
to it after it has gone would look like a lost node and abort the job.

### `HEARTBEAT` — 0x22

Sent on every data-plane connection every 1 s. No payload.

Fixed size: 0 bytes.

Any frame from a peer counts as a sign of life. A connection silent for 3 s aborts the job.

### `JOB_END` — 0x23

Ends the job on the receiver, which prints the reason and exits.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 2 | `status` | From section 12. `OK` for a normal end. |
| 2 | 2 | `node` | The node the status is about (the lost node, the node whose task failed), or 0. |
| 4 | var | `message` | `str16`: text to print, at most 512 bytes. Empty for `OK` and `LEFT`. |

Fixed part: 4 bytes. Size: 6 to 518 bytes.

The launcher sends it to every process at the end of the job and on any abort. A worker that
meets a fatal condition (a task threw, a second reply timeout, its `pmd` is gone) sends it to
the launcher, which sends it on to everyone. A process that cannot reach the launcher prints
the reason and exits on its own. With status `LEFT` it is the reply to `LEAVE_DONE` and goes
to the leaver alone.

A process that ends on an abort prints `job <job_id> aborted: <message>`, for example
`job 42 aborted: node 7 lost`.

**[GATE P18]** Proposal: a worker reports its own fatal condition with `JOB_END` to the
launcher, so that the HLD's rule "the launcher sends `JOB_END`" still names the one process
that ends the job.

### 7.2 Pages

A **page payload** is the page ID, then the page's 4,096 bytes unless `ZERO_PAGE` is set.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 8 | `page_id` | The page. |
| 8 | 4096 | `data` | The page's bytes. Absent when the header has `ZERO_PAGE`. |

Size: 4,104 bytes, or 8 with `ZERO_PAGE`. A full page frame is 4,140 bytes.

A **page ID payload** is the page ID alone.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 8 | `page_id` | The page. |

Fixed size: 8 bytes.

| Opcode | Payload | Notes |
| --- | --- | --- |
| `READ_REQ` — 0x30 | page ID | The requester has no copy and wants to read. |
| `WRITE_REQ` — 0x31 | page ID | The requester has no copy and wants to write. |
| `UPGRADE_REQ` — 0x32 | page ID | The requester holds a read copy and wants to write. |
| `READ_DATA` — 0x38 | page | `READ_ONLY` is always set. `ZERO_PAGE` for a page never written. |
| `WRITE_GRANT` — 0x39 | page | The requester is now the only holder. `ZERO_PAGE` for a page never written. Also the answer to an `UPGRADE_REQ` from a node whose read copy was invalidated while it waited. |
| `UPGRADE_GRANT` — 0x3A | page ID | The requester's read copy is now the only copy and may be written. |
| `BUSY_RETRY` — 0x3C | page ID | The segment is being migrated, or the receiver has not yet applied the sender's epoch. The requester waits `coh.busy_retry_backoff` and sends the request again as a new request. |
| `INV` — 0x40 | page ID | Discard your read copy. |
| `INV_ACK` — 0x41 | page ID | Discarded. Sent whether or not the node still held the copy. |
| `FETCH` — 0x42 | page ID | Send the page and keep a read copy. |
| `FETCH_INV` — 0x43 | page ID | Send the page and give it up. |
| `FETCH_DATA` — 0x44 | page | `READ_ONLY` set when answering `FETCH`, clear when answering `FETCH_INV`. Never `ZERO_PAGE`. |
| `WRITEBACK` — 0x45 | page | The owner is giving up a page it holds for writing. Never `ZERO_PAGE`. |
| `WRITEBACK_ACK` — 0x46 | page ID | The home has stored the data. Only now may the owner discard the page. |

### `REDIRECT` — 0x3B

The receiver of a request is not the home of that page under its own, newer epoch.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 8 | `page_id` | The page asked for. |
| 8 | 4 | `epoch` | The sender's epoch. |
| 12 | 2 | `home` | Node ID of the page's home at that epoch. |
| 14 | 2 | reserved | |

Fixed size: 16 bytes.

The requester sends the request again, as a new request, to `home` once it has applied a map
at `epoch` or later.

**Epoch rule.** Every data-plane frame carries its sender's epoch. A node that receives a
page request or `ATOMIC_OP` with an older epoch and is no longer the page's home answers
`REDIRECT`. A node that receives one with a newer epoch than its own answers `BUSY_RETRY`.

### 7.3 Tasks

### `TASK_REQ` — 0x50

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 2 | `thread` | Index of the worker thread that will run the chunk. |
| 2 | 2 | reserved | |

Fixed size: 4 bytes.

### `TASK_ASSIGN` — 0x51

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 8 | `chunk_id` | Identifies the chunk within the job. A chunk run again keeps its ID. |
| 8 | 8 | `task_id` | 64-bit FNV-1a hash of the task's name. |
| 16 | 8 | `lo` | First index of the chunk. |
| 24 | 8 | `hi` | One past its last index. |
| 32 | 4 | `call_id` | Which `pm_parallel_for` call the chunk belongs to, counting from 1. |
| 36 | 2 | `arg_len` | Length of the argument, 0 to 1,024. |
| 38 | 2 | reserved | |
| 40 | var | `arg` | `arg_len` bytes: the task's argument. |

Fixed part: 40 bytes. Size: 40 + `arg_len`, at most 1,064 bytes.

### `NO_TASK` — 0x52

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 1 | `reason` | 1 = the queue is empty for now: ask again after `task.no_task_backoff`. 2 = this node gets no more tasks (it is leaving): stop asking. |
| 1 | 3 | reserved | |

Fixed size: 4 bytes.

### `TASK_DONE` — 0x53

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 8 | `chunk_id` | The chunk that finished. |
| 8 | 8 | `cpu_ns` | CPU time the worker thread spent on it, in nanoseconds, by the worker's own clock. |
| 16 | 2 | `thread` | Index of the worker thread that ran it. |
| 18 | 6 | reserved | |

Fixed size: 24 bytes.

The launcher counts the first `TASK_DONE` for a chunk and ignores later ones.

**[GATE P3]** Still open #4. Proposal: `TASK_DONE` keeps the CPU time. It is logged and shown,
and M5-2 needs it for one thing: until a node has run a chunk of a task itself, the plan
values that task's chunks at "the median over workers". That median is a value reported by
other nodes and it feeds a credit, which the Trust rule forbids; the person should confirm
that the plan's exception is intended, or remove both.

### 7.4 Locks, barriers and atomics

Locks and barriers are kept by the launcher. A lock or barrier ID is the 64-bit `id` of the
handle in `paramesh.h`. A waiter is identified by its node (the header's `src_node`) and
`thread`.

### `LOCK_ACQ` — 0x60

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 8 | `lock_id` | The lock. |
| 8 | 2 | `thread` | Index of the thread asking; `0xFFFF` for the launcher's main thread. |
| 10 | 6 | reserved | |

Fixed size: 16 bytes.

### `LOCK_GRANT` — 0x61

Sent when the lock becomes the requester's, in first-come order, which may be long after the
request.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 8 | `lock_id` | The lock. |
| 8 | 2 | `status` | `OK`, or `BAD_HANDLE` when there is no such lock. |
| 10 | 6 | reserved | |

Fixed size: 16 bytes.

### `LOCK_REL` — 0x62

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 8 | `lock_id` | The lock. |
| 8 | 2 | `thread` | Index of the thread releasing it. |
| 10 | 6 | reserved | |

Fixed size: 16 bytes.

A process checks locally that the thread holds the lock before it sends this, so no reply is
needed.

### `BARRIER_ENTER` — 0x63

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 8 | `barrier_id` | The barrier. |
| 8 | 2 | `thread` | Index of the thread waiting; `0xFFFF` for the launcher's main thread. |
| 10 | 6 | reserved | |

Fixed size: 16 bytes.

### `BARRIER_RELEASE` — 0x64

Sent to every waiter when the barrier's count is reached.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 8 | `barrier_id` | The barrier. |
| 8 | 2 | `status` | `OK`, or `BAD_HANDLE` when there is no such barrier. |
| 10 | 6 | reserved | |

Fixed size: 16 bytes.

### `ATOMIC_OP` — 0x65

Sent to the home of the page that holds the number.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 8 | `offset` | Byte offset of the 64-bit number from the region base; a multiple of 8. |
| 8 | 8 | `operand` | The value to add. |
| 16 | 1 | `op` | 1 = add. |
| 17 | 7 | reserved | |

Fixed size: 24 bytes.

The home takes the page back first if another node holds it, applies the operation to its
copy modulo 2^64, and replies. `REDIRECT` and `BUSY_RETRY` are possible, with the page ID of
`offset`.

### `ATOMIC_RESULT` — 0x66

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 8 | `offset` | As in the request. |
| 8 | 8 | `old` | The number's value before the operation. |

Fixed size: 16 bytes.

### 7.5 Leave

### `SEG_MIGRATE` — 0x70

Carries part of one segment's directory and page data from a leaving home to the segment's
new home. A segment is sent as one or more frames; the last has `LAST` set.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 4 | `segment_id` | The segment. |
| 4 | 2 | `flags` | `0x0001` `LAST`: the final frame for this segment. |
| 6 | 2 | `entry_count` | Number of entries that follow, 0 to 254. |
| 8 | var | `entries` | `entry_count` entries, below. |

Fixed part: 8 bytes. Size: 8 bytes plus the entries, at most 1,048,576 bytes.

One entry is 24 bytes, followed by 4,096 bytes of page data when `where` is 1:

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 2 | `page` | Index of the page within the segment, 0 to 511. |
| 2 | 1 | `state` | 0 = uncached, 1 = shared, 2 = exclusive. |
| 3 | 1 | `where` | 0 = never written, no data. 1 = data follows. 2 = no data: the owner holds the only valid copy (state exclusive). |
| 4 | 2 | `owner` | Node ID of the owner when exclusive, else 0. |
| 6 | 2 | reserved | |
| 8 | 4 | `version` | The page's version. |
| 12 | 4 | reserved | |
| 16 | 8 | `copyset` | Bit `slot` set for each member holding a copy. |

A page with no entry is uncached and never written. Pages the leaving home had spilled to disk
are read back and sent as data.

### `SEG_MIGRATE_DONE` — 0x71

Sent by the new home after the `LAST` frame, when it has stored the segment and serves it.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 4 | `segment_id` | The segment. |

Fixed size: 4 bytes.

### `LEAVE_DONE` — 0x72

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 4 | `segments` | Number of segments the leaver migrated, for the log. |

Fixed size: 4 bytes.

The launcher replies `JOB_END` with status `LEFT` once every remaining process has
acknowledged the map without the leaver. The leaver then closes its connections and exits.
A connection closed by a node that has sent `LEAVE_DONE` is not a lost node.

## 8. Sequences

These fix the order of messages. The transitions inside each node are in `STATE_MACHINES.md`.

### 8.1 Starting a job

1. `pmrun` sends `L_RUN_REQ` to its `pmd` and receives the job ID in `L_RUN_OK`.
2. `pmrun` starts the launcher process as its own child, with the environment of section 10.
3. The launcher's `pm_init()` maps the region, opens its data-plane listener, sends
   `L_REGISTER` and then `L_ADMIT_REQ` with the region size.
4. Its `pmd` checks capacity against the peers it would choose. If the job cannot fit it
   replies `L_ADMIT_REFUSED`, and `pm_init()` returns `PM_ERR_REFUSED`. No worker has started.
5. Otherwise it sends `SPAWN_REQ` to the N-1 peers with the most free capacity. Each starts a
   worker and replies `SPAWN_OK`, or replies `SPAWN_DECLINE` and is replaced by the next peer.
6. The `pmd` replies `L_ADMIT_OK` with the members.
7. The processes connect, lower node ID to higher, and exchange `JOIN_JOB`. A worker knows the
   launcher's address from its environment; the launcher knows each worker's from `L_ADMIT_OK`.
8. With a connection to every member, the launcher sends `SEG_MAP` at epoch 1. Workers connect
   to each other and reply `SEG_MAP_ACK`.
9. When every member has acknowledged, `pm_init()` returns `PM_OK` on the launcher.

Without `pmd` (M1 and M2), steps 1 to 6 are replaced by the environment variables of section
10, and steps 7 to 9 are the same.

**[GATE P10]** This sequence follows the person's answer of 6 October 2026: the launcher
starts first and admission happens inside its `pm_init()`. It differs from the HLD, which
spawns every process when `pmrun` runs, and M4-4's "refused before any process starts" becomes
"before any worker starts".

### 8.2 A worker leaves

1. The leaver's `pmd` sends `LEAVE_INTENT` to the launcher's `pmd`, and `L_LEAVE` to its own
   job process.
2. The launcher answers the leaver's `TASK_REQ`s with `NO_TASK`, reason 2. Chunks the leaver
   has not finished after 10 s go back in the queue.
3. The leaver writes back every page it holds for writing (`WRITEBACK`, `WRITEBACK_ACK`) and
   drops its read copies.
4. The launcher sends `SEG_MAP` at the next epoch, in which the leaver hosts no segment.
5. For each segment it was home for, the leaver stops taking new requests, lets requests in
   progress finish, and sends the segment to its new home with `SEG_MIGRATE`. Until the new
   home has the segment, it answers requests for it with `BUSY_RETRY`. A request that still
   reaches the leaver gets `REDIRECT`.
6. When every `SEG_MIGRATE_DONE` has arrived, the leaver sends `LEAVE_DONE`.
7. When every remaining process has sent `SEG_MAP_ACK` for the new epoch, the launcher replies
   `JOB_END` with status `LEFT`. The leaver exits and its `pmd` multicasts `BYE`.

**[GATE P11]** The HLD sends the new `SEG_MAP` after migration. The draft sends it before,
because the leaver and the new homes need it to know where each segment goes, and adds the
final `JOB_END` so the leaver closes only when nobody will address it again.

### 8.3 A node joins a running job

The launcher's `pmd` sends `SPAWN_REQ` with `MID_RUN` to a newly seen peer and passes the new
member to the launcher with `L_MEMBER`. The launcher sends `SEG_MAP` at the next epoch with
the new member flagged `NO_HOMES`; no segment moves. After its `SEG_MAP_ACK` the new node
sends `TASK_REQ`.

### 8.4 Abort

Any of these ends the job: a data-plane connection silent for 3 s; a TCP reset or an
unexpected close on a data-plane connection; no reply to a request after one retry; a
malformed data-plane frame; a task that throws; Ctrl+C on `pmrun` or `pm leave --force` on a
launcher. The launcher sends `JOB_END` with the status and the node to every process it can
still reach. Each prints the message and exits. No page is ever filled with zeros or stale
data to wake a parked thread.

## 9. Local link

A Unix stream socket at `~/.local/state/paramesh/pmd.sock`, created by `pmd`. A job process
finds it through `PARAMESH_PMD_SOCKET`. Frames are those of section 3, with `job_id` set and
`dst_node` 0.

**[GATE P4]** Still open #5. The whole of this section is a proposal: the socket's path, the
messages, and the use of the same socket by `pmrun` and `pm`.

**[GATE P9]** Still open #11. Proposal: a job process whose `pmd` connection closes while the
job runs treats it as fatal and ends the job with status `PMD_LOST`, because nothing would
then enforce its quota or record its work.

### Job process → `pmd`

### `L_REGISTER` — 0x80

The first frame a job process sends.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 4 | `pid` | Process ID. |
| 4 | 1 | `role` | 1 = launcher, 2 = worker. |
| 5 | 1 | reserved | |
| 6 | 2 | `data_port` | TCP port of its data-plane listener. |
| 8 | 32 | `binary_hash` | SHA-256 of its executable. |

Fixed size: 40 bytes.

### `L_ADMIT_REQ` — 0x81

Launcher only. Reply: `L_ADMIT_OK` or `L_ADMIT_REFUSED`.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 8 | `region_bytes` | Size of the region asked for. |
| 8 | 2 | `threads_per_node` | The job's upper bound on worker threads per node; 0 for none. |
| 10 | 6 | reserved | |

Fixed size: 16 bytes.

### `L_ADMIT_OK` — 0x82

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 2 | `quota` | The launcher process's own thread quota. |
| 2 | 1 | `member_count` | Number of members, 1 to 8, the launcher included. |
| 3 | 1 | reserved | |
| 4 | var | `members` | `member_count` member entries of 20 bytes, as in `SEG_MAP`, with `slot` 0. |

Fixed part: 4 bytes. Size: 4 + 20 × `member_count`, at most 164 bytes.

### `L_ADMIT_REFUSED` — 0x83

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 8 | `needed` | Bytes the job needs. |
| 8 | 8 | `available` | Bytes the chosen nodes can give. |
| 16 | 2 | `status` | From section 12, normally `NO_CAPACITY`. |
| 18 | var | `message` | `str16`: text to print. |

Fixed part: 18 bytes. Size: 20 to 532 bytes.

### `L_CHUNK` — 0x84

One finished chunk. A process sends it for each chunk its own threads ran; the launcher also
sends it for each chunk a peer completed for its job.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 2 | `node` | Node ID that ran the chunk. |
| 2 | 2 | reserved | |
| 4 | 8 | `task_id` | The task. |
| 12 | 8 | `indexes` | Number of indexes in the chunk. |
| 20 | 8 | `cpu_ns` | CPU time this process measured, when `node` is this node; else 0. |

Fixed size: 28 bytes.

### `L_SEGMENTS` — 0x85

How many of the job's segments each member is home for, sent on every new epoch, so that
`pmd` can compute segment-seconds from its own node's map.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 4 | `epoch` | The epoch the counts are for. |
| 4 | 1 | `count` | Number of entries that follow, 1 to 8. |
| 5 | 3 | reserved | |
| 8 | var | `entries` | `count` entries of 4 bytes: `u16 node`, `u16 segments`. |

Fixed part: 8 bytes. Size: 8 + 4 × `count`, at most 40 bytes.

### `L_JOB_END` — 0x86

The job has ended on this node. The process exits afterwards.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 2 | `status` | As in `JOB_END`. |
| 2 | 2 | `node` | As in `JOB_END`. |
| 4 | var | `message` | `str16`. |

Fixed part: 4 bytes. Size: 6 to 518 bytes.

### `pmd` → job process

### `L_QUOTA` — 0x90

Sent after `L_REGISTER` and whenever the quota changes. A thread over the quota parks at its
next task boundary.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 2 | `threads` | The most worker threads that may run chunks. |
| 2 | 2 | reserved | |

Fixed size: 4 bytes.

### `L_LEAVE` — 0x91

To a worker: begin a graceful leave.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 1 | `reason` | As in `LEAVE_INTENT`. |
| 1 | 3 | reserved | |

Fixed size: 4 bytes.

### `L_ABORT` — 0x92

To a launcher: end the job now.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 2 | `status` | Normally `USER_ABORT`. |
| 2 | var | `message` | `str16`. |

Fixed part: 2 bytes. Size: 4 to 516 bytes.

### `L_LEAVE_INTENT` — 0x93

To a launcher: a member of its job is leaving.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 2 | `node` | Node ID of the leaver. |
| 2 | 1 | `reason` | As in `LEAVE_INTENT`. |
| 3 | 1 | reserved | |

Fixed size: 4 bytes.

### `L_MEMBER` — 0x94

To a launcher: a node has been spawned for its running job (M4-7).

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 20 | `member` | One member entry, as in `SEG_MAP`, with `slot` 0. |

Fixed size: 20 bytes.

### Tools ↔ `pmd`

### `L_RUN_REQ` — 0xA0

From `pmrun`. Reply: `L_RUN_OK` or `L_RUN_REFUSED`.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 2 | `nodes` | N of `pmrun -n N`, 1 to 8. |
| 2 | 2 | reserved | |
| 4 | 32 | `binary_hash` | SHA-256 of the executable. |
| 36 | var | `path` | `str16`: absolute path of the executable. |
| var | var | `cwd` | `str16`: working directory. |
| var | 2 | `argc` | Number of arguments that follow. |
| var | var | `argv` | `argc` values of `str16`. |

Fixed part: 36 bytes. Size: 36 bytes plus the variable part, at most 65,536 bytes.

### `L_RUN_OK` — 0xA1

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 4 | `job_id` | The new job's ID. |
| 4 | 2 | `node` | This node's ID. |
| 6 | 2 | reserved | |

Fixed size: 8 bytes.

**[GATE P6]** Still open #8. Proposal: the launcher's `pmd` generates the job ID. The high 16
bits are its node ID and the low 16 bits a counter it keeps on disk, skipping 0 and any value
still in use. Two nodes cannot produce the same ID unless their node IDs clash, which
discovery reports. Job ID 0 is never used.

### `L_RUN_REFUSED` — 0xA2

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 2 | `status` | From section 12, for example `LEAVING`. |
| 2 | var | `message` | `str16`. |

Fixed part: 2 bytes. Size: 4 to 516 bytes.

### `L_LEAVE_REQ` — 0xA3

From `pm leave`. Reply: `L_LEAVE_REPLY`.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 1 | `force` | 1 for `pm leave --force`, else 0. |
| 1 | 3 | reserved | |

Fixed size: 4 bytes.

### `L_LEAVE_REPLY` — 0xA4

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 2 | `status` | `OK` when the leave has begun; `LEAVING` when one is already under way; `DECLINED` when this node launched a job that is still running and `force` was 0. |
| 2 | var | `message` | `str16`. |

Fixed part: 2 bytes. Size: 4 to 516 bytes.

## 10. Environment of a job process

Set by `pmrun` for the launcher and by `pmd` for a worker. In M1 and M2, which have no `pmd`,
the test scripts set them.

| Variable | Value |
| --- | --- |
| `PARAMESH_ROLE` | `launcher` or `worker`. |
| `PARAMESH_JOB_ID` | The job ID, decimal. |
| `PARAMESH_NODE_ID` | This node's ID, decimal, 1 to 65534. |
| `PARAMESH_PMD_SOCKET` | Path of the local socket. Unset when there is no `pmd`. |
| `PARAMESH_LAUNCHER` | Worker only: `address:port` of the launcher's data-plane listener. |
| `PARAMESH_LISTEN` | Without `pmd` only: `address:port` to listen on. |
| `PARAMESH_PEERS` | Without `pmd` only: every member as `node@address:port`, comma-separated, the launcher first. All members then have the same RAM weight. |
| `PARAMESH_CFG_<KEY>` | One per tunable: the key in upper case with `.` as `_`, for example `PARAMESH_CFG_TASK_PREFETCH`. |

**[GATE P17]** The variable names are proposals. The plan says role, job ID and peer addresses
arrive as environment variables but does not name them; the HLD names only `PARAMESH_ROLE`.

## 11. Timers

Fixed by the design:

| Timer | Value | On expiry |
| --- | --- | --- |
| Reply to a request | 2 s | Send the request again with `RETRY` set and the same `req_id`, and wait 2 s more. Then abort the job with status `TIMEOUT`. |
| `HEARTBEAT` | every 1 s | |
| Silence on a data-plane connection | 3 s | Abort the job with status `NODE_LOST`. |
| `HELLO` | every 1 s | |
| Missed beacons | 3, then 5 | Peer marked suspect, then removed. |
| Grace for running tasks on leave | 10 s | Unfinished chunks return to the queue. |

From the configuration (the plan's Tunables table): `coh.busy_retry_backoff` after
`BUSY_RETRY`, and `task.no_task_backoff` after `NO_TASK`.

**A retry never runs a request twice.** TCP delivers every frame in order or the connection
fails, so a frame with `RETRY` repeats a request the receiver already has. The receiver drops
it. The retry exists to give a slow reply 2 s more and to show the delay in a capture.

**Requests the reply timer does not cover**, because the wait is part of their meaning:
`LOCK_ACQ` (until the lock is free), `BARRIER_ENTER` (until the count is reached),
`SEG_MAP` and `L_ADMIT_REQ` during start-up, and `LEAVE_DONE`. The heartbeat still detects a
lost node while they wait.

**[GATE P12]** The HLD puts every request under the 2 s timer. The exemptions above, and the
meaning of `RETRY`, are proposals. Start-up needs a bound of its own, which no document gives:
the draft proposes a new configuration key, `job.start_timeout`, default 10 s, after which
the launcher ends the job with status `START_TIMEOUT`.

## 12. Status codes

One 16-bit set, used in `JOB_END` and wherever a payload has a `status` field.

| Value | Name | Meaning |
| --- | --- | --- |
| 0 | `OK` | Success, or a job that ended normally. |
| 1 | `LEFT` | The receiving node's graceful leave is complete. |
| 2 | `NODE_LOST` | A node went silent for 3 s or its connection broke. |
| 3 | `TIMEOUT` | No reply to a request after one retry. |
| 4 | `TASK_FAILED` | A task threw; the message has the task's name and what it threw. |
| 5 | `BINARY_MISMATCH` | A node's executable differs from the launcher's. |
| 6 | `PROTOCOL` | A malformed frame. |
| 7 | `UNSUPPORTED` | A request this build does not implement yet. |
| 8 | `USER_ABORT` | Ctrl+C on `pmrun`, or `pm leave --force` on a launcher. |
| 9 | `INTERNAL` | A fatal condition inside a process; the message says which. |
| 10 | `PMD_LOST` | A job process lost its `pmd`. |
| 11 | `NO_CAPACITY` | Not enough RAM, spill or cores. |
| 12 | `DECLINED` | The request was refused for the reason in the message. |
| 13 | `LEAVING` | The node is leaving the pool. |
| 14 | `BAD_HANDLE` | No such lock or barrier. |
| 15 | `NOT_FOUND` | The executable is not on this node. |
| 16 | `START_TIMEOUT` | The job did not finish starting in time. |

**[GATE P15]** The codes are proposals; the earlier documents name none. Also proposed: a job
process that ends with any status other than `OK` or `LEFT` exits with status 1.
