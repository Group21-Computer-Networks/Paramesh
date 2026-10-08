// Fuzz harness for the coherence state machines: the whole protocol of
// docs/STATE_MACHINES.md sections 1 and 2, without the hold window, spill and migration.
//
//   sim_fuzz --schedules N   run seeds 1 to N; exit 1 at the first that fails
//   sim_fuzz --seed S        run one seed and print every step
//   sim_fuzz --self-test     with a planted bug: some seed must fail, and fail identically twice
//
// Three simulated nodes run the real coh_step; node 1 also runs the real home_step for every
// page. They talk over a fake bus: one queue per ordered pair of nodes, kept in order as a TCP
// connection would, with the choice of which queue delivers next (and so every delay and
// every reordering between connections) taken from the seed. Any node may read, write, add
// atomically or evict any page at any step. After every step the invariants of
// docs/STATE_MACHINES.md, section 4, are checked. A run depends on its seed alone.

#include "coh/home_machine.h"
#include "coh/node_machine.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace paramesh;  // NOLINT(google-build-using-namespace): a test program

constexpr int kNodes = 3;  // node IDs 1 to 3; node n has copyset slot n - 1
constexpr int kHome = 1;
constexpr int kPages = 3;
constexpr int kSteps = 300;

struct Failure {
    std::string what;
};

[[noreturn]] void fail(const std::string& what) {
    throw Failure{what};
}

struct Msg {
    Opcode op = Opcode::kHeartbeat;
    int from = 0;
    int to = 0;
    int page = 0;
    ReqId req;
    std::uint32_t value = 0;  // stands for the page's 4,096 bytes
    bool zero = false;
};

struct NodePage {
    PageState state = PageState::kInvalid;
    ReqId current;
    std::optional<std::uint32_t> mapped;  // what the node's copy holds, if it has one
    bool writable = false;
    bool adding = false;  // a thread waits in pm_atomic_add for this page
};

// How often each message was delivered, over every schedule of the run.
std::array<std::uint64_t, 256>& delivered() {
    static std::array<std::uint64_t, 256> counts{};
    return counts;
}

bool is_stable(PageState state) {
    return state == PageState::kInvalid || state == PageState::kShared ||
           state == PageState::kModified;
}

class Sim {
public:
    Sim(std::uint64_t seed, bool planted_bug, bool verbose)
        : rng_(seed), bug_(planted_bug), verbose_(verbose) {
        auto opened = home_open({});
        dir_ = std::move(opened).value();
        static_cast<void>(home_add_segment(*dir_, SegmentId{0}));
    }

    // Runs one schedule. Throws Failure, with the step in the text, if anything goes wrong.
    void run() {
        for (step_ = 1; step_ <= kSteps; step_++) {
            random_move();
            check();
        }
        // No new faults: let everything in transit arrive, then nothing may still be waiting.
        while (deliver_one()) {
            step_++;
            check();
        }
        for (int p = 0; p < kPages; p++) {
            for (int n = 1; n <= kNodes; n++) {
                if (page(n, p).current != kNoReq || page(n, p).adding) {
                    fail("node " + std::to_string(n) + " still waits for page " +
                         std::to_string(p));
                }
            }
            const HomeEntryView entry = home_entry(*dir_, PageId{static_cast<std::uint64_t>(p)});
            if (entry.wait != HomeWait::kIdle || entry.queued != 0) {
                fail("the home is still busy with page " + std::to_string(p));
            }
        }
    }

    [[nodiscard]] int step() const { return step_; }

private:
    NodePage& page(int node, int p) {
        return nodes_.at(static_cast<std::size_t>(node - 1)).at(static_cast<std::size_t>(p));
    }
    std::deque<Msg>& queue(int from, int to) {
        return bus_.at(static_cast<std::size_t>((from - 1) * kNodes + to - 1));
    }
    std::uint64_t pick(std::uint64_t n) {
        return rng_() % n;
    }  // not a distribution: those differ between libraries
    static Slot slot_of(int node) { return Slot{static_cast<std::uint8_t>(node - 1)}; }
    static NodeId id_of(int node) { return NodeId{static_cast<std::uint16_t>(node)}; }

