# ParaMesh internal interfaces

**Status: approved at the M0 gate on 2026-10-07 and frozen: it changes only with the person's approval. Every paragraph tagged [GATE ...] was a proposal and was approved as drafted (`docs/logs/M0-gate.md`).**

This document defines how the source directories of `libparamesh` call each other. Each
section names the headers that make up one directory's interface and gives their declarations
in full. After the gate this document is frozen; the headers in the tree must say what it
says. Everything else in a directory is private to it.

A paragraph tagged **[GATE In]** is a proposal for the person to approve or change at the M0
gate; the alternatives are in `docs/logs/M0-7.md`. Two points were decided by the person on
7 October 2026 and are marked **[DECIDED]**.

## 1. Rules for every interface

**Who may include whom.** The plan's layout fixes it, and it is one-way.

| Directory | May include the interface headers of |
| --- | --- |
| `platform` | nothing in `src/` |
| `wire` | `platform` |
| `coh` | `wire`, `platform` |
| `mem` | `platform` |
| `store` | `platform` |
| `net` | `wire`, `platform` |
| `rt` | `net`, `wire`, `platform`, and `include/paramesh.h` |
| `lib` | all of the above |
| `pmd` | `net`, `wire`, `platform` |
| `tools` | `wire`, `platform` |

A header is included by its path from `src/`, for example `#include "wire/frame.h"`.
Third-party headers are included only by the adapters in `src/platform/` and by tests.

**Names.** Everything is in the one namespace `paramesh`. A free function carries its
directory as a prefix, as the HLD's names do: `coh_step`, `home_step`, `mem_open`,
`store_open`, `net_open`, `rt_task_id`, `wire_decode_header`.

**Errors.** A function that can fail returns `Result<T>`. Nothing throws. `platform`, `wire`,
`mem`, `store` and `net` never end the job themselves: they return the error, and `src/lib/`
decides. The pure machines in `coh` return an abort action. `rt` ends the job only through
`RuntimeHost::abort_job`. So there is one abort-the-job function, and it lives in `src/lib/`.

**Ownership.** A function named `..._open` returns a `std::unique_ptr` that owns the object;
destroying it releases everything. A reference passed to an `..._open` function (a sink, a
handler, a host) is borrowed and must outlive the object. A `std::span` or `std::string_view`
argument is borrowed for the length of the call.

**Time.** Times and durations are `Nanos`. Code in `src/coh/` never reads a clock: the time is
a field of the event.

**Threads.** A job process has these threads, and each interface says which may call it.

| Thread | Waits only on | Runs |
| --- | --- | --- |
| Fault handler (one, in `mem`) | the userfaultfd and an eventfd | `FaultSink::on_fault` |
| Network (one, in `net`) | epoll | every `NetHandler` callback, `coh_step`, `home_step`, the home store's `get`/`put`/`load`/`drop`, the local cache |
| Workers (in `rt`) | task work, locks, page faults | application task bodies |
| Main | whatever the application does | the `paramesh.h` calls made from `main()` |
| Store I/O (in `store`, from M4) | the disk | spill reads and writes; `StoreEvents` callbacks |

The fault handler and the network thread never take a lock that a thread blocked on a page
fault, on the disk or on the network can hold.

**[GATE I1]** The single namespace with prefixed function names is a proposal.

## 2. `platform`: what every directory shares

Headers: `src/platform/result.h`, `src/platform/ids.h`.

`Result<T>` is the return type the engineering rules require of `libparamesh`. `ids.h` holds
the numbers the design fixes and one small type per kind of ID. The rest of `src/platform/`
(hasher, checksum, JSON, HTTP server, logger, clock, configuration, factory) is M0-3's and is
not part of this document.

**[DECIDED]** These two headers live in `src/platform/` because it is the only directory every
other one may include.

`src/platform/result.h`:

```cpp
// Why an operation failed. Deliberately few: a caller either handles one of these or passes
// the Error up to src/lib/, which turns it into a pm_status or aborts the job.
enum class Errc : std::uint8_t {
    kInvalidArgument,  // a caller broke the function's contract
    kState,            // called at the wrong time: not open, already closed, already running
    kNotFound,         // no such page, peer, task or timer
    kNoSpace,          // the region, the home store or the spill file is full
    kNoMemory,         // the process could not allocate
    kIo,               // a system call failed; Error::os_error holds errno
    kClosed,           // the peer closed the connection
    kProtocol,         // a frame failed a check of docs/PROTOCOL.md, section 3
    kUnsupported,      // not implemented in this milestone, or not available on this machine
};

struct Error {
    Errc code;
    int os_error = 0;       // errno when a system call failed, else 0
    const char* what = "";  // a string literal saying what was being done
};

template <typename T>
class [[nodiscard]] Result {
    static_assert(!std::is_same_v<T, Error>, "Result<Error> is ambiguous");
    static_assert(!std::is_reference_v<T>, "Result of a reference is not supported");

public:
    // Implicit on purpose: `return value;` and `return Error{...};` both work.
    Result(T value) : state_(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : state_(std::in_place_index<1>, error) {}

    [[nodiscard]] bool ok() const noexcept { return state_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    // value() requires ok(); error() requires !ok().
    [[nodiscard]] T& value() & noexcept {
        assert(ok());
        return *std::get_if<0>(&state_);
    }
    [[nodiscard]] const T& value() const& noexcept {
        assert(ok());
        return *std::get_if<0>(&state_);
    }
    [[nodiscard]] T&& value() && noexcept {
        assert(ok());
        return std::move(*std::get_if<0>(&state_));
    }
    [[nodiscard]] const Error& error() const noexcept {
        assert(!ok());
        return *std::get_if<1>(&state_);
    }

private:
    std::variant<T, Error> state_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(error), failed_(true) {}

    [[nodiscard]] bool ok() const noexcept { return !failed_; }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] const Error& error() const noexcept {
        assert(!ok());
        return error_;
    }

private:
    Error error_{Errc::kState, 0, ""};  // meaningful only when failed_
    bool failed_ = false;
};
```

