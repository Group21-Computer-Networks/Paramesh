// The home machine of docs/STATE_MACHINES.md, sections 2.4 and 2.5, for the rows marked M1.
// Any other request ends the job as "not supported before M2" (section 5).
//
// How a kAbort action says which: its `opcode` is the refused request's opcode when the row
// belongs to a later milestone, and Opcode::kHeartbeat when the pair is impossible.

#include "coh/home_machine.h"

#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace paramesh {

namespace {

struct Entry {
    HomeState state = HomeState::kUncached;
    HomeWhere where = HomeWhere::kZero;
    HomeWait wait = HomeWait::kIdle;
    NodeId owner;
    Slot owner_slot;
    std::uint64_t copyset = 0;
    std::uint32_t version = 0;
    HomeEvent op;                 // the request being handled, while `wait` is not kIdle
    std::deque<HomeEvent> waitq;  // requests that arrived meanwhile, in order
};

constexpr std::uint64_t bit(Slot slot) noexcept {
    return std::uint64_t{1} << slot.value;
}

constexpr bool is_request(HomeEventKind kind) noexcept {
    return kind == HomeEventKind::kReadReq || kind == HomeEventKind::kWriteReq ||
           kind == HomeEventKind::kUpgradeReq || kind == HomeEventKind::kWriteback ||
           kind == HomeEventKind::kAtomicOp;
}

constexpr Opcode opcode_of(HomeEventKind kind) noexcept {
    switch (kind) {
        case HomeEventKind::kReadReq:
            return Opcode::kReadReq;
        case HomeEventKind::kWriteReq:
            return Opcode::kWriteReq;
        case HomeEventKind::kUpgradeReq:
            return Opcode::kUpgradeReq;
        case HomeEventKind::kWriteback:
            return Opcode::kWriteback;
        case HomeEventKind::kAtomicOp:
            return Opcode::kAtomicOp;
        default:
            return Opcode::kHeartbeat;
    }
}

HomeAction abort_action(PageId page, Opcode refused = Opcode::kHeartbeat) {
    HomeAction a;
    a.kind = HomeActionKind::kAbort;
    a.page = page;
    a.opcode = refused;
    return a;
}

HomeAction reply(const HomeEvent& request, Opcode opcode, PageSource source, bool read_only) {
    HomeAction a;
    a.kind = HomeActionKind::kReply;
    a.page = request.page;
    a.opcode = opcode;
    a.to = request.from;
    a.req = request.req;
    a.source = source;
    a.read_only = read_only;
    return a;
}

// The bytes a grant carries when they come from the home's own copy.
PageSource home_source(const Entry& e) noexcept {
    return e.where == HomeWhere::kZero ? PageSource::kZero : PageSource::kHomeCopy;
}

void grant_read(Entry& e, const HomeEvent& request, PageSource source,
                std::vector<HomeAction>& out) {
    out.push_back(reply(request, Opcode::kReadData, source, true));
    e.state = HomeState::kShared;
    e.copyset |= bit(request.slot);
}

void grant_write(Entry& e, const HomeEvent& request, std::vector<HomeAction>& out) {
    out.push_back(reply(request, Opcode::kWriteGrant, home_source(e), false));
    if (e.where == HomeWhere::kRam) {
        HomeAction drop;
        drop.kind = HomeActionKind::kDropCopy;
        drop.page = request.page;
        out.push_back(drop);
    }
    e.state = HomeState::kExclusive;
    e.where = HomeWhere::kNone;
    e.owner = request.from;
    e.owner_slot = request.slot;
    e.copyset = bit(request.slot);
    e.version++;
}

// One event against an entry that is free to take it.
void handle(Entry& e, const HomeEvent& event, std::vector<HomeAction>& out) {
    const bool others = (e.copyset & ~bit(event.slot)) != 0;
    switch (event.kind) {
        case HomeEventKind::kReadReq:
            if (e.state != HomeState::kExclusive) {
                grant_read(e, event, home_source(e), out);
            } else if (event.from == e.owner) {
                out.push_back(
                    abort_action(event.page));  // H2: the owner does not ask for its own page
            } else {
                HomeAction fetch;
                fetch.kind = HomeActionKind::kSend;
                fetch.page = event.page;
                fetch.opcode = Opcode::kFetch;
                fetch.to = e.owner;
                out.push_back(fetch);
                e.wait = HomeWait::kFetch;
                e.op = event;
            }
            return;
        case HomeEventKind::kWriteReq:
            if (e.state == HomeState::kUncached || (e.state == HomeState::kShared && !others)) {
                grant_write(e, event, out);
            } else {
                out.push_back(
                    abort_action(event.page, Opcode::kWriteReq));  // needs INV or FETCH_INV: M2
            }
            return;
        case HomeEventKind::kFetchData:
            if (e.wait != HomeWait::kFetch || event.from != e.owner) {
                out.push_back(abort_action(event.page));  // H1, H3
                return;
            }
            {
                HomeAction store;
                store.kind = HomeActionKind::kStore;
                store.page = event.page;
                out.push_back(store);
            }
            e.where = HomeWhere::kRam;
            e.state = HomeState::kShared;
            e.copyset = bit(e.owner_slot);
            e.owner = kNoNode;
            e.wait = HomeWait::kIdle;
            grant_read(e, e.op, PageSource::kReceived, out);
            return;
        default:
            out.push_back(abort_action(event.page, opcode_of(event.kind)));
            return;
    }
}

}  // namespace

// ponytail: one map node per touched page, each with its own queue; a flat per-segment array
// with a side table for busy entries if 1M pages make this too heavy.
class HomeDirectory {
public:
    std::unordered_set<std::uint32_t> segments;
    std::unordered_map<std::uint64_t, Entry> entries;
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

void home_step(HomeDirectory& directory, const HomeEvent& event, std::vector<HomeAction>& out) {
    if (!directory.segments.contains(segment_of(event.page).value)) {
        out.push_back(abort_action(event.page));  // this node is not the page's home
        return;
    }
    Entry& e = directory.entries[event.page.value];
    if (e.wait != HomeWait::kIdle && is_request(event.kind)) {
        e.waitq.push_back(event);  // home rule 1: one request per page at a time
        return;
    }
    handle(e, event, out);
    while (e.wait == HomeWait::kIdle && !e.waitq.empty()) {
        const HomeEvent next = e.waitq.front();
        e.waitq.pop_front();
        handle(e, next, out);
    }
}

}  // namespace paramesh
