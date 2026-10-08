#include "rt/tasks.h"

#include "rt/registry.h"

#include <algorithm>
#include <array>
#include <ctime>
#include <string>

namespace paramesh {

namespace {

// CPU time this thread has used, in nanoseconds (docs/PROTOCOL.md, TASK_DONE).
std::uint64_t thread_cpu_ns() noexcept {
    timespec now{};
    if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) != 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(now.tv_nsec);
}

}  // namespace

std::vector<std::pair<std::uint64_t, std::uint64_t>> rt_cut_range(std::uint64_t lo,
                                                                  std::uint64_t hi,
                                                                  std::uint64_t grain,
                                                                  std::uint64_t data,
                                                                  std::uint64_t stride) {
    // Cuts are allowed at first + k * period.
    std::uint64_t period = 1;
    std::uint64_t first = lo;
    if (data != 0 && stride != 0 && kPageSize % stride == 0 && data % stride == 0) {
        period = kPageSize / stride;
        const std::uint64_t into_page = (data + lo * stride) % kPageSize;
        first = lo + (kPageSize - into_page) % kPageSize / stride;
    }
    // A stride of whole pages on a page-aligned array is aligned at every index, and an array
    // that no cut can align is cut by grain alone: period 1 in both cases.
    const std::uint64_t step = std::max<std::uint64_t>(1, (grain + period - 1) / period) * period;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> chunks;
    std::uint64_t from = lo;
    for (std::uint64_t cut = first; from < hi; cut += step) {
        const std::uint64_t to = std::min(cut, hi);
        if (to > from) {
            chunks.emplace_back(from, to);
            from = to;
        }
        if (hi - cut < step) {  // also stops `cut` from wrapping round
            if (from < hi) {
                chunks.emplace_back(from, hi);
            }
            break;
        }
    }
    return chunks;
}

Tasks::Tasks(Transport& transport, RuntimeHost& host, Sync& sync, const TasksConfig& config)
    : transport_(transport),
      host_(host),
      sync_(sync),
      config_(config),
      launcher_(host.self() == host.launcher()),
      workers_(config.threads) {}

Tasks::~Tasks() {
    stop();
}

Result<void> Tasks::parallel_for(const ParallelFor& call) {
    if (!launcher_ || rt_thread_index() != kMainThread) {
        return Error{Errc::kState, 0, "parallel_for: only the launcher's main() queues tasks"};
    }
    const auto data = reinterpret_cast<std::uintptr_t>(call.data);
    const bool array = call.data != nullptr;
    if (call.task.empty() || call.lo > call.hi || call.arg.size() > kMaxTaskArg ||
        (array && call.stride == 0)) {
        return Error{Errc::kInvalidArgument, 0, "parallel_for: a bad task name, range or argument"};
    }
    if (array) {
        // The bytes of lo..hi must lie in the shared region.
        const std::uint64_t room = data >= kRegionBase && data - kRegionBase <= config_.region_bytes
                                       ? config_.region_bytes - (data - kRegionBase)
                                       : 0;
        if (call.hi > room / call.stride) {
            return Error{Errc::kInvalidArgument, 0, "parallel_for: the array is not shared memory"};
        }
    }
    const std::uint64_t task_id = rt_task_id(call.task);
    if (rt_find_task(task_id) == nullptr) {
        return Error{Errc::kNotFound, 0, "parallel_for: no task with that name"};
    }
    if (call.lo == call.hi) {
        return {};
    }
    const std::uint64_t slots =
        std::max<std::uint64_t>(1, std::uint64_t{config_.chunks_per_slot} * config_.total_slots);
    const std::uint64_t grain =
        call.grain != 0 ? call.grain : std::max<std::uint64_t>(1, (call.hi - call.lo) / slots);
    const auto cuts = rt_cut_range(call.lo, call.hi, grain, data, call.stride);

    const std::scoped_lock hold{mu_};
    const std::uint32_t id = next_call_++;
    calls_[id] = Call{task_id, {call.arg.begin(), call.arg.end()}, cuts.size()};
    for (const auto& [lo, hi] : cuts) {
        queue_.push_back(Chunk{next_chunk_++, id, lo, hi});
    }
    sync_.work_added(cuts.size());
    changed_.notify_all();  // the launcher's own threads may be waiting out an empty queue
    return {};
}

Result<void> Tasks::start() {
    std::uint16_t thread = 0;
    for (Worker& worker : workers_) {
        worker.thread = std::thread{[this, thread] { work(thread); }};
        thread++;
    }
    return {};
}

void Tasks::stop() noexcept {
    {
        const std::scoped_lock hold{mu_};
        stopping_ = true;
        changed_.notify_all();
    }
    for (Worker& worker : workers_) {
        if (worker.thread.joinable()) {
            worker.thread.join();
        }
    }
}