`src/platform/ids.h`:

```cpp
inline constexpr std::size_t kPageSize = 4096;
inline constexpr std::size_t kPagesPerSegment = 512;
inline constexpr std::uint64_t kSegmentSize = kPageSize * kPagesPerSegment;  // 2 MiB
inline constexpr std::uint64_t kRegionBase = 0x600000000000ULL;
inline constexpr std::uint64_t kRegionMaxBytes = 4ULL << 30U;
inline constexpr std::size_t kMaxNodes = 8;   // nodes in one job
inline constexpr std::size_t kMaxSlots = 64;  // bits in a copyset

// Each ID is its own type, so a page number cannot be passed where a node number is meant.
struct NodeId {
    std::uint16_t value = 0;
    friend auto operator<=>(const NodeId&, const NodeId&) = default;
};
inline constexpr NodeId kNoNode{0};
inline constexpr NodeId kAllNodes{0xFFFF};

// (address - kRegionBase) / kPageSize.
struct PageId {
    std::uint64_t value = 0;
    friend auto operator<=>(const PageId&, const PageId&) = default;
};

struct SegmentId {
    std::uint32_t value = 0;
    friend auto operator<=>(const SegmentId&, const SegmentId&) = default;
};

// Matches a reply to its request. 0 means "none".
struct ReqId {
    std::uint64_t value = 0;
    friend auto operator<=>(const ReqId&, const ReqId&) = default;
};
inline constexpr ReqId kNoReq{0};

struct JobId {
    std::uint32_t value = 0;
    friend auto operator<=>(const JobId&, const JobId&) = default;
};

// Version of the segment map.
struct Epoch {
    std::uint32_t value = 0;
    friend auto operator<=>(const Epoch&, const Epoch&) = default;
};

// A member's bit number in every copyset of a job, 0 to kMaxSlots - 1.
struct Slot {
    std::uint8_t value = 0;
    friend auto operator<=>(const Slot&, const Slot&) = default;
};

constexpr SegmentId segment_of(PageId page) noexcept {
    return SegmentId{static_cast<std::uint32_t>(page.value / kPagesPerSegment)};
}
constexpr std::uint64_t address_of(PageId page) noexcept {
    return kRegionBase + page.value * kPageSize;
}
// address must lie inside the region.
constexpr PageId page_of(std::uint64_t address) noexcept {
    return PageId{(address - kRegionBase) / kPageSize};
}

// One page of bytes, borrowed from the caller for the length of the call.
using PageView = std::span<const std::byte, kPageSize>;
using PageBuffer = std::span<std::byte, kPageSize>;

// A point in time or a duration, in nanoseconds on one monotonic clock. Only differences mean
// anything. Code in src/coh/ never reads a clock; it is handed these.
using Nanos = std::chrono::nanoseconds;
```

**[GATE I9]** The nine error codes are a proposal. `src/lib/` maps them to the `pm_status`
values of `paramesh.h` and to the status codes of `docs/PROTOCOL.md`.

## 3. `wire`: frames

Header: `src/wire/frame.h`. Implemented by M1-1.

The header, opcodes, flags and status codes of `docs/PROTOCOL.md`, as types. The enum values
are the bytes on the wire.

