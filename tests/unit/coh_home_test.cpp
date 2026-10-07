// src/coh/ home machine, M1 subset: the cases the M1-4 card names, row by row as
// docs/STATE_MACHINES.md sections 2.4 and 2.5 list them, and the refusal of section 5.

#include "coh/home_machine.h"

#include <doctest/doctest.h>

#include <vector>

namespace {

using paramesh::HomeAction;
using paramesh::HomeActionKind;
using paramesh::HomeEvent;
using paramesh::HomeEventKind;
using paramesh::HomeState;
using paramesh::HomeWait;
using paramesh::HomeWhere;
using paramesh::NodeId;
using paramesh::Opcode;
using paramesh::PageId;
using paramesh::PageSource;

constexpr PageId kPage{9};

// A directory that is home for segment 0, and a way to send it events. Node n has slot n.
struct Home {
    paramesh::HomeDirectoryPtr directory;
    std::uint64_t next_req = 100;

    Home() {
        auto opened = paramesh::home_open({});
        REQUIRE(opened.ok());
        directory = std::move(opened).value();
        REQUIRE(paramesh::home_add_segment(*directory, paramesh::SegmentId{0}).ok());
    }

    std::vector<HomeAction> send(HomeEventKind kind, std::uint16_t node, PageId page = kPage) {
        HomeEvent event;
        event.kind = kind;
        event.page = page;
        event.from = NodeId{node};
        event.slot = paramesh::Slot{static_cast<std::uint8_t>(node)};
        event.req = paramesh::ReqId{next_req++};
        std::vector<HomeAction> out;
        paramesh::home_step(*directory, event, out);
        return out;
    }

    [[nodiscard]] paramesh::HomeEntryView entry() const {
        return paramesh::home_entry(*directory, kPage);
    }
};

}  // namespace

TEST_CASE("READ_REQ on a page never written is answered with the zero-page flag") {
    Home home;
    const auto out = home.send(HomeEventKind::kReadReq, 2);
    REQUIRE(out.size() == 1);
    CHECK(out[0].kind == HomeActionKind::kReply);
    CHECK(out[0].opcode == Opcode::kReadData);
    CHECK(out[0].to == NodeId{2});
    CHECK(out[0].req == paramesh::ReqId{100});
    CHECK(out[0].source == PageSource::kZero);
    CHECK(out[0].read_only);
    CHECK(home.entry().state == HomeState::kShared);
    CHECK(home.entry().copyset == 0b100);

    // A second reader is added to the copyset.
    CHECK(home.send(HomeEventKind::kReadReq, 3)[0].source == PageSource::kZero);
    CHECK(home.entry().copyset == 0b1100);
}

TEST_CASE("WRITE_REQ on a page nobody else holds is granted") {
    Home home;
    const auto out = home.send(HomeEventKind::kWriteReq, 2);
    REQUIRE(out.size() == 1);
    CHECK(out[0].opcode == Opcode::kWriteGrant);
    CHECK(out[0].source == PageSource::kZero);
    CHECK_FALSE(out[0].read_only);
    const auto entry = home.entry();
    CHECK(entry.state == HomeState::kExclusive);
    CHECK(entry.where == HomeWhere::kNone);
    CHECK(entry.owner == NodeId{2});
    CHECK(entry.copyset == 0b100);
    CHECK(entry.version == 1);
}

TEST_CASE("READ_REQ on a page another node holds for writing fetches it from the owner") {
    Home home;
    home.send(HomeEventKind::kWriteReq, 2);

    const auto fetch = home.send(HomeEventKind::kReadReq, 3);
    REQUIRE(fetch.size() == 1);
    CHECK(fetch[0].kind == HomeActionKind::kSend);
    CHECK(fetch[0].opcode == Opcode::kFetch);
    CHECK(fetch[0].to == NodeId{2});
    CHECK(home.entry().wait == HomeWait::kFetch);

    const auto done = home.send(HomeEventKind::kFetchData, 2);
    REQUIRE(done.size() == 2);
    CHECK(done[0].kind == HomeActionKind::kStore);
    CHECK(done[1].opcode == Opcode::kReadData);
    CHECK(done[1].to == NodeId{3});
    CHECK(done[1].req == paramesh::ReqId{101});  // the reader's request number
    CHECK(done[1].source == PageSource::kReceived);
    CHECK(done[1].read_only);
    const auto entry = home.entry();
    CHECK(entry.state == HomeState::kShared);
    CHECK(entry.where == HomeWhere::kRam);
    CHECK(entry.owner == paramesh::kNoNode);
    CHECK(entry.copyset == 0b1100);  // the old owner and the reader
    CHECK(entry.wait == HomeWait::kIdle);

    // From now on a reader is answered from the home copy.
    const auto later = home.send(HomeEventKind::kReadReq, 4);
    REQUIRE(later.size() == 1);
    CHECK(later[0].source == PageSource::kHomeCopy);
}