    void say(const char* what, int node, int p) const {
        if (verbose_) {
            std::printf("  step %3d: %-12s node %d page %d\n", step_, what, node, p);
        }
    }

    void random_move() {
        const int node = static_cast<int>(pick(kNodes)) + 1;
        const int p = static_cast<int>(pick(kPages));
        NodePage& mine = page(node, p);
        switch (pick(10)) {
            case 0:
            case 1:  // a thread reads: it faults unless the page is mapped
                if (!mine.mapped) {
                    say("read fault", node, p);
                    node_event(node, p, NodeEventKind::kNeedRead, nullptr);
                }
                break;
            case 2:
            case 3:  // a thread writes: at once if the page is writable, else it faults
                if (mine.writable) {
                    say("write", node, p);
                    mine.mapped = ++latest_.at(static_cast<std::size_t>(p));
                } else {
                    say("write fault", node, p);
                    node_event(node, p, NodeEventKind::kNeedWrite, nullptr);
                }
                break;
            case 4:  // a thread calls pm_atomic_add and waits for the result
                if (!mine.adding) {
                    say("atomic add", node, p);
                    mine.adding = true;
                    queue(node, kHome)
                        .push_back(
                            {Opcode::kAtomicOp, node, kHome, p, ReqId{++next_req_}, 0, false});
                }
                break;
            case 5:  // the local cache frees the page
                say("evict", node, p);
                node_event(node, p, NodeEventKind::kEvict, nullptr);
                break;
            default:  // deliver something
                deliver_one();
        }
    }

    // Delivers the head of one randomly chosen non-empty queue. False if all are empty.
    bool deliver_one() {
        std::vector<std::deque<Msg>*> ready;
        for (std::deque<Msg>& q : bus_) {
            if (!q.empty()) {
                ready.push_back(&q);
            }
        }
        if (ready.empty()) {
            return false;
        }
        std::deque<Msg>& q = *ready.at(pick(ready.size()));
        const Msg msg = q.front();
        q.pop_front();
        say("deliver", msg.to, msg.page);
        delivered().at(static_cast<std::size_t>(msg.op))++;
        switch (msg.op) {
            case Opcode::kReadReq:
                home_event(HomeEventKind::kReadReq, msg);
                break;
            case Opcode::kWriteReq:
                home_event(HomeEventKind::kWriteReq, msg);
                break;
            case Opcode::kUpgradeReq:
                home_event(HomeEventKind::kUpgradeReq, msg);
                break;
            case Opcode::kWriteback:
                home_event(HomeEventKind::kWriteback, msg);
                break;
            case Opcode::kAtomicOp:
                home_event(HomeEventKind::kAtomicOp, msg);
                break;
            case Opcode::kInvAck:
                home_event(HomeEventKind::kInvAck, msg);
                break;
            case Opcode::kFetchData:
                home_event(HomeEventKind::kFetchData, msg);
                break;
            case Opcode::kReadData:
                node_event(msg.to, msg.page, NodeEventKind::kReadData, &msg);
                break;
            case Opcode::kWriteGrant:
                node_event(msg.to, msg.page, NodeEventKind::kWriteGrant, &msg);
                break;
            case Opcode::kUpgradeGrant:
                node_event(msg.to, msg.page, NodeEventKind::kUpgradeGrant, &msg);
                break;
            case Opcode::kWritebackAck:
                node_event(msg.to, msg.page, NodeEventKind::kWritebackAck, &msg);
                break;
            case Opcode::kInv:
                node_event(msg.to, msg.page, NodeEventKind::kInv, &msg);
                break;
            case Opcode::kFetch:
                node_event(msg.to, msg.page, NodeEventKind::kFetch, &msg);
                break;
            case Opcode::kFetchInv:
                node_event(msg.to, msg.page, NodeEventKind::kFetchInv, &msg);
                break;
            case Opcode::kAtomicResult:
                page(msg.to, msg.page).adding = false;
                break;
            default:
                fail("the bus carried an opcode the harness does not know");
        }
        return true;
    }