```cpp
inline constexpr std::size_t kFrameHeaderSize = 36;
inline constexpr std::uint32_t kFrameMagic = 0x504D5348;  // "PMSH"
inline constexpr std::uint8_t kProtocolVersion = 1;
inline constexpr std::uint32_t kMaxPayload = 1U << 20U;

// docs/PROTOCOL.md, section 4. The values are the bytes on the wire.
enum class Opcode : std::uint8_t {
    kHello = 0x01,
    kBye = 0x02,
    kSpawnReq = 0x10,
    kSpawnOk = 0x11,
    kLedgerSync = 0x12,
    kLeaveIntent = 0x13,
    kSpawnDecline = 0x14,
    kJoinJob = 0x20,
    kSegMap = 0x21,
    kHeartbeat = 0x22,
    kJobEnd = 0x23,
    kSegMapAck = 0x24,
    kReadReq = 0x30,
    kWriteReq = 0x31,
    kUpgradeReq = 0x32,
    kReadData = 0x38,
    kWriteGrant = 0x39,
    kUpgradeGrant = 0x3A,
    kRedirect = 0x3B,
    kBusyRetry = 0x3C,
    kInv = 0x40,
    kInvAck = 0x41,
    kFetch = 0x42,
    kFetchInv = 0x43,
    kFetchData = 0x44,
    kWriteback = 0x45,
    kWritebackAck = 0x46,
    kTaskReq = 0x50,
    kTaskAssign = 0x51,
    kNoTask = 0x52,
    kTaskDone = 0x53,
    kLockAcq = 0x60,
    kLockGrant = 0x61,
    kLockRel = 0x62,
    kBarrierEnter = 0x63,
    kBarrierRelease = 0x64,
    kAtomicOp = 0x65,
    kAtomicResult = 0x66,
    kSegMigrate = 0x70,
    kSegMigrateDone = 0x71,
    kLeaveDone = 0x72,
    kLRegister = 0x80,
    kLAdmitReq = 0x81,
    kLAdmitOk = 0x82,
    kLAdmitRefused = 0x83,
    kLChunk = 0x84,
    kLSegments = 0x85,
    kLJobEnd = 0x86,
    kLQuota = 0x90,
    kLLeave = 0x91,
    kLAbort = 0x92,
    kLLeaveIntent = 0x93,
    kLMember = 0x94,
    kLRunReq = 0xA0,
    kLRunOk = 0xA1,
    kLRunRefused = 0xA2,
    kLLeaveReq = 0xA3,
    kLLeaveReply = 0xA4,
};

// Bits of FrameHeader::flags. docs/PROTOCOL.md, section 3.
inline constexpr std::uint16_t kFlagZeroPage = 0x0001;
inline constexpr std::uint16_t kFlagCompressed = 0x0002;  // reserved; never set
inline constexpr std::uint16_t kFlagReadOnly = 0x0004;
inline constexpr std::uint16_t kFlagRetry = 0x0008;

// docs/PROTOCOL.md, section 12. Sixteen bits because that is its width on the wire.
// NOLINTNEXTLINE(performance-enum-size)
enum class Status : std::uint16_t {
    kOk = 0,
    kLeft = 1,
    kNodeLost = 2,
    kTimeout = 3,
    kTaskFailed = 4,
    kBinaryMismatch = 5,
    kProtocol = 6,
    kUnsupported = 7,
    kUserAbort = 8,
    kInternal = 9,
    kPmdLost = 10,
    kNoCapacity = 11,
    kDeclined = 12,
    kLeaving = 13,
    kBadHandle = 14,
    kNotFound = 15,
    kStartTimeout = 16,
};

// The 36-byte header, decoded. Magic and version are not stored: a FrameHeader exists only
// for a frame whose magic and version were right.
struct FrameHeader {
    Opcode opcode = Opcode::kHeartbeat;
    std::uint16_t flags = 0;
    JobId job;
    NodeId src;
    NodeId dst;
    ReqId req;
    Epoch epoch;
    std::uint32_t payload_len = 0;
    std::uint32_t payload_crc = 0;
};

// Writes the header in network byte order, magic and version included.
void wire_encode_header(const FrameHeader& header, std::span<std::byte, kFrameHeaderSize> out);

// Checks magic, version, that the opcode is known and that payload_len is at most kMaxPayload.
// Errc::kProtocol if any check fails.
Result<FrameHeader> wire_decode_header(std::span<const std::byte, kFrameHeaderSize> in);

// CRC-32C of a payload; 0 for an empty one.
std::uint32_t wire_payload_crc(const Checksum& checksum, std::span<const std::byte> payload);

// Errc::kProtocol unless the payload has the length and the CRC its header states.
Result<void> wire_check_payload(const Checksum& checksum, const FrameHeader& header,
                                std::span<const std::byte> payload);
```

**[GATE I8]** Payloads are not declared here. M1-1 adds, for each opcode it implements, a
struct named after the message with `Payload` appended, an encoder
`wire_encode(const XPayload&, std::span<std::byte>) → Result<std::size_t>` and a decoder
`wire_decode_x(std::span<const std::byte>) → Result<XPayload>`, in headers of its own in
`src/wire/`. Those become part of this interface when they exist.

## 4. `coh`: the two state machines

Headers: `src/coh/node_machine.h`, `src/coh/home_machine.h`. Implemented by M1-3, M1-4, M2-1,
M2-2, AT-1 and M4-5.

Both machines are pure functions over the tables of `docs/STATE_MACHINES.md`. They make no
system call, read no clock and send nothing: they return actions, and the caller in `src/lib/`
carries them out against `mem`, `store` and `net`. That is what lets `tests/sim/` run them on
a fake message bus.

`src/coh/node_machine.h`:

