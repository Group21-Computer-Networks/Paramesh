# ParaMesh state machines

**Status: approved at the M0 gate on 2026-10-07 and frozen: it changes only with the person's approval. Every paragraph tagged [GATE ...] was a proposal and was approved as drafted (`docs/logs/M0-gate.md`).**

This document defines the two state machines that keep shared pages coherent: the one every
node runs for each page it may map (the *node* machine), and the one a page's home runs for
its directory entry (the *home* machine). Code in `src/coh/` implements exactly these tables.
Each machine is a pure function: an event goes in, a new state and a list of actions come
out. It makes no system call and reads no clock.

Coherence is single writer or many readers, with a fixed home per page. At any moment a page
has one node that may write it, or any number that may read it, never both.

Message names are those of `docs/PROTOCOL.md`. A paragraph tagged **[GATE Sn]** is a proposal
for the person to approve or change at the M0 gate; the alternatives are in
`docs/logs/M0-6.md`. Four points were decided by the person on 7 October 2026 and are marked
**[DECIDED]**.

The **Since** column of each table names the first milestone that needs the row: M1, M2, M4
or AT (the anti-thrashing task AT-1).

## 1. The node machine

### 1.1 States

Each node keeps one state per page of the region.

| State | Kind | Mapping on this node | Meaning |
| --- | --- | --- | --- |
| `I` | stable | not mapped | Invalid. Any access faults. |
| `S` | stable | mapped, write-protected | Shared. Reads run at full speed; a write faults. |
| `M` | stable | mapped, writable | Modified. This node is the only holder. |
| `I→S` | in flight | not mapped | `READ_REQ` sent; waiting for `READ_DATA`. |
| `I→M` | in flight | not mapped | `WRITE_REQ` (or an earlier `UPGRADE_REQ`) sent; waiting for `WRITE_GRANT`. |
| `S→M` | in flight | mapped, write-protected | `UPGRADE_REQ` sent; waiting for `UPGRADE_GRANT`. |
| `M→I` | in flight | mapped, write-protected | `WRITEBACK` sent; waiting for `WRITEBACK_ACK`. |

**[DECIDED]** Still open #6. The first six are the HLD's "3 stable, 3 in flight". `M→I` is
added for the wait between `WRITEBACK` and `WRITEBACK_ACK`, so the document has three stable
states and four in flight.

A page in an in-flight state has exactly one request outstanding, its **current request**,
and the node remembers that request's `req_id`. A page in a stable state has none.

### 1.2 Events

| Event | Raised when |
| --- | --- |
| `NEED_R` | A thread faulted reading the page, or `pm_touch` asked for read access. |
| `NEED_W` | A thread faulted writing the page (missing or write-protected), or `pm_touch` asked for write access. |
| `EVICT` | The local cache chose this page to free, or the node is leaving and gives the page up. |
| `READ_DATA`, `WRITE_GRANT`, `UPGRADE_GRANT`, `WRITEBACK_ACK`, `BUSY_RETRY`, `REDIRECT` | That reply arrived for the page's current request. |
| `INV`, `FETCH`, `FETCH_INV` | That request arrived from the page's home. |
| `RESEND` | The back-off after a `BUSY_RETRY` ended, or the segment map a `REDIRECT` named has been applied. |

**Rule 0: replies are matched to the current request.** A reply reaches the table only when
its `req_id` is the page's current request. Any other reply answers a request that has been
superseded; it is dropped and counted, and the table never sees it. A request is superseded
when the page leaves the in-flight state that sent it, or when `RESEND` sends a new one.

### 1.3 Actions

| Action | What the node does |
| --- | --- |
| send *X* | Sends message *X* for this page to the page's home under the node's current segment map. A request gets a new `req_id`, which becomes the current request. |
| reply *X* | Sends *X* back to the home, with the `req_id` of the message being answered. |
| install read-only | Maps the received page write-protected, or a page of zeros if the reply has `ZERO_PAGE`. Wakes every thread parked on the page. |
| install writable | The same, mapped writable. |
| write-protect | Makes the mapped page read-only. A thread that then writes parks. |
| allow writes | Removes write protection. Wakes every thread parked on the page. |
| discard | Unmaps the page. The next access faults as missing. |
| wake | Wakes every thread parked on the page; each retries its access and faults again if it still cannot proceed. A `pm_touch` call waiting on the page checks again in the same way. |
| arm resend | Starts the `coh.busy_retry_backoff` timer after `BUSY_RETRY`, or waits for the segment map named by `REDIRECT`; `RESEND` follows. |

In a list of actions the order is the order they are carried out.

### 1.4 Transitions

"none" means no action and no change of state. "join" means none: the fault's thread stays
parked and is woken by the action that completes the current request. "impossible (Rn)" means
the pair cannot occur, for the numbered reason in section 1.5.

