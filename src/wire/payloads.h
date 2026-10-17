// Payloads of the opcodes built so far (docs/PROTOCOL.md, section 7). Each has a struct,
// wire_encode() and a decoder. A decoder returns Errc::kProtocol for a payload that breaks the
// protocol; wire_encode() returns the bytes written, or Errc::kInvalidArgument for a value that
// cannot be sent or a buffer that is too small.
#ifndef PARAMESH_WIRE_PAYLOADS_H
#define PARAMESH_WIRE_PAYLOADS_H

#include "platform/ids.h"
#include "platform/result.h"
#include "wire/frame.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace paramesh {

// READ_REQ, WRITE_REQ, FETCH (and, from M2, every other message that carries only a page ID).
struct PageIdPayload {
    PageId page;
};

// READ_DATA, WRITE_GRANT, FETCH_DATA. `data` is the page's 4,096 bytes, or empty for a frame
// with ZERO_PAGE. After decoding it points into the payload that was decoded.
struct PagePayload {
    PageId page;
    std::span<const std::byte> data;
};

struct JoinJobPayload {
    std::uint8_t role = 0;  // 1 launcher, 2 worker
    std::uint16_t listen_port = 0;
    std::uint32_t pid = 0;
    std::array<std::byte, 32> binary_hash{};
};

inline constexpr std::uint8_t kMemberLauncher = 0x01;
inline constexpr std::uint8_t kMemberNoHomes = 0x02;
inline constexpr std::uint8_t kMemberLeaving = 0x04;

struct SegMapMember {
    NodeId node;
    Slot slot;
    std::uint8_t flags = 0;
    std::uint32_t addr = 0;  // IPv4, host byte order
    std::uint16_t port = 0;
    std::uint64_t ram_weight = 0;
};

struct SegMapPayload {
    std::vector<SegMapMember> members;  // 1 to 8
    std::vector<Slot> homes;            // one per segment, 1 to 2,048: the slot of its home
};

struct SegMapAckPayload {
    Epoch epoch;
};

struct JobEndPayload {
    Status status = Status::kOk;
    NodeId node;
    std::string message;  // at most 512 bytes
};

// LOCK_ACQ, LOCK_REL and BARRIER_ENTER, where `word` is the thread index, and LOCK_GRANT and
// BARRIER_RELEASE, where it is a Status: all five are the handle's id, one 16-bit word and
// six reserved bytes.
struct SyncPayload {
    std::uint64_t id = 0;
    std::uint16_t word = 0;
};

// ATOMIC_OP: add `operand` to the 64-bit number `offset` bytes from the region base.
struct AtomicOpPayload {
    std::uint64_t offset = 0;  // a multiple of 8
    std::uint64_t operand = 0;
};

// ATOMIC_RESULT: the number's value before the operation.
struct AtomicResultPayload {
    std::uint64_t offset = 0;
    std::uint64_t old = 0;
};

// TASK_REQ: a worker thread asks the launcher for a chunk.
struct TaskReqPayload {
    std::uint16_t thread = 0;
};

// TASK_ASSIGN: one chunk, with the argument of its pm_parallel_for call.
struct TaskAssignPayload {
    std::uint64_t chunk_id = 0;
    std::uint64_t task_id = 0;
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;
    std::uint32_t call_id = 0;
    std::vector<std::byte> arg;  // at most kMaxTaskArg bytes
};
inline constexpr std::size_t kMaxTaskArg = 1024;

// NO_TASK. 1: the queue is empty for now. 2: this node gets no more tasks.
struct NoTaskPayload {
    std::uint8_t reason = 1;
};

// TASK_DONE: a chunk finished, and the CPU time its thread spent on it.
struct TaskDonePayload {
    std::uint64_t chunk_id = 0;
    std::uint64_t cpu_ns = 0;
    std::uint16_t thread = 0;
};