// One worker thread: keep one chunk to run and `prefetch` more on hand, run them in order.
void Tasks::work(std::uint16_t thread) {
    rt_thread_index() = thread;
    Worker& me = workers_[thread];
    Nanos backoff = config_.backoff_initial;
    std::unique_lock hold{mu_};
    while (!stopping_) {
        while (!me.refused && !me.no_more && me.have.size() + me.asking < 1 + config_.prefetch) {
            ask(thread);
        }
        if (me.have.empty()) {
            if (me.asking > 0) {
                changed_.wait(hold);  // an answer is on its way
            } else if (me.no_more) {
                return;
            } else {
                // NO_TASK: the queue is empty for now. Ask again after the back-off; the
                // launcher's own threads are also woken when chunks are queued.
                changed_.wait_for(hold, backoff,
                                  [this] { return stopping_ || (launcher_ && !queue_.empty()); });
                backoff = std::min(backoff * 2, config_.backoff_max);
                me.refused = false;
            }
            continue;
        }
        backoff = config_.backoff_initial;
        const TaskAssignPayload chunk = std::move(me.have.front());
        me.have.pop_front();
        // Ask for the next one now, so it is here when this one ends.
        while (!me.refused && !me.no_more && me.have.size() + me.asking < config_.prefetch) {
            ask(thread);
        }
        hold.unlock();
        const pm_task_ctx ctx{host_.self().value, thread,
                              static_cast<std::uint32_t>(chunk.arg.size())};
        const std::uint64_t before = thread_cpu_ns();
        rt_run_task(host_, chunk.task_id, ctx, chunk.lo, chunk.hi,
                    chunk.arg.empty() ? nullptr : chunk.arg.data());
        const std::uint64_t cpu = thread_cpu_ns() - before;
        hold.lock();
        if (launcher_) {
            done(chunk.chunk_id, host_.self(), cpu);
        } else {
            send(host_.launcher(), Opcode::kTaskDone, kNoReq,
                 TaskDonePayload{chunk.chunk_id, cpu, thread}, ReplyTimer::kNone);
        }
    }
}

// Asks for one chunk for this thread. The launcher's threads take it from the queue at once.
void Tasks::ask(std::uint16_t thread) {
    Worker& me = workers_[thread];
    if (launcher_) {
        TaskAssignPayload chunk;
        if (take(chunk)) {
            me.have.push_back(std::move(chunk));
        } else {
            me.refused = true;
        }
        return;
    }
    const ReqId req{next_req_++};
    asked_[req.value] = thread;
    me.asking++;
    send(host_.launcher(), Opcode::kTaskReq, req, TaskReqPayload{thread}, ReplyTimer::kTimed);
}

// Launcher: the next chunk of the queue, if there is one, with its call's argument.
bool Tasks::take(TaskAssignPayload& out) {
    if (queue_.empty()) {
        return false;
    }
    const Chunk chunk = queue_.front();
    queue_.pop_front();
    taken_[chunk.id] = chunk;
    const Call& call = calls_[chunk.call];
    out = TaskAssignPayload{chunk.id, call.task_id, chunk.lo, chunk.hi, chunk.call, call.arg};
    return true;
}

// Launcher: a chunk finished. Only the first report of a chunk counts.
void Tasks::done(std::uint64_t chunk_id, NodeId ran_by, std::uint64_t cpu_ns) {
    const auto found = taken_.find(chunk_id);
    if (found == taken_.end()) {
        return;
    }
    const Chunk chunk = found->second;
    taken_.erase(found);
    const auto call = calls_.find(chunk.call);
    const std::uint64_t task_id = call->second.task_id;
    if (--call->second.chunks_left == 0) {
        calls_.erase(call);
    }
    host_.chunk_finished(ran_by, task_id, chunk.hi - chunk.lo, Nanos{static_cast<long>(cpu_ns)});
    sync_.work_done(1);
}

void Tasks::on_frame(const FrameHeader& header, std::span<const std::byte> payload) noexcept {
    const auto bad = [this] { host_.abort_job(Status::kProtocol, "a task frame is malformed"); };
    const std::scoped_lock hold{mu_};
    switch (header.opcode) {
        case Opcode::kTaskReq: {
            if (!launcher_ || !wire_decode_task_req(payload).ok()) {
                bad();
            }
            TaskAssignPayload chunk;
            if (take(chunk)) {
                send(header.src, Opcode::kTaskAssign, header.req, chunk, ReplyTimer::kNone);
            } else {
                send(header.src, Opcode::kNoTask, header.req, NoTaskPayload{1}, ReplyTimer::kNone);
            }
            break;
        }
        case Opcode::kTaskAssign:
        case Opcode::kNoTask: {
            const auto asked = asked_.find(header.req.value);
            if (asked == asked_.end()) {
                bad();  // a chunk for a request nobody made would never run
            }
            Worker& worker = workers_[asked->second];
            asked_.erase(asked);
            worker.asking--;
            if (header.opcode == Opcode::kTaskAssign) {
                Result<TaskAssignPayload> chunk = wire_decode_task_assign(payload);
                if (!chunk.ok()) {
                    bad();
                }
                worker.have.push_back(std::move(chunk).value());
            } else {
                const Result<NoTaskPayload> none = wire_decode_no_task(payload);
                if (!none.ok()) {
                    bad();
                }
                worker.refused = true;
                worker.no_more = none.value().reason == 2;
            }
            changed_.notify_all();
            break;
        }
        case Opcode::kTaskDone: {
            const Result<TaskDonePayload> report = wire_decode_task_done(payload);
            if (!launcher_ || !report.ok()) {
                bad();
            }
            done(report.value().chunk_id, header.src, report.value().cpu_ns);
            break;
        }
        default:
            break;
    }
}

template <typename Payload>
void Tasks::send(NodeId to, Opcode opcode, ReqId req, const Payload& payload, ReplyTimer timer) {
    std::array<std::byte, 40 + kMaxTaskArg> bytes{};
    FrameHeader header;
    header.opcode = opcode;
    header.dst = to;
    header.req = req;
    const Result<std::size_t> size = wire_encode(payload, bytes);
    if (!size.ok() ||
        !transport_.send(to, header, std::span{bytes}.first(size.value()), timer).ok()) {
        host_.abort_job(Status::kNodeLost, "a task message could not be sent");
    }
}

}  // namespace paramesh