```cpp
// One byte per page of the region. Names follow docs/STATE_MACHINES.md, section 1.1.
enum class PageState : std::uint8_t {
    kInvalid,           // I
    kShared,            // S
    kModified,          // M
    kReadPending,       // I→S
    kWritePending,      // I→M
    kUpgradePending,    // S→M
    kWritebackPending,  // M→I
};

// docs/STATE_MACHINES.md, section 1.2.
enum class NodeEventKind : std::uint8_t {
    kNeedRead,
    kNeedWrite,
    kEvict,
    kReadData,
    kWriteGrant,
    kUpgradeGrant,
    kWritebackAck,
    kBusyRetry,
    kRedirect,
    kResend,
    kInv,
    kFetch,
    kFetchInv,
};

struct NodeEvent {
    NodeEventKind kind = NodeEventKind::kNeedRead;
    PageId page;
    // For a reply: the request it answers. For kInv, kFetch and kFetchInv: the home's request,
    // to be repeated in the answer. Otherwise kNoReq.
    ReqId req;
    // The caller's next unused request number. coh_step uses it if, and only if, the step
    // sends a request; the caller then never offers that number again.
    ReqId fresh;
};

// docs/STATE_MACHINES.md, section 1.3. The caller carries these out, in order.
enum class NodeActionKind : std::uint8_t {
    kSend,             // send `opcode` to the page's home with request number `req`
    kReply,            // send `opcode` back to the home, repeating `req`
    kInstallReadOnly,  // map the page from the reply (zeros if it had ZERO_PAGE), write-protected
    kInstallWritable,  // the same, writable
    kWriteProtect,
    kAllowWrites,
    kDiscard,
    kWake,
    kArmResend,  // deliver kResend after the back-off, or once the redirect's map is applied
    kAbort,      // an impossible pair occurred: end the job with Status::kInternal
};

struct NodeAction {
    NodeActionKind kind = NodeActionKind::kWake;
    Opcode opcode = Opcode::kHeartbeat;  // for kSend and kReply
    ReqId req;                           // for kSend and kReply
    bool with_page = false;              // the message carries this node's copy of the page
    bool read_only = false;              // the message has the READ_ONLY flag
};

inline constexpr std::size_t kMaxNodeActions = 4;

struct NodeStep {
    PageState state = PageState::kInvalid;  // the page's state after the event
    ReqId current;                          // its current request afterwards, or kNoReq
    bool dropped = false;                   // Rule 0: a reply to a superseded request; count it
    std::uint8_t action_count = 0;
    std::array<NodeAction, kMaxNodeActions> actions;
};

// state and current are the page's state and current request before the event.
NodeStep coh_step(PageState state, ReqId current, const NodeEvent& event) noexcept;
```

`src/coh/home_machine.h`:

```cpp
// docs/STATE_MACHINES.md, section 2.1.
enum class HomeState : std::uint8_t { kUncached, kShared, kExclusive };
enum class HomeWhere : std::uint8_t { kZero, kRam, kSpill, kNone };
// Section 2.2: what a busy entry is waiting for.
enum class HomeWait : std::uint8_t { kIdle, kInv, kFetch, kFetchInv, kLoad };

// Section 2.3.
enum class HomeEventKind : std::uint8_t {
    kReadReq,
    kWriteReq,
    kUpgradeReq,
    kWriteback,
    kAtomicOp,
    kInvAck,
    kFetchData,
    kLoaded,
    kHoldExpired,
};

struct HomeEvent {
    HomeEventKind kind = HomeEventKind::kReadReq;
    PageId page;
    NodeId from;  // the node the message came from; kNoNode for kLoaded and kHoldExpired
    Slot slot;    // that node's copyset bit
    ReqId req;    // the message's request number
    Nanos now{};  // the time of the event
    // kAtomicOp only: where in the page the number is, and what to add.
    std::uint16_t offset_in_page = 0;
    std::uint64_t operand = 0;
};

// Where the bytes of a page-carrying reply come from.
enum class PageSource : std::uint8_t {
    kNone,      // the message carries no page data
    kZero,      // send the ZERO_PAGE flag and no data
    kHomeCopy,  // the home copy, from the store
    kReceived,  // the bytes of the FETCH_DATA or WRITEBACK that caused this step
};

enum class HomeActionKind : std::uint8_t {
    kSend,          // send `opcode` (INV, FETCH or FETCH_INV) for the page to node `to`
    kReply,         // send `opcode` to node `to`, repeating `req`, with the page from `source`
    kStore,         // keep the received bytes as the home copy
    kLoad,          // ask the store to read the home copy back; kLoaded follows
    kDropCopy,      // the home copy is no longer valid; the store may free it
    kApplyAtomic,   // add `operand` at `offset_in_page` of the home copy, then reply
                    // ATOMIC_RESULT with the old value to node `to`, repeating `req`
    kArmHoldTimer,  // deliver kHoldExpired at time `at`
    kReportThrash,  // log the page and the two nodes fighting over it: `to` and `other`
    kAbort,         // an impossible pair occurred: end the job with Status::kInternal
};

struct HomeAction {
    HomeActionKind kind = HomeActionKind::kAbort;
    PageId page;
    Opcode opcode = Opcode::kHeartbeat;
    NodeId to;
    NodeId other;
    ReqId req;
    PageSource source = PageSource::kNone;
    bool read_only = false;  // the reply has the READ_ONLY flag
    Nanos at{};
    std::uint16_t offset_in_page = 0;
    std::uint64_t operand = 0;
};

// The tunables the hold window needs (docs/PLAN.md, Tunables).
struct HomeConfig {
    std::uint32_t thrash_transfers = 8;  // thrash.threshold: this many transfers ...
    Nanos thrash_period{};               // ... within this long
    Nanos hold_initial{};                // thrash.hold_initial
    Nanos hold_max{};                    // thrash.hold_max
};

// What a caller may see of one directory entry.
struct HomeEntryView {
    HomeState state = HomeState::kUncached;
    HomeWhere where = HomeWhere::kZero;
    HomeWait wait = HomeWait::kIdle;
    NodeId owner;
    std::uint64_t copyset = 0;  // bit Slot::value set for each member that may hold a copy
    std::uint32_t version = 0;
    std::size_t queued = 0;  // requests waiting in the entry's queue
};

// The entries of every segment this node is home for. How they are stored is private to
// src/coh/.
class HomeDirectory;
struct HomeDirectoryDeleter {
    void operator()(HomeDirectory* directory) const noexcept;
};
using HomeDirectoryPtr = std::unique_ptr<HomeDirectory, HomeDirectoryDeleter>;

Result<HomeDirectoryPtr> home_open(const HomeConfig& config);

// Handles one event for one page and appends the actions to `out`, in the order they are to
// be carried out. One event can finish an operation and start the next queued ones, so the
// number of actions has no fixed bound.
void home_step(HomeDirectory& directory, const HomeEvent& event, std::vector<HomeAction>& out);

// A segment this node is home for from the start of the job: every page uncached and never
// written.
Result<void> home_add_segment(HomeDirectory& directory, SegmentId segment);
// Forgets a segment that has been migrated away.
void home_remove_segment(HomeDirectory& directory, SegmentId segment);
// True while any entry of the segment has an operation in progress.
bool home_segment_busy(const HomeDirectory& directory, SegmentId segment);
// Answers every queued request of the segment with REDIRECT to `new_home` (section 3,
// SERVING to FROZEN).
void home_freeze_segment(HomeDirectory& directory, SegmentId segment, NodeId new_home,
                         std::vector<HomeAction>& out);

// For SEG_MIGRATE: read an entry out, and install one that arrived.
HomeEntryView home_entry(const HomeDirectory& directory, PageId page);
Result<void> home_import_entry(HomeDirectory& directory, PageId page, const HomeEntryView& entry);

// The store moved a home copy between RAM and the spill file.
void home_set_where(HomeDirectory& directory, PageId page, HomeWhere where);
// A member left the job: clear its bit in every copyset.
void home_drop_member(HomeDirectory& directory, Slot slot);
```

