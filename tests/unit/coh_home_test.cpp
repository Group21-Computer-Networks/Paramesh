// src/coh/ home machine: every row of docs/STATE_MACHINES.md sections 2.4 and 2.5, in the
// order the tables list them, and then the hold window of section 2.6.
//
// Actions are compared as text, one word each: "INV>3" (send to node 3), "READ_DATA>3:zero"
// (reply, and where its page comes from), "atomic>3:home", "store", "load", "drop", "abort",
// "arm@15" (ask for HOLD_EXPIRED at 15 ms) and "thrash>3/2" (report: nodes 3 and 2 fight).

#include "coh/home_machine.h"

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
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
    std::chrono::milliseconds now{0};  // the time the next event carries

    explicit Home(const paramesh::HomeConfig& config = {}) {
        auto opened = paramesh::home_open(config);
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
        event.now = now;
        std::vector<HomeAction> out;
        paramesh::home_step(*directory, event, out);
        return out;
    }

    [[nodiscard]] paramesh::HomeEntryView entry() const {
        return paramesh::home_entry(*directory, kPage);
    }

    // The same, with the actions as text.
    std::string does(HomeEventKind kind, std::uint16_t node);
};

std::string name(Opcode opcode) {
    switch (opcode) {
        case Opcode::kReadData:
            return "READ_DATA";
        case Opcode::kWriteGrant:
            return "WRITE_GRANT";
        case Opcode::kUpgradeGrant:
            return "UPGRADE_GRANT";
        case Opcode::kWritebackAck:
            return "WRITEBACK_ACK";
        case Opcode::kInv:
            return "INV";
        case Opcode::kFetch:
            return "FETCH";
        case Opcode::kFetchInv:
            return "FETCH_INV";
        default:
            return "?";
    }
}

std::string text(const std::vector<HomeAction>& actions) {
    static constexpr std::array<const char*, 4> kSources{"", ":zero", ":home", ":received"};
    std::string all;
    for (const HomeAction& a : actions) {
        all += all.empty() ? "" : " ";
        const std::string to = ">" + std::to_string(a.to.value);
        switch (a.kind) {
            case HomeActionKind::kSend:
                all += name(a.opcode) + to;
                break;
            case HomeActionKind::kReply:
                all += name(a.opcode) + to + kSources.at(static_cast<std::size_t>(a.source));
                break;
            case HomeActionKind::kApplyAtomic:
                all += "atomic" + to + kSources.at(static_cast<std::size_t>(a.source));
                break;
            case HomeActionKind::kStore:
                all += "store";
                break;
            case HomeActionKind::kLoad:
                all += "load";
                break;
            case HomeActionKind::kDropCopy:
                all += "drop";
                break;
            case HomeActionKind::kArmHoldTimer:
                all += "arm@" +
                       std::to_string(
                           std::chrono::duration_cast<std::chrono::milliseconds>(a.at).count());
                break;
            case HomeActionKind::kReportThrash:
                all += "thrash" + to + "/" + std::to_string(a.other.value);
                break;
            default:
                all += "abort";
                break;
        }
    }
    return all;
}

std::string Home::does(HomeEventKind kind, std::uint16_t node) {
    return text(send(kind, node));
}

constexpr HomeEventKind kRead = HomeEventKind::kReadReq;
constexpr HomeEventKind kWrite = HomeEventKind::kWriteReq;
constexpr HomeEventKind kUpgrade = HomeEventKind::kUpgradeReq;
constexpr HomeEventKind kWriteback = HomeEventKind::kWriteback;
constexpr HomeEventKind kAtomic = HomeEventKind::kAtomicOp;
constexpr HomeEventKind kInvAck = HomeEventKind::kInvAck;
constexpr HomeEventKind kFetchData = HomeEventKind::kFetchData;
constexpr HomeEventKind kLoaded = HomeEventKind::kLoaded;
constexpr HomeEventKind kHoldExpired = HomeEventKind::kHoldExpired;