    // One event for one node's page, and the actions it returns carried out.
    void node_event(int node, int p, NodeEventKind kind, const Msg* msg) {
        NodePage& mine = page(node, p);
        const bool was_writer =
            mine.state == PageState::kModified || mine.state == PageState::kWritebackPending;
        NodeEvent event;
        event.kind = kind;
        event.page = PageId{static_cast<std::uint64_t>(p)};
        event.req = msg != nullptr ? msg->req : kNoReq;
        event.fresh = ReqId{++next_req_};
        const NodeStep result = coh_step(mine.state, mine.current, event);
        mine.state = result.state;
        mine.current = result.current;
        for (std::size_t i = 0; i < result.action_count; i++) {
            const NodeAction& a = result.actions.at(i);
            switch (a.kind) {
                case NodeActionKind::kSend:
                case NodeActionKind::kReply:
                    if (a.with_page && !mine.mapped) {
                        fail("a node was asked to send a page it does not hold");
                    }
                    queue(node, kHome)
                        .push_back({a.opcode, node, kHome, p, a.req,
                                    a.with_page ? mine.mapped.value_or(0) : 0, false});
                    break;
                case NodeActionKind::kInstallReadOnly:
                case NodeActionKind::kInstallWritable:
                    // Zeros only for unwritten pages.
                    if (msg->zero && latest_.at(static_cast<std::size_t>(p)) != 0) {
                        fail("node " + std::to_string(node) + " installed zeros for page " +
                             std::to_string(p) + ", which has been written");
                    }
                    mine.mapped = msg->zero ? 0 : msg->value;
                    mine.writable = a.kind == NodeActionKind::kInstallWritable;
                    break;
                case NodeActionKind::kWriteProtect:
                    mine.writable = false;
                    break;
                case NodeActionKind::kAllowWrites:
                    mine.writable = true;
                    break;
                case NodeActionKind::kDiscard:
                    // No early discard: a page held for writing goes only once the home has
                    // its bytes or they are on their way.
                    if (was_writer && kind != NodeEventKind::kWritebackAck &&
                        kind != NodeEventKind::kFetchInv && kind != NodeEventKind::kInv) {
                        fail("early discard: node " + std::to_string(node) + " dropped page " +
                             std::to_string(p) + " before the home had it");
                    }
                    mine.mapped.reset();
                    mine.writable = false;
                    break;
                case NodeActionKind::kWake:
                    break;
                default:
                    fail("node " + std::to_string(node) + " aborted on page " + std::to_string(p));
            }
        }
    }

    // One event for the home, and its actions carried out.
    void home_event(HomeEventKind kind, const Msg& msg) {
        HomeEvent event;
        event.kind = kind;
        event.page = PageId{static_cast<std::uint64_t>(msg.page)};
        event.from = id_of(msg.from);
        event.slot = slot_of(msg.from);
        event.req = msg.req;
        std::vector<HomeAction> actions;
        home_step(*dir_, event, actions);
        const bool carries_page = msg.op == Opcode::kFetchData || msg.op == Opcode::kWriteback;
        std::optional<std::uint32_t>& copy = store_.at(static_cast<std::size_t>(msg.page));
        std::uint32_t& latest = latest_.at(static_cast<std::size_t>(msg.page));
        int stores = 0;
        for (const HomeAction& a : actions) {
            const int to = a.to.value;
            switch (a.kind) {
                case HomeActionKind::kSend:
                    queue(kHome, to).push_back(
                        {a.opcode, kHome, to, msg.page, ReqId{++next_req_}, 0, false});
                    break;
                case HomeActionKind::kReply: {
                    Msg reply{
                        a.opcode, kHome, to, msg.page, a.req, 0, a.source == PageSource::kZero};
                    if (a.source == PageSource::kHomeCopy) {
                        if (!copy) {
                            fail("the home was to send a copy it does not have");
                        }
                        reply.value = *copy;
                    } else if (a.source == PageSource::kReceived) {
                        if (!carries_page) {
                            fail("the home was to pass on bytes it did not receive");
                        }
                        reply.value = msg.value;
                    }
                    queue(kHome, to).push_back(reply);
                    break;
                }
                case HomeActionKind::kStore:
                    // The bytes are those of the message that caused this step, so there must
                    // be such bytes, and only one store.
                    if (!carries_page || ++stores > 1) {
                        fail("the home was to store bytes it does not have");
                    }
                    if (!bug_) {  // the planted bug: the home forgets to keep what it received
                        copy = msg.value;
                    }
                    break;
                case HomeActionKind::kDropCopy:
                    copy.reset();
                    break;
                case HomeActionKind::kApplyAtomic:
                    if (a.source == PageSource::kZero) {
                        copy = 0;
                    }
                    if (copy != latest) {
                        fail("lost update: an atomic add was applied to an old copy of page " +
                             std::to_string(msg.page));
                    }
                    copy = ++latest;
                    queue(kHome, to).push_back(
                        {Opcode::kAtomicResult, kHome, to, msg.page, a.req, 0, false});
                    break;
                default:
                    fail("the home aborted on page " + std::to_string(msg.page));
            }
        }
    }

