// One job process: the transport, the memory engine, the node and home machines, the home
// store and the lock manager wired together behind paramesh.h. So far: start-up without pmd,
// read and write sharing, locks, barriers, atomic adds and tasks, clean end, and abort on a lost
// node or a reply timeout. Not yet: pmd (M3-7), eviction and spill (M4).
//
// Threads (docs/INTERNAL_API.md, section 1): every piece of protocol state below is touched
// only on the network thread. The fault-handler thread and the application's threads hand
// work to it through a mutex-guarded queue and a zero-delay timer.

#include "coh/home_machine.h"
#include "coh/node_machine.h"
#include "lib/prefetch.h"
#include "lib/test_hook.h"
#include "mem/memory_engine.h"
#include "net/frame_io.h"
#include "net/transport.h"
#include "platform/factory.h"
#include "rt/region_allocator.h"
#include "rt/registry.h"
#include "rt/runtime.h"
#include "rt/sync.h"
#include "rt/tasks.h"
#include "store/home_store.h"
#include "store/local_cache.h"
#include "store/placement.h"
#include "wire/payloads.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <paramesh.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace paramesh {
namespace {

constexpr std::uint8_t kLauncher = 1;
constexpr std::uint8_t kWorker = 2;
constexpr Nanos kLinger =
    std::chrono::milliseconds{150};  // lets a JOB_END leave before the process exits
constexpr Nanos kFinalizeWait = std::chrono::seconds{2};
constexpr Nanos kLeaveGrace = std::chrono::seconds{10};  // fixed by the design (docs/PLAN.md)
constexpr Nanos kLeavePoll = std::chrono::milliseconds{20};
constexpr std::size_t kMigrateBatch = 32;                     // pages with data in one SEG_MIGRATE
constexpr std::uint64_t kAddRetry = std::uint64_t{1} << 62U;  // retries_ keys of atomic adds

struct Member {
    NodeId node;
    Slot slot;
    Endpoint at;
};

struct Settings {
    std::uint8_t role = 0;
    JobId job;
    NodeId self;
    Endpoint listen;
    std::vector<Member> members;  // launcher first; slots in this order
    Nanos start_timeout = std::chrono::seconds{10};
    std::string pmd_socket;  // PARAMESH_PMD_SOCKET; empty when the job runs without pmd
    LogLevel log_level = LogLevel::kWarn;  // log.level
    // The task queue's tunables (task.*), with the plan's defaults; threads, slots and region
    // are filled in at set-up. PARAMESH_CFG_TASK_AFFINITY=0 turns M3-3 off.
    TasksConfig tasks;
    std::uint64_t cache_share = 25;  // mem.cache_share: percent of the RAM cap
    // cache.policy = cost: a victim is the cheapest to lose of cache.victim_window oldest
    // pages; fifo, the default, is a window of 1 (X-cache-policy).
    std::uint32_t cache_window = 1;
    // prefetch.depth: how many pages a stream of read faults runs ahead (8; 0 turns it off);
    // prefetch.streams: how many such streams are followed at once (X-prefetch).
    std::uint32_t prefetch_depth = 8;
    std::uint32_t prefetch_streams = 4;
    // coh.busy_retry_backoff: PARAMESH_CFG_COH_BUSY_RETRY_BACKOFF sets the first wait, in ms.
    Nanos busy_initial = std::chrono::milliseconds{5};
    Nanos busy_max = std::chrono::milliseconds{100};
    // Tests only (PARAMESH_TEST_MOVE_DELAY_MS): a frozen segment is sent this much later, so
    // that requests are sure to meet a segment on the move.
    Nanos move_delay{};
    // Tests only (PARAMESH_TEST_REPLY_DELAY_MS): the first page this home sends to another node
    // goes this much later, so that the request is sent again with RETRY.
    Nanos reply_delay{};
    // Tests only (PARAMESH_TEST_TASKS_DELAY_MS): the launcher starts its task queue this much
    // later, so that the workers' first TASK_REQs come before it.
    Nanos tasks_delay{};
    // The hold window against thrashing (AT-1), with the plan's proposed defaults.
    // PARAMESH_CFG_THRASH_THRESHOLD sets the count; 0 turns the window off.
    HomeConfig thrash{8, std::chrono::milliseconds{100}, std::chrono::milliseconds{1},
                      std::chrono::milliseconds{64}};
};

// "127.0.0.1:47100" -> endpoint. False if it is not that.
bool parse_endpoint(std::string_view text, Endpoint& out) {
    const auto colon = text.rfind(':');
    if (colon == std::string_view::npos) {
        return false;
    }
    const std::string host{text.substr(0, colon)};
    in_addr addr{};
    const long port = std::strtol(std::string{text.substr(colon + 1)}.c_str(), nullptr, 10);
    if (::inet_pton(AF_INET, host.c_str(), &addr) != 1 || port <= 0 || port > 65535) {
        return false;
    }
    out = Endpoint{ntohl(addr.s_addr), static_cast<std::uint16_t>(port)};
    return true;
}

// Read once, in pm_init(), before this library starts any thread.
const char* env(const char* name) {
    return std::getenv(name);  // NOLINT(concurrency-mt-unsafe)
}

// task.*, thrash.hold_initial, thrash.hold_max and cache.* from PARAMESH_CFG_*.
void read_waits_and_grain(Settings& s) {
    if (const char* policy = env("PARAMESH_CFG_CACHE_POLICY");
        policy != nullptr && std::string_view{policy} == "cost") {
        const char* window = env("PARAMESH_CFG_CACHE_VICTIM_WINDOW");
        s.cache_window =
            window == nullptr
                ? 8
                : std::max<std::uint32_t>(
                      1, static_cast<std::uint32_t>(std::strtoul(window, nullptr, 10)));
    }
    if (const char* depth = env("PARAMESH_CFG_PREFETCH_DEPTH")) {
        s.prefetch_depth = static_cast<std::uint32_t>(std::strtoul(depth, nullptr, 10));
    }
    if (const char* streams = env("PARAMESH_CFG_PREFETCH_STREAMS")) {
        s.prefetch_streams = std::max<std::uint32_t>(
            1, static_cast<std::uint32_t>(std::strtoul(streams, nullptr, 10)));
    }
    // task.no_task_backoff and thrash.hold_initial set the first wait, in ms, as
    // coh.busy_retry_backoff does; it doubles up to its maximum, which is never below it.
    if (const char* backoff = env("PARAMESH_CFG_TASK_NO_TASK_BACKOFF")) {
        s.tasks.backoff_initial =
            std::chrono::milliseconds{std::max(1L, std::strtol(backoff, nullptr, 10))};
        s.tasks.backoff_max = std::max(s.tasks.backoff_max, s.tasks.backoff_initial);
    }
    if (const char* prefetch = env("PARAMESH_CFG_TASK_PREFETCH")) {
        s.tasks.prefetch = static_cast<std::uint32_t>(std::strtoul(prefetch, nullptr, 10));
    }
    if (const char* grain = env("PARAMESH_CFG_TASK_DEFAULT_GRAIN")) {  // chunks per slot
        s.tasks.chunks_per_slot = std::max<std::uint32_t>(
            1, static_cast<std::uint32_t>(std::strtoul(grain, nullptr, 10)));
    }
    if (const char* hold = env("PARAMESH_CFG_THRASH_HOLD_INITIAL")) {
        s.thrash.hold_initial =
            std::chrono::milliseconds{std::max(1L, std::strtol(hold, nullptr, 10))};
    }
    if (const char* hold = env("PARAMESH_CFG_THRASH_HOLD_MAX")) {
        s.thrash.hold_max = std::chrono::milliseconds{std::max(1L, std::strtol(hold, nullptr, 10))};
    }
    s.thrash.hold_max = std::max(s.thrash.hold_max, s.thrash.hold_initial);
}

// The environment of docs/PROTOCOL.md, section 10. With PARAMESH_PMD_SOCKET the process was
// started by pmrun or by a pmd and learns its peers from the daemon; without it, from
// PARAMESH_LISTEN and PARAMESH_PEERS.
bool read_settings(Settings& s) {
    const char* role = env("PARAMESH_ROLE");
    const char* job = env("PARAMESH_JOB_ID");
    const char* node = env("PARAMESH_NODE_ID");
    const char* socket = env("PARAMESH_PMD_SOCKET");
    const char* listen = env("PARAMESH_LISTEN");
    const char* peers = env("PARAMESH_PEERS");
    if (role == nullptr || job == nullptr || node == nullptr ||
        (socket == nullptr && (listen == nullptr || peers == nullptr))) {
        return false;
    }
    if (std::string_view{role} == "launcher") {
        s.role = kLauncher;
    } else if (std::string_view{role} == "worker") {
        s.role = kWorker;
    }
    s.job = JobId{static_cast<std::uint32_t>(std::strtoul(job, nullptr, 10))};
    s.self = NodeId{static_cast<std::uint16_t>(std::strtoul(node, nullptr, 10))};
    if (const char* timeout =
            env("PARAMESH_CFG_JOB_START_TIMEOUT")) {  // job.start_timeout, in seconds
        s.start_timeout = std::chrono::seconds{std::strtol(timeout, nullptr, 10)};
    }
    if (const char* level = env("PARAMESH_CFG_LOG_LEVEL")) {
        const std::string_view name{level};
        if (name == "debug") {
            s.log_level = LogLevel::kDebug;
        } else if (name == "info") {
            s.log_level = LogLevel::kInfo;
        } else if (name == "error") {
            s.log_level = LogLevel::kError;
        }
    }
    if (const char* threshold = env("PARAMESH_CFG_THRASH_THRESHOLD")) {
        s.thrash.thrash_transfers =
            static_cast<std::uint32_t>(std::strtoul(threshold, nullptr, 10));
        if (s.thrash.thrash_transfers == 0) {
            s.thrash.thrash_period = Nanos{};  // off
        }
    }
    if (const char* share = env("PARAMESH_CFG_MEM_CACHE_SHARE")) {
        s.cache_share = std::min<std::uint64_t>(std::strtoul(share, nullptr, 10), 100);
    }
    if (const char* backoff = env("PARAMESH_CFG_COH_BUSY_RETRY_BACKOFF")) {
        s.busy_initial = std::chrono::milliseconds{std::max(1L, std::strtol(backoff, nullptr, 10))};
    }
    if (const char* delay = env("PARAMESH_TEST_MOVE_DELAY_MS")) {
        s.move_delay = std::chrono::milliseconds{std::strtol(delay, nullptr, 10)};
    }
    if (const char* delay = env("PARAMESH_TEST_TASKS_DELAY_MS")) {
        s.tasks_delay = std::chrono::milliseconds{std::strtol(delay, nullptr, 10)};
    }
    if (const char* delay = env("PARAMESH_TEST_REPLY_DELAY_MS")) {
        s.reply_delay = std::chrono::milliseconds{std::strtol(delay, nullptr, 10)};
    }
    if (const char* affinity = env("PARAMESH_CFG_TASK_AFFINITY")) {
        s.tasks.affinity = std::string_view{affinity} != "0";
    }
    read_waits_and_grain(s);
    if (socket != nullptr) {
        s.pmd_socket = socket;
        // Any address, a port the system chooses. The launcher is the first member: a worker
        // knows its address from PARAMESH_LAUNCHER, and its node ID is the high half of the
        // job ID (docs/PROTOCOL.md, L_RUN_OK). The rest come from pmd or from SEG_MAP.
        const char* launcher = env("PARAMESH_LAUNCHER");
        Member first{s.self, Slot{0}, {}};
        if (s.role == kWorker) {
            first.node = NodeId{static_cast<std::uint16_t>(s.job.value >> 16U)};
            if (launcher == nullptr || !parse_endpoint(launcher, first.at) ||
                first.node == s.self) {
                return false;
            }
            s.members = {first, Member{s.self, Slot{1}, {}}};
        } else {
            s.members = {first};
        }
        return s.role != 0 && s.job.value != 0 && s.self != kNoNode;
    }
    bool found_self = false;
    std::string_view rest{peers};
    while (!rest.empty()) {
        const auto comma = rest.find(',');
        const std::string_view one = rest.substr(0, comma);
        rest.remove_prefix(comma == std::string_view::npos ? rest.size() : comma + 1);
        const auto at = one.find('@');
        Member member;
        member.node = NodeId{static_cast<std::uint16_t>(
            std::strtoul(std::string{one.substr(0, at)}.c_str(), nullptr, 10))};
        member.slot = Slot{static_cast<std::uint8_t>(s.members.size())};
        if (at == std::string_view::npos || member.node == kNoNode ||
            !parse_endpoint(one.substr(at + 1), member.at)) {
            return false;
        }
        found_self = found_self || member.node == s.self;
        s.members.push_back(member);
    }
    return s.role != 0 && s.job.value != 0 && found_self && s.members.size() <= kMaxNodes &&
           parse_endpoint(listen, s.listen);
}

void put_u64(std::span<std::byte> out, std::size_t at, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; i++) {
        out[at + i] = static_cast<std::byte>(value >> (8 * (7 - i)));
    }
}
std::uint64_t get_u64(std::span<const std::byte> in) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8 && i < in.size(); i++) {
        value = (value << 8U) | std::to_integer<std::uint64_t>(in[i]);
    }
    return value;
}

std::array<pm_test_fn, 16>& test_functions() {
    static std::array<pm_test_fn, 16> functions{};
    return functions;
}

// The test hook's frames are TASK_ASSIGN and TASK_DONE with this bit in the chunk ID, which no
// real chunk has. Its threads are numbered from kHookThreads, above any worker thread.
constexpr std::uint64_t kHookChunk = std::uint64_t{1} << 63U;
constexpr std::uint16_t kHookThreads = 0x8000;

class Job final : public NetHandler, public FaultSink, public StoreEvents, public RuntimeHost {
public:
    explicit Job(Settings settings) : s_(std::move(settings)) {}

    [[nodiscard]] bool is_launcher() const noexcept { return s_.role == kLauncher; }

