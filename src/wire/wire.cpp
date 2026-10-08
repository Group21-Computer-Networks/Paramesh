// The frame header and the M1 payloads of docs/PROTOCOL.md, to and from bytes. Every integer
// is big-endian and nothing is aligned, so values are built and read a byte at a time.

#include "wire/frame.h"
#include "wire/payloads.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

namespace paramesh {
namespace {

constexpr std::size_t kMaxMembers = 8;
constexpr std::size_t kMaxSegments = 2048;
constexpr std::size_t kMaxMessage = 512;
constexpr std::size_t kMemberSize = 20;

constexpr Error kMalformed{Errc::kProtocol, 0, "decode a payload: it breaks docs/PROTOCOL.md"};
constexpr Error kUnsendable{Errc::kInvalidArgument, 0,
                            "encode a payload: bad value or buffer too small"};

// Every opcode of docs/PROTOCOL.md, section 4.
constexpr std::array kOpcodes = {
    Opcode::kHello,         Opcode::kBye,          Opcode::kSpawnReq,     Opcode::kSpawnOk,
    Opcode::kLedgerSync,    Opcode::kLeaveIntent,  Opcode::kSpawnDecline, Opcode::kJoinJob,
    Opcode::kSegMap,        Opcode::kHeartbeat,    Opcode::kJobEnd,       Opcode::kSegMapAck,
    Opcode::kReadReq,       Opcode::kWriteReq,     Opcode::kUpgradeReq,   Opcode::kReadData,
    Opcode::kWriteGrant,    Opcode::kUpgradeGrant, Opcode::kRedirect,     Opcode::kBusyRetry,
    Opcode::kInv,           Opcode::kInvAck,       Opcode::kFetch,        Opcode::kFetchInv,
    Opcode::kFetchData,     Opcode::kWriteback,    Opcode::kWritebackAck, Opcode::kTaskReq,
    Opcode::kTaskAssign,    Opcode::kNoTask,       Opcode::kTaskDone,     Opcode::kLockAcq,
    Opcode::kLockGrant,     Opcode::kLockRel,      Opcode::kBarrierEnter, Opcode::kBarrierRelease,
    Opcode::kAtomicOp,      Opcode::kAtomicResult, Opcode::kSegMigrate,   Opcode::kSegMigrateDone,
    Opcode::kLeaveDone,     Opcode::kLRegister,    Opcode::kLAdmitReq,    Opcode::kLAdmitOk,
    Opcode::kLAdmitRefused, Opcode::kLChunk,       Opcode::kLSegments,    Opcode::kLJobEnd,
    Opcode::kLQuota,        Opcode::kLLeave,       Opcode::kLAbort,       Opcode::kLLeaveIntent,
    Opcode::kLMember,       Opcode::kLRunReq,      Opcode::kLRunOk,       Opcode::kLRunRefused,
    Opcode::kLLeaveReq,     Opcode::kLLeaveReply,
};

// Appends big-endian values to a buffer. Writing past the end is remembered, not done.
class Writer {
public:
    explicit Writer(std::span<std::byte> out) noexcept : out_(out) {}

    void put(std::uint64_t value, std::size_t width) noexcept {
        if (!room(width)) {
            return;
        }
        for (std::size_t i = 0; i < width; i++) {
            out_[at_ + i] = static_cast<std::byte>(value >> (8 * (width - 1 - i)));
        }
        at_ += width;
    }
    void bytes(std::span<const std::byte> data) noexcept {
        if (room(data.size())) {
            std::copy(data.begin(), data.end(), out_.begin() + static_cast<std::ptrdiff_t>(at_));
            at_ += data.size();
        }
    }
    // The bytes written, or the error if something did not fit.
    [[nodiscard]] Result<std::size_t> done() const noexcept {
        if (!ok_) {
            return kUnsendable;
        }
        return at_;
    }

private:
    bool room(std::size_t width) noexcept {
        ok_ = ok_ && width <= out_.size() - at_;
        return ok_;
    }
    std::span<std::byte> out_;
    std::size_t at_ = 0;
    bool ok_ = true;
};

// Takes big-endian values from a buffer. Reading past the end is remembered and yields zeros.
class Reader {
public:
    explicit Reader(std::span<const std::byte> in) noexcept : in_(in) {}

