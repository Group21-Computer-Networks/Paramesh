// The frame header, opcodes, flags and status codes of docs/PROTOCOL.md. No sockets.
//
// Interface file drafted by M0-7; see docs/INTERNAL_API.md, section 3. M1-1 implements the
// functions and adds one payload struct with an encoder and a decoder per opcode.
#ifndef PARAMESH_WIRE_FRAME_H
#define PARAMESH_WIRE_FRAME_H

#include "platform/checksum.h"
#include "platform/ids.h"
#include "platform/result.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace paramesh {

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

}  // namespace paramesh

#endif  // PARAMESH_WIRE_FRAME_H
