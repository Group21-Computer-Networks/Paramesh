// src/wire/: the frame header and the M1 payloads against docs/PROTOCOL.md. Fixed byte
// sequences written out by hand from the document, round trips, and rejection of anything
// malformed or corrupted.

#include "platform/factory.h"
#include "wire/frame.h"
#include "wire/payloads.h"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using paramesh::Errc;
using Bytes = std::vector<std::byte>;

// "50 4D 53 48" -> four bytes.
Bytes hex(std::string_view text) {
    Bytes out;
    for (std::size_t i = 0; i + 1 < text.size(); i += 3) {
        out.push_back(
            static_cast<std::byte>(std::stoi(std::string{text.substr(i, 2)}, nullptr, 16)));
    }
    return out;
}

Bytes text_bytes(std::string_view text) {
    const auto view = std::as_bytes(std::span{text});
    return {view.begin(), view.end()};
}

template <typename Payload>
Bytes encoded(const Payload& payload) {
    Bytes out(paramesh::kPageSize + 64);
    const auto size = paramesh::wire_encode(payload, out);
    REQUIRE(size.ok());
    out.resize(size.value());
    return out;
}

// READ_REQ with RETRY, job 0x0007002A, node 3 to node 7, request 0x102, epoch 5, and a
// 9-byte payload whose CRC-32C is the check value E3069283.
constexpr std::string_view kHeaderHex =
    "50 4D 53 48 01 30 00 08 00 07 00 2A 00 03 00 07 "
    "00 00 00 00 00 00 01 02 00 00 00 05 00 00 00 09 E3 06 92 83";

paramesh::FrameHeader decoded_header(const Bytes& bytes) {
    REQUIRE(bytes.size() == paramesh::kFrameHeaderSize);
    const auto header =
        paramesh::wire_decode_header(std::span<const std::byte, paramesh::kFrameHeaderSize>{bytes});
    REQUIRE(header.ok());
    return header.value();
}

Errc header_error(Bytes bytes, std::size_t offset, std::uint8_t value) {
    bytes.at(offset) = std::byte{value};
    const auto header =
        paramesh::wire_decode_header(std::span<const std::byte, paramesh::kFrameHeaderSize>{bytes});
    REQUIRE_FALSE(header.ok());
    return header.error().code;
}

paramesh::SegMapPayload two_member_map() {
    paramesh::SegMapPayload map;
    map.members.push_back({paramesh::NodeId{3}, paramesh::Slot{0}, paramesh::kMemberLauncher,
                           0xC0A8010A, 47100, 2ULL << 30U});
    map.members.push_back(
        {paramesh::NodeId{7}, paramesh::Slot{1}, 0, 0xC0A8010B, 47101, 4ULL << 30U});
    map.homes = {paramesh::Slot{0}, paramesh::Slot{1}, paramesh::Slot{0}};
    return map;
}

}  // namespace

TEST_CASE(
    "a header written out from the protocol document decodes to its fields and encodes back") {
    const Bytes bytes = hex(kHeaderHex);
    const paramesh::FrameHeader header = decoded_header(bytes);
    CHECK(header.opcode == paramesh::Opcode::kReadReq);
    CHECK(header.flags == paramesh::kFlagRetry);
    CHECK(header.job == paramesh::JobId{0x0007002A});
    CHECK(header.src == paramesh::NodeId{3});
    CHECK(header.dst == paramesh::NodeId{7});
    CHECK(header.req == paramesh::ReqId{0x102});
    CHECK(header.epoch == paramesh::Epoch{5});
    CHECK(header.payload_len == 9);
    CHECK(header.payload_crc == 0xE3069283U);

    std::array<std::byte, paramesh::kFrameHeaderSize> out{};
    paramesh::wire_encode_header(header, out);
    CHECK(Bytes{out.begin(), out.end()} == bytes);
}