// The seven states of section 2.2, each reached the short way.
using Reach = void (*)(Home&);
void uncached(Home& /*home*/) {}
void shared(Home& home) {  // read by nodes 2 and 3
    home.send(kRead, 2);
    home.send(kRead, 3);
}
void exclusive(Home& home) {  // owned by node 2
    home.send(kWrite, 2);
}
void w_inv(Home& home) {  // node 4 writes; nodes 2 and 3 are to acknowledge
    shared(home);
    home.send(kWrite, 4);
}
void w_fetch(Home& home) {  // node 3 reads what node 2 owns
    exclusive(home);
    home.send(kRead, 3);
}
void w_fetch_inv(Home& home) {  // node 3 writes what node 2 owns
    exclusive(home);
    home.send(kWrite, 3);
}
void w_load(Home& home) {  // node 4 reads a page whose home copy is in the spill file
    shared(home);
    paramesh::home_set_where(*home.directory, kPage, HomeWhere::kSpill);
    home.send(kRead, 4);
}

std::string in(Reach reach, HomeEventKind kind, std::uint16_t node) {
    Home home;
    reach(home);
    return home.does(kind, node);
}

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

TEST_CASE("UNCACHED: the rows of section 2.4") {
    Home upgrade;  // as WRITE_REQ: the read copy was invalidated while the node waited
    CHECK(upgrade.does(kUpgrade, 2) == "WRITE_GRANT>2:zero");
    CHECK(upgrade.entry().state == HomeState::kExclusive);
    CHECK(upgrade.entry().version == 1);

    Home writeback;
    CHECK(writeback.does(kWriteback, 2) == "WRITEBACK_ACK>2");  // late: the data is dropped
    CHECK(writeback.entry().state == HomeState::kUncached);
    CHECK(writeback.entry().where == HomeWhere::kZero);

    Home atomic;
    HomeEvent add;
    add.kind = kAtomic;
    add.page = kPage;
    add.from = NodeId{2};
    add.slot = paramesh::Slot{2};
    add.req = paramesh::ReqId{7};
    add.offset_in_page = 16;
    add.operand = 5;
    std::vector<HomeAction> out;
    paramesh::home_step(*atomic.directory, add, out);
    REQUIRE(text(out) == "atomic>2:zero");  // a page never written starts as zeros
    CHECK(out[0].opcode == Opcode::kAtomicResult);
    CHECK(out[0].req == paramesh::ReqId{7});
    CHECK(out[0].offset_in_page == 16);
    CHECK(out[0].operand == 5);
    CHECK(atomic.entry().state == HomeState::kUncached);
    CHECK(atomic.entry().where == HomeWhere::kRam);
    CHECK(atomic.entry().copyset == 0);
    CHECK(atomic.entry().version == 1);
    CHECK(atomic.does(kAtomic, 3) == "atomic>3:home");
    CHECK(atomic.entry().version == 2);

    CHECK(in(uncached, kInvAck, 2) == "abort");  // H1
    CHECK(in(uncached, kFetchData, 2) == "abort");
    CHECK(in(uncached, kLoaded, 0) == "abort");
    CHECK(in(uncached, kHoldExpired, 0).empty());
}

TEST_CASE("SHARED: a write waits for every other holder's INV_ACK") {
    Home home;
    shared(home);
    CHECK(home.does(kRead, 4) == "READ_DATA>4:zero");
    CHECK(home.does(kWriteback, 2) == "WRITEBACK_ACK>2");  // late: dropped
    CHECK(home.entry().state == HomeState::kShared);
    CHECK(home.entry().copyset == 0b11100);

    CHECK(home.does(kWrite, 4) == "INV>2 INV>3");  // not to the writer itself
    CHECK(home.entry().wait == HomeWait::kInv);
    CHECK(home.does(kInvAck, 3).empty());  // others remain
    CHECK(home.entry().copyset == 0b10100);
    CHECK(home.does(kInvAck, 2) == "WRITE_GRANT>4:zero");
    const auto entry = home.entry();
    CHECK(entry.state == HomeState::kExclusive);
    CHECK(entry.wait == HomeWait::kIdle);
    CHECK(entry.owner == NodeId{4});
    CHECK(entry.copyset == 0b10000);
    CHECK(entry.where == HomeWhere::kNone);
    CHECK(entry.version == 1);
}