// One raw ledger total in HELLO: the sender's own counts with one peer (M5 fills them).
struct LedgerTotal {
    NodeId peer;
    std::uint64_t chunks_for_peer = 0;
    std::uint64_t chunks_by_peer = 0;
    std::uint64_t segsec_for_peer = 0;
    std::uint64_t segsec_by_peer = 0;
};

// LEDGER_SYNC: the sender's own raw counts with the receiver, one entry for each job, sent when
// a job ends on the sender (M5-4). For display and the disagreement alarm only.
struct LedgerSyncEntry {
    JobId job;
    std::uint64_t chunks_for_peer = 0;
    std::uint64_t chunks_by_peer = 0;
    std::uint64_t segsec_for_peer = 0;
    std::uint64_t segsec_by_peer = 0;
};
struct LedgerSyncPayload {
    std::vector<LedgerSyncEntry> entries;  // at least 1
};

inline constexpr std::uint16_t kHelloLeaving = 0x0001;

// HELLO: a pmd's beacon, every second. Its address is the datagram's source address.
struct HelloPayload {
    std::uint32_t instance = 0;  // random, chosen at each start
    std::uint16_t control_port = 0;
    std::uint16_t flags = 0;
    std::uint64_t cap_ram = 0;
    std::uint64_t free_ram = 0;
    std::uint64_t spill_budget = 0;
    std::uint64_t free_spill = 0;
    std::uint16_t cap_cores = 0;
    std::uint16_t free_cores = 0;
    std::uint16_t jobs = 0;
    std::vector<LedgerTotal> totals;  // 0 to 7
};

// BYE: a pmd leaves the pool.
struct ByePayload {
    std::uint32_t instance = 0;
};

// SPAWN_REQ: a launcher's pmd asks a peer's pmd to start one worker process.
inline constexpr std::uint16_t kSpawnMidRun = 0x0001;  // SPAWN_REQ: join a running job
struct SpawnReqPayload {
    NodeId launcher_node;
    std::uint16_t flags = 0;
    std::uint32_t launcher_addr = 0;  // IPv4, host byte order
    std::uint16_t launcher_port = 0;
    std::uint16_t threads_per_node = 0;
    std::uint64_t region_bytes = 0;
    std::array<std::byte, 32> binary_hash{};
    std::string path;               // absolute, the same on every node
    std::string cwd;                // the launcher's working directory
    std::vector<std::string> argv;  // without the program name
};
inline constexpr std::size_t kMaxSpawnReq = 65536;

// SPAWN_OK: the worker is running and has registered.
struct SpawnOkPayload {
    NodeId node;
    std::uint16_t data_port = 0;
    std::uint16_t cores = 0;
    std::uint64_t ram_commit = 0;
    std::uint64_t spill_commit = 0;
};

// SPAWN_DECLINE: why not, and a text for the launcher's log (at most 512 bytes).
struct SpawnDeclinePayload {
    Status status = Status::kInternal;
    std::string message;
};

// L_ADMIT_REFUSED: what the job needs, what the chosen nodes can give, and a text to print.
struct LAdmitRefusedPayload {
    std::uint64_t needed = 0;
    std::uint64_t available = 0;
    Status status = Status::kNoCapacity;
    std::string message;
};

// L_RUN_REFUSED and L_ABORT have the layout of SPAWN_DECLINE: a status and a text.
using StatusTextPayload = SpawnDeclinePayload;

// L_RUN_REQ: pmrun asks the local pmd for a job of `nodes` nodes running this executable.
struct LRunReqPayload {
    std::uint16_t nodes = 1;  // 1 to 8
    std::array<std::byte, 32> binary_hash{};
    std::string path;
    std::string cwd;
    std::vector<std::string> argv;  // without the program name
};

// L_RUN_OK: the new job's ID, and this node's.
struct LRunOkPayload {
    JobId job;
    NodeId node;
};

// L_ADMIT_REQ: the launcher asks its pmd to admit the job and start the workers.
struct LAdmitReqPayload {
    std::uint64_t region_bytes = 0;
    std::uint16_t threads_per_node = 0;
};