    // docs/STATE_MACHINES.md, section 4.
    void check() {
        for (int p = 0; p < kPages; p++) {
            // Names are built only when something has failed.
            const auto name = [p] { return "page " + std::to_string(p); };
            const std::uint32_t latest = latest_.at(static_cast<std::size_t>(p));
            const HomeEntryView entry = home_entry(*dir_, PageId{static_cast<std::uint64_t>(p)});
            const std::optional<std::uint32_t>& copy = store_.at(static_cast<std::size_t>(p));
            if (copy.has_value() != (entry.where == HomeWhere::kRam)) {
                fail("the home's entry and its store disagree about " + name());
            }
            int writers = 0;
            int readable = 0;
            // No lost update: who holds the bytes last written.
            bool held = latest == 0 || copy == latest;
            for (int n = 1; n <= kNodes; n++) {
                const NodePage& mine = page(n, p);
                const auto who = [n] { return "node " + std::to_string(n); };
                const bool mapped = mine.state != PageState::kInvalid &&
                                    mine.state != PageState::kReadPending &&
                                    mine.state != PageState::kWritePending;
                if (mapped != mine.mapped.has_value() ||
                    mine.writable != (mine.state == PageState::kModified) ||
                    is_stable(mine.state) != (mine.current == kNoReq)) {
                    fail(who() + "'s state and its mapping or request disagree on " + name());
                }
                if (!mapped) {
                    continue;
                }
                readable++;
                writers += mine.state == PageState::kModified ? 1 : 0;
                // No stale read: every copy that can be read is the latest.
                if (mine.mapped != latest) {
                    fail("stale read: " + who() + " holds an old copy of " + name());
                }
                // The home knows the holders.
                if ((entry.copyset >> slot_of(n).value & 1U) == 0) {
                    fail("the home does not know " + who() + " holds " + name());
                }
                if (entry.state == HomeState::kExclusive && entry.owner != id_of(n)) {
                    fail(name() + " is exclusive but a node other than its owner holds it");
                }
                held = held || mine.state == PageState::kModified ||
                       mine.state == PageState::kWritebackPending ||
                       (mine.state == PageState::kUpgradePending && entry.owner == id_of(n));
            }
            if (writers > 0 && readable > 1) {
                fail("one writer or many readers: " + name() + " has both");
            }
            for (const std::deque<Msg>& q : bus_) {
                for (const Msg& m : q) {
                    held = held || (m.page == p && !m.zero && m.value == latest &&
                                    (m.op == Opcode::kWriteGrant || m.op == Opcode::kFetchData ||
                                     m.op == Opcode::kWriteback));
                }
            }
            if (!held) {
                fail("lost update: nobody holds the latest bytes of " + name());
            }
        }
    }