TEST_CASE("requests for a busy page wait in order and are served when it is free") {
    Home home;
    home.send(HomeEventKind::kWriteReq, 2);
    home.send(HomeEventKind::kReadReq, 3);

    CHECK(home.send(HomeEventKind::kReadReq, 4).empty());  // queued
    CHECK(home.send(HomeEventKind::kReadReq, 5).empty());
    CHECK(home.entry().queued == 2);

    const auto done = home.send(HomeEventKind::kFetchData, 2);
    REQUIRE(done.size() == 4);  // store, then READ_DATA to 3, 4 and 5 in arrival order
    CHECK(done[1].to == NodeId{3});
    CHECK(done[2].to == NodeId{4});
    CHECK(done[3].to == NodeId{5});
    CHECK(done[2].source == PageSource::kHomeCopy);
    CHECK(home.entry().queued == 0);
    CHECK(home.entry().copyset == 0b111100);

    // Another page of the segment was never busy.
    CHECK(home.send(HomeEventKind::kReadReq, 4, PageId{10}).size() == 1);
}

TEST_CASE("WRITE_REQ from the page's only reader is granted with the home copy") {
    Home home;
    home.send(HomeEventKind::kWriteReq, 2);
    home.send(HomeEventKind::kReadReq, 2, PageId{10});  // page 10: node 2 is its only reader
    const auto out = home.send(HomeEventKind::kWriteReq, 2, PageId{10});
    REQUIRE(out.size() == 1);
    CHECK(out[0].opcode == Opcode::kWriteGrant);
    CHECK(paramesh::home_entry(*home.directory, PageId{10}).state == HomeState::kExclusive);
}

TEST_CASE("any other request ends the job as not supported before M2") {
    const auto refused = [](const std::vector<HomeAction>& out, Opcode request) {
        REQUIRE(out.size() == 1);
        CHECK(out[0].kind == HomeActionKind::kAbort);
        CHECK(out[0].opcode == request);
    };
    Home home;
    refused(home.send(HomeEventKind::kUpgradeReq, 2), Opcode::kUpgradeReq);
    refused(home.send(HomeEventKind::kWriteback, 2), Opcode::kWriteback);
    refused(home.send(HomeEventKind::kAtomicOp, 2), Opcode::kAtomicOp);

    home.send(HomeEventKind::kReadReq, 2);
    home.send(HomeEventKind::kReadReq, 3);
    refused(home.send(HomeEventKind::kWriteReq, 2),
            Opcode::kWriteReq);  // another node reads it: needs INV

    Home held;
    held.send(HomeEventKind::kWriteReq, 2);
    refused(held.send(HomeEventKind::kWriteReq, 3),
            Opcode::kWriteReq);  // another node writes it: needs FETCH_INV
}

TEST_CASE("an impossible event ends the job, marked as impossible") {
    const auto impossible = [](const std::vector<HomeAction>& out) {
        REQUIRE(out.size() == 1);
        CHECK(out[0].kind == HomeActionKind::kAbort);
        CHECK(out[0].opcode == Opcode::kHeartbeat);
    };
    Home home;
    impossible(home.send(HomeEventKind::kFetchData, 2));  // nothing was fetched
    impossible(home.send(HomeEventKind::kReadReq, 2,
                         PageId{paramesh::kPagesPerSegment}));  // not this node's segment
    home.send(HomeEventKind::kWriteReq, 2);
    impossible(home.send(HomeEventKind::kReadReq, 2));  // the owner asks to read its own page
    home.send(HomeEventKind::kReadReq, 3);
    impossible(home.send(HomeEventKind::kFetchData, 4));  // data from a node that is not the owner
}
