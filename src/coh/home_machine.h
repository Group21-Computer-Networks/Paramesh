// The home machine of docs/STATE_MACHINES.md, sections 2 and 3: the directory of the pages
// this node is home for. Pure: no system calls, no clock; time arrives in the event.
//
// Interface file drafted by M0-7; see docs/INTERNAL_API.md, section 4. Implemented by M1-4
// (the rows marked M1), M2-1, AT-1 (the hold window) and M4-5 (segments on the move).
#ifndef PARAMESH_COH_HOME_MACHINE_H
#define PARAMESH_COH_HOME_MACHINE_H

#include "platform/ids.h"
#include "platform/result.h"
#include "wire/frame.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace paramesh {

// docs/STATE_MACHINES.md, section 2.1.
enum class HomeState : std::uint8_t { kUncached, kShared, kExclusive };
enum class HomeWhere : std::uint8_t { kZero, kRam, kSpill, kNone };
// Section 2.2: what a busy entry is waiting for.
enum class HomeWait : std::uint8_t { kIdle, kInv, kFetch, kFetchInv, kLoad };

// Section 2.3.
enum class HomeEventKind : std::uint8_t {
    kReadReq,
    kWriteReq,
    kUpgradeReq,
    kWriteback,
    kAtomicOp,
    kInvAck,
    kFetchData,
    kLoaded,
    kHoldExpired,
};

struct HomeEvent {
    HomeEventKind kind = HomeEventKind::kReadReq;
    PageId page;
    NodeId from;  // the node the message came from; kNoNode for kLoaded and kHoldExpired
    Slot slot;    // that node's copyset bit
    ReqId req;    // the message's request number
    Nanos now{};  // the time of the event
    // kAtomicOp only: where in the page the number is, and what to add.
    std::uint16_t offset_in_page = 0;
    std::uint64_t operand = 0;
};

// Where the bytes of a page-carrying reply come from.
enum class PageSource : std::uint8_t {
    kNone,      // the message carries no page data
    kZero,      // send the ZERO_PAGE flag and no data
    kHomeCopy,  // the home copy, from the store
    kReceived,  // the bytes of the FETCH_DATA or WRITEBACK that caused this step
};

enum class HomeActionKind : std::uint8_t {
    kSend,          // send `opcode` (INV, FETCH or FETCH_INV) for the page to node `to`
    kReply,         // send `opcode` to node `to`, repeating `req`, with the page from `source`
    kStore,         // keep the received bytes as the home copy
    kLoad,          // ask the store to read the home copy back; kLoaded follows
    kDropCopy,      // the home copy is no longer valid; the store may free it
    kApplyAtomic,   // add `operand` at `offset_in_page` of the home copy, then reply
                    // ATOMIC_RESULT with the old value to node `to`, repeating `req`
    kArmHoldTimer,  // deliver kHoldExpired at time `at`
    kReportThrash,  // log the page and the two nodes fighting over it: `to` and `other`
    kAbort,         // an impossible pair occurred: end the job with Status::kInternal
};

struct HomeAction {
    HomeActionKind kind = HomeActionKind::kAbort;
    PageId page;
    Opcode opcode = Opcode::kHeartbeat;
    NodeId to;
    NodeId other;
    ReqId req;
    PageSource source = PageSource::kNone;
    bool read_only = false;  // the reply has the READ_ONLY flag
    Nanos at{};
    std::uint16_t offset_in_page = 0;
    std::uint64_t operand = 0;
};

// The tunables the hold window needs (docs/PLAN.md, Tunables).
struct HomeConfig {
    std::uint32_t thrash_transfers = 8;  // thrash.threshold: this many transfers ...
    Nanos thrash_period{};               // ... within this long
    Nanos hold_initial{};                // thrash.hold_initial
    Nanos hold_max{};                    // thrash.hold_max
};

// What a caller may see of one directory entry.
struct HomeEntryView {
    HomeState state = HomeState::kUncached;
    HomeWhere where = HomeWhere::kZero;
    HomeWait wait = HomeWait::kIdle;
    NodeId owner;
    std::uint64_t copyset = 0;  // bit Slot::value set for each member that may hold a copy
    std::uint32_t version = 0;
    std::size_t queued = 0;  // requests waiting in the entry's queue
};

// The entries of every segment this node is home for. How they are stored is private to
// src/coh/.
class HomeDirectory;
struct HomeDirectoryDeleter {
    void operator()(HomeDirectory* directory) const noexcept;
};
using HomeDirectoryPtr = std::unique_ptr<HomeDirectory, HomeDirectoryDeleter>;

Result<HomeDirectoryPtr> home_open(const HomeConfig& config);

// Handles one event for one page and appends the actions to `out`, in the order they are to
// be carried out. One event can finish an operation and start the next queued ones, so the
// number of actions has no fixed bound.
void home_step(HomeDirectory& directory, const HomeEvent& event, std::vector<HomeAction>& out);

// A segment this node is home for from the start of the job: every page uncached and never
// written.
Result<void> home_add_segment(HomeDirectory& directory, SegmentId segment);
// Forgets a segment that has been migrated away.
void home_remove_segment(HomeDirectory& directory, SegmentId segment);
// True while any entry of the segment has an operation in progress.
bool home_segment_busy(const HomeDirectory& directory, SegmentId segment);
// Answers every queued request of the segment with REDIRECT to `new_home` (section 3,
// SERVING to FROZEN).
void home_freeze_segment(HomeDirectory& directory, SegmentId segment, NodeId new_home,
                         std::vector<HomeAction>& out);

// For SEG_MIGRATE: read an entry out, and install one that arrived.
HomeEntryView home_entry(const HomeDirectory& directory, PageId page);
Result<void> home_import_entry(HomeDirectory& directory, PageId page, const HomeEntryView& entry);

// The store moved a home copy between RAM and the spill file.
void home_set_where(HomeDirectory& directory, PageId page, HomeWhere where);
// A member left the job: clear its bit in every copyset.
void home_drop_member(HomeDirectory& directory, Slot slot);

}  // namespace paramesh

#endif  // PARAMESH_COH_HOME_MACHINE_H
