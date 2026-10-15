#include "pmd/ledger.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <optional>
#include <string_view>
#include <utility>

namespace paramesh {

namespace {

constexpr std::int64_t kVersion = 1;

JsonValue to_json(const WorkSide& side) {
    JsonObject tasks;
    for (const auto& [task, work] : side.tasks) {
        tasks[std::to_string(task)] =
            JsonValue{JsonObject{{"chunks", JsonValue{static_cast<std::int64_t>(work.chunks)}},
                                 {"indexes", JsonValue{static_cast<std::int64_t>(work.indexes)}},
                                 {"cpu_ns", JsonValue{static_cast<std::int64_t>(work.cpu_ns)}}}};
    }
    return JsonValue{JsonObject{{"tasks", JsonValue{std::move(tasks)}},
                                {"segment_seconds", JsonValue{side.segment_seconds}},
                                {"credit", JsonValue{side.credit}}}};
}

const JsonObject* object(const JsonValue& value) {
    return std::get_if<JsonObject>(&value.v);
}

const JsonValue* member(const JsonObject& in, std::string_view key) {
    const auto found = in.find(key);
    return found == in.end() ? nullptr : &found->second;
}

// A count: a non-negative integer.
std::optional<std::uint64_t> count(const JsonObject& in, std::string_view key) {
    const JsonValue* value = member(in, key);
    const auto* number = value != nullptr ? std::get_if<std::int64_t>(&value->v) : nullptr;
    if (number == nullptr || *number < 0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(*number);
}

// A real number; the codec reads 0 and other whole numbers as integers.
std::optional<double> real(const JsonObject& in, std::string_view key) {
    const JsonValue* value = member(in, key);
    if (value == nullptr) {
        return std::nullopt;
    }
    if (const auto* number = std::get_if<double>(&value->v)) {
        return *number;
    }
    if (const auto* number = std::get_if<std::int64_t>(&value->v)) {
        return static_cast<double>(*number);
    }
    return std::nullopt;
}

template <typename Number>
std::optional<Number> key_number(std::string_view key) {
    Number number{};
    const auto [end, error] = std::from_chars(key.data(), key.data() + key.size(), number);
    if (error != std::errc{} || end != key.data() + key.size()) {
        return std::nullopt;
    }
    return number;
}

std::optional<WorkSide> side_from(const JsonValue* value) {
    const JsonObject* in = value != nullptr ? object(*value) : nullptr;
    const JsonValue* tasks_value = in != nullptr ? member(*in, "tasks") : nullptr;
    const JsonObject* tasks = tasks_value != nullptr ? object(*tasks_value) : nullptr;
    if (tasks == nullptr) {
        return std::nullopt;
    }
    WorkSide side;
    const std::optional<double> segment_seconds = real(*in, "segment_seconds");
    const std::optional<double> credit = real(*in, "credit");
    if (!segment_seconds || !credit) {
        return std::nullopt;
    }
    side.segment_seconds = *segment_seconds;
    side.credit = *credit;
    for (const auto& [key, work_value] : *tasks) {
        const std::optional<std::uint64_t> task = key_number<std::uint64_t>(key);
        const JsonObject* work = object(work_value);
        if (!task || work == nullptr) {
            return std::nullopt;
        }
        const auto chunks = count(*work, "chunks");
        const auto indexes = count(*work, "indexes");
        const auto cpu_ns = count(*work, "cpu_ns");
        if (!chunks || !indexes || !cpu_ns) {
            return std::nullopt;
        }
        side.tasks[*task] = TaskWork{*chunks, *indexes, *cpu_ns};
    }
    return side;
}

std::optional<LedgerData> data_from(const JsonValue& whole) {
    const JsonObject* top = object(whole);
    const JsonValue* peers_value = top != nullptr ? member(*top, "peers") : nullptr;
    const JsonObject* peers = peers_value != nullptr ? object(*peers_value) : nullptr;
    if (peers == nullptr || count(*top, "version") != std::optional<std::uint64_t>{kVersion}) {
        return std::nullopt;
    }
    LedgerData data;
    for (const auto& [peer_key, jobs_value] : *peers) {
        const std::optional<std::uint16_t> peer = key_number<std::uint16_t>(peer_key);
        const JsonObject* jobs = object(jobs_value);
        if (!peer || jobs == nullptr) {
            return std::nullopt;
        }
        for (const auto& [job_key, work_value] : *jobs) {
            const std::optional<std::uint32_t> job = key_number<std::uint32_t>(job_key);
            const JsonObject* work = object(work_value);
            const std::optional<WorkSide> for_peer =
                work != nullptr ? side_from(member(*work, "for_peer")) : std::nullopt;
            const std::optional<WorkSide> by_peer =
                work != nullptr ? side_from(member(*work, "by_peer")) : std::nullopt;
            if (!job || !for_peer || !by_peer) {
                return std::nullopt;
            }
            data[*peer][*job] = JobWork{*for_peer, *by_peer};
        }
    }
    return data;
}

// Writes all of `text` to `fd`.
bool write_all(int fd, std::string_view text) {
    while (!text.empty()) {
        const ssize_t wrote = ::write(fd, text.data(), text.size());
        if (wrote < 0 && errno == EINTR) {
            continue;
        }
        if (wrote <= 0) {
            return false;
        }
        text.remove_prefix(static_cast<std::size_t>(wrote));
    }
    return true;
}

}  // namespace

Result<Ledger> Ledger::open(std::string path, const JsonCodec& json) {
    Ledger ledger{std::move(path), json};
    std::ifstream file{ledger.path_, std::ios::binary};
    if (!file) {
        return ledger;  // no ledger yet; a temporary file left by a crash is never read
    }
    const std::string text{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    if (file.bad()) {
        return Error{Errc::kIo, 0, "read the ledger"};
    }
    const Result<JsonValue> parsed = json.decode(text);
    std::optional<LedgerData> data =
        parsed.ok() ? data_from(parsed.value()) : std::optional<LedgerData>{};
    if (!data) {
        return Error{Errc::kInvalidArgument, 0, "the ledger file is not a ledger"};
    }
    ledger.data_ = std::move(*data);
    return ledger;
}

WorkSide& Ledger::side(NodeId peer, JobId job, Direction direction) {
    dirty_ = true;
    JobWork& work = data_[peer.value][job.value];
    return direction == Direction::kForPeer ? work.for_peer : work.by_peer;
}

void Ledger::add_chunk(NodeId peer, JobId job, Direction direction, std::uint64_t task,
                       std::uint64_t indexes, Nanos cpu) {
    TaskWork& work = side(peer, job, direction).tasks[task];
    work.chunks++;
    work.indexes += indexes;
    work.cpu_ns += static_cast<std::uint64_t>(cpu.count());
}

void Ledger::add_segment_seconds(NodeId peer, JobId job, Direction direction, double seconds) {
    side(peer, job, direction).segment_seconds += seconds;
}

double Ledger::balance(NodeId peer) const {
    const auto found = data_.find(peer.value);
    double given = 0;
    double taken = 0;
    if (found != data_.end()) {
        for (const auto& [job, work] : found->second) {
            given += work.by_peer.credit;
            taken += work.for_peer.credit;
        }
    }
    return given - taken;
}

Result<void> Ledger::save() {
    JsonObject peers;
    for (const auto& [peer, jobs] : data_) {
        JsonObject by_job;
        for (const auto& [job, work] : jobs) {
            by_job[std::to_string(job)] = JsonValue{JsonObject{{"for_peer", to_json(work.for_peer)},
                                                               {"by_peer", to_json(work.by_peer)}}};
        }
        peers[std::to_string(peer)] = JsonValue{std::move(by_job)};
    }
    const std::string text =
        json_->encode(JsonValue{
            JsonObject{{"version", JsonValue{kVersion}}, {"peers", JsonValue{std::move(peers)}}}}) +
        "\n";
    const std::string temporary = path_ + ".tmp";
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return Error{Errc::kIo, errno, "open the ledger's temporary file"};
    }
    const bool written = write_all(fd, text) && ::fsync(fd) == 0;
    const int error = errno;
    ::close(fd);
    if (!written || ::rename(temporary.c_str(), path_.c_str()) != 0) {
        return Error{Errc::kIo, written ? errno : error, "write the ledger"};
    }
    // The rename itself reaches the disk with the directory.
    const std::string directory = path_.substr(0, path_.rfind('/') + 1);
    if (const int dir =
            ::open(directory.empty() ? "." : directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        dir >= 0) {
        static_cast<void>(::fsync(dir));
        ::close(dir);
    }
    dirty_ = false;
    return {};
}

}  // namespace paramesh