    // Brings the process into the job. Returns a pm_status; on a worker it returns PM_OK once
    // the job is up, and the caller then waits for the job to end.
    int start(std::uint64_t region_bytes, std::uint32_t threads) {
        auto platform = platform_open({}, PlatformOptions{s_.self, s_.job, 2, s_.log_level});
        if (!platform.ok() || !hash_binary(*platform.value().hasher)) {
            return PM_ERR_PLATFORM;
        }
        platform_ = std::move(platform).value();
        segments_ = static_cast<std::size_t>((region_bytes + kSegmentSize - 1) / kSegmentSize);

        auto mem = mem_open(RegionConfig{kRegionMaxBytes}, *this);
        auto dir = home_open(s_.thrash);
        if (!mem.ok() || !dir.ok()) {
            return PM_ERR_PLATFORM;
        }
        mem_ = std::move(mem).value();
        dir_ = std::move(dir).value();

        TransportConfig config;
        config.job = s_.job;
        config.self = JoinInfo{s_.self, s_.role, s_.listen.port,
                               static_cast<std::uint32_t>(::getpid()), hash_};
        auto net = net_open(config, *platform_.checksum, *this);
        if (!net.ok()) {
            return PM_ERR_NETWORK;
        }
        const Result<std::uint16_t> port = net.value()->listen(s_.listen);
        if (!port.ok()) {
            return PM_ERR_NETWORK;
        }
        s_.listen.port = port.value();
        net_ = std::move(net).value();
        sync_.emplace(*net_, *this);
        if (!s_.pmd_socket.empty()) {
            const int joined = join_through_pmd(region_bytes, threads);
            if (joined != PM_OK) {
                return joined;
            }
        }
        // The RAM cap from pmd, split between the local cache and the home store
        // (mem.cache_share). Without pmd there is no cap.
        // A node that joined mid-run is home for nothing: all of it is the cache.
        cache_bytes_ = (quota_flags_ & kQuotaNoHomes) != 0 ? ram_ : ram_ * s_.cache_share / 100;
        const std::uint64_t cache_bytes = cache_bytes_;
        // The spill file (M4-3), of the spill pmd committed: no cap, no file. It is made beside
        // pmd's socket, in the daemon's state directory.
        StoreConfig home;
        home.ram_cap_bytes = ram_ - cache_bytes;
        home.spill_budget_bytes = ram_ == 0 ? 0 : spill_;
        home.job = s_.job;
        const char* tmp = env("TMPDIR");
        home.spill_dir = !s_.pmd_socket.empty() ? s_.pmd_socket.substr(0, s_.pmd_socket.rfind('/'))
                                                : std::string{tmp != nullptr ? tmp : ""};
        auto store = store_open(home, *this);
        auto cache = cache_open(cache_bytes, s_.cache_window);
        if (!store.ok() || !cache.ok()) {
            return PM_ERR_PLATFORM;
        }
        store_ = std::move(store).value();
        cache_ = std::move(cache).value();
        for (const Member& member : s_.members) {
            if (member.node != s_.self && !net_->add_peer(member.node, member.at).ok()) {
                return PM_ERR_CONFIG;
            }
        }
        thread_ = std::thread{[this] { static_cast<void>(net_->run()); }};
        if (s_.members.size() == 1) {
            run_on_network_thread();  // a job of one node: nobody to wait for
        }

        {
            std::unique_lock<std::mutex> lock{mutex_};
            if (!changed_.wait_for(lock, s_.start_timeout, [this] { return up_; })) {
                return PM_ERR_NETWORK;
            }
        }
        // The job is up: start this process's worker threads, as many as it may ever use: the
        // program's own bound, or one for each processor here. Of those, only as many as the
        // quota from pmd take chunks, and pmd may change it while the job runs (M5-3). Without
        // pmd there is no quota.
        // ponytail: the default grain takes every node to have as many threads as this one;
        // L_ADMIT_OK does not carry the other nodes' quotas.
        const std::uint32_t started = threads != 0 ? threads : std::thread::hardware_concurrency();
        std::uint32_t quota = 0;
        {
            const std::lock_guard<std::mutex> lock{quota_mu_};
            quota = quota_;
        }
        TasksConfig tasks = s_.tasks;
        tasks.threads =
            static_cast<std::uint16_t>(std::clamp<std::uint32_t>(started, 1, kHookThreads - 1));
        tasks.total_slots = static_cast<std::uint32_t>(
            std::clamp<std::uint32_t>(pmd_fd_ < 0 ? started : std::min(started, quota), 1,
                                      kHookThreads - 1) *
            s_.members.size());
        tasks.region_bytes = region_bytes_;
        if (is_launcher()) {
            std::this_thread::sleep_for(s_.tasks_delay);
        }
        tasks_.emplace(*net_, *this, *sync_, tasks);
        tasks_up_.store(true, std::memory_order_release);
        if (pmd_fd_ >= 0) {  // before the threads start, or they would ask for chunks first
            const std::lock_guard<std::mutex> lock{quota_mu_};
            tasks_ready_ = true;
            tasks_->set_quota(static_cast<std::uint16_t>(std::min<std::uint32_t>(quota_, 65535)));
        }
        return tasks_->start().ok() ? PM_OK : PM_ERR_PLATFORM;
    }

    void* allocate(std::size_t bytes) {
        const auto offset = allocator_.allocate(bytes);
        // NOLINTNEXTLINE(performance-no-int-to-ptr): the region lives at a fixed address
        return offset ? reinterpret_cast<void*>(kRegionBase + *offset) : nullptr;
    }

    // pm_touch: makes each page of the range fault, if it has to, exactly as the program's own
    // access would, and returns when the pages are here.
    int touch(const void* p, std::size_t n, pm_access access) const noexcept {
        const auto first = reinterpret_cast<std::uintptr_t>(p);
        const std::uint64_t region = region_bytes_;
        if (region == 0) {
            return PM_ERR_STATE;
        }
        if ((access != PM_ACCESS_READ && access != PM_ACCESS_WRITE) || first < kRegionBase ||
            first - kRegionBase > region || n > region - (first - kRegionBase)) {
            return PM_ERR_INVALID;
        }
        if (n == 0) {
            return PM_OK;
        }
        for (std::uintptr_t page = first / kPageSize * kPageSize; page < first + n;
             page += kPageSize) {
            // NOLINTNEXTLINE(performance-no-int-to-ptr): the region lives at a fixed address
            auto* byte = reinterpret_cast<unsigned char*>(page);
            if (access == PM_ACCESS_READ) {
                static_cast<void>(std::atomic_ref<unsigned char>{*byte}.load());
                continue;
            }
            // One write instruction that changes nothing and cannot lose another thread's update:
            // an atomic "or with 0". It must be a single write, not a read then a compare-and-swap,
            // or the page would be fetched for reading first and upgraded after.
#if defined(__x86_64__)
            __asm__ volatile("lock orb $0, (%0)"
                             :
                             : "r"(byte)
                             : "memory", "cc");  // NOLINT(hicpp-no-assembler)
#else
            // ponytail: other architectures take the read-then-upgrade route until one is
            // supported.
            std::atomic_ref<unsigned char>{*byte}.fetch_or(0);
#endif
        }
        return PM_OK;
    }

    // pm_atomic_add: asks the page's home to add, and sleeps until it answers with the old value.
    std::uint64_t atomic_add(const std::uint64_t* addr, std::uint64_t delta) noexcept {
        const auto address = reinterpret_cast<std::uintptr_t>(addr);
        if (address < kRegionBase || address - kRegionBase >= region_bytes_ || address % 8 != 0) {
            // The return value cannot carry an error (paramesh.h).
            abort_job(Status::kInternal,
                      std::string_view{"pm_atomic_add: the address is not an 8-byte-aligned "
                                       "number in the shared region"});
        }
        const ReqId req = fresh();
        std::unique_lock<std::mutex> lock{mutex_};
        adds_.push_back({req, AtomicOpPayload{address - kRegionBase, delta}});
        // A reference into an unordered_map stays valid while other threads insert and erase.
        std::optional<std::uint64_t>& old = add_results_[req.value];
        run_on_network_thread();
        changed_.wait(lock, [&old] { return old.has_value(); });
        const std::uint64_t value = old.value_or(0);
        add_results_.erase(req.value);
        return value;
    }

    // What pmd's L_LEAVE and L_LEAVE_INTENT do, for a test without a daemon.
    void test_leave() noexcept {
        on_network_thread([this] { begin_leave(); });
    }
    void test_member_leaving(NodeId node) noexcept {
        on_network_thread([this, node] { member_leaving(node); });
    }

    // Locks and barriers, and tasks. Null before pm_init() has built the job.
    [[nodiscard]] Sync* sync() noexcept { return sync_.has_value() ? &*sync_ : nullptr; }
    [[nodiscard]] Tasks* tasks() noexcept { return tasks_.has_value() ? &*tasks_ : nullptr; }

    int run_on_workers(std::uint32_t id) {
        std::array<std::byte, 40>
            assign{};  // a TASK_ASSIGN whose chunk ID is the function's number
        put_u64(assign, 0, kHookChunk | id);
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            hook_done_ = 0;
        }
        std::size_t workers = 0;
        for (const Member& member : s_.members) {
            if (member.node != s_.self) {
                workers++;
                post(member.node, Opcode::kTaskAssign, assign);
            }
        }
        std::unique_lock<std::mutex> lock{mutex_};
        changed_.wait(lock, [&] { return hook_done_ == workers; });
        return PM_OK;
    }

    // Ends the job normally. Launcher only.
    void finish_job() {
        if (tasks_.has_value()) {
            tasks_->stop();
        }
        ending_ = true;
        std::size_t workers = 0;
        for (const Member& member : s_.members) {
            if (member.node != s_.self) {
                workers++;
                post_job_end(member.node, Status::kOk, kNoNode, "");
            }
        }
        {
            std::unique_lock<std::mutex> lock{mutex_};
            changed_.wait_for(lock, kFinalizeWait,
                              [&] { return gone_ == workers; });  // each worker exits and closes
        }
        net_->stop();
        thread_.join();
        flush_reports();
        if (pmd_thread_.joinable()) {
            ::shutdown(pmd_fd_, SHUT_RDWR);  // ends the wait in watch_pmd()
            pmd_thread_.join();
            ::close(pmd_fd_);
        }
        log_pages();
    }

    // Ends the job on an error, from any thread: tells the others, then exits this process.
    void abort_job(Status status, NodeId node, const std::string& message) noexcept {
        if (aborting_.exchange(true) || ending_) {
            return;
        }
        if (is_launcher()) {
            for (const Member& member : s_.members) {
                if (member.node != s_.self) {
                    post_job_end(member.node, status, node, message);
                }
            }
        } else if (node != s_.members.front().node) {
            post_job_end(s_.members.front().node, status, node,
                         message);  // the launcher passes it on
        }
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            abort_message_ = message;
        }
        linger_ = net_->start_timer(kLinger);
    }

    // ---- RuntimeHost: what src/rt/ asks of the job ----------------------------------------------
    [[nodiscard]] NodeId self() const noexcept override { return s_.self; }
    [[nodiscard]] NodeId launcher() const noexcept override { return s_.members.front().node; }
    // Any thread: the launcher's main() asks while it queues chunks, and the map can change.
    [[nodiscard]] NodeId home_of(PageId page) const noexcept override {
        const std::lock_guard<std::mutex> lock{mutex_};
        return slot_node_.at(homes_.at(segment_of(page).value).value);
    }
    [[noreturn]] void abort_job(Status status, std::string_view message) noexcept override {
        if (std::this_thread::get_id() == thread_.get_id()) {
            // ponytail: the network thread cannot wait for its own JOB_END to leave, so it only
            // exits; the other processes then end the job as "node lost". Every caller on this
            // thread reports a peer that broke the protocol.
            exit_with(status, std::string{message});
        }
        abort_job(status, s_.self, std::string{message});
        for (;;) {
            ::pause();  // the abort exits the process shortly
        }
    }
    // The launcher hears of every chunk. The first time a node finishes a chunk of a task, the
    // log says so: with the thrash report, which names nodes, it tells which tasks fought.
    void chunk_finished(NodeId ran_by, std::uint64_t task_id, std::uint64_t indexes,
                        Nanos cpu) noexcept override {
        report(Opcode::kLChunk,
               LChunkPayload{ran_by, task_id, indexes, static_cast<std::uint64_t>(cpu.count())});
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            if (!task_seen_.insert({ran_by.value, task_id}).second) {
                return;
            }
        }
        platform_.logger->log(LogLevel::kInfo, "task_on_node",
                              JsonObject{{"task", JsonValue{std::string{rt_task_name(task_id)}}},
                                         {"on", JsonValue{std::int64_t{ran_by.value}}}});
    }

    // ---- FaultSink: fault-handler thread ---------------------------------------------------
    void on_fault(const FaultEvent& event) noexcept override {
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            faults_.push_back(event);
        }
        run_on_network_thread();
    }

    // ---- StoreEvents: the store's I/O thread, or the network thread ----------------------------
    void on_loaded(PageId page) noexcept override { store_told(page, true); }
    void on_spilled(PageId page) noexcept override { store_told(page, false); }

    void store_told(PageId page, bool loaded) noexcept {
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            store_notes_.push_back(StoreNote{page, loaded});
        }
        run_on_network_thread();
    }

    // ---- NetHandler: network thread ------------------------------------------------------------
    void on_peer_joined(const JoinInfo& peer) noexcept override {
        if (peer.binary_hash != hash_) {
            abort_job(Status::kBinaryMismatch, peer.node,
                      "node " + std::to_string(peer.node.value) + " runs a different binary");
            return;
        }
        joined_.insert(peer.node.value);
        if (is_launcher() && up_ && peer.node == joining_ && !join_sent_) {
            send_join_map();
            return;
        }
        advance_start();
    }

    void on_frame(const FrameHeader& header, std::span<const std::byte> payload) noexcept override {
        if ((header.flags & kFlagRetry) != 0) {
            return;  // a request this process already has: TCP lost nothing (PROTOCOL.md 8)
        }
        handle(header, payload);
        pump();
    }

    void on_peer_lost(NodeId peer, Errc /*why*/) noexcept override {
        if (ending_) {
            const std::lock_guard<std::mutex> lock{mutex_};
            gone_++;
            changed_.notify_all();
            return;
        }
        if (left_.contains(peer.value)) {
            return;  // it said LEAVE_DONE and every process has the map without it
        }
        const std::string message = "node " + std::to_string(peer.value) + " lost";
        if (!is_launcher() && peer == s_.members.front().node) {
            exit_with(Status::kNodeLost, message);  // nobody left to tell
        }
        abort_job(Status::kNodeLost, peer, message);
    }

    void on_reply_timeout(NodeId peer, ReqId /*req*/) noexcept override {
        abort_job(Status::kTimeout, peer, "no reply from node " + std::to_string(peer.value));
    }

    void on_timer(TimerId timer, Nanos /*now*/) noexcept override {
        if (aborting_ && timer == linger_) {
            std::string message;
            {
                const std::lock_guard<std::mutex> lock{mutex_};
                message = abort_message_;
            }
            exit_with(Status::kInternal, message);
        }
        if (s_.members.size() == 1) {
            advance_start();
        }
        if (const auto held = hold_timers_.find(timer.value); held != hold_timers_.end()) {
            const PageId page = held->second;
            hold_timers_.erase(held);
            home_event(HomeEventKind::kHoldExpired, FrameHeader{}, page, {});
        }
        if (const auto due = timers_.find(timer.value); due != timers_.end()) {
            const std::function<void()> what = std::move(due->second);
            timers_.erase(due);
            what();
        }
        std::deque<FaultEvent> faults;
        std::deque<Add> adds;
        std::deque<StoreNote> notes;
        std::deque<std::function<void()>> calls;
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            faults.swap(faults_);
            adds.swap(adds_);
            notes.swap(store_notes_);
            calls.swap(calls_);
        }
        for (const std::function<void()>& call : calls) {
            call();
        }
        // What the store did on its own: the directory records where each page is, and
        // whatever waited for the store goes on.
        for (const StoreNote& note : notes) {
            if (note.loaded) {
                home_event(HomeEventKind::kLoaded, FrameHeader{}, note.page, {});
            } else if (home_entry(*dir_, note.page).where == HomeWhere::kRam) {
                home_set_where(*dir_, note.page, HomeWhere::kSpill);
            }
        }
        if (!notes.empty()) {
            resume_home();
            continue_moves();
        }
        for (const Add& add : adds) {
            add_ops_[add.req.value] = add.op;
            send_add(add.req.value, add.req);
        }
        for (const FaultEvent& fault : faults) {
            if (evicting_) {
                continue;  // a leaving node serves no more faults (STATE_MACHINES.md, 1.6)
            }
            faults_taken_++;  // for L_STATS (M6-1): counted, and timed until installed
            fault_started_.try_emplace(fault.page.value, net_->now());
            node_event(fault.kind == FaultKind::kRead ? NodeEventKind::kNeedRead
                                                      : NodeEventKind::kNeedWrite,
                       fault.page, kNoReq, {}, false);
            if (fault.kind == FaultKind::kRead) {
                prefetch(fault.page);
            }
        }
        pump();
    }