**[GATE I4]** `coh_step` takes the page's state and current request by value and returns the
new ones with at most four actions, so it allocates nothing and the caller keeps the one byte
per page the HLD asks for. The caller supplies an unused request number with each event.

**[GATE I3]** The HLD writes `home_step(entry, msg) → actions`. Here it takes the whole
directory and the event, and appends its actions to a vector: one event can finish an
operation and start queued ones, and how entries and their queues are stored stays private to
`src/coh/`. Other directories see an entry only as a `HomeEntryView`.

## 5. `mem`: the memory engine

Header: `src/mem/memory_engine.h`. Implemented by M0-8 and M2-3.

```cpp
enum class FaultKind : std::uint8_t {
    kRead,   // a thread read a page that is not mapped
    kWrite,  // a thread wrote a page that is not mapped, or one that is write-protected
};

struct FaultEvent {
    PageId page;
    FaultKind kind = FaultKind::kRead;
};

// Receives fault events. Implemented by src/lib/.
class FaultSink {
public:
    FaultSink() = default;
    FaultSink(const FaultSink&) = delete;
    FaultSink& operator=(const FaultSink&) = delete;
    FaultSink(FaultSink&&) = delete;
    FaultSink& operator=(FaultSink&&) = delete;
    virtual ~FaultSink() = default;

    // Called on the fault-handler thread, once per fault the kernel reports. It must not wait
    // for the network, the disk or a lock a blocked thread can hold: it queues or sends and
    // returns. The faulting thread stays parked until install(), write_protect(off) or wake().
    virtual void on_fault(const FaultEvent& event) noexcept = 0;
};

enum class PageProtection : std::uint8_t { kReadOnly, kWritable };

// The HLD's helpers: mem_install() is install(), mem_zap() is zap(), mem_wp() is
// write_protect(), mem_wake() is wake(). All may be called from any thread.
class MemoryEngine {
public:
    MemoryEngine() = default;
    MemoryEngine(const MemoryEngine&) = delete;
    MemoryEngine& operator=(const MemoryEngine&) = delete;
    MemoryEngine(MemoryEngine&&) = delete;
    MemoryEngine& operator=(MemoryEngine&&) = delete;
    // Unmaps the region and stops the fault-handler thread.
    virtual ~MemoryEngine() = default;

    // Maps the page with these bytes and wakes every thread parked on it. A page that is
    // already mapped is left as it is, and that counts as success.
    virtual Result<void> install(PageId page, PageView data, PageProtection protection) = 0;

    // Unmaps the page. The next access to it faults. Parked threads are not woken.
    virtual Result<void> zap(PageId page) = 0;

    // on: a thread that then writes the page parks and a kWrite fault is reported.
    // off: writes are allowed again, and threads parked on the page are woken.
    virtual Result<void> write_protect(PageId page, bool on) = 0;

    // Wakes every thread parked on the page. Each retries its access and faults again if the
    // page still does not allow it.
    virtual Result<void> wake(PageId page) = 0;

    // Copies the mapped page out, for FETCH_DATA and WRITEBACK. The caller write-protects the
    // page first so the bytes cannot change. Errc::kState if the page is not mapped.
    virtual Result<void> read(PageId page, PageBuffer out) = 0;
};

struct RegionConfig {
    std::uint64_t bytes = kRegionMaxBytes;  // a whole number of segments
};

// Maps `bytes` at kRegionBase, registers it for missing and write-protect faults, and starts
// the fault-handler thread, which reports to `sink`. `sink` must outlive the engine.
// Errc::kUnsupported if the kernel lacks what is needed; Errc::kState if the address is taken.
Result<std::unique_ptr<MemoryEngine>> mem_open(const RegionConfig& config, FaultSink& sink);
```