TEST_CASE("SHARED: UPGRADE_REQ is granted without bytes while the node still has its copy") {
    Home alone;  // others is empty
    alone.send(kRead, 2);
    CHECK(alone.does(kUpgrade, 2) == "UPGRADE_GRANT>2");
    CHECK(alone.entry().state == HomeState::kExclusive);
    CHECK(alone.entry().owner == NodeId{2});
    CHECK(alone.entry().version == 1);

    Home both;  // others is not empty
    shared(both);
    CHECK(both.does(kUpgrade, 2) == "INV>3");
    CHECK(both.does(kInvAck, 3) == "UPGRADE_GRANT>2");
    CHECK(both.entry().copyset == 0b100);

    Home stranger;  // n not in copyset: as WRITE_REQ, so the grant carries the page
    shared(stranger);
    CHECK(stranger.does(kUpgrade, 4) == "INV>2 INV>3");
    stranger.send(kInvAck, 2);
    CHECK(stranger.does(kInvAck, 3) == "WRITE_GRANT>4:zero");

    Home raced;  // two readers upgrade at once: the second lost its copy to the first
    shared(raced);
    CHECK(raced.does(kUpgrade, 2) == "INV>3");
    CHECK(raced.does(kUpgrade, 3).empty());  // queued
    CHECK(raced.does(kInvAck, 3) == "UPGRADE_GRANT>2 FETCH_INV>2");
    CHECK(raced.does(kFetchData, 2) == "WRITE_GRANT>3:received");
    CHECK(raced.entry().version == 2);
}

TEST_CASE("SHARED: ATOMIC_OP invalidates every read copy, the requester's included") {
    Home home;
    shared(home);
    CHECK(home.does(kAtomic, 2) == "INV>2 INV>3");
    CHECK(home.does(kInvAck, 2).empty());
    CHECK(home.does(kInvAck, 3) == "atomic>2:zero");
    CHECK(home.entry().state == HomeState::kUncached);
    CHECK(home.entry().copyset == 0);

    CHECK(in(shared, kInvAck, 2) == "abort");  // H1
    CHECK(in(shared, kFetchData, 2) == "abort");
    CHECK(in(shared, kLoaded, 0) == "abort");
    CHECK(in(shared, kHoldExpired, 0).empty());
}

TEST_CASE("EXCLUSIVE: a writer or an atomic operation takes the page from its owner") {
    CHECK(in(exclusive, kWrite, 3) == "FETCH_INV>2");
    CHECK(in(exclusive, kUpgrade, 3) == "FETCH_INV>2");  // as WRITE_REQ
    CHECK(in(exclusive, kAtomic, 3) == "FETCH_INV>2");
    CHECK(in(exclusive, kAtomic, 2) == "FETCH_INV>2");  // even from the owner
    CHECK(in(exclusive, kWrite, 2) == "abort");         // H2
    CHECK(in(exclusive, kUpgrade, 2) == "abort");
    CHECK(in(exclusive, kInvAck, 2) == "abort");  // H1
    CHECK(in(exclusive, kFetchData, 2) == "abort");
    CHECK(in(exclusive, kLoaded, 0) == "abort");
    CHECK(in(exclusive, kHoldExpired, 0).empty());

    Home home;
    w_fetch_inv(home);
    CHECK(home.entry().wait == HomeWait::kFetchInv);
    CHECK(home.does(kFetchData, 2) == "WRITE_GRANT>3:received");  // passed on, never stored
    const auto entry = home.entry();
    CHECK(entry.state == HomeState::kExclusive);
    CHECK(entry.owner == NodeId{3});
    CHECK(entry.copyset == 0b1000);
    CHECK(entry.where == HomeWhere::kNone);
    CHECK(entry.version == 2);

    Home atomic;
    exclusive(atomic);
    atomic.send(kAtomic, 3);
    CHECK(atomic.does(kFetchData, 2) == "store atomic>3:home");
    CHECK(atomic.entry().state == HomeState::kUncached);
    CHECK(atomic.entry().where == HomeWhere::kRam);
    CHECK(atomic.entry().owner == paramesh::kNoNode);
    CHECK(atomic.entry().version == 2);
}