// L_ADMIT_OK: the launcher's own quota and the job's members, the launcher first. The slots
// are 0: the launcher gives them out.
struct LAdmitOkPayload {
    std::uint16_t quota = 0;
    std::vector<SegMapMember> members;  // 1 to 8
};

// REDIRECT: the receiver of a request is not the page's home under its own, newer epoch.
struct RedirectPayload {
    PageId page;
    Epoch epoch;  // the sender's
    NodeId home;  // the page's home at that epoch
};

// LEAVE_INTENT and L_LEAVE_INTENT: a member is leaving. L_LEAVE carries the reason alone.
inline constexpr std::uint8_t kLeaveAsked = 1;      // pm leave
inline constexpr std::uint8_t kLeaveInterrupt = 2;  // Ctrl+C on pmd
struct LeaveIntentPayload {
    NodeId node;
    std::uint8_t reason = kLeaveAsked;
};
// L_CHUNK: one finished chunk, for the ledger (M5-1). cpu_ns is 0 unless `node` is this node.
struct LChunkPayload {
    NodeId node;
    std::uint64_t task_id = 0;
    std::uint64_t indexes = 0;
    std::uint64_t cpu_ns = 0;
};

// L_SEGMENTS: how many of the job's segments each member is home for at an epoch (M5-1).
struct LSegmentsEntry {
    NodeId node;
    std::uint16_t segments = 0;
};
struct LSegmentsPayload {
    Epoch epoch;
    std::vector<LSegmentsEntry> entries;  // 1 to 8
};

// L_STATS: a job process's counts and the pages it holds, once a second (M6-1).
inline constexpr std::size_t kLatencyBuckets = 16;  // count i: below 2^i us; the last: the rest
struct LStatsSegment {
    SegmentId segment;
    NodeId home;
    std::uint16_t shared = 0;
    std::uint16_t modified = 0;
};
struct LStatsPayload {
    std::uint64_t faults = 0;
    std::uint64_t pages_in = 0;
    std::uint64_t pages_out = 0;
    std::uint64_t retries = 0;
    std::array<std::uint32_t, kLatencyBuckets> latency{};
    std::vector<LStatsSegment> segments;  // 0 to 2,048
};

// L_MEMBER: a node spawned for a running job (M4-7), one member entry with slot 0.
struct LMemberPayload {
    SegMapMember member;
};
struct LLeavePayload {
    std::uint8_t reason = kLeaveAsked;
};

// SEG_MIGRATE: part of one segment's directory and pages, from a leaving home to the new one.
struct SegMigrateEntry {
    std::uint16_t page = 0;  // index within the segment, 0 to 511
    std::uint8_t state = 0;  // 0 uncached, 1 shared, 2 exclusive
    std::uint8_t where = 0;  // 0 never written; 1 data follows; 2 the owner has the only copy
    NodeId owner;            // when exclusive
    std::uint32_t version = 0;
    std::uint64_t copyset = 0;
    std::span<const std::byte> data;  // kPageSize bytes when `where` is 1, else none
};
inline constexpr std::size_t kMaxMigrateEntries = 254;
struct SegMigratePayload {
    SegmentId segment;
    bool last = false;  // the final frame for this segment
    std::vector<SegMigrateEntry> entries;
};
struct SegMigrateDonePayload {
    SegmentId segment;
};
// LEAVE_DONE: how many segments the leaver migrated, for the log.
struct LeaveDonePayload {
    std::uint32_t segments = 0;
};
// L_LEAVE_REQ, from `pm leave`. L_LEAVE_REPLY has the layout of SPAWN_DECLINE.
struct LLeaveReqPayload {
    bool force = false;
};

// L_REGISTER: the first frame a job process sends its pmd.
struct LRegisterPayload {
    std::uint32_t pid = 0;
    std::uint8_t role = 0;  // 1 launcher, 2 worker
    std::uint16_t data_port = 0;
    std::array<std::byte, 32> binary_hash{};
};