**[GATE I2]** The HLD names four functions: `mem_install()`, `mem_zap()`, `mem_wp()` and
`mem_wake()`. Here they are the members `install`, `zap`, `write_protect` and `wake` of an
abstract class, and a fifth, `read`, is added. The reason is testing: the plan expects the
hosted runners may not allow userfaultfd, and with an abstract engine `src/lib/` can be
tested in Actions against a fake. M0-8 implements this class; its card's "four `mem` helpers"
are these members. The same choice is made for `HomeStore`, `LocalCache`, `Transport` and
`Runtime`.

## 6. `store`: home copies, the local cache, placement

Headers: `src/store/home_store.h`, `src/store/local_cache.h`, `src/store/placement.h`.
Implemented by M1-6, M4-2 and M4-3.

`src/store/home_store.h`:

```cpp
// Receives what the store does on its own. Implemented by src/lib/, which passes each on to
// the home directory.
class StoreEvents {
public:
    StoreEvents() = default;
    StoreEvents(const StoreEvents&) = delete;
    StoreEvents& operator=(const StoreEvents&) = delete;
    StoreEvents(StoreEvents&&) = delete;
    StoreEvents& operator=(StoreEvents&&) = delete;
    virtual ~StoreEvents() = default;

    // A load() finished: the page is in RAM again and get() will copy it. May be called on a
    // thread of the store's own.
    virtual void on_loaded(PageId page) noexcept = 0;
    // The store moved the page to the spill file to make room. Same threading.
    virtual void on_spilled(PageId page) noexcept = 0;
};

enum class StoreLookup : std::uint8_t {
    kCopied,   // the page was in RAM and has been copied to `out`
    kAbsent,   // the store holds no copy: never put, or dropped
    kSpilled,  // the page is in the spill file; nothing was copied; call load()
};

// get(), put(), load() and drop() are called on the network thread and never wait for the
// disk.
class HomeStore {
public:
    HomeStore() = default;
    HomeStore(const HomeStore&) = delete;
    HomeStore& operator=(const HomeStore&) = delete;
    HomeStore(HomeStore&&) = delete;
    HomeStore& operator=(HomeStore&&) = delete;
    virtual ~HomeStore() = default;

    virtual Result<StoreLookup> get(PageId page, PageBuffer out) = 0;

    // Keeps a copy of the bytes, replacing any earlier copy. Errc::kNoSpace when the RAM cap
    // and the spill budget are both used up.
    virtual Result<void> put(PageId page, PageView data) = 0;

    // Starts reading a spilled page back. StoreEvents::on_loaded follows.
    virtual Result<void> load(PageId page) = 0;

    // Forgets the copy, in RAM or in the spill file.
    virtual void drop(PageId page) = 0;

    // Bytes of page data held in RAM now, for the cap and for the status API.
    [[nodiscard]] virtual std::uint64_t ram_bytes() const noexcept = 0;
};

struct StoreConfig {
    std::uint64_t ram_cap_bytes = 0;       // most page data to hold in RAM
    std::uint64_t spill_budget_bytes = 0;  // most to hold in the spill file; 0 for none
    JobId job;                             // names the spill file
};

// `events` must outlive the store.
Result<std::unique_ptr<HomeStore>> store_open(const StoreConfig& config, StoreEvents& events);
```

`src/store/local_cache.h`:

```cpp
// Called on the network thread only.
class LocalCache {
public:
    LocalCache() = default;
    LocalCache(const LocalCache&) = delete;
    LocalCache& operator=(const LocalCache&) = delete;
    LocalCache(LocalCache&&) = delete;
    LocalCache& operator=(LocalCache&&) = delete;
    virtual ~LocalCache() = default;

    // The page was installed, or discarded, on this node.
    virtual void note_installed(PageId page) = 0;
    virtual void note_discarded(PageId page) = 0;

    // True while more pages are mapped than the budget allows.
    [[nodiscard]] virtual bool over_budget() const noexcept = 0;

    // The page mapped longest that has not been offered since it was last installed, or none.
    // The caller raises EVICT for it; if the node machine does not free it (a page in flight),
    // the caller asks again and gets the next.
    virtual std::optional<PageId> next_victim() = 0;
};

Result<std::unique_ptr<LocalCache>> cache_open(std::uint64_t budget_bytes);
```

`src/store/placement.h`:

```cpp
struct PlacementMember {
    NodeId node;
    Slot slot;
    std::uint64_t ram_weight = 0;  // bytes of RAM contributed; 0 means it hosts no segment
};

// RAM-weighted rendezvous hashing. For each segment i, homes[i] becomes the slot of its home.
// The result depends only on the members' node IDs and weights and on the segment number, so
// it is the same on every node, and removing a member moves only that member's segments.
// Errc::kInvalidArgument if no member has a weight above 0.
Result<void> store_place_segments(std::span<const PlacementMember> members, std::span<Slot> homes);
```

**[GATE I6]** M1-6's card lists both `src/store/` and `src/rt/` for the hashing of segments to
homes. It is placed in `store` here. It returns the home of every segment as a table, which is
what `SEG_MAP` carries (P13 in M0-5).

## 7. `net`: the transport

Header: `src/net/transport.h`. Implemented by M1-2.

