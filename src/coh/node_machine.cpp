// The node machine of docs/STATE_MACHINES.md, section 1.4, as a table of its rows.
// M1 holds the rows marked M1. A pair with no row here ends the job: either the document
// marks it impossible, or it belongs to a later milestone, which adds its rows to kRows.

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
    kWake,
    kInstallReadOnly,
    kInstallWritable,
    kWriteProtect,
    kReplyPageReadOnly
};

struct Row {
    S state;
    E event;
    S next;
    std::array<Op, kMaxNodeActions> ops;
};

constexpr std::array kRows = {
    Row{S::kInvalid, E::kNeedRead, S::kReadPending, {Op::kSendRead}},
    Row{S::kInvalid, E::kNeedWrite, S::kWritePending, {Op::kSendWrite}},
    Row{S::kShared, E::kNeedRead, S::kShared, {Op::kWake}},
    Row{S::kModified, E::kNeedRead, S::kModified, {Op::kWake}},
    Row{S::kModified, E::kNeedWrite, S::kModified, {Op::kWake}},
    Row{S::kModified, E::kFetch, S::kShared, {Op::kWriteProtect, Op::kReplyPageReadOnly}},
    Row{S::kReadPending, E::kNeedRead, S::kReadPending, {}},   // join
    Row{S::kReadPending, E::kNeedWrite, S::kReadPending, {}},  // join
    Row{S::kReadPending, E::kReadData, S::kShared, {Op::kInstallReadOnly}},
    Row{S::kWritePending, E::kNeedRead, S::kWritePending, {}},   // join
    Row{S::kWritePending, E::kNeedWrite, S::kWritePending, {}},  // join
    Row{S::kWritePending, E::kWriteGrant, S::kModified, {Op::kInstallWritable}},
};

constexpr bool is_reply(E event) noexcept {
    return event == E::kReadData || event == E::kWriteGrant || event == E::kUpgradeGrant ||
           event == E::kWritebackAck || event == E::kBusyRetry || event == E::kRedirect;
}

NodeAction action(NodeActionKind kind, Opcode opcode = Opcode::kHeartbeat, ReqId req = kNoReq,
                  bool page = false) {
    return NodeAction{kind, opcode, req, page, page};
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
            switch (op) {
                case Op::kNone:
                    break;
                case Op::kSendRead:
                case Op::kSendWrite:
                    push(action(NodeActionKind::kSend,
                                op == Op::kSendRead ? Opcode::kReadReq : Opcode::kWriteReq,
                                event.fresh));
                    step.current = event.fresh;
                    break;
                case Op::kWake:
                    push(action(NodeActionKind::kWake));
                    break;
                case Op::kInstallReadOnly:
                case Op::kInstallWritable:
                    push(action(op == Op::kInstallReadOnly ? NodeActionKind::kInstallReadOnly
                                                           : NodeActionKind::kInstallWritable));
                    step.current = kNoReq;  // the request is answered
                    break;
                case Op::kWriteProtect:
                    push(action(NodeActionKind::kWriteProtect));
                    break;
                case Op::kReplyPageReadOnly:
                    push(action(NodeActionKind::kReply, Opcode::kFetchData, event.req, true));
                    break;
            }
        }
        return step;
    }

    push(action(NodeActionKind::kAbort));
    return step;
}

}  // namespace paramesh