TEST_CASE("a header with a wrong magic, version, opcode or length is rejected") {
    const Bytes good = hex(kHeaderHex);
    CHECK(header_error(good, 0, 0x51) == Errc::kProtocol);   // magic
    CHECK(header_error(good, 4, 0x02) == Errc::kProtocol);   // version
    CHECK(header_error(good, 5, 0x33) == Errc::kProtocol);   // no opcode 0x33
    CHECK(header_error(good, 29, 0x10) == Errc::kProtocol);  // payload_len 0x100009, over 1 MiB
    CHECK(decoded_header(good).payload_len == 9);            // the unchanged header is still fine
}

TEST_CASE("a corrupted payload is rejected") {
    const auto platform = paramesh::platform_open({}, {});
    REQUIRE(platform.ok());
    const paramesh::Checksum& crc = *platform.value().checksum;
    const paramesh::FrameHeader header = decoded_header(hex(kHeaderHex));
    Bytes payload = text_bytes("123456789");

    CHECK(paramesh::wire_payload_crc(crc, payload) == 0xE3069283U);
    CHECK(paramesh::wire_check_payload(crc, header, payload).ok());

    payload.at(4) ^= std::byte{0x01};  // one flipped bit
    CHECK(paramesh::wire_check_payload(crc, header, payload).error().code == Errc::kProtocol);
    payload.at(4) ^= std::byte{0x01};
    payload.pop_back();  // one byte short
    CHECK(paramesh::wire_check_payload(crc, header, payload).error().code == Errc::kProtocol);

    const paramesh::FrameHeader empty;  // no payload: length 0 and CRC 0
    CHECK(paramesh::wire_check_payload(crc, empty, {}).ok());
}

TEST_CASE("payloads written out from the protocol document decode to their fields") {
    const auto page_id = paramesh::wire_decode_page_id(hex("00 00 00 00 00 0A BC DE"));
    REQUIRE(page_id.ok());
    CHECK(page_id.value().page == paramesh::PageId{0xABCDE});

    const auto ack = paramesh::wire_decode_seg_map_ack(hex("00 00 00 02"));
    REQUIRE(ack.ok());
    CHECK(ack.value().epoch == paramesh::Epoch{2});

    Bytes end_bytes = hex("00 02 00 07 00 0B");
    const Bytes message = text_bytes("node 7 lost");
    end_bytes.insert(end_bytes.end(), message.begin(), message.end());
    const auto end = paramesh::wire_decode_job_end(end_bytes);
    REQUIRE(end.ok());
    CHECK(end.value().status == paramesh::Status::kNodeLost);
    CHECK(end.value().node == paramesh::NodeId{7});
    CHECK(end.value().message == "node 7 lost");

    // Three segments, two members (node 3 at 192.168.1.10:47100 with 2 GiB, the launcher; node 7
    // at 192.168.1.11:47101 with 4 GiB), homes 0, 1, 0.
    const Bytes map_bytes =
        hex("00 03 02 00 "
            "00 03 00 01 C0 A8 01 0A B7 FC 00 00 00 00 00 00 80 00 00 00 "
            "00 07 01 00 C0 A8 01 0B B7 FD 00 00 00 00 00 01 00 00 00 00 "
            "00 01 00");
    const auto map = paramesh::wire_decode_seg_map(map_bytes);
    REQUIRE(map.ok());
    REQUIRE(map.value().members.size() == 2);
    CHECK(map.value().members[0].flags == paramesh::kMemberLauncher);
    CHECK(map.value().members[1].node == paramesh::NodeId{7});
    CHECK(map.value().members[1].addr == 0xC0A8010BU);
    CHECK(map.value().members[1].port == 47101);
    CHECK(map.value().members[1].ram_weight == 4ULL << 30U);
    CHECK(map.value().homes ==
          std::vector{paramesh::Slot{0}, paramesh::Slot{1}, paramesh::Slot{0}});
    CHECK(encoded(two_member_map()) == map_bytes);
}

