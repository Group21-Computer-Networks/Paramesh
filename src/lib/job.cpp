// One job process: the transport, the memory engine, the node and home machines and the home
// store wired together behind paramesh.h. M1: start-up without pmd, read sharing, clean end,
// and abort on a lost node or a reply timeout.
//
// Threads (docs/INTERNAL_API.md, section 1): every piece of protocol state below is touched
// only on the network thread. The fault-handler thread and the application's threads hand
// work to it through a mutex-guarded queue and a zero-delay timer.

#include "coh/home_machine.h"
#include "coh/node_machine.h"
#include "lib/test_hook.h"
#include "mem/memory_engine.h"
#include "net/transport.h"
#include "platform/factory.h"
#include "rt/region_allocator.h"
#include "store/home_store.h"
#include "store/placement.h"
#include "wire/payloads.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <paramesh.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace paramesh {
namespace {

constexpr std::uint8_t kLauncher = 1;
constexpr std::uint8_t kWorker = 2;
constexpr Nanos kLinger =
    std::chrono::milliseconds{150};  // lets a JOB_END leave before the process exits
constexpr Nanos kFinalizeWait = std::chrono::seconds{2};

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

// The environment of docs/PROTOCOL.md, section 10, for a process started without pmd.
bool read_settings(Settings& s) {
    const char* role = env("PARAMESH_ROLE");
    const char* job = env("PARAMESH_JOB_ID");
    const char* node = env("PARAMESH_NODE_ID");
    const char* listen = env("PARAMESH_LISTEN");
    const char* peers = env("PARAMESH_PEERS");
    if (role == nullptr || job == nullptr || node == nullptr || listen == nullptr ||
        peers == nullptr) {
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

class Job final : public NetHandler, public FaultSink, public StoreEvents {
public:
    explicit Job(Settings settings) : s_(std::move(settings)) {}

    [[nodiscard]] bool is_launcher() const noexcept { return s_.role == kLauncher; }

    // Brings the process into the job. Returns a pm_status; on a worker it returns PM_OK once
    // the job is up, and the caller then waits for the job to end.
    int start(std::uint64_t region_bytes) {
        auto platform = platform_open({}, PlatformOptions{s_.self, s_.job, 2, LogLevel::kWarn});
        if (!platform.ok() || !hash_binary(*platform.value().hasher)) {
            return PM_ERR_PLATFORM;
        }
        platform_ = std::move(platform).value();
        segments_ = static_cast<std::size_t>((region_bytes + kSegmentSize - 1) / kSegmentSize);

        auto mem = mem_open(RegionConfig{kRegionMaxBytes}, *this);
        auto dir = home_open({});
        auto store = store_open({}, *this);
        if (!mem.ok() || !dir.ok() || !store.ok()) {
            return PM_ERR_PLATFORM;
        }
        mem_ = std::move(mem).value();
        dir_ = std::move(dir).value();
        store_ = std::move(store).value();

        TransportConfig config;
        config.job = s_.job;
        config.self = JoinInfo{s_.self, s_.role, s_.listen.port,
                               static_cast<std::uint32_t>(::getpid()), hash_};
        auto net = net_open(config, *platform_.checksum, *this);
        if (!net.ok() || !net.value()->listen(s_.listen).ok()) {
            return PM_ERR_NETWORK;
        }
        net_ = std::move(net).value();
        for (const Member& member : s_.members) {
            if (member.node != s_.self && !net_->add_peer(member.node, member.at).ok()) {
                return PM_ERR_CONFIG;
            }
        }
        thread_ = std::thread{[this] { static_cast<void>(net_->run()); }};
        if (s_.members.size() == 1) {
            run_on_network_thread();  // a job of one node: nobody to wait for
        }

        std::unique_lock<std::mutex> lock{mutex_};
        if (!changed_.wait_for(lock, s_.start_timeout, [this] { return up_; })) {
            return PM_ERR_NETWORK;
        }
        return PM_OK;
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

    int run_on_workers(std::uint32_t id) {
        std::array<std::byte, 40>
            assign{};  // a TASK_ASSIGN whose chunk ID is the function's number
        put_u64(assign, 0, id);
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

    // ---- FaultSink: fault-handler thread ---------------------------------------------------
    void on_fault(const FaultEvent& event) noexcept override {
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            faults_.push_back(event);
        }
        run_on_network_thread();
    }

    // ---- StoreEvents: nothing spills in M1 ----------------------------------------------------
    void on_loaded(PageId /*page*/) noexcept override {}
    void on_spilled(PageId /*page*/) noexcept override {}

    // ---- NetHandler: network thread ------------------------------------------------------------
    void on_peer_joined(const JoinInfo& peer) noexcept override {
        if (peer.binary_hash != hash_) {
            abort_job(Status::kBinaryMismatch, peer.node,
                      "node " + std::to_string(peer.node.value) + " runs a different binary");
            return;
        }
        joined_.insert(peer.node.value);
        advance_start();
    }

    void on_frame(const FrameHeader& header, std::span<const std::byte> payload) noexcept override {
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
        std::deque<FaultEvent> faults;
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            faults.swap(faults_);
        }
        for (const FaultEvent& fault : faults) {
            node_event(fault.kind == FaultKind::kRead ? NodeEventKind::kNeedRead
                                                      : NodeEventKind::kNeedWrite,
                       fault.page, kNoReq, {}, false);
        }
        pump();
    }

private:
    struct Local {
        FrameHeader header;
        std::vector<std::byte> payload;
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

    // Delivers the frames this process addressed to itself.
    void pump() noexcept {
        while (!local_.empty()) {
            const Local next = std::move(local_.front());
            local_.pop_front();
            handle(next.header, next.payload);
        }
    }

    // ---- start-up ------------------------------------------------------------------------------

    [[nodiscard]] bool all_joined() const { return joined_.size() + 1 == s_.members.size(); }

    // Moves the start sequence on whenever something it waits for has happened.
    void advance_start() noexcept {
        if (!all_joined()) {
            return;
        }
        if (is_launcher() && !map_sent_) {
            map_sent_ = true;
            SegMapPayload map;
            std::vector<PlacementMember> placement;
            for (const Member& member : s_.members) {
                const std::uint8_t flags = member.node == s_.self ? kMemberLauncher : 0;
                map.members.push_back(
                    {member.node, member.slot, flags, member.at.ipv4, member.at.port, 1});
                placement.push_back(
                    {member.node, member.slot, 1});  // without pmd every member weighs the same
            }
            map.homes.resize(segments_);
            if (!store_place_segments(placement, map.homes).ok()) {
                abort_job(Status::kInternal, kNoNode, "cannot place the segments");
                return;
            }
            apply_map(Epoch{1}, map);
            std::vector<std::byte> buffer(4 + 20 * map.members.size() + map.homes.size());
            const Result<std::size_t> size = wire_encode(map, buffer);
            for (const Member& member : s_.members) {
                if (member.node != s_.self && size.ok()) {
                    post(member.node, Opcode::kSegMap, buffer);
                }
            }
            if (s_.members.size() == 1) {
                set_up();
            }
        } else if (!is_launcher() && have_map_ && !up_sent_) {
            up_sent_ = true;
            std::array<std::byte, 4> ack{};
            static_cast<void>(wire_encode(SegMapAckPayload{epoch_}, ack));
            post(s_.members.front().node, Opcode::kSegMapAck, ack);
            set_up();
        }
    }

    void set_up() noexcept {
        const std::lock_guard<std::mutex> lock{mutex_};
        up_ = true;
        changed_.notify_all();
    }

    void apply_map(Epoch epoch, const SegMapPayload& map) noexcept {
        epoch_ = epoch;
        homes_ = map.homes;
        slot_node_.fill(kNoNode);
        for (const SegMapMember& member : map.members) {
            slot_node_.at(member.slot.value) = member.node;
            if (member.node == s_.self) {
                my_slot_ = member.slot;
            }
        }
        for (std::size_t segment = 0; segment < homes_.size(); segment++) {
            if (homes_[segment] == my_slot_) {
                static_cast<void>(
                    home_add_segment(*dir_, SegmentId{static_cast<std::uint32_t>(segment)}));
            }
        }
        states_.assign(homes_.size() * kPagesPerSegment, PageState::kInvalid);
        allocator_ = RegionAllocator{homes_.size() * kSegmentSize};
        region_bytes_ = homes_.size() * kSegmentSize;
        have_map_ = true;
    }

    [[nodiscard]] NodeId home_of(PageId page) const noexcept {
        return slot_node_.at(homes_.at(segment_of(page).value).value);
    }
    [[nodiscard]] Slot slot_of(NodeId node) const noexcept {
        for (std::size_t slot = 0; slot < slot_node_.size(); slot++) {
            if (slot_node_.at(slot) == node) {
                return Slot{static_cast<std::uint8_t>(slot)};
            }
        }
        return Slot{0};
    }

    // ---- one frame -----------------------------------------------------------------------------

    void handle(const FrameHeader& header, std::span<const std::byte> payload) noexcept {
        const auto bad = [&] {
            abort_job(Status::kProtocol, header.src,
                      "malformed frame from node " + std::to_string(header.src.value));
        };
        switch (header.opcode) {
            case Opcode::kSegMap: {
                const Result<SegMapPayload> map = wire_decode_seg_map(payload);
                if (!map.ok() || is_launcher()) {
                    bad();
                    return;
                }
                apply_map(header.epoch, map.value());
                advance_start();
                return;
            }
            case Opcode::kSegMapAck:
                if (++acks_ + 1 == s_.members.size()) {
                    set_up();
                }
                return;
            case Opcode::kReadReq:
            case Opcode::kWriteReq: {
                const Result<PageIdPayload> request = wire_decode_page_id(payload);
                if (!request.ok()) {
                    bad();
                    return;
                }
                const HomeEventKind kind = header.opcode == Opcode::kReadReq
                                               ? HomeEventKind::kReadReq
                                               : HomeEventKind::kWriteReq;
                home_event(kind, header, request.value().page, {});
                return;
            }
            case Opcode::kFetchData: {
                const Result<PagePayload> page = wire_decode_page(payload, header.flags);
                if (!page.ok() || page.value().data.size() != kPageSize) {
                    bad();
                    return;
                }
                home_event(HomeEventKind::kFetchData, header, page.value().page, page.value().data);
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
            case Opcode::kFetch: {
                const Result<PageIdPayload> request = wire_decode_page_id(payload);
                if (!request.ok()) {
                    bad();
                    return;
                }
                node_event(NodeEventKind::kFetch, request.value().page, header.req, {}, false);
                return;
            }
            case Opcode::kTaskAssign: {  // the test hook: run a registered function, then say so
                const std::uint64_t id = get_u64(payload);
                const pm_test_fn function =
                    id < test_functions().size() ? test_functions().at(id) : nullptr;
                if (function == nullptr) {
                    bad();
                    return;
                }
                std::thread{[this, function, id, launcher = header.src] {
                    function();
                    std::array<std::byte, 24> done{};  // a TASK_DONE for that chunk ID
                    put_u64(done, 0, id);
                    FrameHeader reply;
                    reply.opcode = Opcode::kTaskDone;
                    static_cast<void>(net_->send(launcher, reply, done, ReplyTimer::kNone));
                }}.detach();
                return;
            }
            case Opcode::kTaskDone: {
                const std::lock_guard<std::mutex> lock{mutex_};
                hook_done_++;
                changed_.notify_all();
                return;
            }
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
                exit_with(end.value().status, end.value().message);
            }
            default:
                abort_job(Status::kUnsupported, header.src,
                          "a message that is not supported before M2");
        }
    }

    // ---- the node machine
    // ------------------------------------------------------------------------

    void node_event(NodeEventKind kind, PageId page, ReqId req, std::span<const std::byte> data,
                    bool zero) noexcept {
        if (aborting_ || page.value >= states_.size()) {
            if (!aborting_) {
                abort_job(Status::kInternal, s_.self, "an access outside the job's region");
            }
            return;
        }
        NodeEvent event;
        event.kind = kind;
        event.page = page;
        event.req = req;
        event.fresh = fresh();
        const auto found = current_.find(page.value);
        const NodeStep step =
            coh_step(states_[page.value], found == current_.end() ? kNoReq : found->second, event);
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
                case NodeActionKind::kSend:
                    post_page_id(home_of(page), a.opcode, page, a.req, ReplyTimer::kTimed);
                    break;
                case NodeActionKind::kReply: {
                    std::array<std::byte, 8 + kPageSize> buffer{};
                    ok = mem_->read(page, std::span{buffer}.last<kPageSize>()).ok();
                    put_u64(buffer, 0, page.value);
                    post(home_of(page), a.opcode, buffer, a.req,
                         a.read_only ? kFlagReadOnly : std::uint16_t{0});
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
                    break;
                }
                case NodeActionKind::kWriteProtect:
                    ok = mem_->write_protect(page, true).ok();
                    break;
                case NodeActionKind::kWake:
                    ok = mem_->wake(page).ok();
                    break;
                default:
                    ok = false;
            }
        }
        if (!ok) {
            abort_job(Status::kInternal, s_.self,
                      "the node machine could not handle page " + std::to_string(page.value));
        }
    }

    // ---- the home machine
    // ------------------------------------------------------------------------

    void home_event(HomeEventKind kind, const FrameHeader& header, PageId page,
                    std::span<const std::byte> data) noexcept {
        HomeEvent event;
        event.kind = kind;
        event.page = page;
        event.from = header.src;
        event.slot = slot_of(header.src);
        event.req = header.req;
        event.now = net_->now();
        actions_.clear();
        home_step(*dir_, event, actions_);
        for (const HomeAction& a : actions_) {
            switch (a.kind) {
                case HomeActionKind::kSend:
                    post_page_id(a.to, a.opcode, a.page, fresh(), ReplyTimer::kTimed);
                    break;
                case HomeActionKind::kReply: {
                    std::array<std::byte, 8 + kPageSize> buffer{};
                    put_u64(buffer, 0, a.page.value);
                    std::uint16_t flags = a.read_only ? kFlagReadOnly : std::uint16_t{0};
                    std::size_t size = buffer.size();
                    if (a.source == PageSource::kZero) {
                        flags |= kFlagZeroPage;
                        size = 8;
                    } else if (a.source == PageSource::kReceived && data.size() == kPageSize) {
                        std::copy(data.begin(), data.end(), buffer.begin() + 8);
                    } else {
                        const Result<StoreLookup> got =
                            store_->get(a.page, std::span{buffer}.last<kPageSize>());
                        if (!got.ok() || got.value() != StoreLookup::kCopied) {
                            abort_job(
                                Status::kInternal, s_.self,
                                "the home has no copy of page " + std::to_string(a.page.value));
                            return;
                        }
                    }
                    post(a.to, a.opcode, std::span{buffer}.first(size), a.req, flags);
                    break;
                }
                case HomeActionKind::kStore:
                    if (data.size() != kPageSize ||
                        !store_->put(a.page, data.first<kPageSize>()).ok()) {
                        abort_job(Status::kInternal, s_.self, "the home store is full");
                        return;
                    }
                    break;
                case HomeActionKind::kDropCopy:
                    store_->drop(a.page);
                    break;
                default:
                    if (a.opcode != Opcode::kHeartbeat) {
                        abort_job(Status::kUnsupported, header.src, "not supported before M2");
                        return;
                    }
                    abort_job(Status::kInternal, s_.self,
                              "the home machine met an impossible event");
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
    std::unique_ptr<Transport> net_;
    std::thread thread_;
    std::size_t segments_ = 0;
    RegionAllocator allocator_{0};

    // Network thread only.
    std::unordered_set<std::uint16_t> joined_;
    bool map_sent_ = false;
    bool have_map_ = false;
    bool up_sent_ = false;
    std::size_t acks_ = 0;
    Epoch epoch_;
    Slot my_slot_;
    std::vector<Slot> homes_;
    std::array<NodeId, kMaxSlots> slot_node_{};
    std::vector<PageState> states_;
    std::unordered_map<std::uint64_t, ReqId> current_;
    std::deque<Local> local_;
    std::vector<HomeAction> actions_;

    // Shared.
    std::atomic<std::uint64_t> next_req_{1};
    std::atomic<bool> aborting_{false};
    std::atomic<bool> ending_{false};
    std::atomic<std::uint64_t> region_bytes_{0};  // 0 until the segment map is applied
    TimerId linger_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<FaultEvent> faults_;
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

int pm_init(int* /*argc*/, char*** /*argv*/, const pm_config* cfg) {
    if (paramesh::job() != nullptr) {
        return PM_ERR_STATE;
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
    const int status = paramesh::job()->start(bytes);
    if (status == PM_OK && !paramesh::job()->is_launcher()) {
        for (;;) {
            ::pause();  // a worker serves until JOB_END, which exits the process
        }
    }
    return status;
}

int pm_finalize(void) {
    if (paramesh::job() == nullptr || !paramesh::job()->is_launcher()) {
        return PM_ERR_STATE;
    }
    paramesh::job()->finish_job();
    paramesh::job().reset();
    return PM_OK;
}

void* pm_malloc(size_t bytes) {
    return paramesh::job() != nullptr && paramesh::job()->is_launcher()
               ? paramesh::job()->allocate(bytes)
               : nullptr;
}

int pm_touch(const void* p, size_t n, pm_access access) {
    return paramesh::job() != nullptr ? paramesh::job()->touch(p, n, access) : PM_ERR_STATE;
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

void pm_test_fail(const char* message) {
    if (paramesh::job() != nullptr) {
        paramesh::job()->abort_job(paramesh::Status::kInternal, paramesh::kNoNode, message);
        for (;;) {
            ::pause();  // the abort exits the process shortly
        }
    }
}

}  // extern "C"
