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

}  // namespace paramesh

#endif  // PARAMESH_WIRE_PAYLOADS_H