TEST_CASE("every M1 payload survives a round trip") {
    CHECK(paramesh::wire_decode_page_id(encoded(paramesh::PageIdPayload{paramesh::PageId{1048575}}))
              .value()
              .page == paramesh::PageId{1048575});

    std::array<std::byte, paramesh::kPageSize> page{};
    for (std::size_t i = 0; i < page.size(); i++) {
        page.at(i) = static_cast<std::byte>(i * 7);
    }
    const Bytes with_data = encoded(paramesh::PagePayload{paramesh::PageId{9}, page});
    CHECK(with_data.size() == 4104);
    const auto full = paramesh::wire_decode_page(with_data, paramesh::kFlagReadOnly);
    REQUIRE(full.ok());
    CHECK(full.value().page == paramesh::PageId{9});
    CHECK(std::equal(page.begin(), page.end(), full.value().data.begin(), full.value().data.end()));

    const Bytes zero = encoded(paramesh::PagePayload{paramesh::PageId{9}, {}});
    CHECK(zero.size() == 8);
    const auto none = paramesh::wire_decode_page(zero, paramesh::kFlagZeroPage);
    REQUIRE(none.ok());
    CHECK(none.value().data.empty());

    paramesh::JoinJobPayload join;
    join.role = 2;
    join.listen_port = 47123;
    join.pid = 4242;
    join.binary_hash.fill(std::byte{0xAB});
    const Bytes join_bytes = encoded(join);
    CHECK(join_bytes.size() == 40);
    const auto join_back = paramesh::wire_decode_join_job(join_bytes);
    REQUIRE(join_back.ok());
    CHECK(join_back.value().listen_port == 47123);
    CHECK(join_back.value().pid == 4242);
    CHECK(join_back.value().binary_hash == join.binary_hash);

    const auto end = paramesh::wire_decode_job_end(
        encoded(paramesh::JobEndPayload{paramesh::Status::kOk, {}, ""}));
    REQUIRE(end.ok());
    CHECK(end.value().message.empty());
}

TEST_CASE("payloads that break the protocol are rejected, and unsendable ones are not encoded") {
    CHECK(paramesh::wire_decode_page_id(hex("00 00 00 00 00 00 00")).error().code ==
          Errc::kProtocol);  // 7 bytes
    CHECK(paramesh::wire_decode_page_id(hex("00 00 00 00 00 00 00 01 00")).error().code ==
          Errc::kProtocol);  // 9 bytes

    // A page frame must carry data exactly when ZERO_PAGE is clear.
    const Bytes id_only = hex("00 00 00 00 00 00 00 09");
    CHECK(paramesh::wire_decode_page(id_only, 0).error().code == Errc::kProtocol);
    std::array<std::byte, paramesh::kPageSize> page{};
    const Bytes with_data = encoded(paramesh::PagePayload{paramesh::PageId{9}, page});
    CHECK(paramesh::wire_decode_page(with_data, paramesh::kFlagZeroPage).error().code ==
          Errc::kProtocol);

    Bytes join = encoded(paramesh::JoinJobPayload{1, 1, 1, {}});
    join.at(0) = std::byte{3};  // no such role
    CHECK(paramesh::wire_decode_join_job(join).error().code == Errc::kProtocol);

    // Segment maps: a home that is no member's slot, no members, nine members, a short payload.
    Bytes map = encoded(two_member_map());
    map.back() = std::byte{5};
    CHECK(paramesh::wire_decode_seg_map(map).error().code == Errc::kProtocol);
    CHECK(paramesh::wire_decode_seg_map(hex("00 01 00 00 00")).error().code == Errc::kProtocol);
    CHECK(paramesh::wire_decode_seg_map(hex("00 01 09 00 00")).error().code == Errc::kProtocol);
    map = encoded(two_member_map());
    map.pop_back();
    CHECK(paramesh::wire_decode_seg_map(map).error().code == Errc::kProtocol);

    // JOB_END: an unknown status, a message longer than its bytes, a message over 512 bytes.
    CHECK(paramesh::wire_decode_job_end(hex("00 63 00 00 00 00")).error().code == Errc::kProtocol);
    CHECK(paramesh::wire_decode_job_end(hex("00 00 00 00 00 05 41 42")).error().code ==
          Errc::kProtocol);

    std::array<std::byte, 64> small{};
    CHECK(paramesh::wire_encode(
              paramesh::JobEndPayload{paramesh::Status::kOk, {}, std::string(513, 'x')}, small)
              .error()
              .code == Errc::kInvalidArgument);
    CHECK(paramesh::wire_encode(paramesh::PagePayload{paramesh::PageId{1}, page}, small)
              .error()
              .code == Errc::kInvalidArgument);  // the buffer is too small
    paramesh::SegMapPayload twice = two_member_map();
    twice.members[1].slot = paramesh::Slot{0};  // two members, one slot
    CHECK(paramesh::wire_encode(twice, small).error().code == Errc::kInvalidArgument);
}

