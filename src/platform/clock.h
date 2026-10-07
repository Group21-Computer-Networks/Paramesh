// Time. Code that needs the time is given a Clock, so tests can give it a FakeClock.
#ifndef PARAMESH_PLATFORM_CLOCK_H
#define PARAMESH_PLATFORM_CLOCK_H

#include "platform/ids.h"

namespace paramesh {

class Clock {
public:
    Clock() = default;
    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;
    Clock(Clock&&) = delete;
    Clock& operator=(Clock&&) = delete;
    virtual ~Clock() = default;

    // Monotonic: only differences mean anything.
    [[nodiscard]] virtual Nanos now() const noexcept = 0;
    // Wall-clock time since the Unix epoch, for log lines.
    [[nodiscard]] virtual Nanos wall() const noexcept = 0;
};

// A clock that moves only when told to. Not thread-safe.
class FakeClock final : public Clock {
public:
    [[nodiscard]] Nanos now() const noexcept override { return now_; }
    [[nodiscard]] Nanos wall() const noexcept override { return now_; }
    void advance(Nanos by) noexcept { now_ += by; }

private:
    Nanos now_{0};
};

}  // namespace paramesh

#endif  // PARAMESH_PLATFORM_CLOCK_H