private:
    struct Local {
        FrameHeader header;
        std::vector<std::byte> payload;
    };
    // One pm_atomic_add on its way from an application thread to the network thread.
    struct StoreNote {
        PageId page;
        bool loaded;  // else spilled
    };
    struct Add {
        ReqId req;
        AtomicOpPayload op;
    };

    bool hash_binary(const Hasher& hasher) {
        std::ifstream file{"/proc/self/exe", std::ios::binary};
        std::vector<char> image{std::istreambuf_iterator<char>{file},
                                std::istreambuf_iterator<char>{}};
        if (image.empty()) {
            return false;
        }
        hash_ = hasher.sha256(std::as_bytes(std::span{image}));
        return true;
    }

    // The pages the prefetcher guesses `page`'s read fault leads to, asked for as a fault would
    // ask: those not held and not on their way. Write faults are not followed: a page fetched
    // to read and then written costs a second round trip.
    void prefetch(PageId page) noexcept {
        for (const std::uint64_t next : prefetcher_.on_fault(page.value)) {
            if (evicting_ || aborting_ || next >= states_.size() ||
                states_[next] != PageState::kInvalid) {
                continue;
            }
            prefetched_++;
            node_event(NodeEventKind::kNeedRead, PageId{next}, kNoReq, {}, false);
        }
    }

    void run_on_network_thread() noexcept { static_cast<void>(net_->start_timer(Nanos{0})); }

    [[noreturn]] void exit_with(Status status, const std::string& message) const noexcept {
        if (status != Status::kOk) {
            static_cast<void>(
                std::fprintf(stderr, "job %u aborted: %s\n", s_.job.value, message.c_str()));
        }
        static_cast<void>(std::fflush(nullptr));
        ::_exit(status == Status::kOk ? 0 : 1);
    }

    // ---- sending -------------------------------------------------------------------------------

    ReqId fresh() noexcept { return ReqId{next_req_++}; }

    // Sends one frame, or queues it for this process when it is addressed to itself. Any thread.
    void post(NodeId to, Opcode opcode, std::span<const std::byte> payload, ReqId req = kNoReq,
              std::uint16_t flags = 0, ReplyTimer timer = ReplyTimer::kNone) noexcept {
        FrameHeader header;
        header.opcode = opcode;
        header.flags = flags;
        header.req = req;
        header.epoch = epoch_;
        if (to == s_.self) {
            header.src = header.dst = s_.self;
            local_.push_back(
                Local{header, {payload.begin(), payload.end()}});  // network thread only
        } else if (!net_->send(to, header, payload, timer).ok() && !ending_ && !aborting_) {
            abort_job(Status::kNodeLost, to, "node " + std::to_string(to.value) + " lost");
        }
    }

    void post_job_end(NodeId to, Status status, NodeId node, const std::string& message) noexcept {
        std::array<std::byte, 520> buffer{};
        const Result<std::size_t> size =
            wire_encode(JobEndPayload{status, node, message.substr(0, 512)}, buffer);
        if (size.ok()) {
            post(to, Opcode::kJobEnd, std::span{buffer}.first(size.value()));
        }
    }

    void post_page_id(NodeId to, Opcode opcode, PageId page, ReqId req, ReplyTimer timer) noexcept {
        std::array<std::byte, 8> buffer{};
        static_cast<void>(wire_encode(PageIdPayload{page}, buffer));
        post(to, opcode, buffer, req, 0, timer);
    }

    // Runs `what` on the network thread after `delay`. Network thread only.
    void after(Nanos delay, std::function<void()> what) noexcept {
        timers_[net_->start_timer(delay).value] = std::move(what);
    }

    // Runs `what` on the network thread soon. Any thread.
    void on_network_thread(std::function<void()> what) noexcept {
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            calls_.push_back(std::move(what));
        }
        run_on_network_thread();
    }

    // The wait before a request is sent again after BUSY_RETRY or REDIRECT: it doubles for
    // each retry of the same thing, up to the most (coh.busy_retry_backoff).
    Nanos backoff(std::uint64_t key) noexcept {
        const std::uint32_t tries = std::min<std::uint32_t>(retries_[key]++, 16);
        return std::min(s_.busy_initial * (1U << tries), s_.busy_max);
    }

    // Sends the pm_atomic_add that began as request `first` to the page's home under the map
    // of this moment, as a new request.
    void send_add(std::uint64_t first, ReqId req) noexcept {
        const auto op = add_ops_.find(first);
        if (op == add_ops_.end()) {
            return;
        }
        add_first_[req.value] = first;
        std::array<std::byte, 24> payload{};
        static_cast<void>(wire_encode(op->second, payload));
        post(home_of(page_of(kRegionBase + op->second.offset)), Opcode::kAtomicOp, payload, req, 0,
             ReplyTimer::kTimed);
    }

    // Delivers the frames this process addressed to itself.
    void pump() noexcept {
        while (!local_.empty()) {
            const Local next = std::move(local_.front());
            local_.pop_front();
            handle(next.header, next.payload);
        }
    }

    // ---- start-up ------------------------------------------------------------------------------

    [[nodiscard]] bool all_joined() const {
        return std::all_of(s_.members.begin(), s_.members.end(), [this](const Member& m) {
            return m.node == s_.self || joined_.contains(m.node.value);
        });
    }

    // ---- the local pmd (docs/PROTOCOL.md, sections 8.1 and 9) ----------------------------------

    template <typename Payload>
    bool tell_pmd(Opcode opcode, const Payload& payload) {
        std::array<std::byte, 64> bytes{};
        const Result<std::size_t> size = wire_encode(payload, bytes);
        FrameHeader header;
        header.opcode = opcode;
        header.job = s_.job;
        header.src = s_.self;
        return size.ok() && frame_write(pmd_fd_, *platform_.checksum, header,
                                        std::span{bytes}.first(size.value()))
                                .ok();
    }

    // Registers with the local pmd and takes this process's thread quota from it. The
    // launcher then asks for admission: pmd starts the workers and answers with the members.
    // Called by start() before the network thread runs.
    int join_through_pmd(std::uint64_t region_bytes, std::uint32_t threads) {
        sockaddr_un to{};
        to.sun_family = AF_UNIX;
        if (s_.pmd_socket.size() >= sizeof to.sun_path) {
            return PM_ERR_CONFIG;
        }
        std::copy(s_.pmd_socket.begin(), s_.pmd_socket.end(), std::begin(to.sun_path));
        pmd_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API
        if (pmd_fd_ < 0 ||
            ::connect(pmd_fd_, reinterpret_cast<const sockaddr*>(&to), sizeof to) != 0 ||
            !tell_pmd(Opcode::kLRegister, LRegisterPayload{static_cast<std::uint32_t>(::getpid()),
                                                           s_.role, s_.listen.port, hash_})) {
            return PM_ERR_NETWORK;
        }
        const Result<Frame> told = frame_read(pmd_fd_, *platform_.checksum);
        const Result<LQuotaPayload> quota = told.ok() ? wire_decode_l_quota(told.value().payload)
                                                      : Result<LQuotaPayload>{told.error()};
        if (!quota.ok() || told.value().header.opcode != Opcode::kLQuota) {
            return PM_ERR_NETWORK;
        }
        quota_ = quota.value().threads;
        ram_ = quota.value().ram_bytes;
        spill_ = quota.value().spill_bytes;
        quota_flags_ = quota.value().flags;

        if (is_launcher()) {
            const auto bound = static_cast<std::uint16_t>(std::min<std::uint32_t>(threads, 65535));
            if (!tell_pmd(Opcode::kLAdmitReq, LAdmitReqPayload{region_bytes, bound})) {
                return PM_ERR_NETWORK;
            }
            const Result<Frame> answer = frame_read(pmd_fd_, *platform_.checksum);
            if (answer.ok() && answer.value().header.opcode == Opcode::kLAdmitRefused) {
                const Result<LAdmitRefusedPayload> why =
                    wire_decode_l_admit_refused(answer.value().payload);
                static_cast<void>(std::fprintf(stderr, "job %u refused: %s\n", s_.job.value,
                                               why.ok() ? why.value().message.c_str() : "?"));
                return PM_ERR_REFUSED;
            }
            const Result<LAdmitOkPayload> admitted =
                answer.ok() ? wire_decode_l_admit_ok(answer.value().payload)
                            : Result<LAdmitOkPayload>{answer.error()};
            if (!admitted.ok() || answer.value().header.opcode != Opcode::kLAdmitOk ||
                admitted.value().members.size() > kMaxNodes) {
                return PM_ERR_NETWORK;
            }
            // Then L_QUOTA: what this node committed to the job.
            const Result<Frame> committed = frame_read(pmd_fd_, *platform_.checksum);
            const Result<LQuotaPayload> own = committed.ok()
                                                  ? wire_decode_l_quota(committed.value().payload)
                                                  : Result<LQuotaPayload>{committed.error()};
            if (!own.ok() || committed.value().header.opcode != Opcode::kLQuota) {
                return PM_ERR_NETWORK;
            }
            quota_ = own.value().threads;
            ram_ = own.value().ram_bytes;
            spill_ = own.value().spill_bytes;
            quota_flags_ = own.value().flags;
            for (const SegMapMember& member : admitted.value().members) {
                if (member.node != s_.self) {
                    s_.members.push_back(Member{member.node,
                                                Slot{static_cast<std::uint8_t>(s_.members.size())},
                                                Endpoint{member.addr, member.port}});
                }
            }
        }
        pmd_thread_ = std::thread{[this] { watch_pmd(); }};
        report_thread_ = std::thread{[this] { write_reports(); }};
        return PM_OK;
    }

    // ---- reports to pmd for its ledger (M5-1) ---------------------------------------------------

    // Any thread: queues one frame for pmd. The writer thread sends it, so that no thread that
    // must not block (the fault handler, the network thread) ever waits on the socket.
    template <typename Payload>
    void report(Opcode opcode, const Payload& payload) noexcept {
        std::array<std::byte, 64> bytes{};
        const Result<std::size_t> size = wire_encode(payload, bytes);
        if (pmd_fd_ < 0 || !size.ok()) {
            return;
        }
        queue_report(opcode, {bytes.begin(), bytes.begin() + static_cast<long>(size.value())});
    }

    void queue_report(Opcode opcode, std::vector<std::byte> payload) noexcept {
        if (pmd_fd_ < 0) {
            return;
        }
        {
            const std::lock_guard<std::mutex> lock{report_mu_};
            reports_.push_back(Report{opcode, std::move(payload)});
        }
        report_ready_.notify_one();
    }

    // ---- the status API's counts (M6-1) ---------------------------------------------------------

    // A fault's page is installed or woken: how long that took, in its power-of-two bucket.
    void fault_ended(PageId page) noexcept {
        const auto started = fault_started_.find(page.value);
        if (started == fault_started_.end()) {
            return;
        }
        const auto us =
            std::chrono::duration_cast<std::chrono::microseconds>(net_->now() - started->second);
        fault_started_.erase(started);
        std::size_t bucket = 0;
        while (bucket + 1 < kLatencyBuckets && (std::int64_t{1} << bucket) <= us.count()) {
            bucket++;
        }
        latency_.at(bucket)++;
    }

    // L_STATS, once a second while the job runs: the counts, and the pages held per segment.
    // ponytail: every page of the region is looked at each second; fine for the regions tested
    // (up to a few hundred MB), a count kept per segment if a region of GB makes it slow.
    void send_stats() noexcept {
        LStatsPayload stats;
        stats.faults = faults_taken_;
        stats.pages_in = pages_in_;
        stats.pages_out = pages_out_;
        stats.retries = sent_again_;
        stats.latency = latency_;
        for (std::size_t s = 0; s < homes_.size(); s++) {
            LStatsSegment entry{SegmentId{static_cast<std::uint32_t>(s)},
                                slot_node_.at(homes_[s].value), 0, 0};
            for (std::size_t p = s * kPagesPerSegment;
                 p < (s + 1) * kPagesPerSegment && p < states_.size(); p++) {
                entry.shared = static_cast<std::uint16_t>(
                    entry.shared + (states_[p] == PageState::kShared ? 1 : 0));
                entry.modified = static_cast<std::uint16_t>(
                    entry.modified + (states_[p] == PageState::kModified ? 1 : 0));
            }
            if (entry.home == s_.self || entry.shared != 0 || entry.modified != 0) {
                stats.segments.push_back(entry);
            }
        }
        std::vector<std::byte> bytes(100 + 12 * stats.segments.size());
        if (const Result<std::size_t> size = wire_encode(stats, bytes); size.ok()) {
            queue_report(Opcode::kLStats, std::move(bytes));
        }
        after(std::chrono::seconds{1}, [this] { send_stats(); });
    }

    // True when the newest map makes this node home for more than what pmd committed holds:
    // the home store's RAM, the commitment less the cache, and the spill.
    [[nodiscard]] bool room_short() const noexcept {
        const auto homed =
            static_cast<std::uint64_t>(std::count(homes_.begin(), homes_.end(), my_slot_));
        return ram_ != 0 && homed * kSegmentSize > ram_ - cache_bytes_ + spill_;
    }

    // L_QUOTA: pmd's commitment, grown when this node was made home for more. The store takes
    // the new caps; the start or the segments on their way go on once they hold its homes.
    void grant_room(std::uint64_t ram, std::uint64_t spill) noexcept {
        if (ram_ == 0 || (ram <= ram_ && spill <= spill_)) {
            return;
        }
        ram_ = std::max(ram_, ram);
        spill_ = std::max(spill_, spill);
        store_->grow(ram_ - cache_bytes_, spill_);
        if (!room_wanted_ || room_short()) {
            return;
        }
        room_wanted_ = false;
        if (is_launcher()) {
            up_if_room();
        } else {
            advance_start();
        }
        take_segments();
    }

    // Launcher: up once every member has the first map and this node has room for its homes.
    void up_if_room() noexcept {
        if (!up_ && start_acked_ && !room_wanted_) {
            set_up();
        }
    }

    // L_SEGMENTS: how many segments each member of `map` is home for.
    void report_segments(const SegMapPayload& map) noexcept {
        LSegmentsPayload counts{epoch_, {}};
        for (const SegMapMember& member : map.members) {
            const auto homed = std::count(map.homes.begin(), map.homes.end(), member.slot);
            counts.entries.push_back({member.node, static_cast<std::uint16_t>(homed)});
        }
        report(Opcode::kLSegments, counts);
    }

    // The writer thread: sends the queued reports in order until flush_reports().
    void write_reports() noexcept {
        std::unique_lock<std::mutex> lock{report_mu_};
        for (;;) {
            report_ready_.wait(lock, [this] { return reports_stop_ || !reports_.empty(); });
            if (reports_.empty()) {
                return;  // told to stop, and everything is sent
            }
            const Report next = std::move(reports_.front());
            reports_.pop_front();
            lock.unlock();
            FrameHeader header;
            header.opcode = next.opcode;
            header.job = s_.job;
            header.src = s_.self;
            static_cast<void>(frame_write(pmd_fd_, *platform_.checksum, header, next.payload));
            lock.lock();
        }
    }

    // Sends what is queued and ends the writer thread. Once, when the job ends in order.
    void flush_reports() noexcept {
        {
            const std::lock_guard<std::mutex> lock{report_mu_};
            reports_stop_ = true;
        }
        report_ready_.notify_one();
        if (report_thread_.joinable()) {
            report_thread_.join();
        }
    }

    // The local-link thread: waits only on the socket to pmd. pmd may tell a launcher to end
    // the job (Ctrl+C on pmrun); and a job whose pmd has gone ends, because nothing would then
    // enforce its quota or record its work.
    void watch_pmd() noexcept {
        for (;;) {
            const Result<Frame> frame = frame_read(pmd_fd_, *platform_.checksum);
            if (!frame.ok()) {
                if (!ending_ && !aborting_) {
                    abort_job(Status::kPmdLost, s_.self, "the pmd of this node is gone");
                }
                return;
            }
            if (frame.value().header.opcode == Opcode::kLQuota) {  // a new share (M5-3)
                const Result<LQuotaPayload> quota = wire_decode_l_quota(frame.value().payload);
                const std::lock_guard<std::mutex> lock{quota_mu_};
                if (quota.ok()) {
                    quota_ = quota.value().threads;
                    if (tasks_ready_ && tasks_.has_value()) {
                        tasks_->set_quota(quota.value().threads);
                    }
                    on_network_thread(
                        [this, ram = quota.value().ram_bytes, spill = quota.value().spill_bytes] {
                            grant_room(ram, spill);
                        });
                }
            } else if (frame.value().header.opcode == Opcode::kLLeave) {
                on_network_thread([this] { begin_leave(); });
            } else if (frame.value().header.opcode == Opcode::kLMember) {
                const Result<LMemberPayload> joined = wire_decode_l_member(frame.value().payload);
                if (joined.ok()) {
                    on_network_thread([this, m = joined.value().member] {
                        joiners_.push_back(m);
                        try_join();
                    });
                }
            } else if (frame.value().header.opcode == Opcode::kLLeaveIntent) {
                const Result<LeaveIntentPayload> who =
                    wire_decode_leave_intent(frame.value().payload);
                if (who.ok()) {
                    on_network_thread([this, node = who.value().node] { member_leaving(node); });
                }
            }
            if (frame.value().header.opcode == Opcode::kLAbort) {
                const Result<StatusTextPayload> why =
                    wire_decode_spawn_decline(frame.value().payload);
                abort_job(why.ok() ? why.value().status : Status::kUserAbort, s_.self,
                          why.ok() ? why.value().message : std::string{"ended by pmd"});
            }
        }
    }

    // Moves the start sequence on whenever something it waits for has happened.
    void advance_start() noexcept {
        if (!all_joined()) {
            return;
        }
        if (is_launcher() && !map_sent_) {
            map_sent_ = true;
            send_map(kNoNode, false);
            if (s_.members.size() == 1) {
                start_acked_ = true;
                up_if_room();
            }
        } else if (!is_launcher() && have_map_ && !up_sent_ && !room_wanted_) {
            up_sent_ = true;
            std::array<std::byte, 4> ack{};
            static_cast<void>(wire_encode(SegMapAckPayload{epoch_}, ack));
            post(s_.members.front().node, Opcode::kSegMapAck, ack);
            set_up();
        }
    }

    void set_up() noexcept {
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            up_ = true;
            changed_.notify_all();
        }
        // A leave that pmd told of while the job was starting begins now.
        if (leave_early_) {
            on_network_thread([this] { begin_leave(); });
        }
        for (const NodeId node : early_leavers_) {
            on_network_thread([this, node] { member_leaving(node); });
        }
        early_leavers_.clear();
        on_network_thread([this] { try_join(); });  // a node offered while the job started
        if (pmd_fd_ >= 0) {
            on_network_thread([this] { send_stats(); });  // and then once a second
        }
    }

    // Launcher: places the segments, applies the map at the next epoch and sends it to every
    // member. A member that is leaving is named once more, flagged, with no weight, so it is
    // home for nothing; with `drop` it is named no longer (docs/PROTOCOL.md, SEG_MAP and 8.2).
    void send_map(NodeId leaver, bool drop) noexcept {
        SegMapPayload map;
        std::vector<PlacementMember> placement;
        for (const Member& member : s_.members) {
            const bool leaving = member.node == leaver;
            if (leaving && drop) {
                continue;  // apply_map() takes it out of the members and the copysets
            }
            const bool no_homes = no_homes_.contains(member.node.value);
            std::uint8_t flags = member.node == s_.self ? kMemberLauncher : 0;
            flags |= leaving ? kMemberLeaving : 0;
            flags |= no_homes ? kMemberNoHomes : 0;
            // Without pmd's commitments every member weighs the same; one that joined mid-run
            // or is leaving weighs nothing, so no segment moves to it.
            const std::uint64_t weight = leaving || no_homes ? 0 : 1;
            map.members.push_back(
                {member.node, member.slot, flags, member.at.ipv4, member.at.port, weight});
            placement.push_back({member.node, member.slot, weight});
        }
        map.homes.resize(segments_);
        // The current homes stay where they are, so a leave moves only the leaver's segments.
        std::vector<NodeId> keep;
        keep.reserve(homes_.size());
        for (const Slot home : homes_) {
            keep.push_back(slot_node_.at(home.value));
        }
        if (!store_place_segments(placement, map.homes, keep).ok()) {
            abort_job(Status::kInternal, kNoNode, "cannot place the segments");
            return;
        }
        acks_ = 0;
        dropping_ = drop;
        apply_map(Epoch{epoch_.value + 1}, map);
        std::vector<std::byte> buffer(4 + 20 * map.members.size() + map.homes.size());
        const Result<std::size_t> size = wire_encode(map, buffer);
        for (const Member& member : s_.members) {
            if (member.node != s_.self && size.ok()) {
                post(member.node, Opcode::kSegMap, buffer);
            }
        }
        if (drop && s_.members.size() == 1) {
            map_acked();  // nobody is left to acknowledge it
        }
    }

    // Launcher: every member has applied the newest map.
    void map_acked() noexcept {
        if (!up_) {
            start_acked_ = true;
            up_if_room();
            return;
        }
        if (join_sent_) {  // the newest map names the node that joined: it may take chunks
            platform_.logger->log(LogLevel::kInfo, "member_joined",
                                  JsonObject{{"node", JsonValue{std::int64_t{joining_.value}}}});
            joining_ = kNoNode;
            join_sent_ = false;
        }
        if (!dropping_ || leavers_.empty()) {
            try_join();
            return;
        }
        // The map without the leaver is everywhere: nobody will address it again, and it may go.
        const NodeId gone = leavers_.front();
        leavers_.pop_front();
        dropping_ = false;
        post_job_end(gone, Status::kLeft, gone, "");
        if (Tasks* queue = tasks(); queue != nullptr) {
            queue->return_chunks(gone);
        }
        platform_.logger->log(LogLevel::kInfo, "member_left",
                              JsonObject{{"node", JsonValue{std::int64_t{gone.value}}},
                                         {"segments", JsonValue{std::int64_t{segments_moved_}}}});
        if (!leavers_.empty()) {
            start_leave();
        }
        try_join();
    }

    // ---- a node joins (docs/PROTOCOL.md, 8.3) ---------------------------------------------------

    // Launcher: the next node pmd spawned for this job (L_MEMBER) becomes a member with a new
    // slot, hosting no segment. One at a time, and not while a leave is under way; the map
    // naming it goes out once it has connected.
    void try_join() noexcept {
        if (!is_launcher() || !up_ || joining_ != kNoNode || !leavers_.empty() ||
            joiners_.empty()) {
            return;
        }
        const SegMapMember joiner = joiners_.front();
        joiners_.pop_front();
        std::uint8_t slot = 0;
        for (const auto& [node, used] : slots_) {
            slot = std::max<std::uint8_t>(slot, used.value + 1);
        }
        const Endpoint at{joiner.addr, joiner.port};
        if (slot >= kMaxSlots || !net_->add_peer(joiner.node, at).ok()) {
            try_join();  // ponytail: a node that cannot be added is passed over
            return;
        }
        s_.members.push_back(Member{joiner.node, Slot{slot}, at});
        slots_[joiner.node.value] = Slot{slot};
        no_homes_.insert(joiner.node.value);
        joining_ = joiner.node;
        if (joined_.contains(joiner.node.value)) {
            send_join_map();
        }
    }

    void send_join_map() noexcept {
        join_sent_ = true;
        send_map(kNoNode, false);
    }

    // ---- a member leaves (docs/PROTOCOL.md, 8.2) ------------------------------------------------

    // Launcher: a member is leaving (L_LEAVE_INTENT). One at a time; another waits its turn.
    void member_leaving(NodeId node) noexcept {
        if (is_launcher() && !up_) {
            early_leavers_.push_back(node);  // taken when the job is up (set_up)
            return;
        }
        const bool member = std::any_of(s_.members.begin(), s_.members.end(),
                                        [node](const Member& m) { return m.node == node; });
        if (!is_launcher() || !up_ || node == s_.self || !member ||
            std::find(leavers_.begin(), leavers_.end(), node) != leavers_.end()) {
            return;
        }
        leavers_.push_back(node);
        if (leavers_.size() == 1) {
            start_leave();
        }
    }

    // No more chunks for the leaver; what it has not finished after the grace goes back in the
    // queue; and a map in which it is home for nothing.
    void start_leave() noexcept {
        const NodeId node = leavers_.front();
        platform_.logger->log(LogLevel::kInfo, "member_leaving",
                              JsonObject{{"node", JsonValue{std::int64_t{node.value}}}});
        if (Tasks* queue = tasks(); queue != nullptr) {
            queue->member_leaving(node);
            after(kLeaveGrace, [this, node] {
                if (Tasks* later = tasks(); later != nullptr && !ending_ && !aborting_) {
                    later->return_chunks(node);
                }
            });
        }
        send_map(node, false);
    }

    // This process is to leave (L_LEAVE): its threads take no more chunks, and when they are
    // idle, or after the grace, it gives up every page it holds.
    void begin_leave() noexcept {
        if (!up_) {
            leave_early_ = true;  // taken when the job is up (set_up)
            return;
        }
        if (is_launcher() || leaving_) {
            return;
        }
        leaving_ = true;
        platform_.logger->log(LogLevel::kInfo, "leaving", JsonObject{});
        if (Tasks* queue = tasks(); queue != nullptr) {
            queue->leave();
        }
        after(kLeaveGrace, [this] { give_up_pages(); });
        wait_for_tasks();
    }

    void wait_for_tasks() noexcept {
        Tasks* queue = tasks();
        if (queue == nullptr || queue->idle()) {
            give_up_pages();
        } else if (!evicting_) {
            after(kLeavePoll, [this] { wait_for_tasks(); });
        }
    }

    // Leave, step 3: EVICT on every page held, so modified pages are written back and read
    // copies dropped. From here on no fault of this process is served (STATE_MACHINES.md, 1.6);
    // a page still on its way in is given up as soon as it arrives.
    void give_up_pages() noexcept {
        if (evicting_) {
            return;
        }
        evicting_ = true;
        for (std::uint64_t page = 0; page < states_.size(); page++) {
            if (states_[page] == PageState::kShared || states_[page] == PageState::kModified) {
                node_event(NodeEventKind::kEvict, PageId{page}, kNoReq, {}, false);
            }
        }
        pump();
        leave_if_done();
    }

    // LEAVE_DONE, once this node holds no page and is home for nothing.
    void leave_if_done() noexcept {
        const bool hosting = std::any_of(seg_.begin(), seg_.end(), [](Seg s) {
            return s == Seg::kServing || s == Seg::kFrozen || s == Seg::kArriving;
        });
        if (!evicting_ || leave_done_ || held_pages_ != 0 || hosting || !hosts_nothing()) {
            return;
        }
        leave_done_ = true;
        std::array<std::byte, 4> done{};
        static_cast<void>(wire_encode(LeaveDonePayload{segments_moved_}, done));
        post(s_.members.front().node, Opcode::kLeaveDone, done);
    }

    // True once a map has made this node home for no segment.
    [[nodiscard]] bool hosts_nothing() const noexcept {
        return epoch_.value > 1 && std::none_of(homes_.begin(), homes_.end(),
                                                [this](Slot home) { return home == my_slot_; });
    }

    // ---- segments on the move (docs/STATE_MACHINES.md, section 3) ------------------------------

    // Is a page request for this node to handle? If not it is answered here: REDIRECT when
    // this node knows better where the page lives, BUSY_RETRY when the sender knows a newer
    // map or the segment has not arrived yet.
    bool serves(const FrameHeader& header, PageId page) noexcept {
        const std::uint32_t segment = segment_of(page).value;
        const Seg state = segment < seg_.size() ? seg_[segment] : Seg::kOther;
        if (state == Seg::kServing) {
            return true;
        }
        if (state == Seg::kArriving ||
            (state == Seg::kOther && header.epoch.value > epoch_.value)) {
            post_page_id(header.src, Opcode::kBusyRetry, page, header.req, ReplyTimer::kNone);
            return false;
        }
        std::array<std::byte, 16> redirect{};
        static_cast<void>(wire_encode(RedirectPayload{page, epoch_, home_of(page)}, redirect));
        post(header.src, Opcode::kRedirect, redirect, header.req);
        return false;
    }

    // BUSY_RETRY or REDIRECT came for a request of this process: it is sent again, as a new
    // request, after the back-off, to the page's home under the map this process then has.
    // ponytail: after a REDIRECT this does not wait for the map of the epoch it names; if the
    // old map is still the newest here the request is redirected once more.
    void try_again(const FrameHeader& header, std::span<const std::byte> payload) noexcept {
        sent_again_++;
        if (const auto add = add_first_.find(header.req.value); add != add_first_.end()) {
            const std::uint64_t first = add->second;
            add_first_.erase(add);
            after(backoff(kAddRetry | first), [this, first] { send_add(first, fresh()); });
            return;
        }
        PageId page;
        if (header.opcode == Opcode::kBusyRetry) {
            const Result<PageIdPayload> busy = wire_decode_page_id(payload);
            page = busy.ok() ? busy.value().page : PageId{states_.size()};
        } else {
            const Result<RedirectPayload> redirect = wire_decode_redirect(payload);
            page = redirect.ok() ? redirect.value().page : PageId{states_.size()};
        }
        if (page.value >= states_.size()) {
            abort_job(Status::kProtocol, header.src,
                      "malformed frame from node " + std::to_string(header.src.value));
            return;
        }
        node_event(header.opcode == Opcode::kBusyRetry ? NodeEventKind::kBusyRetry
                                                       : NodeEventKind::kRedirect,
                   page, header.req, {}, false);
    }

    // Sends frozen segments that have nothing in progress any more, and goes on with those
    // that waited for the store.
    void continue_moves() noexcept {
        if (!stalled_) {
            for (std::size_t s = 0; s < seg_.size(); s++) {
                if (seg_[s] == Seg::kFrozen) {
                    send_segment(SegmentId{static_cast<std::uint32_t>(s)});
                }
            }
        }
        take_segments();
    }

    // SEG_MIGRATE for one frozen segment, from where it stopped: every entry of the directory,
    // with the home copy where there is one. A page in the spill file is asked for first and
    // the sending goes on when the store has it.
    // ponytail: up to kMigrateBatch pages are copied out of the store for one frame, outside
    // its cap, for as long as the frame takes to leave.
    void send_segment(SegmentId segment) noexcept {
        const auto found = sending_.find(segment.value);
        std::size_t next = found == sending_.end() ? 0 : found->second;
        if (next > kPagesPerSegment || home_segment_busy(*dir_, segment) ||
            net_->now() < moves_from_) {
            return;  // sent, and waiting for SEG_MIGRATE_DONE; or an operation is still ending
        }
        const NodeId to = home_of(PageId{segment.value * kPagesPerSegment});
        std::vector<std::array<std::byte, kPageSize>> pages(kMigrateBatch);
        std::vector<std::byte> frame(8 + kMaxMigrateEntries * 24 + kMigrateBatch * kPageSize);
        while (next <= kPagesPerSegment) {
            SegMigratePayload batch;
            batch.segment = segment;
            std::size_t copied = 0;
            bool waiting = false;  // for the store to read a page back
            for (; next < kPagesPerSegment && copied < kMigrateBatch &&
                   batch.entries.size() < kMaxMigrateEntries;
                 next++) {
                const PageId page{segment.value * kPagesPerSegment + next};
                const HomeEntryView entry = home_entry(*dir_, page);
                if (entry.state == HomeState::kUncached && entry.where == HomeWhere::kZero &&
                    entry.version == 0) {
                    continue;  // never touched: no entry is sent
                }
                SegMigrateEntry out;
                out.page = static_cast<std::uint16_t>(next);
                out.state = static_cast<std::uint8_t>(entry.state);
                out.owner = entry.owner;
                out.version = entry.version;
                out.copyset = entry.copyset;
                if (entry.where == HomeWhere::kNone) {
                    out.where = 2;
                } else if (entry.where != HomeWhere::kZero) {
                    const Result<StoreLookup> got = store_->get(page, pages[copied]);
                    if (got.ok() && got.value() == StoreLookup::kSpilled &&
                        store_->load(page).ok()) {
                        waiting = true;  // goes on from this page at the store's next word
                        break;
                    }
                    if (!got.ok() || got.value() != StoreLookup::kCopied) {
                        abort_job(Status::kInternal, s_.self,
                                  "the home has no copy of page " + std::to_string(page.value));
                        return;
                    }
                    out.where = 1;
                    out.data = pages[copied++];
                }
                batch.entries.push_back(out);
            }
            batch.last = next == kPagesPerSegment;
            if (!batch.entries.empty() || batch.last) {
                const Result<std::size_t> size = wire_encode(batch, frame);
                if (size.ok()) {
                    post(to, Opcode::kSegMigrate, std::span{frame}.first(size.value()), fresh(), 0,
                         batch.last ? ReplyTimer::kTimed : ReplyTimer::kNone);
                }
            }
            if (batch.last) {
                next = kPagesPerSegment + 1;  // all sent
            }
            if (waiting) {
                break;
            }
        }
        sending_[segment.value] = next;
    }

    // SEG_MIGRATE_DONE: the new home serves the segment. Its entries and home copies go.
    void segment_gone(SegmentId segment) noexcept {
        if (seg_[segment.value] != Seg::kFrozen) {
            return;
        }
        for (std::uint64_t i = 0; i < kPagesPerSegment; i++) {
            store_->drop(PageId{segment.value * kPagesPerSegment + i});
        }
        home_remove_segment(*dir_, segment);
        sending_.erase(segment.value);
        seg_[segment.value] = Seg::kMoved;
        segments_moved_++;
        leave_if_done();
    }

    // The SEG_MIGRATE frames that have come, in order: each entry into the directory and its
    // page into the store, which may be making room. After the last frame of a segment this
    // node serves it and says SEG_MIGRATE_DONE.
    void take_segments() noexcept {
        while (!incoming_.empty() && !aborting_ && !room_wanted_) {  // room first
            const Incoming& frame = incoming_.front();
            const Result<SegMigratePayload> batch = wire_decode_seg_migrate(frame.bytes);
            if (batch.ok() && frame.epoch.value > epoch_.value) {
                return;  // the map that makes this node the home has not come yet
            }
            if (!batch.ok() || batch.value().segment.value >= seg_.size() ||
                seg_[batch.value().segment.value] != Seg::kArriving) {
                abort_job(Status::kProtocol, frame.from,
                          "malformed frame from node " + std::to_string(frame.from.value));
                return;
            }
            const SegmentId segment = batch.value().segment;
            for (; incoming_at_ < batch.value().entries.size(); incoming_at_++) {
                const SegMigrateEntry& in = batch.value().entries[incoming_at_];
                const PageId page{segment.value * kPagesPerSegment + in.page};
                if (in.where == 1) {
                    const Result<void> put = store_->put(page, in.data.first<kPageSize>());
                    if (!put.ok() && put.error().code == Errc::kState) {
                        return;  // goes on from this entry at the store's next word
                    }
                    if (!put.ok()) {
                        abort_job(Status::kInternal, s_.self, "the home store is full");
                        return;
                    }
                }
                HomeEntryView entry;
                entry.state = static_cast<HomeState>(in.state);
                static constexpr std::array kWhere{HomeWhere::kZero, HomeWhere::kRam,
                                                   HomeWhere::kNone};
                entry.where = kWhere.at(in.where);
                entry.owner = in.owner;
                entry.copyset = in.copyset;
                entry.version = in.version;
                if (!home_import_entry(*dir_, page, entry).ok()) {
                    abort_job(Status::kProtocol, frame.from,
                              "malformed frame from node " + std::to_string(frame.from.value));
                    return;
                }
            }
            if (batch.value().last) {
                seg_[segment.value] = Seg::kServing;
                std::array<std::byte, 4> done{};
                static_cast<void>(wire_encode(SegMigrateDonePayload{segment}, done));
                post(frame.from, Opcode::kSegMigrateDone, done, frame.req);
            }
            incoming_.pop_front();
            incoming_at_ = 0;
        }
    }

    // The first map makes this node home for its segments, all untouched. A later one moves
    // segments (docs/STATE_MACHINES.md, section 3): one this node was serving and no longer
    // hosts is frozen and sent to its new home; one it now hosts and did not is on its way.
    void apply_map(Epoch epoch, const SegMapPayload& map) noexcept {
        const bool first = !have_map_;
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            epoch_ = epoch;
            homes_ = map.homes;
            slot_node_.fill(kNoNode);
            for (const SegMapMember& member : map.members) {
                slot_node_.at(member.slot.value) = member.node;
            }
        }
        for (const SegMapMember& member : map.members) {
            if (member.node == s_.self) {
                my_slot_ = member.slot;
            }
            // The directory sends INV and FETCH to the node behind a copyset bit.
            home_set_member(*dir_, member.slot, member.node);
            slots_[member.node.value] = member.slot;
            // A worker started by pmd hears of the other workers here, and connects to them.
            const bool known =
                std::any_of(s_.members.begin(), s_.members.end(),
                            [&member](const Member& m) { return m.node == member.node; });
            if (!known) {
                const Endpoint at{member.addr, member.port};
                s_.members.push_back(Member{member.node, member.slot, at});
                static_cast<void>(net_->add_peer(member.node, at));
            }
        }
        // A member named no longer has left: it is in no copyset, and its connection closing
        // is no loss.
        std::erase_if(s_.members, [&](const Member& gone) {
            const bool listed =
                std::any_of(map.members.begin(), map.members.end(),
                            [&gone](const SegMapMember& m) { return m.node == gone.node; });
            if (!listed) {
                home_drop_member(*dir_, gone.slot);
                left_.insert(gone.node.value);
            }
            return !listed;
        });
        if (first) {
            seg_.assign(homes_.size(), Seg::kOther);
            states_.assign(homes_.size() * kPagesPerSegment, PageState::kInvalid);
            allocator_ = RegionAllocator{homes_.size() * kSegmentSize};
            region_bytes_ = homes_.size() * kSegmentSize;
        }
        for (std::size_t s = 0; s < homes_.size(); s++) {
            const SegmentId segment{static_cast<std::uint32_t>(s)};
            const bool mine = homes_[s] == my_slot_;
            if (first && mine) {
                static_cast<void>(home_add_segment(*dir_, segment));
                seg_[s] = Seg::kServing;
            } else if (seg_[s] == Seg::kServing && !mine) {
                seg_[s] = Seg::kFrozen;
                if (s_.move_delay > Nanos{} && moves_from_ <= net_->now()) {
                    moves_from_ = net_->now() + s_.move_delay;
                    after(s_.move_delay, [this] { continue_moves(); });
                }
                actions_.clear();
                home_freeze_segment(*dir_, segment, slot_node_.at(homes_[s].value), actions_);
                run_home_actions(actions_, {}, net_->now());
            } else if (!first && mine && (seg_[s] == Seg::kOther || seg_[s] == Seg::kMoved)) {
                static_cast<void>(home_add_segment(*dir_, segment));
                seg_[s] = Seg::kArriving;
            }
        }
        have_map_ = true;
        room_wanted_ = room_short();
        report_segments(map);  // which asks pmd for more room when room_wanted_
        if (!first) {
            continue_moves();
            leave_if_done();
        }
    }

    // The copyset bit of a node. A member that has left keeps its bit for the frames it sent
    // before it went: a slot is never given to another member (docs/PROTOCOL.md, [GATE P5]),
    // and a home that asked it for an INV_ACK just before the map without it still gets one.
    [[nodiscard]] Slot slot_of(NodeId node) const noexcept {
        const auto found = slots_.find(node.value);
        return found == slots_.end() ? Slot{0} : found->second;
    }

    // ---- one frame -----------------------------------------------------------------------------

    // What this process has to say when the job ends in order: how many pages, or rights to
    // pages, reached it from other nodes.
    // With this process's whole counts, as L_STATS has them (M6-5 reads them from here, since a
    // short job ends before its first report).
    void log_pages() const noexcept {
        JsonArray latency;
        for (const std::uint32_t count : latency_) {
            latency.push_back(JsonValue{std::int64_t{count}});
        }
        platform_.logger->log(
            LogLevel::kInfo, "job_pages",
            JsonObject{{"received", JsonValue{static_cast<std::int64_t>(pages_in_)}},
                       {"sent_again", JsonValue{static_cast<std::int64_t>(sent_again_)}},
                       {"faults", JsonValue{static_cast<std::int64_t>(faults_taken_)}},
                       {"prefetched", JsonValue{static_cast<std::int64_t>(prefetched_)}},
                       {"sent", JsonValue{static_cast<std::int64_t>(pages_out_)}},
                       {"latency", JsonValue{std::move(latency)}}});
    }

    // The frames of maps, of segments on the move and of leaving. True if it was one of them.
    template <typename Bad>
    bool handle_map_or_move(const FrameHeader& header, std::span<const std::byte> payload,
                            const Bad& bad) noexcept {
        switch (header.opcode) {
            case Opcode::kSegMap: {
                const Result<SegMapPayload> map = wire_decode_seg_map(payload);
                if (!map.ok() || is_launcher()) {
                    bad();
                    return true;
                }
                const bool first = !have_map_;
                apply_map(header.epoch, map.value());
                if (first) {
                    advance_start();
                } else {
                    std::array<std::byte, 4> ack{};
                    static_cast<void>(wire_encode(SegMapAckPayload{epoch_}, ack));
                    post(s_.members.front().node, Opcode::kSegMapAck, ack);
                }
                return true;
            }
            case Opcode::kSegMapAck: {
                const Result<SegMapAckPayload> ack = wire_decode_seg_map_ack(payload);
                if (!ack.ok()) {
                    bad();
                    return true;
                }
                // Every member but the launcher, and but one that is leaving (it is told with
                // JOB_END), has applied the newest map.
                if (ack.value().epoch == epoch_ && ++acks_ + 1 == s_.members.size()) {
                    map_acked();
                }
                return true;
            }
            case Opcode::kBusyRetry:
            case Opcode::kRedirect:
                try_again(header, payload);
                return true;
            case Opcode::kSegMigrate:
                incoming_.push_back(Incoming{
                    header.src, header.req, header.epoch, {payload.begin(), payload.end()}});
                take_segments();
                return true;
            case Opcode::kSegMigrateDone: {
                const Result<SegMigrateDonePayload> done = wire_decode_seg_migrate_done(payload);
                if (!done.ok() || done.value().segment.value >= seg_.size()) {
                    bad();
                    return true;
                }
                segment_gone(done.value().segment);
                return true;
            }
            case Opcode::kLeaveDone: {
                const Result<LeaveDonePayload> done = wire_decode_leave_done(payload);
                if (!done.ok() || !is_launcher() || leavers_.empty() ||
                    leavers_.front() != header.src) {
                    bad();
                    return true;
                }
                segments_moved_ = done.value().segments;
                send_map(header.src, true);  // the map without the leaver
                return true;
            }
            default:
                return false;
        }
    }

    void handle(const FrameHeader& header, std::span<const std::byte> payload) noexcept {
        if (header.src != s_.self &&
            (header.opcode == Opcode::kReadData || header.opcode == Opcode::kWriteGrant ||
             header.opcode == Opcode::kUpgradeGrant || header.opcode == Opcode::kFetchData)) {
            pages_in_++;
        }
        const auto bad = [&] {
            abort_job(Status::kProtocol, header.src,
                      "malformed frame from node " + std::to_string(header.src.value));
        };
        if (handle_map_or_move(header, payload, bad)) {
            return;
        }
        switch (header.opcode) {
            // The messages that carry a page ID and nothing else: to the home machine ...
            case Opcode::kReadReq:
            case Opcode::kWriteReq:
            case Opcode::kUpgradeReq:
            case Opcode::kInvAck:
            // ... and to the node machine.
            case Opcode::kUpgradeGrant:
            case Opcode::kWritebackAck:
            case Opcode::kInv:
            case Opcode::kFetch:
            case Opcode::kFetchInv: {
                const Result<PageIdPayload> request = wire_decode_page_id(payload);
                if (!request.ok()) {
                    bad();
                    return;
                }
                const PageId page = request.value().page;
                switch (header.opcode) {
                    case Opcode::kReadReq:
                        home_event(HomeEventKind::kReadReq, header, page, {});
                        break;
                    case Opcode::kWriteReq:
                        home_event(HomeEventKind::kWriteReq, header, page, {});
                        break;
                    case Opcode::kUpgradeReq:
                        home_event(HomeEventKind::kUpgradeReq, header, page, {});
                        break;
                    case Opcode::kInvAck:
                        home_event(HomeEventKind::kInvAck, header, page, {});
                        break;
                    case Opcode::kUpgradeGrant:
                        node_event(NodeEventKind::kUpgradeGrant, page, header.req, {}, false);
                        break;
                    case Opcode::kWritebackAck:
                        node_event(NodeEventKind::kWritebackAck, page, header.req, {}, false);
                        break;
                    case Opcode::kInv:
                        node_event(NodeEventKind::kInv, page, header.req, {}, false, header.src);
                        break;
                    case Opcode::kFetch:
                        node_event(NodeEventKind::kFetch, page, header.req, {}, false, header.src);
                        break;
                    default:
                        node_event(NodeEventKind::kFetchInv, page, header.req, {}, false,
                                   header.src);
                        break;
                }
                return;
            }
            case Opcode::kAtomicOp: {
                const Result<AtomicOpPayload> op = wire_decode_atomic_op(payload);
                if (!op.ok() || op.value().offset >= region_bytes_) {
                    bad();
                    return;
                }
                home_event(
                    HomeEventKind::kAtomicOp, header, page_of(kRegionBase + op.value().offset), {},
                    static_cast<std::uint16_t>(op.value().offset % kPageSize), op.value().operand);
                return;
            }
            case Opcode::kAtomicResult: {
                const Result<AtomicResultPayload> result = wire_decode_atomic_result(payload);
                const auto first = add_first_.find(header.req.value);
                const std::lock_guard<std::mutex> lock{mutex_};
                const auto waiting = first == add_first_.end() ? add_results_.end()
                                                               : add_results_.find(first->second);
                if (!result.ok() || waiting == add_results_.end()) {
                    bad();
                    return;
                }
                add_ops_.erase(first->second);
                retries_.erase(kAddRetry | first->second);
                add_first_.erase(first);
                waiting->second = result.value().old;
                changed_.notify_all();
                return;
            }
            case Opcode::kLockAcq:
            case Opcode::kLockGrant:
            case Opcode::kLockRel:
            case Opcode::kBarrierEnter:
            case Opcode::kBarrierRelease:
                if (Sync* locks = sync(); locks != nullptr) {
                    locks->on_frame(header, payload);
                }
                return;
            case Opcode::kFetchData:
            case Opcode::kWriteback: {
                const Result<PagePayload> page = wire_decode_page(payload, header.flags);
                if (!page.ok() || page.value().data.size() != kPageSize) {
                    bad();
                    return;
                }
                home_event(header.opcode == Opcode::kFetchData ? HomeEventKind::kFetchData
                                                               : HomeEventKind::kWriteback,
                           header, page.value().page, page.value().data);
                return;
            }
            case Opcode::kReadData:
            case Opcode::kWriteGrant: {
                const Result<PagePayload> page = wire_decode_page(payload, header.flags);
                if (!page.ok()) {
                    bad();
                    return;
                }
                const NodeEventKind kind = header.opcode == Opcode::kReadData
                                               ? NodeEventKind::kReadData
                                               : NodeEventKind::kWriteGrant;
                node_event(kind, page.value().page, header.req, page.value().data,
                           (header.flags & kFlagZeroPage) != 0);
                return;
            }
            case Opcode::kTaskReq:
            case Opcode::kNoTask:
            case Opcode::kTaskAssign:
            case Opcode::kTaskDone:
                handle_task(header, payload);
                return;
            case Opcode::kJobEnd: {
                const Result<JobEndPayload> end = wire_decode_job_end(payload);
                if (!end.ok()) {
                    bad();
                    return;
                }
                if (is_launcher()) {  // a worker reported a fatal condition: pass it on and stop
                    abort_job(end.value().status, end.value().node, end.value().message);
                    return;
                }
                if (end.value().status == Status::kOk || end.value().status == Status::kLeft) {
                    log_pages();
                    flush_reports();  // every chunk this process ran reaches the ledger
                }
                // LEFT: the reply to this node's LEAVE_DONE. Nobody will address it again.
                exit_with(end.value().status == Status::kLeft ? Status::kOk : end.value().status,
                          end.value().message);
            }
            default:
                abort_job(Status::kUnsupported, header.src,
                          "a message this build does not handle yet");
        }
    }

    // TASK_REQ, TASK_ASSIGN, NO_TASK and TASK_DONE: to the task queue, unless the frame is one
    // of the test hook's.
    void handle_task(const FrameHeader& header, std::span<const std::byte> payload) noexcept {
        const bool hook =
            (header.opcode == Opcode::kTaskAssign || header.opcode == Opcode::kTaskDone) &&
            (get_u64(payload) & kHookChunk) != 0;
        if (!hook) {
            if (Tasks* queue = tasks_up_.load(std::memory_order_acquire) ? tasks() : nullptr;
                queue != nullptr) {
                queue->on_frame(header, payload);
            } else if (header.opcode == Opcode::kTaskReq) {  // the queue is not up yet: ask later
                std::array<std::byte, 4> none{};
                static_cast<void>(wire_encode(NoTaskPayload{1}, none));
                post(header.src, Opcode::kNoTask, none, header.req);
            }
            return;
        }
        if (header.opcode == Opcode::kTaskDone) {
            const std::lock_guard<std::mutex> lock{mutex_};
            hook_done_++;
            changed_.notify_all();
            return;
        }
        // Run a registered function on a thread of its own, then say so.
        const std::uint64_t id = get_u64(payload) & ~kHookChunk;
        const pm_test_fn function =
            id < test_functions().size() ? test_functions().at(id) : nullptr;
        if (function == nullptr) {
            abort_job(Status::kProtocol, header.src,
                      "malformed frame from node " + std::to_string(header.src.value));
            return;
        }
        std::thread{[this, function, id, launcher = header.src] {
            rt_thread_index() = next_thread_++;
            function();
            std::array<std::byte, 24> done{};  // a TASK_DONE for that chunk ID
            put_u64(done, 0, kHookChunk | id);
            FrameHeader reply;
            reply.opcode = Opcode::kTaskDone;
            static_cast<void>(net_->send(launcher, reply, done, ReplyTimer::kNone));
        }}.detach();
    }

    // ---- the node machine
    // ------------------------------------------------------------------------

    // `from`: for INV, FETCH and FETCH_INV, the home that asks; its answer goes back to it,
    // which is not the page's home under this node's map while a segment is moving.
    void node_event(NodeEventKind kind, PageId page, ReqId req, std::span<const std::byte> data,
                    bool zero, NodeId from = kNoNode) noexcept {
        if (aborting_ || page.value >= states_.size()) {
            if (!aborting_) {
                abort_job(Status::kInternal, s_.self, "an access outside the job's region");
            }
            return;
        }
        // An event for a page that has one held back waits behind it, so that the node machine
        // sees a page's events in the order they came.
        const bool behind = std::any_of(held_.begin(), held_.end(),
                                        [page](const Held& h) { return h.page == page; });
        if (behind || !node_step(kind, page, req, data, zero, from)) {
            held_.push_back(Held{kind, page, req, {data.begin(), data.end()}, zero, from});
        }
        if (depth_ == 0) {
            release_held();
        }
        // A leaving node keeps nothing: a page that has just arrived is given up at once.
        if (evicting_ && kind != NodeEventKind::kEvict &&
            (states_[page.value] == PageState::kShared ||
             states_[page.value] == PageState::kModified)) {
            node_event(NodeEventKind::kEvict, page, kNoReq, {}, false);
        }
        if (evicting_ && depth_ == 0) {
            leave_if_done();
        }
    }

    // The events held back, oldest first, for as long as the first can still not go ahead.
    void release_held() noexcept {
        while (!held_.empty() && !aborting_) {
            Held next = std::move(held_.front());
            held_.pop_front();
            if (!node_step(next.kind, next.page, next.req, next.data, next.zero, next.from)) {
                held_.push_front(std::move(next));
                return;
            }
        }
    }

    // What a leave needs counted: the pages held, and that a request got through at last.
    void note_step(NodeEventKind kind, PageId page, PageState before, PageState after) noexcept {
        held_pages_ += after != PageState::kInvalid ? 1 : 0;
        held_pages_ -= before != PageState::kInvalid ? 1 : 0;
        if (kind == NodeEventKind::kReadData || kind == NodeEventKind::kWriteGrant ||
            kind == NodeEventKind::kUpgradeGrant || kind == NodeEventKind::kWritebackAck) {
            retries_.erase(page.value);
        }
    }

    // One event through the node machine. False if it would install a page and the local
    // cache has no room until a page given up is discarded: nothing has been done then.
    bool node_step(NodeEventKind kind, PageId page, ReqId req, std::span<const std::byte> data,
                   bool zero, NodeId from) noexcept {
        NodeEvent event;
        event.kind = kind;
        event.page = page;
        event.req = req;
        event.fresh = fresh();
        const auto found = current_.find(page.value);
        const PageState before = states_[page.value];
        const NodeStep step =
            coh_step(before, found == current_.end() ? kNoReq : found->second, event);
        const bool installs =
            (before == PageState::kReadPending || before == PageState::kWritePending) &&
            std::any_of(step.actions.begin(), step.actions.begin() + step.action_count,
                        [](const NodeAction& a) {
                            return a.kind == NodeActionKind::kInstallReadOnly ||
                                   a.kind == NodeActionKind::kInstallWritable;
                        });
        if (installs && !make_room(page)) {
            return false;
        }
        note_step(kind, page, before, step.state);
        states_[page.value] = step.state;
        if (step.current == kNoReq) {
            current_.erase(page.value);
        } else {
            current_[page.value] = step.current;
        }
        static constexpr std::array<std::byte, kPageSize> kZeros{};
        bool ok = true;
        for (std::size_t i = 0; i < step.action_count && ok; i++) {
            const NodeAction& a = step.actions.at(i);
            switch (a.kind) {
                case NodeActionKind::kSend:  // a request; WRITEBACK carries the page
                case NodeActionKind::kReply: {
                    std::array<std::byte, 8 + kPageSize> buffer{};
                    put_u64(buffer, 0, page.value);
                    if (a.with_page) {
                        ok = mem_->read(page, std::span{buffer}.last<kPageSize>()).ok();
                        pages_out_++;
                    }
                    post(a.kind == NodeActionKind::kReply && from != kNoNode ? from : home_of(page),
                         a.opcode, std::span{buffer}.first(a.with_page ? buffer.size() : 8), a.req,
                         a.read_only ? kFlagReadOnly : std::uint16_t{0},
                         a.kind == NodeActionKind::kSend ? ReplyTimer::kTimed : ReplyTimer::kNone);
                    break;
                }
                case NodeActionKind::kInstallReadOnly:
                case NodeActionKind::kInstallWritable: {
                    const PageView bytes = zero || data.size() != kPageSize
                                               ? PageView{kZeros}
                                               : data.first<kPageSize>();
                    ok = (zero || data.size() == kPageSize) &&
                         mem_->install(page, bytes,
                                       a.kind == NodeActionKind::kInstallWritable
                                           ? PageProtection::kWritable
                                           : PageProtection::kReadOnly)
                             .ok();
                    cache_->note_installed(page, a.kind == NodeActionKind::kInstallWritable);
                    fault_ended(page);
                    break;
                }
                case NodeActionKind::kWriteProtect:
                    ok = mem_->write_protect(page, true).ok();
                    break;
                case NodeActionKind::kAllowWrites:
                    ok = mem_->write_protect(page, false).ok();
                    cache_->note_installed(page, true);  // costlier to lose now
                    break;
                case NodeActionKind::kDiscard:
                    ok = mem_->zap(page).ok();
                    cache_->note_discarded(page);
                    break;
                case NodeActionKind::kWake:
                    ok = mem_->wake(page).ok();
                    fault_ended(page);
                    break;
                case NodeActionKind::kArmResend:  // BUSY_RETRY or REDIRECT: again, later
                    after(backoff(page.value), [this, page] {
                        node_event(NodeEventKind::kResend, page, kNoReq, {}, false);
                        pump();
                    });
                    break;
                default:
                    ok = false;
            }
        }
        if (!ok) {
            abort_job(Status::kInternal, s_.self,
                      "the node machine could not handle page " + std::to_string(page.value));
        }
        return true;
    }

    // Makes room in the local cache for `page`, about to be installed, by giving up the
    // oldest pages while the cache would pass its share of the RAM cap (M4-2). EVICT discards
    // a shared copy and writes a modified one back, to be discarded on WRITEBACK_ACK; a page
    // waiting for an upgrade is kept and queued again. False, with the page not counted, if
    // the room is still taken by pages written back and not yet acknowledged.
    bool make_room(PageId page) noexcept {
        cache_->note_installed(page, false);
        std::vector<PageId> kept;
        depth_++;
        while (!aborting_ && cache_->over_budget()) {
            const std::optional<PageId> victim = cache_->next_victim();
            if (!victim) {
                break;
            }
            if (*victim == page) {
                kept.push_back(page);
                continue;
            }
            node_event(NodeEventKind::kEvict, *victim, kNoReq, {}, false);
            if (states_[victim->value] == PageState::kUpgradePending) {
                kept.push_back(*victim);
            }
        }
        depth_--;
        for (const PageId kept_page : kept) {
            cache_->note_installed(kept_page, false);
        }
        if (cache_->full()) {
            cache_->note_discarded(page);
            return false;
        }
        return true;
    }

    // ---- the home machine
    // ------------------------------------------------------------------------

    void home_event(HomeEventKind kind, const FrameHeader& header, PageId page,
                    std::span<const std::byte> data, std::uint16_t offset_in_page = 0,
                    std::uint64_t operand = 0) noexcept {
        // While the store keeps an action waiting, every later event waits behind it.
        if (stalled_) {
            home_parked_.push_back(HomeParked{
                kind, header, page, {data.begin(), data.end()}, offset_in_page, operand});
            return;
        }
        // Section 3: a request is handled only for a segment this node serves, and what is
        // left of a segment that has gone (a timer, a word from the store) is nothing.
        const std::uint32_t segment = segment_of(page).value;
        const Seg where = segment < seg_.size() ? seg_[segment] : Seg::kOther;
        const bool request = kind == HomeEventKind::kReadReq || kind == HomeEventKind::kWriteReq ||
                             kind == HomeEventKind::kUpgradeReq ||
                             kind == HomeEventKind::kWriteback || kind == HomeEventKind::kAtomicOp;
        if (request ? !serves(header, page) : (where != Seg::kServing && where != Seg::kFrozen)) {
            return;
        }
        // The store says a page is in RAM. Only an entry waiting for that takes it as an event:
        // otherwise this node asked on its own (see home_copy()), or the page was there already.
        if (kind == HomeEventKind::kLoaded) {
            const HomeEntryView entry = home_entry(*dir_, page);
            if (entry.wait != HomeWait::kLoad) {
                if (entry.where == HomeWhere::kSpill) {
                    home_set_where(*dir_, page, HomeWhere::kRam);
                }
                return;
            }
        }
        HomeEvent event;
        event.kind = kind;
        event.page = page;
        event.offset_in_page = offset_in_page;
        event.operand = operand;
        event.from = header.src;
        event.slot = slot_of(header.src);
        event.req = header.req;
        event.now = net_->now();
        const HomeEntryView was = home_entry(*dir_, page);
        actions_.clear();
        home_step(*dir_, event, actions_);
        if (std::any_of(actions_.begin(), actions_.end(),
                        [](const HomeAction& a) { return a.kind == HomeActionKind::kAbort; })) {
            // docs/STATE_MACHINES.md, [GATE S8]: name the page, the state and the event.
            abort_job(Status::kInternal, s_.self,
                      "the home machine met an impossible event: page " +
                          std::to_string(page.value) + ", event " +
                          std::to_string(static_cast<int>(kind)) + " from node " +
                          std::to_string(header.src.value) + ", state " +
                          std::to_string(static_cast<int>(was.state)) + ", waiting for " +
                          std::to_string(static_cast<int>(was.wait)) + ", segment state " +
                          std::to_string(static_cast<int>(where)));
            return;
        }
        run_home_actions(actions_, data, event.now);
        if (where == Seg::kFrozen && !stalled_) {
            send_segment(SegmentId{segment});  // if that was the last operation in progress
        }
    }

    // What became of one home action.
    enum class Done : std::uint8_t {
        kYes,
        kWait,    // the store cannot yet: the action and the rest wait for its next word
        kFailed,  // the job is ending
    };

    // The home copy of a page, from the store. A page in the spill file is asked for and
    // the caller waits.
    Done home_copy(PageId page, PageBuffer out) noexcept {
        const Result<StoreLookup> got = store_->get(page, out);
        if (got.ok() && got.value() == StoreLookup::kCopied) {
            return Done::kYes;
        }
        if (got.ok() && got.value() == StoreLookup::kSpilled && store_->load(page).ok()) {
            return Done::kWait;
        }
        abort_job(Status::kInternal, s_.self,
                  "the home has no copy of page " + std::to_string(page.value));
        return Done::kFailed;
    }

    // Keeps a page as the home copy. The store may be writing a page out to make room.
    Done home_keep(PageId page, std::span<const std::byte> bytes) noexcept {
        const Result<void> put = bytes.size() == kPageSize
                                     ? store_->put(page, bytes.first<kPageSize>())
                                     : Result<void>{Error{Errc::kInvalidArgument}};
        if (put.ok()) {
            return Done::kYes;
        }
        if (put.error().code == Errc::kState) {
            return Done::kWait;
        }
        abort_job(Status::kInternal, s_.self, "the home store is full");
        return Done::kFailed;
    }

    Done home_reply(const HomeAction& a, std::span<const std::byte> data) noexcept {
        if (a.opcode == Opcode::kRedirect) {  // a waiting request of a segment that moves
            std::array<std::byte, 16> redirect{};
            static_cast<void>(wire_encode(RedirectPayload{a.page, epoch_, a.other}, redirect));
            post(a.to, a.opcode, redirect, a.req);
            return Done::kYes;
        }
        std::array<std::byte, 8 + kPageSize> buffer{};
        put_u64(buffer, 0, a.page.value);
        std::uint16_t flags = a.read_only ? kFlagReadOnly : std::uint16_t{0};
        std::size_t size = buffer.size();
        if (a.source == PageSource::kNone) {
            size = 8;  // UPGRADE_GRANT: the page ID alone
        } else if (a.source == PageSource::kZero) {
            flags |= kFlagZeroPage;
            size = 8;
        } else if (a.source == PageSource::kReceived && data.size() == kPageSize) {
            std::copy(data.begin(), data.end(), buffer.begin() + 8);
        } else if (const Done copied = home_copy(a.page, std::span{buffer}.last<kPageSize>());
                   copied != Done::kYes) {
            return copied;
        }
        pages_out_ += size == buffer.size() && a.to != s_.self ? 1U : 0U;
        if (s_.reply_delay > Nanos{} && a.to != s_.self) {
            std::vector<std::byte> later{buffer.begin(), buffer.begin() + size};
            after(std::exchange(s_.reply_delay, Nanos{}),
                  [this, a, later = std::move(later), flags] {
                      post(a.to, a.opcode, later, a.req, flags);
                  });
            return Done::kYes;
        }
        post(a.to, a.opcode, std::span{buffer}.first(size), a.req, flags);
        return Done::kYes;
    }

    // Nothing is sent before the store has taken the sum, so the action can run again whole.
    Done home_atomic(const HomeAction& a) noexcept {
        // The number is in the program's own byte order, as its threads read it.
        std::array<std::byte, kPageSize> copy{};  // zeros for a page never written
        std::uint64_t number = 0;
        std::byte* const at = copy.data() + a.offset_in_page;
        if (a.offset_in_page > kPageSize - sizeof number) {
            abort_job(Status::kInternal, s_.self,
                      "the home has no copy of page " + std::to_string(a.page.value));
            return Done::kFailed;
        }
        if (a.source != PageSource::kZero) {
            if (const Done copied = home_copy(a.page, copy); copied != Done::kYes) {
                return copied;
            }
        }
        std::memcpy(&number, at, sizeof number);
        const std::uint64_t sum = number + a.operand;  // modulo 2^64
        std::memcpy(at, &sum, sizeof sum);
        if (const Done kept = home_keep(a.page, copy); kept != Done::kYes) {
            return kept;
        }
        std::array<std::byte, 16> result{};
        static_cast<void>(wire_encode(
            AtomicResultPayload{a.page.value * kPageSize + a.offset_in_page, number}, result));
        post(a.to, Opcode::kAtomicResult, result, a.req);
        return Done::kYes;
    }

    // The store has spoken: the waiting actions are tried again, then the events that came
    // meanwhile, in order.
    void resume_home() noexcept {
        if (stalled_) {
            const Stalled waiting = std::move(*stalled_);
            stalled_.reset();
            run_home_actions(waiting.actions, waiting.data, waiting.now);
        }
        while (!stalled_ && !aborting_ && !home_parked_.empty()) {
            const HomeParked next = std::move(home_parked_.front());
            home_parked_.pop_front();
            home_event(next.kind, next.header, next.page, next.data, next.offset_in_page,
                       next.operand);
        }
        continue_moves();
    }

    // `data`: the page that came with the event; `now`: when the event came. An action the
    // store cannot carry out yet waits, with those after it, for the store's next word (M4-3).
    // ponytail: one waiting page holds back every home event of this node for one disk
    // operation; a queue for each page if that shows in a measurement.
    void run_home_actions(const std::vector<HomeAction>& actions, std::span<const std::byte> data,
                          Nanos now) noexcept {
        for (std::size_t i = 0; i < actions.size(); i++) {
            const HomeAction& a = actions.at(i);
            Done done = Done::kYes;
            switch (a.kind) {
                case HomeActionKind::kSend:
                    post_page_id(a.to, a.opcode, a.page, fresh(), ReplyTimer::kTimed);
                    break;
                case HomeActionKind::kReply:
                    done = home_reply(a, data);
                    break;
                case HomeActionKind::kStore:
                    done = home_keep(a.page, data);
                    break;
                case HomeActionKind::kLoad:
                    if (!store_->load(a.page).ok()) {
                        abort_job(Status::kInternal, s_.self,
                                  "the home has no copy of page " + std::to_string(a.page.value));
                        done = Done::kFailed;
                    }
                    break;
                case HomeActionKind::kDropCopy:
                    store_->drop(a.page);
                    break;
                case HomeActionKind::kApplyAtomic:
                    done = home_atomic(a);
                    break;
                case HomeActionKind::kArmHoldTimer:
                    hold_timers_[net_->start_timer(std::max(a.at - now, Nanos{})).value] = a.page;
                    break;
                case HomeActionKind::kReportThrash:
                    platform_.logger->log(
                        LogLevel::kInfo, "page_thrash",
                        JsonObject{{"page", JsonValue{static_cast<std::int64_t>(a.page.value)}},
                                   {"node_a", JsonValue{std::int64_t{a.other.value}}},
                                   {"node_b", JsonValue{std::int64_t{a.to.value}}}});
                    break;
                case HomeActionKind::kAbort:
                    abort_job(Status::kInternal, s_.self,
                              "the home machine met an impossible event");
                    done = Done::kFailed;
                    break;
                default:
                    abort_job(Status::kUnsupported, s_.self,
                              "a home action this build does not carry out yet");
                    done = Done::kFailed;
                    break;
            }
            if (done == Done::kWait) {
                Stalled waiting;
                waiting.actions.assign(actions.begin() + static_cast<std::ptrdiff_t>(i),
                                       actions.end());
                waiting.data.assign(data.begin(), data.end());
                waiting.now = now;
                stalled_ = std::move(waiting);
            }
            if (done != Done::kYes) {
                return;
            }
        }
    }

    Settings s_;
    Platform platform_;
    Sha256 hash_{};
    std::unique_ptr<MemoryEngine> mem_;
    HomeDirectoryPtr dir_;
    std::unique_ptr<HomeStore> store_;
    std::unique_ptr<LocalCache> cache_;
    std::unique_ptr<Transport> net_;
    std::optional<Sync> sync_;
    std::optional<Tasks> tasks_;         // after what it uses, so it goes first
    std::atomic<bool> tasks_up_{false};  // tasks_ is made, for the network thread
    std::thread thread_;
    int pmd_fd_ = -1;         // the local socket to pmd, or -1 without pmd
    std::thread pmd_thread_;  // watch_pmd()
    struct Report {
        Opcode opcode;
        std::vector<std::byte> payload;
    };
    std::mutex report_mu_;  // held only to queue or take one report
    std::condition_variable report_ready_;
    std::deque<Report> reports_;
    bool reports_stop_ = false;
    std::thread report_thread_;      // write_reports()
    std::uint32_t quota_ = 0;        // the thread quota pmd gave; 0 without pmd
    std::mutex quota_mu_;            // quota_ and tasks_ready_, between main() and watch_pmd()
    bool tasks_ready_ = false;       // tasks_ is started: a new quota goes to it
    std::uint64_t ram_ = 0;          // the RAM cap pmd gave; 0 without pmd
    std::uint64_t spill_ = 0;        // the spill budget pmd gave
    std::uint64_t cache_bytes_ = 0;  // the part of ram_ that is the local cache
    std::uint16_t quota_flags_ = 0;  // L_QUOTA's flags
    std::size_t segments_ = 0;
    RegionAllocator allocator_{0};

    // Network thread only.
    std::unordered_set<std::uint16_t> joined_;
    bool map_sent_ = false;
    bool have_map_ = false;
    bool start_acked_ = false;  // launcher: every member has the first map
    bool room_wanted_ = false;  // the newest map needs more than pmd's commitment holds
    bool up_sent_ = false;
    std::size_t acks_ = 0;
    Epoch epoch_;
    Slot my_slot_;
    std::vector<Slot> homes_;
    std::array<NodeId, kMaxSlots> slot_node_{};
    std::vector<PageState> states_;
    std::unordered_map<std::uint64_t, ReqId> current_;
    std::deque<Local> local_;
    std::uint64_t pages_in_ = 0;
    std::uint64_t pages_out_ = 0;     // pages sent to other nodes (L_STATS)
    std::uint64_t faults_taken_ = 0;  // faults handled (L_STATS)
    std::uint64_t prefetched_ = 0;    // pages asked for ahead
    Prefetcher prefetcher_{s_.prefetch_depth, s_.prefetch_streams};
    std::array<std::uint32_t, kLatencyBuckets> latency_{};    // fault to install, by bucket
    std::unordered_map<std::uint64_t, Nanos> fault_started_;  // pages faulted, not yet served
    std::uint64_t sent_again_ = 0;  // requests answered BUSY_RETRY or REDIRECT
    // Events held back until the local cache has room (M4-2), in the order they came.
    struct Held {
        NodeEventKind kind;
        PageId page;
        ReqId req;
        std::vector<std::byte> data;
        bool zero;
        NodeId from;
    };
    std::deque<Held> held_;
    // Leaving and segments on the move (M4-5).
    enum class Seg : std::uint8_t { kOther, kServing, kFrozen, kMoved, kArriving };
    struct Incoming {
        NodeId from;
        ReqId req;
        Epoch epoch;
        std::vector<std::byte> bytes;
    };
    std::vector<Seg> seg_;  // one state for each segment (section 3)
    std::unordered_map<std::uint32_t, std::size_t> sending_;  // frozen: the next page to send
    std::deque<Incoming> incoming_;                  // SEG_MIGRATE frames not yet installed
    std::size_t incoming_at_ = 0;                    // the next entry of the first of them
    std::unordered_set<std::uint16_t> left_;         // members that have left
    std::unordered_map<std::uint16_t, Slot> slots_;  // every member there has been: its bit
    std::deque<NodeId> leavers_;                     // launcher: leaving, the first in progress
    std::vector<NodeId> early_leavers_;              // launcher: told of before the job was up
    std::deque<SegMapMember> joiners_;               // launcher: spawned for it, not yet members
    std::unordered_set<std::uint16_t> no_homes_;     // launcher: members that joined mid-run
    NodeId joining_ = kNoNode;                       // launcher: the one joining now
    bool join_sent_ = false;                         // and the map naming it is out
    bool leave_early_ = false;      // this process was told to leave before it was up
    bool dropping_ = false;         // launcher: the map without it is out
    bool leaving_ = false;          // this process is leaving
    bool evicting_ = false;         // and serves no more faults
    bool leave_done_ = false;       // LEAVE_DONE is sent
    std::uint64_t held_pages_ = 0;  // pages not in state I
    std::uint32_t segments_moved_ = 0;
    Nanos moves_from_{};  // tests only: no segment is sent before this
    std::unordered_map<std::uint64_t, std::function<void()>> timers_;
    std::unordered_map<std::uint64_t, std::uint32_t> retries_;    // by page, or kAddRetry | add
    std::unordered_map<std::uint64_t, AtomicOpPayload> add_ops_;  // by first request: unanswered
    std::unordered_map<std::uint64_t, std::uint64_t> add_first_;  // by request in flight
    // The same for the home side and its store (M4-3): the actions waiting for the store, and
    // the events that came meanwhile.
    struct Stalled {
        std::vector<HomeAction> actions;
        std::vector<std::byte> data;
        Nanos now{};
    };
    struct HomeParked {
        HomeEventKind kind;
        FrameHeader header;
        PageId page;
        std::vector<std::byte> data;
        std::uint16_t offset_in_page;
        std::uint64_t operand;
    };
    std::optional<Stalled> stalled_;
    std::deque<HomeParked> home_parked_;
    int depth_ = 0;  // make_room() calls running: held events wait until they end
    std::unordered_map<std::uint64_t, PageId> hold_timers_;  // by timer: the page it is for
    std::vector<HomeAction> actions_;

    // Shared.
    std::atomic<std::uint64_t> next_req_{1};
    std::atomic<std::uint16_t> next_thread_{kHookThreads};
    std::atomic<bool> aborting_{false};
    std::atomic<bool> ending_{false};
    std::atomic<std::uint64_t> region_bytes_{0};  // 0 until the segment map is applied
    TimerId linger_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<FaultEvent> faults_;
    std::deque<Add> adds_;
    std::deque<StoreNote> store_notes_;
    std::deque<std::function<void()>> calls_;  // for the network thread, from any thread
    std::set<std::pair<std::uint16_t, std::uint64_t>> task_seen_;  // (node, task) logged
    std::unordered_map<std::uint64_t, std::optional<std::uint64_t>> add_results_;
    std::string abort_message_;
    bool up_ = false;
    std::size_t hook_done_ = 0;
    std::size_t gone_ = 0;
};