TEST_CASE("the lock and barrier payload: id, one word, six reserved bytes") {
    // LOCK_ACQ for lock 0x1122334455667788 by the launcher's main thread.
    const Bytes acquire = hex("11 22 33 44 55 66 77 88 FF FF 00 00 00 00 00 00");
    const auto decoded = paramesh::wire_decode_sync(acquire);
    REQUIRE(decoded.ok());
    CHECK(decoded.value().id == 0x1122334455667788ULL);
    CHECK(decoded.value().word == 0xFFFF);
    CHECK(encoded(paramesh::SyncPayload{0x1122334455667788ULL, 0xFFFF}) == acquire);

    // LOCK_GRANT with BAD_HANDLE (14); the reserved bytes are not checked.
    const auto grant =
        paramesh::wire_decode_sync(hex("00 00 00 00 00 00 00 05 00 0E 01 02 03 04 05 06"));
    REQUIRE(grant.ok());
    CHECK(grant.value().id == 5);
    CHECK(grant.value().word == static_cast<std::uint16_t>(paramesh::Status::kBadHandle));

    CHECK(paramesh::wire_decode_sync(hex("00 00 00 00 00 00 00 05 00 0E")).error().code ==
          Errc::kProtocol);  // short
    Bytes longer = acquire;
    longer.push_back(std::byte{0});
    CHECK(paramesh::wire_decode_sync(longer).error().code == Errc::kProtocol);
    std::array<std::byte, 15> small{};
    CHECK(paramesh::wire_encode(paramesh::SyncPayload{1, 2}, small).error().code ==
          Errc::kInvalidArgument);
}

TEST_CASE("the atomic payloads: ATOMIC_OP is 24 bytes with op 1, ATOMIC_RESULT 16") {
    // Add 3 to the number 0x2008 bytes into the region.
    const Bytes op = hex("00 00 00 00 00 00 20 08 00 00 00 00 00 00 00 03 01 00 00 00 00 00 00 00");
    const auto decoded = paramesh::wire_decode_atomic_op(op);
    REQUIRE(decoded.ok());
    CHECK(decoded.value().offset == 0x2008);
    CHECK(decoded.value().operand == 3);
    CHECK(encoded(paramesh::AtomicOpPayload{0x2008, 3}) == op);

    Bytes other = op;
    other[16] = std::byte{2};  // an operation that does not exist
    CHECK(paramesh::wire_decode_atomic_op(other).error().code == Errc::kProtocol);
    other = op;
    other[7] = std::byte{0x09};  // not a multiple of 8
    CHECK(paramesh::wire_decode_atomic_op(other).error().code == Errc::kProtocol);
    other = op;
    other.pop_back();
    CHECK(paramesh::wire_decode_atomic_op(other).error().code == Errc::kProtocol);
    std::array<std::byte, 24> room{};
    CHECK(paramesh::wire_encode(paramesh::AtomicOpPayload{0x2009, 3}, room).error().code ==
          Errc::kInvalidArgument);

    const Bytes result = hex("00 00 00 00 00 00 20 08 FF FF FF FF FF FF FF FE");
    const auto old = paramesh::wire_decode_atomic_result(result);
    REQUIRE(old.ok());
    CHECK(old.value().offset == 0x2008);
    CHECK(old.value().old == 0xFFFFFFFFFFFFFFFEULL);
    CHECK(encoded(paramesh::AtomicResultPayload{0x2008, 0xFFFFFFFFFFFFFFFEULL}) == result);
    CHECK(paramesh::wire_decode_atomic_result(op).error().code == Errc::kProtocol);  // 24 bytes
}

