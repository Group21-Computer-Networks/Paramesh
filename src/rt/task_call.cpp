// The one file that calls application task bodies. It is compiled with exceptions on (see
// CMakeLists.txt) and lets none out: a task that throws ends the job.

#include "rt/registry.h"

#include <exception>
#include <string>

namespace paramesh {

void rt_run_task(RuntimeHost& host, std::uint64_t id, const pm_task_ctx& ctx, std::uint64_t lo,
                 std::uint64_t hi, const void* arg) noexcept {
    const pm_task_fn body = rt_find_task(id);
    if (body == nullptr) {
        host.abort_job(Status::kTaskFailed, "a chunk names a task this binary does not have");
    }
    const std::string task = std::string{"task '"} + rt_task_name(id) + "' threw";
    try {
        body(&ctx, lo, hi, arg);
    } catch (const std::exception& thrown) {
        host.abort_job(Status::kTaskFailed, task + ": " + thrown.what());
    } catch (...) {
        host.abort_job(Status::kTaskFailed, task + " something that is not a std::exception");
    }
}

}  // namespace paramesh