    std::mt19937_64 rng_;
    bool bug_;
    bool verbose_;
    int step_ = 0;
    std::uint64_t next_req_ = 0;
    HomeDirectoryPtr dir_;
    std::array<std::array<NodePage, kPages>, kNodes> nodes_{};
    std::array<std::deque<Msg>, static_cast<std::size_t>(kNodes) * kNodes> bus_;
    std::array<std::optional<std::uint32_t>, kPages> store_{};  // the home's copies
    std::array<std::uint32_t, kPages> latest_{};  // the value last written to each page
};

// Runs one seed. Empty if it passed, else "step N: what went wrong".
std::string run_seed(std::uint64_t seed, bool planted_bug, bool verbose) {
    Sim sim{seed, planted_bug, verbose};
    try {
        sim.run();
    } catch (const Failure& failure) {
        return "step " + std::to_string(sim.step()) + ": " + failure.what;
    }
    return {};
}

int self_test() {
    for (std::uint64_t seed = 1; seed <= 1000; seed++) {
        const std::string first = run_seed(seed, true, false);
        if (first.empty()) {
            continue;
        }
        const std::string again = run_seed(seed, true, false);
        std::printf("planted bug caught at seed %llu, %s\n", static_cast<unsigned long long>(seed),
                    first.c_str());
        if (again != first) {
            std::printf("FAILED: the same seed then gave: %s\n", again.c_str());
            return 1;
        }
        if (!run_seed(seed, false, false).empty()) {
            std::printf("FAILED: seed %llu also fails without the planted bug\n",
                        static_cast<unsigned long long>(seed));
            return 1;
        }
        std::printf("the same seed fails identically again, and passes without the bug\n");
        return 0;
    }
    std::printf("FAILED: 1000 seeds ran with a planted bug and none caught it\n");
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() == 1 && args[0] == "--self-test") {
        return self_test();
    }
    if (args.size() == 2 && args[0] == "--seed") {
        const std::string result =
            run_seed(std::strtoull(args[1].c_str(), nullptr, 10), false, true);
        std::printf("%s\n", result.empty() ? "passed" : result.c_str());
        return result.empty() ? 0 : 1;
    }
    if (args.size() == 2 && args[0] == "--schedules") {
        const std::uint64_t count = std::strtoull(args[1].c_str(), nullptr, 10);
        for (std::uint64_t seed = 1; seed <= count; seed++) {
            const std::string result = run_seed(seed, false, false);
            if (!result.empty()) {
                std::printf("seed %llu FAILED, %s\nreproduce: sim_fuzz --seed %llu\n",
                            static_cast<unsigned long long>(seed), result.c_str(),
                            static_cast<unsigned long long>(seed));
                return 1;
            }
        }
        std::printf("%llu schedules passed\n", static_cast<unsigned long long>(count));
        // What the schedules exercised, so a quiet path shows.
        static constexpr std::array<std::pair<Opcode, const char*>, 15> kNames{{
            {Opcode::kReadReq, "READ_REQ"},
            {Opcode::kWriteReq, "WRITE_REQ"},
            {Opcode::kUpgradeReq, "UPGRADE_REQ"},
            {Opcode::kReadData, "READ_DATA"},
            {Opcode::kWriteGrant, "WRITE_GRANT"},
            {Opcode::kUpgradeGrant, "UPGRADE_GRANT"},
            {Opcode::kInv, "INV"},
            {Opcode::kInvAck, "INV_ACK"},
            {Opcode::kFetch, "FETCH"},
            {Opcode::kFetchInv, "FETCH_INV"},
            {Opcode::kFetchData, "FETCH_DATA"},
            {Opcode::kWriteback, "WRITEBACK"},
            {Opcode::kWritebackAck, "WRITEBACK_ACK"},
            {Opcode::kAtomicOp, "ATOMIC_OP"},
            {Opcode::kAtomicResult, "ATOMIC_RESULT"},
        }};
        std::printf("delivered:");
        for (const auto& [opcode, name] : kNames) {
            std::printf(
                " %s %llu", name,
                static_cast<unsigned long long>(delivered().at(static_cast<std::size_t>(opcode))));
        }
        std::printf("\n");
        return 0;
    }
    std::printf("usage: sim_fuzz --schedules N | --seed S | --self-test\n");
    return 2;
}