TEST_CASE("the task payloads: TASK_REQ, TASK_ASSIGN, NO_TASK and TASK_DONE") {
    const Bytes ask = hex("00 05 00 00");
    REQUIRE(paramesh::wire_decode_task_req(ask).ok());
    CHECK(paramesh::wire_decode_task_req(ask).value().thread == 5);
    CHECK(encoded(paramesh::TaskReqPayload{5}) == ask);
    CHECK(paramesh::wire_decode_task_req(hex("00 05")).error().code == Errc::kProtocol);

    // Chunk 3 of task 0x1122334455667788, indexes 16 to 32, call 2, with a 3-byte argument.
    const Bytes assign =
        hex("00 00 00 00 00 00 00 03 11 22 33 44 55 66 77 88 "
            "00 00 00 00 00 00 00 10 00 00 00 00 00 00 00 20 "
            "00 00 00 02 00 03 00 00 AA BB CC");
    const auto chunk = paramesh::wire_decode_task_assign(assign);
    REQUIRE(chunk.ok());
    CHECK(chunk.value().chunk_id == 3);
    CHECK(chunk.value().task_id == 0x1122334455667788ULL);
    CHECK(chunk.value().lo == 16);
    CHECK(chunk.value().hi == 32);
    CHECK(chunk.value().call_id == 2);
    CHECK(chunk.value().arg == hex("AA BB CC"));
    CHECK(encoded(chunk.value()) == assign);
    Bytes shorter = assign;
    shorter.pop_back();  // the argument is not as long as the payload says
    CHECK(paramesh::wire_decode_task_assign(shorter).error().code == Errc::kProtocol);
    paramesh::TaskAssignPayload big;
    big.arg.resize(paramesh::kMaxTaskArg);
    CHECK(encoded(big).size() == 40 + paramesh::kMaxTaskArg);  // the largest
    big.arg.resize(paramesh::kMaxTaskArg + 1);
    Bytes room(2048);
    CHECK(paramesh::wire_encode(big, room).error().code == Errc::kInvalidArgument);

    CHECK(encoded(paramesh::NoTaskPayload{2}) == hex("02 00 00 00"));
    CHECK(paramesh::wire_decode_no_task(hex("01 00 00 00")).value().reason == 1);
    CHECK(paramesh::wire_decode_no_task(hex("03 00 00 00")).error().code == Errc::kProtocol);
    CHECK(paramesh::wire_encode(paramesh::NoTaskPayload{0}, room).error().code ==
          Errc::kInvalidArgument);

    const Bytes done =
        hex("00 00 00 00 00 00 00 03 00 00 00 00 00 0F 42 40 00 01 00 00 00 00 00 00");
    const auto report = paramesh::wire_decode_task_done(done);
    REQUIRE(report.ok());
    CHECK(report.value().chunk_id == 3);
    CHECK(report.value().cpu_ns == 1000000);
    CHECK(report.value().thread == 1);
    CHECK(encoded(paramesh::TaskDonePayload{3, 1000000, 1}) == done);
    CHECK(paramesh::wire_decode_task_done(ask).error().code == Errc::kProtocol);
}