TEST_CASE("EXCLUSIVE: a WRITEBACK from the owner leaves it as the page's one reader") {
    Home home;
    exclusive(home);
    CHECK(home.does(kWriteback, 3) == "WRITEBACK_ACK>3");  // not the owner: late, dropped
    CHECK(home.entry().state == HomeState::kExclusive);
    CHECK(home.entry().owner == NodeId{2});

    CHECK(home.does(kWriteback, 2) == "store WRITEBACK_ACK>2");
    const auto entry = home.entry();
    CHECK(entry.state == HomeState::kShared);
    CHECK(entry.where == HomeWhere::kRam);
    CHECK(entry.owner == paramesh::kNoNode);
    CHECK(entry.copyset == 0b100);
    CHECK(entry.version == 1);  // a writeback is not a grant

    // A later writer invalidates the evictor like any other reader, and gets the stored page.
    CHECK(home.does(kWrite, 3) == "INV>2");
    CHECK(home.does(kInvAck, 2) == "WRITE_GRANT>3:home drop");
}

TEST_CASE("a busy entry queues every request, whatever it waits for") {
    for (const Reach reach : {w_inv, w_fetch, w_fetch_inv, w_load}) {
        Home home;
        reach(home);
        std::size_t queued = 0;
        for (const HomeEventKind kind : {kRead, kWrite, kUpgrade, kWriteback, kAtomic}) {
            CHECK(home.does(kind, 5).empty());
            CHECK(home.entry().queued == ++queued);
        }
        CHECK(home.does(kHoldExpired, 0).empty());
        CHECK(home.entry().queued == queued);
    }
}

TEST_CASE("a busy entry takes only the answer it waits for, from the node it asked") {
    CHECK(in(w_inv, kInvAck, 5) == "abort");  // H3: not in pending
    CHECK(in(w_inv, kInvAck, 4) == "abort");  // the writer was never asked
    CHECK(in(w_inv, kFetchData, 2) == "abort");
    CHECK(in(w_inv, kLoaded, 0) == "abort");
    Home twice;
    w_inv(twice);
    twice.send(kInvAck, 2);
    CHECK(twice.does(kInvAck, 2) == "abort");  // already acknowledged

    CHECK(in(w_fetch, kInvAck, 2) == "abort");
    CHECK(in(w_fetch, kFetchData, 3) == "abort");
    CHECK(in(w_fetch, kLoaded, 0) == "abort");

    CHECK(in(w_fetch_inv, kInvAck, 2) == "abort");
    CHECK(in(w_fetch_inv, kFetchData, 3) == "abort");
    CHECK(in(w_fetch_inv, kLoaded, 0) == "abort");

    CHECK(in(w_load, kInvAck, 2) == "abort");
    CHECK(in(w_load, kFetchData, 2) == "abort");
}

TEST_CASE("W_LOAD: a step that needs a spilled home copy runs when it is loaded") {
    Home read;
    w_load(read);
    CHECK(read.entry().wait == HomeWait::kLoad);
    CHECK(read.does(kLoaded, 0) == "READ_DATA>4:home");
    CHECK(read.entry().where == HomeWhere::kRam);
    CHECK(read.entry().copyset == 0b11100);

    Home write;  // the load comes after the invalidations, when the grant needs the bytes
    shared(write);
    paramesh::home_set_where(*write.directory, kPage, HomeWhere::kSpill);
    CHECK(write.does(kWrite, 4) == "INV>2 INV>3");
    write.send(kInvAck, 2);
    CHECK(write.does(kInvAck, 3) == "load");
    CHECK(write.does(kLoaded, 0) == "WRITE_GRANT>4:home drop");

    Home atomic;
    paramesh::home_set_where(*atomic.directory, kPage, HomeWhere::kSpill);
    CHECK(atomic.does(kAtomic, 2) == "load");
    CHECK(atomic.does(kLoaded, 0) == "atomic>2:home");

    Home upgrade;  // UPGRADE_GRANT carries no bytes, so nothing is loaded
    upgrade.send(kRead, 2);
    paramesh::home_set_where(*upgrade.directory, kPage, HomeWhere::kSpill);
    CHECK(upgrade.does(kUpgrade, 2) == "UPGRADE_GRANT>2 drop");
}