| State | Event | Actions | Next | Since |
| --- | --- | --- | --- | --- |
| `I` | `NEED_R` | send `READ_REQ` | `I→S` | M1 |
| `I` | `NEED_W` | send `WRITE_REQ` | `I→M` | M1 |
| `I` | `EVICT` | none: there is nothing to free | `I` | M4 |
| `I` | `READ_DATA` | impossible (R1) | | |
| `I` | `WRITE_GRANT` | impossible (R1) | | |
| `I` | `UPGRADE_GRANT` | impossible (R1) | | |
| `I` | `WRITEBACK_ACK` | impossible (R1) | | |
| `I` | `BUSY_RETRY` | impossible (R1) | | |
| `I` | `REDIRECT` | impossible (R1) | | |
| `I` | `RESEND` | none: the request it belonged to is gone | `I` | M4 |
| `I` | `INV` | reply `INV_ACK` | `I` | M2 |
| `I` | `FETCH` | impossible (R2) | | |
| `I` | `FETCH_INV` | impossible (R2) | | |
| `S` | `NEED_R` | wake: the page is already readable | `S` | M1 |
| `S` | `NEED_W` | send `UPGRADE_REQ` | `S→M` | M2 |
| `S` | `EVICT` | discard | `I` | M4 |
| `S` | `READ_DATA` | impossible (R1) | | |
| `S` | `WRITE_GRANT` | impossible (R1) | | |
| `S` | `UPGRADE_GRANT` | impossible (R1) | | |
| `S` | `WRITEBACK_ACK` | impossible (R1) | | |
| `S` | `BUSY_RETRY` | impossible (R1) | | |
| `S` | `REDIRECT` | impossible (R1) | | |
| `S` | `RESEND` | none | `S` | M4 |
| `S` | `INV` | discard, reply `INV_ACK` | `I` | M2 |
| `S` | `FETCH` | impossible (R2) | | |
| `S` | `FETCH_INV` | impossible (R2) | | |
| `M` | `NEED_R` | wake: the page is already readable | `M` | M1 |
| `M` | `NEED_W` | wake: the page is already writable | `M` | M1 |
| `M` | `EVICT` | write-protect, send `WRITEBACK` with the page | `M→I` | M4 |
| `M` | `READ_DATA` | impossible (R1) | | |
| `M` | `WRITE_GRANT` | impossible (R1) | | |
| `M` | `UPGRADE_GRANT` | impossible (R1) | | |
| `M` | `WRITEBACK_ACK` | impossible (R1) | | |
| `M` | `BUSY_RETRY` | impossible (R1) | | |
| `M` | `REDIRECT` | impossible (R1) | | |
| `M` | `RESEND` | none | `M` | M4 |
| `M` | `INV` | impossible (R3) | | |
| `M` | `FETCH` | write-protect, reply `FETCH_DATA` with the page and `READ_ONLY` | `S` | M1 |
| `M` | `FETCH_INV` | write-protect, reply `FETCH_DATA` with the page, discard, wake | `I` | M2 |
| `I→S` | `NEED_R` | join | `I→S` | M1 |
| `I→S` | `NEED_W` | join: after the install the thread faults again, as a write to a shared page | `I→S` | M1 |
| `I→S` | `EVICT` | none: the cache picks another page | `I→S` | M4 |
| `I→S` | `READ_DATA` | install read-only | `S` | M1 |
| `I→S` | `WRITE_GRANT` | impossible (R4) | | |
| `I→S` | `UPGRADE_GRANT` | impossible (R4) | | |
| `I→S` | `WRITEBACK_ACK` | impossible (R4) | | |
| `I→S` | `BUSY_RETRY` | arm resend | `I→S` | M4 |
| `I→S` | `REDIRECT` | arm resend | `I→S` | M4 |
| `I→S` | `RESEND` | send `READ_REQ` | `I→S` | M4 |
| `I→S` | `INV` | reply `INV_ACK`: the home still lists a copy this node dropped | `I→S` | M2 |
| `I→S` | `FETCH` | impossible (R2) | | |
| `I→S` | `FETCH_INV` | impossible (R2) | | |
| `I→M` | `NEED_R` | join | `I→M` | M1 |
| `I→M` | `NEED_W` | join | `I→M` | M1 |
| `I→M` | `EVICT` | none: the cache picks another page | `I→M` | M4 |
| `I→M` | `READ_DATA` | impossible (R4) | | |
| `I→M` | `WRITE_GRANT` | install writable | `M` | M1 |
| `I→M` | `UPGRADE_GRANT` | impossible (R5) | | |
| `I→M` | `WRITEBACK_ACK` | impossible (R4) | | |
| `I→M` | `BUSY_RETRY` | arm resend | `I→M` | M4 |
| `I→M` | `REDIRECT` | arm resend | `I→M` | M4 |
| `I→M` | `RESEND` | send `WRITE_REQ` | `I→M` | M4 |
| `I→M` | `INV` | reply `INV_ACK` | `I→M` | M2 |
| `I→M` | `FETCH` | impossible (R2) | | |
| `I→M` | `FETCH_INV` | impossible (R2) | | |
| `S→M` | `NEED_R` | wake: the page is still readable | `S→M` | M2 |
| `S→M` | `NEED_W` | join | `S→M` | M2 |
| `S→M` | `EVICT` | none: the cache picks another page | `S→M` | M4 |
| `S→M` | `READ_DATA` | impossible (R4) | | |
| `S→M` | `WRITE_GRANT` | impossible (R6) | | |
| `S→M` | `UPGRADE_GRANT` | allow writes | `M` | M2 |
| `S→M` | `WRITEBACK_ACK` | impossible (R4) | | |
| `S→M` | `BUSY_RETRY` | arm resend | `S→M` | M4 |
| `S→M` | `REDIRECT` | arm resend | `S→M` | M4 |
| `S→M` | `RESEND` | send `UPGRADE_REQ` | `S→M` | M4 |
| `S→M` | `INV` | discard, reply `INV_ACK`: another writer won; the `UPGRADE_REQ` stays the current request and the home answers it with `WRITE_GRANT` and the page | `I→M` | M2 |
| `S→M` | `FETCH` | impossible (R2) | | |
| `S→M` | `FETCH_INV` | impossible (R2) | | |
| `M→I` | `NEED_R` | wake: the page is still readable | `M→I` | M4 |
| `M→I` | `NEED_W` | join: the thread stays parked until the page is discarded, then faults again | `M→I` | M4 |
| `M→I` | `EVICT` | none: already being freed | `M→I` | M4 |
| `M→I` | `READ_DATA` | impossible (R4) | | |
| `M→I` | `WRITE_GRANT` | impossible (R4) | | |
| `M→I` | `UPGRADE_GRANT` | impossible (R4) | | |
| `M→I` | `WRITEBACK_ACK` | discard, wake | `I` | M4 |
| `M→I` | `BUSY_RETRY` | arm resend | `M→I` | M4 |
| `M→I` | `REDIRECT` | arm resend | `M→I` | M4 |
| `M→I` | `RESEND` | send `WRITEBACK` with the page | `M→I` | M4 |
| `M→I` | `INV` | discard, wake, reply `INV_ACK`; the `WRITEBACK` is superseded | `I` | M4 |
| `M→I` | `FETCH` | reply `FETCH_DATA` with the page and `READ_ONLY`; the `WRITEBACK` stays current | `M→I` | M4 |
| `M→I` | `FETCH_INV` | reply `FETCH_DATA` with the page, discard, wake; the `WRITEBACK` is superseded | `I` | M4 |