// L_QUOTA: the most worker threads that may run chunks, and the RAM the process may use.
struct LQuotaPayload {
    std::uint16_t threads = 0;
    std::uint64_t ram_bytes = 0;  // home store and local cache together; 0 for no cap
};

Result<std::size_t> wire_encode(const PageIdPayload& payload, std::span<std::byte> out);
Result<std::size_t> wire_encode(const PagePayload& payload, std::span<std::byte> out);
Result<std::size_t> wire_encode(const JoinJobPayload& payload, std::span<std::byte> out);
Result<std::size_t> wire_encode(const SegMapPayload& payload, std::span<std::byte> out);
Result<std::size_t> wire_encode(const SegMapAckPayload& payload, std::span<std::byte> out);
Result<std::size_t> wire_encode(const JobEndPayload& payload, std::span<std::byte> out);
Result<std::size_t> wire_encode(const SyncPayload& payload, std::span<std::byte> out);
Result<std::size_t> wire_encode(const AtomicOpPayload& payload, std::span<std::byte> out);
Result<std::size_t> wire_encode(const AtomicResultPayload& payload, std::span<std::byte> out);
Result<std::size_t> wire_encode(const TaskReqPayload& payload, std::span<std::byte> out);
Result<std::size_t> wire_encode(const TaskAssignPayload& payload, std::span<std::byte> out);
Result<std::size_t> wire_encode(const NoTaskPayload& payload, std::span<std::byte> out);
Result<std::size_t> wire_encode(const TaskDonePayload& payload, std::span<std::byte> out);
Result<std::size_t> wire_encode(const SpawnReqPayload& payload, std::span<std::byte> out);
Result<std::size_t> wire_encode(const HelloPayload& payload, std::span<std::byte> out);
Result<std::size_t> wire_encode(const ByePayload& payload, std::span<std::byte> out);
Result<std::size_t> wire_encode(const SpawnOkPayload& payload, std::span<std::byte> out);
Result<std::size_t> wire_encode(const SpawnDeclinePayload& payload, std::span<std::byte> out);
Result<std::size_t> wire_encode(const LRegisterPayload& payload, std::span<std::byte> out);
Result<std::size_t> wire_encode(const LQuotaPayload& payload, std::span<std::byte> out);
Result<std::size_t> wire_encode(const LRunReqPayload& payload, std::span<std::byte> out);
Result<std::size_t> wire_encode(const LRunOkPayload& payload, std::span<std::byte> out);
Result<std::size_t> wire_encode(const LAdmitReqPayload& payload, std::span<std::byte> out);
Result<std::size_t> wire_encode(const LAdmitOkPayload& payload, std::span<std::byte> out);

