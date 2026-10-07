// Fuzz harness for the coherence state machines (M1 subset).
//
//   sim_fuzz --schedules N   run seeds 1 to N; exit 1 at the first that fails
//   sim_fuzz --seed S        run one seed and print every step
//   sim_fuzz --self-test     with a planted bug: some seed must fail, and fail identically twice
//
// Three simulated nodes run the real coh_step; node 1 also runs the real home_step for every
// page. They talk over a fake bus: one queue per ordered pair of nodes, kept in order as a TCP
// connection would, with the choice of which queue delivers next (and so every delay and
// every reordering between connections) taken from the seed. After every step the invariants
// of docs/STATE_MACHINES.md, section 4, are checked. A run depends on its seed alone.

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
#include <vector>

namespace {

using namespace paramesh;  // NOLINT(google-build-using-namespace): a test program

constexpr int kNodes = 3;  // node IDs 1 to 3; node n has copyset slot n - 1
constexpr int kHome = 1;
constexpr int kPages = 3;
// Who writes each page first, or 0 for a page nobody writes. In M1 the home refuses a write to
// a page others hold, so a page's readers start only after its writer has finished.
constexpr std::array<int, kPages> kWriter = {2, 1, 0};
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
};

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
        for (int n = 1; n <= kNodes; n++) {
            for (int p = 0; p < kPages; p++) {
                if (page(n, p).current != kNoReq) {
                    fail("node " + std::to_string(n) + " still waits for page " +
                         std::to_string(p));
                }
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
        const int writer = kWriter.at(static_cast<std::size_t>(p));
        switch (pick(4)) {
            case 0:  // a read fault, once the page's writer is done (or it has none)
                if (writer == 0 || published_.at(static_cast<std::size_t>(p))) {
                    say("read fault", node, p);
                    node_event(node, p, NodeEventKind::kNeedRead, nullptr);
                }
                break;
            case 1:  // the writer's write fault, then its write, then it lets the readers in
                if (node == writer && !published_.at(static_cast<std::size_t>(p))) {
                    NodePage& mine = page(node, p);
                    if (mine.state == PageState::kModified) {
                        say("write", node, p);
                        mine.mapped = ++latest_.at(static_cast<std::size_t>(p));
                        published_.at(static_cast<std::size_t>(p)) = pick(2) == 0;
                    } else {
                        say("write fault", node, p);
                        node_event(node, p, NodeEventKind::kNeedWrite, nullptr);
                    }
                }
                break;
            default:  // deliver something, twice as often as each kind of fault
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
        switch (msg.op) {
            case Opcode::kReadReq:
                home_event(HomeEventKind::kReadReq, msg);
                break;
            case Opcode::kWriteReq:
                home_event(HomeEventKind::kWriteReq, msg);
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
            case Opcode::kFetch:
                node_event(msg.to, msg.page, NodeEventKind::kFetch, &msg);
                break;
            default:
                fail("the bus carried an opcode M1 does not use");
        }
        return true;
    }

    // One event for one node's page, and the actions it returns carried out.
    void node_event(int node, int p, NodeEventKind kind, const Msg* msg) {
        NodePage& mine = page(node, p);
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
                    queue(node, kHome).push_back({a.opcode, node, kHome, p, a.req, 0, false});
                    break;
                case NodeActionKind::kReply:
                    if (!mine.mapped) {
                        fail("a node was asked to send a page it does not hold");
                    }
                    queue(node, kHome)
                        .push_back({a.opcode, node, kHome, p, a.req, *mine.mapped, false});
                    break;
                case NodeActionKind::kInstallReadOnly:
                case NodeActionKind::kInstallWritable:
                    mine.mapped = msg->zero ? 0 : msg->value;
                    mine.writable = a.kind == NodeActionKind::kInstallWritable;
                    break;
                case NodeActionKind::kWriteProtect:
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
        std::optional<std::uint32_t>& copy = store_.at(static_cast<std::size_t>(msg.page));
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
                        reply.value = msg.value;
                    }
                    queue(kHome, to).push_back(reply);
                    break;
                }
                case HomeActionKind::kStore:
                    if (!bug_) {  // the planted bug: the home forgets to keep what it fetched
                        copy = msg.value;
                    }
                    break;
                case HomeActionKind::kDropCopy:
                    copy.reset();
                    break;
                default:
                    fail("the home aborted on page " + std::to_string(msg.page));
            }
        }
    }

    // docs/STATE_MACHINES.md, section 4.
    void check() {
        for (int p = 0; p < kPages; p++) {
            const std::uint32_t latest = latest_.at(static_cast<std::size_t>(p));
            const HomeEntryView entry = home_entry(*dir_, PageId{static_cast<std::uint64_t>(p)});
            const std::optional<std::uint32_t>& copy = store_.at(static_cast<std::size_t>(p));
            int writers = 0;
            int holders = 0;
            bool held = latest == 0 || copy == latest;
            for (int n = 1; n <= kNodes; n++) {
                const NodePage& mine = page(n, p);
                const bool has_copy =
                    mine.state == PageState::kShared || mine.state == PageState::kModified;
                if (has_copy != mine.mapped.has_value()) {
                    fail("a node's state and its mapping disagree");
                }
                if (!has_copy) {
                    continue;
                }
                holders++;
                writers += mine.state == PageState::kModified ? 1 : 0;
                if (*mine.mapped != latest) {
                    fail("stale read: node " + std::to_string(n) + " holds an old copy of page " +
                         std::to_string(p));
                }
                if ((entry.copyset >> slot_of(n).value & 1U) == 0) {
                    fail("the home does not know node " + std::to_string(n) + " holds page " +
                         std::to_string(p));
                }
                if (entry.state == HomeState::kExclusive && entry.owner != id_of(n)) {
                    fail("a page is exclusive but a node other than its owner holds it");
                }
                held = true;
            }
            if (writers > 0 && holders > 1) {
                fail("one writer or many readers: page " + std::to_string(p) + " has both");
            }
            for (const std::deque<Msg>& q : bus_) {
                for (const Msg& m : q) {
                    held = held || (m.page == p && !m.zero && m.value == latest &&
                                    (m.op == Opcode::kWriteGrant || m.op == Opcode::kFetchData ||
                                     m.op == Opcode::kReadData));
                }
            }
            if (!held) {
                fail("lost update: nobody holds the latest bytes of page " + std::to_string(p));
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
    std::array<bool, kPages> published_{};        // the writer is done; readers may start
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
        return 0;
    }
    std::printf("usage: sim_fuzz --schedules N | --seed S | --self-test\n");
    return 2;
}