TEST_CASE("queued requests of every kind are taken in order as each operation ends") {
    Home home;
    w_fetch_inv(home);                        // node 3 writes what node 2 owns
    CHECK(home.does(kRead, 4).empty());       // queued
    CHECK(home.does(kAtomic, 5).empty());     // queued
    CHECK(home.does(kWriteback, 2).empty());  // queued: node 2 was evicting meanwhile
    CHECK(home.does(kFetchData, 2) == "WRITE_GRANT>3:received FETCH>3");
    // The read ends; the atomic operation starts and invalidates both readers; the late
    // writeback waits behind it.
    CHECK(home.does(kFetchData, 3) == "store READ_DATA>4:received INV>3 INV>4");
    home.send(kInvAck, 4);
    CHECK(home.does(kInvAck, 3) == "atomic>5:home WRITEBACK_ACK>2");
    const auto entry = home.entry();
    CHECK(entry.state == HomeState::kUncached);
    CHECK(entry.queued == 0);
    CHECK(entry.version == 3);  // two write grants and one atomic operation
}

namespace {

// More than 2 transfers in 100 ms is thrashing; the hold starts at 10 ms and doubles to 40.
paramesh::HomeConfig quick_to_hold() {
    paramesh::HomeConfig config;
    config.thrash_transfers = 2;
    config.thrash_period = std::chrono::milliseconds{100};
    config.hold_initial = std::chrono::milliseconds{10};
    config.hold_max = std::chrono::milliseconds{40};
    return config;
}

// Passes write access from its owner to `to`: the request, then the owner's FETCH_DATA.
// Returns what the home does on the FETCH_DATA.
std::string pass_to(Home& home, std::uint16_t to) {
    const std::uint16_t owner = home.entry().owner.value;
    REQUIRE(home.does(kWrite, to) == "FETCH_INV>" + std::to_string(owner));
    return home.does(kFetchData, owner);
}

// Nodes 2 and 3 pass the page back and forth until the home holds it: node 2 writes first,
// then 3, 2, 3, and the third transfer is one too many. Node 3 owns it, held until 10 ms.
void fight(Home& home) {
    home.send(kWrite, 2);
    REQUIRE(pass_to(home, 3) == "WRITE_GRANT>3:received");
    REQUIRE(pass_to(home, 2) == "WRITE_GRANT>2:received");
    REQUIRE(pass_to(home, 3) == "WRITE_GRANT>3:received thrash>3/2");
}

}  // namespace

TEST_CASE("hold window: off by default, however often the page changes hands") {
    Home home;
    home.send(kWrite, 2);
    for (int round = 0; round < 20; round++) {
        CHECK(pass_to(home, 3) == "WRITE_GRANT>3:received");
        CHECK(pass_to(home, 2) == "WRITE_GRANT>2:received");
    }
}

