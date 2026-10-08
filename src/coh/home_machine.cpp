// The home machine of docs/STATE_MACHINES.md, sections 2.3 to 2.5 and 2.7: every row but the
// hold window of section 2.6, which is AT-1's. Until then no hold runs, so the rows with
// "hold running" never apply and HOLD_EXPIRED does nothing.
//
// An impossible pair gives one kAbort action.

#include "coh/home_machine.h"

#include <array>
#include <cstddef>
#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace paramesh {

namespace {

struct Entry {
    HomeState state = HomeState::kUncached;
    HomeWhere where = HomeWhere::kZero;
    HomeWait wait = HomeWait::kIdle;
    NodeId owner;
    std::uint64_t copyset = 0;
    std::uint64_t pending = 0;  // W_INV: the slots whose INV_ACK has not come
    std::uint32_t version = 0;
    HomeEvent op;                 // the request being handled, while `wait` is not kIdle
    std::deque<HomeEvent> waitq;  // requests that arrived meanwhile, in order
};

// The node behind each copyset bit, learnt from the requests themselves: a node enters a
// copyset only through a request, and every request carries both.
using Nodes = std::array<NodeId, kMaxSlots>;

constexpr std::uint64_t bit(Slot slot) noexcept {
    return std::uint64_t{1} << slot.value;
}

constexpr bool is_request(HomeEventKind kind) noexcept {
    return kind == HomeEventKind::kReadReq || kind == HomeEventKind::kWriteReq ||
           kind == HomeEventKind::kUpgradeReq || kind == HomeEventKind::kWriteback ||
           kind == HomeEventKind::kAtomicOp;
}

HomeAction action(HomeActionKind kind, PageId page) {
    HomeAction a;
    a.kind = kind;
    a.page = page;
    return a;
}

HomeAction send(PageId page, Opcode opcode, NodeId to) {
    HomeAction a = action(HomeActionKind::kSend, page);
    a.opcode = opcode;
    a.to = to;
    return a;
}

HomeAction reply(const HomeEvent& request, Opcode opcode, PageSource source, bool read_only) {
    HomeAction a = action(HomeActionKind::kReply, request.page);
    a.opcode = opcode;
    a.to = request.from;
    a.req = request.req;
    a.source = source;
    a.read_only = read_only;
    return a;
}

// The bytes a grant carries: the ones just received, or the home's own copy.
PageSource source_of(const Entry& e, bool received) noexcept {
    if (received) {
        return PageSource::kReceived;
    }
    return e.where == HomeWhere::kZero ? PageSource::kZero : PageSource::kHomeCopy;
}

// The page left its owner: the home handled its FETCH_DATA or WRITEBACK.
void store_received(Entry& e, PageId page, std::vector<HomeAction>& out) {
    out.push_back(action(HomeActionKind::kStore, page));
    e.where = HomeWhere::kRam;
    e.owner = kNoNode;
}

// Section 2.3: the step that ends the operation in e.op. `received` says the bytes of the
// event that led here are the page. A step that needs a spilled home copy loads it first and
// runs again on LOADED.
void finish(Entry& e, bool received, std::vector<HomeAction>& out) {
    const HomeEvent& op = e.op;
    const bool upgrade =
        op.kind == HomeEventKind::kUpgradeReq && (e.copyset & bit(op.slot)) != 0;  // no bytes
    if (!received && !upgrade && e.where == HomeWhere::kSpill) {
        out.push_back(action(HomeActionKind::kLoad, op.page));
        e.wait = HomeWait::kLoad;
        return;
    }
    e.wait = HomeWait::kIdle;
    if (op.kind == HomeEventKind::kReadReq) {  // grant read
        out.push_back(reply(op, Opcode::kReadData, source_of(e, received), true));
        e.state = HomeState::kShared;
        e.copyset |= bit(op.slot);
        return;
    }
    if (op.kind == HomeEventKind::kAtomicOp) {  // apply atomic
        HomeAction apply = action(HomeActionKind::kApplyAtomic, op.page);
        apply.opcode = Opcode::kAtomicResult;
        apply.to = op.from;
        apply.req = op.req;
        apply.source = source_of(e, false);  // kZero: start from a page of zeros
        apply.offset_in_page = op.offset_in_page;
        apply.operand = op.operand;
        out.push_back(apply);
        e.state = HomeState::kUncached;
        e.where = HomeWhere::kRam;
        e.copyset = 0;
        e.version++;
        return;
    }
    // grant write
    out.push_back(upgrade ? reply(op, Opcode::kUpgradeGrant, PageSource::kNone, false)
                          : reply(op, Opcode::kWriteGrant, source_of(e, received), false));
    if (e.where == HomeWhere::kRam || e.where == HomeWhere::kSpill) {
        out.push_back(action(HomeActionKind::kDropCopy, op.page));
    }
    e.state = HomeState::kExclusive;
    e.where = HomeWhere::kNone;
    e.owner = op.from;
    e.copyset = bit(op.slot);
    e.version++;
}

// Section 2.4: a request against an idle entry.
void start(Entry& e, const HomeEvent& event, const Nodes& nodes, std::vector<HomeAction>& out) {
    const bool exclusive = e.state == HomeState::kExclusive;
    const bool from_owner = exclusive && event.from == e.owner;
    if (event.kind == HomeEventKind::kWriteback) {
        if (from_owner) {  // the evictor stays a reader until the acknowledgement reaches it
            store_received(e, event.page, out);
            e.state = HomeState::kShared;
        }  // otherwise section 2.7: late, and the data is dropped
        out.push_back(reply(event, Opcode::kWritebackAck, PageSource::kNone, false));
        return;
    }
    if (from_owner && event.kind != HomeEventKind::kAtomicOp) {
        out.push_back(action(HomeActionKind::kAbort, event.page));  // H2
        return;
    }
    e.op = event;
    if (exclusive) {
        const bool keeps_copy = event.kind == HomeEventKind::kReadReq;
        out.push_back(send(event.page, keeps_copy ? Opcode::kFetch : Opcode::kFetchInv, e.owner));
        e.wait = keeps_copy ? HomeWait::kFetch : HomeWait::kFetchInv;
        return;
    }
    // Who has to lose a read copy first: nobody for a read, every other holder for a write,
    // every holder for an atomic operation.
    std::uint64_t losers = 0;
    if (event.kind == HomeEventKind::kAtomicOp) {
        losers = e.copyset;
    } else if (event.kind != HomeEventKind::kReadReq) {
        losers = e.copyset & ~bit(event.slot);
    }
    if (losers == 0) {
        finish(e, false, out);
        return;
    }
    for (std::size_t slot = 0; slot < kMaxSlots; slot++) {
        if ((losers >> slot & 1U) != 0) {
            out.push_back(send(event.page, Opcode::kInv, nodes.at(slot)));
        }
    }
    e.pending = losers;
    e.wait = HomeWait::kInv;
}

// Section 2.5: an answer the entry may be waiting for.
void answer(Entry& e, const HomeEvent& event, std::vector<HomeAction>& out) {
    switch (event.kind) {
        case HomeEventKind::kInvAck:
            if (e.wait == HomeWait::kInv && (e.pending & bit(event.slot)) != 0) {
                e.pending &= ~bit(event.slot);
                e.copyset &= ~bit(event.slot);
                if (e.pending == 0) {
                    finish(e, false, out);
                }
                return;
            }
            break;
        case HomeEventKind::kFetchData:
            if (event.from != e.owner) {
                break;
            }
            if (e.wait == HomeWait::kFetch) {  // the owner keeps a read copy
                store_received(e, event.page, out);
                e.state = HomeState::kShared;  // `copyset` is already the old owner alone
                finish(e, true, out);
                return;
            }
            if (e.wait == HomeWait::kFetchInv) {  // the owner gave the page up
                const bool atomic = e.op.kind == HomeEventKind::kAtomicOp;
                if (atomic) {
                    store_received(e, event.page, out);
                }
                e.owner = kNoNode;
                e.copyset = 0;
                finish(e, !atomic, out);
                return;
            }
            break;
        case HomeEventKind::kLoaded:
            if (e.wait == HomeWait::kLoad) {
                e.where = HomeWhere::kRam;
                finish(e, false, out);
                return;
            }
            break;
        case HomeEventKind::kHoldExpired:
            return;  // no hold runs before AT-1
        default:
            break;
    }
    out.push_back(action(HomeActionKind::kAbort, event.page));  // H1, H3
}

}  // namespace

