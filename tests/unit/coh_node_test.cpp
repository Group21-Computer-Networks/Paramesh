// src/coh/ node machine: every pair of docs/STATE_MACHINES.md, section 1.4. kRows below is
// the document's table written out again; the 44 pairs it leaves out are the impossible ones.
//
// Actions are compared as text, one word each: a request by its name ("UPGRADE_REQ",
// "WRITEBACK+page"), a reply as "INV_ACK" or "FETCH_DATA+page+ro", and "install-ro",
// "install-rw", "protect", "allow", "discard", "wake", "arm".

#include "coh/node_machine.h"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <string>

namespace {

using paramesh::NodeAction;
using paramesh::NodeActionKind;
using paramesh::NodeEvent;
using paramesh::NodeStep;
using paramesh::Opcode;
using paramesh::ReqId;
using S = paramesh::PageState;
using E = paramesh::NodeEventKind;

constexpr ReqId kCurrent{41};  // the request an in-flight page is waiting on
constexpr ReqId kFresh{42};    // the number offered for a new request
constexpr ReqId kTheirs{77};   // the home's request number on INV, FETCH and FETCH_INV

constexpr std::array kStates{S::kInvalid,         S::kShared,       S::kModified,
                             S::kReadPending,     S::kWritePending, S::kUpgradePending,
                             S::kWritebackPending};
constexpr std::array kEvents{E::kNeedRead,   E::kNeedWrite,    E::kEvict,        E::kReadData,
                             E::kWriteGrant, E::kUpgradeGrant, E::kWritebackAck, E::kBusyRetry,
                             E::kRedirect,   E::kResend,       E::kInv,          E::kFetch,
                             E::kFetchInv};

bool stable(S state) {
    return state == S::kInvalid || state == S::kShared || state == S::kModified;
}
bool from_home(E event) {
    return event == E::kInv || event == E::kFetch || event == E::kFetchInv;
}
bool reply(E event) {
    return event == E::kReadData || event == E::kWriteGrant || event == E::kUpgradeGrant ||
           event == E::kWritebackAck || event == E::kBusyRetry || event == E::kRedirect;
}

// Feeds one event to a page in `state`. A reply answers the current request; a request from
// the home carries the home's number.
NodeStep step(S state, E kind) {
    NodeEvent event;
    event.kind = kind;
    event.page = paramesh::PageId{5};
    event.fresh = kFresh;
    if (reply(kind)) {
        event.req = stable(state) ? paramesh::kNoReq : kCurrent;
    } else if (from_home(kind)) {
        event.req = kTheirs;
    }
    return paramesh::coh_step(state, stable(state) ? paramesh::kNoReq : kCurrent, event);
}

std::string word(const NodeAction& a) {
    switch (a.kind) {
        case NodeActionKind::kSend:
        case NodeActionKind::kReply: {
            std::string name = "?";
            switch (a.opcode) {
                case Opcode::kReadReq:
                    name = "READ_REQ";
                    break;
                case Opcode::kWriteReq:
                    name = "WRITE_REQ";
                    break;
                case Opcode::kUpgradeReq:
                    name = "UPGRADE_REQ";
                    break;
                case Opcode::kWriteback:
                    name = "WRITEBACK";
                    break;
                case Opcode::kInvAck:
                    name = "INV_ACK";
                    break;
                case Opcode::kFetchData:
                    name = "FETCH_DATA";
                    break;
                default:
                    break;
            }
            return name + (a.with_page ? "+page" : "") + (a.read_only ? "+ro" : "");
        }
        case NodeActionKind::kInstallReadOnly:
            return "install-ro";
        case NodeActionKind::kInstallWritable:
            return "install-rw";
        case NodeActionKind::kWriteProtect:
            return "protect";
        case NodeActionKind::kAllowWrites:
            return "allow";
        case NodeActionKind::kDiscard:
            return "discard";
        case NodeActionKind::kWake:
            return "wake";
        case NodeActionKind::kArmResend:
            return "arm";
        default:
            return "abort";
    }
}

std::string text(const NodeStep& s) {
    std::string all;
    for (std::size_t i = 0; i < s.action_count; i++) {
        all += (i == 0 ? "" : " ") + word(s.actions.at(i));
    }
    return all;
}

struct Row {
    S state;
    E event;
    S next;
    const char* actions;
};

// Section 1.4, without the impossible pairs. "" is the document's "none" or "join".
constexpr std::array kRows{
    Row{S::kInvalid, E::kNeedRead, S::kReadPending, "READ_REQ"},
    Row{S::kInvalid, E::kNeedWrite, S::kWritePending, "WRITE_REQ"},
    Row{S::kInvalid, E::kEvict, S::kInvalid, ""},
    Row{S::kInvalid, E::kResend, S::kInvalid, ""},
    Row{S::kInvalid, E::kInv, S::kInvalid, "INV_ACK"},

    Row{S::kShared, E::kNeedRead, S::kShared, "wake"},
    Row{S::kShared, E::kNeedWrite, S::kUpgradePending, "UPGRADE_REQ"},
    Row{S::kShared, E::kEvict, S::kInvalid, "discard"},
    Row{S::kShared, E::kResend, S::kShared, ""},
    Row{S::kShared, E::kInv, S::kInvalid, "discard INV_ACK"},

    Row{S::kModified, E::kNeedRead, S::kModified, "wake"},
    Row{S::kModified, E::kNeedWrite, S::kModified, "wake"},
    Row{S::kModified, E::kEvict, S::kWritebackPending, "protect WRITEBACK+page"},
    Row{S::kModified, E::kResend, S::kModified, ""},
    Row{S::kModified, E::kFetch, S::kShared, "protect FETCH_DATA+page+ro"},
    Row{S::kModified, E::kFetchInv, S::kInvalid, "protect FETCH_DATA+page discard wake"},

    Row{S::kReadPending, E::kNeedRead, S::kReadPending, ""},
    Row{S::kReadPending, E::kNeedWrite, S::kReadPending, ""},
    Row{S::kReadPending, E::kEvict, S::kReadPending, ""},
    Row{S::kReadPending, E::kReadData, S::kShared, "install-ro"},
    Row{S::kReadPending, E::kBusyRetry, S::kReadPending, "arm"},
    Row{S::kReadPending, E::kRedirect, S::kReadPending, "arm"},
    Row{S::kReadPending, E::kResend, S::kReadPending, "READ_REQ"},
    Row{S::kReadPending, E::kInv, S::kReadPending, "INV_ACK"},

    Row{S::kWritePending, E::kNeedRead, S::kWritePending, ""},
    Row{S::kWritePending, E::kNeedWrite, S::kWritePending, ""},
    Row{S::kWritePending, E::kEvict, S::kWritePending, ""},
    Row{S::kWritePending, E::kWriteGrant, S::kModified, "install-rw"},
    Row{S::kWritePending, E::kBusyRetry, S::kWritePending, "arm"},
    Row{S::kWritePending, E::kRedirect, S::kWritePending, "arm"},
    Row{S::kWritePending, E::kResend, S::kWritePending, "WRITE_REQ"},
    Row{S::kWritePending, E::kInv, S::kWritePending, "INV_ACK"},

    Row{S::kUpgradePending, E::kNeedRead, S::kUpgradePending, "wake"},
    Row{S::kUpgradePending, E::kNeedWrite, S::kUpgradePending, ""},
    Row{S::kUpgradePending, E::kEvict, S::kUpgradePending, ""},
    Row{S::kUpgradePending, E::kUpgradeGrant, S::kModified, "allow"},
    Row{S::kUpgradePending, E::kBusyRetry, S::kUpgradePending, "arm"},
    Row{S::kUpgradePending, E::kRedirect, S::kUpgradePending, "arm"},
    Row{S::kUpgradePending, E::kResend, S::kUpgradePending, "UPGRADE_REQ"},
    Row{S::kUpgradePending, E::kInv, S::kWritePending, "discard INV_ACK"},

    Row{S::kWritebackPending, E::kNeedRead, S::kWritebackPending, "wake"},
    Row{S::kWritebackPending, E::kNeedWrite, S::kWritebackPending, ""},
    Row{S::kWritebackPending, E::kEvict, S::kWritebackPending, ""},
    Row{S::kWritebackPending, E::kWritebackAck, S::kInvalid, "discard wake"},
    Row{S::kWritebackPending, E::kBusyRetry, S::kWritebackPending, "arm"},
    Row{S::kWritebackPending, E::kRedirect, S::kWritebackPending, "arm"},
    Row{S::kWritebackPending, E::kResend, S::kWritebackPending, "WRITEBACK+page"},
    Row{S::kWritebackPending, E::kInv, S::kInvalid, "discard wake INV_ACK"},
    Row{S::kWritebackPending, E::kFetch, S::kWritebackPending, "FETCH_DATA+page+ro"},
    Row{S::kWritebackPending, E::kFetchInv, S::kInvalid, "FETCH_DATA+page discard wake"},
};

const Row* row_of(S state, E event) {
    for (const Row& row : kRows) {
        if (row.state == state && row.event == event) {
            return &row;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("every pair of section 1.4 does what its row says, and the rest end the job") {
    std::size_t rows = 0;
    std::size_t impossible = 0;
    for (const S state : kStates) {
        for (const E event : kEvents) {
            CAPTURE(static_cast<int>(state));
            CAPTURE(static_cast<int>(event));
            const NodeStep s = step(state, event);
            const Row* row = row_of(state, event);
            if (row == nullptr) {
                impossible++;
                CHECK(text(s) == "abort");
                CHECK(s.state == state);  // the page is left as it was
                continue;
            }
            rows++;
            CHECK(text(s) == row->actions);
            CHECK(s.state == row->next);
            CHECK_FALSE(s.dropped);
        }
    }
    CHECK(rows == kRows.size());
    CHECK(rows == 50);
    CHECK(impossible == 41);
}

TEST_CASE("the current request: set by a send, kept while in flight, none when stable") {
    for (const Row& row : kRows) {
        CAPTURE(static_cast<int>(row.state));
        CAPTURE(static_cast<int>(row.event));
        const NodeStep s = step(row.state, row.event);
        const bool sends =
            s.action_count > 0 && s.actions.at(s.action_count - 1).kind == NodeActionKind::kSend;
        for (std::size_t i = 0; i < s.action_count; i++) {
            const NodeAction& a = s.actions.at(i);
            if (a.kind == NodeActionKind::kSend) {
                CHECK(a.req == kFresh);  // a request takes the number offered
            } else if (a.kind == NodeActionKind::kReply) {
                CHECK(a.req == kTheirs);  // an answer repeats the home's number
            }
        }
        if (sends) {
            CHECK(s.current == kFresh);
        } else if (stable(s.state)) {
            CHECK(s.current == paramesh::kNoReq);
        } else {
            CHECK(s.current == kCurrent);  // still the one request
        }
    }
    // The two rows the document spells out: the UPGRADE_REQ outlives the INV that beat it, and
    // a WRITEBACK outlives a FETCH.
    CHECK(step(S::kUpgradePending, E::kInv).current == kCurrent);
    CHECK(step(S::kWritebackPending, E::kFetch).current == kCurrent);
}

TEST_CASE("Rule 0: a reply that does not answer the current request is dropped") {
    for (const S state : kStates) {
        for (const E event : kEvents) {
            if (!reply(event)) {
                continue;
            }
            NodeEvent stale;
            stale.kind = event;
            stale.req = ReqId{7};
            stale.fresh = kFresh;
            const ReqId current = stable(state) ? paramesh::kNoReq : kCurrent;
            const NodeStep s = paramesh::coh_step(state, current, stale);
            CHECK(s.dropped);  // in a stable state this is reason R1
            CHECK(s.state == state);
            CHECK(s.current == current);
            CHECK(s.action_count == 0);
        }
    }
}

TEST_CASE("a modified page is never discarded before the home has its bytes") {
    // From M or M→I, the only steps that discard are the acknowledgement of the WRITEBACK and
    // the two where the home took the data another way; where the node sends the bytes itself
    // in that step, the send comes first, after the write-protect.
    for (const S state : {S::kModified, S::kWritebackPending}) {
        for (const E event : kEvents) {
            const std::string actions = text(step(state, event));
            const bool home_has_it =
                event == E::kFetchInv ||
                (state == S::kWritebackPending && (event == E::kWritebackAck || event == E::kInv));
            CAPTURE(actions);
            CHECK((actions.find("discard") != std::string::npos) == home_has_it);
        }
    }
    CHECK(text(step(S::kModified, E::kEvict)) == "protect WRITEBACK+page");  // and then it waits
    CHECK(text(step(S::kModified, E::kFetchInv)) == "protect FETCH_DATA+page discard wake");

    // An eviction, with a reader's FETCH arriving before the acknowledgement.
    NodeStep s = step(S::kModified, E::kEvict);
    REQUIRE(s.state == S::kWritebackPending);
    s = step(s.state, E::kFetch);
    CHECK(s.state == S::kWritebackPending);
    CHECK(text(s) == "FETCH_DATA+page+ro");
    s = step(s.state, E::kWritebackAck);
    CHECK(s.state == S::kInvalid);
    CHECK(text(s) == "discard wake");
}