```cpp
struct Endpoint {
    std::uint32_t ipv4 = 0;  // host byte order
    std::uint16_t port = 0;
};

// What this process says about itself in JOIN_JOB, and what a peer said.
struct JoinInfo {
    NodeId node;
    std::uint8_t role = 0;  // 1 launcher, 2 worker
    std::uint16_t listen_port = 0;
    std::uint32_t pid = 0;
    std::array<std::byte, 32> binary_hash{};
};

struct TimerId {
    std::uint64_t value = 0;
    friend auto operator<=>(const TimerId&, const TimerId&) = default;
};

// Whether a sent frame is a request that the 2 s reply timer covers
// (docs/PROTOCOL.md, section 11).
enum class ReplyTimer : std::uint8_t { kNone, kTimed };

// The receive callbacks. Implemented by src/lib/, and by src/rt/ tests with a fake. Every
// call is made on the network thread and must not block.
class NetHandler {
public:
    NetHandler() = default;
    NetHandler(const NetHandler&) = delete;
    NetHandler& operator=(const NetHandler&) = delete;
    NetHandler(NetHandler&&) = delete;
    NetHandler& operator=(NetHandler&&) = delete;
    virtual ~NetHandler() = default;

    // A connection to the peer is up and its JOIN_JOB has arrived.
    virtual void on_peer_joined(const JoinInfo& peer) noexcept = 0;

    // A whole frame arrived and passed the checks of docs/PROTOCOL.md, section 3. `payload`
    // is valid only during the call. Frames from one peer are delivered in the order sent.
    // HEARTBEAT and JOIN_JOB are handled by the transport and are not delivered.
    virtual void on_frame(const FrameHeader& header,
                          std::span<const std::byte> payload) noexcept = 0;

    // The peer is gone: 3 s of silence, a reset, an unexpected close, or a malformed frame
    // (Errc::kProtocol). Not called for a peer removed with remove_peer().
    virtual void on_peer_lost(NodeId peer, Errc why) noexcept = 0;

    // A timed request got no reply within 2 s, was sent again with RETRY, and got none in
    // 2 s more.
    virtual void on_reply_timeout(NodeId peer, ReqId req) noexcept = 0;

    // A timer started with start_timer() fired.
    virtual void on_timer(TimerId timer, Nanos now) noexcept = 0;
};

// The HLD's net_send() is send().
class Transport {
public:
    Transport() = default;
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;
    Transport(Transport&&) = delete;
    Transport& operator=(Transport&&) = delete;
    virtual ~Transport() = default;

    // Starts accepting connections and returns the port chosen when `local.port` is 0.
    virtual Result<std::uint16_t> listen(Endpoint local) = 0;

    // Makes the peer known. If this node's ID is the lower, connects to it; otherwise waits
    // for it to connect. on_peer_joined follows.
    virtual Result<void> add_peer(NodeId peer, Endpoint remote) = 0;

    // Closes the connection to a peer that has left in order. Not reported as lost.
    virtual void remove_peer(NodeId peer) = 0;

    // Queues one frame for the peer and returns; may be called from any thread. The transport
    // fills in the header's job, src, payload_len and payload_crc. Frames to one peer leave in
    // the order send() was called. A reply is recognised by its req: it stops the reply timer
    // of the request with that number sent to that peer.
    // Errc::kNotFound for an unknown peer, Errc::kInvalidArgument for a payload over
    // kMaxPayload.
    virtual Result<void> send(NodeId to, const FrameHeader& header,
                              std::span<const std::byte> payload, ReplyTimer timer) = 0;

    // One-shot timer; on_timer follows after `delay`. May be called from any thread.
    virtual TimerId start_timer(Nanos delay) = 0;
    virtual void cancel_timer(TimerId timer) = 0;

    // The time on the clock that on_timer reports.
    [[nodiscard]] virtual Nanos now() const noexcept = 0;

    // Runs the network thread's loop on the calling thread until stop(). It waits only on
    // epoll.
    virtual Result<void> run() = 0;
    // May be called from any thread.
    virtual void stop() noexcept = 0;
};

struct TransportConfig {
    JobId job;
    JoinInfo self;
};

// `checksum` and `handler` must outlive the transport.
Result<std::unique_ptr<Transport>> net_open(const TransportConfig& config, const Checksum& checksum,
                                            NetHandler& handler);
```

**[GATE I7]** Three things are given to the transport that the HLD does not assign: it sends
and checks `JOIN_JOB` and `HEARTBEAT` itself, reporting only what the peer said; it fills in
the header fields that are the same for every frame of a process; and it provides the one-shot
timers that the back-off after `BUSY_RETRY` and the hold window need, since it already owns
the only thread that may wait on a timer.

## 8. `rt`: the task runtime

Header: `src/rt/runtime.h`: what the runtime needs from the job process, one
`pm_parallel_for` call as it is handed over, and the task registry.

**[CHANGED at the M3 gate, 2026-10-10]** The draft had one `Runtime` class behind `rt_open()`
that carried out every runtime function of `paramesh.h`. It was never built, and the person
approved bringing this section in line with the code (`docs/logs/M3-gate.md`,
`docs/logs/X-m3-runtime-api.md`). The runtime is these pieces, each created and owned by the
job process in `src/lib/`, which is the runtime's only user:

| Header | What it is | Built by |
| --- | --- | --- |
| `rt/sync.h` | `Sync`: locks and barriers, kept by the launcher, and the count of outstanding chunks that `pm_wait_all` waits on | M2-4 |
| `rt/tasks.h` | `Tasks`: the launcher's queue of chunks and every process's worker threads | M3-2, M3-3 |
| `rt/registry.h` | Looking a task up by its ID, and `rt_run_task`, the one caller of task bodies | M3-1 |
| `rt/region_allocator.h` | `RegionAllocator`: the bump allocator behind `pm_malloc` | M1-6 |