std::unique_ptr<Job>& job() {
    static std::unique_ptr<Job> the_job;
    return the_job;
}

}  // namespace
}  // namespace paramesh

// ---------------------------------------------------------------------------------- the C API

extern "C" {

const char* pm_strerror(int status) {
    switch (status) {
        case PM_OK:
            return "success";
        case PM_ERR_INVALID:
            return "invalid argument";
        case PM_ERR_STATE:
            return "called at the wrong time or place";
        case PM_ERR_NOMEM:
            return "out of memory";
        case PM_ERR_UNKNOWN_TASK:
            return "no task with that name";
        case PM_ERR_PLATFORM:
            return "this machine cannot run ParaMesh";
        case PM_ERR_CONFIG:
            return "the launch environment is missing or malformed";
        case PM_ERR_REFUSED:
            return "the job was not admitted";
        case PM_ERR_NETWORK:
            return "a peer could not be reached while the job was starting";
        default:
            return "unknown error";
    }
}

int pm_register_task(const char* name, pm_task_fn fn) {
    return paramesh::rt_register_task(name, fn).ok() ? PM_OK : PM_ERR_INVALID;
}

int pm_init(int* /*argc*/, char*** /*argv*/, const pm_config* cfg) {
    if (paramesh::job() != nullptr) {
        return PM_ERR_STATE;
    }
    // Two tasks that would be one on the wire: the program cannot run (paramesh.h).
    if (const paramesh::Result<void> tasks = paramesh::rt_check_registry(); !tasks.ok()) {
        static_cast<void>(std::fprintf(stderr, "pm_init: %s\n", tasks.error().what));
        static_cast<void>(std::fflush(nullptr));
        ::_exit(1);
    }
    const std::uint64_t bytes =
        cfg != nullptr && cfg->region_bytes != 0 ? cfg->region_bytes : PM_REGION_MAX_BYTES;
    if (bytes > PM_REGION_MAX_BYTES) {
        return PM_ERR_INVALID;
    }
    paramesh::Settings settings;
    if (!paramesh::read_settings(settings)) {
        return PM_ERR_CONFIG;
    }
    paramesh::job() = std::make_unique<paramesh::Job>(std::move(settings));
    const int status = paramesh::job()->start(bytes, cfg != nullptr ? cfg->threads_per_node : 0);
    if (status == PM_OK && !paramesh::job()->is_launcher()) {
        for (;;) {
            ::pause();  // a worker serves until JOB_END, which exits the process
        }
    }
    return status;
}

int pm_finalize(void) {
    if (paramesh::job() == nullptr || !paramesh::job()->is_launcher() ||
        paramesh::rt_thread_index() != paramesh::kMainThread ||
        (paramesh::job()->sync() != nullptr && paramesh::job()->sync()->outstanding() != 0)) {
        return PM_ERR_STATE;  // also: work still outstanding
    }
    paramesh::job()->finish_job();
    paramesh::job().reset();
    return PM_OK;
}

void* pm_malloc(size_t bytes) {
    return paramesh::job() != nullptr && paramesh::job()->is_launcher() &&
                   paramesh::rt_thread_index() == paramesh::kMainThread  // not from a task
               ? paramesh::job()->allocate(bytes)
               : nullptr;
}

int pm_touch(const void* p, size_t n, pm_access access) {
    return paramesh::job() != nullptr ? paramesh::job()->touch(p, n, access) : PM_ERR_STATE;
}

// Locks and barriers: each is the Sync call of the same name, made for the calling thread.
namespace {

int status_of(const paramesh::Result<void>& result) {
    if (result.ok()) {
        return PM_OK;
    }
    return result.error().code == paramesh::Errc::kInvalidArgument ? PM_ERR_INVALID : PM_ERR_STATE;
}

paramesh::Sync* locks() {
    return paramesh::job() != nullptr ? paramesh::job()->sync() : nullptr;
}

}  // namespace

// Tasks: pm_parallel_for is pm_parallel_for_data with no array.
namespace {

int queue_tasks(const char* task, uint64_t lo, uint64_t hi, uint64_t grain, const void* arg,
                size_t arg_len, const void* data, size_t stride) {
    paramesh::Tasks* tasks = paramesh::job() != nullptr ? paramesh::job()->tasks() : nullptr;
    if (tasks == nullptr) {
        return PM_ERR_STATE;
    }
    if (task == nullptr || (arg == nullptr && arg_len > 0)) {
        return PM_ERR_INVALID;
    }
    paramesh::ParallelFor call;
    call.task = task;
    call.lo = lo;
    call.hi = hi;
    call.grain = grain;
    call.arg = std::span{static_cast<const std::byte*>(arg), arg_len};
    call.data = data;
    call.stride = stride;
    const paramesh::Result<void> queued = tasks->parallel_for(call);
    if (queued.ok()) {
        return PM_OK;
    }
    switch (queued.error().code) {
        case paramesh::Errc::kInvalidArgument:
            return PM_ERR_INVALID;
        case paramesh::Errc::kNotFound:
            return PM_ERR_UNKNOWN_TASK;
        default:
            return PM_ERR_STATE;
    }
}

}  // namespace

int pm_parallel_for(const char* task, uint64_t lo, uint64_t hi, uint64_t grain, const void* arg,
                    size_t arg_len) {
    return queue_tasks(task, lo, hi, grain, arg, arg_len, nullptr, 0);
}

int pm_parallel_for_data(const char* task, uint64_t lo, uint64_t hi, uint64_t grain,
                         const void* arg, size_t arg_len, const void* data, size_t stride) {
    if (data == nullptr || stride == 0) {
        return PM_ERR_INVALID;
    }
    return queue_tasks(task, lo, hi, grain, arg, arg_len, data, stride);
}

int pm_wait_all(void) {
    paramesh::Sync* sync = paramesh::job() != nullptr ? paramesh::job()->sync() : nullptr;
    if (sync == nullptr || paramesh::rt_thread_index() != paramesh::kMainThread) {
        return PM_ERR_STATE;
    }
    return sync->wait_all().ok() ? PM_OK : PM_ERR_STATE;
}

uint64_t pm_atomic_add(uint64_t* addr, uint64_t delta) {
    if (paramesh::job() == nullptr) {
        static_cast<void>(std::fprintf(stderr, "pm_atomic_add: there is no job\n"));
        ::_exit(1);
    }
    return paramesh::job()->atomic_add(addr, delta);
}

pm_lock_t pm_lock_create(void) {
    return locks() != nullptr ? locks()->lock_create() : pm_lock_t{0};
}

int pm_lock(pm_lock_t lock) {
    return locks() != nullptr ? status_of(locks()->lock(lock, paramesh::rt_thread_index()))
                              : PM_ERR_STATE;
}

int pm_unlock(pm_lock_t lock) {
    return locks() != nullptr ? status_of(locks()->unlock(lock, paramesh::rt_thread_index()))
                              : PM_ERR_STATE;
}

pm_barrier_t pm_barrier_create(uint32_t count) {
    return locks() != nullptr ? locks()->barrier_create(count) : pm_barrier_t{0};
}

int pm_barrier_wait(pm_barrier_t barrier) {
    return locks() != nullptr
               ? status_of(locks()->barrier_wait(barrier, paramesh::rt_thread_index()))
               : PM_ERR_STATE;
}

void pm_test_register(uint32_t id, pm_test_fn fn) {
    if (id < paramesh::test_functions().size()) {
        paramesh::test_functions().at(id) = fn;
    }
}

int pm_test_run_on_workers(uint32_t id) {
    if (paramesh::job() == nullptr || !paramesh::job()->is_launcher()) {
        return PM_ERR_STATE;
    }
    return paramesh::job()->run_on_workers(id);
}

void pm_test_leave(void) {  // NOLINT(modernize-redundant-void-arg): also a C header
    if (paramesh::job() != nullptr) {
        paramesh::job()->test_leave();
    }
}

void pm_test_member_leaving(uint16_t node) {
    if (paramesh::job() != nullptr) {
        paramesh::job()->test_member_leaving(paramesh::NodeId{node});
    }
}

void pm_test_fail(const char* message) {
    if (paramesh::job() != nullptr) {
        paramesh::job()->abort_job(paramesh::Status::kInternal, paramesh::kNoNode, message);
        for (;;) {
            ::pause();  // the abort exits the process shortly
        }
    }
}

}  // extern "C"