// ponytail: one map node per touched page, each with its own queue; a flat per-segment array
// with a side table for busy entries if 1M pages make this too heavy.
class HomeDirectory {
public:
    std::unordered_set<std::uint32_t> segments;
    std::unordered_map<std::uint64_t, Entry> entries;
    Nodes nodes{};
};

void HomeDirectoryDeleter::operator()(HomeDirectory* directory) const noexcept {
    // Hand it back to a unique_ptr, which destroys it: no raw delete (engineering rules).
    const std::unique_ptr<HomeDirectory> owned{directory};
}

Result<HomeDirectoryPtr> home_open(const HomeConfig& /*config*/) {
    return HomeDirectoryPtr{std::make_unique<HomeDirectory>().release()};
}

Result<void> home_add_segment(HomeDirectory& directory, SegmentId segment) {
    directory.segments.insert(segment.value);
    return {};
}

HomeEntryView home_entry(const HomeDirectory& directory, PageId page) {
    HomeEntryView view;
    const auto found = directory.entries.find(page.value);
    if (found != directory.entries.end()) {
        const Entry& e = found->second;
        view =
            HomeEntryView{e.state, e.where, e.wait, e.owner, e.copyset, e.version, e.waitq.size()};
    }
    return view;
}

void home_set_where(HomeDirectory& directory, PageId page, HomeWhere where) {
    directory.entries[page.value].where = where;
}

void home_step(HomeDirectory& directory, const HomeEvent& event, std::vector<HomeAction>& out) {
    if (!directory.segments.contains(segment_of(event.page).value) ||
        event.slot.value >= kMaxSlots) {
        out.push_back(action(HomeActionKind::kAbort, event.page));  // not the page's home
        return;
    }
    Entry& e = directory.entries[event.page.value];
    if (!is_request(event.kind)) {
        answer(e, event, out);
    } else if (e.wait != HomeWait::kIdle) {
        e.waitq.push_back(event);  // home rule 1: one request per page at a time
        return;
    } else {
        directory.nodes.at(event.slot.value) = event.from;
        start(e, event, directory.nodes, out);
    }
    while (e.wait == HomeWait::kIdle && !e.waitq.empty()) {
        const HomeEvent next = e.waitq.front();
        e.waitq.pop_front();
        directory.nodes.at(next.slot.value) = next.from;
        start(e, next, directory.nodes, out);
    }
}

}  // namespace paramesh