Those four headers are not frozen; their declarations are in the headers themselves.
`pm_malloc` and `pm_atomic_add` are carried out in `src/lib/`: an atomic add is one frame to
the page's home, which may be the caller's own node, and only the job process can deliver a
frame to itself.


```cpp
// What the runtime needs from the job process. Implemented by src/lib/, and by a fake in the
// runtime's unit tests.
class RuntimeHost {
public:
    RuntimeHost() = default;
    RuntimeHost(const RuntimeHost&) = delete;
    RuntimeHost& operator=(const RuntimeHost&) = delete;
    RuntimeHost(RuntimeHost&&) = delete;
    RuntimeHost& operator=(RuntimeHost&&) = delete;
    virtual ~RuntimeHost() = default;

    [[nodiscard]] virtual NodeId self() const noexcept = 0;
    [[nodiscard]] virtual NodeId launcher() const noexcept = 0;
    // The home of a page under the current segment map, for ATOMIC_OP and for data affinity.
    [[nodiscard]] virtual NodeId home_of(PageId page) const noexcept = 0;

    // The one way to end the job on a fatal condition: logs the reason, tells the launcher
    // (or every process, on the launcher) with JOB_END, and exits the process.
    [[noreturn]] virtual void abort_job(Status status, std::string_view message) noexcept = 0;

    // A chunk finished. `ran_by` is this node for a chunk this process ran, with the CPU time
    // it measured; on the launcher it is also called for chunks peers completed, with 0.
    // src/lib/ reports it to pmd as L_CHUNK.
    virtual void chunk_finished(NodeId ran_by, std::uint64_t task_id, std::uint64_t indexes,
                                Nanos cpu) noexcept = 0;
};

// One pm_parallel_for or pm_parallel_for_data call.
struct ParallelFor {
    std::string_view task;
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;
    std::uint64_t grain = 0;
    std::span<const std::byte> arg;
    const void* data = nullptr;  // nullptr for pm_parallel_for
    std::size_t stride = 0;
};

// The task registry is process-wide and filled before pm_init() by PM_TASK's constructors.
// Errc::kInvalidArgument for an empty name or a null function.
Result<void> rt_register_task(const char* name, pm_task_fn fn) noexcept;
// 64-bit FNV-1a of the name: the task's ID on the wire.
std::uint64_t rt_task_id(std::string_view name) noexcept;
// Errc::kInvalidArgument, with the two names in Error::what, if two registered names are
// equal or share an ID. pm_init() calls it and stops the program on failure.
Result<void> rt_check_registry() noexcept;
```

**[GATE I5]** `rt/runtime.h` includes `paramesh.h`, for `pm_task_fn`, `pm_lock_t` and
`pm_barrier_t`. The plan's layout gives `rt` only `wire` and `net` to depend on. The public
header depends on nothing, so no cycle arises, and the build file of `src/rt/` now names it.

**[GATE I10]** Still open #12 (data affinity by "cached at"). The runtime can ask its host only
for a page's home, `RuntimeHost::home_of`. That is enough for M3-3 as the plan words it. If
the person wants affinity by where pages are cached as well, the launcher needs that
information from other homes, and this interface and `docs/PROTOCOL.md` both grow.

## 9. `lib`, `pmd` and `tools`

**[DECIDED]** These three directories have no interface header. Nothing includes them: they
are the top of the dependency order. `src/lib/` implements `include/paramesh.h`, which is its
interface. `pmd` and a job process talk through the local-link messages of
`docs/PROTOCOL.md`, section 9, and `pm` and `pmrun` through the same socket and the status
API. Headers inside these directories are private and may change without approval.

## 10. How the pieces meet: a read miss

This is the path M1-7 wires. Each step names the interface it uses.

1. A task reads a page this node does not hold. The kernel parks the thread, and the fault
   handler thread calls `FaultSink::on_fault` with `FaultKind::kRead`.
2. `src/lib/` passes the fault to the network thread, which calls `coh_step` with
   `NodeEventKind::kNeedRead`. The step returns state `kReadPending` and one action:
   `kSend` of `READ_REQ`.
3. `src/lib/` finds the page's home in its segment map and calls `Transport::send` with
   `ReplyTimer::kTimed`.
4. On the home, `NetHandler::on_frame` delivers the request. `src/lib/` calls `home_step` with
   `HomeEventKind::kReadReq`. For a page in RAM the actions are one `kReply` of `READ_DATA`
   with `PageSource::kHomeCopy`.
5. `src/lib/` on the home calls `HomeStore::get` and `Transport::send`.
6. Back on the requester, `on_frame` delivers `READ_DATA`. `coh_step` with `kReadData`
   returns state `kShared` and the action `kInstallReadOnly`.
7. `src/lib/` calls `MemoryEngine::install` with `PageProtection::kReadOnly`, which wakes the
   task, and `LocalCache::note_installed`.

## 11. The HLD's names

| HLD | Here |
| --- | --- |
| `mem_install()` | `MemoryEngine::install` |
| `mem_zap()` | `MemoryEngine::zap` |
| `mem_wp()` | `MemoryEngine::write_protect` |
| `mem_wake()` | `MemoryEngine::wake` |
| fault events | `FaultSink::on_fault` |
| `coh_step(state, event) → actions` | `coh_step` |
| `home_step(entry, msg) → actions` | `home_step` |
| `net_send()` | `Transport::send` |
| receive callbacks | `NetHandler` |
