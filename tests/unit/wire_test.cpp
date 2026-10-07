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