Result<PageIdPayload> wire_decode_page_id(std::span<const std::byte> in);
// `flags` are the frame's: with ZERO_PAGE the payload must carry no data, without it 4,096 bytes.
Result<PagePayload> wire_decode_page(std::span<const std::byte> in, std::uint16_t flags);
Result<JoinJobPayload> wire_decode_join_job(std::span<const std::byte> in);
Result<SegMapPayload> wire_decode_seg_map(std::span<const std::byte> in);
Result<SegMapAckPayload> wire_decode_seg_map_ack(std::span<const std::byte> in);
Result<JobEndPayload> wire_decode_job_end(std::span<const std::byte> in);
Result<SyncPayload> wire_decode_sync(std::span<const std::byte> in);
Result<AtomicOpPayload> wire_decode_atomic_op(std::span<const std::byte> in);
Result<AtomicResultPayload> wire_decode_atomic_result(std::span<const std::byte> in);
Result<TaskReqPayload> wire_decode_task_req(std::span<const std::byte> in);
Result<TaskAssignPayload> wire_decode_task_assign(std::span<const std::byte> in);
Result<NoTaskPayload> wire_decode_no_task(std::span<const std::byte> in);
Result<TaskDonePayload> wire_decode_task_done(std::span<const std::byte> in);
Result<SpawnReqPayload> wire_decode_spawn_req(std::span<const std::byte> in);
Result<HelloPayload> wire_decode_hello(std::span<const std::byte> in);
Result<ByePayload> wire_decode_bye(std::span<const std::byte> in);
Result<SpawnOkPayload> wire_decode_spawn_ok(std::span<const std::byte> in);
Result<SpawnDeclinePayload> wire_decode_spawn_decline(std::span<const std::byte> in);
Result<std::size_t> wire_encode(const LAdmitRefusedPayload& payload, std::span<std::byte> out);
Result<std::size_t> wire_encode(const RedirectPayload& payload, std::span<std::byte> out);
Result<std::size_t> wire_encode(const LeaveIntentPayload& payload, std::span<std::byte> out);
Result<std::size_t> wire_encode(const LLeavePayload& payload, std::span<std::byte> out);
Result<std::size_t> wire_encode(const SegMigratePayload& payload, std::span<std::byte> out);
Result<std::size_t> wire_encode(const SegMigrateDonePayload& payload, std::span<std::byte> out);
Result<std::size_t> wire_encode(const LeaveDonePayload& payload, std::span<std::byte> out);
Result<std::size_t> wire_encode(const LLeaveReqPayload& payload, std::span<std::byte> out);
Result<RedirectPayload> wire_decode_redirect(std::span<const std::byte> in);
Result<LeaveIntentPayload> wire_decode_leave_intent(std::span<const std::byte> in);
Result<std::size_t> wire_encode(const LMemberPayload& payload, std::span<std::byte> out);
Result<std::size_t> wire_encode(const LStatsPayload& payload, std::span<std::byte> out);
Result<LStatsPayload> wire_decode_l_stats(std::span<const std::byte> in);
Result<std::size_t> wire_encode(const LedgerSyncPayload& payload, std::span<std::byte> out);
Result<LedgerSyncPayload> wire_decode_ledger_sync(std::span<const std::byte> in);
Result<std::size_t> wire_encode(const LChunkPayload& payload, std::span<std::byte> out);
Result<LChunkPayload> wire_decode_l_chunk(std::span<const std::byte> in);
Result<std::size_t> wire_encode(const LSegmentsPayload& payload, std::span<std::byte> out);
Result<LSegmentsPayload> wire_decode_l_segments(std::span<const std::byte> in);
Result<LMemberPayload> wire_decode_l_member(std::span<const std::byte> in);
Result<LLeavePayload> wire_decode_l_leave(std::span<const std::byte> in);
// The entries' data are views of `in`.
Result<SegMigratePayload> wire_decode_seg_migrate(std::span<const std::byte> in);
Result<SegMigrateDonePayload> wire_decode_seg_migrate_done(std::span<const std::byte> in);
Result<LeaveDonePayload> wire_decode_leave_done(std::span<const std::byte> in);
Result<LLeaveReqPayload> wire_decode_l_leave_req(std::span<const std::byte> in);
Result<LAdmitRefusedPayload> wire_decode_l_admit_refused(std::span<const std::byte> in);
Result<LRegisterPayload> wire_decode_l_register(std::span<const std::byte> in);
Result<LQuotaPayload> wire_decode_l_quota(std::span<const std::byte> in);
Result<LRunReqPayload> wire_decode_l_run_req(std::span<const std::byte> in);
Result<LRunOkPayload> wire_decode_l_run_ok(std::span<const std::byte> in);
Result<LAdmitReqPayload> wire_decode_l_admit_req(std::span<const std::byte> in);
Result<LAdmitOkPayload> wire_decode_l_admit_ok(std::span<const std::byte> in);

}  // namespace paramesh

#endif  // PARAMESH_WIRE_PAYLOADS_H