**[DECIDED]** `FETCH_INV` follows the HLD's order: write-protect, send, discard, wake, with no
wait. The plan's "wait for acknowledgement" (M2-2) applies to eviction only: a page in `M→I`
is discarded only on `WRITEBACK_ACK`, or when the home has taken the data another way
(`FETCH_INV`, or `INV` after a `FETCH`).

**[GATE S1]** Three choices of form in this section are proposals: one `NEED_W` event for both
kinds of write fault, with the state deciding what it means; a fault that finds the page
already usable is answered with a wake; and Rule 0.

**[GATE S2]** The M1 card for the node machine (M1-3) lists only `I`, `S` and `I→S`. But the
M1 gate has node B read what node A *wrote*, and M1-4 has the home fetch from an owner. So in
M1 a node must also take a write fault, hold a page in `M` and answer `FETCH`. Those rows are
marked M1 here. The person should confirm M1-3 covers them.

### 1.5 Why the impossible pairs cannot occur

Two facts carry every reason. The home handles one request for a page at a time (section 2).
And all frames from one node to another travel on one TCP connection, so they arrive in the
order they were sent.

| Reason | Statement |
| --- | --- |
| R1 | A stable state has no current request, so by Rule 0 no reply reaches the table. |
| R2 | The home sends `FETCH` and `FETCH_INV` only to the node it records as the page's exclusive owner. That node received its `WRITE_GRANT` or `UPGRADE_GRANT` first, so it is in `M`, or in `M→I` if it has since begun to evict. Whatever ended its ownership (`FETCH_INV`, or the home handling its `WRITEBACK`) was sent before the home could address it as owner again. |
| R3 | The home sends `INV` only while the page is shared. A node in `M` was granted the page after every `INV` the home had sent it, and those arrived first. |
| R4 | The home answers a request only with the replies `docs/PROTOCOL.md` lists for it, and this reply is not one for the state's current request. |
| R5 | The home sends `UPGRADE_GRANT` only to a node in the page's copyset. A node reaches `I→M` with an `UPGRADE_REQ` outstanding only through `INV`. The home removes it from the copyset on its `INV_ACK`, and handles the `UPGRADE_REQ` only after the operation that sent the `INV` has finished. |
| R6 | The home answers `UPGRADE_REQ` with `WRITE_GRANT` only when the requester is not in the copyset, which means it sent this node an `INV` earlier. That `INV` arrives first and moves the page to `I→M`. |