TEST_CASE("the daemon payloads: SPAWN_REQ, SPAWN_OK, SPAWN_DECLINE, L_REGISTER and L_QUOTA") {
    paramesh::SpawnReqPayload spawn;
    spawn.launcher_node = paramesh::NodeId{3};
    spawn.flags = 1;
    spawn.launcher_addr = 0x0A000001;
    spawn.launcher_port = 5000;
    spawn.threads_per_node = 4;
    spawn.region_bytes = 0x10000000;
    spawn.binary_hash.fill(std::byte{0xAB});
    spawn.path = "/a";
    spawn.cwd = "/b";
    spawn.argv = {"x", ""};
    const Bytes bytes = encoded(spawn);
    // The fixed part, then str16 "/a", str16 "/b", argc 2, str16 "x", str16 "".
    Bytes expected = hex("00 03 00 01 0A 00 00 01 13 88 00 04 00 00 00 00 10 00 00 00");
    expected.insert(expected.end(), 32, std::byte{0xAB});
    const Bytes tail = hex("00 02 2F 61 00 02 2F 62 00 02 00 01 78 00 00");
    expected.insert(expected.end(), tail.begin(), tail.end());
    CHECK(bytes == expected);
    const auto back = paramesh::wire_decode_spawn_req(bytes);
    REQUIRE(back.ok());
    CHECK(back.value().launcher_node == paramesh::NodeId{3});
    CHECK(back.value().launcher_addr == 0x0A000001);
    CHECK(back.value().launcher_port == 5000);
    CHECK(back.value().threads_per_node == 4);
    CHECK(back.value().region_bytes == 0x10000000);
    CHECK(back.value().binary_hash == spawn.binary_hash);
    CHECK(back.value().path == "/a");
    CHECK(back.value().cwd == "/b");
    CHECK(back.value().argv == spawn.argv);
    Bytes cut = bytes;
    cut.pop_back();  // the last argument's length is missing
    CHECK(paramesh::wire_decode_spawn_req(cut).error().code == Errc::kProtocol);
    cut = bytes;
    cut.push_back(std::byte{0});  // a byte too many
    CHECK(paramesh::wire_decode_spawn_req(cut).error().code == Errc::kProtocol);

    const Bytes ok = hex("00 05 10 92 00 03 00 00 00 00 00 00 00 00 00 07 00 00 00 00 00 00 00 09");
    const auto accepted = paramesh::wire_decode_spawn_ok(ok);
    REQUIRE(accepted.ok());
    CHECK(accepted.value().node == paramesh::NodeId{5});
    CHECK(accepted.value().data_port == 4242);
    CHECK(accepted.value().cores == 3);
    CHECK(accepted.value().ram_commit == 7);
    CHECK(accepted.value().spill_commit == 9);
    CHECK(encoded(accepted.value()) == ok);
    CHECK(paramesh::wire_decode_spawn_ok(hex("00 05")).error().code == Errc::kProtocol);

    const Bytes decline = hex("00 05 00 02 6E 6F");  // BINARY_MISMATCH, "no"
    const auto declined = paramesh::wire_decode_spawn_decline(decline);
    REQUIRE(declined.ok());
    CHECK(declined.value().status == paramesh::Status::kBinaryMismatch);
    CHECK(declined.value().message == "no");
    CHECK(encoded(declined.value()) == decline);
    CHECK(paramesh::wire_decode_spawn_decline(hex("00 63 00 00")).error().code ==
          Errc::kProtocol);  // no such status
    Bytes room(1024);
    CHECK(
        paramesh::wire_encode(
            paramesh::SpawnDeclinePayload{paramesh::Status::kInternal, std::string(513, 'x')}, room)
            .error()
            .code == Errc::kInvalidArgument);

    paramesh::LRegisterPayload process;
    process.pid = 0x01020304;
    process.role = 2;
    process.data_port = 4242;
    process.binary_hash.fill(std::byte{0xCD});
    Bytes registered = hex("01 02 03 04 02 00 10 92");
    registered.insert(registered.end(), 32, std::byte{0xCD});
    CHECK(encoded(process) == registered);
    const auto job = paramesh::wire_decode_l_register(registered);
    REQUIRE(job.ok());
    CHECK(job.value().pid == 0x01020304);
    CHECK(job.value().role == 2);
    CHECK(job.value().data_port == 4242);
    registered[4] = std::byte{3};  // neither launcher nor worker
    CHECK(paramesh::wire_decode_l_register(registered).error().code == Errc::kProtocol);

    CHECK(encoded(paramesh::LQuotaPayload{3}) == hex("00 03 00 00"));
    CHECK(paramesh::wire_decode_l_quota(hex("00 03 00 00")).value().threads == 3);
    CHECK(paramesh::wire_decode_l_quota(hex("00 03")).error().code == Errc::kProtocol);
}

