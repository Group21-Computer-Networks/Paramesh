// The node machine of docs/STATE_MACHINES.md, section 1.4, as a table of its rows: every pair
// the document does not mark impossible. A pair with no row here ends the job.

#include "coh/node_machine.h"

#include <array>
#include <cstdint>

namespace paramesh {
namespace {

using S = PageState;
using E = NodeEventKind;

// What one row does, in order. kNone ends the list.
enum class Op : std::uint8_t {
    kNone,
    kSendRead,
    kSendWrite,
    kSendUpgrade,
    kSendWriteback,  // with the page
    kWake,
    kInstallReadOnly,
    kInstallWritable,
    kWriteProtect,
    kAllowWrites,
    kDiscard,
    kArmResend,
    kReplyInvAck,
    kReplyPageReadOnly,  // FETCH_DATA; this node keeps a read copy
    kReplyPage,          // FETCH_DATA; this node gives the page up
};

struct Row {
    S state;
    E event;
    S next;
    std::array<Op, kMaxNodeActions> ops;
};

// "join" and "none" rows have no actions. The page's current request follows from the row:
// a row that sends a request makes it current, and a stable next state has none.
constexpr std::array kRows = {
    Row{S::kInvalid, E::kNeedRead, S::kReadPending, {Op::kSendRead}},
    Row{S::kInvalid, E::kNeedWrite, S::kWritePending, {Op::kSendWrite}},
    Row{S::kInvalid, E::kEvict, S::kInvalid, {}},
    Row{S::kInvalid, E::kResend, S::kInvalid, {}},
    Row{S::kInvalid, E::kInv, S::kInvalid, {Op::kReplyInvAck}},

    Row{S::kShared, E::kNeedRead, S::kShared, {Op::kWake}},
    Row{S::kShared, E::kNeedWrite, S::kUpgradePending, {Op::kSendUpgrade}},
    Row{S::kShared, E::kEvict, S::kInvalid, {Op::kDiscard}},
    Row{S::kShared, E::kResend, S::kShared, {}},
    Row{S::kShared, E::kInv, S::kInvalid, {Op::kDiscard, Op::kReplyInvAck}},

    Row{S::kModified, E::kNeedRead, S::kModified, {Op::kWake}},
    Row{S::kModified, E::kNeedWrite, S::kModified, {Op::kWake}},
    Row{S::kModified, E::kEvict, S::kWritebackPending, {Op::kWriteProtect, Op::kSendWriteback}},
    Row{S::kModified, E::kResend, S::kModified, {}},
    Row{S::kModified, E::kFetch, S::kShared, {Op::kWriteProtect, Op::kReplyPageReadOnly}},
    Row{S::kModified,
        E::kFetchInv,
        S::kInvalid,
        {Op::kWriteProtect, Op::kReplyPage, Op::kDiscard, Op::kWake}},

    Row{S::kReadPending, E::kNeedRead, S::kReadPending, {}},
    Row{S::kReadPending, E::kNeedWrite, S::kReadPending, {}},
    Row{S::kReadPending, E::kEvict, S::kReadPending, {}},
    Row{S::kReadPending, E::kReadData, S::kShared, {Op::kInstallReadOnly}},
    Row{S::kReadPending, E::kBusyRetry, S::kReadPending, {Op::kArmResend}},
    Row{S::kReadPending, E::kRedirect, S::kReadPending, {Op::kArmResend}},
    Row{S::kReadPending, E::kResend, S::kReadPending, {Op::kSendRead}},
    Row{S::kReadPending, E::kInv, S::kReadPending, {Op::kReplyInvAck}},

    Row{S::kWritePending, E::kNeedRead, S::kWritePending, {}},
    Row{S::kWritePending, E::kNeedWrite, S::kWritePending, {}},
    Row{S::kWritePending, E::kEvict, S::kWritePending, {}},
    Row{S::kWritePending, E::kWriteGrant, S::kModified, {Op::kInstallWritable}},
    Row{S::kWritePending, E::kBusyRetry, S::kWritePending, {Op::kArmResend}},
    Row{S::kWritePending, E::kRedirect, S::kWritePending, {Op::kArmResend}},
    Row{S::kWritePending, E::kResend, S::kWritePending, {Op::kSendWrite}},
    Row{S::kWritePending, E::kInv, S::kWritePending, {Op::kReplyInvAck}},

    Row{S::kUpgradePending, E::kNeedRead, S::kUpgradePending, {Op::kWake}},
    Row{S::kUpgradePending, E::kNeedWrite, S::kUpgradePending, {}},
    Row{S::kUpgradePending, E::kEvict, S::kUpgradePending, {}},
    Row{S::kUpgradePending, E::kUpgradeGrant, S::kModified, {Op::kAllowWrites}},
    Row{S::kUpgradePending, E::kBusyRetry, S::kUpgradePending, {Op::kArmResend}},
    Row{S::kUpgradePending, E::kRedirect, S::kUpgradePending, {Op::kArmResend}},
    Row{S::kUpgradePending, E::kResend, S::kUpgradePending, {Op::kSendUpgrade}},
    // Another writer won. The UPGRADE_REQ stays current; the home answers it with WRITE_GRANT.
    Row{S::kUpgradePending, E::kInv, S::kWritePending, {Op::kDiscard, Op::kReplyInvAck}},

    // A page being evicted is discarded only on WRITEBACK_ACK, or when the home has taken the
    // data another way (INV after a FETCH, or FETCH_INV).
    Row{S::kWritebackPending, E::kNeedRead, S::kWritebackPending, {Op::kWake}},
    Row{S::kWritebackPending, E::kNeedWrite, S::kWritebackPending, {}},
    Row{S::kWritebackPending, E::kEvict, S::kWritebackPending, {}},
    Row{S::kWritebackPending, E::kWritebackAck, S::kInvalid, {Op::kDiscard, Op::kWake}},
    Row{S::kWritebackPending, E::kBusyRetry, S::kWritebackPending, {Op::kArmResend}},
    Row{S::kWritebackPending, E::kRedirect, S::kWritebackPending, {Op::kArmResend}},
    Row{S::kWritebackPending, E::kResend, S::kWritebackPending, {Op::kSendWriteback}},
    Row{S::kWritebackPending, E::kInv, S::kInvalid, {Op::kDiscard, Op::kWake, Op::kReplyInvAck}},
    Row{S::kWritebackPending, E::kFetch, S::kWritebackPending, {Op::kReplyPageReadOnly}},
    Row{S::kWritebackPending, E::kFetchInv, S::kInvalid, {Op::kReplyPage, Op::kDiscard, Op::kWake}},
};

constexpr bool is_stable(S state) noexcept {
    return state == S::kInvalid || state == S::kShared || state == S::kModified;
}

constexpr bool is_reply(E event) noexcept {
    return event == E::kReadData || event == E::kWriteGrant || event == E::kUpgradeGrant ||
           event == E::kWritebackAck || event == E::kBusyRetry || event == E::kRedirect;
}

// The action one Op stands for. `fresh` numbers a request, `theirs` is repeated in a reply.
NodeAction action_of(Op op, ReqId fresh, ReqId theirs) noexcept {
    const auto simple = [](NodeActionKind kind) {
        NodeAction a;
        a.kind = kind;
        return a;
    };
    const auto message = [](NodeActionKind kind, Opcode opcode, ReqId req, bool page = false,
                            bool read_only = false) {
        return NodeAction{kind, opcode, req, page, read_only};
    };
    switch (op) {
        case Op::kSendRead:
            return message(NodeActionKind::kSend, Opcode::kReadReq, fresh);
        case Op::kSendWrite:
            return message(NodeActionKind::kSend, Opcode::kWriteReq, fresh);
        case Op::kSendUpgrade:
            return message(NodeActionKind::kSend, Opcode::kUpgradeReq, fresh);
        case Op::kSendWriteback:
            return message(NodeActionKind::kSend, Opcode::kWriteback, fresh, true);
        case Op::kReplyInvAck:
            return message(NodeActionKind::kReply, Opcode::kInvAck, theirs);
        case Op::kReplyPageReadOnly:
            return message(NodeActionKind::kReply, Opcode::kFetchData, theirs, true, true);
        case Op::kReplyPage:
            return message(NodeActionKind::kReply, Opcode::kFetchData, theirs, true);
        case Op::kInstallReadOnly:
            return simple(NodeActionKind::kInstallReadOnly);
        case Op::kInstallWritable:
            return simple(NodeActionKind::kInstallWritable);
        case Op::kWriteProtect:
            return simple(NodeActionKind::kWriteProtect);
        case Op::kAllowWrites:
            return simple(NodeActionKind::kAllowWrites);
        case Op::kDiscard:
            return simple(NodeActionKind::kDiscard);
        case Op::kArmResend:
            return simple(NodeActionKind::kArmResend);
        case Op::kWake:
            return simple(NodeActionKind::kWake);
        default:
            return simple(NodeActionKind::kAbort);  // kNone: the pair has no row
    }
}

}  // namespace

NodeStep coh_step(PageState state, ReqId current, const NodeEvent& event) noexcept {
    NodeStep step;
    step.state = state;
    step.current = current;

    // Rule 0: a reply that does not answer the page's current request was superseded.
    if (is_reply(event.kind) && event.req != current) {
        step.dropped = true;
        return step;
    }

    const auto push = [&step](const NodeAction& a) { step.actions.at(step.action_count++) = a; };
    for (const Row& row : kRows) {
        if (row.state != state || row.event != event.kind) {
            continue;
        }
        step.state = row.next;
        for (const Op op : row.ops) {
            if (op == Op::kNone) {
                break;
            }
            const NodeAction a = action_of(op, event.fresh, event.req);
            push(a);
            if (a.kind == NodeActionKind::kSend) {
                step.current = event.fresh;
            }
        }
        if (is_stable(row.next)) {
            step.current = kNoReq;
        }
        return step;
    }

    push(action_of(Op::kNone, kNoReq, kNoReq));
    return step;
}

}  // namespace paramesh
