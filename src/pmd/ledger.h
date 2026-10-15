// The ledger (M5-1): what this node and each peer did for each other's jobs, per job and in
// both directions, from this node's own records only: the job processes on this node report
// each finished chunk (L_CHUNK) and the segment homes at each epoch (L_SEGMENTS). Kept as JSON
// in the state directory and written atomically: a temporary file, then a rename.
//
// Raw counts are chunks completed per task (with their indexes and, for this node's own work,
// the CPU time measured) and segment-seconds hosted. The credits are set by the credit rules
// (M5-2) from these counts; nothing a peer reports is in here.
//
// Threads: pmd's one thread.
#ifndef PARAMESH_PMD_LEDGER_H
#define PARAMESH_PMD_LEDGER_H

#include "platform/clock.h"
#include "platform/ids.h"
#include "platform/json.h"
#include "platform/result.h"

#include <cstdint>
#include <map>
#include <string>

namespace paramesh {

// Whose job the work was for: kForPeer, this node's work for the peer's job (taken from this
// node by the peer); kByPeer, the peer's work for this node's job (given to it by the peer).
enum class Direction : std::uint8_t { kForPeer, kByPeer };

struct TaskWork {
    std::uint64_t chunks = 0;
    std::uint64_t indexes = 0;
    std::uint64_t cpu_ns = 0;  // measured on this node; 0 for work a peer did
    friend bool operator==(const TaskWork&, const TaskWork&) = default;
};

struct WorkSide {
    std::map<std::uint64_t, TaskWork> tasks;  // by task ID
    double segment_seconds = 0;               // segments hosted x seconds
    double credit = 0;                        // set by the credit rules (M5-2)
    friend bool operator==(const WorkSide&, const WorkSide&) = default;
};

struct JobWork {
    WorkSide for_peer;
    WorkSide by_peer;
    friend bool operator==(const JobWork&, const JobWork&) = default;
};

// By peer node ID, then by job ID.
using LedgerData = std::map<std::uint16_t, std::map<std::uint32_t, JobWork>>;

// What the slot allocation (M5-3) reads; its tests give it a fake.
class LedgerView {
public:
    LedgerView() = default;
    LedgerView(const LedgerView&) = default;
    LedgerView& operator=(const LedgerView&) = default;
    LedgerView(LedgerView&&) = default;
    LedgerView& operator=(LedgerView&&) = default;
    virtual ~LedgerView() = default;

    // What the peer gave this node minus what it took, in credits, over all jobs.
    [[nodiscard]] virtual double balance(NodeId peer) const = 0;
};

class Ledger final : public LedgerView {
public:
    // The ledger in `path`, or an empty one if there is no such file. Errc::kInvalidArgument
    // if the file is not a ledger, Errc::kIo if it cannot be read.
    static Result<Ledger> open(std::string path, const JsonCodec& json);

    void add_chunk(NodeId peer, JobId job, Direction direction, std::uint64_t task,
                   std::uint64_t indexes, Nanos cpu);
    void add_segment_seconds(NodeId peer, JobId job, Direction direction, double seconds);
    // For the credit rules, which set `credit`.
    WorkSide& side(NodeId peer, JobId job, Direction direction);

    [[nodiscard]] const LedgerData& data() const noexcept { return data_; }
    [[nodiscard]] double balance(NodeId peer) const override;
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }

    // Writes the whole ledger to `path`.tmp, flushes it to disk and renames it over `path`, so
    // the file is always one complete ledger, the old or the new.
    Result<void> save();

private:
    Ledger(std::string path, const JsonCodec& json) : path_(std::move(path)), json_(&json) {}

    std::string path_;
    const JsonCodec* json_;
    LedgerData data_;
    bool dirty_ = false;
};

}  // namespace paramesh

#endif  // PARAMESH_PMD_LEDGER_H
