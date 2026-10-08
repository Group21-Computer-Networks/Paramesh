#include "rt/registry.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace paramesh {

namespace {

struct Task {
    std::string name;
    std::uint64_t id = 0;
    pm_task_fn fn = nullptr;
};

struct Registry {
    std::vector<Task> tasks;
    std::string clash;  // the text of rt_check_registry's error, kept alive here
};

// A function-local static: PM_TASK's constructors run before main(), in no set order.
// ponytail: a vector scanned from the front; a job has a handful of tasks.
Registry& registry() noexcept {
    static Registry the_registry;
    return the_registry;
}

const Task* find(std::uint64_t id) noexcept {
    const std::vector<Task>& tasks = registry().tasks;
    const auto found =
        std::find_if(tasks.begin(), tasks.end(), [id](const Task& t) { return t.id == id; });
    return found == tasks.end() ? nullptr : &*found;
}

}  // namespace

std::uint64_t rt_task_id(std::string_view name) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;  // 64-bit FNV-1a
    for (const char c : name) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
    }
    return hash;
}

Result<void> rt_register_task_as(const char* name, std::uint64_t id, pm_task_fn fn) noexcept {
    if (name == nullptr || *name == '\0' || fn == nullptr) {
        return Error{Errc::kInvalidArgument, 0, "register a task: no name or no function"};
    }
    registry().tasks.push_back({name, id, fn});
    return {};
}

Result<void> rt_register_task(const char* name, pm_task_fn fn) noexcept {
    return rt_register_task_as(name, name != nullptr ? rt_task_id(name) : 0, fn);
}

Result<void> rt_check_registry() noexcept {
    Registry& all = registry();
    for (std::size_t i = 0; i < all.tasks.size(); i++) {
        for (std::size_t j = i + 1; j < all.tasks.size(); j++) {
            const Task& a = all.tasks[i];
            const Task& b = all.tasks[j];
            if (a.name == b.name) {
                all.clash = "task '" + a.name + "' is registered twice";
            } else if (a.id == b.id) {
                all.clash = "tasks '" + a.name + "' and '" + b.name +
                            "' have the same ID (the hash of their names); rename one";
            } else {
                continue;
            }
            return Error{Errc::kInvalidArgument, 0, all.clash.c_str()};
        }
    }
    return {};
}

pm_task_fn rt_find_task(std::uint64_t id) noexcept {
    const Task* task = find(id);
    return task != nullptr ? task->fn : nullptr;
}

const char* rt_task_name(std::uint64_t id) noexcept {
    const Task* task = find(id);
    return task != nullptr ? task->name.c_str() : "";
}

}  // namespace paramesh