TEST_CASE(
    "hold window: a page that changes hands too often is held, and the home says who fights") {
    Home home{quick_to_hold()};
    fight(home);

    // A writer other than the owner waits, and the home asks to be told when the window ends.
    home.now = std::chrono::milliseconds{4};
    CHECK(home.does(kWrite, 2) == "arm@10");
    CHECK(home.entry().queued == 1);
    CHECK(home.entry().wait == HomeWait::kIdle);
    CHECK(home.does(kUpgrade, 4).empty());  // held too; the timer is already asked for
    CHECK(home.does(kAtomic, 4).empty());   // and an atomic operation from another node
    CHECK(home.entry().queued == 3);

    // The window ends: the held requests are taken in the order they came.
    home.now = std::chrono::milliseconds{10};
    CHECK(home.does(kHoldExpired, 0) == "FETCH_INV>3");
    CHECK(home.entry().queued == 2);
    // Node 2 gets the page: a fourth transfer in the period, so the hold doubles to 20 ms and
    // the two behind it wait again, until 30 ms. No second report: the fight is the same one.
    CHECK(home.does(kFetchData, 3) == "WRITE_GRANT>2:received arm@30");
    home.now = std::chrono::milliseconds{30};
    CHECK(home.does(kHoldExpired, 0) == "FETCH_INV>2");
    // Node 4 gets the page, held for 40 ms, the most. What is left in the queue is node 4's
    // own atomic operation, and the owner is not held: the home takes the page back at once.
    CHECK(home.does(kFetchData, 2) == "WRITE_GRANT>4:received FETCH_INV>4");
    CHECK(home.does(kFetchData, 4) == "store atomic>4:home");
    CHECK(home.entry().queued == 0);
}

TEST_CASE("hold window: readers and the owner are not held, and a reader ends the window") {
    Home owner{quick_to_hold()};
    fight(owner);
    owner.now = std::chrono::milliseconds{4};
    CHECK(owner.does(kAtomic, 3) == "FETCH_INV>3");  // the owner's own atomic operation

    Home home{quick_to_hold()};
    fight(home);
    home.now = std::chrono::milliseconds{4};
    CHECK(home.does(kWrite, 2) == "arm@10");
    // A read is served at once, ahead of the writer that waits.
    CHECK(home.does(kRead, 4) == "FETCH>3");
    // The page is shared now, so the window is over: the writer is taken without waiting for
    // 10 ms, and invalidates both readers.
    CHECK(home.does(kFetchData, 3) == "store READ_DATA>4:received INV>3 INV>4");
    CHECK(home.entry().queued == 0);
    // The timer still fires; by then there is nothing for it to do.
    home.send(kInvAck, 3);
    home.send(kInvAck, 4);
    home.now = std::chrono::milliseconds{10};
    CHECK(home.does(kHoldExpired, 0).empty());
}

TEST_CASE(
    "hold window: a timer that fires early is asked for again; a quiet period ends the hold") {
    Home home{quick_to_hold()};
    fight(home);
    home.now = std::chrono::milliseconds{4};
    CHECK(home.does(kWrite, 2) == "arm@10");
    home.now = std::chrono::milliseconds{9};
    CHECK(home.does(kHoldExpired, 0) == "arm@10");  // not over yet
    CHECK(home.entry().queued == 1);
    home.now = std::chrono::milliseconds{10};
    CHECK(home.does(kHoldExpired, 0) == "FETCH_INV>3");
    home.does(kFetchData, 3);  // node 2 has it, held until 30 ms

    // Nobody asks for more than the period. The next transfer is the first of a new period:
    // the fight has stopped, the hold is reset, and a writer is served at once.
    home.now = std::chrono::milliseconds{250};
    CHECK(pass_to(home, 3) == "WRITE_GRANT>3:received");
    home.now = std::chrono::milliseconds{251};
    CHECK(home.does(kWrite, 2) == "FETCH_INV>3");
    // If the fight starts again, it is reported again and the hold starts from 10 ms.
    CHECK(home.does(kFetchData, 3) == "WRITE_GRANT>2:received");
    CHECK(pass_to(home, 3) == "WRITE_GRANT>3:received thrash>3/2");
    CHECK(home.does(kWrite, 2) == "arm@261");
}

TEST_CASE("hold window: the same node writing again and again is no transfer") {
    Home home{quick_to_hold()};
    home.send(kWrite, 2);
    for (int round = 0; round < 10; round++) {
        // Node 2 writes back and takes the page again: nobody else is involved.
        CHECK(home.does(kWriteback, 2) == "store WRITEBACK_ACK>2");
        CHECK(home.does(kUpgrade, 2) == "UPGRADE_GRANT>2 drop");
    }
    CHECK(home.does(kWrite, 3) == "FETCH_INV>2");  // and nothing is held
}
