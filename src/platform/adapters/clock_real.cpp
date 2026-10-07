#include "platform/adapters/adapters.h"

#include <chrono>

namespace paramesh {
namespace {

class RealClock final : public Clock {
public:
    [[nodiscard]] Nanos now() const noexcept override {
        return std::chrono::steady_clock::now().time_since_epoch();
    }
    [[nodiscard]] Nanos wall() const noexcept override {
        return std::chrono::system_clock::now().time_since_epoch();
    }
};

}  // namespace

std::unique_ptr<Clock> make_real_clock(const Platform& /*made*/,
                                       const PlatformOptions& /*options*/) {
    return std::make_unique<RealClock>();
}

}  // namespace paramesh