TEST_CASE("the launch payloads: L_RUN_REQ, L_RUN_OK, L_ADMIT_REQ and L_ADMIT_OK") {
    paramesh::LRunReqPayload run;
    run.nodes = 3;
    run.binary_hash.fill(std::byte{0xEE});
    run.path = "/a";
    run.cwd = "/b";
    run.argv = {"x"};
    Bytes expected = hex("00 03 00 00");
    expected.insert(expected.end(), 32, std::byte{0xEE});
    const Bytes tail = hex("00 02 2F 61 00 02 2F 62 00 01 00 01 78");
    expected.insert(expected.end(), tail.begin(), tail.end());
    CHECK(encoded(run) == expected);
    const auto back = paramesh::wire_decode_l_run_req(expected);
    REQUIRE(back.ok());
    CHECK(back.value().nodes == 3);
    CHECK(back.value().binary_hash == run.binary_hash);
    CHECK(back.value().path == "/a");
    CHECK(back.value().cwd == "/b");
    CHECK(back.value().argv == run.argv);
    Bytes none = expected;
    none[1] = std::byte{0};  // a job of no nodes
    CHECK(paramesh::wire_decode_l_run_req(none).error().code == Errc::kProtocol);
    none[1] = std::byte{9};  // or of more than eight
    CHECK(paramesh::wire_decode_l_run_req(none).error().code == Errc::kProtocol);
    Bytes room(1024);
    run.nodes = 0;
    CHECK(paramesh::wire_encode(run, room).error().code == Errc::kInvalidArgument);

    const Bytes ok = hex("00 01 00 02 00 01 00 00");  // job 0x00010002 of node 1
    const auto named = paramesh::wire_decode_l_run_ok(ok);
    REQUIRE(named.ok());
    CHECK(named.value().job == paramesh::JobId{0x00010002});
    CHECK(named.value().node == paramesh::NodeId{1});
    CHECK(encoded(named.value()) == ok);

    const Bytes ask = hex("00 00 00 00 00 20 00 00 00 04 00 00 00 00 00 00");
    const auto wants = paramesh::wire_decode_l_admit_req(ask);
    REQUIRE(wants.ok());
    CHECK(wants.value().region_bytes == 0x200000);
    CHECK(wants.value().threads_per_node == 4);
    CHECK(encoded(wants.value()) == ask);
    CHECK(paramesh::wire_decode_l_admit_req(ok).error().code == Errc::kProtocol);  // 8 bytes

    // Quota 3; the launcher (node 1, port 5000) and one worker (node 2 at 10.0.0.2:6000).
    const Bytes admitted =
        hex("00 03 02 00 "
            "00 01 00 01 00 00 00 00 13 88 00 00 00 00 00 00 00 00 00 00 "
            "00 02 00 00 0A 00 00 02 17 70 00 00 00 00 00 00 00 00 00 07");
    const auto members = paramesh::wire_decode_l_admit_ok(admitted);
    REQUIRE(members.ok());
    CHECK(members.value().quota == 3);
    REQUIRE(members.value().members.size() == 2);
    CHECK(members.value().members[0].flags == paramesh::kMemberLauncher);
    CHECK(members.value().members[0].port == 5000);
    CHECK(members.value().members[1].node == paramesh::NodeId{2});
    CHECK(members.value().members[1].addr == 0x0A000002);
    CHECK(members.value().members[1].port == 6000);
    CHECK(members.value().members[1].ram_weight == 7);
    CHECK(encoded(members.value()) == admitted);
    Bytes short_one = admitted;
    short_one.pop_back();
    CHECK(paramesh::wire_decode_l_admit_ok(short_one).error().code == Errc::kProtocol);
    CHECK(paramesh::wire_encode(paramesh::LAdmitOkPayload{}, room).error().code ==
          Errc::kInvalidArgument);  // no members
}