**[GATE S8]** If an impossible pair does occur, the implementation has a bug or a peer is
faulty. Proposal: the node aborts the job with status `INTERNAL`, naming the page, the state
and the event. It never guesses a recovery.

### 1.6 A node that is leaving

After a leaving node has written back its last page (`docs/PROTOCOL.md`, section 8.2, step
3), `NEED_R` and `NEED_W` are no longer served: the fault's thread stays parked until the
process exits. Until then the tables apply unchanged; leaving raises `EVICT` on every page in
`S` or `M`, and waits for pages in flight to settle first.

**[GATE S9]** This is a proposal. The HLD says unfinished tasks return to the queue after 10 s
but not what happens to a task still running on the leaver after that.

## 2. The home machine

### 2.1 The directory entry

A home keeps one entry per page of each segment it is home for.

| Field | Values | Meaning |
| --- | --- | --- |
| `state` | `UNCACHED`, `SHARED`, `EXCLUSIVE` | Who holds the page outside the home. |
| `where` | `ZERO`, `RAM`, `SPILL`, `NONE` | Where the home's own copy is: never written, in a RAM slot, in the spill file, or absent because the owner holds the only valid copy. `NONE` exactly when `EXCLUSIVE`. |
| `owner` | a node, or none | The exclusive owner. Set exactly when `EXCLUSIVE`. |
| `copyset` | set of nodes | The nodes that may hold a copy. Empty when `UNCACHED`; the owner alone when `EXCLUSIVE`. A node listed here may have dropped its read copy without saying so. |
| `version` | 32-bit count | Raised by 1 each time write access is granted or an atomic operation is applied. |
| `op` | none, or one operation | The request being handled, who asked, and what it waits for. |
| `waitq` | queue of requests | Requests that arrived while `op` was set, in arrival order. |
| `hold` | a duration and an end time | The anti-thrashing hold window (section 2.6). |

The store moves a home copy between `RAM` and `SPILL` on its own. That changes `where` and
nothing else, and is not an event of this machine.

### 2.2 States

An entry with no `op` is **idle** and its state is the `state` field. An entry with an `op`
is **busy** and is in one of four waiting states.

| State | Kind | Meaning |
| --- | --- | --- |
| `UNCACHED` | idle | No node holds the page. The home copy is the page. |
| `SHARED` | idle | The nodes in `copyset` may hold read copies. The home copy is valid. |
| `EXCLUSIVE` | idle | `owner` may write the page. The home has no valid copy. |
| `W_INV` | busy | Waiting for `INV_ACK` from each node in the operation's pending set. |
| `W_FETCH` | busy | Waiting for `FETCH_DATA` from the owner, which keeps a read copy. |
| `W_FETCH_INV` | busy | Waiting for `FETCH_DATA` from the owner, which gives the page up. |
| `W_LOAD` | busy | Waiting for the store to read the home copy back from the spill file. |

### 2.3 Events and actions

Every event carries the current time, `now`, supplied by the caller.

