// The node machine of docs/STATE_MACHINES.md, section 1, as one pure function.
// No system calls, no clock, no allocation.
//
// Interface file drafted by M0-7; see docs/INTERNAL_API.md, section 4. Implemented by M1-3
// (the rows marked M1) and M2-2 (the rest).
#ifndef PARAMESH_COH_NODE_MACHINE_H
#define PARAMESH_COH_NODE_MACHINE_H

#include "platform/ids.h"
#include "wire/frame.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace paramesh {

// One byte per page of the region. Names follow docs/STATE_MACHINES.md, section 1.1.
enum class PageState : std::uint8_t {
    kInvalid,           // I
    kShared,            // S
    kModified,          // M
    kReadPending,       // I→S
    kWritePending,      // I→M
    kUpgradePending,    // S→M
    kWritebackPending,  // M→I
};

// docs/STATE_MACHINES.md, section 1.2.
enum class NodeEventKind : std::uint8_t {
    kNeedRead,
    kNeedWrite,
    kEvict,
    kReadData,
    kWriteGrant,
    kUpgradeGrant,
    kWritebackAck,
    kBusyRetry,
    kRedirect,
    kResend,
    kInv,
    kFetch,
    kFetchInv,
};

struct NodeEvent {
    NodeEventKind kind = NodeEventKind::kNeedRead;
    PageId page;
    // For a reply: the request it answers. For kInv, kFetch and kFetchInv: the home's request,
    // to be repeated in the answer. Otherwise kNoReq.
    ReqId req;
    // The caller's next unused request number. coh_step uses it if, and only if, the step
    // sends a request; the caller then never offers that number again.
    ReqId fresh;
};

// docs/STATE_MACHINES.md, section 1.3. The caller carries these out, in order.
enum class NodeActionKind : std::uint8_t {
    kSend,             // send `opcode` to the page's home with request number `req`
    kReply,            // send `opcode` back to the home, repeating `req`
    kInstallReadOnly,  // map the page from the reply (zeros if it had ZERO_PAGE), write-protected
    kInstallWritable,  // the same, writable
    kWriteProtect,
    kAllowWrites,
    kDiscard,
    kWake,
    kArmResend,  // deliver kResend after the back-off, or once the redirect's map is applied
    kAbort,      // an impossible pair occurred: end the job with Status::kInternal
};

struct NodeAction {
    NodeActionKind kind = NodeActionKind::kWake;
    Opcode opcode = Opcode::kHeartbeat;  // for kSend and kReply
    ReqId req;                           // for kSend and kReply
    bool with_page = false;              // the message carries this node's copy of the page
    bool read_only = false;              // the message has the READ_ONLY flag
};

inline constexpr std::size_t kMaxNodeActions = 4;

struct NodeStep {
    PageState state = PageState::kInvalid;  // the page's state after the event
    ReqId current;                          // its current request afterwards, or kNoReq
    bool dropped = false;                   // Rule 0: a reply to a superseded request; count it
    std::uint8_t action_count = 0;
    std::array<NodeAction, kMaxNodeActions> actions;
};

// state and current are the page's state and current request before the event.
NodeStep coh_step(PageState state, ReqId current, const NodeEvent& event) noexcept;

}  // namespace paramesh

#endif  // PARAMESH_COH_NODE_MACHINE_H