    std::uint64_t get(std::size_t width) noexcept {
        std::uint64_t value = 0;
        for (const std::byte b : take(width)) {
            value = (value << 8U) | std::to_integer<std::uint64_t>(b);
        }
        return value;
    }
    std::span<const std::byte> take(std::size_t width) noexcept {
        if (width > in_.size() - at_) {
            ok_ = false;
            return {};
        }
        at_ += width;
        return in_.subspan(at_ - width, width);
    }
    // True if every read fitted and nothing is left over.
    [[nodiscard]] bool exact() const noexcept { return ok_ && at_ == in_.size(); }

private:
    std::span<const std::byte> in_;
    std::size_t at_ = 0;
    bool ok_ = true;
};

}  // namespace

// ------------------------------------------------------------------------------- header

void wire_encode_header(const FrameHeader& header, std::span<std::byte, kFrameHeaderSize> out) {
    Writer w{out};
    w.put(kFrameMagic, 4);
    w.put(kProtocolVersion, 1);
    w.put(static_cast<std::uint8_t>(header.opcode), 1);
    w.put(header.flags, 2);
    w.put(header.job.value, 4);
    w.put(header.src.value, 2);
    w.put(header.dst.value, 2);
    w.put(header.req.value, 8);
    w.put(header.epoch.value, 4);
    w.put(header.payload_len, 4);
    w.put(header.payload_crc, 4);
}

Result<FrameHeader> wire_decode_header(std::span<const std::byte, kFrameHeaderSize> in) {
    Reader r{in};
    const std::uint64_t magic = r.get(4);
    const std::uint64_t version = r.get(1);
    FrameHeader header;
    header.opcode = static_cast<Opcode>(r.get(1));
    header.flags = static_cast<std::uint16_t>(r.get(2));
    header.job = JobId{static_cast<std::uint32_t>(r.get(4))};
    header.src = NodeId{static_cast<std::uint16_t>(r.get(2))};
    header.dst = NodeId{static_cast<std::uint16_t>(r.get(2))};
    header.req = ReqId{r.get(8)};
    header.epoch = Epoch{static_cast<std::uint32_t>(r.get(4))};
    header.payload_len = static_cast<std::uint32_t>(r.get(4));
    header.payload_crc = static_cast<std::uint32_t>(r.get(4));

    if (magic != kFrameMagic) {
        return Error{Errc::kProtocol, 0, "decode a frame header: wrong magic"};
    }
    if (version != kProtocolVersion) {
        return Error{Errc::kProtocol, 0, "decode a frame header: unknown protocol version"};
    }
    if (std::find(kOpcodes.begin(), kOpcodes.end(), header.opcode) == kOpcodes.end()) {
        return Error{Errc::kProtocol, 0, "decode a frame header: unknown opcode"};
    }
    if (header.payload_len > kMaxPayload) {
        return Error{Errc::kProtocol, 0, "decode a frame header: payload too long"};
    }
    return header;
}

std::uint32_t wire_payload_crc(const Checksum& checksum, std::span<const std::byte> payload) {
    return payload.empty() ? 0 : checksum.crc32c(payload);
}

Result<void> wire_check_payload(const Checksum& checksum, const FrameHeader& header,
                                std::span<const std::byte> payload) {
    if (payload.size() != header.payload_len) {
        return Error{Errc::kProtocol, 0, "check a payload: its length is not the header's"};
    }
    if (wire_payload_crc(checksum, payload) != header.payload_crc) {
        return Error{Errc::kProtocol, 0, "check a payload: its CRC-32C is not the header's"};
    }
    return {};
}

// ------------------------------------------------------------------------------ payloads

Result<std::size_t> wire_encode(const PageIdPayload& payload, std::span<std::byte> out) {
    Writer w{out};
    w.put(payload.page.value, 8);
    return w.done();
}

Result<PageIdPayload> wire_decode_page_id(std::span<const std::byte> in) {
    Reader r{in};
    const PageIdPayload payload{PageId{r.get(8)}};
    if (!r.exact()) {
        return kMalformed;
    }
    return payload;
}

Result<std::size_t> wire_encode(const PagePayload& payload, std::span<std::byte> out) {
    if (!payload.data.empty() && payload.data.size() != kPageSize) {
        return kUnsendable;
    }
    Writer w{out};
    w.put(payload.page.value, 8);
    w.bytes(payload.data);
    return w.done();
}

Result<PagePayload> wire_decode_page(std::span<const std::byte> in, std::uint16_t flags) {
    Reader r{in};
    PagePayload payload;
    payload.page = PageId{r.get(8)};
    payload.data = r.take((flags & kFlagZeroPage) != 0 ? 0 : kPageSize);
    if (!r.exact()) {
        return kMalformed;
    }
    return payload;
}

Result<std::size_t> wire_encode(const JoinJobPayload& payload, std::span<std::byte> out) {
    if (payload.role != 1 && payload.role != 2) {
        return kUnsendable;
    }
    Writer w{out};
    w.put(payload.role, 1);
    w.put(0, 1);
    w.put(payload.listen_port, 2);
    w.put(payload.pid, 4);
    w.bytes(payload.binary_hash);
    return w.done();
}

Result<JoinJobPayload> wire_decode_join_job(std::span<const std::byte> in) {
    Reader r{in};
    JoinJobPayload payload;
    payload.role = static_cast<std::uint8_t>(r.get(1));
    r.get(1);
    payload.listen_port = static_cast<std::uint16_t>(r.get(2));
    payload.pid = static_cast<std::uint32_t>(r.get(4));
    const std::span<const std::byte> hash = r.take(payload.binary_hash.size());
    if (!r.exact() || (payload.role != 1 && payload.role != 2)) {
        return kMalformed;
    }
    std::copy(hash.begin(), hash.end(), payload.binary_hash.begin());
    return payload;
}

namespace {

// A map is sendable, and acceptable, when it has 1 to 8 members with distinct slots below 64,
// 1 to 2,048 segments, and every segment's home is the slot of a member.
bool valid(const SegMapPayload& map) noexcept {
    if (map.members.empty() || map.members.size() > kMaxMembers || map.homes.empty() ||
        map.homes.size() > kMaxSegments) {
        return false;
    }
    std::uint64_t slots = 0;
    for (const SegMapMember& member : map.members) {
        if (member.slot.value >= kMaxSlots || (slots >> member.slot.value & 1U) != 0) {
            return false;
        }
        slots |= std::uint64_t{1} << member.slot.value;
    }
    return std::all_of(map.homes.begin(), map.homes.end(), [slots](Slot home) {
        return home.value < kMaxSlots && (slots >> home.value & 1U) != 0;
    });
}

}  // namespace

Result<std::size_t> wire_encode(const SegMapPayload& payload, std::span<std::byte> out) {
    if (!valid(payload)) {
        return kUnsendable;
    }
    Writer w{out};
    w.put(payload.homes.size(), 2);
    w.put(payload.members.size(), 1);
    w.put(0, 1);
    for (const SegMapMember& member : payload.members) {
        w.put(member.node.value, 2);
        w.put(member.slot.value, 1);
        w.put(member.flags, 1);
        w.put(member.addr, 4);
        w.put(member.port, 2);
        w.put(0, 2);
        w.put(member.ram_weight, 8);
    }
    for (const Slot home : payload.homes) {
        w.put(home.value, 1);
    }
    return w.done();
}

Result<SegMapPayload> wire_decode_seg_map(std::span<const std::byte> in) {
    Reader r{in};
    const std::size_t segments = r.get(2);
    const std::size_t members = r.get(1);
    r.get(1);
    // The counts are checked against the length before anything is allocated for them.
    if (members > kMaxMembers || segments > kMaxSegments ||
        in.size() != 4 + kMemberSize * members + segments) {
        return kMalformed;
    }
    SegMapPayload payload;
    for (std::size_t i = 0; i < members; i++) {
        SegMapMember member;
        member.node = NodeId{static_cast<std::uint16_t>(r.get(2))};
        member.slot = Slot{static_cast<std::uint8_t>(r.get(1))};
        member.flags = static_cast<std::uint8_t>(r.get(1));
        member.addr = static_cast<std::uint32_t>(r.get(4));
        member.port = static_cast<std::uint16_t>(r.get(2));
        r.get(2);
        member.ram_weight = r.get(8);
        payload.members.push_back(member);
    }
    for (std::size_t i = 0; i < segments; i++) {
        payload.homes.push_back(Slot{static_cast<std::uint8_t>(r.get(1))});
    }
    if (!r.exact() || !valid(payload)) {
        return kMalformed;
    }
    return payload;
}

Result<std::size_t> wire_encode(const SegMapAckPayload& payload, std::span<std::byte> out) {
    Writer w{out};
    w.put(payload.epoch.value, 4);
    return w.done();
}

Result<SegMapAckPayload> wire_decode_seg_map_ack(std::span<const std::byte> in) {
    Reader r{in};
    const SegMapAckPayload payload{Epoch{static_cast<std::uint32_t>(r.get(4))}};
    if (!r.exact()) {
        return kMalformed;
    }
    return payload;
}

Result<std::size_t> wire_encode(const JobEndPayload& payload, std::span<std::byte> out) {
    if (payload.message.size() > kMaxMessage) {
        return kUnsendable;
    }
    Writer w{out};
    w.put(static_cast<std::uint16_t>(payload.status), 2);
    w.put(payload.node.value, 2);
    w.put(payload.message.size(), 2);
    w.bytes(std::as_bytes(std::span{payload.message}));
    return w.done();
}

Result<JobEndPayload> wire_decode_job_end(std::span<const std::byte> in) {
    Reader r{in};
    JobEndPayload payload;
    const std::uint64_t status = r.get(2);
    payload.node = NodeId{static_cast<std::uint16_t>(r.get(2))};
    const std::size_t length = r.get(2);
    const std::span<const std::byte> text = r.take(length);
    if (!r.exact() || length > kMaxMessage ||
        status > static_cast<std::uint64_t>(Status::kStartTimeout)) {
        return kMalformed;
    }
    payload.status = static_cast<Status>(status);
    payload.message.resize(text.size());
    std::transform(text.begin(), text.end(), payload.message.begin(),
                   [](std::byte b) { return std::to_integer<char>(b); });
    return payload;
}

Result<std::size_t> wire_encode(const SyncPayload& payload, std::span<std::byte> out) {
    Writer w{out};
    w.put(payload.id, 8);
    w.put(payload.word, 2);
    w.put(0, 6);  // reserved
    return w.done();
}

Result<SyncPayload> wire_decode_sync(std::span<const std::byte> in) {
    Reader r{in};
    SyncPayload payload;
    payload.id = r.get(8);
    payload.word = static_cast<std::uint16_t>(r.get(2));
    r.take(6);  // reserved: not checked
    if (!r.exact()) {
        return kMalformed;
    }
    return payload;
}

Result<std::size_t> wire_encode(const AtomicOpPayload& payload, std::span<std::byte> out) {
    if (payload.offset % 8 != 0) {
        return kUnsendable;
    }
    Writer w{out};
    w.put(payload.offset, 8);
    w.put(payload.operand, 8);
    w.put(1, 1);  // op: add
    w.put(0, 7);  // reserved
    return w.done();
}

Result<AtomicOpPayload> wire_decode_atomic_op(std::span<const std::byte> in) {
    Reader r{in};
    AtomicOpPayload payload;
    payload.offset = r.get(8);
    payload.operand = r.get(8);
    const std::uint64_t op = r.get(1);
    r.take(7);  // reserved: not checked
    if (!r.exact() || op != 1 || payload.offset % 8 != 0) {
        return kMalformed;
    }
    return payload;
}

Result<std::size_t> wire_encode(const AtomicResultPayload& payload, std::span<std::byte> out) {
    Writer w{out};
    w.put(payload.offset, 8);
    w.put(payload.old, 8);
    return w.done();
}

Result<AtomicResultPayload> wire_decode_atomic_result(std::span<const std::byte> in) {
    Reader r{in};
    AtomicResultPayload payload;
    payload.offset = r.get(8);
    payload.old = r.get(8);
    if (!r.exact()) {
        return kMalformed;
    }
    return payload;
}

}  // namespace paramesh