| Event | Raised when |
| --- | --- |
| `READ_REQ`, `WRITE_REQ`, `UPGRADE_REQ`, `WRITEBACK`, `ATOMIC_OP` | That request arrived from node *n* (which may be the home's own node, delivered in-process). |
| `INV_ACK`, `FETCH_DATA` | That reply arrived from a node. |
| `LOADED` | The store finished reading the page from the spill file. |
| `HOLD_EXPIRED` | The hold window's end time passed. |

| Action | What the home does |
| --- | --- |
| send *X* to *m* | Sends request *X* for the page to node *m*. |
| reply *X* | Sends *X* to the requester, with the request's `req_id`. |
| store | Keeps the received page as the home copy; `where` becomes `RAM`. |
| load | Asks the store to read the home copy from the spill file; `LOADED` follows. |
| queue | Appends the request to `waitq`. |
| arm hold timer | Asks for `HOLD_EXPIRED` at the hold window's end time. |

Three named steps finish an operation. Each needs the home copy's bytes unless stated, so if
`where` is `SPILL` when the step is reached, the entry does **load**, waits in `W_LOAD`, and
runs the step on `LOADED`.

| Step | What it does | Entry afterwards |
| --- | --- | --- |
| **grant read** to *n* | reply `READ_DATA` with `READ_ONLY`, and `ZERO_PAGE` if `where` is `ZERO`, else the home copy. | `SHARED`; *n* added to `copyset` |
| **grant write** to *n* | If the operation is an `UPGRADE_REQ` and *n* is in `copyset`: reply `UPGRADE_GRANT`, which needs no bytes. Otherwise reply `WRITE_GRANT` with `ZERO_PAGE` if `where` is `ZERO`, else the page: the home copy, or the bytes just received in `FETCH_DATA`. | `EXCLUSIVE`; `owner` = *n*; `copyset` = {*n*}; `where` = `NONE`; `version` + 1; the transfer is counted (section 2.6) |
| **apply atomic** for *n* | Turn a `ZERO` page into a page of zeros, add the operand to the 64-bit number at the offset, modulo 2^64, reply `ATOMIC_RESULT` with the old value. | `UNCACHED`; `copyset` empty; `where` = `RAM`; `version` + 1 |

After a step, or after any row that leaves the entry idle, the home takes the **next request**
from `waitq`: the first one, unless a hold window is running, in which case the first that is
not held back by it (section 2.6). If none can be taken the entry stays idle.

### 2.4 Transitions of an idle entry

*n* is the node the request came from. "others" is `copyset` without *n*.

| State | Event | Condition | Actions | Next | Since |
| --- | --- | --- | --- | --- | --- |
| `UNCACHED` | `READ_REQ` | | grant read to *n* | `SHARED` | M1 |
| `UNCACHED` | `WRITE_REQ` | | grant write to *n* | `EXCLUSIVE` | M1 |
| `UNCACHED` | `UPGRADE_REQ` | | as `WRITE_REQ` from *n*: its read copy was invalidated while it waited | `EXCLUSIVE` | M2 |
| `UNCACHED` | `WRITEBACK` | | reply `WRITEBACK_ACK`; the data is dropped (section 2.7) | `UNCACHED` | M4 |
| `UNCACHED` | `ATOMIC_OP` | | apply atomic for *n* | `UNCACHED` | M2 |
| `UNCACHED` | `INV_ACK` | | impossible (H1) | | |
| `UNCACHED` | `FETCH_DATA` | | impossible (H1) | | |
| `UNCACHED` | `LOADED` | | impossible (H1) | | |
| `UNCACHED` | `HOLD_EXPIRED` | | none: a hold applies only to an exclusive page | `UNCACHED` | AT |
| `SHARED` | `READ_REQ` | | grant read to *n* | `SHARED` | M1 |
| `SHARED` | `WRITE_REQ` | others is empty | grant write to *n* | `EXCLUSIVE` | M1 |
| `SHARED` | `WRITE_REQ` | others is not empty | send `INV` to each of others; pending = others | `W_INV` | M2 |
| `SHARED` | `UPGRADE_REQ` | *n* in `copyset`, others is empty | grant write to *n* | `EXCLUSIVE` | M2 |
| `SHARED` | `UPGRADE_REQ` | *n* in `copyset`, others is not empty | send `INV` to each of others; pending = others | `W_INV` | M2 |
| `SHARED` | `UPGRADE_REQ` | *n* not in `copyset` | as `WRITE_REQ` from *n* | as that row | M2 |
| `SHARED` | `WRITEBACK` | | reply `WRITEBACK_ACK`; the data is dropped (section 2.7) | `SHARED` | M4 |
| `SHARED` | `ATOMIC_OP` | | send `INV` to every node in `copyset`, *n* included; pending = `copyset` | `W_INV` | M2 |
| `SHARED` | `INV_ACK` | | impossible (H1) | | |
| `SHARED` | `FETCH_DATA` | | impossible (H1) | | |
| `SHARED` | `LOADED` | | impossible (H1) | | |
| `SHARED` | `HOLD_EXPIRED` | | none | `SHARED` | AT |
| `EXCLUSIVE` | `READ_REQ` | *n* is not `owner` | send `FETCH` to `owner` | `W_FETCH` | M1 |
| `EXCLUSIVE` | `READ_REQ` | *n* is `owner` | impossible (H2) | | |
| `EXCLUSIVE` | `WRITE_REQ` | *n* is not `owner`, no hold running | send `FETCH_INV` to `owner` | `W_FETCH_INV` | M2 |
| `EXCLUSIVE` | `WRITE_REQ` | *n* is not `owner`, hold running | queue; arm hold timer | `EXCLUSIVE` | AT |
| `EXCLUSIVE` | `WRITE_REQ` | *n* is `owner` | impossible (H2) | | |
| `EXCLUSIVE` | `UPGRADE_REQ` | *n* is not `owner` | as `WRITE_REQ` from *n* | as that row | M2 |
| `EXCLUSIVE` | `UPGRADE_REQ` | *n* is `owner` | impossible (H2) | | |
| `EXCLUSIVE` | `WRITEBACK` | *n* is `owner` | store; reply `WRITEBACK_ACK`; `owner` = none; `copyset` = {*n*} | `SHARED` | M4 |
| `EXCLUSIVE` | `WRITEBACK` | *n* is not `owner` | reply `WRITEBACK_ACK`; the data is dropped (section 2.7) | `EXCLUSIVE` | M4 |
| `EXCLUSIVE` | `ATOMIC_OP` | *n* is `owner`, or no hold running | send `FETCH_INV` to `owner` | `W_FETCH_INV` | M2 |
| `EXCLUSIVE` | `ATOMIC_OP` | *n* is not `owner`, hold running | queue; arm hold timer | `EXCLUSIVE` | AT |
| `EXCLUSIVE` | `INV_ACK` | | impossible (H1) | | |
| `EXCLUSIVE` | `FETCH_DATA` | | impossible (H1) | | |
| `EXCLUSIVE` | `LOADED` | | impossible (H1) | | |
| `EXCLUSIVE` | `HOLD_EXPIRED` | | the hold is over; take the next request | `EXCLUSIVE` | AT |

**[DECIDED]** A `WRITEBACK` from the owner leaves the page `SHARED` with that node as its one
reader, not `UNCACHED` as HLD home rule 5 has it. The node keeps a readable copy until the
`WRITEBACK_ACK` reaches it, so the home must count it as a reader; a later writer then
invalidates it like any other.

### 2.5 Transitions of a busy entry

The operation's requester is *n*.

| State | Event | Condition | Actions | Next | Since |
| --- | --- | --- | --- | --- | --- |
| `W_INV` | `READ_REQ` | | queue | `W_INV` | M2 |
| `W_INV` | `WRITE_REQ` | | queue | `W_INV` | M2 |
| `W_INV` | `UPGRADE_REQ` | | queue | `W_INV` | M2 |
| `W_INV` | `WRITEBACK` | | queue | `W_INV` | M4 |
| `W_INV` | `ATOMIC_OP` | | queue | `W_INV` | M2 |
| `W_INV` | `INV_ACK` | from a node in pending, others remain | remove it from pending and from `copyset` | `W_INV` | M2 |
| `W_INV` | `INV_ACK` | from the last node in pending | remove it from `copyset`; then grant write to *n*, or apply atomic for *n* | `EXCLUSIVE`, or `UNCACHED` | M2 |
| `W_INV` | `INV_ACK` | from a node not in pending | impossible (H3) | | |
| `W_INV` | `FETCH_DATA` | | impossible (H3) | | |
| `W_INV` | `LOADED` | | impossible (H3) | | |
| `W_INV` | `HOLD_EXPIRED` | | none: the page is not exclusive | `W_INV` | AT |
| `W_FETCH` | `READ_REQ` | | queue | `W_FETCH` | M1 |
| `W_FETCH` | `WRITE_REQ` | | queue | `W_FETCH` | M1 |
| `W_FETCH` | `UPGRADE_REQ` | | queue | `W_FETCH` | M2 |
| `W_FETCH` | `WRITEBACK` | | queue | `W_FETCH` | M4 |
| `W_FETCH` | `ATOMIC_OP` | | queue | `W_FETCH` | M2 |
| `W_FETCH` | `INV_ACK` | | impossible (H3) | | |
| `W_FETCH` | `FETCH_DATA` | from `owner` | store; `owner` = none; `copyset` = {old owner}; grant read to *n* | `SHARED` | M1 |
| `W_FETCH` | `FETCH_DATA` | from another node | impossible (H3) | | |
| `W_FETCH` | `LOADED` | | impossible (H3) | | |
| `W_FETCH` | `HOLD_EXPIRED` | | none: the hold ends with this operation | `W_FETCH` | AT |
| `W_FETCH_INV` | `READ_REQ` | | queue | `W_FETCH_INV` | M2 |
| `W_FETCH_INV` | `WRITE_REQ` | | queue | `W_FETCH_INV` | M2 |
| `W_FETCH_INV` | `UPGRADE_REQ` | | queue | `W_FETCH_INV` | M2 |
| `W_FETCH_INV` | `WRITEBACK` | | queue | `W_FETCH_INV` | M4 |
| `W_FETCH_INV` | `ATOMIC_OP` | | queue | `W_FETCH_INV` | M2 |
| `W_FETCH_INV` | `INV_ACK` | | impossible (H3) | | |
| `W_FETCH_INV` | `FETCH_DATA` | from `owner`, operation is a write | `owner` = none; `copyset` empty; grant write to *n* with the bytes received | `EXCLUSIVE` | M2 |
| `W_FETCH_INV` | `FETCH_DATA` | from `owner`, operation is `ATOMIC_OP` | store; `owner` = none; `copyset` empty; apply atomic for *n* | `UNCACHED` | M2 |
| `W_FETCH_INV` | `FETCH_DATA` | from another node | impossible (H3) | | |
| `W_FETCH_INV` | `LOADED` | | impossible (H3) | | |
| `W_FETCH_INV` | `HOLD_EXPIRED` | | none | `W_FETCH_INV` | AT |
| `W_LOAD` | `READ_REQ` | | queue | `W_LOAD` | M4 |
| `W_LOAD` | `WRITE_REQ` | | queue | `W_LOAD` | M4 |
| `W_LOAD` | `UPGRADE_REQ` | | queue | `W_LOAD` | M4 |
| `W_LOAD` | `WRITEBACK` | | queue | `W_LOAD` | M4 |
| `W_LOAD` | `ATOMIC_OP` | | queue | `W_LOAD` | M4 |
| `W_LOAD` | `INV_ACK` | | impossible (H3) | | |
| `W_LOAD` | `FETCH_DATA` | | impossible (H3) | | |
| `W_LOAD` | `LOADED` | | run the step that was waiting for the bytes | as that step | M4 |
| `W_LOAD` | `HOLD_EXPIRED` | | none: the page is not exclusive | `W_LOAD` | AT |

| Reason | Statement |
| --- | --- |
| H1 | An idle entry has sent no `INV`, `FETCH`, `FETCH_INV` or load that is unanswered: each is sent by an operation, and the operation ends only when all its answers are in. |
| H2 | A node sends `READ_REQ`, `WRITE_REQ` or `UPGRADE_REQ` only when it does not hold the page for writing. The home records a node as owner until it has handled that node's `WRITEBACK` or received its `FETCH_DATA`, and either way the node's next request arrives after that. |
| H3 | A busy entry waits for exactly the answers named by its state, from exactly the nodes it asked. |

An impossible pair that does occur is handled as in section 1.5: the job is aborted.

**[GATE S4]** `ATOMIC_OP` on a shared page invalidates every read copy, the requester's
included, and leaves the page `UNCACHED`. The plan says only that the home "first takes the
page back if another node holds it"; readers must lose their copies too or they would read
the old number.

**[GATE S5]** `W_LOAD` is a proposal. A page in the spill file has to be read from disk before
it can be sent, and the plan forbids the network thread to wait on anything but epoll, so the
read is done elsewhere and returns as the `LOADED` event.

### 2.6 The hold window (AT-1)

The home counts ownership transfers of a page: a grant of write access to a node other than
the last one that had it. `thrash.threshold` gives a count and a period (8 transfers in
100 ms by default).

- When a transfer brings the count within the period above the threshold, the page is
  thrashing. The hold duration becomes `thrash.hold_initial`, or twice its last value up to
  `thrash.hold_max` if the page was already thrashing, and the hold window runs from `now`
  for that long. The home logs the page and the two nodes.
- When a transfer leaves the count at or below the threshold, the hold duration is reset to
  nothing and no window runs.
- While a window runs and the page is `EXCLUSIVE`, the requests that would pass write access
  to another node are **held back**: `WRITE_REQ` and `UPGRADE_REQ`, and `ATOMIC_OP` from a
  node other than the owner. They stay in `waitq`, in order. Every other request is taken as
  usual, ahead of them.
- The window ends at its end time (`HOLD_EXPIRED`), or as soon as the page stops being
  `EXCLUSIVE`, whichever is first. The held-back requests are then taken in order.

**[DECIDED]** Only writers wait. A `READ_REQ` during a hold window is served at once, which
makes the page shared and so ends the window.

**[GATE S6]** The counting rule above is a proposal, as is treating `ATOMIC_OP` as a writer.
One thing in the plan cannot be met from the messages as drafted: AT-1's report is to name
"the page and the two tasks", but a page request carries no task. Proposal: the home reports
the two nodes, and each node's own log shows which task it was running. The alternative is to
add the task ID to `WRITE_REQ` and `UPGRADE_REQ` in `docs/PROTOCOL.md`.

### 2.7 A `WRITEBACK` that arrives late

A node that is evicting a page may be asked for it by `FETCH` or `FETCH_INV` before the home
has handled its `WRITEBACK`. It answers with `FETCH_DATA`, and its `WRITEBACK` is handled
afterwards, when the node is no longer the owner. The page was write-protected from before
the `WRITEBACK` was sent, so those bytes are the ones the home already received, or older
than what a new owner has since written. The home acknowledges and drops them. The node
ignores the acknowledgement if the page has left `M→I` (Rule 0).

**[GATE S3]** This rule is a proposal; the HLD does not cover the case.

## 3. Segments on the move (M4)

When a node leaves, each segment it was home for moves to a new home
(`docs/PROTOCOL.md`, section 8.2). A node keeps one state per segment.

| State | Meaning |
| --- | --- |
| `OTHER` | Another node is home for the segment under this node's map. |
| `SERVING` | This node is home and handles requests by section 2. |
| `FROZEN` | A new map has moved the segment away; operations in progress are finishing. |
| `MOVED` | The segment has been sent and acknowledged. |
| `ARRIVING` | A new map made this node the home; the segment has not yet arrived. |

A "page request" below is `READ_REQ`, `WRITE_REQ`, `UPGRADE_REQ`, `WRITEBACK` or `ATOMIC_OP`
for a page of the segment.

| State | Event | Actions | Next | Since |
| --- | --- | --- | --- | --- |
| `OTHER` | page request, sender's epoch not newer than this node's | reply `REDIRECT` with this node's epoch and the home under it | `OTHER` | M4 |
| `OTHER` | page request, sender's epoch newer | reply `BUSY_RETRY` | `OTHER` | M4 |
| `OTHER` | new map makes this node the home | none | `ARRIVING` | M4 |
| `SERVING` | page request | handle by section 2 | `SERVING` | M1 |
| `SERVING` | new map names another home | answer every request in every `waitq` with `REDIRECT`; if no entry is busy, begin sending the segment | `FROZEN` | M4 |
| `SERVING` | new map drops a member | remove it from every `copyset`; a `SHARED` entry left with an empty `copyset` becomes `UNCACHED`; an entry whose `owner` it is cannot exist (the leaver wrote back first) | `SERVING` | M4 |
| `FROZEN` | page request | reply `REDIRECT` with the new home | `FROZEN` | M4 |
| `FROZEN` | `INV_ACK`, `FETCH_DATA`, `LOADED` | handle by section 2; when no entry is busy, begin sending the segment (`SEG_MIGRATE`) | `FROZEN` | M4 |
| `FROZEN` | `SEG_MIGRATE_DONE` | free the segment's entries and home copies | `MOVED` | M4 |
| `MOVED` | page request | reply `REDIRECT` with the new home | `MOVED` | M4 |
| `ARRIVING` | page request | reply `BUSY_RETRY` | `ARRIVING` | M4 |
| `ARRIVING` | last `SEG_MIGRATE` frame for the segment | install the entries and pages received; reply `SEG_MIGRATE_DONE` | `SERVING` | M4 |

A segment that is `SERVING` at epoch 1 was never sent to anyone: its entries all start
`UNCACHED` with `where` = `ZERO`.

**[GATE S7]** This section follows the leave sequence proposed in `docs/PROTOCOL.md` (P11): the
new map arrives before the segment moves. If the person keeps the HLD's order instead, this
section changes with it.

## 4. Invariants

The simulation harness (M1-5, M2-6) checks these after every step. They hold between events
on every node; a message in transit counts as holding what it carries.

| Name | Statement |
| --- | --- |
| One writer or many readers | If any node has a page in `M`, every other node has it in `I`, `I→S` or `I→M`: no other node can read it. |
| The home knows the holders | Every node with a page in `S`, `S→M`, `M` or `M→I` is in the entry's `copyset`. When the entry is `EXCLUSIVE`, no node but `owner` is in one of those states. |
| No lost update | The bytes last written to a page are always held somewhere: by the node that has it in `M` or `M→I`, or in `S→M` once the home has recorded it as owner; by the home copy (`where` is not `NONE`); or by a `WRITE_GRANT`, `FETCH_DATA` or `WRITEBACK` in transit or waiting in `waitq`. Every copy of the page that can be read is identical to them. |
| No stale read | A read on any node returns the bytes of the latest write to the page that finished before the read began. |
| No early discard | A node discards a page it held in `M` only after the home has the bytes or the bytes are on their way to it: on `WRITEBACK_ACK`, after sending `FETCH_DATA` for `FETCH_INV`, or on `INV` in `M→I`. |
| Zeros only for unwritten pages | A node installs zeros only for a reply with `ZERO_PAGE`, which the home sends only while `where` is `ZERO`. |

## 5. What M1 implements

M1 builds the rows marked M1 and nothing else. On the home side that is: `READ_REQ` on an
`UNCACHED`, `SHARED` or `EXCLUSIVE` page (the last through `FETCH`), and `WRITE_REQ` on a page
no other node holds. Any other request stops the job with "not supported before M2", status
`UNSUPPORTED`.
