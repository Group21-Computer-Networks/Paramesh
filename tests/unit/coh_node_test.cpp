// src/coh/ node machine: every transition M1 uses, row by row as listed in
// docs/STATE_MACHINES.md, section 1.4, plus Rule 0 and the pairs that must end the job.

#include "coh/node_machine.h"

#include <doctest/doctest.h>

#include <vector>

namespace {

using paramesh::NodeActionKind;
using paramesh::NodeEvent;
using paramesh::NodeEventKind;
using paramesh::NodeStep;
using paramesh::Opcode;
using paramesh::PageState;
using paramesh::ReqId;

constexpr ReqId kCurrent{41};  // the request an in-flight page is waiting on
constexpr ReqId kFresh{42};    // the number offered for a new request
constexpr ReqId kTheirs{77};   // the home's request number on FETCH

bool in_flight(PageState state) {
    return state == PageState::kReadPending || state == PageState::kWritePending;
}

// Feeds one event to a page in `state`. A reply answers the current request; FETCH carries
// the home's request number.
NodeStep step(PageState state, NodeEventKind kind) {
    NodeEvent event;
    event.kind = kind;
    event.page = paramesh::PageId{5};
    event.fresh = kFresh;
    if (kind == NodeEventKind::kReadData || kind == NodeEventKind::kWriteGrant) {
        event.req = kCurrent;
    } else if (kind == NodeEventKind::kFetch) {
        event.req = kTheirs;
    }
    return paramesh::coh_step(state, in_flight(state) ? kCurrent : paramesh::kNoReq, event);
}

std::vector<NodeActionKind> kinds(const NodeStep& s) {
    std::vector<NodeActionKind> out;
    for (std::size_t i = 0; i < s.action_count; i++) {
        out.push_back(s.actions.at(i).kind);
    }
    return out;
}

}  // namespace

TEST_CASE("I: a read fault sends READ_REQ, a write fault sends WRITE_REQ") {
    const NodeStep read = step(PageState::kInvalid, NodeEventKind::kNeedRead);
    CHECK(read.state == PageState::kReadPending);
    REQUIRE(kinds(read) == std::vector{NodeActionKind::kSend});
    CHECK(read.actions.at(0).opcode == Opcode::kReadReq);
    CHECK(read.actions.at(0).req == kFresh);
    CHECK(read.current == kFresh);

    const NodeStep write = step(PageState::kInvalid, NodeEventKind::kNeedWrite);
    CHECK(write.state == PageState::kWritePending);
    REQUIRE(kinds(write) == std::vector{NodeActionKind::kSend});
    CHECK(write.actions.at(0).opcode == Opcode::kWriteReq);
    CHECK(write.current == kFresh);
}

TEST_CASE("S and M: a fault on a page that already allows the access only wakes") {
    for (const auto& [state, kind] : {std::pair{PageState::kShared, NodeEventKind::kNeedRead},
                                      std::pair{PageState::kModified, NodeEventKind::kNeedRead},
                                      std::pair{PageState::kModified, NodeEventKind::kNeedWrite}}) {
        const NodeStep s = step(state, kind);
        CHECK(s.state == state);
        CHECK(kinds(s) == std::vector{NodeActionKind::kWake});
        CHECK(s.current == paramesh::kNoReq);
    }
}

TEST_CASE("in flight: further faults on the page join the request and send nothing") {
    for (const PageState state : {PageState::kReadPending, PageState::kWritePending}) {
        for (const NodeEventKind kind : {NodeEventKind::kNeedRead, NodeEventKind::kNeedWrite}) {
            const NodeStep s = step(state, kind);
            CHECK(s.state == state);
            CHECK(s.action_count == 0);
            CHECK(s.current == kCurrent);  // still the one request
            CHECK_FALSE(s.dropped);
        }
    }
}

TEST_CASE("I→S: READ_DATA installs the page read-only; I→M: WRITE_GRANT installs it writable") {
    const NodeStep read = step(PageState::kReadPending, NodeEventKind::kReadData);
    CHECK(read.state == PageState::kShared);
    CHECK(kinds(read) == std::vector{NodeActionKind::kInstallReadOnly});
    CHECK(read.current == paramesh::kNoReq);

    const NodeStep write = step(PageState::kWritePending, NodeEventKind::kWriteGrant);
    CHECK(write.state == PageState::kModified);
    CHECK(kinds(write) == std::vector{NodeActionKind::kInstallWritable});
    CHECK(write.current == paramesh::kNoReq);
}

TEST_CASE("M: FETCH write-protects, then replies FETCH_DATA with the page, read-only") {
    const NodeStep s = step(PageState::kModified, NodeEventKind::kFetch);
    CHECK(s.state == PageState::kShared);
    REQUIRE(kinds(s) == std::vector{NodeActionKind::kWriteProtect, NodeActionKind::kReply});
    CHECK(s.actions.at(1).opcode == Opcode::kFetchData);
    CHECK(s.actions.at(1).req == kTheirs);  // the home's request number comes back
    CHECK(s.actions.at(1).with_page);
    CHECK(s.actions.at(1).read_only);
}

TEST_CASE("Rule 0: a reply that does not answer the current request is dropped") {
    NodeEvent stale;
    stale.kind = NodeEventKind::kReadData;
    stale.req = ReqId{7};
    stale.fresh = kFresh;

    const NodeStep waiting = paramesh::coh_step(PageState::kReadPending, kCurrent, stale);
    CHECK(waiting.dropped);
    CHECK(waiting.state == PageState::kReadPending);
    CHECK(waiting.current == kCurrent);
    CHECK(waiting.action_count == 0);

    // A stable state has no current request, so any reply is dropped (reason R1).
    const NodeStep idle = paramesh::coh_step(PageState::kShared, paramesh::kNoReq, stale);
    CHECK(idle.dropped);
    CHECK(idle.action_count == 0);
}

TEST_CASE("a pair with no M1 row ends the job and leaves the page as it was") {
    // FETCH to a node that is not the owner is impossible (R2); INV belongs to M2; a WRITE_GRANT
    // that answers a READ_REQ is impossible (R4).
    for (const auto& [state, kind] :
         {std::pair{PageState::kShared, NodeEventKind::kFetch},
          std::pair{PageState::kInvalid, NodeEventKind::kFetchInv},
          std::pair{PageState::kShared, NodeEventKind::kInv},
          std::pair{PageState::kReadPending, NodeEventKind::kWriteGrant}}) {
        const NodeStep s = step(state, kind);
        CHECK(s.state == state);
        CHECK_FALSE(s.dropped);
        CHECK(kinds(s) == std::vector{NodeActionKind::kAbort});
    }
}
